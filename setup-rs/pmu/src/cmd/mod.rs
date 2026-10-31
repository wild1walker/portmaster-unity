//! One module a command; each `run(args) -> ExitCode` takes the arguments after the command's name.
use std::process::ExitCode;

mod convert;
mod macpkg;
mod patch_obb;
mod player;
mod zip_extract;

/// The commands this binary has, as the setup scripts name them.
pub const NAMES: &[&str] = &[
    "macpkg",
    "zip-extract",
    "player",
    "patch-obb",
    "convert",
    "journal-files",
];

/// Runs a command, or None when it is not ported yet.
pub fn run(name: &str, args: &[String]) -> Option<ExitCode> {
    match name {
        "macpkg" => Some(macpkg::run(args)),
        "zip-extract" => Some(zip_extract::run(args)),
        "player" => Some(player::run(args)),
        "patch-obb" => Some(patch_obb::run(args)),
        "convert" => Some(convert::run(args)),
        "journal-files" => Some(convert::journal_files(args)),
        _ => None,
    }
}

/// A pkgzip command's end as the Python's: its message on stderr and its exit status (1, or 3 for
/// an incomplete installer or copy).
fn status(cmd: &str, r: pkgzip::Result<()>) -> ExitCode {
    match r {
        Ok(()) => ExitCode::SUCCESS,
        Err(e @ pkgzip::Error::Fail(_)) => {
            eprintln!("pmu {cmd}: error: {e}");
            ExitCode::from(e.status())
        }
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(e.status())
        }
    }
}
