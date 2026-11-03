//! `pmu wwise <banks> <out> [--check] [--wwise-lib PATH]` (crates/wwise; no Python twin: the
//! converter is frozen): a game's Wwise soundbanks (a folder of .bnk, or one) read once into the
//! data of ua_host's Wwise player, docs/wwise-player.md: `<out>/<Bank>.uaww.json` and
//! `<Bank>.uaww.pak`. Wwise Vorbis's codebooks come from the game's own Wwise library
//! (`--wwise-lib`: the library or a folder with it; default: the Plugins folder nearest above the
//! banks); without them the Vorbis media are left out. Media or objects it cannot read are left
//! out with a warning (the rest still plays). Status: 0 done, 1 failed
//! (or `--check` found a problem), 2 usage, 3 a bank of a version the converter does not read.
use super::argp::{flag, val, Parser};
use std::process::ExitCode;

pub fn run(args: &[String]) -> ExitCode {
    let p = Parser {
        prog: "pmu wwise",
        positionals: &["banks", "out"],
        opts: vec![flag("--check"), val("--wwise-lib")],
    };
    let a = match p.parse(args) {
        Ok(a) => a,
        Err(c) => return c,
    };
    let opts = wwise::Opts {
        check: a.has("--check"),
        wwise_lib: a.get("--wwise-lib").map(std::path::PathBuf::from),
    };
    let t = std::time::Instant::now();
    let r = wwise::run(
        std::path::Path::new(&a.pos[0]),
        std::path::Path::new(&a.pos[1]),
        &opts,
        &mut |l| println!("{l}"),
    );
    match r {
        Ok(s) => {
            for w in &s.problems {
                eprintln!("pmu wwise: warning: {w}");
            }
            // (peak memory: the handhelds have 1 GB)
            let mut ru: libc::rusage = unsafe { std::mem::zeroed() };
            unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut ru) };
            println!(
                "pmu wwise: done in {:.1} s, peak memory {} MB{}",
                t.elapsed().as_secs_f64(),
                ru.ru_maxrss / 1024,
                if s.problems.is_empty() {
                    String::new()
                } else {
                    format!(", {} warnings", s.problems.len())
                }
            );
            if s.check_failures > 0 {
                eprintln!("pmu wwise: check: {} problems", s.check_failures);
                return ExitCode::from(1);
            }
            ExitCode::SUCCESS
        }
        Err(wwise::Error::Unsupported(e)) => {
            eprintln!("pmu wwise: {e}");
            ExitCode::from(3)
        }
        Err(wwise::Error::Fail(e)) => {
            eprintln!("pmu wwise: error: {e}");
            ExitCode::from(1)
        }
    }
}
