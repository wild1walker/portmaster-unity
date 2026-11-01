//! `pmu version <data>`: the Unity version a game's data was built with and whether the
//! converter supports it (`python -m pmunity version`: "<version> (supported)" or "(not
//! supported)"), read from globalgamemanagers, mainData or data.unity3d.
use std::process::ExitCode;

pub fn run(args: &[String]) -> ExitCode {
    let Some(data) = args.first() else {
        eprintln!("IndexError: list index out of range");
        return ExitCode::from(1);
    };
    match convert::version::of_data(std::path::Path::new(data)) {
        Ok(v) => {
            let (lo, hi) = (convert::version::FIRST, convert::version::LAST);
            let ok = lo <= v.line && v.line <= hi;
            println!(
                "{} ({})",
                v.text,
                if ok { "supported" } else { "not supported" }
            );
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(1)
        }
    }
}
