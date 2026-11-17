//! `pmu fmod <banks> <out> [--check] [--fmod-lib PATH]` (crates/fmod; no Python twin): a game's
//! FMOD Studio banks (a folder of .bank, or one) read once into the data of ua_host's FMOD player,
//! docs/fmod-player.md: `<out>/<Bank>.uafm.json` and `<Bank>.uafm.pak`. The Vorbis setup headers
//! come from the game's own FMOD library (`--fmod-lib`: libfmod.so or a folder with it; default:
//! the Plugins folder nearest above the banks); without them the samples are left out. Objects or
//! samples it cannot read are left out with a warning (the rest still plays). Status: 0 done,
//! 1 failed (or `--check` found a problem), 2 usage, 3 banks of a format the converter does not read.
use super::argp::{flag, val, Parser};
use std::process::ExitCode;

pub fn run(args: &[String]) -> ExitCode {
    let p = Parser {
        prog: "pmu fmod",
        positionals: &["banks", "out"],
        opts: vec![flag("--check"), val("--fmod-lib")],
    };
    let a = match p.parse(args) {
        Ok(a) => a,
        Err(c) => return c,
    };
    let opts = fmod::Opts {
        check: a.has("--check"),
        fmod_lib: a.get("--fmod-lib").map(std::path::PathBuf::from),
    };
    let t = std::time::Instant::now();
    let r = fmod::run(
        std::path::Path::new(&a.pos[0]),
        std::path::Path::new(&a.pos[1]),
        &opts,
        &mut |l| println!("{l}"),
    );
    match r {
        Ok(s) => {
            for w in &s.problems {
                eprintln!("pmu fmod: warning: {w}");
            }
            // (peak memory: the handhelds have 1 GB)
            let mut ru: libc::rusage = unsafe { std::mem::zeroed() };
            unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut ru) };
            println!(
                "pmu fmod: done in {:.1} s, peak memory {} MB{}",
                t.elapsed().as_secs_f64(),
                ru.ru_maxrss / 1024,
                if s.problems.is_empty() {
                    String::new()
                } else {
                    format!(", {} warnings", s.problems.len())
                }
            );
            if s.check_failures > 0 {
                eprintln!("pmu fmod: check: {} problems", s.check_failures);
                return ExitCode::from(1);
            }
            ExitCode::SUCCESS
        }
        Err(fmod::Error::Unsupported(e)) => {
            eprintln!("pmu fmod: {e}");
            ExitCode::from(3)
        }
        Err(fmod::Error::Fail(e)) => {
            eprintln!("pmu fmod: error: {e}");
            ExitCode::from(1)
        }
    }
}
