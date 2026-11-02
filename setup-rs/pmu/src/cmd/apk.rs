//! `pmu apk <file>... <out> [--target T] [--abi A] [--any-unity-version] [--force] [--dry-run]
//! [--json] [--il2cpp ...]` (crates/apk, apk.py): a game's own Android build laid out for ua_host.
//! With --il2cpp --on-device a Mono build is compiled for the arm64 IL2CPP player on this machine
//! (crates/il2cpp); with --il2cpp alone (Docker, a porter's PC) it stops with status 2 (the
//! Python's `pmunity apk --il2cpp` does that).
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
        il2cpp_opts: apk::il2cpp::Il2cppOpts {
            home: a.get("--home").map(str::to_string),
            pkg: a.get("--pkg").map(str::to_string),
            jobs: a
                .get("--jobs")
                .and_then(|j| pkgzip::py::int(j, 10))
                .map(|j| j.max(0) as usize),
            on_device: a.has("--on-device"),
            toolchain: a.get("--toolchain").map(str::to_string),
            link_xml: a.opts.get("--link-xml").cloned().unwrap_or_default(),
            strip: a.get("--strip").map(str::to_string),
            keep_cpp: a.has("--keep-cpp"),
        },
    };
    if o.il2cpp && o.il2cpp_opts.on_device && !o.dry_run {
        il2cpp::device::install_ca();
    }
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
        Err(apk::Fail::Exit(m)) => {
            eprintln!("{m}");
            ExitCode::from(1)
        }
        Err(apk::Fail::NotPorted(m)) => {
            eprintln!("pmu: apk: not ported yet: {m} (run `python -m pmunity apk`)");
            ExitCode::from(2)
        }
    }
}
