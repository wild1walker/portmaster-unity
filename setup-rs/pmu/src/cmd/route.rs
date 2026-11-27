//! `pmu route <game folder | <Game>_Data | <Game>.app | executable> [--json | --env] [--launcher
//! OUT.sh [--port NAME] [--title TEXT] [--template FILE]]` (crates/check's route, route.py): which
//! way a Unity game fits a PortMaster handheld, and why; --env prints the game.env the box64
//! port's setup writes for its launcher. Everything is read from file headers and the player
//! binary's strings; nothing of the game runs.
use super::argp::{flag, val, Opt, Parser};
use std::process::ExitCode;

pub fn run(args: &[String]) -> ExitCode {
    let p = Parser {
        prog: "pmu route",
        positionals: &["path"],
        opts: vec![flag("--json"), flag("--env"), Opt { metavar: Some("OUT"), ..val("--launcher") }, val("--port"), val("--title"), val("--template")],
    };
    let a = match p.parse(args) {
        Ok(a) => a,
        Err(code) => return code,
    };
    let s = |k: &str| a.get(k).map(String::from);
    let args = check::route::Args {
        path: a.pos[0].clone(),
        json: a.has("--json"),
        env: a.has("--env"),
        launcher: s("--launcher"),
        port: s("--port"),
        title: s("--title"),
        template: s("--template"),
    };
    // (the checkout is needed only for --launcher's default template)
    let mut starts = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        starts.push(exe.canonicalize().unwrap_or(exe));
    }
    if let Ok(cwd) = std::env::current_dir() {
        starts.push(cwd);
    }
    let repo = port::Repo::find(&starts);
    ExitCode::from(check::route::main(repo.as_ref(), &args))
}
