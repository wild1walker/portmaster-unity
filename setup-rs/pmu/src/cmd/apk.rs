//! `pmu apk <file>... <out> [--target T] [--abi A] [--any-unity-version] [--force] [--dry-run]
//! [--json] [--il2cpp ...]` (crates/apk, apk.py): a game's own Android build laid out for ua_host.
//! With --il2cpp, a Mono build that would be compiled for the arm64 IL2CPP player stops with
//! status 2 (the Python's `pmunity apk --il2cpp` does that).
use super::argp::{choice, flag, val, Opt, Parser};
use std::io::Write;
use std::process::ExitCode;

const PROG: &str = "pmunity apk";

pub fn run(args: &[String]) -> ExitCode {
    let p = Parser {
        prog: PROG,
        positionals: &["files [files ...]"],
        opts: vec![
            val("--target"),
            choice("--abi", &["arm64-v8a", "armeabi-v7a"]),
            flag("--any-unity-version"),
            flag("--force"),
            flag("--dry-run"),
            flag("--json"),
            flag("--il2cpp"),
            val("--home"),
            val("--pkg"),
            val("--jobs"),
            flag("--on-device"),
            val("--toolchain"),
            Opt {
                append: true,
                metavar: Some("FILE"),
                ..val("--link-xml")
            },
            choice("--strip", &["low", "none"]),
            flag("--keep-cpp"),
        ],
    };
    let a = match p.parse_many(args) {
        Ok(a) => a,
        Err(code) => return code,
    };
    if let Some(j) = a.get("--jobs") {
        if pkgzip::py::int(j, 10).is_none() {
            return p.error(&format!("argument --jobs: invalid int value: '{j}'"));
        }
    }
    if a.pos.len() < 2 {
        return p.error("give the input file(s) and the output directory");
    }
    let (inputs, out) = a.pos.split_at(a.pos.len() - 1);
    let json = a.has("--json");
    let o = apk::Opts {
        target: a.get("--target").unwrap_or("auto").to_string(),
        abi: a.get("--abi").map(str::to_string),
        any_version: a.has("--any-unity-version"),
        force: a.has("--force"),
        dry_run: a.has("--dry-run"),
        il2cpp: a.has("--il2cpp"),
    };
    let mut log = |m: &str| {
        if json {
            eprintln!("{m}");
        } else {
            println!("{m}");
            let _ = std::io::stdout().flush();
        }
    };
    match apk::run(inputs, &out[0], &o, &mut log) {
        Ok(res) => {
            if json {
                println!("{}", apk::json::dumps(&res));
            }
            ExitCode::SUCCESS
        }
        Err(apk::Fail::Refused(m)) => {
            eprintln!("{PROG}: refused: {m}");
            ExitCode::from(1)
        }
        Err(apk::Fail::Error(m)) => {
            eprintln!("pmu: error: {m}");
            ExitCode::from(1)
        }
        Err(apk::Fail::NotPorted(m)) => {
            eprintln!("pmu: apk: not ported yet: {m} (run `python -m pmunity apk`)");
            ExitCode::from(2)
        }
    }
}
