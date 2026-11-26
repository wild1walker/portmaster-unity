//! lockstep compare <A.hash> <B.hash> [--classes <n>]: the first frame two lockstep runs part at, and what
//!   differs there (exit 0: every frame the same, 1: they differ, 2: a file could not be read)
//! lockstep diff <A.dump> <B.dump> [--max <n>]: the objects and fields two dumps of one frame differ in
//!   (exit 0: none, 1: some)
//! The files are ua_host's (UA_LOCKSTEP_HASH, UA_LOCKSTEP_DUMP; runtime/host/lockstep.c, docs/lockstep.md).
use std::process::ExitCode;

fn read(p: &str) -> Result<String, ExitCode> {
    std::fs::read_to_string(p).map_err(|e| {
        eprintln!("lockstep: {}: {}", p, e);
        ExitCode::from(2)
    })
}

fn opt(args: &[String], name: &str, default: usize) -> usize {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1)).and_then(|v| v.parse().ok()).unwrap_or(default)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let files: Vec<&String> = args.iter().skip(1).take(2).collect();
    match (args.first().map(String::as_str), files.len()) {
        (Some("compare"), 2) => {
            let (a, b) = match (read(files[0]), read(files[1])) { (Ok(a), Ok(b)) => (a, b), (Err(e), _) | (_, Err(e)) => return e };
            let c = lockstep::compare(&lockstep::parse_run(&a), &lockstep::parse_run(&b), opt(&args, "--classes", 20));
            print!("{}", c.report);
            if c.frames == 0 { ExitCode::from(2) } else if c.first.is_some() { ExitCode::from(1) } else { ExitCode::SUCCESS }
        }
        (Some("diff"), 2) => {
            let (a, b) = match (read(files[0]), read(files[1])) { (Ok(a), Ok(b)) => (a, b), (Err(e), _) | (_, Err(e)) => return e };
            let (n, out) = lockstep::diff(&a, &b, opt(&args, "--max", 200));
            print!("{}", out);
            if n == 0 {
                println!("the dumps are the same");
                ExitCode::SUCCESS
            } else {
                ExitCode::from(1)
            }
        }
        _ => {
            eprintln!("usage: lockstep compare <A.hash> <B.hash> [--classes <n>]\n       lockstep diff <A.dump> <B.dump> [--max <n>]");
            ExitCode::from(2)
        }
    }
}
