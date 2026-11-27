//! `pmu slim-donor <apk> [out apk]` (pmu's own, no Python): a donor APK without its 32-bit player,
//! for an aarch64-only release (crates/port package.rs `slim_donor`, which `pmu port package` runs on
//! the donors of a port with `port.aarch64_only`): every entry but those in lib/<abi>/ for another
//! ABI than arm64-v8a, copied with the same bytes and compression. In place without [out apk].
//! Prints how many files of lib/ were left out.
use std::path::Path;
use std::process::ExitCode;

const USAGE: &str = "usage: pmu slim-donor <apk> [out apk]";

pub fn run(args: &[String]) -> ExitCode {
    if args.first().is_some_and(|a| a == "-h" || a == "--help") {
        println!("{USAGE}");
        return ExitCode::SUCCESS;
    }
    if args.is_empty() || args.len() > 2 || args.iter().any(|a| a.starts_with('-')) {
        eprintln!("{USAGE}");
        return ExitCode::from(2);
    }
    let src = Path::new(&args[0]);
    let dst = Path::new(args.get(1).unwrap_or(&args[0]));
    match port::package::slim_donor(src, dst) {
        Ok(gone) => {
            println!("{}: {} files of lib/ left out (aarch64 only)", dst.display(), gone.len());
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("pmu slim-donor: error: {e}");
            ExitCode::from(e.status())
        }
    }
}
