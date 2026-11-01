//! `pmu macpkg <root> <installer.pkg>...`: macOS installers unpacked (pkgzip::macpkg); exit 3 when
//! one is incomplete (a download or copy that stopped early).
use std::process::ExitCode;

pub fn run(args: &[String]) -> ExitCode {
    super::status("macpkg", pkgzip::macpkg::main(args))
}
