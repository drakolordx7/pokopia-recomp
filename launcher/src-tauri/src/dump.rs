// SPDX-License-Identifier: GPL-3.0-or-later
//! Validates a decrypted game dump folder. Read-only: nothing here writes to the dump.
//!
//! Expected layout: `<dump>/base/{exefs,romfs}` and `<dump>/updates/<version>/{exefs,romfs}`.
//! The highest valid update is used; with none, the base.

use serde::Serialize;
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Serialize, Clone)]
pub struct Check {
    pub label: String,
    pub ok: bool,
}

#[derive(Serialize, Clone)]
pub struct Candidate {
    /// "base" or "update"
    pub kind: String,
    /// Update version folder name; empty for the base.
    pub version: String,
    pub path: String,
    pub exefs: String,
    pub romfs: String,
    pub ok: bool,
    pub checks: Vec<Check>,
}

#[derive(Serialize, Clone)]
pub struct DumpReport {
    pub path: String,
    pub exists: bool,
    pub errors: Vec<String>,
    pub candidates: Vec<Candidate>,
    pub chosen: Option<Candidate>,
}

pub fn fwd(p: &Path) -> String {
    p.to_string_lossy().replace('\\', "/")
}

fn check_candidate(kind: &str, version: &str, dir: &Path, files: &[String]) -> Candidate {
    let exefs = dir.join("exefs");
    let romfs = dir.join("romfs");
    let mut checks = Vec::new();
    for f in files {
        checks.push(Check { label: format!("exefs/{}", f), ok: exefs.join(f).is_file() });
    }
    checks.push(Check { label: "romfs/".to_string(), ok: romfs.is_dir() });
    let ok = checks.iter().all(|c| c.ok);
    Candidate {
        kind: kind.to_string(),
        version: version.to_string(),
        path: fwd(dir),
        exefs: fwd(&exefs),
        romfs: fwd(&romfs),
        ok,
        checks,
    }
}

/// Newest first: numeric versions by value, then anything non-numeric by name.
fn version_order(a: &str, b: &str) -> std::cmp::Ordering {
    match (a.parse::<u64>(), b.parse::<u64>()) {
        (Ok(x), Ok(y)) => y.cmp(&x),
        (Ok(_), Err(_)) => std::cmp::Ordering::Less,
        (Err(_), Ok(_)) => std::cmp::Ordering::Greater,
        (Err(_), Err(_)) => b.cmp(a),
    }
}

pub fn scan(path: &str) -> DumpReport {
    let root = PathBuf::from(path.trim());
    // the exefs files and the example folder name come from the game manifest
    let (files, example) = match crate::manifest::get() {
        Ok(m) => (m.exefs_files(), m.dump.example_folder.clone()),
        Err(e) => {
            return DumpReport { path: fwd(&root), exists: root.is_dir(), errors: vec![e], candidates: Vec::new(), chosen: None };
        }
    };
    let mut report = DumpReport {
        path: fwd(&root),
        exists: root.is_dir(),
        errors: Vec::new(),
        candidates: Vec::new(),
        chosen: None,
    };
    if path.trim().is_empty() {
        report.errors.push("No folder chosen yet.".to_string());
        return report;
    }
    if !report.exists {
        report.errors.push("That folder does not exist or cannot be read.".to_string());
        return report;
    }
    let base = root.join("base");
    let updates = root.join("updates");
    if !base.is_dir() && !updates.is_dir() {
        report.errors.push(format!(
            "This folder has no base/ or updates/ subfolder. Pick the folder that contains them (for example {}).",
            example
        ));
        return report;
    }

    if updates.is_dir() {
        let mut versions: Vec<String> = fs::read_dir(&updates)
            .map(|rd| {
                rd.filter_map(|e| e.ok())
                    .filter(|e| e.path().is_dir())
                    .map(|e| e.file_name().to_string_lossy().to_string())
                    .collect()
            })
            .unwrap_or_default();
        versions.sort_by(|a, b| version_order(a, b));
        for v in versions {
            report.candidates.push(check_candidate("update", &v, &updates.join(&v), &files));
        }
    }
    if base.is_dir() {
        report.candidates.push(check_candidate("base", "", &base, &files));
    }

    report.chosen = report.candidates.iter().find(|c| c.ok).cloned();
    if report.chosen.is_none() {
        report.errors.push(format!(
            "No usable game files found. Each of base/ or an updates/<version>/ folder needs exefs/ ({}) and romfs/.",
            files.join(", ")
        ));
    }
    report
}
