//! `pmu patch-obb new|ggm|check ...` (pkgzip::patch_obb): an add-on's bundles beside the installed
//! game; `check` exits 3 on an incomplete copy. `pack` is not ported yet.
use std::process::ExitCode;

pub fn run(args: &[String]) -> ExitCode {
    if args.len() == 3 && args[0] == "pack" {
        eprintln!("pmu: patch-obb pack: not ported yet (run `python -m pmunity patch-obb pack`)");
        return ExitCode::from(2);
    }
    let out = pkgzip::patch_obb::main(args).map(|text| print!("{text}"));
    super::status("patch-obb", out)
}
