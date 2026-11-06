// SPDX-License-Identifier: GPL-3.0-or-later
//! Raw dumps: the player's NSP / XCI files (base game plus an update) and their own keys, turned into the extracted layout the rest of
//! the launcher reads (`<game>/dump/base/exefs`, `<game>/dump/updates/<version>/{exefs,romfs}`, see dump.rs). The work is done by
//! `tools/extract/nx_extract.exe` (nx repo, tools/extract: NCA / NSP / XCI readers, the update applied to the RomFS, key derivation);
//! this file starts it and reads its progress lines. Key files are passed to it by path; the launcher never reads them.
//!
//! Also ZBIC: an ExeFS whose NSOs use ZBIC compression (NSO flag bit 7, runtime/docs/zbic.md) is converted to plain NSOs in
//! `<game>/build/exefs_plain` before the game build, because the lifter reads plain NSOs only. The runtime reads ZBIC itself, so the
//! game's ini keeps pointing at the dump's own ExeFS.
//! No crate-local imports, so it can be built alone like gamebuild.rs.

use serde::Serialize;
use std::fs;
use std::io::{BufRead, BufReader, Read};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

/// Folder in the game folder that receives the extracted dump.
pub const OUT_REL: &str = "dump";

/// What `nx_extract --check` found (the page shows it before Set up).
#[derive(Serialize, Clone, Default)]
pub struct Found {
    /// "base" or "updates/<version>": the part the game will use
    pub dir: String,
    pub version: u64,
    pub display_version: String,
    pub build_id: String,
    pub files: u64,
    pub bytes: u64,
    pub zbic_modules: u64,
    pub notes: Vec<String>,
}

pub struct Tick {
    pub phase: String,
    pub done: u64,
    pub total: u64,
    pub message: String,
}

/// The extractor in the package (`tools/extract/nx_extract.exe`), or `RECOMP_NX_EXTRACT` for development builds.
pub fn find_tool(tools_dir: &Path) -> Result<PathBuf, String> {
    if let Ok(p) = std::env::var("RECOMP_NX_EXTRACT") {
        let p = PathBuf::from(p);
        if p.is_file() {
            return Ok(p);
        }
    }
    let p = tools_dir.join("extract").join("nx_extract.exe");
    if p.is_file() {
        Ok(p)
    } else {
        Err(format!("The dump extractor {} is missing. Extract the release zip again so tools/ is complete.", p.display()))
    }
}

fn quiet(cmd: &mut Command) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS
        cmd.creation_flags(0x0800_0000 | 0x0000_4000);
    }
}

fn base_args(cmd: &mut Command, inputs: &[String], keys: &[String], title_id: &str) {
    for i in inputs {
        cmd.arg("--input").arg(i);
    }
    cmd.arg("--keys").arg(keys.join(","));
    cmd.arg("--title-id").arg(title_id);
}

fn check_args(inputs: &[String], keys: &[String], title_id: &str) -> Result<(), String> {
    if inputs.is_empty() {
        return Err("Choose your dump file(s) first (.nsp or .xci; the base game and, if the game needs it, the update).".into());
    }
    if keys.is_empty() {
        return Err("Choose your prod.keys file.".into());
    }
    for p in inputs.iter().chain(keys.iter()) {
        if !Path::new(p).is_file() {
            return Err(format!("{} does not exist or cannot be read.", p));
        }
    }
    if title_id.trim().len() != 16 {
        return Err("The game description (game.toml) has no [game] title_id.".into());
    }
    Ok(())
}

fn parse_found(json: &str, notes: Vec<String>) -> Found {
    let v: serde_json::Value = serde_json::from_str(json).unwrap_or_default();
    let s = |k: &str| v.get(k).and_then(|x| x.as_str()).unwrap_or("").to_string();
    let n = |k: &str| v.get(k).and_then(|x| x.as_u64()).unwrap_or(0);
    Found {
        dir: s("dir"),
        version: n("version"),
        display_version: s("display_version"),
        build_id: s("build_id"),
        files: n("files"),
        bytes: n("bytes"),
        zbic_modules: n("zbic_modules"),
        notes,
    }
}

/// Runs the tool, feeding "@progress" lines to `report`. Returns (exit code, the "@result" JSON, notes, the "@error" text).
fn run_tool(mut cmd: Command, cancel: Option<(&AtomicBool, &Path)>, mut report: impl FnMut(Tick)) -> Result<(i32, String, Vec<String>, Option<String>), String> {
    quiet(&mut cmd);
    let mut child = cmd.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().map_err(|e| format!("could not start the dump extractor: {}", e))?;
    let out = child.stdout.take().unwrap();
    let mut err = child.stderr.take().unwrap();
    let (tx, rx) = std::sync::mpsc::channel::<String>();
    std::thread::spawn(move || BufReader::new(out).lines().map_while(Result::ok).for_each(|l| drop(tx.send(l))));
    let err_text = std::thread::spawn(move || {
        let mut s = String::new();
        let _ = err.read_to_string(&mut s);
        s
    });
    let (mut result, mut notes, mut error) = (String::new(), Vec::new(), None);
    let mut asked = false;
    loop {
        if let Some((flag, file)) = cancel {
            if flag.load(Ordering::SeqCst) && !asked {
                // the tool polls this file and exits 5; nothing is killed from here
                let _ = fs::write(file, "pause\n");
                asked = true;
            }
        }
        match rx.recv_timeout(Duration::from_millis(200)) {
            Ok(l) => {
                if let Some(rest) = l.strip_prefix("@progress ") {
                    let mut it = rest.splitn(4, ' ');
                    let phase = it.next().unwrap_or("").to_string();
                    let done = it.next().and_then(|x| x.parse().ok()).unwrap_or(0);
                    let total = it.next().and_then(|x| x.parse().ok()).unwrap_or(0);
                    let message = it.next().unwrap_or("").to_string();
                    report(Tick { phase, done, total, message });
                } else if let Some(rest) = l.strip_prefix("@result ") {
                    result = rest.to_string();
                } else if let Some(rest) = l.strip_prefix("@note ") {
                    notes.push(rest.to_string());
                } else if let Some(rest) = l.strip_prefix("@error ") {
                    error = Some(rest.to_string());
                }
            }
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }
    let st = child.wait().map_err(|e| e.to_string())?;
    if error.is_none() && !st.success() {
        let t = err_text.join().unwrap_or_default();
        let t = t.trim();
        if !t.is_empty() {
            error = Some(t.lines().rev().take(6).collect::<Vec<_>>().into_iter().rev().collect::<Vec<_>>().join("\n"));
        }
    }
    Ok((st.code().unwrap_or(1), result, notes, error))
}

/// Opens the dump files with the keys and says what they hold. Writes nothing.
pub fn check(tool: &Path, inputs: &[String], keys: &[String], title_id: &str) -> Result<Found, String> {
    check_args(inputs, keys, title_id)?;
    let mut cmd = Command::new(tool);
    base_args(&mut cmd, inputs, keys, title_id);
    cmd.arg("--check");
    let (code, result, notes, error) = run_tool(cmd, None, |_| {})?;
    if code != 0 {
        return Err(error.unwrap_or_else(|| format!("The dump check failed (exit {}).", code)));
    }
    Ok(parse_found(&result, notes))
}

/// The extraction in `out` is complete and was made from these inputs (same paths and sizes).
pub fn is_extracted(out: &Path, inputs: &[String]) -> bool {
    let Some(v) = fs::read_to_string(out.join("extract.json")).ok().and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok()) else {
        return false;
    };
    let Some(list) = v.get("inputs").and_then(|x| x.as_array()) else { return false };
    if list.len() != inputs.len() {
        return false;
    }
    inputs.iter().all(|p| {
        let size = fs::metadata(p).map(|m| m.len()).unwrap_or(u64::MAX);
        list.iter().any(|e| e.get("path").and_then(|x| x.as_str()) == Some(p.as_str()) && e.get("size").and_then(|x| x.as_u64()) == Some(size))
    })
}

/// Extracts into `out`. Ok(true) = done, Ok(false) = paused (run again to continue), Err = the tool's message.
pub fn extract(tool: &Path, inputs: &[String], keys: &[String], title_id: &str, out: &Path, cancel: &AtomicBool, report: impl FnMut(Tick)) -> Result<bool, String> {
    check_args(inputs, keys, title_id)?;
    fs::create_dir_all(out).map_err(|e| format!("cannot create {}: {}", out.display(), e))?;
    let cancel_file = out.join("cancel");
    let _ = fs::remove_file(&cancel_file);
    let mut cmd = Command::new(tool);
    base_args(&mut cmd, inputs, keys, title_id);
    cmd.arg("--out").arg(out).arg("--cancel-file").arg(&cancel_file).arg("--log").arg(out.join("extract.log"));
    let (code, _result, _notes, error) = run_tool(cmd, Some((cancel, &cancel_file)), report)?;
    let _ = fs::remove_file(&cancel_file);
    match code {
        0 => Ok(true),
        5 => Ok(false),
        _ => Err(error.unwrap_or_else(|| format!("Extracting the dump failed (exit {}).", code))),
    }
}

/// True when an NSO in the ExeFS folder uses ZBIC compression (NSO0 flags bit 7).
pub fn exefs_has_zbic(exefs: &Path) -> bool {
    let Ok(rd) = fs::read_dir(exefs) else { return false };
    rd.filter_map(|e| e.ok()).any(|e| {
        let mut h = [0u8; 16];
        fs::File::open(e.path()).and_then(|mut f| f.read_exact(&mut h)).is_ok() && &h[..4] == b"NSO0" && h[12] & 0x80 != 0
    })
}

/// The ExeFS the game build should lift: the dump's own, or, when it holds ZBIC modules, a plain copy in `<game>/build/exefs_plain`.
pub fn exefs_for_build(tool: Option<&Path>, exefs: &Path, game_dir: &Path) -> Result<PathBuf, String> {
    if !exefs_has_zbic(exefs) {
        return Ok(exefs.to_path_buf());
    }
    let tool = tool.ok_or("The game program uses ZBIC compression and the dump extractor (tools/extract/nx_extract.exe) is missing.")?;
    let out = game_dir.join("build").join("exefs_plain");
    let mut cmd = Command::new(tool);
    cmd.arg("--plain-nso").arg(exefs).arg(&out);
    let (code, _r, _n, error) = run_tool(cmd, None, |_| {})?;
    if code != 0 {
        return Err(error.unwrap_or_else(|| format!("Decompressing the ZBIC game program failed (exit {}).", code)));
    }
    Ok(out)
}
