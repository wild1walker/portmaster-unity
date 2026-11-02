//! Converter plugins without code (edits.py): a JSON file of named steps, each a list of field
//! edits, run here as the setup's Python runs them (docs/plugins.md, "Converter plugins"):
//!
//! ```text
//! {"about": "...", "steps": {"<step>": [{"file": "<glob>", "class": "<class>", "name": "<glob>",
//!                                         "set": {"<field path>": <value>, ...}}, ...]}}
//! ```
//!
//! Every object of the class in the data files whose relative path matches `file` (fnmatch's
//! case-sensitive glob), with `name` only those whose m_Name matches, gets each value set: the
//! path's parts are field names, a decimal number indexes a list (0 / 1 a pair); the value's kind
//! must be the field's (a bool, an integer, a number for a float, a string). Files that are not
//! Unity files (UnityPy's check_file_type: streams, configs) are left out. An edited file is
//! written again and journaled; the others are journaled as they are.
use super::objects;
use crate::error::{Error, Result};
use crate::job::Job;
use crate::pyjson::{self, Json};
use std::sync::Arc;
use unityfs::Value;

/// One edit of a step.
#[derive(Clone, Debug)]
pub struct Edit {
    pub file: String,
    pub class: String,
    pub name: Option<String>,
    pub set: Vec<(String, Json)>,
}

/// A JSON plugin's steps, in its order.
pub type Steps = Vec<(String, Vec<Edit>)>;

fn repr(s: &str) -> String {
    shaderx::py::repr(s)
}

/// The plugin file's steps (edits.load); Error::Exit with the reason for a bad file.
pub fn load(path: &str) -> Result<Steps> {
    let fail = |m: String| Error::Exit(format!("plugin {path}: {m}"));
    let bytes = std::fs::read(path).map_err(|e| {
        fail(format!(
            "[Errno {}] {}: {}",
            e.raw_os_error().unwrap_or(0),
            io_text(&e),
            repr(path)
        ))
    })?;
    let text = String::from_utf8(bytes)
        .map_err(|e| fail(format!("'utf-8' codec can't decode bytes: {e}")))?;
    let cfg = pyjson::loads(&text).map_err(|e| fail(decode_error(&text, &e.to_string())))?;
    check(&cfg).map_err(fail)
}

/// json.JSONDecodeError's text of a parse error ("<what>: char <byte offset>"): its line, column
/// and character position.
fn decode_error(text: &str, e: &str) -> String {
    let Some((what, at)) = e.rsplit_once(": char ") else {
        return e.to_string();
    };
    let Ok(b) = at.parse::<usize>() else {
        return e.to_string();
    };
    let mut b = b.min(text.len());
    while !text.is_char_boundary(b) {
        b -= 1;
    }
    let head = &text[..b];
    let pos = head.chars().count();
    let line = head.matches('\n').count() + 1;
    let col = match head.rfind('\n') {
        Some(i) => head[i + 1..].chars().count() + 1,
        None => pos + 1,
    };
    format!("{what}: line {line} column {col} (char {pos})")
}

/// strerror's text of an I/O error (Python's OSError message).
fn io_text(e: &std::io::Error) -> String {
    let s = e.to_string();
    match s.find(" (os error") {
        Some(i) => s[..i].to_string(),
        None => s,
    }
}

/// edits._check, the same messages in the same order.
fn check(cfg: &Json) -> std::result::Result<Steps, String> {
    let Json::Dict(top) = cfg else {
        return Err("not a JSON object".into());
    };
    for (k, _) in top {
        if k != "about" && k != "steps" {
            return Err(format!("unknown key {} (about, steps)", repr(k)));
        }
    }
    let steps = match cfg.get("steps") {
        Some(Json::Dict(d)) if !d.is_empty() => d,
        _ => return Err("\"steps\" must be an object of named steps".into()),
    };
    let mut out = Vec::new();
    for (name, edits) in steps {
        if name.is_empty() {
            return Err("a step without a name".into());
        }
        let edits = match edits {
            Json::List(l) if !l.is_empty() => l,
            _ => return Err(format!("step {name}: not a list of edits")),
        };
        let mut list = Vec::new();
        for (i, e) in edits.iter().enumerate() {
            let at = format!("step {name}, edit {i}");
            let Json::Dict(fields) = e else {
                return Err(format!("{at}: not an object"));
            };
            for (k, _) in fields {
                if !["file", "class", "name", "set"].contains(&k.as_str()) {
                    return Err(format!(
                        "{at}: unknown key {} (file, class, name, set)",
                        repr(k)
                    ));
                }
            }
            for k in ["file", "class", "name"] {
                if let Some(v) = e.get(k) {
                    if !matches!(v, Json::Str(s) if !s.is_empty()) {
                        return Err(format!("{at}: \"{k}\" must be a string"));
                    }
                }
            }
            for k in ["file", "class", "set"] {
                if e.get(k).is_none() {
                    return Err(format!("{at}: no \"{k}\""));
                }
            }
            let s = |k: &str| match e.get(k) {
                Some(Json::Str(s)) => Some(s.clone()),
                _ => None,
            };
            let class = s("class").unwrap();
            if class == "MonoBehaviour" {
                return Err(format!(
                    "{at}: MonoBehaviour fields need the game's assemblies (a Python plugin)"
                ));
            }
            let set = match e.get("set") {
                Some(Json::Dict(d)) if !d.is_empty() => d,
                _ => {
                    return Err(format!(
                        "{at}: \"set\" must be an object of field paths and values"
                    ))
                }
            };
            for (p, v) in set {
                if p.is_empty() || p.split('.').any(str::is_empty) {
                    return Err(format!("{at}: field path {}", repr(p)));
                }
                if matches!(v, Json::Null | Json::List(_) | Json::Dict(_)) {
                    return Err(format!(
                        "{at}: {p}: a value must be a bool, a number or a string"
                    ));
                }
            }
            list.push(Edit {
                file: s("file").unwrap(),
                class,
                name: s("name"),
                set: set.clone(),
            });
        }
        out.push((name.clone(), list));
    }
    Ok(out)
}

/// fnmatch.fnmatchcase(name, pat): `*` any run (including '/'), `?` one character, `[seq]`,
/// `[!seq]` with ranges; an unclosed `[` is itself.
pub fn fnmatch(name: &str, pat: &str) -> bool {
    enum Tok {
        Star,
        Any,
        Lit(char),
        Class(bool, Vec<(char, char)>),
    }
    let p: Vec<char> = pat.chars().collect();
    let mut toks = Vec::new();
    let mut i = 0;
    while i < p.len() {
        let c = p[i];
        i += 1;
        match c {
            '*' => {
                if !matches!(toks.last(), Some(Tok::Star)) {
                    toks.push(Tok::Star)
                }
            }
            '?' => toks.push(Tok::Any),
            '[' => {
                let mut j = i;
                if j < p.len() && p[j] == '!' {
                    j += 1;
                }
                if j < p.len() && p[j] == ']' {
                    j += 1;
                }
                while j < p.len() && p[j] != ']' {
                    j += 1;
                }
                if j >= p.len() {
                    toks.push(Tok::Lit('['));
                    continue;
                }
                let mut body = &p[i..j];
                i = j + 1;
                let neg = body.first() == Some(&'!');
                if neg {
                    body = &body[1..];
                }
                let mut ranges = Vec::new();
                let mut k = 0;
                while k < body.len() {
                    if k + 2 < body.len() && body[k + 1] == '-' {
                        // (an empty range, z-a, matches nothing: Python drops it)
                        if body[k] <= body[k + 2] {
                            ranges.push((body[k], body[k + 2]));
                        }
                        k += 3;
                    } else {
                        ranges.push((body[k], body[k]));
                        k += 1;
                    }
                }
                toks.push(Tok::Class(neg, ranges));
            }
            c => toks.push(Tok::Lit(c)),
        }
    }
    let s: Vec<char> = name.chars().collect();
    // (backtracking on the last star: enough for globs)
    fn m(t: &[Tok], s: &[char]) -> bool {
        match t.first() {
            None => s.is_empty(),
            Some(Tok::Star) => (0..=s.len()).any(|k| m(&t[1..], &s[k..])),
            Some(tok) => {
                let Some(&c) = s.first() else {
                    return false;
                };
                let ok = match tok {
                    Tok::Any => true,
                    Tok::Lit(l) => *l == c,
                    Tok::Class(neg, r) => r.iter().any(|&(a, b)| a <= c && c <= b) != *neg,
                    Tok::Star => unreachable!(),
                };
                ok && m(&t[1..], &s[1..])
            }
        }
    }
    m(&toks, &s)
}

/// The files under the data folder, '/'-separated and sorted, dot files and folders left out
/// (edits.data_files).
pub fn data_files(root: &std::path::Path) -> Vec<String> {
    fn rec(abs: &std::path::Path, rel: &str, out: &mut Vec<String>) {
        let Ok(rd) = std::fs::read_dir(abs) else {
            return;
        };
        for e in rd.flatten() {
            let Ok(name) = e.file_name().into_string() else {
                continue;
            };
            if name.starts_with('.') {
                continue;
            }
            let p = abs.join(&name);
            let r = if rel.is_empty() {
                name.clone()
            } else {
                format!("{rel}/{name}")
            };
            // (os.walk: a symlink to a folder is listed as a folder, not entered)
            if p.is_dir() {
                if !p.is_symlink() {
                    rec(&p, &r, out);
                }
            } else {
                out.push(r);
            }
        }
    }
    let mut out = Vec::new();
    rec(root, "", &mut out);
    out.sort();
    out
}

/// Whether UnityPy takes a file for a serialized file or a bundle (check_file_type): a bundle's
/// signature, or a serialized file's header.
fn unity_file(path: &std::path::Path) -> Result<bool> {
    use std::io::Read;
    let mut first = Vec::new();
    let mut f = std::fs::File::open(path)?;
    let len = f.metadata()?.len();
    f.by_ref().take(64).read_to_end(&mut first)?;
    if len < 20 {
        return Ok(false);
    }
    let end = first
        .iter()
        .take(20)
        .position(|&c| c == 0)
        .unwrap_or(first.len().min(20));
    let sig = &first[..end];
    if [
        &b"UnityWeb"[..],
        b"UnityRaw",
        b"\xfa\xfa\xfa\xfa\xfa\xfa\xfa\xfa",
        b"UnityFS",
    ]
    .contains(&sig)
    {
        return Ok(true);
    }
    Ok(unityfs::serialized::looks_serialized(&first, len))
}

/// The kind of a value as the Python's edits._kind names it (None: not a single value).
fn kind(v: &Value) -> Option<&'static str> {
    Some(match v {
        Value::Bool(_) => "a bool",
        Value::F32(_) | Value::F64(_) => "a number",
        Value::Str(_) => "a string",
        Value::Bytes(_) | Value::Array(_) | Value::Pair(_) | Value::Struct(_) => return None,
        _ => "an integer",
    })
}

fn json_kind(v: &Json) -> &'static str {
    match v {
        Json::Bool(_) => "a bool",
        Json::Int(_) => "an integer",
        Json::Float(_) => "a number",
        _ => "a string",
    }
}

/// edits._set: the value at `path` replaced by `v`.
fn set(t: &mut Value, path: &str, v: &Json) -> std::result::Result<(), String> {
    let parts: Vec<&str> = path.split('.').collect();
    let mut c = t;
    for (i, k) in parts.iter().enumerate() {
        let next = match c {
            Value::Struct(s) => s
                .fields
                .iter_mut()
                .rev()
                .find(|(n, _)| &**n == *k)
                .map(|(_, x)| x)
                .ok_or_else(|| format!("no field {k}"))?,
            Value::Array(a) => {
                let n = a.len();
                let idx = index(k, n)?;
                &mut a[idx]
            }
            Value::Pair(p) => match index(k, 2)? {
                0 => &mut p.0,
                _ => &mut p.1,
            },
            _ => {
                let at = parts[..i].join(".");
                return Err(format!(
                    "{k}: {} is not a struct or a list",
                    if at.is_empty() { "the object" } else { &at }
                ));
            }
        };
        c = next;
    }
    let Some(have) = kind(c) else {
        return Err("not a single value (a struct, list or byte array)".into());
    };
    let want = json_kind(v);
    if !(want == have || (have == "a number" && want == "an integer")) {
        return Err(format!("{have}, not {want}"));
    }
    if let Value::F32(_) = c {
        // (UnityPy's struct.pack('<f'): OverflowError past float32's range)
        let x = match v {
            Json::Int(i) => *i as f64,
            Json::Float(f) => *f,
            _ => 0.0,
        };
        if x.is_finite() && (x as f32).is_infinite() {
            return Err("float too large to pack with f format".into());
        }
    }
    *c = match v {
        Json::Bool(b) => Value::Bool(*b),
        Json::Int(i) if have == "a number" => Value::F64(*i as f64),
        Json::Int(i) => match (i64::try_from(*i), u64::try_from(*i)) {
            (Ok(x), _) => Value::I64(x),
            (_, Ok(x)) => Value::U64(x),
            _ => return Err(format!("{i}: out of range")),
        },
        Json::Float(f) => Value::F64(*f),
        Json::Str(s) => Value::str(s),
        _ => unreachable!(),
    };
    Ok(())
}

/// A list index part: decimal digits under the list's length.
fn index(k: &str, n: usize) -> std::result::Result<usize, String> {
    let bad = || format!("{k}: not an index of {n} items");
    if k.is_empty() || !k.bytes().all(|b| b.is_ascii_digit()) {
        return Err(bad());
    }
    match k.parse::<usize>() {
        Ok(i) if i < n => Ok(i),
        _ => Err(bad()),
    }
}

/// edits.run: one step of a plugin.
pub fn run(job: &mut Job, plugin: &str, step: &str, edits: &[Edit]) -> Result<()> {
    let files: Vec<String> = data_files(&job.src)
        .into_iter()
        .filter(|rel| edits.iter().any(|e| fnmatch(rel, &e.file)))
        .collect();
    let mut total = 0;
    for rel in &files {
        if job.is_done(step, rel) {
            continue;
        }
        if !unity_file(&job.path_in(rel))? {
            continue;
        }
        let mine: Vec<&Edit> = edits.iter().filter(|e| fnmatch(rel, &e.file)).collect();
        let mut f = match job.open(&job.path_in(rel), Default::default()) {
            Err(Error::Unity(unityfs::Error::Unsupported(m)))
                if m.contains(": expected one serialized file in the bundle, found ") =>
            {
                job.log(&format!(
                    "{step}: {rel}: a bundle of several serialized files (scenes): left out"
                ));
                continue;
            }
            r => r?,
        };
        let mut n = 0;
        for (pid, class) in objects(&f) {
            let es: Vec<&&Edit> = mine.iter().filter(|e| e.class == class).collect();
            if es.is_empty() {
                continue;
            }
            let mut t = f.sf.read_typetree(pid)?;
            let mut hit = false;
            for e in es {
                if let Some(glob) = &e.name {
                    let nm = match t.get("m_Name") {
                        Some(Value::Str(b)) => String::from_utf8_lossy(b).into_owned(),
                        _ => continue,
                    };
                    if !fnmatch(&nm, glob) {
                        continue;
                    }
                }
                for (path, v) in &e.set {
                    // (a float field's float32: a value past its range is the Python's OverflowError)
                    set(&mut t, path, v).map_err(|why| {
                        Error::Exit(format!(
                            "plugin {plugin}: step {step}: {rel}: {class} (path id {pid}): {path}: {why}"
                        ))
                    })?;
                }
                hit = true;
            }
            if hit {
                f.sf.save_typetree(pid, &t)?;
                n += 1;
            }
        }
        if n > 0 {
            job.write_with(rel, |tmp| Ok(f.write_to(tmp, false)?))?;
        }
        drop(f);
        job.mark_done(step, rel)?;
        total += n;
        job.log(&format!("{step}: {rel}: {n} object(s) edited"));
    }
    job.log(&format!(
        "{step}: {} file(s), {total} object(s) edited",
        files.len()
    ));
    Ok(())
}

/// The steps of a JSON plugin as the convert command runs them.
pub fn steps(path: &str) -> Result<Vec<(String, super::StepFn)>> {
    let plugin: Arc<str> = path.into();
    Ok(load(path)?
        .into_iter()
        .map(|(name, edits)| {
            let (p, n) = (plugin.clone(), name.clone());
            let f: super::StepFn =
                Arc::new(move |job: &mut Job, _: &super::Args| run(job, &p, &n, &edits));
            (name, f)
        })
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn globs_as_fnmatchcase() {
        for (n, p, ok) in [
            ("globalgamemanagers", "globalgamemanagers", true),
            ("sharedassets12.assets", "sharedassets*.assets", true),
            ("aa/x/y.bundle", "aa/*.bundle", true),
            ("level1", "level?", true),
            ("level10", "level?", false),
            ("level3", "level[0-2]", false),
            ("level3", "level[!0-2]", true),
            ("a]", "a[]]", true),
            ("[x", "[x", true),
            ("Level1", "level1", false),
            ("x", "[z-a]", false),
        ] {
            assert_eq!(fnmatch(n, p), ok, "{n} {p}");
        }
    }

    #[test]
    fn checks_in_order() {
        let c = |s: &str| check(&pyjson::loads(s).unwrap());
        assert_eq!(c("[]").unwrap_err(), "not a JSON object");
        assert_eq!(
            c(r#"{"x": 1}"#).unwrap_err(),
            "unknown key 'x' (about, steps)"
        );
        assert_eq!(
            c(r#"{"steps": {}}"#).unwrap_err(),
            "\"steps\" must be an object of named steps"
        );
        assert_eq!(
            c(r#"{"steps": {"a": []}}"#).unwrap_err(),
            "step a: not a list of edits"
        );
        assert_eq!(
            c(r#"{"steps": {"a": [{"file": "g", "class": "MonoBehaviour", "set": {"x": 1}}]}}"#)
                .unwrap_err(),
            "step a, edit 0: MonoBehaviour fields need the game's assemblies (a Python plugin)"
        );
        assert_eq!(
            c(r#"{"steps": {"a": [{"file": "g", "class": "C", "set": {"x..y": 1}}]}}"#)
                .unwrap_err(),
            "step a, edit 0: field path 'x..y'"
        );
        let s = c(r#"{"about": "t", "steps": {"a": [{"file": "g*", "class": "C", "name": "n", "set": {"x.0": 1.5}}]}}"#)
            .unwrap();
        assert_eq!(
            (s[0].0.as_str(), s[0].1[0].name.as_deref()),
            ("a", Some("n"))
        );
    }

    #[test]
    fn sets_by_kind() {
        let mut t = Value::Struct(unityfs::Struct {
            fields: vec![
                ("b".into(), Value::Bool(false)),
                ("i".into(), Value::I32(3)),
                ("f".into(), Value::F32(1.0)),
                ("s".into(), Value::str("x")),
                (
                    "l".into(),
                    Value::Array(vec![Value::Pair(Box::new((
                        Value::str("k"),
                        Value::F32(0.5),
                    )))]),
                ),
            ],
        });
        set(&mut t, "b", &Json::Bool(true)).unwrap();
        set(&mut t, "i", &Json::Int(-7)).unwrap();
        set(&mut t, "f", &Json::Int(2)).unwrap();
        set(&mut t, "l.0.1", &Json::Float(0.25)).unwrap();
        assert_eq!(
            t.path(&["l"]).unwrap().as_array().unwrap()[0],
            Value::Pair(Box::new((Value::str("k"), Value::F64(0.25))))
        );
        assert_eq!(
            set(&mut t, "i", &Json::Float(1.5)).unwrap_err(),
            "an integer, not a number"
        );
        assert_eq!(
            set(&mut t, "s", &Json::Bool(true)).unwrap_err(),
            "a string, not a bool"
        );
        assert_eq!(
            set(&mut t, "l", &Json::Int(1)).unwrap_err(),
            "not a single value (a struct, list or byte array)"
        );
        assert_eq!(
            set(&mut t, "l.1", &Json::Int(1)).unwrap_err(),
            "1: not an index of 1 items"
        );
        assert_eq!(
            set(&mut t, "nope", &Json::Int(1)).unwrap_err(),
            "no field nope"
        );
        assert_eq!(
            set(&mut t, "i.x", &Json::Int(1)).unwrap_err(),
            "x: i is not a struct or a list"
        );
    }
}
