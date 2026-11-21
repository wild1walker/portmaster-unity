//! `pmu port new <name> --config port.toml [--out DIR] [--route R] [--title T] [--player PKG]
//! [--force]` and `pmu port package <port dir> [--out DIR] [--donors DIR] [--runtime DIR] ...`
//! (crates/port, port.py's `port new` and `port package`): a port's folder from its port.toml and
//! the route's templates, and its PortMaster release zip, the same files as the Python's. The
//! templates, the converter and the licences come from the portmaster-unity checkout this program
//! was built in ($PMUNITY_REPO, else the folders above the program, else above the working folder).
//! `port new --data <game>` (the check of a game) and `port check`, `port shot`, `port routes`
//! stop with status 2: the Python's.
use super::argp::{flag, val, Parser};
use std::path::PathBuf;
use std::process::ExitCode;

fn repo() -> Result<port::Repo, ExitCode> {
    let mut starts = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        starts.push(exe.canonicalize().unwrap_or(exe));
    }
    if let Ok(cwd) = std::env::current_dir() {
        starts.push(cwd);
    }
    port::Repo::find(&starts).ok_or_else(|| {
        eprintln!("pmu port: no portmaster-unity checkout (convert/pmunity/port_template): set PMUNITY_REPO");
        ExitCode::from(2)
    })
}

fn status(r: port::Result<PathBuf>) -> ExitCode {
    match r {
        Ok(_) => ExitCode::SUCCESS,
        Err(e @ pkgzip::Error::Fail(_)) => {
            eprintln!("pmu port: error: {e}");
            ExitCode::from(e.status())
        }
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(e.status())
        }
    }
}

pub fn run(args: &[String]) -> ExitCode {
    let usage = "usage: pmunity port [-h] {check,new,package,shot,routes} ...";
    let Some(sub) = args.first() else {
        eprintln!("{usage}\npmunity port: error: the following arguments are required: cmd");
        return ExitCode::from(2);
    };
    let rest = &args[1..];
    match sub.as_str() {
        "new" => new(rest),
        "package" => package(rest),
        "-h" | "--help" => {
            println!("{usage}\n\npmu port new and pmu port package (crates/port); check, shot, routes: python -m pmunity port");
            ExitCode::SUCCESS
        }
        "check" | "shot" | "routes" => {
            eprintln!("pmu port {sub}: not ported (run `python -m pmunity port {sub}`)");
            ExitCode::from(2)
        }
        _ => {
            eprintln!(
                "{usage}\npmunity port: error: argument cmd: invalid choice: '{sub}' (choose from 'check', 'new', 'package', 'shot', 'routes')"
            );
            ExitCode::from(2)
        }
    }
}

fn new(args: &[String]) -> ExitCode {
    let p = Parser {
        prog: "pmunity port new",
        positionals: &["name"],
        opts: vec![
            val("--data"),
            val("--config"),
            val("--out"),
            val("--route"),
            val("--title"),
            val("--player"),
            val("--asmpatch"),
            flag("--force"),
        ],
    };
    let a = match p.parse(args) {
        Ok(a) => a,
        Err(code) => return code,
    };
    let repo = match repo() {
        Ok(r) => r,
        Err(c) => return c,
    };
    if a.has("--data") {
        eprintln!(
            "pmu port new: --data (the check of the game) is not ported: run `python -m pmunity port new` with --data, \
             or give a port.toml that names game.unity_versions"
        );
        return ExitCode::from(2);
    }
    let s = |k: &str| a.get(k).map(String::from);
    let args = port::new::NewArgs {
        name: a.pos[0].clone(),
        data: None,
        config: s("--config"),
        out: s("--out"),
        route: s("--route"),
        title: s("--title"),
        player: s("--player"),
        force: a.has("--force"),
    };
    status(port::new::new(&repo, &args, &mut |m| println!("{m}")))
}

fn package(args: &[String]) -> ExitCode {
    let p = Parser {
        prog: "pmunity port package",
        positionals: &["port"],
        opts: vec![
            val("--out"),
            val("--donors"),
            val("--runtime"),
            val("--ua-host"),
            val("--media-licenses"),
            val("--pybundle"),
            val("--pmu"),
            val("--asmpatch-bin"),
            val("--pmhlslcc-bin"),
            val("--pmspirvcross-bin"),
            val("--mono64-work"),
            flag("--no-build"),
            flag("--submission"),
        ],
    };
    let a = match p.parse(args) {
        Ok(a) => a,
        Err(code) => return code,
    };
    let repo = match repo() {
        Ok(r) => r,
        Err(c) => return c,
    };
    let s = |k: &str| a.get(k).map(String::from);
    let opts = port::package::PackOpts {
        out: s("--out"),
        runtime: s("--runtime"),
        donors: s("--donors"),
        ua_host: s("--ua-host"),
        media_licenses: s("--media-licenses"),
        pybundle: s("--pybundle"),
        pmu: s("--pmu"),
        asmpatch_bin: s("--asmpatch-bin"),
        pmhlslcc_bin: s("--pmhlslcc-bin"),
        pmspirvcross_bin: s("--pmspirvcross-bin"),
        mono64_work: s("--mono64-work"),
        no_build: a.has("--no-build"),
        submission: a.has("--submission"),
        ..Default::default()
    };
    status(port::package::package(&repo, &a.pos[0], &opts, &mut |m| println!("{m}")))
}
