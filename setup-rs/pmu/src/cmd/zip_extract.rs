//! `pmu zip-extract <zip> <dir>`: what `python -m zipfile -e <zip> <dir>` does (the launcher
//! unpacks the setup's own zips that way before the first setup).
use std::process::ExitCode;

pub fn run(args: &[String]) -> ExitCode {
    let [zip, dir] = args else {
        eprintln!("usage: pmu zip-extract <zipfile> <output_dir>");
        return ExitCode::from(2);
    };
    super::status("zip-extract", pkgzip::zip::extract_all(zip, dir))
}
