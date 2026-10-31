//! What the Python's standard library does with paths, numbers and text, where the port has to do
//! the same: posixpath's normpath/join/basename/dirname, int() of a string, repr() of bytes and
//! strings (in messages), str.strip(), and json.dump(..., indent=1).
use std::fmt::Write as _;

/// posixpath.normpath: collapses `//`, `.` and `a/..` (lexically), keeps one or two leading slashes.
pub fn normpath(path: &str) -> String {
    if path.is_empty() {
        return ".".into();
    }
    let slashes = if path.starts_with("//") && !path.starts_with("///") {
        2
    } else if path.starts_with('/') {
        1
    } else {
        0
    };
    let mut comps: Vec<&str> = Vec::new();
    for c in path.split('/') {
        if c.is_empty() || c == "." {
            continue;
        }
        if c != ".." || (slashes == 0 && comps.is_empty()) || comps.last() == Some(&"..") {
            comps.push(c);
        } else if !comps.is_empty() {
            comps.pop();
        }
    }
    let mut out = "/".repeat(slashes);
    out.push_str(&comps.join("/"));
    if out.is_empty() {
        ".".into()
    } else {
        out
    }
}

/// posixpath.join of two parts: an absolute second part replaces the first.
pub fn join(a: &str, b: &str) -> String {
    if b.starts_with('/') {
        b.to_string()
    } else if a.is_empty() || a.ends_with('/') {
        format!("{a}{b}")
    } else {
        format!("{a}/{b}")
    }
}

/// posixpath.basename: what follows the last slash.
pub fn basename(p: &str) -> &str {
    p.rsplit('/').next().unwrap_or(p)
}

/// posixpath.dirname: what comes before the last slash, without its trailing slashes (unless it
/// is only slashes).
pub fn dirname(p: &str) -> &str {
    let i = p.rfind('/').map_or(0, |i| i + 1);
    let head = &p[..i];
    if !head.is_empty() && head.chars().any(|c| c != '/') {
        head.trim_end_matches('/')
    } else {
        head
    }
}

/// Python's str.isspace() characters (str.strip() and int() remove them).
pub fn is_space(c: char) -> bool {
    matches!(c, '\t'..='\r' | '\x1c'..='\x20' | '\u{85}' | '\u{a0}' | '\u{1680}' | '\u{2000}'..='\u{200a}'
        | '\u{2028}' | '\u{2029}' | '\u{202f}' | '\u{205f}' | '\u{3000}')
}

/// str.strip() with no arguments.
pub fn strip(s: &str) -> &str {
    s.trim_matches(is_space)
}

/// int(text, radix): surrounding whitespace, a sign, the radix's 0x/0o/0b prefix and single
/// underscores between digits allowed, as Python allows them; None where Python raises ValueError.
pub fn int(text: &str, radix: u32) -> Option<i128> {
    let t = strip(text);
    let (neg, t) = match t.as_bytes().first() {
        Some(b'-') => (true, &t[1..]),
        Some(b'+') => (false, &t[1..]),
        _ => (false, t),
    };
    let prefix = match radix {
        16 => ["0x", "0X"],
        8 => ["0o", "0O"],
        2 => ["0b", "0B"],
        _ => ["", ""],
    };
    let (t, after_prefix) = match prefix.iter().find(|p| !p.is_empty() && t.starts_with(**p)) {
        Some(p) => (&t[p.len()..], true),
        None => (t, false),
    };
    let mut v: i128 = 0;
    let mut digits = 0;
    let mut prev_us = false;
    for (i, c) in t.chars().enumerate() {
        if c == '_' {
            // (between digits, or right after the prefix)
            if prev_us || (i == 0 && !after_prefix) {
                return None;
            }
            prev_us = true;
            continue;
        }
        let d = c.to_digit(radix)?;
        v = v.checked_mul(radix as i128)?.checked_add(d as i128)?;
        digits += 1;
        prev_us = false;
    }
    if digits == 0 || prev_us {
        return None;
    }
    Some(if neg { -v } else { v })
}

/// int(text) of a byte string (cpio's header fields): ASCII only.
pub fn int_bytes(b: &[u8], radix: u32) -> Option<i128> {
    if !b.is_ascii() {
        return None;
    }
    int(std::str::from_utf8(b).ok()?, radix)
}

/// repr() of bytes: b'...' with Python's escapes and choice of quotes.
pub fn bytes_repr(b: &[u8]) -> String {
    let q = if b.contains(&b'\'') && !b.contains(&b'"') {
        '"'
    } else {
        '\''
    };
    let mut s = format!("b{q}");
    for &c in b {
        match c {
            b'\\' => s.push_str("\\\\"),
            b'\t' => s.push_str("\\t"),
            b'\n' => s.push_str("\\n"),
            b'\r' => s.push_str("\\r"),
            c if c as char == q => {
                s.push('\\');
                s.push(q)
            }
            0x20..=0x7e => s.push(c as char),
            _ => {
                let _ = write!(s, "\\x{c:02x}");
            }
        }
    }
    s.push(q);
    s
}

/// repr() of a str (printable characters as they are, the rest escaped).
pub fn str_repr(t: &str) -> String {
    let q = if t.contains('\'') && !t.contains('"') {
        '"'
    } else {
        '\''
    };
    let mut s = String::from(q);
    for c in t.chars() {
        match c {
            '\\' => s.push_str("\\\\"),
            '\t' => s.push_str("\\t"),
            '\n' => s.push_str("\\n"),
            '\r' => s.push_str("\\r"),
            c if c == q => {
                s.push('\\');
                s.push(q)
            }
            c if (c as u32) < 0x20 || c as u32 == 0x7f => {
                let _ = write!(s, "\\x{:02x}", c as u32);
            }
            c => s.push(c),
        }
    }
    s.push(q);
    s
}

/// A JSON string as json.dumps writes it (ensure_ascii: everything past ASCII as \uXXXX).
pub fn json_str(t: &str) -> String {
    let mut s = String::from('"');
    for c in t.chars() {
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
    s.push('"');
    s
}

/// A value json.dump writes: what the port's JSON files hold.
pub enum Json {
    Str(String),
    Int(i64),
    List(Vec<Json>),
    Obj(Vec<(String, Json)>),
}

/// json.dump(value, f, indent=1): the keys in order, one item a line, no newline at the end.
pub fn json_dump(v: &Json) -> String {
    let mut s = String::new();
    dump(v, 0, &mut s);
    s
}

fn dump(v: &Json, depth: usize, s: &mut String) {
    let pad = |s: &mut String, d: usize| {
        s.push('\n');
        s.push_str(&" ".repeat(d));
    };
    match v {
        Json::Str(t) => s.push_str(&json_str(t)),
        Json::Int(i) => {
            let _ = write!(s, "{i}");
        }
        Json::List(items) if items.is_empty() => s.push_str("[]"),
        Json::Obj(items) if items.is_empty() => s.push_str("{}"),
        Json::List(items) => {
            s.push('[');
            for (i, it) in items.iter().enumerate() {
                if i > 0 {
                    s.push(',');
                }
                pad(s, depth + 1);
                dump(it, depth + 1, s);
            }
            pad(s, depth);
            s.push(']');
        }
        Json::Obj(items) => {
            s.push('{');
            for (i, (k, it)) in items.iter().enumerate() {
                if i > 0 {
                    s.push(',');
                }
                pad(s, depth + 1);
                s.push_str(&json_str(k));
                s.push_str(": ");
                dump(it, depth + 1, s);
            }
            pad(s, depth);
            s.push('}');
        }
    }
}

/// The byte offset of `needle` in `hay` (bytes.find), or None.
pub fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() {
        return Some(0);
    }
    hay.windows(needle.len()).position(|w| w == needle)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normpath_as_posixpath() {
        for (a, b) in [
            ("", "."),
            (".", "."),
            ("./a/./b/", "a/b"),
            ("a/../..", ".."),
            ("../a/../../b", "../../b"),
            ("/../a", "/a"),
            ("//a//b", "//a/b"),
            ("///a", "/a"),
            ("a/b/..", "a"),
            ("./payload/x", "payload/x"),
        ] {
            assert_eq!(normpath(a), b, "{a}");
        }
    }

    #[test]
    fn join_and_names() {
        assert_eq!(join("a", "b"), "a/b");
        assert_eq!(join("a/", "b"), "a/b");
        assert_eq!(join("", "b"), "b");
        assert_eq!(join("a", "/b"), "/b");
        assert_eq!(join("a", ""), "a/");
        assert_eq!(basename("a/b.pkg"), "b.pkg");
        assert_eq!(basename("b.pkg"), "b.pkg");
        assert_eq!(basename("a/"), "");
        assert_eq!(dirname("a/b/c"), "a/b");
        assert_eq!(dirname("a//c"), "a");
        assert_eq!(dirname("/c"), "/");
        assert_eq!(dirname("c"), "");
    }

    #[test]
    fn int_as_python() {
        assert_eq!(int(" 12 ", 10), Some(12));
        assert_eq!(int("-1_000", 10), Some(-1000));
        assert_eq!(int("0x_ff", 16), Some(255));
        assert_eq!(int("ff", 16), Some(255));
        assert_eq!(int("000755", 8), Some(0o755));
        assert_eq!(int("", 10), None);
        assert_eq!(int("1__0", 10), None);
        assert_eq!(int("_1", 10), None);
        assert_eq!(int("1_", 10), None);
        assert_eq!(int("8", 8), None);
    }

    #[test]
    fn reprs_and_json() {
        assert_eq!(bytes_repr(b"07a\n'\xff"), "b\"07a\\n'\\xff\"");
        assert_eq!(bytes_repr(b"ab"), "b'ab'");
        assert_eq!(str_repr("a'b\""), "'a\\'b\"'");
        let j = Json::Obj(vec![
            ("v".into(), Json::Str("é\u{1F600}".into())),
            ("l".into(), Json::List(vec![Json::Str("a".into())])),
            ("e".into(), Json::List(vec![])),
            ("n".into(), Json::Int(3)),
        ]);
        assert_eq!(
            json_dump(&j),
            "{\n \"v\": \"\\u00e9\\ud83d\\ude00\",\n \"l\": [\n  \"a\"\n ],\n \"e\": [],\n \"n\": 3\n}"
        );
        assert_eq!(strip("\x1c a\u{3000}"), "a");
    }
}
