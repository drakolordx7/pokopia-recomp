// SPDX-License-Identifier: GPL-3.0-or-later
//! Command line of pokopia-setup: hand-rolled parsing (no dependencies) and the usage text.

#[derive(Debug, PartialEq)]
pub enum Cmd {
    /// jobs: None = one per core (as the launcher); limit: shader test runs, 0 = all
    Setup { jobs: Option<usize>, limit: usize },
    Status,
    Play { wait: bool },
    /// Only "deck" exists.
    Preset(String),
    Help,
    Version,
}

#[derive(Debug, PartialEq)]
pub struct Args {
    pub cmd: Cmd,
    /// --game: folder with pkp.exe
    pub game: Option<String>,
    /// --dump: root of the decrypted dump (base/, updates/)
    pub dump: Option<String>,
}

pub const PRESETS: [&str; 1] = ["deck"];

fn number(opt: &str, v: &str, min: usize) -> Result<usize, String> {
    match v.trim().parse::<usize>() {
        Ok(n) if n >= min => Ok(n),
        _ => Err(format!("{} needs a whole number{}, not '{}'", opt, if min > 0 { format!(" of at least {}", min) } else { String::new() }, v)),
    }
}

/// Parses the arguments after the program name. Options may come before or after the command, as `--opt value` or `--opt=value`.
pub fn parse(argv: &[String]) -> Result<Args, String> {
    let mut pos: Vec<&str> = Vec::new();
    let (mut game, mut dump, mut jobs, mut limit) = (None, None, None, None);
    let (mut help, mut version, mut no_wait) = (false, false, false);
    let mut i = 0;
    while i < argv.len() {
        let a = argv[i].as_str();
        i += 1;
        if a == "-h" || a == "-?" || a == "/?" {
            help = true;
            continue;
        }
        if a == "-V" {
            version = true;
            continue;
        }
        if !a.starts_with("--") || a == "--" {
            pos.push(a);
            continue;
        }
        let (name, inline) = match a.find('=') {
            Some(eq) => (&a[..eq], Some(a[eq + 1..].to_string())),
            None => (a, None),
        };
        match name {
            "--help" | "--version" | "--no-wait" => {
                if inline.is_some() {
                    return Err(format!("{} takes no value", name));
                }
                match name {
                    "--help" => help = true,
                    "--version" => version = true,
                    _ => no_wait = true,
                }
            }
            "--game" | "--dump" | "--jobs" | "--limit" => {
                let v = match inline {
                    Some(v) => v,
                    None if i < argv.len() => {
                        i += 1;
                        argv[i - 1].clone()
                    }
                    None => return Err(format!("{} needs a value", name)),
                };
                match name {
                    "--game" => game = Some(v),
                    "--dump" => dump = Some(v),
                    "--jobs" => jobs = Some(number(name, &v, 1)?),
                    _ => limit = Some(number(name, &v, 0)?),
                }
            }
            _ => return Err(format!("unknown option '{}'", a)),
        }
    }
    if help {
        return Ok(Args { cmd: Cmd::Help, game, dump });
    }
    if version {
        return Ok(Args { cmd: Cmd::Version, game, dump });
    }
    let Some((&name, rest)) = pos.split_first() else {
        return Err("no command given".into());
    };
    let cmd = match name {
        "setup" => Cmd::Setup { jobs, limit: limit.unwrap_or(0) },
        "status" => Cmd::Status,
        "play" => Cmd::Play { wait: !no_wait },
        "preset" => match rest.first() {
            Some(p) if PRESETS.contains(p) => Cmd::Preset(p.to_string()),
            Some(p) => return Err(format!("unknown preset '{}' (known: {})", p, PRESETS.join(", "))),
            None => return Err(format!("preset needs a name (known: {})", PRESETS.join(", "))),
        },
        "help" => Cmd::Help,
        other => return Err(format!("unknown command '{}'", other)),
    };
    let extra = if name == "preset" { &rest[rest.len().min(1)..] } else { rest };
    if let Some(x) = extra.first() {
        return Err(format!("unexpected argument '{}' (paths go after --dump or --game)", x));
    }
    if !matches!(cmd, Cmd::Setup { .. }) {
        if jobs.is_some() {
            return Err("--jobs only works with setup".into());
        }
        if limit.is_some() {
            return Err("--limit only works with setup".into());
        }
    }
    if no_wait && !matches!(cmd, Cmd::Play { .. }) {
        return Err("--no-wait only works with play".into());
    }
    Ok(Args { cmd, game, dump })
}

pub const USAGE: &str = "\
pokopia-setup: first-run setup and start of the Pokopia PC port from a console, without the launcher window.
Works on Windows and under Proton/Wine (Steam Deck), where the launcher's window (WebView2) cannot open.

Usage: pokopia-setup <command> [options]

Commands:
  setup [--dump <dir>] [--jobs N] [--limit N]
      Checks the dump, writes the paths in pkp.ini, builds pkp.exe from your dump (release packages that carry
      tools/build/) and then the shader cache in shader_cache/spirv. Takes a long time (hours on a Steam Deck).
      --dump is needed the first time; it is remembered in launcher.json.
      --jobs N   parallel jobs (default: one per CPU core minus one; the game build uses one more)
      --limit N  test runs only: translate at most N shaders (the setup is then not marked complete)
  status
      Shows the game and dump folders, the chosen game files, whether pkp.exe is built, the shader setup state
      and the paths in pkp.ini.
  play [--no-wait]
      Starts pkp.exe --log pkp.log once setup has finished, waits for it and reports how it ended
      (exit code and which of pkp_crash.txt, pkp_freeze.txt, pkp_fatal.txt, pkp.log it wrote).
      --no-wait  start the game and return at once
  preset deck
      Steam Deck preset: sets [display] window_mode = borderless in controls.ini (creates the file if missing;
      the game adds its factory settings and bindings on first start). Nothing else is changed.
  help, --help   this text
  --version      the program's version

Options for every command:
  --game <dir>   folder with pkp.exe (default: the one remembered in launcher.json, else the folder this program is in)
  --dump <dir>   the decrypted dump: the folder with base/ and/or updates/<version>/ (stored in launcher.json)
The tools (Python, Zig, the shader translator) are always taken from tools/ next to this program.

Setup is resumable: Ctrl+C, closing the window or a crash loses at most the step in progress, and running
setup again continues where it stopped (finished modules and translated shaders are kept). To pause cleanly,
create an empty file named setup.cancel in the game folder; setup stops its tools and exits with code 2.

Files written:
  launcher.json             next to this program: dump_dir and game_dir (same format as the launcher's)
  <game>/setup.log          every setup step with a UTC timestamp (appended)
  <game>/setup_status.txt   the latest setup step, one line (watch it from a file manager)
  <game>/setup.cancel       create it to pause; setup deletes it
  <game>/pkp.ini            [fs] exefs, romfs, save_dir and [nvn] shader_cache (setup, play)
  <game>/controls.ini       preset deck only
Exit codes: 0 done, 1 error (or the game exited with an error), 2 setup paused, 3 usage error.

Examples (Steam Deck, Proton; Z: is the Linux root):
  pokopia-setup.exe setup --dump Z:\\home\\deck\\Games\\Pokopia_v2.0.1a
  pokopia-setup.exe status
  pokopia-setup.exe preset deck
  pokopia-setup.exe play
";

#[cfg(test)]
mod tests {
    use super::*;

    fn p(s: &str) -> Result<Args, String> {
        parse(&s.split_whitespace().map(String::from).collect::<Vec<_>>())
    }

    #[test]
    fn setup_and_its_options() {
        let a = p("setup --dump D:/dump --jobs 4 --limit=100").unwrap();
        assert_eq!(a.cmd, Cmd::Setup { jobs: Some(4), limit: 100 });
        assert_eq!(a.dump.as_deref(), Some("D:/dump"));
        assert_eq!(a.game, None);
        let a = p("--game=G:/pkp setup").unwrap();
        assert_eq!(a.cmd, Cmd::Setup { jobs: None, limit: 0 });
        assert_eq!(a.game.as_deref(), Some("G:/pkp"));
        assert!(p("setup --jobs 0").is_err());
        assert!(p("setup --jobs x").is_err());
        assert!(p("setup --limit").is_err());
    }

    #[test]
    fn status_play_preset_help_version() {
        assert_eq!(p("status --game /tmp/x").unwrap(), Args { cmd: Cmd::Status, game: Some("/tmp/x".into()), dump: None });
        assert_eq!(p("status --dump /d").unwrap().dump.as_deref(), Some("/d"));
        assert_eq!(p("play").unwrap().cmd, Cmd::Play { wait: true });
        assert_eq!(p("play --no-wait").unwrap().cmd, Cmd::Play { wait: false });
        assert_eq!(p("preset deck").unwrap().cmd, Cmd::Preset("deck".into()));
        assert_eq!(p("help").unwrap().cmd, Cmd::Help);
        assert_eq!(p("--help").unwrap().cmd, Cmd::Help);
        assert_eq!(p("setup -h").unwrap().cmd, Cmd::Help);
        assert_eq!(p("--version").unwrap().cmd, Cmd::Version);
    }

    #[test]
    fn usage_errors() {
        for bad in [
            "",
            "frobnicate",
            "status --bogus",
            "status extra",
            "preset",
            "preset handheld",
            "preset deck extra",
            "play --jobs 2",
            "status --limit 3",
            "setup --no-wait",
            "play --no-wait=1",
            "status --game",
        ] {
            assert!(p(bad).is_err(), "should fail: '{}'", bad);
        }
    }
}
