//! `pmu port check|new|package|routes` (crates/port and crates/check, port.py's commands): the
//! check of a game (what it is, what it needs, which route fits), a port's folder from its
//! port.toml (or from the check of `--data <game>`) and the route's templates, its PortMaster
//! release zip, and the routes whose templates are here. The templates, the converter and the
//! licences come from the portmaster-unity checkout this program was built in ($PMUNITY_REPO, else
//! the folders above the program, else above the working folder).
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
        eprintln!("pmu port: no portmaster-unity checkout (setup-rs/templates): set PMUNITY_REPO");
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
    let usage = "usage: pmu port [-h] {check,new,package,routes} ...";
    let Some(sub) = args.first() else {
        eprintln!("{usage}\npmunity port: error: the following arguments are required: cmd");
        return ExitCode::from(2);
    };
    let rest = &args[1..];
    match sub.as_str() {
        "new" => new(rest),
        "package" => package(rest),
        "check" => check(rest),
        "routes" => routes(rest),
        "-h" | "--help" => {
            println!("{usage}\n\npmu port check, new, package and routes (crates/port, crates/check)");
            ExitCode::SUCCESS
        }
        _ => {
            eprintln!(
                "{usage}\npmunity port: error: argument cmd: invalid choice: '{sub}' (choose from 'check', 'new', 'package', 'routes')"
            );
            ExitCode::from(2)
        }
    }
}

fn new(args: &[String]) -> ExitCode {
    let p = Parser {
        prog: "pmu port new",
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
    let s = |k: &str| a.get(k).map(String::from);
    let args = port::new::NewArgs {
        name: a.pos[0].clone(),
        data: s("--data"),
        config: s("--config"),
        out: s("--out"),
        route: s("--route"),
        title: s("--title"),
        player: s("--player"),
        force: a.has("--force"),
    };
    let asmpatch = s("--asmpatch");
    let mut checker = |data: &str| check::check(&repo, data, asmpatch.as_deref(), true, &mut |_| {});
    status(port::new::new_with(&repo, &args, Some(&mut checker), &mut |m| println!("{m}")))
}

fn package(args: &[String]) -> ExitCode {
    let p = Parser {
        prog: "pmu port package",
        positionals: &["port"],
        opts: vec![
            val("--out"),
            val("--donors"),
            val("--runtime"),
            val("--ua-host"),
            val("--media-licenses"),
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

/// `port check <game> [--json] [--asmpatch CMD] [--no-scan]`: the report (status 1 when no route fits).
fn check(args: &[String]) -> ExitCode {
    let p = Parser {
        prog: "pmu port check",
        positionals: &["game"],
        opts: vec![flag("--json"), val("--asmpatch"), flag("--no-scan")],
    };
    let a = match p.parse(args) {
        Ok(a) => a,
        Err(code) => return code,
    };
    let repo = match repo() {
        Ok(r) => r,
        Err(c) => return c,
    };
    match check::check(&repo, &a.pos[0], a.get("--asmpatch"), !a.has("--no-scan"), &mut |m| eprintln!(".. {m}")) {
        Ok(rep) => {
            if a.has("--json") {
                print!("{}", check::report_json(&rep));
            } else {
                print!("{}", check::report_text(&rep));
            }
            if rep.truthy("route") { ExitCode::SUCCESS } else { ExitCode::from(1) }
        }
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(e.status())
        }
    }
}

/// `port routes`: the routes and whether their templates are here.
fn routes(args: &[String]) -> ExitCode {
    let p = Parser { prog: "pmu port routes", positionals: &[], opts: vec![] };
    if let Err(code) = p.parse(args) {
        return code;
    }
    let repo = match repo() {
        Ok(r) => r,
        Err(c) => return c,
    };
    let have = match port::routes::routes(&repo) {
        Ok(h) => h,
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::from(e.status());
        }
    };
    let known = ["android", "il2cpp-arm64", "mono-arm64", "box64", "apk"];
    for r in known {
        let t = if have.iter().any(|(n, _)| n == r) { "templates" } else { "no templates" };
        println!("{r:13} {t:13} {}", port::text::route_text(r).unwrap_or(""));
    }
    for (r, d) in &have {
        if !known.contains(&r.as_str()) {
            println!("{r:13} {:13} {}", "templates", d.str("title"));
        }
    }
    ExitCode::SUCCESS
}
