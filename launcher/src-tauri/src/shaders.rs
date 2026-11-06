// SPDX-License-Identifier: GPL-3.0-or-later
//! First-run shader setup: builds `shader_cache/spirv` next to the game exe from the user's own romfs and main.
//!
//! Two existing tools do the work, both shipped in the launcher's `tools/` folder:
//!   1. `tools/shaders/extract_corpus.py` (Python 3 with the `lz4` package) finds every GLSLC output in the romfs and in
//!      exefs/main and writes the distinct shader blocks to `shader_cache/corpus` (code/, ctrl/, glslc/, unique.tsv);
//!   2. `tools/spirv_harness/spirv_harness.exe` (self-contained .NET publish) translates them to `<hash>.spv` plus the
//!      `results.tsv` rows the runtime reads.
//!
//! Translation: one `spirv_harness --serve` process (protocol in the harness's Serve.cs) with one job per hardware thread, at
//! below-normal priority. It is sent the blocks in this order: the recorded boot order (`<game>/shader_order.txt`, which the runtime
//! writes, then `tools/shaders/shader_order.txt` if the package has one), then the blocks of the main executable, then the rest, and
//! writes each `<hash>.spv` and its `results.tsv` row straight into `spirv/` (so the game can use them at once).
//! Resumable: the full block list is kept as `corpus/unique_all.tsv`; a block with a row in `spirv/results.tsv` is never translated again,
//! whoever wrote it (the runtime's on-demand translator writes the same cache). Pause = the harness is stopped.
//! The game may run meanwhile: while it holds `shader_cache/game_running.lock` the harness drops to idle priority.
//! `shader_cache/setup.json` records what is done; `shader_cache/setup.log` the order, job count and priority changes.
//! No crate-local imports, so `examples/shader_setup.rs` can build this file alone.

use serde::{Deserialize, Serialize};
use std::collections::HashSet;
use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::time::Duration;

/// Folder (in the game folder) the runtime opens by default; also what setup writes as `[nvn] shader_cache`.
pub const SPIRV_REL: &str = "shader_cache/spirv";

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
pub struct Marker {
    /// romfs the corpus was extracted from
    pub romfs: String,
    pub extracted: bool,
    pub blocks: usize,
    pub complete: bool,
}

#[derive(Serialize, Clone, Default)]
pub struct Progress {
    /// "idle", "running", "done", "error", "cancelled"
    pub state: String,
    /// "check", "extract", "translate", "finish"
    pub phase: String,
    pub done: u64,
    pub total: u64,
    pub message: String,
}

pub struct Tools {
    pub python: PathBuf,
    pub extract: PathBuf,
    pub harness: PathBuf,
    /// A boot shader order shipped with the package (one hash per line), if any.
    pub order: Option<PathBuf>,
}

pub struct Job {
    pub game_dir: PathBuf,
    pub romfs: PathBuf,
    pub main: PathBuf,
    /// Translator jobs (0 = one per hardware thread).
    pub jobs: usize,
    /// Test runs: translate at most this many blocks (0 = all); the setup is then never marked complete.
    pub limit: usize,
}

pub fn cache_dir(game_dir: &Path) -> PathBuf {
    game_dir.join("shader_cache")
}

pub fn read_marker(game_dir: &Path) -> Marker {
    fs::read_to_string(cache_dir(game_dir).join("setup.json")).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
}

fn write_marker(game_dir: &Path, m: &Marker) -> Result<(), String> {
    let p = cache_dir(game_dir).join("setup.json");
    fs::write(&p, serde_json::to_string_pretty(m).unwrap_or_default()).map_err(|e| format!("cannot write {}: {}", p.display(), e))
}

/// True when a finished setup's cache is in place.
pub fn is_complete(game_dir: &Path) -> bool {
    read_marker(game_dir).complete && game_dir.join(SPIRV_REL).join("results.tsv").is_file()
}

fn quiet(cmd: &mut Command) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS (the tools' own child processes inherit it)
        cmd.creation_flags(0x0800_0000 | 0x0000_4000);
    }
}

fn python_has_lz4(py: &Path) -> bool {
    let mut c = Command::new(py);
    c.args(["-I", "-c", "import lz4.block"]).stdout(Stdio::null()).stderr(Stdio::null());
    quiet(&mut c);
    c.status().map(|s| s.success()).unwrap_or(false)
}

fn on_path(exe: &str) -> Vec<PathBuf> {
    std::env::var_os("PATH").map(|p| std::env::split_paths(&p).map(|d| d.join(exe)).filter(|f| f.is_file()).collect()).unwrap_or_default()
}

/// Finds the tools in `tools_dir` (the launcher's `tools/` folder). Python: `tools/python/python.exe`, else the first
/// python.exe on PATH that has `lz4`. Every missing piece is named in the error.
pub fn find_tools(tools_dir: &Path) -> Result<Tools, String> {
    let mut missing = Vec::new();
    let harness = tools_dir.join("spirv_harness").join("spirv_harness.exe");
    if !harness.is_file() {
        missing.push(format!("the shader translator ({})", harness.display()));
    }
    let extract = tools_dir.join("shaders").join("extract_corpus.py");
    if !extract.is_file() {
        missing.push(format!("the shader extractor ({})", extract.display()));
    }
    let bundled = tools_dir.join("python").join("python.exe");
    let mut candidates = vec![bundled.clone()];
    candidates.extend(on_path("python.exe"));
    let python = candidates.into_iter().filter(|p| p.is_file()).find(|p| python_has_lz4(p));
    if python.is_none() {
        missing.push(format!(
            "Python 3 with the lz4 package ({}, or python.exe on PATH with `pip install lz4`)",
            bundled.display()
        ));
    }
    if !missing.is_empty() {
        return Err(format!("Shader setup cannot run, missing: {}. Re-extract the launcher's release folder so tools/ is complete.", missing.join("; ")));
    }
    let order = Some(tools_dir.join("shaders").join("shader_order.txt")).filter(|p| p.is_file());
    Ok(Tools { python: python.unwrap(), extract, harness, order })
}

/// Runs a tool, handing each stdout line to `line`; kills it when `cancel` is set. Ok(true) = exit 0, Ok(false) = cancelled.
fn run_tool(mut cmd: Command, cancel: &AtomicBool, mut line: impl FnMut(&str)) -> Result<bool, String> {
    quiet(&mut cmd);
    let name = cmd.get_program().to_string_lossy().to_string();
    let mut child: Child = cmd.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().map_err(|e| format!("could not start {}: {}", name, e))?;
    let (tx, rx) = mpsc::channel::<String>();
    let out = child.stdout.take().unwrap();
    let tx2 = tx.clone();
    std::thread::spawn(move || BufReader::new(out).lines().map_while(Result::ok).for_each(|l| drop(tx2.send(l))));
    let err = child.stderr.take().unwrap();
    let (etx, erx) = mpsc::channel::<String>();
    std::thread::spawn(move || {
        let mut tail: Vec<String> = Vec::new();
        for l in BufReader::new(err).lines().map_while(Result::ok) {
            tail.push(l);
            if tail.len() > 12 {
                tail.remove(0);
            }
        }
        let _ = etx.send(tail.join("\n"));
    });
    drop(tx);
    loop {
        match rx.recv_timeout(Duration::from_millis(200)) {
            Ok(l) => line(&l),
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                // stdout closed: the process is ending
                let st = child.wait().map_err(|e| e.to_string())?;
                if cancel.load(Ordering::SeqCst) {
                    return Ok(false);
                }
                if st.success() {
                    return Ok(true);
                }
                let tail = erx.recv_timeout(Duration::from_secs(5)).unwrap_or_default();
                return Err(format!("{} failed ({}). {}", name, st, tail.trim()));
            }
        }
        if cancel.load(Ordering::SeqCst) {
            let _ = child.kill();
            let _ = child.wait();
            return Ok(false);
        }
    }
}

/// Header + rows of a tsv file (rows without the trailing newline).
fn read_tsv(p: &Path) -> Result<(String, Vec<String>), String> {
    let text = fs::read_to_string(p).map_err(|e| format!("cannot read {}: {}", p.display(), e))?;
    let mut it = text.lines();
    let header = it.next().unwrap_or("").to_string();
    Ok((header, it.filter(|l| !l.is_empty()).map(|l| l.to_string()).collect()))
}

fn first_col(row: &str) -> &str {
    row.split('\t').next().unwrap_or("")
}

/// Column `name` of a tsv row, by the header.
fn col<'a>(header: &str, row: &'a str, name: &str) -> &'a str {
    header.split('\t').position(|h| h == name).and_then(|i| row.split('\t').nth(i)).unwrap_or("")
}

/// True while the game runs: the runtime holds `shader_cache/game_running.lock` open for writing (see nvn_shaders.cpp).
pub fn game_running(game_dir: &Path) -> bool {
    let p = cache_dir(game_dir).join("game_running.lock");
    if !p.is_file() {
        return false;
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        // ERROR_SHARING_VIOLATION: the game has it
        return matches!(fs::OpenOptions::new().write(true).share_mode(0).open(&p), Err(e) if e.raw_os_error() == Some(32));
    }
    #[allow(unreachable_code)]
    false
}

/// Appends a line to `shader_cache/setup.log` (seconds since 1970, then the text).
fn log(game_dir: &Path, text: &str) {
    let secs = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(cache_dir(game_dir).join("setup.log")) {
        let _ = writeln!(f, "{} {}", secs, text);
    }
}

/// Hashes with a row in results.tsv (translated, or tried and failed: either way not sent again).
fn done_hashes(results: &Path) -> HashSet<String> {
    fs::read_to_string(results).map(|t| t.lines().skip(1).map(|l| first_col(l).to_string()).filter(|h| !h.is_empty()).collect()).unwrap_or_default()
}

/// (blocks with a row in the cache, blocks in the game) for the launcher's pages; total 0 = not known yet (not extracted).
pub fn cache_progress(game_dir: &Path) -> (u64, u64) {
    let total = read_marker(game_dir).blocks as u64;
    let done = done_hashes(&game_dir.join(SPIRV_REL).join("results.tsv")).len() as u64;
    (done.min(total.max(done)), total)
}

/// The recorded boot order: hashes, first file first, each once.
fn boot_order(files: &[PathBuf]) -> (Vec<String>, Vec<String>) {
    let mut seen = HashSet::new();
    let mut out = Vec::new();
    let mut used = Vec::new();
    for f in files {
        if let Ok(t) = fs::read_to_string(f) {
            used.push(f.to_string_lossy().replace('\\', "/"));
            for h in t.lines().map(|l| l.trim()).filter(|l| !l.is_empty()) {
                if seen.insert(h.to_string()) {
                    out.push(h.to_string());
                }
            }
        }
    }
    (out, used)
}

/// Runs `spirv_harness --serve`: sends `C <hash>` for every hash in order, then `E`; counts the `R` answers. Every 2 s it checks
/// whether the game runs and sends `P idle` / `P below` when that changes. Ok(true) = all answered, Ok(false) = cancelled.
fn run_serve(
    tools: &Tools,
    game_dir: &Path,
    spirv: &Path,
    corpus: &Path,
    jobs: usize,
    hashes: Vec<String>,
    cancel: &AtomicBool,
    mut answered: impl FnMut(u64),
) -> Result<bool, String> {
    let mut cmd = Command::new(&tools.harness);
    cmd.arg("--serve").arg("--cache").arg(spirv).arg("--corpus").arg(corpus).args(["--jobs", &jobs.to_string(), "--priority", "below"]);
    quiet(&mut cmd);
    let mut child = cmd.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().map_err(|e| format!("could not start the shader translator: {}", e))?;
    let stdin = std::sync::Arc::new(std::sync::Mutex::new(child.stdin.take().unwrap()));
    {
        let stdin = stdin.clone();
        std::thread::spawn(move || {
            for part in hashes.chunks(512) {
                let mut text = String::with_capacity(part.len() * 20);
                for h in part {
                    text.push_str("C ");
                    text.push_str(h);
                    text.push('\n');
                }
                if stdin.lock().unwrap().write_all(text.as_bytes()).is_err() {
                    return;
                }
            }
            let mut s = stdin.lock().unwrap();
            let _ = s.write_all(b"E\n");
            let _ = s.flush();
        });
    }
    let (tx, rx) = mpsc::channel::<String>();
    let out = child.stdout.take().unwrap();
    std::thread::spawn(move || BufReader::new(out).lines().map_while(Result::ok).for_each(|l| drop(tx.send(l))));
    let err = child.stderr.take().unwrap();
    let (etx, erx) = mpsc::channel::<String>();
    std::thread::spawn(move || {
        let mut tail: Vec<String> = Vec::new();
        for l in BufReader::new(err).lines().map_while(Result::ok) {
            tail.push(l);
            if tail.len() > 12 {
                tail.remove(0);
            }
        }
        let _ = etx.send(tail.join("\n"));
    });
    let mut n = 0u64;
    let mut refused = 0u64;
    let mut idle = false;
    let mut checked = std::time::Instant::now() - Duration::from_secs(10);
    loop {
        match rx.recv_timeout(Duration::from_millis(200)) {
            Ok(l) => {
                if l.starts_with("R\t") {
                    n += 1;
                    answered(n);
                } else if let Some(x) = l.strip_prefix("X ") {
                    refused += 1;
                    if refused <= 20 {
                        log(game_dir, &format!("translate: refused {}", x));
                    }
                } else if let Some(i) = l.strip_prefix("I ") {
                    log(game_dir, &format!("translator: {}", i));
                }
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                let st = child.wait().map_err(|e| e.to_string())?;
                if cancel.load(Ordering::SeqCst) {
                    return Ok(false);
                }
                if st.success() {
                    return Ok(true);
                }
                let tail = erx.recv_timeout(Duration::from_secs(5)).unwrap_or_default();
                return Err(format!("the shader translator failed ({}). {}", st, tail.trim()));
            }
        }
        if cancel.load(Ordering::SeqCst) {
            let _ = child.kill();
            let _ = child.wait();
            return Ok(false);
        }
        if checked.elapsed() >= Duration::from_secs(2) {
            checked = std::time::Instant::now();
            let running = game_running(game_dir);
            if running != idle {
                idle = running;
                let _ = stdin.lock().unwrap().write_all(if idle { b"P idle\n" } else { b"P below\n" });
                log(game_dir, if idle { "game running: translation at idle priority" } else { "game closed: translation back at below-normal priority" });
            }
        }
    }
}

/// The whole setup. `report` gets every progress change. Ok(true) = finished (or the limit reached), Ok(false) = cancelled.
pub fn run(job: &Job, tools: &Tools, cancel: &AtomicBool, mut report: impl FnMut(Progress)) -> Result<bool, String> {
    let cache = cache_dir(&job.game_dir);
    let corpus = cache.join("corpus");
    let spirv = job.game_dir.join(SPIRV_REL);
    fs::create_dir_all(&spirv).map_err(|e| format!("cannot create {}: {}", spirv.display(), e))?;
    let romfs_s = job.romfs.to_string_lossy().replace('\\', "/");
    let mut marker = read_marker(&job.game_dir);
    if marker.romfs != romfs_s {
        marker = Marker { romfs: romfs_s.clone(), ..Default::default() };
    }
    marker.complete = false;
    write_marker(&job.game_dir, &marker)?;
    let jobs = job.jobs.max(1).to_string();
    let p = |phase: &str, done: u64, total: u64, message: String| Progress { state: "running".into(), phase: phase.into(), done, total, message };

    // 1. extract (skipped when this romfs was already extracted)
    let all = corpus.join("unique_all.tsv");
    if !(marker.extracted && all.is_file()) {
        report(p("extract", 0, 0, "Finding the game's shaders in your game files...".into()));
        let _ = fs::remove_file(corpus.join("unique.tsv"));
        let mut cmd = Command::new(&tools.python);
        cmd.arg("-I").arg(&tools.extract).arg("--romfs").arg(&job.romfs).arg("--main").arg(&job.main).arg("--out").arg(&corpus);
        cmd.args(["--jobs", &jobs, "--progress"]);
        let ok = run_tool(cmd, cancel, |l| {
            if let Some(rest) = l.strip_prefix("scan: ") {
                let mut it = rest.split_whitespace().filter_map(|x| x.parse::<u64>().ok());
                if let (Some(d), Some(t)) = (it.next(), it.next()) {
                    report(p("extract", d, t, format!("Scanning game files: {} of {}", d, t)));
                }
            }
        })?;
        if !ok {
            return Ok(false);
        }
        fs::rename(corpus.join("unique.tsv"), &all).map_err(|e| format!("the extractor wrote no block list: {}", e))?;
        marker.extracted = true;
        write_marker(&job.game_dir, &marker)?;
    }

    // 2. translate what spirv/results.tsv does not list yet, boot order first
    let _ = fs::remove_dir_all(cache.join("stage"));   // the chunked setup of launcher 0.1.1
    let (uheader, urows) = read_tsv(&all)?;
    marker.blocks = urows.len();
    write_marker(&job.game_dir, &marker)?;
    let results = spirv.join("results.tsv");
    let done = done_hashes(&results);
    let total = urows.len() as u64;
    let todo: HashSet<&str> = urows.iter().map(|r| first_col(r)).filter(|h| !done.contains(*h)).collect();
    let mut order_files = vec![job.game_dir.join("shader_order.txt")];
    order_files.extend(tools.order.clone());
    let (recorded, used) = boot_order(&order_files);
    let mut queue: Vec<String> = Vec::with_capacity(todo.len());
    let mut queued: HashSet<&str> = HashSet::new();
    for h in &recorded {
        if let Some(t) = todo.get(h.as_str()) {
            if queued.insert(t) {
                queue.push(h.clone());
            }
        }
    }
    let n_recorded = queue.len();
    for r in &urows {
        let h = first_col(r);
        if todo.contains(h) && col(&uheader, r, "first_source") == "main" && queued.insert(h) {
            queue.push(h.to_string());
        }
    }
    let n_main = queue.len() - n_recorded;
    for r in &urows {
        let h = first_col(r);
        if todo.contains(h) && queued.insert(h) {
            queue.push(h.to_string());
        }
    }
    let limited = job.limit > 0 && queue.len() > job.limit;
    if job.limit > 0 {
        queue.truncate(job.limit);
    }
    let all_jobs = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4);
    let jobs = if job.jobs == 0 { all_jobs } else { job.jobs };
    let finished = total - todo.len() as u64;
    log(
        &job.game_dir,
        &format!(
            "translate: {} of {} blocks to do, in this order: {} from the recorded boot order ({} hashes in {}), {} of the main executable, {} others; spirv_harness --serve, {} jobs ({} hardware threads), below-normal priority",
            queue.len(),
            total,
            n_recorded,
            recorded.len(),
            if used.is_empty() { "no order file".to_string() } else { used.join(", ") },
            n_main,
            queue.len() - n_recorded - n_main,
            jobs,
            all_jobs
        ),
    );
    let msg = |d: u64| format!("Translating shaders: {} of {} (the game can start; missing shaders are made while you play)", d, total);
    report(p("translate", finished, total, msg(finished)));
    if !queue.is_empty() {
        let mut last = std::time::Instant::now();
        let ok = run_serve(tools, &job.game_dir, &spirv, &corpus, jobs, queue, cancel, |n| {
            if last.elapsed() >= Duration::from_millis(250) {
                last = std::time::Instant::now();
                report(p("translate", (finished + n).min(total), total, msg((finished + n).min(total))));
            }
        })?;
        if !ok {
            log(&job.game_dir, &format!("translate: paused at {} of {}", done_hashes(&results).len().min(total as usize), total));
            return Ok(false);
        }
    }
    let _ = fs::remove_file(corpus.join("unique.tsv"));
    let now_done = done_hashes(&results);
    let missing = urows.iter().filter(|r| !now_done.contains(first_col(r))).count();
    log(&job.game_dir, &format!("translate: {} of {} blocks in the cache", total as usize - missing, total));
    if limited {
        return Ok(true);
    }
    if missing > 0 {
        return Err(format!("{} shaders could not be translated (shader_cache/setup.log). Set up again retries them.", missing));
    }

    // 3. done: the corpus is only needed to translate, drop it (several GB). The game's on-demand translator may still read it: wait for the game to close.
    while game_running(&job.game_dir) {
        report(p("finish", total, total, "All shaders are translated. Cleaning up once the game is closed...".into()));
        if cancel.load(Ordering::SeqCst) {
            return Ok(false);
        }
        std::thread::sleep(Duration::from_secs(2));
    }
    report(p("finish", total, total, "Cleaning up...".into()));
    let _ = fs::remove_dir_all(&corpus);
    marker.extracted = false;
    marker.complete = true;
    write_marker(&job.game_dir, &marker)?;
    Ok(true)
}
