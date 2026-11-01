//! `pmu winbcl <data> <out> [--version V] [--installer FILE|URL] [--player PKG] [--no-facades]
//! [--json FILE]` (androiddata::winbcl, winbcl.py): Unity's class libraries a build lacks for the
//! Android player, from Unity's Windows editor installer; prints android-data's --managed-file
//! options.
use super::argp::{flag, val, Parser};
use androiddata::winbcl::{self, Fail};
use std::process::ExitCode;

pub fn run(args: &[String]) -> ExitCode {
    let p = Parser {
        prog: "python -m pmunity.winbcl",
        positionals: &["data", "out"],
        opts: vec![
            val("--version"),
            val("--installer"),
            val("--player"),
            flag("--no-facades"),
            val("--json"),
        ],
    };
    let a = match p.parse(args) {
        Ok(a) => a,
        Err(code) => return code,
    };
    match winbcl::main(
        &a.pos[0],
        &a.pos[1],
        a.get("--version"),
        a.get("--installer"),
        a.get("--player"),
        a.has("--no-facades"),
        a.get("--json"),
    ) {
        Ok(()) => ExitCode::SUCCESS,
        Err(Fail::Exit(m) | Fail::Error(m)) => {
            eprintln!("{m}");
            ExitCode::from(1)
        }
    }
}
