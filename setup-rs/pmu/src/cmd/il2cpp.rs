//! `pmu il2cpp <data> <out> --on-device ...` and `pmu il2cpp fetch <version> ...` (crates/il2cpp,
//! il2cpp.py and il2cpp_device.py): the game's code compiled with Unity's IL2CPP on the handheld.
//! Without --on-device (the Docker route of a porter's PC) it exits 2: the Python runs that.
use std::process::ExitCode;

pub fn run(args: &[String]) -> ExitCode {
    ExitCode::from(il2cpp::cli::main(args))
}
