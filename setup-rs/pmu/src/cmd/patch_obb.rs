//! `pmu patch-obb new|ggm|pack|check ...` (pkgzip::patch_obb, androiddata::patch_obb): an add-on's
//! bundles beside the installed game; `check` exits 3 on an incomplete copy, `pack` writes the
//! patch OBB (its log on stderr, as the Python's).
use std::process::ExitCode;

pub fn run(args: &[String]) -> ExitCode {
    if args.len() == 3 && args[0] == "pack" {
        return match androiddata::patch_obb::pack(&args[1], &args[2], &mut |m| eprintln!("{m}")) {
            Ok(_) => ExitCode::SUCCESS,
            Err(e) => ExitCode::from(super::android_data::say(e)),
        };
    }
    let out = pkgzip::patch_obb::main(args).map(|text| print!("{text}"));
    super::status("patch-obb", out)
}
