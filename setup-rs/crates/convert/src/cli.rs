//! `convert <data> [<out>] --config <json> [--steps a,b] [--plugin <py>]... [--only REGEX]
//! [--set SECTION.KEY=VALUE]... [--any-unity-version]`: __main__.py's convert, with its
//! arguments (argparse's rules: `--opt value` or `--opt=value`, unambiguous prefixes), output
//! and exit statuses: 0 done, 1 an error (its message on stderr), 2 bad arguments or a part not
//! ported (run the Python), 3 not enough free space.
use crate::error::{Error, Result};
use crate::job::Job;
use crate::pyjson::{self, Json};
use crate::steps::{self, Args, StepFn};
use std::io::Write as _;
use std::path::Path;

const USAGE: &str = "usage: pmunity convert [-h] --config CONFIG [--steps STEPS] [--plugin PLUGIN]
                       [--only ONLY] [--set SECTION.KEY=VALUE]
                       [--any-unity-version]
                       data [out]";

const HELP: &str = "
positional arguments:
  data
  out

options:
  -h, --help            show this help message and exit
  --config CONFIG
  --steps STEPS         comma-separated (default: the config's \"steps\", else
                        shaders,textures,settings,audio)
  --plugin PLUGIN       a plugin's steps: pmu has them built in, by the file's
                        name (animclips.py: animclips)
  --only ONLY           regex: only convert matching files (tests)
  --set SECTION.KEY=VALUE
                        override a config value (VALUE as JSON, else a
                        string), e.g. --set shaders.precision=highp
  --any-unity-version   convert data of a Unity version outside 5.6 - 6000.6
                        (untested; layouts guessed)";

/// The default step list (BUILTIN_STEPS).
pub const BUILTIN_STEPS: [&str; 4] = ["shaders", "textures", "settings", "audio"];

/// The parsed arguments.
#[derive(Clone, Debug, Default)]
pub struct Cli {
    pub data: String,
    pub out: Option<String>,
    pub config: String,
    pub steps: Option<String>,
    pub plugins: Vec<String>,
    pub only: Option<String>,
    pub sets: Vec<String>,
    pub any_unity_version: bool,
}

const OPTIONS: [&str; 7] = [
    "--help",
    "--config",
    "--steps",
    "--plugin",
    "--only",
    "--set",
    "--any-unity-version",
];

/// Parses convert's arguments as argparse does: Ok(None) for --help.
pub fn parse(args: &[String]) -> std::result::Result<Option<Cli>, String> {
    let mut cli = Cli::default();
    let (mut pos, mut extra) = (Vec::new(), Vec::new());
    let mut config = None;
    let mut it = args.iter().peekable();
    let mut only_pos = false;
    let mut chunks = 0;
    while let Some(a) = it.next() {
        if only_pos || !a.starts_with('-') || a == "-" {
            // (argparse fills data and out from the first run of positionals; later ones are
            // unrecognized)
            if chunks == 1 || pos.is_empty() {
                chunks = 1;
                pos.push(a.clone());
            } else {
                extra.push(a.clone());
            }
            continue;
        }
        if !pos.is_empty() {
            chunks = 2;
        }
        if a == "--" {
            only_pos = true;
            continue;
        }
        if a == "-h" {
            return Ok(None);
        }
        if !a.starts_with("--") {
            extra.push(a.clone());
            continue;
        }
        let (name, inline) = match a.split_once('=') {
            Some((n, v)) => (n, Some(v.to_string())),
            None => (a.as_str(), None),
        };
        let hits: Vec<&str> = if OPTIONS.contains(&name) {
            vec![name]
        } else {
            OPTIONS
                .iter()
                .copied()
                .filter(|o| o.starts_with(name))
                .collect()
        };
        let opt = match hits.as_slice() {
            [one] => *one,
            [] => {
                extra.push(a.clone());
                continue;
            }
            many => {
                return Err(format!(
                    "ambiguous option: {name} could match {}",
                    many.join(", ")
                ))
            }
        };
        if opt == "--help" || opt == "--any-unity-version" {
            if let Some(v) = inline {
                return Err(format!("argument {opt}: ignored explicit argument '{v}'"));
            }
            if opt == "--help" {
                return Ok(None);
            }
            cli.any_unity_version = true;
            continue;
        }
        let value = match inline {
            Some(v) => v,
            None => match it.peek() {
                Some(v) if !v.starts_with('-') || v.as_str() == "-" => it.next().unwrap().clone(),
                _ => return Err(format!("argument {opt}: expected one argument")),
            },
        };
        match opt {
            "--config" => config = Some(value),
            "--steps" => cli.steps = Some(value),
            "--plugin" => cli.plugins.push(value),
            "--only" => cli.only = Some(value),
            _ => cli.sets.push(value),
        }
    }
    let mut missing = Vec::new();
    if pos.is_empty() {
        missing.push("data");
    }
    if config.is_none() {
        missing.push("--config");
    }
    if !missing.is_empty() {
        return Err(format!(
            "the following arguments are required: {}",
            missing.join(", ")
        ));
    }
    extra.extend(pos.iter().skip(2).cloned());
    if !extra.is_empty() {
        return Err(format!("unrecognized arguments: {}", extra.join(" ")));
    }
    cli.data = pos[0].clone();
    cli.out = pos.get(1).cloned();
    cli.config = config.unwrap();
    Ok(Some(cli))
}

/// The config file with --set's overrides (VALUE as JSON, else a string).
pub fn config(cli: &Cli) -> Result<Json> {
    let text = std::fs::read_to_string(&cli.config)?;
    let mut cfg = pyjson::loads(&text).map_err(|e| Error::Value(format!("{}: {e}", cli.config)))?;
    if cfg.setdefault("version", Json::Int(1)).is_none() {
        return Err(Error::Value(format!("{}: not a JSON object", cli.config)));
    }
    for kv in &cli.sets {
        let (k, v) = kv.split_once('=').unwrap_or((kv, ""));
        let (sec, key) = k.split_once('.').unwrap_or((k, ""));
        let v = pyjson::loads(v).unwrap_or_else(|_| Json::Str(v.to_string()));
        let s = cfg.setdefault(sec, Json::dict()).unwrap();
        if !s.set(key, v) {
            return Err(Error::Value(format!(
                "--set {kv}: the config's {sec} is not an object"
            )));
        }
    }
    Ok(cfg)
}

/// The step order: --steps, else the config's "steps", else the built-in list.
fn order(cli: &Cli, cfg: &Json) -> Vec<String> {
    match &cli.steps {
        Some(s) if !s.is_empty() => s.split(',').map(String::from).collect(),
        _ => match cfg.get("steps") {
            Some(Json::List(l)) => l.iter().map(Json::to_str).collect(),
            Some(Json::Str(s)) => s.chars().map(String::from).collect(),
            Some(other) => vec![other.to_str()],
            None => BUILTIN_STEPS.iter().map(|s| s.to_string()).collect(),
        },
    }
}

/// Runs a conversion as `python -m pmunity convert` does.
pub fn run(cli: &Cli) -> Result<()> {
    let data = Path::new(&cli.data);
    let v = crate::version::check_data(data, cli.any_unity_version)?;
    {
        let mut out = std::io::stdout().lock();
        let _ = writeln!(out, "data: Unity {}", v.text);
        let _ = out.flush();
    }
    let cfg = config(cli)?;
    let mut steps = steps::builtin();
    for p in &cli.plugins {
        for (n, f) in steps::plugin(p)? {
            match steps.iter_mut().find(|(k, _)| *k == n) {
                Some(slot) => slot.1 = f,
                None => steps.push((n, f)),
            }
        }
    }
    let order = order(cli, &cfg);
    let out = cli.out.as_deref().unwrap_or(&cli.data);
    let mut job = Job::new(data, Path::new(out), cfg)?;
    let args = Args {
        only: cli.only.clone(),
    };
    for n in &order {
        let Some(f): Option<StepFn> = steps.iter().find(|(k, _)| k == n).map(|(_, f)| *f) else {
            let names: Vec<&str> = steps.iter().map(|(k, _)| k.as_str()).collect();
            return Err(Error::Exit(format!(
                "unknown step {n}; steps: {}",
                names.join(", ")
            )));
        };
        job.log(&format!("== {n}"));
        f(&mut job, &args)?;
    }
    job.log("done");
    Ok(())
}

/// `convert ...` from the command line: the exit status.
pub fn main(args: &[String]) -> u8 {
    let cli = match parse(args) {
        Ok(Some(c)) => c,
        Ok(None) => {
            println!("{USAGE}\n{HELP}");
            return 0;
        }
        Err(msg) => {
            eprintln!("{USAGE}\npmunity convert: error: {msg}");
            return 2;
        }
    };
    match run(&cli) {
        Ok(()) => 0,
        Err(e) => {
            match &e {
                Error::Status(_) => {}
                Error::Exit(m) => eprintln!("{m}"),
                Error::NotPorted(_) => eprintln!("pmu: convert: {e}"),
                _ => eprintln!("pmu: convert: error: {e}"),
            }
            e.status()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(a: &[&str]) -> Vec<String> {
        a.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn arguments_as_argparse() {
        let c = parse(&args(&[
            "data",
            "--conf=c.json",
            "--plugin",
            "x/animclips.py",
            "--set",
            "a.b=1",
            "--set=t.q=fast",
        ]))
        .unwrap()
        .unwrap();
        assert_eq!(c.data, "data");
        assert_eq!(c.config, "c.json");
        assert_eq!(c.plugins, ["x/animclips.py"]);
        assert_eq!(c.sets, ["a.b=1", "t.q=fast"]);
        assert!(parse(&args(&["data"])).unwrap_err().contains("--config"));
        assert!(parse(&args(&["d", "--config", "c", "--s", "x"]))
            .unwrap_err()
            .starts_with("ambiguous option: --s"));
        assert!(parse(&args(&["d", "--config"])).is_err());
        assert!(parse(&args(&["d", "o", "x", "--config", "c"]))
            .unwrap_err()
            .contains("unrecognized arguments: x"));
        let c = parse(&args(&["d", "o", "--config", "c", "--any"]))
            .unwrap()
            .unwrap();
        assert_eq!(c.out.as_deref(), Some("o"));
        assert!(c.any_unity_version);
        assert!(parse(&args(&["-h"])).unwrap().is_none());
    }

    #[test]
    fn sets_as_python() {
        let d = std::env::temp_dir().join(format!("pmu-cli-{}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        let p = d.join("c.json");
        std::fs::write(&p, r#"{"steps": ["shaders"], "textures": {"scale": 0.5}}"#).unwrap();
        let cli = Cli {
            config: p.to_string_lossy().into_owned(),
            sets: vec![
                "textures.quality=fast".into(),
                "textures.scale=1".into(),
                "audio.pcm_budget_mb=null".into(),
                "x=[1, 2]".into(),
            ],
            ..Default::default()
        };
        let cfg = config(&cli).unwrap();
        assert_eq!(
            cfg.dumps(),
            r#"{"steps": ["shaders"], "textures": {"scale": 1, "quality": "fast"}, "version": 1, "audio": {"pcm_budget_mb": null}, "x": {"": [1, 2]}}"#
        );
        std::fs::remove_dir_all(&d).unwrap();
    }
}
