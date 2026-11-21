//! The values port.py works with: what tomllib and json.loads return (dicts in insertion order,
//! lists, strings, ints, floats, bools, None), and what Python does with them where the packager
//! depends on it: truth, ==, str(), repr(), int().

/// A Python value as tomllib or json.loads returns it.
#[derive(Clone, Debug)]
pub enum Val {
    Null,
    Bool(bool),
    Int(i64),
    Float(f64),
    Str(String),
    List(Vec<Val>),
    Dict(Dict),
}

/// A dict: its keys in insertion order (setting a key it has keeps the key's place, a new key goes
/// last, popping one removes it), as Python's.
#[derive(Clone, Debug, Default)]
pub struct Dict(pub Vec<(String, Val)>);

impl Dict {
    pub fn new() -> Dict {
        Dict(Vec::new())
    }

    pub fn get(&self, k: &str) -> Option<&Val> {
        self.0.iter().find(|(x, _)| x == k).map(|(_, v)| v)
    }

    pub fn get_mut(&mut self, k: &str) -> Option<&mut Val> {
        self.0.iter_mut().find(|(x, _)| x == k).map(|(_, v)| v)
    }

    pub fn contains(&self, k: &str) -> bool {
        self.0.iter().any(|(x, _)| x == k)
    }

    /// d[k] = v
    pub fn set(&mut self, k: &str, v: Val) {
        match self.get_mut(k) {
            Some(x) => *x = v,
            None => self.0.push((k.to_string(), v)),
        }
    }

    /// d.pop(k, None)
    pub fn pop(&mut self, k: &str) -> Option<Val> {
        let i = self.0.iter().position(|(x, _)| x == k)?;
        Some(self.0.remove(i).1)
    }

    pub fn keys(&self) -> impl Iterator<Item = &str> {
        self.0.iter().map(|(k, _)| k.as_str())
    }

    pub fn iter(&self) -> impl Iterator<Item = (&str, &Val)> {
        self.0.iter().map(|(k, v)| (k.as_str(), v))
    }

    pub fn len(&self) -> usize {
        self.0.len()
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    /// The table at k (a new empty one when k is missing or not a table: d.setdefault(k, {})).
    pub fn table(&mut self, k: &str) -> &mut Dict {
        if !matches!(self.get(k), Some(Val::Dict(_))) {
            self.set(k, Val::Dict(Dict::new()));
        }
        match self.get_mut(k) {
            Some(Val::Dict(d)) => d,
            _ => unreachable!(),
        }
    }

    /// d.get(k) as a string ('' when missing or not a string).
    pub fn str(&self, k: &str) -> &str {
        match self.get(k) {
            Some(Val::Str(s)) => s,
            _ => "",
        }
    }

    /// bool(d.get(k))
    pub fn truthy(&self, k: &str) -> bool {
        self.get(k).is_some_and(Val::truthy)
    }

    /// d.get(k) or {} as a dict.
    pub fn dict(&self, k: &str) -> &Dict {
        static EMPTY: Dict = Dict(Vec::new());
        match self.get(k) {
            Some(Val::Dict(d)) => d,
            _ => &EMPTY,
        }
    }

    /// d.get(k) or [] as a list.
    pub fn list(&self, k: &str) -> &[Val] {
        match self.get(k) {
            Some(Val::List(l)) => l,
            _ => &[],
        }
    }
}

impl Val {
    pub fn s(x: &str) -> Val {
        Val::Str(x.to_string())
    }

    pub fn strs<S: AsRef<str>>(xs: &[S]) -> Val {
        Val::List(xs.iter().map(|x| Val::s(x.as_ref())).collect())
    }

    /// bool(v)
    pub fn truthy(&self) -> bool {
        match self {
            Val::Null => false,
            Val::Bool(b) => *b,
            Val::Int(i) => *i != 0,
            Val::Float(f) => *f != 0.0,
            Val::Str(s) => !s.is_empty(),
            Val::List(l) => !l.is_empty(),
            Val::Dict(d) => !d.is_empty(),
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Val::Str(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_dict(&self) -> Option<&Dict> {
        match self {
            Val::Dict(d) => Some(d),
            _ => None,
        }
    }

    pub fn as_dict_mut(&mut self) -> Option<&mut Dict> {
        match self {
            Val::Dict(d) => Some(d),
            _ => None,
        }
    }

    pub fn as_list(&self) -> Option<&[Val]> {
        match self {
            Val::List(l) => Some(l),
            _ => None,
        }
    }

    /// The value as a number, for ==: bools are ints in Python.
    fn num(&self) -> Option<f64> {
        match self {
            Val::Bool(b) => Some(*b as i64 as f64),
            Val::Int(i) => Some(*i as f64),
            Val::Float(f) => Some(*f),
            _ => None,
        }
    }

    /// v == w, as Python compares (1 == 1.0 == True; dicts by their items, in any order).
    pub fn eq(&self, other: &Val) -> bool {
        match (self, other) {
            (Val::Null, Val::Null) => true,
            (Val::Str(a), Val::Str(b)) => a == b,
            (Val::Int(a), Val::Int(b)) => a == b,
            (Val::List(a), Val::List(b)) => a.len() == b.len() && a.iter().zip(b).all(|(x, y)| x.eq(y)),
            (Val::Dict(a), Val::Dict(b)) => {
                a.len() == b.len() && a.iter().all(|(k, v)| b.get(k).is_some_and(|w| v.eq(w)))
            }
            _ => match (self.num(), other.num()) {
                (Some(a), Some(b)) => a == b,
                _ => false,
            },
        }
    }

    /// str(v)
    pub fn py_str(&self) -> String {
        match self {
            Val::Str(s) => s.clone(),
            _ => self.repr(),
        }
    }

    /// repr(v)
    pub fn repr(&self) -> String {
        match self {
            Val::Null => "None".into(),
            Val::Bool(true) => "True".into(),
            Val::Bool(false) => "False".into(),
            Val::Int(i) => i.to_string(),
            Val::Float(f) => float_repr(*f),
            Val::Str(s) => str_repr(s),
            Val::List(l) => format!("[{}]", l.iter().map(Val::repr).collect::<Vec<_>>().join(", ")),
            Val::Dict(d) => format!(
                "{{{}}}",
                d.iter().map(|(k, v)| format!("{}: {}", str_repr(k), v.repr())).collect::<Vec<_>>().join(", ")
            ),
        }
    }

    /// int(v or 0): an int, a float cut toward zero, a string of digits, a bool; None for what
    /// Python's int() raises on.
    pub fn int_or0(v: Option<&Val>) -> Option<i64> {
        match v {
            None => Some(0),
            Some(x) if !x.truthy() => Some(0),
            Some(Val::Int(i)) => Some(*i),
            Some(Val::Bool(b)) => Some(*b as i64),
            Some(Val::Float(f)) if f.is_finite() => Some(f.trunc() as i64),
            Some(Val::Str(s)) => pkgzip::py::int(s, 10).and_then(|i| i64::try_from(i).ok()),
            _ => None,
        }
    }
}

/// repr() of a float: the shortest digits that read back the same, fixed notation for exponents
/// -4 to 15 (with ".0" when whole), else d.ddde+XX.
pub fn float_repr(f: f64) -> String {
    if f.is_nan() {
        return "nan".into();
    }
    if f.is_infinite() {
        return if f > 0.0 { "inf".into() } else { "-inf".into() };
    }
    if f == 0.0 {
        return if f.is_sign_negative() { "-0.0".into() } else { "0.0".into() };
    }
    let e = format!("{f:e}"); // e.g. -1.25e-5
    let (mant, exp) = e.split_once('e').unwrap();
    let exp: i32 = exp.parse().unwrap();
    let (neg, mant) = match mant.strip_prefix('-') {
        Some(m) => (true, m),
        None => (false, mant),
    };
    let digits: String = mant.chars().filter(|c| *c != '.').collect();
    let mut out = String::new();
    if neg {
        out.push('-');
    }
    if (-4..16).contains(&exp) {
        let n = digits.len() as i32;
        if exp < 0 {
            out.push_str("0.");
            out.push_str(&"0".repeat((-exp - 1) as usize));
            out.push_str(&digits);
        } else if exp + 1 >= n {
            out.push_str(&digits);
            out.push_str(&"0".repeat((exp + 1 - n) as usize));
            out.push_str(".0");
        } else {
            out.push_str(&digits[..(exp + 1) as usize]);
            out.push('.');
            out.push_str(&digits[(exp + 1) as usize..]);
        }
    } else {
        out.push_str(&digits[..1]);
        if digits.len() > 1 {
            out.push('.');
            out.push_str(&digits[1..]);
        }
        out.push('e');
        out.push(if exp < 0 { '-' } else { '+' });
        out.push_str(&format!("{:02}", exp.abs()));
    }
    out
}

/// repr() of a string: single quotes unless the text has one and no double quote; \\, \n, \r, \t
/// and the characters str.isprintable() refuses escaped.
pub fn str_repr(s: &str) -> String {
    let q = if s.contains('\'') && !s.contains('"') { '"' } else { '\'' };
    let mut out = String::with_capacity(s.len() + 2);
    out.push(q);
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c == q => {
                out.push('\\');
                out.push(c);
            }
            c if !printable(c) => {
                let n = c as u32;
                if n < 0x100 {
                    out.push_str(&format!("\\x{n:02x}"));
                } else if n < 0x10000 {
                    out.push_str(&format!("\\u{n:04x}"));
                } else {
                    out.push_str(&format!("\\U{n:08x}"));
                }
            }
            c => out.push(c),
        }
    }
    out.push(q);
    out
}

/// str.isprintable() of one character (close enough: controls, separators other than the space,
/// and the unassigned planes' private use are not).
fn printable(c: char) -> bool {
    let n = c as u32;
    !(n < 0x20
        || (0x7f..0xa1).contains(&n)
        || n == 0xad
        || (c != ' ' && c.is_whitespace())
        || (0x200b..=0x200f).contains(&n)
        || (0x2028..=0x202e).contains(&n)
        || (0x2060..=0x206f).contains(&n)
        || n == 0xfeff
        || (0xd800..=0xf8ff).contains(&n)
        || (0xfff9..=0xfffb).contains(&n)
        || n >= 0xf0000)
}

/// Builds a Dict from pairs (the literals of port.py's tables).
#[macro_export]
macro_rules! dict {
    ($($k:expr => $v:expr),* $(,)?) => {
        $crate::val::Dict(vec![$(($k.to_string(), $v)),*])
    };
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn floats() {
        for (f, s) in [
            (1.0, "1.0"),
            (0.375, "0.375"),
            (1e-5, "1e-05"),
            (1e16, "1e+16"),
            (1.5e16, "1.5e+16"),
            (123456789012345.0, "123456789012345.0"),
            (0.0001, "0.0001"),
            (-2.5, "-2.5"),
            (1e100, "1e+100"),
            (0.1, "0.1"),
        ] {
            assert_eq!(float_repr(f), s);
        }
    }

    #[test]
    fn reprs() {
        assert_eq!(str_repr("a'b"), "\"a'b\"");
        assert_eq!(str_repr("a'b\""), "'a\\'b\"'");
        assert_eq!(str_repr("x\ny\u{1}"), "'x\\ny\\x01'");
        assert_eq!(Val::List(vec![Val::s("a"), Val::Int(1), Val::Bool(true), Val::Null]).repr(), "['a', 1, True, None]");
        assert!(Val::Int(1).eq(&Val::Bool(true)) && Val::Int(1).eq(&Val::Float(1.0)) && !Val::Int(1).eq(&Val::s("1")));
    }
}
