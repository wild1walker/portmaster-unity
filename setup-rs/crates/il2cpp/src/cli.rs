//! The command lines of `pmunity il2cpp` and `pmunity il2cpp fetch` (il2cpp.main), read as
//! argparse reads them: options anywhere, `--opt=value`, unique prefixes, `--` ending the options,
//! `--assembly-patch NAME COMMAND`; argument errors exit 2 with argparse's message.
use crate::build::{self, BuildOpts};
use crate::{device, Error};
use androiddata::pyjson;

struct Opt {
    name: &'static str,
    /// values it takes (0: a flag)
    nargs: usize,
    choices: &'static [&'static str],
}

const fn flag(name: &'static str) -> Opt {
    Opt {
        name,
        nargs: 0,
        choices: &[],
    }
}

const fn val(name: &'static str) -> Opt {
    Opt {
        name,
        nargs: 1,
        choices: &[],
    }
}

const fn choice(name: &'static str, choices: &'static [&'static str]) -> Opt {
    Opt {
        name,
        nargs: 1,
        choices,
    }
}

const ABIS: &[&str] = &["arm64-v8a", "armeabi-v7a"];

struct Parser {
    prog: &'static str,
    usage: &'static str,
    positionals: &'static [&'static str],
    opts: Vec<Opt>,
}

#[derive(Default, Debug)]
struct Parsed {
    pos: Vec<String>,
    opts: Vec<(&'static str, Vec<String>)>,
}

impl Parsed {
    fn last(&self, n: &str) -> Option<&str> {
        self.opts
            .iter()
            .rev()
            .find(|o| o.0 == n)
            .and_then(|o| o.1.first())
            .map(String::as_str)
    }

    fn all(&self, n: &str) -> Vec<&Vec<String>> {
        self.opts
            .iter()
            .filter(|o| o.0 == n)
            .map(|o| &o.1)
            .collect()
    }

    fn has(&self, n: &str) -> bool {
        self.opts.iter().any(|o| o.0 == n)
    }
}

/// Exit status of a parse that stops (help: 0, an error: 2).
type Stop = u8;

impl Parser {
    fn error(&self, m: &str) -> Stop {
        eprintln!("{}\n{}: error: {m}", self.usage, self.prog);
        2
    }

    fn option(&self, tok: &str) -> Result<Option<&Opt>, String> {
        if let Some(o) = self.opts.iter().find(|o| o.name == tok) {
            return Ok(Some(o));
        }
        let hits: Vec<&Opt> = self
            .opts
            .iter()
            .filter(|o| o.name.starts_with(tok))
            .collect();
        match hits.as_slice() {
            [o] => Ok(Some(o)),
            [] => Ok(None),
            many => Err(format!(
                "ambiguous option: {tok} could match {}",
                many.iter().map(|o| o.name).collect::<Vec<_>>().join(", ")
            )),
        }
    }

    fn parse(&self, args: &[String]) -> Result<Parsed, Stop> {
        let mut p = Parsed::default();
        let mut extra: Vec<String> = Vec::new();
        let mut it = args.iter().peekable();
        let mut only_pos = false;
        let is_opt = |s: &str| s.starts_with('-') && s.len() > 1 && s.parse::<f64>().is_err();
        while let Some(tok) = it.next() {
            if only_pos || !is_opt(tok) {
                p.pos.push(tok.clone());
                continue;
            }
            if tok == "--" {
                only_pos = true;
                continue;
            }
            if tok == "-h"
                || (tok.starts_with("--") && tok.len() > 2 && "--help".starts_with(tok.as_str()))
            {
                println!("{}", self.usage);
                return Err(0);
            }
            if !tok.starts_with("--") {
                extra.push(tok.clone());
                continue;
            }
            let (name, inline) = match tok.split_once('=') {
                Some((n, v)) => (n, Some(v.to_string())),
                None => (tok.as_str(), None),
            };
            let o = match self.option(name) {
                Ok(Some(o)) => o,
                Ok(None) => {
                    extra.push(tok.clone());
                    continue;
                }
                Err(m) => return Err(self.error(&m)),
            };
            if o.nargs == 0 {
                if let Some(v) = inline {
                    return Err(self.error(&format!(
                        "argument {}: ignored explicit argument '{v}'",
                        o.name
                    )));
                }
                p.opts.push((o.name, Vec::new()));
                continue;
            }
            let mut vals = Vec::new();
            if let Some(v) = inline {
                if o.nargs != 1 {
                    return Err(self.error(&format!(
                        "argument {}: expected {} arguments",
                        o.name, o.nargs
                    )));
                }
                vals.push(v);
            } else {
                while vals.len() < o.nargs {
                    match it.peek() {
                        Some(v) if !is_opt(v) => {
                            vals.push((*v).clone());
                            it.next();
                        }
                        _ => {
                            return Err(self.error(&if o.nargs == 1 {
                                format!("argument {}: expected one argument", o.name)
                            } else {
                                format!("argument {}: expected {} arguments", o.name, o.nargs)
                            }))
                        }
                    }
                }
            }
            if !o.choices.is_empty() && !o.choices.contains(&vals[0].as_str()) {
                let list: Vec<String> = o.choices.iter().map(|c| format!("'{c}'")).collect();
                return Err(self.error(&format!(
                    "argument {}: invalid choice: '{}' (choose from {})",
                    o.name,
                    vals[0],
                    list.join(", ")
                )));
            }
            p.opts.push((o.name, vals));
        }
        let missing: Vec<&str> = self
            .positionals
            .iter()
            .skip(p.pos.len().min(self.positionals.len()))
            .copied()
            .collect();
        if !missing.is_empty() {
            return Err(self.error(&format!(
                "the following arguments are required: {}",
                missing.join(", ")
            )));
        }
        extra.extend(p.pos.drain(self.positionals.len()..));
        if !extra.is_empty() {
            return Err(self.error(&format!("unrecognized arguments: {}", extra.join(" "))));
        }
        Ok(p)
    }
}

const USAGE: &str = "usage: pmunity il2cpp [-h] [--abi {arm64-v8a,armeabi-v7a}] [--home HOME]
                      [--pkg PKG] [--jobs JOBS] [--image IMAGE] [--keep-cpp]
                      [--strip {low,none}] [--unity VERSION]
                      [--managed-file DLL] [--asmpatch COMMAND]
                      [--assembly-patch NAME COMMAND]
                      [--streaming-assets {obb,both,loose}] [--data-path]
                      [--link-xml FILE] [--dry-run] [--on-device]
                      [--toolchain TOOLCHAIN]
                      data out";

const FETCH_USAGE: &str = "usage: pmunity il2cpp fetch [-h] [--abi {arm64-v8a,armeabi-v7a}]
                            [--home HOME] [--pkg PKG] [--net {2,2subset,4}]
                            [--on-device] [--toolchain TOOLCHAIN]
                            version";

/// The command's end: its message and status.
fn end(r: crate::Result<()>) -> u8 {
    match r {
        Ok(()) => 0,
        Err(Error::Exit(m) | Error::Unsupported(m)) => {
            eprintln!("{m}");
            1
        }
        Err(Error::Fail(m)) => {
            eprintln!("pmu il2cpp: error: {m}");
            1
        }
        Err(Error::NotPorted(m)) => {
            eprintln!("{m}");
            2
        }
    }
}

/// `il2cpp ...`: the exit status.
pub fn main(argv: &[String]) -> u8 {
    if argv.first().map(String::as_str) == Some("fetch") {
        return fetch_main(&argv[1..]);
    }
    let p = Parser {
        prog: "pmunity il2cpp",
        usage: USAGE,
        positionals: &["data", "out"],
        opts: vec![
            choice("--abi", ABIS),
            val("--home"),
            val("--pkg"),
            val("--jobs"),
            val("--image"),
            flag("--keep-cpp"),
            choice("--strip", &["low", "none"]),
            val("--unity"),
            val("--managed-file"),
            val("--asmpatch"),
            Opt {
                name: "--assembly-patch",
                nargs: 2,
                choices: &[],
            },
            choice("--streaming-assets", &["obb", "both", "loose"]),
            flag("--data-path"),
            val("--link-xml"),
            flag("--dry-run"),
            flag("--on-device"),
            val("--toolchain"),
        ],
    };
    let a = match p.parse(argv) {
        Ok(a) => a,
        Err(code) => return code,
    };
    let jobs = match a.last("--jobs") {
        None => None,
        Some(j) => match pkgzip::py::int(j, 10) {
            Some(n) => Some(n.max(0) as usize),
            None => {
                return p.error(&format!(
                    "argument --jobs: invalid int value: {}",
                    pkgzip::py::str_repr(j)
                ))
            }
        },
    };
    let on_device = a.has("--on-device");
    if !on_device {
        return end(Err(Error::NotPorted(
            "pmu il2cpp: only the build on the handheld (--on-device) is ported; the Docker route (a PC's) stays with `python -m pmunity il2cpp`".into(),
        )));
    }
    device::install_ca();
    let mut o = BuildOpts::new(&a.pos[0], &a.pos[1]);
    if let Some(x) = a.last("--abi") {
        o.abi = x.into();
    }
    o.home = a.last("--home").map(str::to_string);
    o.pkg = a.last("--pkg").map(str::to_string);
    o.jobs = jobs;
    o.keep_cpp = a.has("--keep-cpp");
    o.dry = a.has("--dry-run");
    if let Some(x) = a.last("--strip") {
        o.strip = x.into();
    }
    o.unity = a.last("--unity").map(str::to_string);
    o.managed_files = a
        .all("--managed-file")
        .iter()
        .map(|v| v[0].clone())
        .collect();
    o.asmpatch = a.last("--asmpatch").map(str::to_string);
    o.on_device = true;
    o.device_tc = a.last("--toolchain").map(str::to_string);
    o.link_xml = a
        .all("--link-xml")
        .iter()
        .map(|v| crate::util::abspath(&v[0]))
        .collect();
    o.assembly_patches = a
        .all("--assembly-patch")
        .iter()
        .map(|v| (v[0].clone(), v[1].clone()))
        .collect();
    if let Some(x) = a.last("--streaming-assets") {
        o.streaming_assets = x.into();
    }
    o.data_path = a.has("--data-path");
    end(build::build(&o).map(|_| ()))
}

/// `il2cpp fetch <version> ...`: prints what it fetched as JSON.
fn fetch_main(argv: &[String]) -> u8 {
    let p = Parser {
        prog: "pmunity il2cpp fetch",
        usage: FETCH_USAGE,
        positionals: &["version"],
        opts: vec![
            choice("--abi", ABIS),
            val("--home"),
            val("--pkg"),
            choice("--net", &["2", "2subset", "4"]),
            flag("--on-device"),
            val("--toolchain"),
        ],
    };
    let a = match p.parse(argv) {
        Ok(a) => a,
        Err(code) => return code,
    };
    let r = build::fetch_only(
        &a.pos[0],
        a.last("--abi").unwrap_or("arm64-v8a"),
        a.last("--home"),
        a.last("--pkg"),
        a.last("--net").unwrap_or("4"),
        a.has("--on-device"),
        a.last("--toolchain"),
    );
    end(r.map(|j| println!("{}", pyjson::dumps(&j, Some(1)))))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(v: &[&str]) -> Vec<String> {
        v.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn parses() {
        let p = Parser {
            prog: "p",
            usage: "usage: p",
            positionals: &["data", "out"],
            opts: vec![
                val("--asmpatch"),
                Opt {
                    name: "--assembly-patch",
                    nargs: 2,
                    choices: &[],
                },
                flag("--on-device"),
                val("--managed-file"),
                choice("--streaming-assets", &["obb", "both"]),
            ],
        };
        let a = p
            .parse(&s(&[
                "d",
                "--on-dev",
                "o",
                "--assembly-patch",
                "A.dll",
                "cmd {managed} {out}",
                "--managed-file=x.dll",
                "--managed-file",
                "y.dll",
                "--asmpatch",
                "'/s/asmpatch' --only=",
                "--streaming-assets",
                "both",
            ]))
            .unwrap();
        assert_eq!(a.pos, s(&["d", "o"]));
        assert!(a.has("--on-device"));
        assert_eq!(
            a.all("--assembly-patch")[0],
            &s(&["A.dll", "cmd {managed} {out}"])
        );
        assert_eq!(a.all("--managed-file").len(), 2);
        assert_eq!(a.last("--asmpatch"), Some("'/s/asmpatch' --only="));
        assert_eq!(p.parse(&s(&["d"])).unwrap_err(), 2);
        assert_eq!(p.parse(&s(&["d", "o", "--as", "x"])).unwrap_err(), 2);
        assert_eq!(
            p.parse(&s(&["d", "o", "--assembly-patch", "A"]))
                .unwrap_err(),
            2
        );
        assert_eq!(
            p.parse(&s(&["d", "o", "--streaming-assets", "x"]))
                .unwrap_err(),
            2
        );
        assert_eq!(p.parse(&s(&["d", "o", "--nope"])).unwrap_err(), 2);
    }

    #[test]
    fn docker_route_stays_with_the_python() {
        assert_eq!(main(&s(&["/nonexistent", "/tmp/x"])), 2);
    }
}
