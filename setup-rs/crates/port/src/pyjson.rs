//! JSON as Python's json module reads and writes it: json.loads (dicts in the file's order, ints
//! and floats apart, NaN and Infinity) and json.dumps(v, indent=n, ensure_ascii=...) byte for
//! byte (", " and ": " without an indent; "," and ": " with one).
use crate::val::{float_repr, Dict, Val};
use pkgzip::{Error, Result};

/// json.dumps(v, indent=indent, ensure_ascii=ensure_ascii)
pub fn dumps(v: &Val, indent: Option<usize>, ensure_ascii: bool) -> String {
    let mut out = String::new();
    write(&mut out, v, indent, ensure_ascii, 0);
    out
}

fn write(out: &mut String, v: &Val, indent: Option<usize>, ascii: bool, level: usize) {
    match v {
        Val::Null => out.push_str("null"),
        Val::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Val::Int(i) => out.push_str(&i.to_string()),
        Val::Float(f) => out.push_str(&if f.is_nan() {
            "NaN".into()
        } else if f.is_infinite() {
            if *f > 0.0 { "Infinity".into() } else { "-Infinity".into() }
        } else {
            float_repr(*f)
        }),
        Val::Str(s) => quote(out, s, ascii),
        Val::List(l) => {
            if l.is_empty() {
                out.push_str("[]");
                return;
            }
            out.push('[');
            for (i, x) in l.iter().enumerate() {
                sep(out, i, indent, level + 1);
                write(out, x, indent, ascii, level + 1);
            }
            close(out, indent, level);
            out.push(']');
        }
        Val::Dict(d) => {
            if d.is_empty() {
                out.push_str("{}");
                return;
            }
            out.push('{');
            for (i, (k, x)) in d.iter().enumerate() {
                sep(out, i, indent, level + 1);
                quote(out, k, ascii);
                out.push_str(": ");
                write(out, x, indent, ascii, level + 1);
            }
            close(out, indent, level);
            out.push('}');
        }
    }
}

fn sep(out: &mut String, i: usize, indent: Option<usize>, level: usize) {
    match indent {
        Some(n) => {
            if i > 0 {
                out.push(',');
            }
            out.push('\n');
            out.push_str(&" ".repeat(n * level));
        }
        None => {
            if i > 0 {
                out.push_str(", ");
            }
        }
    }
}

fn close(out: &mut String, indent: Option<usize>, level: usize) {
    if let Some(n) = indent {
        out.push('\n');
        out.push_str(&" ".repeat(n * level));
    }
}

/// A JSON string: with ensure_ascii every character outside ' '..'~' escaped (\uXXXX, surrogate
/// pairs past the BMP), else only '"', '\\' and the controls.
pub fn quote(out: &mut String, s: &str, ascii: bool) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 || (ascii && !(' '..='~').contains(&c)) => {
                let mut b = [0u16; 2];
                for u in c.encode_utf16(&mut b) {
                    out.push_str(&format!("\\u{:04x}", u));
                }
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

/// json.loads(text)
pub fn loads(text: &str) -> Result<Val> {
    let mut p = P { s: text.as_bytes(), i: 0, text };
    p.ws();
    let v = p.value()?;
    p.ws();
    if p.i != p.s.len() {
        return Err(p.err("Extra data"));
    }
    Ok(v)
}

struct P<'a> {
    s: &'a [u8],
    i: usize,
    text: &'a str,
}

impl P<'_> {
    fn err(&self, what: &str) -> Error {
        let line = self.text[..self.i.min(self.text.len())].matches('\n').count() + 1;
        let col = self.i - self.text[..self.i.min(self.text.len())].rfind('\n').map_or(0, |n| n + 1) + 1;
        Error::Fail(format!("json.decoder.JSONDecodeError: {what}: line {line} column {col} (char {})", self.i))
    }

    fn ws(&mut self) {
        while self.i < self.s.len() && matches!(self.s[self.i], b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
    }

    fn lit(&mut self, w: &str) -> bool {
        if self.s[self.i..].starts_with(w.as_bytes()) {
            self.i += w.len();
            true
        } else {
            false
        }
    }

    fn value(&mut self) -> Result<Val> {
        let Some(&c) = self.s.get(self.i) else {
            return Err(self.err("Expecting value"));
        };
        match c {
            b'{' => {
                self.i += 1;
                let mut d = Dict::new();
                self.ws();
                if self.lit("}") {
                    return Ok(Val::Dict(d));
                }
                loop {
                    self.ws();
                    if self.s.get(self.i) != Some(&b'"') {
                        return Err(self.err("Expecting property name enclosed in double quotes"));
                    }
                    let k = self.string()?;
                    self.ws();
                    if !self.lit(":") {
                        return Err(self.err("Expecting ':' delimiter"));
                    }
                    self.ws();
                    let v = self.value()?;
                    d.set(&k, v);
                    self.ws();
                    if self.lit(",") {
                        continue;
                    }
                    if self.lit("}") {
                        return Ok(Val::Dict(d));
                    }
                    return Err(self.err("Expecting ',' delimiter"));
                }
            }
            b'[' => {
                self.i += 1;
                let mut l = Vec::new();
                self.ws();
                if self.lit("]") {
                    return Ok(Val::List(l));
                }
                loop {
                    self.ws();
                    l.push(self.value()?);
                    self.ws();
                    if self.lit(",") {
                        continue;
                    }
                    if self.lit("]") {
                        return Ok(Val::List(l));
                    }
                    return Err(self.err("Expecting ',' delimiter"));
                }
            }
            b'"' => Ok(Val::Str(self.string()?)),
            _ if self.lit("null") => Ok(Val::Null),
            _ if self.lit("true") => Ok(Val::Bool(true)),
            _ if self.lit("false") => Ok(Val::Bool(false)),
            _ if self.lit("NaN") => Ok(Val::Float(f64::NAN)),
            _ if self.lit("Infinity") => Ok(Val::Float(f64::INFINITY)),
            _ if self.lit("-Infinity") => Ok(Val::Float(f64::NEG_INFINITY)),
            _ => self.number(),
        }
    }

    fn number(&mut self) -> Result<Val> {
        let st = self.i;
        let digits = |p: &mut Self| {
            let a = p.i;
            while p.i < p.s.len() && p.s[p.i].is_ascii_digit() {
                p.i += 1;
            }
            p.i - a
        };
        if self.s.get(self.i) == Some(&b'-') {
            self.i += 1;
        }
        let ds = self.i;
        let n = digits(self);
        if n == 0 || (n > 1 && self.s[ds] == b'0') {
            self.i = st;
            if n > 1 {
                self.i = ds + 1;
                return Err(self.err("Extra data"));
            }
            return Err(self.err("Expecting value"));
        }
        let mut float = false;
        if self.s.get(self.i) == Some(&b'.') && self.s.get(self.i + 1).is_some_and(u8::is_ascii_digit) {
            self.i += 1;
            digits(self);
            float = true;
        }
        if matches!(self.s.get(self.i), Some(b'e' | b'E')) {
            let save = self.i;
            self.i += 1;
            if matches!(self.s.get(self.i), Some(b'+' | b'-')) {
                self.i += 1;
            }
            if digits(self) == 0 {
                self.i = save;
            } else {
                float = true;
            }
        }
        let t = &self.text[st..self.i];
        if !float {
            if let Ok(i) = t.parse::<i64>() {
                return Ok(Val::Int(i));
            }
        }
        Ok(Val::Float(t.parse::<f64>().map_err(|_| self.err("Expecting value"))?))
    }

    fn string(&mut self) -> Result<String> {
        self.i += 1;
        let mut out = String::new();
        loop {
            let st = self.i;
            while self.i < self.s.len() && self.s[self.i] != b'"' && self.s[self.i] != b'\\' && self.s[self.i] >= 0x20 {
                self.i += 1;
            }
            out.push_str(&self.text[st..self.i]);
            match self.s.get(self.i) {
                None => return Err(self.err("Unterminated string starting at")),
                Some(b'"') => {
                    self.i += 1;
                    return Ok(out);
                }
                Some(b'\\') => {
                    let Some(&e) = self.s.get(self.i + 1) else {
                        return Err(self.err("Unterminated string starting at"));
                    };
                    self.i += 2;
                    match e {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{8}'),
                        b'f' => out.push('\u{c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let mut u = self.hex4()?;
                            if (0xd800..0xdc00).contains(&u) && self.s[self.i..].starts_with(b"\\u") {
                                let save = self.i;
                                self.i += 2;
                                let lo = self.hex4()?;
                                if (0xdc00..0xe000).contains(&lo) {
                                    u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                                } else {
                                    self.i = save;
                                }
                            }
                            out.push(char::from_u32(u).unwrap_or('\u{fffd}'));
                        }
                        _ => {
                            self.i -= 2;
                            return Err(self.err("Invalid \\escape"));
                        }
                    }
                }
                Some(_) => return Err(self.err("Invalid control character at")),
            }
        }
    }

    fn hex4(&mut self) -> Result<u32> {
        let h = self.text.get(self.i..self.i + 4).and_then(|h| u32::from_str_radix(h, 16).ok());
        match h {
            Some(u) => {
                self.i += 4;
                Ok(u)
            }
            None => Err(self.err("Invalid \\uXXXX escape")),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let t = r#"{"b": [1, 2.5, "x\u00e9\ud83d\ude00", null, true], "a": {}, "c": [], "d": {"e": -0.0}}"#;
        let v = loads(t).unwrap();
        assert_eq!(dumps(&v, None, true), r#"{"b": [1, 2.5, "x\u00e9\ud83d\ude00", null, true], "a": {}, "c": [], "d": {"e": -0.0}}"#);
        assert_eq!(dumps(&v, Some(1), false), "{\n \"b\": [\n  1,\n  2.5,\n  \"x\u{e9}\u{1f600}\",\n  null,\n  true\n ],\n \"a\": {},\n \"c\": [],\n \"d\": {\n  \"e\": -0.0\n }\n}");
        assert_eq!(dumps(&Val::s("a\u{7f}\u{1}"), None, true), "\"a\\u007f\\u0001\"");
        assert_eq!(dumps(&Val::s("a\u{7f}\u{1}"), None, false), "\"a\u{7f}\\u0001\"");
    }
}
