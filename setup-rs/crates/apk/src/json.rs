//! What apk.py writes as JSON (android_package.json, the --json report): json.dumps(v, indent=1),
//! keys in insertion order, ensure_ascii. Strings are bytes: UTF-8, and where a string came from
//! Unity data that is not UTF-8, its bytes as Python's surrogateescape gives them (\udcXX).
use std::fmt::Write as _;

#[derive(Debug, Clone, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Int(i128),
    Str(Vec<u8>),
    List(Vec<Json>),
    Obj(Vec<(String, Json)>),
}

impl Json {
    pub fn str(s: &str) -> Json {
        Json::Str(s.as_bytes().to_vec())
    }

    pub fn opt_str(s: Option<&str>) -> Json {
        s.map_or(Json::Null, Json::str)
    }

    pub fn opt_int(i: Option<i128>) -> Json {
        i.map_or(Json::Null, Json::Int)
    }

    pub fn strs<S: AsRef<str>>(v: &[S]) -> Json {
        Json::List(v.iter().map(|s| Json::str(s.as_ref())).collect())
    }

    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Obj(o) => o.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    /// d[key] = v (a key set again keeps its place).
    pub fn set(&mut self, key: &str, v: Json) {
        if let Json::Obj(o) = self {
            match o.iter_mut().find(|(k, _)| k == key) {
                Some(e) => e.1 = v,
                None => o.push((key.to_string(), v)),
            }
        }
    }
}

/// A JSON string of bytes: UTF-8 decoded, a byte that is not escaped as a lone surrogate.
pub fn json_bytes(b: &[u8]) -> String {
    let mut s = String::from('"');
    for chunk in b.utf8_chunks() {
        for c in chunk.valid().chars() {
            match c {
                '"' => s.push_str("\\\""),
                '\\' => s.push_str("\\\\"),
                '\n' => s.push_str("\\n"),
                '\r' => s.push_str("\\r"),
                '\t' => s.push_str("\\t"),
                '\x08' => s.push_str("\\b"),
                '\x0c' => s.push_str("\\f"),
                c if (c as u32) < 0x20 || (c as u32) > 0x7e => {
                    let mut u = [0u16; 2];
                    for w in c.encode_utf16(&mut u) {
                        let _ = write!(s, "\\u{w:04x}");
                    }
                }
                c => s.push(c),
            }
        }
        for &x in chunk.invalid() {
            let _ = write!(s, "\\u{:04x}", 0xdc00 | x as u32);
        }
    }
    s.push('"');
    s
}

/// json.dumps(v, indent=1).
pub fn dumps(v: &Json) -> String {
    let mut s = String::new();
    dump(v, 0, &mut s);
    s
}

fn dump(v: &Json, depth: usize, s: &mut String) {
    let nl = |s: &mut String, d: usize| {
        s.push('\n');
        s.push_str(&" ".repeat(d));
    };
    match v {
        Json::Null => s.push_str("null"),
        Json::Bool(b) => s.push_str(if *b { "true" } else { "false" }),
        Json::Int(i) => {
            let _ = write!(s, "{i}");
        }
        Json::Str(b) => s.push_str(&json_bytes(b)),
        Json::List(l) if l.is_empty() => s.push_str("[]"),
        Json::Obj(o) if o.is_empty() => s.push_str("{}"),
        Json::List(l) => {
            s.push('[');
            for (i, it) in l.iter().enumerate() {
                if i > 0 {
                    s.push(',');
                }
                nl(s, depth + 1);
                dump(it, depth + 1, s);
            }
            nl(s, depth);
            s.push(']');
        }
        Json::Obj(o) => {
            s.push('{');
            for (i, (k, it)) in o.iter().enumerate() {
                if i > 0 {
                    s.push(',');
                }
                nl(s, depth + 1);
                s.push_str(&json_bytes(k.as_bytes()));
                s.push_str(": ");
                dump(it, depth + 1, s);
            }
            nl(s, depth);
            s.push('}');
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn as_json_dumps() {
        let v = Json::Obj(vec![
            ("a".into(), Json::str("é\"")),
            ("b".into(), Json::Str(b"x\xffy".to_vec())),
            (
                "c".into(),
                Json::List(vec![Json::Int(-3), Json::Null, Json::Bool(true)]),
            ),
            ("d".into(), Json::Obj(vec![])),
        ]);
        assert_eq!(
            dumps(&v),
            "{\n \"a\": \"\\u00e9\\\"\",\n \"b\": \"x\\udcffy\",\n \"c\": [\n  -3,\n  null,\n  true\n ],\n \"d\": {}\n}"
        );
    }
}
