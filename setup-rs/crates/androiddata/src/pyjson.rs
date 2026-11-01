//! JSON as Python's json module reads and writes it: objects keep their keys' order (dicts), integers
//! of any size, json.dump's layouts (indent=None, 0, 1, ...), ensure_ascii, floats as repr() gives
//! them. The files android-data reads (plugin maps, il2cpp.json, catalogs, android_package.json) and
//! writes (android_package.json, the report, the state file) go through it.
use std::fmt::Write as _;

/// A JSON value, as json.loads gives it and json.dump takes it.
#[derive(Clone, Debug, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Int(i128),
    Float(f64),
    Str(String),
    List(Vec<Json>),
    /// (key, value) in insertion order; a key set again keeps its first place.
    Obj(Vec<(String, Json)>),
}

impl Json {
    pub fn obj() -> Json {
        Json::Obj(Vec::new())
    }

    pub fn str(s: impl Into<String>) -> Json {
        Json::Str(s.into())
    }

    /// d[key] (None when missing or not an object).
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Obj(items) => items.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn get_mut(&mut self, key: &str) -> Option<&mut Json> {
        match self {
            Json::Obj(items) => items.iter_mut().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    /// d[key] = value (a dict keeps a key's first place).
    pub fn set(&mut self, key: &str, v: Json) {
        if let Json::Obj(items) = self {
            match items.iter_mut().find(|(k, _)| k == key) {
                Some(e) => e.1 = v,
                None => items.push((key.to_string(), v)),
            }
        }
    }

    /// del d[key]
    pub fn remove(&mut self, key: &str) -> Option<Json> {
        if let Json::Obj(items) = self {
            if let Some(i) = items.iter().position(|(k, _)| k == key) {
                return Some(items.remove(i).1);
            }
        }
        None
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_int(&self) -> Option<i128> {
        match self {
            Json::Int(i) => Some(*i),
            Json::Bool(b) => Some(*b as i128),
            _ => None,
        }
    }

    pub fn as_list(&self) -> Option<&Vec<Json>> {
        match self {
            Json::List(l) => Some(l),
            _ => None,
        }
    }

    pub fn as_obj(&self) -> Option<&Vec<(String, Json)>> {
        match self {
            Json::Obj(o) => Some(o),
            _ => None,
        }
    }

    /// Python truthiness.
    pub fn truthy(&self) -> bool {
        match self {
            Json::Null => false,
            Json::Bool(b) => *b,
            Json::Int(i) => *i != 0,
            Json::Float(f) => *f != 0.0,
            Json::Str(s) => !s.is_empty(),
            Json::List(l) => !l.is_empty(),
            Json::Obj(o) => !o.is_empty(),
        }
    }

    /// str() of the value as Python prints it in an f-string (a list or dict: its repr).
    pub fn py_str(&self) -> String {
        match self {
            Json::Str(s) => s.clone(),
            v => v.py_repr(),
        }
    }

    /// repr() of the value (Python's literal syntax).
    pub fn py_repr(&self) -> String {
        match self {
            Json::Null => "None".into(),
            Json::Bool(b) => (if *b { "True" } else { "False" }).into(),
            Json::Int(i) => i.to_string(),
            Json::Float(f) => float_repr(*f),
            Json::Str(s) => pkgzip::py::str_repr(s),
            Json::List(l) => format!(
                "[{}]",
                l.iter().map(Json::py_repr).collect::<Vec<_>>().join(", ")
            ),
            Json::Obj(o) => format!(
                "{{{}}}",
                o.iter()
                    .map(|(k, v)| format!("{}: {}", pkgzip::py::str_repr(k), v.py_repr()))
                    .collect::<Vec<_>>()
                    .join(", ")
            ),
        }
    }
}

impl From<&str> for Json {
    fn from(s: &str) -> Json {
        Json::Str(s.to_string())
    }
}

impl From<String> for Json {
    fn from(s: String) -> Json {
        Json::Str(s)
    }
}

impl From<bool> for Json {
    fn from(b: bool) -> Json {
        Json::Bool(b)
    }
}

impl From<i64> for Json {
    fn from(i: i64) -> Json {
        Json::Int(i as i128)
    }
}

impl From<u64> for Json {
    fn from(i: u64) -> Json {
        Json::Int(i as i128)
    }
}

impl From<usize> for Json {
    fn from(i: usize) -> Json {
        Json::Int(i as i128)
    }
}

impl From<u32> for Json {
    fn from(i: u32) -> Json {
        Json::Int(i as i128)
    }
}

impl From<i32> for Json {
    fn from(i: i32) -> Json {
        Json::Int(i as i128)
    }
}

impl<T: Into<Json>> From<Option<T>> for Json {
    fn from(o: Option<T>) -> Json {
        o.map_or(Json::Null, Into::into)
    }
}

impl<T: Into<Json>> From<Vec<T>> for Json {
    fn from(v: Vec<T>) -> Json {
        Json::List(v.into_iter().map(Into::into).collect())
    }
}

/// repr() of a float: the shortest digits that read back the same, fixed notation for exponents
/// -4 to 15, else d.ddde+XX.
pub fn float_repr(f: f64) -> String {
    if f.is_nan() {
        return "nan".into();
    }
    if f.is_infinite() {
        return (if f > 0.0 { "inf" } else { "-inf" }).into();
    }
    if f == 0.0 {
        return (if f.is_sign_negative() { "-0.0" } else { "0.0" }).into();
    }
    let e = format!("{f:e}"); // e.g. -1.2345e-7, 1e20
    let (mant, exp) = e.split_once('e').unwrap();
    let exp: i32 = exp.parse().unwrap();
    let neg = mant.starts_with('-');
    let digits: String = mant.chars().filter(|c| c.is_ascii_digit()).collect();
    let mut s = String::new();
    if neg {
        s.push('-');
    }
    if (-4..16).contains(&exp) {
        if exp < 0 {
            s.push_str("0.");
            s.push_str(&"0".repeat((-exp - 1) as usize));
            s.push_str(&digits);
        } else {
            let point = exp as usize + 1;
            if digits.len() <= point {
                s.push_str(&digits);
                s.push_str(&"0".repeat(point - digits.len()));
                s.push_str(".0");
            } else {
                s.push_str(&digits[..point]);
                s.push('.');
                s.push_str(&digits[point..]);
            }
        }
    } else {
        s.push_str(&digits[..1]);
        if digits.len() > 1 {
            s.push('.');
            s.push_str(&digits[1..]);
        }
        let _ = write!(s, "e{}{:02}", if exp < 0 { '-' } else { '+' }, exp.abs());
    }
    s
}

/// A JSON string as json.dumps writes it (ensure_ascii: everything past ASCII as \uXXXX).
pub fn json_str(t: &str) -> String {
    pkgzip::py::json_str(t)
}

fn float_json(f: f64) -> String {
    if f.is_nan() {
        "NaN".into()
    } else if f.is_infinite() {
        (if f > 0.0 { "Infinity" } else { "-Infinity" }).into()
    } else {
        float_repr(f)
    }
}

/// json.dumps(v, indent=indent): None is the one-line form (", " and ": "), Some(n) one item a
/// line indented n spaces a level (n = 0: no indentation, still one item a line).
pub fn dumps(v: &Json, indent: Option<usize>) -> String {
    let mut s = String::new();
    dump(v, indent, 0, &mut s);
    s
}

fn dump(v: &Json, indent: Option<usize>, depth: usize, s: &mut String) {
    let nl = |s: &mut String, d: usize| {
        if let Some(n) = indent {
            s.push('\n');
            s.push_str(&" ".repeat(n * d));
        }
    };
    let sep = if indent.is_some() { "," } else { ", " };
    match v {
        Json::Null => s.push_str("null"),
        Json::Bool(b) => s.push_str(if *b { "true" } else { "false" }),
        Json::Int(i) => {
            let _ = write!(s, "{i}");
        }
        Json::Float(f) => s.push_str(&float_json(*f)),
        Json::Str(t) => s.push_str(&json_str(t)),
        Json::List(l) if l.is_empty() => s.push_str("[]"),
        Json::Obj(o) if o.is_empty() => s.push_str("{}"),
        Json::List(l) => {
            s.push('[');
            for (i, it) in l.iter().enumerate() {
                if i > 0 {
                    s.push_str(sep);
                }
                nl(s, depth + 1);
                dump(it, indent, depth + 1, s);
            }
            nl(s, depth);
            s.push(']');
        }
        Json::Obj(o) => {
            s.push('{');
            for (i, (k, it)) in o.iter().enumerate() {
                if i > 0 {
                    s.push_str(sep);
                }
                nl(s, depth + 1);
                s.push_str(&json_str(k));
                s.push_str(": ");
                dump(it, indent, depth + 1, s);
            }
            nl(s, depth);
            s.push('}');
        }
    }
}

/// json.loads: Err(message) where Python raises JSONDecodeError (a ValueError).
pub fn loads(text: &str) -> Result<Json, String> {
    let b = text.as_bytes();
    let mut p = Parser { b, i: 0 };
    p.ws();
    let v = p.value()?;
    p.ws();
    if p.i != b.len() {
        return Err(p.err("Extra data"));
    }
    Ok(v)
}

/// json.loads of bytes (UTF-8, a BOM refused as Python does).
pub fn loads_bytes(b: &[u8]) -> Result<Json, String> {
    if b.starts_with(b"\xef\xbb\xbf") {
        return Err(
            "Unexpected UTF-8 BOM (decode using utf-8-sig): line 1 column 1 (char 0)".into(),
        );
    }
    let t = std::str::from_utf8(b).map_err(|e| format!("'utf-8' codec can't decode bytes: {e}"))?;
    loads(t)
}

struct Parser<'a> {
    b: &'a [u8],
    i: usize,
}

impl Parser<'_> {
    fn err(&self, what: &str) -> String {
        let before = &self.b[..self.i.min(self.b.len())];
        let line = before.iter().filter(|&&c| c == b'\n').count() + 1;
        let col = self.i
            - before
                .iter()
                .rposition(|&c| c == b'\n')
                .map_or(0, |p| p + 1)
            + 1;
        let ch = String::from_utf8_lossy(before).chars().count();
        format!("{what}: line {line} column {col} (char {ch})")
    }

    fn ws(&mut self) {
        while self.i < self.b.len() && matches!(self.b[self.i], b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
    }

    fn value(&mut self) -> Result<Json, String> {
        match self.b.get(self.i) {
            Some(b'{') => self.object(),
            Some(b'[') => self.array(),
            Some(b'"') => Ok(Json::Str(self.string()?)),
            Some(b'n') if self.b[self.i..].starts_with(b"null") => {
                self.i += 4;
                Ok(Json::Null)
            }
            Some(b't') if self.b[self.i..].starts_with(b"true") => {
                self.i += 4;
                Ok(Json::Bool(true))
            }
            Some(b'f') if self.b[self.i..].starts_with(b"false") => {
                self.i += 5;
                Ok(Json::Bool(false))
            }
            Some(b'N') if self.b[self.i..].starts_with(b"NaN") => {
                self.i += 3;
                Ok(Json::Float(f64::NAN))
            }
            Some(b'I') if self.b[self.i..].starts_with(b"Infinity") => {
                self.i += 8;
                Ok(Json::Float(f64::INFINITY))
            }
            Some(b'-') if self.b[self.i..].starts_with(b"-Infinity") => {
                self.i += 9;
                Ok(Json::Float(f64::NEG_INFINITY))
            }
            Some(c) if *c == b'-' || c.is_ascii_digit() => self.number(),
            _ => Err(self.err("Expecting value")),
        }
    }

    fn number(&mut self) -> Result<Json, String> {
        let start = self.i;
        if self.b[self.i] == b'-' {
            self.i += 1;
        }
        let digits = |p: &mut Self| {
            let s = p.i;
            while p.i < p.b.len() && p.b[p.i].is_ascii_digit() {
                p.i += 1;
            }
            p.i - s
        };
        if self.b.get(self.i) == Some(&b'0') {
            self.i += 1;
        } else if digits(self) == 0 {
            self.i = start;
            return Err(self.err("Expecting value"));
        }
        let mut float = false;
        if self.b.get(self.i) == Some(&b'.')
            && self.b.get(self.i + 1).is_some_and(u8::is_ascii_digit)
        {
            self.i += 1;
            digits(self);
            float = true;
        }
        if matches!(self.b.get(self.i), Some(b'e' | b'E')) {
            let save = self.i;
            self.i += 1;
            if matches!(self.b.get(self.i), Some(b'+' | b'-')) {
                self.i += 1;
            }
            if digits(self) == 0 {
                self.i = save;
            } else {
                float = true;
            }
        }
        let t = std::str::from_utf8(&self.b[start..self.i]).unwrap();
        if float {
            Ok(Json::Float(
                t.parse().map_err(|_| self.err("Expecting value"))?,
            ))
        } else {
            Ok(Json::Int(
                t.parse().map_err(|_| self.err("Expecting value"))?,
            ))
        }
    }

    fn string(&mut self) -> Result<String, String> {
        let start = self.i;
        self.i += 1;
        let mut out: Vec<u16> = Vec::new();
        let mut s = String::new();
        loop {
            let Some(&c) = self.b.get(self.i) else {
                self.i = start;
                return Err(self.err("Unterminated string starting at"));
            };
            match c {
                b'"' => {
                    self.i += 1;
                    if !out.is_empty() {
                        s.push_str(&String::from_utf16_lossy(&out));
                    }
                    return Ok(s);
                }
                b'\\' => {
                    let e = self.b.get(self.i + 1).copied();
                    let ch = match e {
                        Some(b'"') => Some('"'),
                        Some(b'\\') => Some('\\'),
                        Some(b'/') => Some('/'),
                        Some(b'b') => Some('\x08'),
                        Some(b'f') => Some('\x0c'),
                        Some(b'n') => Some('\n'),
                        Some(b'r') => Some('\r'),
                        Some(b't') => Some('\t'),
                        Some(b'u') => None,
                        _ => {
                            self.i += 1;
                            return Err(self.err("Invalid \\escape"));
                        }
                    };
                    if let Some(ch) = ch {
                        if !out.is_empty() {
                            s.push_str(&String::from_utf16_lossy(&out));
                            out.clear();
                        }
                        s.push(ch);
                        self.i += 2;
                    } else {
                        let hex = self
                            .b
                            .get(self.i + 2..self.i + 6)
                            .and_then(|h| std::str::from_utf8(h).ok())
                            .and_then(|h| u16::from_str_radix(h, 16).ok());
                        let Some(u) = hex else {
                            self.i += 1;
                            return Err(self.err("Invalid \\uXXXX escape"));
                        };
                        out.push(u);
                        self.i += 6;
                    }
                }
                c if c < 0x20 => return Err(self.err("Invalid control character at")),
                _ => {
                    if !out.is_empty() {
                        s.push_str(&String::from_utf16_lossy(&out));
                        out.clear();
                    }
                    let n = utf8_len(c);
                    let piece =
                        std::str::from_utf8(&self.b[self.i..self.i + n]).unwrap_or("\u{fffd}");
                    s.push_str(piece);
                    self.i += n;
                }
            }
        }
    }

    fn array(&mut self) -> Result<Json, String> {
        self.i += 1;
        let mut v = Vec::new();
        self.ws();
        if self.b.get(self.i) == Some(&b']') {
            self.i += 1;
            return Ok(Json::List(v));
        }
        loop {
            self.ws();
            v.push(self.value()?);
            self.ws();
            match self.b.get(self.i) {
                Some(b',') => self.i += 1,
                Some(b']') => {
                    self.i += 1;
                    return Ok(Json::List(v));
                }
                _ => return Err(self.err("Expecting ',' delimiter")),
            }
        }
    }

    fn object(&mut self) -> Result<Json, String> {
        self.i += 1;
        let mut o = Json::obj();
        self.ws();
        if self.b.get(self.i) == Some(&b'}') {
            self.i += 1;
            return Ok(o);
        }
        loop {
            self.ws();
            if self.b.get(self.i) != Some(&b'"') {
                return Err(self.err("Expecting property name enclosed in double quotes"));
            }
            let k = self.string()?;
            self.ws();
            if self.b.get(self.i) != Some(&b':') {
                return Err(self.err("Expecting ':' delimiter"));
            }
            self.i += 1;
            self.ws();
            let v = self.value()?;
            o.set(&k, v);
            self.ws();
            match self.b.get(self.i) {
                Some(b',') => self.i += 1,
                Some(b'}') => {
                    self.i += 1;
                    return Ok(o);
                }
                _ => return Err(self.err("Expecting ',' delimiter")),
            }
        }
    }
}

fn utf8_len(c: u8) -> usize {
    match c {
        0xF0..=0xF7 => 4,
        0xE0..=0xEF => 3,
        0xC0..=0xDF => 2,
        _ => 1,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dumps_as_python() {
        let v = loads(r#"{"b": [1, 2.5, "é"], "a": {}, "c": null, "d": true, "b": []}"#).unwrap();
        assert_eq!(
            dumps(&v, None),
            r#"{"b": [], "a": {}, "c": null, "d": true}"#
        );
        let v = loads(r#"{"x": [1, [2]], "y": "é😀"}"#).unwrap();
        assert_eq!(
            dumps(&v, Some(1)),
            "{\n \"x\": [\n  1,\n  [\n   2\n  ]\n ],\n \"y\": \"\\u00e9\\ud83d\\ude00\"\n}"
        );
        assert_eq!(
            dumps(&v, Some(0)),
            "{\n\"x\": [\n1,\n[\n2\n]\n],\n\"y\": \"\\u00e9\\ud83d\\ude00\"\n}"
        );
        assert_eq!(v.get("y").unwrap().as_str(), Some("é\u{1F600}"));
    }

    #[test]
    fn floats_as_repr() {
        for (f, s) in [
            (0.1, "0.1"),
            (2.0, "2.0"),
            (1e16, "1e+16"),
            (1.5e-5, "1.5e-05"),
            (0.0001, "0.0001"),
            (123456789012345.6, "123456789012345.6"),
            (-3.25, "-3.25"),
        ] {
            assert_eq!(float_repr(f), s);
        }
    }

    #[test]
    fn errors() {
        assert!(loads("{").is_err());
        assert!(loads("[1,]").is_err());
        assert!(loads("1 2").is_err());
        assert_eq!(loads(" 12 ").unwrap(), Json::Int(12));
    }
}
