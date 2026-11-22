//! What the check's Python does with files and numbers, where its output depends on it: os.walk,
//! os.path's tests, shlex, float formatting.
use port::val::{Dict, Val};
use std::path::Path;

pub fn isfile(p: &str) -> bool {
    std::fs::metadata(p).is_ok_and(|m| m.is_file())
}

pub fn isdir(p: &str) -> bool {
    std::fs::metadata(p).is_ok_and(|m| m.is_dir())
}

pub fn exists(p: &str) -> bool {
    std::fs::metadata(p).is_ok()
}

pub fn join(a: &str, b: &str) -> String {
    pkgzip::py::join(a, b)
}

pub fn joins(parts: &[&str]) -> String {
    let mut s = parts[0].to_string();
    for p in &parts[1..] {
        s = join(&s, p);
    }
    s
}

pub fn basename(p: &str) -> &str {
    pkgzip::py::basename(p)
}

pub fn dirname(p: &str) -> &str {
    pkgzip::py::dirname(p)
}

pub fn abspath(p: &str) -> String {
    port::ps(&port::abspath(Path::new(p)))
}

pub fn relpath(p: &str, start: &str) -> String {
    port::relpath(Path::new(p), Path::new(start))
}

/// os.listdir (the folder's own order; empty when it cannot be read).
pub fn listdir(d: &str) -> Vec<String> {
    std::fs::read_dir(d)
        .map(|rd| rd.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect())
        .unwrap_or_default()
}

pub fn sorted_listdir(d: &str) -> Vec<String> {
    let mut v = listdir(d);
    v.sort();
    v
}

/// os.walk(top): `visit(dir, dirs, files)` for each folder, top down; what visit leaves in dirs is
/// walked (in that order); links to folders are listed and not followed. An entry's kind is the
/// folder listing's, as os.scandir's (only a link is looked up).
pub fn walk(top: &str, visit: &mut dyn FnMut(&str, &mut Vec<String>, &[String])) {
    let Ok(rd) = std::fs::read_dir(top) else { return };
    let (mut dirs, mut files, mut links) = (Vec::new(), Vec::new(), Vec::new());
    for e in rd.flatten() {
        let n = e.file_name().to_string_lossy().into_owned();
        let (dir, link) = match e.file_type() {
            Ok(t) if t.is_symlink() => (isdir(&join(top, &n)), true),
            Ok(t) => (t.is_dir(), false),
            Err(_) => (false, false),
        };
        if dir {
            if link {
                links.push(n.clone());
            }
            dirs.push(n);
        } else {
            files.push(n);
        }
    }
    visit(top, &mut dirs, &files);
    for d in dirs {
        if !links.contains(&d) {
            walk(&join(top, &d), visit);
        }
    }
}

/// The first n bytes of a file (b'' when it cannot be read).
pub fn head(p: &str, n: usize) -> Vec<u8> {
    use std::io::Read;
    let mut b = Vec::new();
    if let Ok(f) = std::fs::File::open(p) {
        let _ = f.take(n as u64).read_to_end(&mut b);
    }
    b
}

pub fn find(h: &[u8], n: &[u8]) -> Option<usize> {
    pkgzip::py::find(h, n)
}

/// round(x, 1)
pub fn round1(x: f64) -> f64 {
    format!("{x:.1}").parse().unwrap_or(x)
}

/// shlex.quote
pub fn shquote(s: &str) -> String {
    port::rx::shquote(s)
}

/// shlex.split (POSIX rules; None for an unclosed quote).
pub fn shsplit(s: &str) -> Option<Vec<String>> {
    let mut out = Vec::new();
    let mut cur: Option<String> = None;
    let mut it = s.chars().peekable();
    while let Some(c) = it.next() {
        match c {
            ' ' | '\t' | '\r' | '\n' => {
                if let Some(w) = cur.take() {
                    out.push(w);
                }
            }
            '\'' => {
                let w = cur.get_or_insert_with(String::new);
                loop {
                    match it.next()? {
                        '\'' => break,
                        x => w.push(x),
                    }
                }
            }
            '"' => {
                let w = cur.get_or_insert_with(String::new);
                loop {
                    match it.next()? {
                        '"' => break,
                        '\\' => match it.next()? {
                            x @ ('"' | '\\') => w.push(x),
                            x => {
                                w.push('\\');
                                w.push(x);
                            }
                        },
                        x => w.push(x),
                    }
                }
            }
            '\\' => cur.get_or_insert_with(String::new).push(it.next()?),
            x => cur.get_or_insert_with(String::new).push(x),
        }
    }
    if let Some(w) = cur {
        out.push(w);
    }
    Some(out)
}

// ---- Val helpers

pub fn s(x: impl AsRef<str>) -> Val {
    Val::Str(x.as_ref().to_string())
}

pub fn strs<S: AsRef<str>>(xs: &[S]) -> Val {
    Val::List(xs.iter().map(|x| Val::Str(x.as_ref().to_string())).collect())
}

pub fn opt_s(x: Option<impl AsRef<str>>) -> Val {
    x.map_or(Val::Null, |x| Val::Str(x.as_ref().to_string()))
}

pub fn dict(items: Vec<(&str, Val)>) -> Dict {
    Dict(items.into_iter().map(|(k, v)| (k.to_string(), v)).collect())
}

pub fn vdict(items: Vec<(&str, Val)>) -> Val {
    Val::Dict(dict(items))
}

/// d[k].append(v) (the list made when missing)
pub fn push(d: &mut Dict, k: &str, v: Val) {
    if !matches!(d.get(k), Some(Val::List(_))) {
        d.set(k, Val::List(Vec::new()));
    }
    if let Some(Val::List(l)) = d.get_mut(k) {
        l.push(v);
    }
}

pub fn str_list(d: &Dict, k: &str) -> Vec<String> {
    d.list(k).iter().map(Val::py_str).collect()
}

pub fn int(d: &Dict, k: &str) -> i64 {
    match d.get(k) {
        Some(Val::Int(i)) => *i,
        Some(Val::Bool(b)) => *b as i64,
        Some(Val::Float(f)) => *f as i64,
        _ => 0,
    }
}

pub fn float(d: &Dict, k: &str) -> f64 {
    match d.get(k) {
        Some(Val::Int(i)) => *i as f64,
        Some(Val::Float(f)) => *f,
        _ => 0.0,
    }
}

/// The value with every dict's keys sorted (json.dumps(sort_keys=True)).
pub fn sort_keys(v: &Val) -> Val {
    match v {
        Val::Dict(d) => {
            let mut items: Vec<(String, Val)> = d.iter().map(|(k, v)| (k.to_string(), sort_keys(v))).collect();
            items.sort_by(|a, b| a.0.cmp(&b.0));
            Val::Dict(Dict(items))
        }
        Val::List(l) => Val::List(l.iter().map(sort_keys).collect()),
        x => x.clone(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shell_words() {
        assert_eq!(shsplit("a 'b c' \"d\\\"e\" f\\ g").unwrap(), ["a", "b c", "d\"e", "f g"]);
        assert_eq!(shsplit("''").unwrap(), [""]);
        assert!(shsplit("'a").is_none());
        assert_eq!(round1(0.25), 0.2);
        assert_eq!(round1(1.26), 1.3);
    }
}
