// SPDX-License-Identifier: GPL-3.0-or-later
//! pokopia-setup: the launcher's work (first-run setup, status, Play) as a console program, for systems where the
//! launcher's window cannot open (Proton/Wine on the Steam Deck has no WebView2). Same code as the launcher: the
//! plain modules of `../src-tauri/src` are included as they are. See `pokopia-setup --help`.

mod cli;

#[allow(dead_code)]
#[path = "../../src-tauri/src/dump.rs"]
mod dump;
#[allow(dead_code)]
#[path = "../../src-tauri/src/gamebuild.rs"]
mod gamebuild;
#[allow(dead_code)]
#[path = "../../src-tauri/src/ini.rs"]
mod ini;
#[allow(dead_code)]
#[path = "../../src-tauri/src/settings.rs"]
mod settings;
#[allow(dead_code)]
#[path = "../../src-tauri/src/shaders.rs"]
mod shaders;

use cli::Cmd;
use serde::{Deserialize, Serialize};
use std::fmt::Write as _;
use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const CONFIG_FILE: &str = "launcher.json";
const LOG_FILE: &str = "setup.log";
const STATUS_FILE: &str = "setup_status.txt";
const CANCEL_FILE: &str = "setup.cancel";
/// Files pkp.exe may write when something goes wrong (as play.rs).
const REPORT_FILES: [&str; 4] = ["pkp_crash.txt", "pkp_freeze.txt", "pkp_fatal.txt", "pkp.log"];
const DECK_HEADER: &str = "# Written by pokopia-setup (Steam Deck preset); the game adds its factory settings on first start.";

const EXIT_OK: i32 = 0;
const EXIT_ERROR: i32 = 1;
const EXIT_PAUSED: i32 = 2;
const EXIT_USAGE: i32 = 3;

/// Same shape as the launcher's Config (src-tauri/src/main.rs), but kept next to the exe: a Proton prefix may be deleted.
#[derive(Serialize, Deserialize, Clone, Default, Debug, PartialEq)]
#[serde(default)]
struct Config {
    dump_dir: String,
    game_dir: String,
    minimize_on_play: bool,
}

fn load_config(path: &Path) -> Config {
    fs::read_to_string(path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
}

fn save_config(path: &Path, cfg: &Config) -> Result<(), String> {
    let text = serde_json::to_string_pretty(cfg).map_err(|e| e.to_string())?;
    fs::write(path, text).map_err(|e| format!("cannot save {}: {}", path.display(), e))
}

fn exe_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_path_buf())).unwrap_or_else(|| PathBuf::from("."))
}

/// A path from the command line, made absolute (the exe's folder is usually not the working directory).
fn absolute(p: &str) -> String {
    let p = PathBuf::from(p.trim());
    let full = if p.is_absolute() { p } else { std::env::current_dir().map(|d| d.join(&p)).unwrap_or(p) };
    dump::fwd(&full)
}

/// Everything a command needs: folders and the stored options.
struct Ctx {
    game: PathBuf,
    tools: PathBuf,
    cfg: Config,
}

impl Ctx {
    fn new(exe_dir: &Path, cfg: Config) -> Ctx {
        let game = if cfg.game_dir.trim().is_empty() { exe_dir.to_path_buf() } else { PathBuf::from(cfg.game_dir.trim()) };
        Ctx { game, tools: exe_dir.join("tools"), cfg }
    }
}

fn main() {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let args = match cli::parse(&argv) {
        Ok(a) => a,
        Err(e) => {
            eprintln!("error: {}", e);
            eprintln!("Run `pokopia-setup --help` for usage.");
            std::process::exit(EXIT_USAGE);
        }
    };
    let code = match args.cmd {
        Cmd::Help => {
            print!("{}", cli::USAGE);
            EXIT_OK
        }
        Cmd::Version => {
            println!("pokopia-setup {}", env!("CARGO_PKG_VERSION"));
            EXIT_OK
        }
        cmd => {
            let exe = exe_dir();
            let cfg_path = exe.join(CONFIG_FILE);
            let mut cfg = load_config(&cfg_path);
            let mut changed = false;
            if let Some(d) = &args.dump {
                cfg.dump_dir = absolute(d);
                changed = true;
            }
            if let Some(g) = &args.game {
                cfg.game_dir = absolute(g);
                changed = true;
            }
            if changed {
                if let Err(e) = save_config(&cfg_path, &cfg) {
                    eprintln!("warning: {} (the folders are used for this run only)", e);
                }
            }
            let ctx = Ctx::new(&exe, cfg);
            match cmd {
                Cmd::Setup { jobs, limit } => cmd_setup(&ctx, jobs, limit),
                Cmd::Status => {
                    print!("{}", status_text(&ctx));
                    EXIT_OK
                }
                Cmd::Play { wait } => cmd_play(&ctx, wait),
                Cmd::Preset(_) => cmd_preset_deck(&ctx),
                Cmd::Help | Cmd::Version => unreachable!(),
            }
        }
    };
    let _ = std::io::stdout().flush();
    std::process::exit(code);
}

fn fail(e: &str) -> i32 {
    eprintln!("error: {}", e);
    EXIT_ERROR
}

fn describe(c: &dump::Candidate) -> String {
    if c.kind == "update" {
        format!("update {}", c.version)
    } else {
        c.kind.clone()
    }
}

// ---------------------------------------------------------------------------------------------------------------- time

/// "2026-11-04 12:00:00 UTC". No local time zone without a dependency; UTC is unambiguous in a log.
fn utc_stamp(t: SystemTime) -> String {
    let secs = t.duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let (days, rem) = ((secs / 86400) as i64, secs % 86400);
    // civil_from_days (H. Hinnant)
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC", y, m, d, rem / 3600, rem / 60 % 60, rem % 60)
}

fn fmt_dur(d: Duration) -> String {
    let s = d.as_secs();
    if s >= 3600 {
        format!("{}h {:02}m {:02}s", s / 3600, s / 60 % 60, s % 60)
    } else if s >= 60 {
        format!("{}m {:02}s", s / 60, s % 60)
    } else {
        format!("{:.1}s", d.as_secs_f64())
    }
}

// ---------------------------------------------------------------------------------------------------------------- setup

fn progress_line(phase: &str, done: u64, total: u64, message: &str) -> String {
    if total > 0 {
        format!("[{}] {}/{} {}", phase, done, total, message)
    } else {
        format!("[{}] {}", phase, message)
    }
}

fn append_log(path: &Path, line: &str) {
    if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(f, "{}  {}", utc_stamp(SystemTime::now()), line);
    }
}

/// Prints every progress change; mirrors it to setup.log and setup_status.txt at most every 2 s (and on every phase
/// change), so a storm of ticks does not hammer the disk. Also times the phases.
struct Reporter {
    log: PathBuf,
    status: PathBuf,
    last: (String, String),
    pending: Option<String>,
    last_write: Option<Instant>,
    /// "build" or "shaders": both tools have a "finish" phase
    stage: &'static str,
    phase: String,
    since: Instant,
    times: Vec<(String, Duration)>,
}

impl Reporter {
    fn new(game: &Path) -> Reporter {
        Reporter {
            log: game.join(LOG_FILE),
            status: game.join(STATUS_FILE),
            last: Default::default(),
            pending: None,
            last_write: None,
            stage: "setup",
            phase: String::new(),
            since: Instant::now(),
            times: Vec::new(),
        }
    }

    fn write_files(&mut self, line: &str) {
        append_log(&self.log, line);
        let _ = fs::write(&self.status, format!("{}  {}\r\n", utc_stamp(SystemTime::now()), line));
        self.last_write = Some(Instant::now());
        self.pending = None;
    }

    /// A line that is always printed and written at once.
    fn note(&mut self, line: &str) {
        println!("{}", line);
        let _ = std::io::stdout().flush();
        self.flush();
        self.write_files(line);
    }

    fn flush(&mut self) {
        if let Some(l) = self.pending.take() {
            self.write_files(&l);
        }
    }

    fn end_phase(&mut self) {
        if !self.phase.is_empty() {
            let d = self.since.elapsed();
            match self.times.last_mut() {
                Some(last) if last.0 == self.phase => last.1 += d,
                _ => self.times.push((self.phase.clone(), d)),
            }
        }
        self.phase.clear();
        self.since = Instant::now();
    }

    fn tick(&mut self, phase: &str, done: u64, total: u64, message: &str) {
        if self.last.0 == phase && self.last.1 == message {
            return;
        }
        let label = format!("{}/{}", self.stage, phase);
        let new_phase = label != self.phase;
        if new_phase {
            self.end_phase();
            self.phase = label;
        }
        self.last = (phase.to_string(), message.to_string());
        let line = progress_line(phase, done, total, message);
        println!("{}", line);
        let _ = std::io::stdout().flush();
        if new_phase || self.last_write.is_none_or(|t| t.elapsed() >= Duration::from_secs(2)) {
            self.flush();
            self.write_files(&line);
        } else {
            self.pending = Some(line);
        }
    }

    fn summary(&self, total: Duration) -> String {
        let mut s = String::from("Time per phase:");
        for (p, d) in &self.times {
            let _ = write!(s, " {} {},", p, fmt_dur(*d));
        }
        let _ = write!(s, " total {}", fmt_dur(total));
        s
    }
}

/// Polls for `<game>/setup.cancel` every 500 ms and sets `cancel` when it appears.
fn watch_cancel(file: PathBuf, log: PathBuf, cancel: Arc<AtomicBool>, stop: Arc<AtomicBool>) -> std::thread::JoinHandle<()> {
    std::thread::spawn(move || {
        while !stop.load(Ordering::SeqCst) {
            if !cancel.load(Ordering::SeqCst) && file.exists() {
                cancel.store(true, Ordering::SeqCst);
                println!("Pausing... ({} found; the tools stop at the next safe point)", CANCEL_FILE);
                let _ = std::io::stdout().flush();
                append_log(&log, "Pausing...");
            }
            std::thread::sleep(Duration::from_millis(500));
        }
    })
}

fn cmd_setup(ctx: &Ctx, jobs: Option<usize>, limit: usize) -> i32 {
    let game = &ctx.game;
    if ctx.cfg.dump_dir.trim().is_empty() {
        eprintln!("error: no game files chosen yet. Pass --dump <folder>: the decrypted dump with base/ and/or updates/<version>/.");
        return EXIT_USAGE;
    }
    let report = dump::scan(&ctx.cfg.dump_dir);
    let Some(chosen) = report.chosen.clone() else {
        eprintln!("error: the game files at {} are not usable:", report.path);
        for e in &report.errors {
            eprintln!("  {}", e);
        }
        return EXIT_ERROR;
    };
    println!("Game files: {} ({})", describe(&chosen), chosen.path);
    println!("  exefs: {}", chosen.exefs);
    println!("  romfs: {}", chosen.romfs);
    println!("Game folder: {}", dump::fwd(game));
    let package_b = gamebuild::is_package_b(&ctx.tools);
    if package_b {
        if let Err(e) = fs::create_dir_all(game) {
            return fail(&format!("cannot create {}: {}", game.display(), e));
        }
    } else if !game.join("pkp.exe").is_file() {
        return fail(&format!(
            "pkp.exe was not found in {}. Put pokopia-setup next to pkp.exe or pass --game <folder with pkp.exe>.",
            dump::fwd(game)
        ));
    }
    if let Err(e) = settings::ensure_play_paths(game, &chosen.exefs, &chosen.romfs) {
        return fail(&e);
    }

    let cancel_file = game.join(CANCEL_FILE);
    let _ = fs::remove_file(&cancel_file);
    let cancel = Arc::new(AtomicBool::new(false));
    let stop = Arc::new(AtomicBool::new(false));
    let watcher = watch_cancel(cancel_file.clone(), game.join(LOG_FILE), cancel.clone(), stop.clone());

    let t0 = Instant::now();
    let mut rep = Reporter::new(game);
    rep.note(&format!(
        "Setup started: {} from {}, game folder {}, tools {}",
        describe(&chosen),
        report.path,
        dump::fwd(game),
        dump::fwd(&ctx.tools)
    ));
    let result = run_setup(ctx, &chosen, package_b, jobs, limit, &cancel, &mut rep);
    stop.store(true, Ordering::SeqCst);
    let _ = watcher.join();
    let _ = fs::remove_file(&cancel_file);
    rep.end_phase();
    let summary = rep.summary(t0.elapsed());
    let (code, line) = match result {
        Ok(true) if limit > 0 => (EXIT_OK, format!("Setup test run finished (--limit {}): the shader setup is not marked complete.", limit)),
        Ok(true) => (EXIT_OK, "Setup finished. Start the game with `pokopia-setup play` (or pkp.exe --log pkp.log).".to_string()),
        Ok(false) => (EXIT_PAUSED, "Setup paused. Run setup again to continue where it stopped.".to_string()),
        Err(e) => (EXIT_ERROR, format!("Setup failed: {}", e)),
    };
    rep.note(&summary);
    if code == EXIT_ERROR {
        eprintln!("error: {}", line.trim_start_matches("Setup failed: "));
        rep.flush();
        rep.write_files(&line);
    } else {
        rep.note(&line);
    }
    code
}

/// The setup steps after the checks: game build (model b) then shaders. Ok(true) = finished, Ok(false) = paused.
fn run_setup(ctx: &Ctx, chosen: &dump::Candidate, package_b: bool, jobs: Option<usize>, limit: usize, cancel: &AtomicBool, rep: &mut Reporter) -> Result<bool, String> {
    let game = &ctx.game;
    // as the launcher: the shader tools get one core less, the build all of them
    let auto = std::thread::available_parallelism().map(|n| n.get().saturating_sub(1)).unwrap_or(2).max(1);
    let (build_jobs, shader_jobs) = match jobs {
        Some(n) => (n, n),
        None => (auto + 1, auto),
    };
    rep.tick("check", 0, 0, "Checking the setup tools...");
    if package_b && !gamebuild::is_built(game) {
        let tools = gamebuild::find_tools(&ctx.tools)?;
        rep.stage = "build";
        let job = gamebuild::Job { game_dir: game.clone(), exefs: PathBuf::from(&chosen.exefs), jobs: build_jobs };
        rep.note(&format!("Building pkp.exe from your game files ({} jobs)...", build_jobs));
        if !gamebuild::run(&job, &tools, cancel, |t| rep.tick(&t.phase, t.done, t.total, &t.message))? {
            return Ok(false);
        }
    } else if package_b {
        rep.note("pkp.exe is already built.");
    }
    if cancel.load(Ordering::SeqCst) {
        return Ok(false);
    }
    rep.stage = "shaders";
    let tools = shaders::find_tools(&ctx.tools)?;
    let job = shaders::Job {
        game_dir: game.clone(),
        romfs: PathBuf::from(&chosen.romfs),
        main: PathBuf::from(&chosen.exefs).join("main"),
        jobs: shader_jobs,
        limit,
    };
    rep.note(&format!("Shader setup ({} jobs)...", shader_jobs));
    shaders::run(&job, &tools, cancel, |p| rep.tick(&p.phase, p.done, p.total, &p.message))
}

// ---------------------------------------------------------------------------------------------------------------- status

/// Data rows of a tsv (lines after the header that are not empty).
fn tsv_rows(path: &Path) -> Option<usize> {
    let f = fs::File::open(path).ok()?;
    Some(BufReader::new(f).lines().map_while(Result::ok).skip(1).filter(|l| !l.is_empty()).count())
}

fn yes(b: bool) -> &'static str {
    if b {
        "yes"
    } else {
        "no"
    }
}

fn status_text(ctx: &Ctx) -> String {
    let game = &ctx.game;
    let mut o = String::new();
    let _ = writeln!(o, "Game folder:     {}{}", dump::fwd(game), if game.is_dir() { "" } else { " (does not exist)" });
    let _ = writeln!(o, "Tools folder:    {}{}", dump::fwd(&ctx.tools), if ctx.tools.is_dir() { "" } else { " (does not exist)" });
    let mut chosen = None;
    if ctx.cfg.dump_dir.trim().is_empty() {
        let _ = writeln!(o, "Dump folder:     (not set; pass --dump <folder>)");
    } else {
        let report = dump::scan(&ctx.cfg.dump_dir);
        let _ = writeln!(o, "Dump folder:     {}", report.path);
        match &report.chosen {
            Some(c) => {
                let _ = writeln!(o, "Game files:      {}", describe(c));
                let _ = writeln!(o, "  exefs:         {}", c.exefs);
                let _ = writeln!(o, "  romfs:         {}", c.romfs);
            }
            None => {
                let _ = writeln!(o, "Game files:      not usable: {}", report.errors.join(" "));
            }
        }
        chosen = report.chosen;
    }
    let package_b = gamebuild::is_package_b(&ctx.tools);
    let built = if package_b { gamebuild::is_built(game) } else { game.join("pkp.exe").is_file() };
    let _ = writeln!(o, "Package:         {}", if package_b { "model b (pkp.exe is built here from your dump)" } else { "prebuilt pkp.exe" });
    let _ = writeln!(o, "Game built:      {}", yes(built));
    let complete = shaders::is_complete(game);
    let m = shaders::read_marker(game);
    let _ = writeln!(o, "Shader setup:    complete {}", yes(complete));
    let _ = writeln!(
        o,
        "  setup.json:    romfs {}, blocks {}, extracted {}, complete {}",
        if m.romfs.is_empty() { "(none)" } else { &m.romfs },
        m.blocks,
        yes(m.extracted),
        yes(m.complete)
    );
    match tsv_rows(&game.join(shaders::SPIRV_REL).join("results.tsv")) {
        Some(n) => {
            let _ = writeln!(o, "  results.tsv:   {} translated shaders", n);
        }
        None => {
            let _ = writeln!(o, "  results.tsv:   (not present)");
        }
    }
    let s = settings::read(game);
    let _ = writeln!(o, "pkp.ini:         {}", if s.pkp_exists { "present" } else { "missing (setup writes it)" });
    for (k, label) in [("fs.exefs", "[fs] exefs"), ("fs.romfs", "[fs] romfs"), ("fs.save_dir", "[fs] save_dir"), ("nvn.shader_cache", "[nvn] shader_cache")] {
        let v = s.pkp.get(k).map(|v| v.as_str()).unwrap_or("(not set)");
        let _ = writeln!(o, "  {:<19}= {}", label, v);
    }
    let _ = writeln!(o, "controls.ini:    {}", if s.controls_exists { "present" } else { "missing (the game writes it on first start)" });
    let ready = chosen.is_some() && built && complete;
    let _ = writeln!(o, "Ready to play:   {}", yes(ready));
    o
}

// ---------------------------------------------------------------------------------------------------------------- play

/// Which report files exist and whether each was written since `started` (2 s slack, as the launcher).
fn report_files(dir: &Path, started: SystemTime) -> Vec<(String, bool)> {
    REPORT_FILES
        .iter()
        .filter_map(|n| {
            let md = fs::metadata(dir.join(n)).ok()?;
            let fresh = md.modified().map(|m| m + Duration::from_secs(2) >= started).unwrap_or(false);
            Some((n.to_string(), fresh))
        })
        .collect()
}

fn cmd_play(ctx: &Ctx, wait: bool) -> i32 {
    let game = &ctx.game;
    if ctx.cfg.dump_dir.trim().is_empty() {
        return fail("Choose your game files first: pokopia-setup setup --dump <folder>.");
    }
    let report = dump::scan(&ctx.cfg.dump_dir);
    let Some(chosen) = report.chosen else {
        return fail(&format!("The game files at {} are not usable: {}", ctx.cfg.dump_dir, report.errors.join(" ")));
    };
    if gamebuild::is_package_b(&ctx.tools) && !gamebuild::is_built(game) {
        return fail("Build the game first: run pokopia-setup setup.");
    }
    if !shaders::is_complete(game) {
        return fail("Finish the shader setup first: run pokopia-setup setup.");
    }
    if let Err(e) = settings::ensure_play_paths(game, &chosen.exefs, &chosen.romfs) {
        return fail(&e);
    }
    let exe = game.join("pkp.exe");
    if !exe.is_file() {
        return fail(&format!("pkp.exe was not found in {}. Pass --game <folder with pkp.exe>.", dump::fwd(game)));
    }
    let mut cmd = Command::new(&exe);
    // every path (exefs, romfs, shader cache) comes from pkp.ini next to pkp.exe
    cmd.current_dir(game).args(["--log", "pkp.log"]);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no stray console if pkp.exe is a console build
    }
    let started = SystemTime::now();
    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => return fail(&format!("Could not start pkp.exe: {}", e)),
    };
    println!("pkp.exe started (process {}).", child.id());
    if !wait {
        return EXIT_OK;
    }
    let code = match child.wait() {
        Ok(s) => s.code(),
        Err(e) => return fail(&format!("Lost track of pkp.exe: {}", e)),
    };
    match code {
        Some(0) => println!("pkp.exe exited normally."),
        Some(c) => println!("pkp.exe exited with code {} (0x{:08X}).", c, c as u32),
        None => println!("pkp.exe was ended by the system."),
    }
    let fresh: Vec<String> = report_files(game, started).into_iter().filter(|f| f.1).map(|f| f.0).collect();
    if !fresh.is_empty() {
        println!("Written by this run (in {}): {}", dump::fwd(game), fresh.join(", "));
    }
    if code == Some(0) {
        EXIT_OK
    } else {
        EXIT_ERROR
    }
}

// ---------------------------------------------------------------------------------------------------------------- preset

/// Steam Deck preset: borderless window. Returns what was done, one line each. Creates a minimal controls.ini when there is
/// none: the runtime loads its factory bindings for a file without `[bindings.*]` sections.
fn preset_deck(game: &Path) -> Result<Vec<String>, String> {
    if !game.is_dir() {
        return Err(format!("The game folder {} does not exist. Pass --game <folder with pkp.exe>.", dump::fwd(game)));
    }
    let path = game.join(settings::CONTROLS_INI);
    let mut out = Vec::new();
    let mut ini = match fs::read(&path) {
        Ok(b) => ini::Ini::parse(&String::from_utf8_lossy(&b)),
        Err(_) => {
            out.push(format!("created {}", dump::fwd(&path)));
            ini::Ini::parse(&format!("{}\r\n", DECK_HEADER))
        }
    };
    for (section, key, value) in [("display", "window_mode", "borderless")] {
        if ini.get(section, key).as_deref().map(str::trim) == Some(value) {
            out.push(format!("[{}] {} = {} (already set)", section, key, value));
        } else {
            ini.set(section, key, value);
            out.push(format!("[{}] {} = {}", section, key, value));
        }
    }
    let tmp = path.with_extension("ini.tmp");
    fs::write(&tmp, ini.to_text()).map_err(|e| format!("cannot write {}: {}", tmp.display(), e))?;
    fs::rename(&tmp, &path).map_err(|e| {
        let _ = fs::remove_file(&tmp);
        format!("cannot replace {}: {}", path.display(), e)
    })?;
    Ok(out)
}

fn cmd_preset_deck(ctx: &Ctx) -> i32 {
    match preset_deck(&ctx.game) {
        Ok(lines) => {
            println!("Steam Deck preset ({}):", dump::fwd(&ctx.game.join(settings::CONTROLS_INI)));
            for l in lines {
                println!("  {}", l);
            }
            EXIT_OK
        }
        Err(e) => fail(&e),
    }
}

// ---------------------------------------------------------------------------------------------------------------- tests

#[cfg(test)]
mod tests {
    use super::*;

    /// A fresh empty folder under the system temp folder; removed on drop.
    struct Tmp(PathBuf);
    impl Tmp {
        fn new(name: &str) -> Tmp {
            let nanos = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
            let p = std::env::temp_dir().join(format!("pokopia-setup-test-{}-{}-{}", name, std::process::id(), nanos));
            fs::create_dir_all(&p).unwrap();
            Tmp(p)
        }
    }
    impl Drop for Tmp {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn config_round_trip_and_launcher_shape() {
        let t = Tmp::new("config");
        let p = t.0.join(CONFIG_FILE);
        assert_eq!(load_config(&p), Config::default());
        let cfg = Config { dump_dir: "Z:/home/deck/dump".into(), game_dir: "Z:/home/deck/pkp".into(), minimize_on_play: true };
        save_config(&p, &cfg).unwrap();
        assert_eq!(load_config(&p), cfg);
        // a file written by the launcher (extra or missing keys) still loads
        fs::write(&p, r#"{"dump_dir":"D:/d","future_key":1}"#).unwrap();
        assert_eq!(load_config(&p), Config { dump_dir: "D:/d".into(), ..Default::default() });
        let v: serde_json::Value = serde_json::from_str(&serde_json::to_string(&cfg).unwrap()).unwrap();
        assert!(v.get("dump_dir").is_some() && v.get("game_dir").is_some() && v.get("minimize_on_play").is_some());
    }

    #[test]
    fn preset_deck_creates_controls_ini() {
        let t = Tmp::new("preset-new");
        let lines = preset_deck(&t.0).unwrap();
        assert!(lines[0].starts_with("created "));
        let text = fs::read_to_string(t.0.join("controls.ini")).unwrap();
        assert_eq!(text, format!("{}\r\n\r\n[display]\r\nwindow_mode = borderless\r\n", DECK_HEADER));
        // second run changes nothing
        let lines = preset_deck(&t.0).unwrap();
        assert_eq!(lines, vec!["[display] window_mode = borderless (already set)".to_string()]);
        assert_eq!(fs::read_to_string(t.0.join("controls.ini")).unwrap(), text);
        assert!(!t.0.join("controls.ini.tmp").exists());
    }

    #[test]
    fn preset_deck_edits_existing_controls_ini() {
        let t = Tmp::new("preset-edit");
        let before = "# controls header\r\n[display]\r\nwindow_mode = windowed ; f11 toggles\r\nvsync = 1\r\n\r\n[bindings.keyboard]\r\njump = space\r\n";
        fs::write(t.0.join("controls.ini"), before).unwrap();
        let lines = preset_deck(&t.0).unwrap();
        assert_eq!(lines, vec!["[display] window_mode = borderless".to_string()]);
        let after = fs::read_to_string(t.0.join("controls.ini")).unwrap();
        assert_eq!(after, before.replace("windowed", "borderless"));
        // a file without the section gets it appended
        fs::write(t.0.join("controls.ini"), "[mouse]\nyaw = 0.5\n").unwrap();
        preset_deck(&t.0).unwrap();
        assert_eq!(fs::read_to_string(t.0.join("controls.ini")).unwrap(), "[mouse]\nyaw = 0.5\n\n[display]\nwindow_mode = borderless\n");
        assert!(preset_deck(&t.0.join("missing")).is_err());
    }

    #[test]
    fn report_files_fresh_and_old() {
        let t = Tmp::new("report");
        assert!(report_files(&t.0, SystemTime::now()).is_empty());
        fs::write(t.0.join("pkp.log"), "x").unwrap();
        fs::write(t.0.join("pkp_crash.txt"), "x").unwrap();
        let now = report_files(&t.0, SystemTime::now() - Duration::from_secs(60));
        assert_eq!(now, vec![("pkp_crash.txt".to_string(), true), ("pkp.log".to_string(), true)]);
        let later = report_files(&t.0, SystemTime::now() + Duration::from_secs(3600));
        assert!(later.iter().all(|f| !f.1));
    }

    fn touch(p: &Path) {
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::write(p, "x").unwrap();
    }

    #[test]
    fn status_reports_everything() {
        let t = Tmp::new("status");
        let exe = t.0.join("release");
        let game = exe.clone();
        fs::create_dir_all(&game).unwrap();
        let ctx = Ctx::new(&exe, Config::default());
        let s = status_text(&ctx);
        assert!(s.contains("Dump folder:     (not set"));
        assert!(s.contains("Package:         prebuilt"));
        assert!(s.contains("Game built:      no"));
        assert!(s.contains("results.tsv:   (not present)"));
        assert!(s.contains("pkp.ini:         missing"));
        assert!(s.contains("Ready to play:   no"));

        let dump_dir = t.0.join("dump");
        for f in ["main", "sdk", "subsdk0", "rtld", "main.npdm"] {
            touch(&dump_dir.join("updates").join("131072").join("exefs").join(f));
        }
        fs::create_dir_all(dump_dir.join("updates/131072/romfs")).unwrap();
        touch(&game.join("pkp.exe"));
        fs::write(game.join("pkp.ini"), "[fs]\r\nsave_dir = D:/save\r\n").unwrap();
        fs::create_dir_all(game.join("shader_cache/spirv")).unwrap();
        fs::write(game.join("shader_cache/setup.json"), r#"{"romfs":"R:/romfs","extracted":false,"blocks":3,"complete":true}"#).unwrap();
        fs::write(game.join("shader_cache/spirv/results.tsv"), "hash\tstatus\na\tok\nb\tok\nc\tok\n").unwrap();
        fs::write(game.join("controls.ini"), "").unwrap();
        let ctx = Ctx::new(&exe, Config { dump_dir: dump::fwd(&dump_dir), ..Default::default() });
        let s = status_text(&ctx);
        assert!(s.contains("Game files:      update 131072"), "{}", s);
        assert!(s.contains("Game built:      yes"));
        assert!(s.contains("Shader setup:    complete yes"));
        assert!(s.contains("romfs R:/romfs, blocks 3, extracted no, complete yes"));
        assert!(s.contains("results.tsv:   3 translated shaders"));
        assert!(s.contains("[fs] save_dir      = D:/save"));
        assert!(s.contains("[fs] romfs         = (not set)"));
        assert!(s.contains("controls.ini:    present"));
        assert!(s.contains("Ready to play:   yes"));

        // model b: tools/build/pkp_build.py present, no build/done.json yet
        touch(&exe.join("tools/build/pkp_build.py"));
        let s = status_text(&ctx);
        assert!(s.contains("Package:         model b"));
        assert!(s.contains("Game built:      no"));
    }

    #[test]
    fn helpers() {
        assert_eq!(utc_stamp(UNIX_EPOCH), "1970-01-01 00:00:00 UTC");
        assert_eq!(utc_stamp(UNIX_EPOCH + Duration::from_secs(1_700_000_000)), "2023-11-14 22:13:20 UTC");
        assert_eq!(utc_stamp(UNIX_EPOCH + Duration::from_secs(951_782_400)), "2000-02-29 00:00:00 UTC");
        assert_eq!(progress_line("translate", 5, 10, "Translating shaders: 5 of 10"), "[translate] 5/10 Translating shaders: 5 of 10");
        assert_eq!(progress_line("extract", 0, 0, "Finding..."), "[extract] Finding...");
        assert_eq!(fmt_dur(Duration::from_secs(3723)), "1h 02m 03s");
        assert_eq!(fmt_dur(Duration::from_secs(65)), "1m 05s");
    }

    #[test]
    fn reporter_throttles_file_writes() {
        let t = Tmp::new("reporter");
        let mut r = Reporter::new(&t.0);
        r.stage = "shaders";
        r.tick("translate", 0, 3, "Translating shaders: 0 of 3");
        r.tick("translate", 1, 3, "Translating shaders: 1 of 3");
        r.tick("translate", 1, 3, "Translating shaders: 1 of 3");
        r.tick("translate", 2, 3, "Translating shaders: 2 of 3");
        let log = fs::read_to_string(t.0.join(LOG_FILE)).unwrap();
        assert_eq!(log.lines().count(), 1, "{}", log);
        r.flush();
        let log = fs::read_to_string(t.0.join(LOG_FILE)).unwrap();
        assert_eq!(log.lines().count(), 2);
        assert!(log.ends_with("[translate] 2/3 Translating shaders: 2 of 3\n"));
        assert!(fs::read_to_string(t.0.join(STATUS_FILE)).unwrap().contains("2 of 3"));
        r.tick("finish", 3, 3, "Cleaning up...");
        r.end_phase();
        let s = r.summary(Duration::from_secs(1));
        assert!(s.starts_with("Time per phase: shaders/translate ") && s.contains("shaders/finish ") && s.ends_with("total 1.0s"), "{}", s);
    }
}
