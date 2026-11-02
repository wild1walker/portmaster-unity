//! Command lines (the input's NUL-separated parts) read as pmu reads them: the argparse rules of
//! pmu's small commands (cmd/argp.rs, with unpack's, winbcl's and a parser of every kind of
//! option), `convert`'s (with --set values read as JSON) and `android-data`'s.
#![no_main]
use libfuzzer_sys::fuzz_target;

#[allow(dead_code)]
#[path = "../../pmu/src/cmd/argp.rs"]
mod argp;

use argp::{choice, flag, val, Opt, Parser};

fn parsers() -> Vec<Parser> {
    vec![
        Parser {
            prog: "pmunity unpack",
            positionals: &["data"],
            opts: vec![val("--out"), flag("--remove")],
        },
        Parser {
            prog: "python -m pmunity.winbcl",
            positionals: &["data", "out"],
            opts: vec![
                val("--version"),
                val("--installer"),
                val("--player"),
                flag("--no-facades"),
                val("--json"),
            ],
        },
        Parser {
            prog: "pmunity test",
            positionals: &["a", "b", "c"],
            opts: vec![
                choice("--backend", &["mono", "il2cpp"]),
                Opt {
                    name: "--apk",
                    value: true,
                    choices: &[],
                    append: true,
                    required: true,
                    metavar: Some("APK"),
                },
                flag("--managed"),
                flag("--man"),
            ],
        },
    ]
}

fuzz_target!(|data: &[u8]| {
    let args = pmu_fuzz::args(data);
    for p in parsers() {
        if let Ok(a) = p.parse(&args) {
            let _ = (a.get("--out"), a.has("--remove"));
        }
        // (apk's: its one positional takes one or more values)
        if let Ok(a) = p.parse_many(&args) {
            let _ = a.pos.len();
        }
    }
    if let Ok(Some(c)) = convert::cli::parse(&args) {
        let _ = c.sets.len();
    }
    let _ = androiddata::args::parse(&args);
});
