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
    /// The value's name in the usage (argparse's metavar; default: the name in capitals).
    pub metavar: Option<&'static str>,
}

pub const fn flag(name: &'static str) -> Opt {
    Opt {
        name,
        value: false,
        choices: &[],
        append: false,
        required: false,
        metavar: None,
    }
}

pub const fn val(name: &'static str) -> Opt {
    Opt {
        name,
        value: true,
        choices: &[],
        append: false,
        required: false,
        metavar: None,
    }
}

pub const fn choice(name: &'static str, choices: &'static [&'static str]) -> Opt {
    Opt {
        name,
        value: true,
        choices,
        append: false,
        required: false,
        metavar: None,
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
        self.opts
            .get(name)
            .and_then(|v| v.last())
            .map(String::as_str)
    }

    pub fn has(&self, name: &str) -> bool {
        self.opts.contains_key(name)
    }
}

fn metavar(name: &str) -> String {
    name.trim_start_matches('-')
        .replace('-', "_")
        .to_uppercase()
}

/// shutil.get_terminal_size().columns as argparse's HelpFormatter takes it: $COLUMNS, else stdout's
/// terminal, else 80.
fn columns() -> usize {
    if let Some(c) = std::env::var("COLUMNS")
        .ok()
        .and_then(|v| v.trim().parse::<i64>().ok())
        .filter(|&c| c > 0)
    {
        return c as usize;
    }
    let mut ws: libc::winsize = unsafe { std::mem::zeroed() };
    if unsafe { libc::ioctl(1, libc::TIOCGWINSZ, &mut ws) } == 0 && ws.ws_col > 0 {
        return ws.ws_col as usize;
    }
    80
}

/// argparse's usage parts (the part_regexp's matches): bracketed groups whole, else words.
fn usage_parts(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    let b: Vec<char> = text.chars().collect();
    let mut i = 0;
    while i < b.len() {
        if b[i].is_whitespace() {
            i += 1;
            continue;
        }
        if b[i] == '[' || b[i] == '(' {
            let close = if b[i] == '[' { ']' } else { ')' };
            // the shortest run up to closers followed by a space or the end
            let mut j = i + 1;
            let mut found = None;
            while j < b.len() {
                if b[j] == close {
                    let mut k = j;
                    while k < b.len() && b[k] == close {
                        k += 1;
                    }
                    if k == b.len() || b[k].is_whitespace() {
                        found = Some(k);
                        break;
                    }
                }
                j += 1;
            }
            if let Some(k) = found {
                out.push(b[i..k].iter().collect());
                i = k;
                continue;
            }
        }
        let s = i;
        while i < b.len() && !b[i].is_whitespace() {
            i += 1;
        }
        out.push(b[s..i].iter().collect());
    }
    out
}

impl Parser {
    /// argparse's usage line, wrapped as its HelpFormatter wraps one past the terminal's width.
    pub fn usage(&self) -> String {
        let mut opts = vec!["[-h]".to_string()];
        for o in &self.opts {
            let arg = if !o.value {
                o.name.to_string()
            } else if !o.choices.is_empty() {
                format!("{} {{{}}}", o.name, o.choices.join(","))
            } else {
                format!(
                    "{} {}",
                    o.name,
                    o.metavar.map_or_else(|| metavar(o.name), str::to_string)
                )
            };
            opts.push(if o.required { arg } else { format!("[{arg}]") });
        }
        let pos = self.positionals.join(" ");
        let prefix = "usage: ";
        let usage = [self.prog.to_string(), opts.join(" "), pos.clone()]
            .into_iter()
            .filter(|s| !s.is_empty())
            .collect::<Vec<_>>()
            .join(" ");
        let width = columns().saturating_sub(2);
        if prefix.len() + usage.len() <= width {
            return format!("{prefix}{usage}");
        }
        let opt_parts = usage_parts(&opts.join(" "));
        let pos_parts = usage_parts(&pos);
        let get_lines = |parts: &[String], indent: &str, prefix: Option<&str>| -> Vec<String> {
            let mut lines = Vec::new();
            let mut line: Vec<&str> = Vec::new();
            let mut len = prefix.map_or(indent.len(), str::len) as isize - 1;
            for p in parts {
                if len + 1 + p.len() as isize > width as isize && !line.is_empty() {
                    lines.push(format!("{indent}{}", line.join(" ")));
                    line.clear();
                    len = indent.len() as isize - 1;
                }
                line.push(p);
                len += p.len() as isize + 1;
            }
            if !line.is_empty() {
                lines.push(format!("{indent}{}", line.join(" ")));
            }
            if prefix.is_some() && !lines.is_empty() {
                lines[0] = lines[0][indent.len()..].to_string();
            }
            lines
        };
        let mut lines;
        if (prefix.len() + self.prog.len()) as f64 <= 0.75 * width as f64 {
            let indent = " ".repeat(prefix.len() + self.prog.len() + 1);
            let mut first = vec![self.prog.to_string()];
            if !opt_parts.is_empty() {
                first.extend(opt_parts.iter().cloned());
                lines = get_lines(&first, &indent, Some(prefix));
                lines.extend(get_lines(&pos_parts, &indent, None));
            } else if !pos_parts.is_empty() {
                first.extend(pos_parts.iter().cloned());
                lines = get_lines(&first, &indent, Some(prefix));
            } else {
                lines = first;
            }
        } else {
            let indent = " ".repeat(prefix.len());
            let all: Vec<String> = opt_parts.iter().chain(pos_parts.iter()).cloned().collect();
            lines = get_lines(&all, &indent, None);
            if lines.len() > 1 {
                lines = get_lines(&opt_parts, &indent, None);
                lines.extend(get_lines(&pos_parts, &indent, None));
            }
            lines.insert(0, self.prog.to_string());
        }
        format!("{prefix}{}", lines.join("\n"))
    }

    pub fn error(&self, msg: &str) -> ExitCode {
        eprintln!("{}\n{}: error: {msg}", self.usage(), self.prog);
        ExitCode::from(2)
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

    /// The arguments parsed, or the exit status to stop with (help: 0, errors: 2).
    pub fn parse(&self, args: &[String]) -> Result<Parsed, ExitCode> {
        self.parse_with(args, false)
    }

    /// parse() for a parser whose one positional takes one or more values (nargs='+'): every
    /// positional goes to `pos`.
    pub fn parse_many(&self, args: &[String]) -> Result<Parsed, ExitCode> {
        self.parse_with(args, true)
    }

    fn parse_with(&self, args: &[String], many: bool) -> Result<Parsed, ExitCode> {
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
            if tok == "-h"
                || (tok.starts_with("--") && "--help".starts_with(tok.as_str()) && tok.len() > 2)
            {
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
                        return Err(
                            self.error(&format!("argument {}: expected one argument", o.name))
                        )
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
            .map(|s| s.split(' ').next().unwrap_or(s).to_string())
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
        if !many {
            extra.extend(p.pos.drain(self.positionals.len()..));
        }
        if !extra.is_empty() {
            return Err(self.error(&format!("unrecognized arguments: {}", extra.join(" "))));
        }
        Ok(p)
    }
}
