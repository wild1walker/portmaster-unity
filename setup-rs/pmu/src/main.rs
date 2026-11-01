//! `pmu <command> ...`: the commands of `python -m pmunity` (convert/pmunity/__main__.py) that a
//! port's setup runs on the handheld, with the same arguments, output files and exit statuses, so a
//! setup script can run either. Each command lives in cmd/<name>.rs; until a command is ported it
//! exits 2 and the setup keeps running the Python one.
use std::process::ExitCode;

mod cmd;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(name) = args.first() else {
        eprintln!(
            "usage: pmu <command> [args...]\ncommands: {}",
            cmd::NAMES.join(", ")
        );
        return ExitCode::from(2);
    };
    match cmd::run(name, &args[1..]) {
        Some(code) => code,
        None => {
            eprintln!("pmu: {name}: not ported yet (run `python -m pmunity {name}`)");
            ExitCode::from(2)
        }
    }
}
