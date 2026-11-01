//! `pmu android-data --src --pkg --out --name --package ... --pack all` (androiddata): the APK and
//! OBB(s) Unity's Android player reads; exit 4 (NO_SPACE) when the card is too full to pack.
use std::process::ExitCode;

pub fn run(args: &[String]) -> ExitCode {
    match androiddata::android_data_main(args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => ExitCode::from(say(e)),
    }
}

/// An androiddata stop as the Python ends: its message on stderr (none for a bare status).
pub fn say(e: androiddata::Stop) -> u8 {
    use androiddata::Stop;
    let code = e.status();
    match e {
        Stop::Exit(m) => eprintln!("{m}"),
        Stop::Fail(m) => eprintln!("pmu: error: {m}"),
        Stop::NotPorted(m) => eprintln!(
            "pmu: android-data: not ported yet: {m} (run `python -m pmunity android-data`)"
        ),
        Stop::Status(_) => {}
    }
    code
}
