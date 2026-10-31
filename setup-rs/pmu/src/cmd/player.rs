//! `pmu player donor <version> <dir> --apk APK [--apk APK] [--backend mono|il2cpp]
//! [--abi armeabi-v7a|arm64-v8a] [--managed]` (pkgzip::player), its arguments read as player.py's
//! argparse reads them (options anywhere, `--opt=value`, unique prefixes); argument errors exit 2.
//! `player url` and `player fetch` (Unity's servers) are not ported.
use std::process::ExitCode;

const USAGE: &str = "usage: pmunity player donor [-h] --apk APK [--backend {mono,il2cpp}] \
                     [--abi {armeabi-v7a,arm64-v8a}] [--managed] version dir";

const OPTS: &[&str] = &["--help", "--apk", "--backend", "--abi", "--managed"];

fn usage_error(msg: &str) -> ExitCode {
    eprintln!("{USAGE}\npmunity player donor: error: {msg}");
    ExitCode::from(2)
}

/// The option a token names: exactly, or by a prefix only one option has (argparse's abbreviations).
fn option(tok: &str) -> Result<&'static str, String> {
    if let Some(o) = OPTS.iter().find(|o| **o == tok) {
        return Ok(*o);
    }
    let hits: Vec<&str> = OPTS
        .iter()
        .copied()
        .filter(|o| o.starts_with(tok))
        .collect();
    match hits.as_slice() {
        [o] => Ok(*o),
        [] => Err(format!("unrecognized arguments: {tok}")),
        many => Err(format!(
            "ambiguous option: {tok} could match {}",
            many.join(", ")
        )),
    }
}

pub fn run(args: &[String]) -> ExitCode {
    match args.first().map(String::as_str) {
        Some("donor") => {}
        Some("-h" | "--help") => {
            println!("usage: pmunity player [-h] {{url,fetch,donor}} ...");
            return ExitCode::SUCCESS;
        }
        Some(c @ ("url" | "fetch")) => {
            eprintln!("pmu: player {c}: not ported yet (run `python -m pmunity player {c}`)");
            return ExitCode::from(2);
        }
        Some(c) => {
            eprintln!(
                "usage: pmunity player [-h] {{url,fetch,donor}} ...\npmunity player: error: argument cmd: \
                 invalid choice: '{c}' (choose from 'url', 'fetch', 'donor')"
            );
            return ExitCode::from(2);
        }
        None => {
            eprintln!("usage: pmunity player [-h] {{url,fetch,donor}} ...\npmunity player: error: the following arguments are required: cmd");
            return ExitCode::from(2);
        }
    }
    let (mut pos, mut apks) = (Vec::new(), Vec::new());
    let (mut backend, mut abi, mut managed) =
        ("mono".to_string(), "armeabi-v7a".to_string(), false);
    let mut extra = Vec::new();
    let mut it = args[1..].iter();
    let mut only_pos = false;
    while let Some(tok) = it.next() {
        if only_pos || !tok.starts_with('-') || tok == "-" {
            pos.push(tok.clone());
            continue;
        }
        if tok == "--" {
            only_pos = true;
            continue;
        }
        if tok == "-h" {
            println!("{USAGE}");
            return ExitCode::SUCCESS;
        }
        if !tok.starts_with("--") {
            extra.push(tok.clone());
            continue;
        }
        let (name, inline) = match tok.split_once('=') {
            Some((n, v)) => (n, Some(v.to_string())),
            None => (tok.as_str(), None),
        };
        let opt = match option(name) {
            Ok(o) => o,
            Err(_) if inline.is_none() => {
                extra.push(tok.clone());
                continue;
            }
            Err(m) => return usage_error(&m),
        };
        match opt {
            "--help" => {
                println!("{USAGE}");
                return ExitCode::SUCCESS;
            }
            "--managed" => {
                if let Some(v) = inline {
                    return usage_error(&format!(
                        "argument --managed: ignored explicit argument '{v}'"
                    ));
                }
                managed = true;
            }
            _ => {
                let v = match inline {
                    Some(v) => v,
                    None => match it.next() {
                        Some(v) if !(v.starts_with('-') && v.len() > 1) => v.clone(),
                        _ => return usage_error(&format!("argument {opt}: expected one argument")),
                    },
                };
                let choices: &[&str] = match opt {
                    "--backend" => &["mono", "il2cpp"],
                    "--abi" => &["armeabi-v7a", "arm64-v8a"],
                    _ => &[],
                };
                if !choices.is_empty() && !choices.contains(&v.as_str()) {
                    let list: Vec<String> = choices.iter().map(|c| format!("'{c}'")).collect();
                    return usage_error(&format!(
                        "argument {opt}: invalid choice: '{v}' (choose from {})",
                        list.join(", ")
                    ));
                }
                match opt {
                    "--apk" => apks.push(v),
                    "--backend" => backend = v,
                    _ => abi = v,
                }
            }
        }
    }
    let mut missing: Vec<&str> = ["version", "dir"]
        .into_iter()
        .skip(pos.len().min(2))
        .collect();
    if apks.is_empty() {
        missing.push("--apk");
    }
    if !missing.is_empty() {
        return usage_error(&format!(
            "the following arguments are required: {}",
            missing.join(", ")
        ));
    }
    extra.extend(pos.drain(2..));
    if !extra.is_empty() {
        eprintln!("usage: pmunity [-h] {{convert,shaders,shaders-export,android-data}} ...\npmunity: error: unrecognized arguments: {}", extra.join(" "));
        return ExitCode::from(2);
    }
    let r = pkgzip::player::donor(&pos[0], &pos[1], &apks, &backend, &abi, managed, &mut |m| {
        eprintln!("{m}")
    });
    super::status("player", r)
}
