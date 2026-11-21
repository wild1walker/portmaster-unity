//! TOML read as Python's tomllib reads it (TOML 1.0): tables in the file's order, strings (basic,
//! literal, multi-line), integers, floats, booleans, arrays, inline tables, arrays of tables.
//! Dates and times, which no port.toml has, stop with an error.
use crate::val::{Dict, Val};
use pkgzip::{Error, Result};

/// tomllib.loads(text)
pub fn loads(text: &str) -> Result<Val> {
    let mut p = P { s: text.as_bytes(), i: 0, text };
    let mut root = Dict::new();
    // the table key/value lines go into: a path from the root (an array of tables: its last table)
    let mut cur: Vec<String> = Vec::new();
    loop {
        p.blank();
        if p.i >= p.s.len() {
            break;
        }
        if p.s[p.i] == b'[' {
            let arr = p.s.get(p.i + 1) == Some(&b'[');
            p.i += if arr { 2 } else { 1 };
            p.sp();
            let key = p.key()?;
            p.sp();
            if !p.lit(if arr { "]]" } else { "]" }) {
                return Err(p.err("Expected ']' at the end of a table declaration"));
            }
            p.line_end()?;
            if arr {
                let (last, parent) = key.split_last().unwrap();
                let t = walk(&mut root, parent, &p)?;
                match t.get_mut(last) {
                    None => t.set(last, Val::List(vec![Val::Dict(Dict::new())])),
                    Some(Val::List(l)) => l.push(Val::Dict(Dict::new())),
                    Some(_) => return Err(p.err("Cannot overwrite a value")),
                }
            } else {
                let t = walk(&mut root, &key, &p)?;
                let _ = t;
            }
            cur = key;
            continue;
        }
        let key = p.key()?;
        p.sp();
        if !p.lit("=") {
            return Err(p.err("Expected '=' after a key in a key/value pair"));
        }
        p.sp();
        let v = p.value()?;
        p.line_end()?;
        let t = walk(&mut root, &cur, &p)?;
        let (last, parent) = key.split_last().unwrap();
        let t = walk(t, parent, &p)?;
        if t.contains(last) {
            return Err(p.err("Cannot overwrite a value"));
        }
        t.set(last, v);
    }
    Ok(Val::Dict(root))
}

/// The table at path under t, made where missing (an array of tables: its last one).
fn walk<'a>(t: &'a mut Dict, path: &[String], p: &P) -> Result<&'a mut Dict> {
    let Some((k, rest)) = path.split_first() else {
        return Ok(t);
    };
    if !t.contains(k) {
        t.set(k, Val::Dict(Dict::new()));
    }
    match t.get_mut(k).unwrap() {
        Val::Dict(d) => walk(d, rest, p),
        Val::List(l) => match l.last_mut() {
            Some(Val::Dict(d)) => walk(d, rest, p),
            _ => Err(p.err("Cannot overwrite a value")),
        },
        _ => Err(p.err("Cannot overwrite a value")),
    }
}

struct P<'a> {
    s: &'a [u8],
    i: usize,
    text: &'a str,
}

impl P<'_> {
    fn err(&self, what: &str) -> Error {
        let at = self.i.min(self.text.len());
        let line = self.text[..at].matches('\n').count() + 1;
        let col = at - self.text[..at].rfind('\n').map_or(0, |n| n + 1) + 1;
        Error::Fail(format!("tomllib.TOMLDecodeError: {what} (at line {line}, column {col})"))
    }

    fn peek(&self) -> Option<u8> {
        self.s.get(self.i).copied()
    }

    fn lit(&mut self, w: &str) -> bool {
        if self.s[self.i..].starts_with(w.as_bytes()) {
            self.i += w.len();
            true
        } else {
            false
        }
    }

    /// spaces and tabs
    fn sp(&mut self) {
        while matches!(self.peek(), Some(b' ' | b'\t')) {
            self.i += 1;
        }
    }

    fn comment(&mut self) {
        if self.peek() == Some(b'#') {
            while let Some(c) = self.peek() {
                if c == b'\n' {
                    break;
                }
                self.i += 1;
            }
        }
    }

    /// blank lines and comment lines
    fn blank(&mut self) {
        loop {
            self.sp();
            self.comment();
            if self.lit("\r\n") || self.lit("\n") {
                continue;
            }
            break;
        }
    }

    /// what may follow a value or a header: spaces, a comment, the line's end
    fn line_end(&mut self) -> Result<()> {
        self.sp();
        self.comment();
        if self.i >= self.s.len() || self.lit("\n") || self.lit("\r\n") {
            Ok(())
        } else {
            Err(self.err("Expected newline or end of document after a statement"))
        }
    }

    /// blank space inside an array: whitespace, newlines, comments
    fn array_ws(&mut self) {
        loop {
            self.sp();
            self.comment();
            if self.lit("\n") || self.lit("\r\n") {
                continue;
            }
            break;
        }
    }

    fn key(&mut self) -> Result<Vec<String>> {
        let mut out = vec![self.simple_key()?];
        loop {
            let save = self.i;
            self.sp();
            if self.lit(".") {
                self.sp();
                out.push(self.simple_key()?);
            } else {
                self.i = save;
                return Ok(out);
            }
        }
    }

    fn simple_key(&mut self) -> Result<String> {
        match self.peek() {
            Some(b'"') => self.basic(),
            Some(b'\'') => self.literal(),
            _ => {
                let st = self.i;
                while matches!(self.peek(), Some(c) if c.is_ascii_alphanumeric() || c == b'_' || c == b'-') {
                    self.i += 1;
                }
                if self.i == st {
                    return Err(self.err("Invalid initial character for a key part"));
                }
                Ok(self.text[st..self.i].to_string())
            }
        }
    }

    fn value(&mut self) -> Result<Val> {
        match self.peek() {
            Some(b'"') => {
                if self.s[self.i..].starts_with(b"\"\"\"") {
                    self.ml_basic().map(Val::Str)
                } else {
                    self.basic().map(Val::Str)
                }
            }
            Some(b'\'') => {
                if self.s[self.i..].starts_with(b"'''") {
                    self.ml_literal().map(Val::Str)
                } else {
                    self.literal().map(Val::Str)
                }
            }
            Some(b'[') => {
                self.i += 1;
                let mut l = Vec::new();
                loop {
                    self.array_ws();
                    if self.lit("]") {
                        return Ok(Val::List(l));
                    }
                    l.push(self.value()?);
                    self.array_ws();
                    if self.lit(",") {
                        continue;
                    }
                    if self.lit("]") {
                        return Ok(Val::List(l));
                    }
                    return Err(self.err("Unclosed array"));
                }
            }
            Some(b'{') => {
                self.i += 1;
                let mut d = Dict::new();
                self.sp();
                if self.lit("}") {
                    return Ok(Val::Dict(d));
                }
                loop {
                    self.sp();
                    let key = self.key()?;
                    self.sp();
                    if !self.lit("=") {
                        return Err(self.err("Expected '=' after a key in a key/value pair"));
                    }
                    self.sp();
                    let v = self.value()?;
                    let (last, parent) = key.split_last().unwrap();
                    let t = walk(&mut d, parent, self)?;
                    if t.contains(last) {
                        return Err(self.err("Cannot overwrite a value"));
                    }
                    t.set(last, v);
                    self.sp();
                    if self.lit(",") {
                        continue;
                    }
                    if self.lit("}") {
                        return Ok(Val::Dict(d));
                    }
                    return Err(self.err("Unclosed inline table"));
                }
            }
            _ if self.lit("true") => Ok(Val::Bool(true)),
            _ if self.lit("false") => Ok(Val::Bool(false)),
            _ => self.number(),
        }
    }

    fn number(&mut self) -> Result<Val> {
        let st = self.i;
        while matches!(self.peek(), Some(c) if c.is_ascii_alphanumeric() || matches!(c, b'_' | b'+' | b'-' | b'.' | b':')) {
            self.i += 1;
        }
        let t = &self.text[st..self.i];
        let bad = |p: &mut Self| {
            p.i = st;
            Err(p.err("Invalid value"))
        };
        match t {
            "inf" | "+inf" => return Ok(Val::Float(f64::INFINITY)),
            "-inf" => return Ok(Val::Float(f64::NEG_INFINITY)),
            "nan" | "+nan" | "-nan" => return Ok(Val::Float(f64::NAN)),
            _ => {}
        }
        if t.len() >= 10 && t.as_bytes()[4] == b'-' && t[..4].bytes().all(|c| c.is_ascii_digit()) || t.contains(':') {
            self.i = st;
            return Err(self.err("dates and times are not read by the port packager"));
        }
        // underscores only between digits
        let b = t.as_bytes();
        for (k, &c) in b.iter().enumerate() {
            if c == b'_' && !(k > 0 && k + 1 < b.len() && b[k - 1].is_ascii_hexdigit() && b[k + 1].is_ascii_hexdigit()) {
                return bad(self);
            }
        }
        let clean: String = t.chars().filter(|c| *c != '_').collect();
        for (pre, radix) in [("0x", 16), ("0o", 8), ("0b", 2)] {
            if let Some(d) = clean.strip_prefix(pre) {
                return match i64::from_str_radix(d, radix) {
                    Ok(v) if !d.is_empty() && !d.starts_with(['+', '-']) => Ok(Val::Int(v)),
                    _ => bad(self),
                };
            }
        }
        let digits = clean.trim_start_matches(['+', '-']);
        if digits.is_empty() || !digits.as_bytes()[0].is_ascii_digit() {
            return bad(self);
        }
        if digits.bytes().all(|c| c.is_ascii_digit()) {
            if digits.len() > 1 && digits.starts_with('0') {
                return bad(self);
            }
            return match clean.parse::<i64>() {
                Ok(v) => Ok(Val::Int(v)),
                Err(_) => bad(self),
            };
        }
        // a float: int part, then .digits and/or e[+-]digits
        let ip_end = digits.find(['.', 'e', 'E']).unwrap_or(digits.len());
        let ip = &digits[..ip_end];
        if ip.is_empty() || (ip.len() > 1 && ip.starts_with('0')) || !ip.bytes().all(|c| c.is_ascii_digit()) {
            return bad(self);
        }
        let mut rest = &digits[ip_end..];
        if let Some(r) = rest.strip_prefix('.') {
            let n = r.bytes().take_while(u8::is_ascii_digit).count();
            if n == 0 {
                return bad(self);
            }
            rest = &r[n..];
        }
        if let Some(r) = rest.strip_prefix(['e', 'E']) {
            let r = r.strip_prefix(['+', '-']).unwrap_or(r);
            if r.is_empty() || !r.bytes().all(|c| c.is_ascii_digit()) {
                return bad(self);
            }
            rest = "";
        }
        if !rest.is_empty() {
            return bad(self);
        }
        match clean.parse::<f64>() {
            Ok(v) => Ok(Val::Float(v)),
            Err(_) => bad(self),
        }
    }

    fn escape(&mut self, out: &mut String) -> Result<()> {
        let Some(e) = self.peek() else {
            return Err(self.err("Unterminated string"));
        };
        self.i += 1;
        match e {
            b'b' => out.push('\u{8}'),
            b't' => out.push('\t'),
            b'n' => out.push('\n'),
            b'f' => out.push('\u{c}'),
            b'r' => out.push('\r'),
            b'"' => out.push('"'),
            b'\\' => out.push('\\'),
            b'u' | b'U' => {
                let n = if e == b'u' { 4 } else { 8 };
                let h = self.text.get(self.i..self.i + n).filter(|h| h.bytes().all(|c| c.is_ascii_hexdigit()));
                let c = h.and_then(|h| u32::from_str_radix(h, 16).ok()).and_then(char::from_u32);
                match c {
                    Some(c) => {
                        out.push(c);
                        self.i += n;
                    }
                    None => return Err(self.err("Invalid hex value or not a valid Unicode scalar value")),
                }
            }
            _ => {
                self.i -= 1;
                return Err(self.err("Unescaped '\\' in a string"));
            }
        }
        Ok(())
    }

    fn basic(&mut self) -> Result<String> {
        self.i += 1;
        let mut out = String::new();
        loop {
            let st = self.i;
            while matches!(self.peek(), Some(c) if c != b'"' && c != b'\\' && c != b'\n' && (c >= 0x20 || c == b'\t') && c != 0x7f) {
                self.i += 1;
            }
            out.push_str(&self.text[st..self.i]);
            match self.peek() {
                Some(b'"') => {
                    self.i += 1;
                    return Ok(out);
                }
                Some(b'\\') => {
                    self.i += 1;
                    self.escape(&mut out)?;
                }
                _ => return Err(self.err("Unterminated string or illegal character in a string")),
            }
        }
    }

    fn literal(&mut self) -> Result<String> {
        self.i += 1;
        let st = self.i;
        while matches!(self.peek(), Some(c) if c != b'\'' && c != b'\n' && (c >= 0x20 || c == b'\t') && c != 0x7f) {
            self.i += 1;
        }
        if self.peek() != Some(b'\'') {
            return Err(self.err("Unterminated string or illegal character in a string"));
        }
        let s = self.text[st..self.i].to_string();
        self.i += 1;
        Ok(s)
    }

    fn ml_basic(&mut self) -> Result<String> {
        self.i += 3;
        let _ = self.lit("\n") || self.lit("\r\n");
        let mut out = String::new();
        loop {
            if self.s[self.i..].starts_with(b"\"\"\"") {
                // up to two more quotes belong to the string
                let mut n = 3;
                while self.s.get(self.i + n) == Some(&b'"') && n < 5 {
                    n += 1;
                }
                out.push_str(&"\"".repeat(n - 3));
                self.i += n;
                return Ok(out);
            }
            match self.peek() {
                None => return Err(self.err("Unterminated string")),
                Some(b'\\') => {
                    self.i += 1;
                    // a line-ending backslash: the newline and the whitespace after it go
                    let save = self.i;
                    self.sp();
                    if self.peek() == Some(b'\n') || self.s[self.i..].starts_with(b"\r\n") {
                        while matches!(self.peek(), Some(b' ' | b'\t' | b'\n')) || self.s[self.i..].starts_with(b"\r\n") {
                            self.i += 1;
                        }
                    } else {
                        self.i = save;
                        self.escape(&mut out)?;
                    }
                }
                Some(b'\r') if self.s.get(self.i + 1) == Some(&b'\n') => {
                    out.push('\n');
                    self.i += 2;
                }
                Some(c) if c == b'\n' || c == b'\t' || (c >= 0x20 && c != 0x7f) => {
                    let st = self.i;
                    self.i += 1;
                    while matches!(self.peek(), Some(c) if c >= 0x80) {
                        self.i += 1;
                    }
                    out.push_str(&self.text[st..self.i]);
                }
                _ => return Err(self.err("Illegal character in a string")),
            }
        }
    }

    fn ml_literal(&mut self) -> Result<String> {
        self.i += 3;
        let _ = self.lit("\n") || self.lit("\r\n");
        let mut out = String::new();
        loop {
            if self.s[self.i..].starts_with(b"'''") {
                let mut n = 3;
                while self.s.get(self.i + n) == Some(&b'\'') && n < 5 {
                    n += 1;
                }
                out.push_str(&"'".repeat(n - 3));
                self.i += n;
                return Ok(out);
            }
            match self.peek() {
                None => return Err(self.err("Unterminated string")),
                Some(b'\r') if self.s.get(self.i + 1) == Some(&b'\n') => {
                    out.push('\n');
                    self.i += 2;
                }
                Some(c) if c == b'\n' || c == b'\t' || (c >= 0x20 && c != 0x7f) => {
                    let st = self.i;
                    self.i += 1;
                    while matches!(self.peek(), Some(c) if c >= 0x80) {
                        self.i += 1;
                    }
                    out.push_str(&self.text[st..self.i]);
                }
                _ => return Err(self.err("Illegal character in a string")),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pyjson::dumps;

    #[test]
    fn document() {
        let t = "# c\n[port]\nname = \"g\" # x\ntitle = 'T\\x'\nlist = [1, 2_000, # c\n  -3.5e2, true,\n]\n\
                 store = [{name = \"S\", gameurl = \"u\"}]\n\n[player.sha256]\n\"lib.so\" = \"\\u00e9\"\n\
                 [[questions]]\nid = \"A\"\nopts = [[\"a\", \"b\"]]\n[[questions]]\nid = \"B\"\n\
                 [convert]\nsteps = []\nshaders.from = [\"d3d11\"]\nx = \"\"\"\nab\\\n   c\"\"\"\ny = 0x1f\nz = inf\n";
        let v = loads(t).unwrap();
        assert_eq!(
            dumps(&v, None, false),
            r#"{"port": {"name": "g", "title": "T\\x", "list": [1, 2000, -350.0, true], "store": [{"name": "S", "gameurl": "u"}]}, "player": {"sha256": {"lib.so": "é"}}, "questions": [{"id": "A", "opts": [["a", "b"]]}, {"id": "B"}], "convert": {"steps": [], "shaders": {"from": ["d3d11"]}, "x": "abc", "y": 31, "z": Infinity}}"#
        );
        for bad in ["a = 1\na = 2\n", "a = 01\n", "a = 1979-05-27\n", "a = \"x\n", "a = [1 2]\n", "[a\n", "a = 1_\n"] {
            assert!(loads(bad).is_err(), "{bad}");
        }
    }
}
