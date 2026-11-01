//! `pmu unpack <data> [--out DIR] [--remove]`: a compressed build's data.unity3d unpacked into
//! its files (unityfs::unpack, unpack.py), one block at a time; --remove deletes the archive.
use super::argp::{flag, val, Parser};
use std::path::Path;
use std::process::ExitCode;

pub fn run(args: &[String]) -> ExitCode {
    let p = Parser {
        prog: "pmunity unpack",
        positionals: &["data"],
        opts: vec![val("--out"), flag("--remove")],
    };
    let a = match p.parse(args) {
        Ok(a) => a,
        Err(code) => return code,
    };
    let out = a.get("--out").map(Path::new);
    match unityfs::unpack::unpack(Path::new(&a.pos[0]), out, a.has("--remove"), &mut |l| {
        println!("{l}")
    }) {
        Ok(_) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(1)
        }
    }
}
