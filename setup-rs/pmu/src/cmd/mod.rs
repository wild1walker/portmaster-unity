//! One module a command; each `run(args) -> ExitCode` takes the arguments after the command's name.
use std::process::ExitCode;

/// The commands this binary has, as the setup scripts name them.
pub const NAMES: &[&str] = &[];

/// Runs a command, or None when it is not ported yet.
pub fn run(name: &str, args: &[String]) -> Option<ExitCode> {
    let _ = args;
    match name {
        _ => None,
    }
}
