//! JSON as Python's json module reads and writes it, and the Python values the job prints: the
//! config (`json.load`, `--set`'s `json.loads`), the journal (`json.dump`: ", " and ": " between
//! items, ASCII only, floats as `repr` gives them), dicts in insertion order, ints apart from
//! floats (2 and 2.0 are different JSON but equal values), and `repr`, `format(x, 'g')`,
//! truthiness and `==` for the log lines and checks that use them.
use std::fmt::Write as _;

/// A JSON value as Python holds it.
#[derive(Clone, Debug)]
pub enum Json {
    Null,
    Bool(bool),
    Int(i128),
    Float(f64),
    Str(String),
    List(Vec<Json>),
    /// A dict: keys in insertion order (a repeated key keeps its first place, its last value).
    Dict(Vec<(String, Json)>),
}

impl Json {
    pub fn dict() -> Json {
        Json::Dict(Vec::new())
    }

    /// `d.get(key)` of a dict (None for a missing key or another value).
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Dict(d) => d.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn get_mut(&mut self, key: &str) -> Option<&mut Json> {
        match self {
            Json::Dict(d) => d.iter_mut().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    /// `d[key] = v`: in place when the key is there, else at the end. False when not a dict.
    pub fn set(&mut self, key: &str, v: Json) -> bool {
        match self {
            Json::Dict(d) => {
                match d.iter_mut().find(|(k, _)| k == key) {
                    Some(slot) => slot.1 = v,
                    None => d.push((key.to_string(), v)),
                }
                true
            }
            _ => false,
        }
    }

    /// `d.setdefault(key, v)`: the value at key, added first when missing (None: not a dict).
    pub fn setdefault(&mut self, key: &str, v: Json) -> Option<&mut Json> {
        let Json::Dict(d) = self else { return None };
        let i = match d.iter().position(|(k, _)| k == key) {
            Some(i) => i,
            None => {
                d.push((key.to_string(), v));
                d.len() - 1
            }
        };
        Some(&mut d[i].1)
    }

    /// `d.pop(key, None)`.
    pub fn pop(&mut self, key: &str) -> Option<Json> {
        let Json::Dict(d) = self else { return None };
        let i = d.iter().position(|(k, _)| k == key)?;
        Some(d.remove(i).1)
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_list(&self) -> Option<&Vec<Json>> {
        match self {
            Json::List(l) => Some(l),
            _ => None,
        }
    }

    /// An int (bools are ints in Python).
    pub fn as_int(&self) -> Option<i128> {
        match *self {
            Json::Int(i) => Some(i),
            Json::Bool(b) => Some(b as i128),
            _ => None,
        }
    }

    /// A number as a float (ints and bools included).
    pub fn as_f64(&self) -> Option<f64> {
        match *self {
            Json::Float(f) => Some(f),
            _ => self.as_int().map(|i| i as f64),
        }
    }

    /// Python's truthiness.
    pub fn truthy(&self) -> bool {
        match self {
            Json::Null => false,
            Json::Bool(b) => *b,
            Json::Int(i) => *i != 0,
            Json::Float(f) => *f != 0.0,
            Json::Str(s) => !s.is_empty(),
            Json::List(l) => !l.is_empty(),
            Json::Dict(d) => !d.is_empty(),
        }
    }

    /// `json.dumps(v)` (json.dump's text).
    pub fn dumps(&self) -> String {
        let mut s = String::new();
        dump(self, &mut s);
        s
    }

    /// `repr(v)` of the Python value.
    pub fn repr(&self) -> String {
        let mut s = String::new();
        repr(self, &mut s);
        s
    }

    /// `str(v)` (an f-string's `{v}`): a str as it is, anything else as repr.
    pub fn to_str(&self) -> String {
        match self {
            Json::Str(s) => s.clone(),
            other => other.repr(),
        }
    }
}

/// Python's `==`: numbers by value (1 == 1.0 == True), dicts whatever their order.
impl PartialEq for Json {
    fn eq(&self, other: &Json) -> bool {
        use Json::*;
        match (self, other) {
            (Null, Null) => true,
            (Str(a), Str(b)) => a == b,
            (List(a), List(b)) => a == b,
            (Dict(a), Dict(b)) => {
                a.len() == b.len() && a.iter().all(|(k, v)| other.get(k).is_some_and(|w| v == w))
            }
            (Float(a), Float(b)) => a == b,
            (Float(f), x) | (x, Float(f)) => match x.as_int() {
                Some(i) => int_eq_float(i, *f),
                None => false,
            },
            (a, b) => match (a.as_int(), b.as_int()) {
                (Some(x), Some(y)) => x == y,
                _ => false,
            },
        }
    }
}

fn int_eq_float(i: i128, f: f64) -> bool {
    f.is_finite() && f.fract() == 0.0 && f.abs() < 1.8e38 && f as i128 == i
}

/// A config or journal that is not JSON (json.JSONDecodeError's message, roughly).
#[derive(Debug)]
pub struct ParseError(pub String);

impl std::fmt::Display for ParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

/// `json.loads(text)`: strict JSON plus Python's NaN, Infinity and -Infinity.
pub fn loads(text: &str) -> Result<Json, ParseError> {
    let mut p = Parser {
        s: text.as_bytes(),
        i: 0,
    };
    p.ws();
    let v = p.value()?;
    p.ws();
    if p.i != p.s.len() {
        return Err(p.err("Extra data"));
    }
    Ok(v)
}

struct Parser<'a> {
    s: &'a [u8],
    i: usize,
}

impl Parser<'_> {
    fn err(&self, what: &str) -> ParseError {
        ParseError(format!("{what}: char {}", self.i))
    }

    fn ws(&mut self) {
        while self.i < self.s.len() && matches!(self.s[self.i], b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
    }

    fn eat(&mut self, lit: &str) -> bool {
        if self.s[self.i..].starts_with(lit.as_bytes()) {
            self.i += lit.len();
            true
        } else {
            false
        }
    }

    fn value(&mut self) -> Result<Json, ParseError> {
        let Some(&c) = self.s.get(self.i) else {
            return Err(self.err("Expecting value"));
        };
        match c {
            b'{' => self.dict(),
            b'[' => self.list(),
            b'"' => Ok(Json::Str(self.string()?)),
            _ if self.eat("null") => Ok(Json::Null),
            _ if self.eat("true") => Ok(Json::Bool(true)),
            _ if self.eat("false") => Ok(Json::Bool(false)),
            _ if self.eat("NaN") => Ok(Json::Float(f64::NAN)),
            _ if self.eat("Infinity") => Ok(Json::Float(f64::INFINITY)),
            _ if self.eat("-Infinity") => Ok(Json::Float(f64::NEG_INFINITY)),
            b'-' | b'0'..=b'9' => self.number(),
            _ => Err(self.err("Expecting value")),
        }
    }

    fn digits(&mut self) -> usize {
        let start = self.i;
        while self.i < self.s.len() && self.s[self.i].is_ascii_digit() {
            self.i += 1;
        }
        self.i - start
    }

    fn number(&mut self) -> Result<Json, ParseError> {
        // -?(0|[1-9]\d*)(\.\d+)?([eE][-+]?\d+)?, as json.scanner's NUMBER_RE
        let start = self.i;
        if self.s[self.i] == b'-' {
            self.i += 1;
        }
        match self.s.get(self.i) {
            Some(b'0') => self.i += 1,
            Some(b'1'..=b'9') => {
                self.digits();
            }
            _ => {
                self.i = start;
                return Err(self.err("Expecting value"));
            }
        }
        let mut float = false;
        if self.s.get(self.i) == Some(&b'.') {
            let at = self.i;
            self.i += 1;
            if self.digits() == 0 {
                self.i = at;
            } else {
                float = true;
            }
        }
        if matches!(self.s.get(self.i), Some(b'e' | b'E')) {
            let at = self.i;
            self.i += 1;
            if matches!(self.s.get(self.i), Some(b'+' | b'-')) {
                self.i += 1;
            }
            if self.digits() == 0 {
                self.i = at;
            } else {
                float = true;
            }
        }
        let t = std::str::from_utf8(&self.s[start..self.i]).unwrap();
        if float {
            t.parse::<f64>()
                .map(Json::Float)
                .map_err(|_| self.err("Bad number"))
        } else {
            t.parse::<i128>()
                .map(Json::Int)
                .map_err(|_| self.err("Integer too large"))
        }
    }

    fn hex4(&mut self) -> Result<u32, ParseError> {
        let h = self
            .s
            .get(self.i..self.i + 4)
            .and_then(|h| std::str::from_utf8(h).ok())
            .and_then(|h| u32::from_str_radix(h, 16).ok())
            .ok_or_else(|| self.err("Invalid \\uXXXX escape"))?;
        self.i += 4;
        Ok(h)
    }

    fn string(&mut self) -> Result<String, ParseError> {
        self.i += 1;
        let mut out = String::new();
        loop {
            let start = self.i;
            while self.i < self.s.len() && !matches!(self.s[self.i], b'"' | b'\\' | 0..=0x1f) {
                self.i += 1;
            }
            out.push_str(
                std::str::from_utf8(&self.s[start..self.i]).map_err(|_| self.err("Not UTF-8"))?,
            );
            match self.s.get(self.i) {
                Some(b'"') => {
                    self.i += 1;
                    return Ok(out);
                }
                Some(b'\\') => {
                    self.i += 1;
                    let c = *self
                        .s
                        .get(self.i)
                        .ok_or_else(|| self.err("Unterminated string"))?;
                    self.i += 1;
                    match c {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\x08'),
                        b'f' => out.push('\x0c'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let mut u = self.hex4()?;
                            if (0xd800..0xdc00).contains(&u) && self.s[self.i..].starts_with(b"\\u")
                            {
                                let at = self.i;
                                self.i += 2;
                                let lo = self.hex4()?;
                                if (0xdc00..0xe000).contains(&lo) {
                                    u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                                } else {
                                    self.i = at;
                                }
                            }
                            // (a lone surrogate, which Python keeps, has no Rust char)
                            out.push(char::from_u32(u).ok_or_else(|| self.err("Lone surrogate"))?);
                        }
                        _ => return Err(self.err("Invalid \\escape")),
                    }
                }
                Some(_) => return Err(self.err("Invalid control character")),
                None => return Err(self.err("Unterminated string")),
            }
        }
    }

    fn list(&mut self) -> Result<Json, ParseError> {
        self.i += 1;
        let mut out = Vec::new();
        self.ws();
        if self.eat("]") {
            return Ok(Json::List(out));
        }
        loop {
            self.ws();
            out.push(self.value()?);
            self.ws();
            if self.eat(",") {
                continue;
            }
            if self.eat("]") {
                return Ok(Json::List(out));
            }
            return Err(self.err("Expecting ',' delimiter"));
        }
    }

    fn dict(&mut self) -> Result<Json, ParseError> {
        self.i += 1;
        let mut out = Json::dict();
        self.ws();
        if self.eat("}") {
            return Ok(out);
        }
        loop {
            self.ws();
            if self.s.get(self.i) != Some(&b'"') {
                return Err(self.err("Expecting property name enclosed in double quotes"));
            }
            let k = self.string()?;
            self.ws();
            if !self.eat(":") {
                return Err(self.err("Expecting ':' delimiter"));
            }
            self.ws();
            let v = self.value()?;
            out.set(&k, v);
            self.ws();
            if self.eat(",") {
                continue;
            }
            if self.eat("}") {
                return Ok(out);
            }
            return Err(self.err("Expecting ',' delimiter"));
        }
    }
}

fn dump_str(s: &str, out: &mut String) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\x08' => out.push_str("\\b"),
            '\x0c' => out.push_str("\\f"),
            ' '..='~' => out.push(c),
            _ => {
                let mut buf = [0u16; 2];
                for u in c.encode_utf16(&mut buf) {
                    let _ = write!(out, "\\u{u:04x}");
                }
            }
        }
    }
    out.push('"');
}

fn dump(v: &Json, out: &mut String) {
    match v {
        Json::Null => out.push_str("null"),
        Json::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Json::Int(i) => {
            let _ = write!(out, "{i}");
        }
        Json::Float(f) => {
            if f.is_nan() {
                out.push_str("NaN")
            } else if f.is_infinite() {
                out.push_str(if *f > 0.0 { "Infinity" } else { "-Infinity" })
            } else {
                out.push_str(&float_repr(*f))
            }
        }
        Json::Str(s) => dump_str(s, out),
        Json::List(l) => {
            out.push('[');
            for (k, x) in l.iter().enumerate() {
                if k > 0 {
                    out.push_str(", ");
                }
                dump(x, out);
            }
            out.push(']');
        }
        Json::Dict(d) => {
            out.push('{');
            for (k, (key, x)) in d.iter().enumerate() {
                if k > 0 {
                    out.push_str(", ");
                }
                dump_str(key, out);
                out.push_str(": ");
                dump(x, out);
            }
            out.push('}');
        }
    }
}

/// `repr(s)` of a str: single quotes unless it holds a ' and no ", Python's escapes.
pub fn str_repr(s: &str) -> String {
    let q = if s.contains('\'') && !s.contains('"') {
        '"'
    } else {
        '\''
    };
    let mut out = String::new();
    out.push(q);
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            _ if c == q => {
                out.push('\\');
                out.push(c)
            }
            '\0'..='\x1f' | '\x7f'..='\u{a0}' | '\u{ad}' => {
                let _ = write!(out, "\\x{:02x}", c as u32);
            }
            _ => out.push(c),
        }
    }
    out.push(q);
    out
}

fn repr(v: &Json, out: &mut String) {
    match v {
        Json::Null => out.push_str("None"),
        Json::Bool(b) => out.push_str(if *b { "True" } else { "False" }),
        Json::Int(i) => {
            let _ = write!(out, "{i}");
        }
        Json::Float(f) => out.push_str(&float_repr(*f)),
        Json::Str(s) => out.push_str(&str_repr(s)),
        Json::List(l) => {
            out.push('[');
            for (k, x) in l.iter().enumerate() {
                if k > 0 {
                    out.push_str(", ");
                }
                repr(x, out);
            }
            out.push(']');
        }
        Json::Dict(d) => {
            out.push('{');
            for (k, (key, x)) in d.iter().enumerate() {
                if k > 0 {
                    out.push_str(", ");
                }
                out.push_str(&str_repr(key));
                out.push_str(": ");
                repr(x, out);
            }
            out.push('}');
        }
    }
}

/// The shortest digits of a finite float and its decimal exponent (d.ddd x 10^exp).
fn shortest(x: f64) -> (bool, String, i32) {
    let s = format!("{:e}", x.abs());
    let (m, e) = s.split_once('e').unwrap();
    (
        x.is_sign_negative(),
        m.replace('.', ""),
        e.parse::<i32>().unwrap(),
    )
}

fn exponent(e: i32) -> String {
    format!("e{}{:02}", if e < 0 { '-' } else { '+' }, e.abs())
}

/// `repr(x)` of a float: the shortest digits that read back as x, positional from 1e-4 up to
/// 1e16 (with ".0" when whole), else with an exponent ("1e+16", "1.5e-05").
pub fn float_repr(x: f64) -> String {
    if x.is_nan() {
        return "nan".into();
    }
    if x.is_infinite() {
        return if x > 0.0 { "inf" } else { "-inf" }.into();
    }
    let (neg, digits, exp) = shortest(x);
    let mut out = String::from(if neg { "-" } else { "" });
    if (-4..16).contains(&exp) {
        if exp >= 0 {
            let int = exp as usize + 1;
            if digits.len() <= int {
                out.push_str(&digits);
                out.push_str(&"0".repeat(int - digits.len()));
                out.push_str(".0");
            } else {
                out.push_str(&digits[..int]);
                out.push('.');
                out.push_str(&digits[int..]);
            }
        } else {
            out.push_str("0.");
            out.push_str(&"0".repeat((-exp - 1) as usize));
            out.push_str(&digits);
        }
    } else {
        out.push_str(&digits[..1]);
        if digits.len() > 1 {
            out.push('.');
            out.push_str(&digits[1..]);
        }
        out.push_str(&exponent(exp));
    }
    out
}

/// `format(x, 'g')`: six significant digits, positional from 1e-4 up to 1e6, trailing zeros
/// dropped.
pub fn format_g(x: f64) -> String {
    if x.is_nan() {
        return "nan".into();
    }
    if x.is_infinite() {
        return if x > 0.0 { "inf" } else { "-inf" }.into();
    }
    if x == 0.0 {
        return if x.is_sign_negative() { "-0" } else { "0" }.into();
    }
    let s = format!("{x:.5e}");
    let (m, e) = s.split_once('e').unwrap();
    let exp: i32 = e.parse().unwrap();
    let trim = |t: &str| -> String {
        if t.contains('.') {
            t.trim_end_matches('0').trim_end_matches('.').to_string()
        } else {
            t.to_string()
        }
    };
    if (-4..6).contains(&exp) {
        trim(&format!("{:.*}", (5 - exp) as usize, x))
    } else {
        format!("{}{}", trim(m), exponent(exp))
    }
}

/// `format(x, '.Nf')`.
pub fn format_f(x: f64, decimals: usize) -> String {
    format!("{x:.decimals$}")
}

/// A JSON value's `format(v, 'g')` (an int is formatted as a float, as Python does).
pub fn json_g(v: &Json) -> String {
    match v {
        Json::Int(_) | Json::Bool(_) | Json::Float(_) => format_g(v.as_f64().unwrap()),
        other => other.repr(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn floats_as_python() {
        for (x, r) in [
            (2.0, "2.0"),
            (0.1, "0.1"),
            (1e16, "1e+16"),
            (1e15, "1000000000000000.0"),
            (0.0001, "0.0001"),
            (0.00001, "1e-05"),
            (-0.0, "-0.0"),
            (1.5e-7, "1.5e-07"),
            (123456789.125, "123456789.125"),
            (1.7976931348623157e308, "1.7976931348623157e+308"),
            (0.375, "0.375"),
        ] {
            assert_eq!(float_repr(x), r, "{x}");
        }
        for (x, g) in [
            (16.0, "16"),
            (2.0, "2"),
            (0.5, "0.5"),
            (1234567.0, "1.23457e+06"),
            (999999.5, "1e+06"),
            (0.0001, "0.0001"),
            (0.00001234, "1.234e-05"),
            (123456.0, "123456"),
            (2.25, "2.25"),
        ] {
            assert_eq!(format_g(x), g, "{x}");
        }
        assert_eq!(format_f(0.25, 1), "0.2");
        assert_eq!(format_f(0.35, 1), "0.3");
        assert_eq!(format_f(2.5, 0), "2");
        assert_eq!(format_f(53.04, 1), "53.0");
    }

    #[test]
    fn json_round_trip_as_python() {
        let v = loads(
            r#" {"b": 1, "a": [2.0, 1e400, -0.5, "x\u00e9\ud83d\ude00\n"], "b": null, "c": {}} "#,
        )
        .unwrap();
        assert_eq!(
            v.dumps(),
            r#"{"b": null, "a": [2.0, Infinity, -0.5, "x\u00e9\ud83d\ude00\n"], "c": {}}"#
        );
        assert_eq!(
            v.repr(),
            "{'b': None, 'a': [2.0, inf, -0.5, 'xé😀\\n'], 'c': {}}"
        );
        assert!(loads("fast").is_err());
        assert!(loads("[1,]").is_err());
        assert_eq!(loads("12").unwrap().dumps(), "12");
        assert_eq!(str_repr("it's"), "\"it's\"");
        assert_eq!(Json::Int(2), Json::Float(2.0));
        assert_eq!(Json::Bool(true), Json::Int(1));
        assert_ne!(Json::Str("1".into()), Json::Int(1));
        let a = loads(r#"{"x": 1, "y": 2.0}"#).unwrap();
        let b = loads(r#"{"y": 2, "x": 1.0}"#).unwrap();
        assert_eq!(a, b);
    }
}
