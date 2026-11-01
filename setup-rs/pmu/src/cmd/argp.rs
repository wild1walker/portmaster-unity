//! Command lines read as the Python's argparse reads them, for the commands whose arguments are
//! a few positionals and options: options anywhere, `--opt=value`, unique prefixes of option
//! names, `--` ending the options; argument errors print argparse's usage and message and exit 2.
use std::collections::BTreeMap;
use std::process::ExitCode;

/// An option: its name (`--out`), whether it takes a value, the values it allows (empty: any),
/// whether it repeats (append) and whether it is required.
pub struct Opt {
    pub name: &'static str,
    pub value: bool,
    pub choices: &'static [&'static str],
    pub append: bool,
    pub required: bool,
}

pub const fn flag(name: &'static str) -> Opt {
    Opt {
        name,
        value: false,
        choices: &[],
        append: false,
        required: false,
    }
}

pub const fn val(name: &'static str) -> Opt {
    Opt {
        name,
        value: true,
        choices: &[],
        append: false,
        required: false,
    }
}

pub const fn choice(name: &'static str, choices: &'static [&'static str]) -> Opt {
    Opt {
        name,
        value: true,
        choices,
        append: false,
        required: false,
    }
}

/// A parser: the program name argparse prints (`pmunity unpack`), the positionals, the options.
pub struct Parser {
    pub prog: &'static str,
    pub positionals: &'static [&'static str],
    pub opts: Vec<Opt>,
}

/// What was given: the positionals in order, each option's values (a flag: one empty value).
#[derive(Debug, Default)]
pub struct Parsed {
    pub pos: Vec<String>,
    pub opts: BTreeMap<&'static str, Vec<String>>,
}

impl Parsed {
    pub fn get(&self, name: &str) -> Option<&str> {
        self.opts.get(name).and_then(|v| v.last()).map(String::as_str)
    }

    pub fn all(&self, name: &str) -> Vec<String> {
        self.opts.get(name).cloned().unwrap_or_default()
    }

    pub fn has(&self, name: &str) -> bool {
        self.opts.contains_key(name)
    }
}

fn metavar(name: &str) -> String {
    name.trim_start_matches('-').replace('-', "_").to_uppercase()
}

impl Parser {
    pub fn usage(&self) -> String {
        let mut parts = vec![format!("usage: {} [-h]", self.prog)];
        for o in &self.opts {
            let arg = if !o.value {
                o.name.to_string()
            } else if !o.choices.is_empty() {
                format!("{} {{{}}}", o.name, o.choices.join(","))
            } else {
                format!("{} {}", o.name, metavar(o.name))
            };
            parts.push(if o.required { arg } else { format!("[{arg}]") });
        }
        parts.extend(self.positionals.iter().map(|p| p.to_string()));
        parts.join(" ")
    }

    pub fn error(&self, msg: &str) -> ExitCode {
        eprintln!("{}\n{}: error: {msg}", self.usage(), self.prog);
        ExitCode::from(2)
    }

    fn option(&self, tok: &str) -> Result<Option<&Opt>, String> {
        if let Some(o) = self.opts.iter().find(|o| o.name == tok) {
            return Ok(Some(o));
        }
        let hits: Vec<&Opt> = self.opts.iter().filter(|o| o.name.starts_with(tok)).collect();
        match hits.as_slice() {
            [o] => Ok(Some(o)),
            [] => Ok(None),
            many => Err(format!(
                "ambiguous option: {tok} could match {}",
                many.iter().map(|o| o.name).collect::<Vec<_>>().join(", ")
            )),
        }
    }

    /// The arguments parsed, or the exit status to stop with (help: 0, errors: 2).
    pub fn parse(&self, args: &[String]) -> Result<Parsed, ExitCode> {
        let mut p = Parsed::default();
        let mut extra = Vec::new();
        let mut it = args.iter();
        let mut only_pos = false;
        while let Some(tok) = it.next() {
            if only_pos || !tok.starts_with('-') || tok == "-" {
                p.pos.push(tok.clone());
                continue;
            }
            if tok == "--" {
                only_pos = true;
                continue;
            }
            if tok == "-h" || (tok.starts_with("--") && "--help".starts_with(tok.as_str()) && tok.len() > 2) {
                println!("{}", self.usage());
                return Err(ExitCode::SUCCESS);
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
            if !o.value {
                if let Some(v) = inline {
                    return Err(self.error(&format!(
                        "argument {}: ignored explicit argument '{v}'",
                        o.name
                    )));
                }
                p.opts.entry(o.name).or_default().push(String::new());
                continue;
            }
            let v = match inline {
                Some(v) => v,
                None => match it.next() {
                    Some(v) if !(v.starts_with('-') && v.len() > 1) => v.clone(),
                    _ => {
                        return Err(self.error(&format!("argument {}: expected one argument", o.name)))
                    }
                },
            };
            if !o.choices.is_empty() && !o.choices.contains(&v.as_str()) {
                let list: Vec<String> = o.choices.iter().map(|c| format!("'{c}'")).collect();
                return Err(self.error(&format!(
                    "argument {}: invalid choice: '{v}' (choose from {})",
                    o.name,
                    list.join(", ")
                )));
            }
            let e = p.opts.entry(o.name).or_default();
            if !o.append {
                e.clear();
            }
            e.push(v);
        }
        let mut missing: Vec<String> = self
            .positionals
            .iter()
            .skip(p.pos.len().min(self.positionals.len()))
            .map(|s| s.to_string())
            .collect();
        for o in &self.opts {
            if o.required && !p.opts.contains_key(o.name) {
                missing.push(o.name.to_string());
            }
        }
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
