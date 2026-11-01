//! `pmu convert <data> [<out>] --config <json> [--plugin <py>] [--steps a,b] [--set k=v]...`
//! (convert::cli): the in-place conversion with its journal, as `python -m pmunity convert`.
use std::process::ExitCode;

pub fn run(args: &[String]) -> ExitCode {
    ExitCode::from(convert::cli::main(args))
}

/// `pmu journal-files <data>/.pmunity.json`: the files the journal's steps have finished (0
/// before the first), for the setup's rerun-after-SIGKILL loop (resume.txt's journal_files).
pub fn journal_files(args: &[String]) -> ExitCode {
    let n = args
        .first()
        .map_or(0, |p| convert::job::journal_files(std::path::Path::new(p)));
    println!("{n}");
    ExitCode::SUCCESS
}
