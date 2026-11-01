//! The program side of `shaders.grab_fetch` (shaders/grabfetch.py): whether a GLES3 program's
//! fragment stage samples `_GrabTexture` only at the fragment's own screen position, checked by
//! evaluating both stages' main() symbolically (values as polynomials with exact rational
//! coefficients, as the Python's Fraction), and the program rewritten to read the framebuffer
//! instead. The Shader object's side (which SubShaders get a copy, the blob entries, the passes)
//! is the convert crate's.
//!
//! The analysis follows the Python statement by statement, so it accepts and refuses the same
//! programs with the same reasons. Values the analysis does not follow get fresh atoms
//! (`#<what><n>`); their numbers are the only thing that may differ from the Python's (which
//! numbers them partly in its sets' hash order, so they vary from run to run there too): no
//! decision depends on them, only a message that prints such a value.
use crate::glsl;
use crate::py;
use num_bigint::BigInt;
use num_rational::BigRational;
use num_traits::{One, Signed, ToPrimitive, Zero};
use regex::Regex;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::rc::Rc;
use std::sync::LazyLock;

pub const GRAB_TEXTURE: &str = "_GrabTexture";
pub const MARK: &str = "// pmunity grab_fetch: reads the framebuffer instead of _GrabTexture";
pub const FETCHED: &str = "ua_FramebufferColor";
/// A value with more terms becomes opaque (it is not a screen position).
const MAX_TERMS: usize = 512;

/// Why a program is not converted: `Not` is the Python's NotScreenPosition (the SubShader is
/// left as it is, with that reason); `Crash` an error the Python does not catch (its message).
#[derive(Clone, Debug, PartialEq)]
pub enum Fail {
    Not(String),
    Crash(String),
}

impl std::fmt::Display for Fail {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Fail::Not(m) | Fail::Crash(m) => f.write_str(m),
        }
    }
}

type R<T> = Result<T, Fail>;

fn not<T>(m: impl Into<String>) -> R<T> {
    Err(Fail::Not(m.into()))
}

fn no_text<T>() -> R<T> {
    Err(Fail::Crash(
        "AttributeError: 'NoneType' object has no attribute 'text'".into(),
    ))
}

// ---------------------------------------------------------------- symbolic values (polynomials)

type Q = BigRational;
type Atom = Rc<str>;
/// A monomial: its atoms, sorted (Python's sorted tuple of str).
type Mono = Vec<Atom>;

fn q(n: i64) -> Q {
    Q::from_integer(BigInt::from(n))
}

/// str(Fraction): "n" or "n/d".
fn q_str(v: &Q) -> String {
    if v.denom().is_one() {
        v.numer().to_string()
    } else {
        format!("{}/{}", v.numer(), v.denom())
    }
}

/// int(Fraction): truncated toward zero.
fn q_int(v: &Q) -> BigInt {
    v.to_integer()
}

/// A polynomial over named atoms with rational coefficients, its terms in the order the Python's
/// dict has them (no zero coefficient).
#[derive(Clone, Debug, Default)]
pub struct Poly {
    t: Vec<(Mono, Q)>,
}

/// Terms summed in insertion order (a dict's `t[k] = t.get(k, 0) + v`), zeros dropped at the end.
#[derive(Default)]
struct Acc {
    t: Vec<(Mono, Q)>,
    idx: HashMap<Mono, usize>,
}

impl Acc {
    fn add(&mut self, m: Mono, v: Q) {
        match self.idx.get(&m) {
            Some(&i) => self.t[i].1 += v,
            None => {
                self.idx.insert(m.clone(), self.t.len());
                self.t.push((m, v));
            }
        }
    }

    fn done(self) -> Poly {
        Poly {
            t: self.t.into_iter().filter(|(_, v)| !v.is_zero()).collect(),
        }
    }
}

impl Poly {
    fn constant(c: Q) -> Poly {
        let mut a = Acc::default();
        a.add(Vec::new(), c);
        a.done()
    }

    fn atom(a: &str) -> Poly {
        Poly {
            t: vec![(vec![Rc::from(a)], q(1))],
        }
    }

    fn add(&self, o: &Poly) -> Poly {
        let mut a = Acc::default();
        for (k, v) in self.t.iter().chain(&o.t) {
            a.add(k.clone(), v.clone());
        }
        a.done()
    }

    fn neg(&self) -> Poly {
        Poly {
            t: self.t.iter().map(|(k, v)| (k.clone(), -v)).collect(),
        }
    }

    fn sub(&self, o: &Poly) -> Poly {
        self.add(&o.neg())
    }

    fn mul(&self, o: &Poly) -> Poly {
        let mut a = Acc::default();
        for (ka, x) in &self.t {
            for (kb, y) in &o.t {
                let mut k: Mono = ka.iter().chain(kb).cloned().collect();
                k.sort();
                a.add(k, x * y);
            }
        }
        a.done()
    }

    fn scale(&self, c: &Q) -> Poly {
        Poly {
            t: self
                .t
                .iter()
                .map(|(k, v)| (k.clone(), v * c))
                .filter(|(_, v)| !v.is_zero())
                .collect(),
        }
    }

    fn is_const(&self) -> bool {
        self.t.iter().all(|(k, _)| k.is_empty())
    }

    fn value(&self) -> Q {
        self.get(&[]).cloned().unwrap_or_else(Q::zero)
    }

    fn get(&self, m: &[Atom]) -> Option<&Q> {
        self.t
            .iter()
            .find(|(k, _)| k.as_slice() == m)
            .map(|(_, v)| v)
    }

    fn sorted(&self) -> Vec<&(Mono, Q)> {
        let mut v: Vec<&(Mono, Q)> = self.t.iter().collect();
        v.sort_by(|a, b| a.0.cmp(&b.0));
        v
    }

    /// Poly.key(): the terms sorted, as text.
    pub fn key(&self) -> String {
        let parts: Vec<String> = self
            .sorted()
            .into_iter()
            .map(|(k, v)| {
                let m = k.iter().map(|a| &**a).collect::<Vec<&str>>().join("*");
                format!("{}*{}", q_str(v), if m.is_empty() { "1" } else { &m })
            })
            .collect();
        if parts.is_empty() {
            "0".into()
        } else {
            parts.join(" + ")
        }
    }

    /// The same terms (key() equal).
    fn same(&self, o: &Poly) -> bool {
        self.t.len() == o.t.len() && self.t.iter().all(|(k, v)| o.get(k) == Some(v))
    }
}

type P = Rc<Poly>;
type Vals = Vec<P>;

fn konst(c: Q) -> P {
    Rc::new(Poly::constant(c))
}

// ---------------------------------------------------------------- GLSL (HLSLcc's subset) tokens

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind {
    PpIf,
    PpElse,
    PpEnd,
    PpOther,
    Num,
    Id,
    Op,
}

#[derive(Clone, Debug)]
struct Tok {
    kind: Kind,
    text: Rc<str>,
    start: usize,
    end: usize,
}

static TOKEN: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(
        r"(?xs)^(?:
    (?P<pp>\#[^\n]*)
  | (?P<ws>\s+)
  | (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<num>(?:\d+\.\d*|\.\d+)(?:[eE][-+]?\d+)?[fF]?|\d+[eE][-+]?\d+[fF]?|0[xX][0-9a-fA-F]+[uU]?|\d+[uU]?)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op><<=|>>=|\+\+|--|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<|>>|<=|>=|==|!=|&&|\|\||\^\^|[-+*/%<>=!&|^~?:;,.(){}\[\]])
)",
    )
    .unwrap()
});
static PP_NAME: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^[ \t]*#\s*(\w*)").unwrap());
static PP_ELSE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^[ \t]*#\s*else\b").unwrap());
static MATRIX: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^mat[234](x[234])?$").unwrap());
static MAIN: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"\bvoid\s+main\s*\(\s*(?:void\s*)?\)\s*\{").unwrap());

fn tokens(src: &str, start: usize, end: usize) -> R<Vec<Tok>> {
    let mut out = Vec::new();
    let mut p = start;
    while p < end {
        let Some(c) = TOKEN.captures(&src[p..end]) else {
            let head: String = src[p..].chars().take(20).collect();
            return not(format!("cannot read the GLSL at {}", py::repr(&head)));
        };
        let m = c.get(0).unwrap();
        let (a, b) = (p + m.start(), p + m.end());
        let text = m.as_str();
        if c.name("pp").is_some() {
            let d = PP_NAME
                .captures(text)
                .and_then(|x| x.get(1))
                .map_or("", |x| x.as_str());
            let kind = match d {
                "if" | "ifdef" | "ifndef" => Kind::PpIf,
                "elif" | "else" => Kind::PpElse,
                "endif" => Kind::PpEnd,
                _ => Kind::PpOther,
            };
            out.push(Tok {
                kind,
                text: text.into(),
                start: a,
                end: b,
            });
        } else if c.name("ws").is_none() && c.name("comment").is_none() {
            let kind = if c.name("num").is_some() {
                Kind::Num
            } else if c.name("id").is_some() {
                Kind::Id
            } else {
                Kind::Op
            };
            out.push(Tok {
                kind,
                text: text.into(),
                start: a,
                end: b,
            });
        }
        p = b;
    }
    Ok(out)
}

/// _SIZES: the scalar and vector types' component counts.
fn type_size(t: &str) -> Option<usize> {
    Some(match t {
        "float" | "int" | "uint" | "bool" => 1,
        _ => {
            let rest = t.strip_prefix(['i', 'u', 'b']).unwrap_or(t);
            match rest {
                "vec2" => 2,
                "vec3" => 3,
                "vec4" => 4,
                _ => return None,
            }
        }
    })
}

fn is_type(t: &str) -> bool {
    type_size(t).is_some() || MATRIX.is_match(t)
}

const QUALIFIERS: [&str; 14] = [
    "highp",
    "mediump",
    "lowp",
    "const",
    "flat",
    "smooth",
    "centroid",
    "uniform",
    "in",
    "out",
    "inout",
    "precise",
    "invariant",
    "UNITY_UNIFORM",
];

fn builtin(name: &str) -> Option<usize> {
    Some(match name {
        "gl_Position" | "gl_FragCoord" => 4,
        "gl_VertexID" | "gl_InstanceID" | "gl_FrontFacing" => 1,
        "gl_PointCoord" => 2,
        "gl_PointSize" | "gl_FragDepth" => 1,
        _ => return None,
    })
}

fn swizzle(c: char) -> Option<usize> {
    match c {
        'x' | 'r' | 's' => Some(0),
        'y' | 'g' | 't' => Some(1),
        'z' | 'b' | 'p' => Some(2),
        'w' | 'a' | 'q' => Some(3),
        _ => None,
    }
}

const TEXTURE_FUNCS: [&str; 19] = [
    "texture",
    "textureProj",
    "textureLod",
    "textureOffset",
    "textureProjOffset",
    "textureLodOffset",
    "textureProjLod",
    "textureProjLodOffset",
    "textureGrad",
    "textureGradOffset",
    "textureProjGrad",
    "textureProjGradOffset",
    "texelFetch",
    "texelFetchOffset",
    "textureSize",
    "textureGather",
    "textureGatherOffset",
    "textureGatherOffsets",
    "textureQueryLod",
];

const ASSIGN: [&str; 13] = [
    "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", "++", "--",
];

fn binary_prec(op: &str) -> Option<u8> {
    Some(match op {
        "||" => 1,
        "^^" => 2,
        "&&" => 3,
        "|" => 4,
        "^" => 5,
        "&" => 6,
        "==" | "!=" => 7,
        "<" | ">" | "<=" | ">=" => 8,
        "<<" | ">>" => 9,
        "+" | "-" => 10,
        "*" | "/" | "%" => 11,
        _ => return None,
    })
}

type Size = (usize, Option<i64>);

static DECL: LazyLock<Regex> = LazyLock::new(|| {
    let mut types: Vec<String> = ["float", "int", "uint", "bool"].map(String::from).to_vec();
    for n in ["2", "3", "4"] {
        for p in ["", "i", "u", "b"] {
            types.push(format!("{p}vec{n}"));
        }
    }
    Regex::new(&format!(
        r"\b({}|mat[234](?:x[234])?)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*[;=,)]",
        types.join("|")
    ))
    .unwrap()
});

/// int() of a decimal digit string (Python's int accepts any Unicode decimal digits).
fn py_int(s: &str) -> R<BigInt> {
    let digits: Option<String> = s
        .chars()
        .map(|c| c.to_digit(10).map(|d| char::from(b'0' + d as u8)))
        .collect();
    match digits.filter(|d| !d.is_empty()) {
        Some(d) => Ok(d.parse().unwrap()),
        None => Err(Fail::Crash(format!(
            "ValueError: invalid literal for int() with base 10: {}",
            py::repr(s)
        ))),
    }
}

fn small(v: &BigInt) -> R<i64> {
    v.to_i64()
        .ok_or_else(|| Fail::Crash("OverflowError: index too large".into()))
}

/// {name: (components, array length or None)} of a stage's declarations outside its functions.
fn declarations(src: &str) -> R<HashMap<String, Size>> {
    let mut out = HashMap::new();
    for c in DECL.captures_iter(&glsl::strip_comments(src)) {
        let t = &c[1];
        let n = if MATRIX.is_match(t) {
            4
        } else {
            type_size(t).unwrap()
        };
        let arr = match c.get(3) {
            Some(a) => Some(small(&py_int(a.as_str())?)?),
            None => None,
        };
        out.insert(c[2].to_string(), (n, arr));
    }
    Ok(out)
}

/// Fraction(s) of a number token (digits, a point, an exponent).
fn fraction(s: &str) -> R<Q> {
    let bad = || {
        Fail::Crash(format!(
            "ValueError: Invalid literal for Fraction: {}",
            py::repr(s)
        ))
    };
    let (mant, exp) = match s.find(['e', 'E']) {
        Some(i) => (&s[..i], Some(&s[i + 1..])),
        None => (s, None),
    };
    let (ip, fp) = match mant.find('.') {
        Some(i) => (&mant[..i], &mant[i + 1..]),
        None => (mant, ""),
    };
    if ip.is_empty() && fp.is_empty() {
        return Err(bad());
    }
    let digits = format!("{ip}{fp}");
    let mut num: BigInt = if digits.is_empty() {
        BigInt::zero()
    } else {
        py_int(&digits).map_err(|_| bad())?
    };
    let mut e: i64 = -(fp.chars().count() as i64);
    if let Some(x) = exp {
        let (neg, d) = match x.strip_prefix('-') {
            Some(d) => (true, d),
            None => (false, x.strip_prefix('+').unwrap_or(x)),
        };
        let v = small(&py_int(d).map_err(|_| bad())?)?;
        e += if neg { -v } else { v };
    }
    let ten = BigInt::from(10);
    let mut den = BigInt::one();
    if e >= 0 {
        num *= num_traits::pow(ten, e as usize);
    } else {
        den = num_traits::pow(ten, (-e) as usize);
    }
    Ok(Q::new(num, den))
}

/// A sample of the grab texture: the function, its other arguments' values, the call's span in
/// the stage source, how many conditional blocks it is in.
#[derive(Clone, Debug)]
struct GrabCall {
    f: Rc<str>,
    args: Vec<Vals>,
    start: usize,
    end: usize,
}

/// One stage's main() evaluated symbolically (the Python's Stage).
struct Stage {
    sampler: String,
    decl: HashMap<String, Size>,
    vars: BTreeMap<String, Vals>,
    local: HashMap<String, Size>,
    opaque: usize,
    inverses: HashMap<Atom, P>,
    grab_calls: Vec<GrabCall>,
    clobbered: HashSet<String>,
    cond_return: bool,
    depth: usize,
    body_start: usize,
    toks: Vec<Tok>,
    i: usize,
    uses: usize,
}

fn base_name(key: &str) -> &str {
    key.split('[').next().unwrap_or(key)
}

impl Stage {
    fn new(src: &str, sampler: &str) -> R<Stage> {
        let decl = declarations(src)?;
        let Some(m) = MAIN.find(src) else {
            return not("no main()");
        };
        let toks = tokens(src, m.end(), src.len())?;
        let mut depth = 1;
        let mut end = None;
        for (i, t) in toks.iter().enumerate() {
            if &*t.text == "{" {
                depth += 1;
            } else if &*t.text == "}" {
                depth -= 1;
                if depth == 0 {
                    end = Some(i);
                    break;
                }
            }
        }
        let Some(end) = end else {
            return not("main() has no end");
        };
        let mut toks = toks;
        toks.truncate(end);
        let uses = tokens(src, 0, src.len())?
            .iter()
            .filter(|t| t.kind == Kind::Id && &*t.text == sampler)
            .count();
        Ok(Stage {
            sampler: sampler.into(),
            decl,
            vars: BTreeMap::new(),
            local: HashMap::new(),
            opaque: 0,
            inverses: HashMap::new(),
            grab_calls: Vec::new(),
            clobbered: HashSet::new(),
            cond_return: false,
            depth: 0,
            body_start: m.end(),
            toks,
            i: 0,
            uses,
        })
    }

    // --- values
    fn new_atom(&mut self, what: &str) -> P {
        self.opaque += 1;
        Rc::new(Poly::atom(&format!("#{what}{}", self.opaque)))
    }

    fn opaques(&mut self, what: &str, n: usize) -> Vals {
        (0..n).map(|_| self.new_atom(what)).collect()
    }

    fn size_of(&self, name: &str) -> Option<Size> {
        if let Some(s) = self.local.get(name) {
            return Some(*s);
        }
        if let Some(s) = self.decl.get(name) {
            return Some(*s);
        }
        builtin(name).map(|n| (n, None))
    }

    fn read(&mut self, name: &str, index: Option<&BigInt>) -> Vals {
        let Some((n, arr)) = self.size_of(name) else {
            return vec![self.new_atom(name)];
        };
        let key = match (index, arr) {
            (Some(i), Some(_)) => format!("{name}[{i}]"),
            _ => name.to_string(),
        };
        if arr.is_some()
            && (index.is_none() || self.clobbered.contains(name) && !self.vars.contains_key(&key))
        {
            return self.opaques(name, n);
        }
        if !self.vars.contains_key(&key) {
            let d = self.default(&key);
            self.vars.insert(key.clone(), d);
        }
        self.vars[&key].clone()
    }

    fn write(&mut self, key: &str, comps: &[i64], vals: Vals) -> R<()> {
        let name = base_name(key).to_string();
        let n = match self.size_of(&name) {
            Some((n, _)) => n as i64,
            None => match comps.iter().max() {
                Some(m) => m + 1,
                None => {
                    return Err(Fail::Crash(
                        "ValueError: max() arg is an empty sequence".into(),
                    ))
                }
            },
        };
        let mut cur = match self.vars.get(key) {
            Some(v) if !v.is_empty() => v.clone(),
            _ => self.default(key),
        };
        for _ in 0..(n - cur.len() as i64).max(0) {
            let a = self.new_atom(key);
            cur.push(a);
        }
        let mut vals = vals;
        if vals.len() == 1 && comps.len() > 1 {
            vals = vec![vals[0].clone(); comps.len()];
        }
        if vals.len() < comps.len() {
            return not(format!(
                "assignment to {key} of {} components for {}",
                vals.len(),
                comps.len()
            ));
        }
        for (&c, v) in comps.iter().zip(vals) {
            if c >= cur.len() as i64 {
                for _ in 0..(c + 1 - cur.len() as i64) {
                    let a = self.new_atom(key);
                    cur.push(a);
                }
            }
            let j = if c < 0 { c + cur.len() as i64 } else { c };
            if j < 0 {
                return Err(Fail::Crash(
                    "IndexError: list assignment index out of range".into(),
                ));
            }
            cur[j as usize] = v;
        }
        self.vars.insert(key.to_string(), cur);
        Ok(())
    }

    fn inv(&mut self, p: &P) -> P {
        if p.is_const() {
            let v = p.value();
            if v.is_zero() {
                return self.new_atom("div0");
            }
            return konst(v.recip());
        }
        let a: Atom = format!("1/({})", p.key()).into();
        self.inverses.insert(a.clone(), p.clone());
        Rc::new(Poly {
            t: vec![(vec![a], q(1))],
        })
    }

    fn guard(&mut self, p: Poly) -> P {
        if p.t.len() <= MAX_TERMS {
            Rc::new(p)
        } else {
            self.new_atom("big")
        }
    }

    // --- tokens
    fn peek(&self, k: usize) -> Option<&Tok> {
        self.toks.get(self.i + k)
    }

    fn peek_is(&self, text: &str) -> bool {
        self.peek(0).is_some_and(|t| &*t.text == text)
    }

    /// self.peek().text where the Python does not check for the end.
    fn peek_text(&self) -> R<Rc<str>> {
        match self.peek(0) {
            Some(t) => Ok(t.text.clone()),
            None => no_text(),
        }
    }

    fn next(&mut self) -> R<Tok> {
        match self.toks.get(self.i) {
            Some(t) => {
                self.i += 1;
                Ok(t.clone())
            }
            None => not("unexpected end of main()"),
        }
    }

    fn expect(&mut self, text: &str) -> R<Tok> {
        let t = self.next()?;
        if &*t.text != text {
            return not(format!(
                "expected {}, found {}",
                py::repr(text),
                py::repr(&t.text)
            ));
        }
        Ok(t)
    }

    // --- statements
    fn run(mut self) -> R<Stage> {
        while self.peek(0).is_some() {
            self.statement()?;
        }
        Ok(self)
    }

    fn statement(&mut self) -> R<()> {
        let Some(t) = self.peek(0).cloned() else {
            return Err(Fail::Crash(
                "AttributeError: 'NoneType' object has no attribute 'kind'".into(),
            ));
        };
        match t.kind {
            Kind::PpIf => {
                self.next()?;
                return self.pp_block();
            }
            Kind::PpElse | Kind::PpEnd => {
                return not(format!("unmatched {}", py::repr(py::strip(&t.text))));
            }
            Kind::PpOther => {
                self.next()?;
                return Ok(());
            }
            _ => {}
        }
        match &*t.text {
            "{" => {
                self.next()?;
                while self.peek(0).is_some_and(|t| &*t.text != "}") {
                    self.statement()?;
                }
                if self.peek(0).is_some() {
                    self.expect("}")?;
                }
                return Ok(());
            }
            ";" => {
                self.next()?;
                return Ok(());
            }
            "if" => {
                self.next()?;
                self.expect("(")?;
                self.expr()?;
                self.expect(")")?;
                let before = self.vars.clone();
                self.depth += 1;
                self.statement()?;
                let mut results = vec![std::mem::replace(&mut self.vars, before.clone())];
                if self.peek_is("else") {
                    self.next()?;
                    self.statement()?;
                    results.push(self.vars.clone());
                } else {
                    results.push(before);
                }
                self.depth -= 1;
                self.vars = self.merge(&results);
                return Ok(());
            }
            "for" | "while" | "do" => return self.lp(),
            "discard" | "break" | "continue" => {
                self.next()?;
                self.expect(";")?;
                return Ok(());
            }
            "return" => {
                self.next()?;
                if &*self.peek_text()? != ";" {
                    self.expr()?;
                }
                self.expect(";")?;
                if self.depth > 0 {
                    self.cond_return = true;
                } else {
                    self.toks.truncate(self.i); // nothing after an unconditional return runs
                }
                return Ok(());
            }
            _ => {}
        }
        if self.is_declaration() {
            return self.declaration();
        }
        self.assignment_or_expr()?;
        self.expect(";")?;
        Ok(())
    }

    /// A variable's value before main() writes it: its atoms.
    fn default(&self, key: &str) -> Vals {
        let n = self.size_of(base_name(key)).map_or(4, |s| s.0);
        (0..n)
            .map(|c| Rc::new(Poly::atom(&format!("{key}.{}", &"xyzw"[c..c + 1]))))
            .collect()
    }

    /// The state after alternatives the analysis does not decide between: a component that
    /// differs between them is opaque.
    fn merge(&mut self, results: &[BTreeMap<String, Vals>]) -> BTreeMap<String, Vals> {
        let keys: BTreeSet<&String> = results.iter().flat_map(|r| r.keys()).collect();
        let mut merged = BTreeMap::new();
        for k in keys {
            let vals: Vec<Vals> = results
                .iter()
                .map(|r| match r.get(k) {
                    Some(v) if !v.is_empty() => v.clone(),
                    _ => self.default(k),
                })
                .collect();
            let n = vals.iter().map(Vec::len).max().unwrap_or(0);
            let mut out = Vec::with_capacity(n);
            for c in 0..n {
                let same = vals
                    .iter()
                    .all(|v| c < v.len() && c < vals[0].len() && v[c].same(&vals[0][c]));
                if same {
                    out.push(vals[0][c].clone());
                } else {
                    out.push(self.new_atom(k));
                }
            }
            merged.insert(k.clone(), out);
        }
        merged
    }

    /// #if ... [#elif ... | #else ...] #endif between statements: any branch possible.
    fn pp_block(&mut self) -> R<()> {
        let before = self.vars.clone();
        self.depth += 1;
        self.pp_part()?;
        let mut results = vec![self.vars.clone()];
        let mut has_else = false;
        while self.peek(0).is_some_and(|t| t.kind == Kind::PpElse) {
            let t = self.next()?;
            has_else |= PP_ELSE.is_match(&t.text);
            self.vars = before.clone();
            self.pp_part()?;
            results.push(self.vars.clone());
        }
        if !has_else {
            results.push(before);
        }
        self.depth -= 1;
        let t = self.next()?;
        if t.kind != Kind::PpEnd {
            return not("#if without #endif in main()");
        }
        self.vars = self.merge(&results);
        Ok(())
    }

    fn pp_part(&mut self) -> R<()> {
        while self
            .peek(0)
            .is_some_and(|t| !matches!(t.kind, Kind::PpElse | Kind::PpEnd))
        {
            self.statement()?;
        }
        Ok(())
    }

    /// A loop: what its body assigns is opaque before, during and after it.
    fn lp(&mut self) -> R<()> {
        let start = self.i;
        let t = self.next()?;
        let body;
        if &*t.text == "do" {
            body = self.skip_statement()?;
            self.expect("while")?;
            self.skip_parens()?;
            self.expect(";")?;
        } else {
            self.skip_parens()?;
            body = self.skip_statement()?;
        }
        let assigned = self.assigned_in(start, self.i);
        self.forget(&assigned);
        let end = self.i;
        self.i = body;
        self.depth += 1;
        let saved = self.vars.clone();
        self.statement()?;
        self.vars = saved;
        self.depth -= 1;
        self.i = end;
        Ok(())
    }

    fn skip_parens(&mut self) -> R<()> {
        self.expect("(")?;
        let mut depth = 1i64;
        while depth != 0 {
            let t = self.next()?;
            depth += match &*t.text {
                "(" => 1,
                ")" => -1,
                _ => 0,
            };
        }
        Ok(())
    }

    /// Skips one statement; returns its first token's index.
    fn skip_statement(&mut self) -> R<usize> {
        let start = self.i;
        let mut t = self.next()?;
        if &*t.text == "{" {
            let mut depth = 1i64;
            while depth != 0 {
                t = self.next()?;
                depth += match &*t.text {
                    "{" => 1,
                    "}" => -1,
                    _ => 0,
                };
            }
        } else if matches!(&*t.text, "if" | "for" | "while") {
            self.skip_parens()?;
            self.skip_statement()?;
            if &*t.text == "if" && self.peek_is("else") {
                self.next()?;
                self.skip_statement()?;
            }
        } else {
            while &*t.text != ";" {
                t = self.next()?;
            }
        }
        Ok(start)
    }

    fn assigned_in(&self, a: usize, b: usize) -> BTreeSet<String> {
        let mut names = BTreeSet::new();
        for j in a..b {
            let t = &self.toks[j];
            if t.kind != Kind::Id {
                continue;
            }
            let mut k = j + 1;
            if k < b && &*self.toks[k].text == "[" {
                let mut depth = 0i64;
                while k < b {
                    depth += match &*self.toks[k].text {
                        "[" => 1,
                        "]" => -1,
                        _ => 0,
                    };
                    k += 1;
                    if depth == 0 {
                        break;
                    }
                }
            }
            if k + 1 < b && &*self.toks[k].text == "." && self.toks[k + 1].kind == Kind::Id {
                k += 2;
            }
            if k < b && ASSIGN.contains(&&*self.toks[k].text) {
                names.insert(t.text.to_string());
            }
            if j > a && matches!(&*self.toks[j - 1].text, "++" | "--") {
                names.insert(t.text.to_string());
            }
        }
        names
    }

    fn is_declaration(&self) -> bool {
        let mut k = 0;
        while self.peek(k).is_some_and(|t| QUALIFIERS.contains(&&*t.text)) {
            k += 1;
        }
        match (self.peek(k), self.peek(k + 1)) {
            (Some(t), Some(u)) => is_type(&t.text) && u.kind == Kind::Id,
            _ => false,
        }
    }

    fn declaration(&mut self) -> R<()> {
        while QUALIFIERS.contains(&&*self.peek_text()?) {
            self.next()?;
        }
        let t = self.next()?.text;
        let n = if MATRIX.is_match(&t) {
            4
        } else {
            type_size(&t).unwrap()
        };
        loop {
            let name = self.next()?.text.to_string();
            let mut arr = None;
            if &*self.peek_text()? == "[" {
                self.next()?;
                let a = self.next()?.text;
                arr = Some(small(&py_int(a.trim_end_matches(['u', 'U']))?)?);
                self.expect("]")?;
            }
            self.local.insert(name.clone(), (n, arr));
            self.vars.retain(|k, _| base_name(k) != name.as_str());
            if &*self.peek_text()? == "=" {
                self.next()?;
                let v = self.expr()?;
                if arr.is_none() {
                    let comps: Vec<i64> = (0..n as i64).collect();
                    let v = if v.len() == 1 {
                        vec![v[0].clone(); n]
                    } else {
                        v
                    };
                    self.write(&name, &comps, v)?;
                }
            } else if arr.is_none() {
                let v = self.opaques(&name, n);
                self.vars.insert(name.clone(), v);
            }
            if &*self.peek_text()? == "," {
                self.next()?;
                continue;
            }
            self.expect(";")?;
            return Ok(());
        }
    }

    /// (key, components, name) of an assignment's target, or None (not one).
    #[allow(clippy::type_complexity)]
    fn lvalue(&mut self) -> R<Option<(Option<String>, Vec<i64>, String)>> {
        let save = self.i;
        let Some(t) = self.peek(0).cloned() else {
            return Ok(None);
        };
        if t.kind != Kind::Id {
            return Ok(None);
        }
        let Some((n, arr)) = self.size_of(&t.text) else {
            return Ok(None);
        };
        self.next()?;
        let mut key = Some(t.text.to_string());
        let mut comps: Vec<i64> = (0..n as i64).collect();
        if self.peek_is("[") {
            self.next()?;
            let idx = self.expr()?;
            self.expect("]")?;
            if arr.is_some() && idx[0].is_const() {
                key = Some(format!("{}[{}]", t.text, q_int(&idx[0].value())));
            } else if arr.is_none() && idx[0].is_const() {
                comps = vec![small(&q_int(&idx[0].value()))?];
            } else {
                key = None; // (a dynamic index: the whole variable becomes opaque)
            }
        }
        if self.peek_is(".") {
            self.next()?;
            let sw = self.next()?.text;
            let s: Option<Vec<usize>> = sw.chars().map(swizzle).collect();
            let Some(s) = s else {
                self.i = save;
                return Ok(None);
            };
            comps = s
                .into_iter()
                .map(|c| if c < comps.len() { comps[c] } else { c as i64 })
                .collect();
        }
        if !self.peek(0).is_some_and(|t| ASSIGN.contains(&&*t.text)) {
            self.i = save;
            return Ok(None);
        }
        Ok(Some((key, comps, t.text.to_string())))
    }

    fn assignment_or_expr(&mut self) -> R<()> {
        let Some((key, comps, name)) = self.lvalue()? else {
            self.expr()?;
            return Ok(());
        };
        let op = self.next()?.text;
        let v = if &*op == "++" || &*op == "--" {
            self.opaques(&name, comps.len())
        } else {
            let mut v = self.expr()?;
            if &*op != "=" {
                let cur = match &key {
                    Some(k) => {
                        let (nm, idx) = split_key(k);
                        self.read(&nm, idx.as_ref())
                    }
                    None => {
                        let a = self.new_atom(&name);
                        vec![a; 4]
                    }
                };
                let mut old = Vec::with_capacity(comps.len());
                for &c in &comps {
                    let j = if c < 0 { c + cur.len() as i64 } else { c };
                    if c < cur.len() as i64 {
                        if j < 0 {
                            return Err(Fail::Crash("IndexError: list index out of range".into()));
                        }
                        old.push(cur[j as usize].clone());
                    } else {
                        old.push(self.new_atom(&name));
                    }
                }
                v = self.binop(&op[..op.len() - 1], old, v);
            }
            v
        };
        match key {
            None => {
                self.forget(&BTreeSet::from([name]));
                Ok(())
            }
            Some(k) => self.write(&k, &comps, v),
        }
    }

    /// Variables written where the analysis does not know (a loop, a dynamic index): opaque.
    fn forget(&mut self, names: &BTreeSet<String>) {
        self.vars.retain(|k, _| !names.contains(base_name(k)));
        for name in names {
            match self.size_of(name) {
                Some((n, None)) => {
                    let v = self.opaques(name, n);
                    self.vars.insert(name.clone(), v);
                }
                Some(_) => {
                    self.clobbered.insert(name.clone());
                }
                None => {}
            }
        }
    }

    // --- expressions
    fn expr(&mut self) -> R<Vals> {
        let c = self.binary(1)?;
        if self.peek_is("?") {
            self.next()?;
            let a = self.expr()?;
            self.expect(":")?;
            let b = self.expr()?;
            return Ok(self.opaques("?", a.len().max(b.len())));
        }
        Ok(c)
    }

    fn binary(&mut self, prec: u8) -> R<Vals> {
        let mut left = self.unary()?;
        loop {
            let p = match self.peek(0) {
                Some(t) if t.kind == Kind::Op => binary_prec(&t.text),
                _ => None,
            };
            let Some(p) = p.filter(|&p| p >= prec) else {
                return Ok(left);
            };
            let t = self.next()?;
            let right = self.binary(p + 1)?;
            left = self.binop(&t.text, left, right);
        }
    }

    fn binop(&mut self, op: &str, a: Vals, b: Vals) -> Vals {
        let n = a.len().max(b.len());
        let a = if a.len() == 1 {
            vec![a[0].clone(); n]
        } else {
            a
        };
        let b = if b.len() == 1 {
            vec![b[0].clone(); n]
        } else {
            b
        };
        if a.len() != b.len() {
            return self.opaques(op, n); // (matrix products: not followed)
        }
        let mut out = Vec::with_capacity(n);
        match op {
            "+" | "-" | "*" => {
                for (x, y) in a.iter().zip(&b) {
                    let p = match op {
                        "+" => x.add(y),
                        "-" => x.sub(y),
                        _ => x.mul(y),
                    };
                    out.push(self.guard(p));
                }
            }
            "/" => {
                for (x, y) in a.iter().zip(&b) {
                    let p = if y.is_const() && !y.value().is_zero() {
                        x.scale(&y.value().recip())
                    } else {
                        let iy = self.inv(y);
                        x.mul(&iy)
                    };
                    out.push(self.guard(p));
                }
            }
            _ => return self.opaques(op, n),
        }
        out
    }

    fn unary(&mut self) -> R<Vals> {
        let t = self.peek_text()?;
        match &*t {
            "-" => {
                self.next()?;
                Ok(self.unary()?.iter().map(|x| Rc::new(x.neg())).collect())
            }
            "+" => {
                self.next()?;
                self.unary()
            }
            "!" | "~" | "++" | "--" => {
                self.next()?;
                let n = self.unary()?.len();
                Ok(self.opaques(&t, n))
            }
            _ => {
                let v = self.primary()?;
                self.postfix(v)
            }
        }
    }

    fn primary(&mut self) -> R<Vals> {
        let t = self.next()?;
        if t.kind == Kind::Num {
            let s = t.text.trim_end_matches(['f', 'F']);
            if s.get(..2).is_some_and(|p| p.eq_ignore_ascii_case("0x")) {
                let h = s.trim_end_matches(['u', 'U']);
                let v = BigInt::parse_bytes(&h.as_bytes()[2..], 16).ok_or_else(|| {
                    Fail::Crash(format!(
                        "ValueError: invalid literal for int() with base 16: {}",
                        py::repr(h)
                    ))
                })?;
                return Ok(vec![konst(Q::from_integer(v))]);
            }
            return Ok(vec![konst(fraction(s.trim_end_matches(['u', 'U']))?)]);
        }
        if &*t.text == "(" {
            let mut v = self.expr()?;
            while &*self.peek_text()? == "," {
                // (the comma operator: the last value)
                self.next()?;
                v = self.expr()?;
            }
            self.expect(")")?;
            return Ok(v);
        }
        if t.kind == Kind::Id {
            if &*t.text == "true" || &*t.text == "false" {
                return Ok(vec![konst(q((&*t.text == "true") as i64))]);
            }
            if self.peek_is("(") {
                return self.call(&t);
            }
            if self.peek_is("[") {
                if let Some((n, Some(_))) = self.size_of(&t.text) {
                    self.next()?;
                    let idx = self.expr()?;
                    self.expect("]")?;
                    if idx[0].is_const() {
                        let i = q_int(&idx[0].value());
                        return Ok(self.read(&t.text, Some(&i)));
                    }
                    return Ok(self.opaques(&t.text, n));
                }
            }
            if *t.text == *self.sampler {
                return not(format!(
                    "{} used other than as a texture function's sampler",
                    self.sampler
                ));
            }
            return Ok(self.read(&t.text, None));
        }
        not(format!("unexpected {}", py::repr(&t.text)))
    }

    fn postfix(&mut self, mut v: Vals) -> R<Vals> {
        while let Some(t) = self.peek(0).cloned() {
            match &*t.text {
                "." => {
                    self.next()?;
                    let sw = self.next()?.text;
                    let idx: Option<Vec<usize>> = sw
                        .chars()
                        .map(|c| swizzle(c).filter(|&k| k < v.len()))
                        .collect();
                    match idx {
                        Some(ix) => v = ix.into_iter().map(|k| v[k].clone()).collect(),
                        None => {
                            let n = sw.chars().count();
                            return Ok(self.opaques(&format!(".{sw}"), n));
                        }
                    }
                }
                "[" => {
                    self.next()?;
                    let idx = self.expr()?;
                    self.expect("]")?;
                    let k = idx[0].value();
                    v = if idx[0].is_const() && !k.is_negative() && k < q(v.len() as i64) {
                        let k = q_int(&k).to_usize().unwrap();
                        vec![v[k].clone()]
                    } else {
                        vec![self.new_atom("[]")]
                    };
                }
                "++" | "--" => {
                    self.next()?;
                    v = self.opaques(&t.text, v.len());
                }
                _ => return Ok(v),
            }
        }
        Ok(v)
    }

    fn args(&mut self) -> R<Vec<Vals>> {
        let mut out = Vec::new();
        self.expect("(")?;
        if &*self.peek_text()? == ")" {
            self.next()?;
            return Ok(out);
        }
        loop {
            out.push(self.expr()?);
            let t = self.next()?;
            if &*t.text == ")" {
                return Ok(out);
            }
            if &*t.text != "," {
                return not(format!(
                    "expected , or ) in a call, found {}",
                    py::repr(&t.text)
                ));
            }
        }
    }

    fn call(&mut self, name: &Tok) -> R<Vals> {
        let f = &*name.text;
        if TEXTURE_FUNCS.contains(&f) && self.peek(1).is_some_and(|t| *t.text == *self.sampler) {
            self.expect("(")?;
            self.next()?; // the sampler
            let mut rest = Vec::new();
            while &*self.peek_text()? == "," {
                self.next()?;
                rest.push(self.expr()?);
            }
            let end = self.expect(")")?;
            self.grab_calls.push(GrabCall {
                f: name.text.clone(),
                args: rest,
                start: name.start,
                end: end.end,
            });
            return Ok(self.opaques("grab", 4));
        }
        let a = self.args()?;
        if let Some(n) = type_size(f) {
            let mut flat: Vals = a.iter().flatten().cloned().collect();
            if flat.len() == 1 {
                flat = vec![flat[0].clone(); n];
            }
            if flat.len() < n {
                return not(format!("{f} of {} components", flat.len()));
            }
            flat.truncate(n);
            if f == "float" || f.starts_with("vec") {
                return Ok(flat);
            }
            return Ok(self.opaques(f, flat.len())); // (ints, uints, bools: conversions not followed)
        }
        if f == "dot" && a.len() == 2 && a[0].len() == a[1].len() {
            let mut s = Poly::default();
            for (x, y) in a[0].iter().zip(&a[1]) {
                s = s.add(&x.mul(y));
            }
            return Ok(vec![self.guard(s)]);
        }
        let mut n = match f {
            "dot" | "length" | "distance" | "determinant" | "any" | "all" => 1,
            _ => a.iter().map(Vec::len).max().unwrap_or(1),
        };
        if TEXTURE_FUNCS.contains(&f) {
            n = 4;
        }
        Ok(self.opaques(f, n))
    }
}

fn split_key(key: &str) -> (String, Option<BigInt>) {
    match key.find('[') {
        Some(i) => (
            key[..i].to_string(),
            key[i + 1..key.len() - 1].parse::<BigInt>().ok(),
        ),
        None => (key.to_string(), None),
    }
}

// ---------------------------------------------------------------- the screen-position check

/// Rational coefficients c with target == sum(c[j] * basis[j]) (polynomial identity), or None.
fn linear(target: &Poly, basis: &[P]) -> Option<Vec<Q>> {
    let mut monos: Vec<&Mono> = basis
        .iter()
        .map(|p| &**p)
        .chain([target])
        .flat_map(|p| p.t.iter().map(|(k, _)| k))
        .collect();
    monos.sort();
    monos.dedup();
    let n = basis.len();
    let mut rows: Vec<Vec<Q>> = monos
        .iter()
        .map(|m| {
            basis
                .iter()
                .map(|p| p.get(m).cloned().unwrap_or_else(Q::zero))
                .chain([target.get(m).cloned().unwrap_or_else(Q::zero)])
                .collect()
        })
        .collect();
    let mut piv = Vec::new();
    let mut r = 0;
    for c in 0..n {
        let Some(k) = (r..rows.len()).find(|&i| !rows[i][c].is_zero()) else {
            continue;
        };
        rows.swap(r, k);
        let pv = rows[r][c].clone();
        rows[r] = rows[r].iter().map(|x| x / &pv).collect();
        for i in 0..rows.len() {
            if i != r && !rows[i][c].is_zero() {
                let f = rows[i][c].clone();
                let rr = rows[r].clone();
                rows[i] = rows[i].iter().zip(&rr).map(|(x, y)| x - &f * y).collect();
            }
        }
        piv.push(c);
        r += 1;
    }
    if rows
        .iter()
        .any(|row| !row[n].is_zero() && row[..n].iter().all(Zero::is_zero))
    {
        return None;
    }
    let mut sol = vec![Q::zero(); n];
    for (i, &c) in piv.iter().enumerate() {
        sol[c] = rows[i][n].clone();
    }
    Some(sol)
}

/// A fragment value as N / D + c: ({varying atom: coefficient} of N in the Python's dict order,
/// D, c), every term of N divided by the same D; or None.
#[allow(clippy::type_complexity)]
fn ratio(p: &Poly, frag: &Stage) -> Option<(Vec<(Atom, Q)>, P, Q)> {
    let mut inv_atom: Option<Atom> = None;
    let mut num: Vec<(Atom, Q)> = Vec::new();
    let mut c = Q::zero();
    for (mono, coef) in &p.t {
        if mono.is_empty() {
            c += coef;
            continue;
        }
        let invs: Vec<&Atom> = mono
            .iter()
            .filter(|a| frag.inverses.contains_key(*a))
            .collect();
        let rest: Vec<&Atom> = mono
            .iter()
            .filter(|a| !frag.inverses.contains_key(*a))
            .collect();
        if invs.len() != 1 || rest.len() != 1 || inv_atom.as_ref().is_some_and(|i| i != invs[0]) {
            return None;
        }
        inv_atom = Some(invs[0].clone());
        match num.iter_mut().find(|(a, _)| a == rest[0]) {
            Some((_, v)) => *v += coef,
            None => num.push((rest[0].clone(), coef.clone())),
        }
    }
    let inv_atom = inv_atom?; // (no perspective division: not a screen position)
    Some((num, frag.inverses[&inv_atom].clone(), c))
}

/// {varying atom: coefficient} of a denominator linear in the varyings (no constant term).
fn ratio_den(den: &Poly, frag: &Stage) -> R<Vec<(Atom, Q)>> {
    let mut out: Vec<(Atom, Q)> = Vec::new();
    for (mono, coef) in &den.t {
        if mono.len() != 1 || frag.inverses.contains_key(&mono[0]) || mono[0].starts_with('#') {
            return not(format!(
                "a division by {}: not linear in the varyings",
                den.key()
            ));
        }
        match out.iter_mut().find(|(a, _)| *a == mono[0]) {
            Some((_, v)) => *v = coef.clone(),
            None => out.push((mono[0].clone(), coef.clone())),
        }
    }
    Ok(out)
}

/// The fragment stage evaluated, if every use of _GrabTexture samples it at the fragment's own
/// screen position (grabfetch.screen_uv: its docstring has the rule), else why not.
fn screen_uv(vertex_src: &str, fragment_src: &str) -> R<Stage> {
    let mut frag = Stage::new(fragment_src, GRAB_TEXTURE)?.run()?;
    if frag.grab_calls.is_empty() {
        return not(format!("the fragment stage does not sample {GRAB_TEXTURE}"));
    }
    if frag.uses != frag.grab_calls.len() + 1 {
        return not(format!(
            "{GRAB_TEXTURE} is used other than declared once and sampled"
        ));
    }
    let mut vert = Stage::new(vertex_src, "\0")?.run()?;
    if vert.cond_return {
        return not("the vertex stage returns early");
    }
    let mut basis = vert.read("gl_Position", None);
    basis.push(konst(q(1)));
    let mut lin: HashMap<Atom, Vec<Q>> = HashMap::new();
    let mut varying = |atom: &Atom, vert: &mut Stage| -> R<Vec<Q>> {
        if let Some(v) = lin.get(atom) {
            return Ok(v.clone());
        }
        if atom.starts_with('#') {
            return not(
                "its coordinate depends on a value the check does not follow (a function, a \
                 comparison, a branch or a loop)",
            );
        }
        let (name, comp) = match atom.rfind('.') {
            Some(i) => (&atom[..i], &atom[i + 1..]),
            None => ("", &atom[..]),
        };
        let ok = vert.decl.get(name).is_some_and(|d| d.1.is_none())
            && "xyzw".contains(comp)
            && vert.vars.contains_key(name);
        if !ok {
            return not(format!("{atom}: not a varying the vertex stage writes"));
        }
        let v = vert.read(name, None);
        let k = "xyzw".find(comp).unwrap();
        if k >= v.len() {
            return not(format!("{atom}: no such component"));
        }
        let Some(sol) = linear(&v[k], &basis) else {
            return not(format!(
                "varying {atom} is not a linear function of gl_Position"
            ));
        };
        if !sol[4].is_zero() {
            return not(format!(
                "varying {atom} has a constant term (it does not interpolate as a position)"
            ));
        }
        let s = sol[..4].to_vec();
        lin.insert(atom.clone(), s.clone());
        Ok(s)
    };
    let mut in_p = |coefs: &[(Atom, Q)], vert: &mut Stage| -> R<Vec<Q>> {
        let mut out = vec![Q::zero(); 4];
        for (atom, c) in coefs {
            for (j, x) in varying(atom, vert)?.iter().enumerate() {
                out[j] += c * x;
            }
        }
        Ok(out)
    };
    let calls = frag.grab_calls.clone();
    for g in &calls {
        let f = &*g.f;
        let a = &g.args;
        let plain = f == "texture" && a.len() == 1 && a[0].len() >= 2;
        let lod0 = f == "textureLod"
            && a.len() == 2
            && a[0].len() >= 2
            && a[1][0].is_const()
            && a[1][0].value().is_zero();
        let uv: Vals = if plain || lod0 {
            a[0][..2].to_vec()
        } else if f == "textureProj" && a.len() == 1 && matches!(a[0].len(), 3 | 4) {
            let qv = a[0][a[0].len() - 1].clone();
            let i0 = frag.inv(&qv);
            let u = Rc::new(a[0][0].mul(&i0));
            let i1 = frag.inv(&qv);
            let v = Rc::new(a[0][1].mul(&i1));
            vec![u, v]
        } else {
            return not(format!(
                "{f}({GRAB_TEXTURE}, ...) with {} more arguments: not a plain sample",
                a.len()
            ));
        };
        for (axis, comp) in [(0usize, &uv[0]), (1, &uv[1])] {
            let ax = &"uv"[axis..axis + 1];
            let Some((num, den, c)) = ratio(comp, &frag) else {
                return not(format!(
                    "{f}({GRAB_TEXTURE}): its {ax} is not the fragment's screen position (a varying \
                     divided by another, plus a constant: an offset, a scale or another coordinate)"
                ));
            };
            let dsol = ratio_den(&den, &frag)?;
            let d = in_p(&dsol, &mut vert)?;
            let n = in_p(&num, &mut vert)?;
            if !d[0].is_zero() || !d[1].is_zero() || !d[2].is_zero() || d[3].is_zero() {
                return not(format!(
                    "{f}({GRAB_TEXTURE}): its {ax} is not divided by gl_Position.w"
                ));
            }
            let k = &d[3];
            let half = Q::new(BigInt::from(1), BigInt::from(2));
            if !n[1 - axis].is_zero()
                || !n[2].is_zero()
                || &n[axis] / k != half
                || &n[3] / k + &c != half
            {
                return not(format!(
                    "{f}({GRAB_TEXTURE}): its {ax} is not the fragment's screen position (0.5 * P.{} / P.w + 0.5)",
                    &"xy"[axis..axis + 1]
                ));
            }
        }
    }
    Ok(frag)
}

// ---------------------------------------------------------------- the program rewrite

static OUT_DECL: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(
        r"(?m)^(?P<pre>[ \t]*(?:layout\s*\(\s*location\s*=\s*0\s*\)\s*)?)out(?P<post>\s+(?:(?:highp|mediump|lowp)\s+)?vec4\s+(?P<name>\w+)\s*;)[ \t]*$",
    )
    .unwrap()
});
static ANY_OUT: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(
        r"(?m)^[ \t]*(?:layout\s*\([^)]*\)\s*)?(?:(?:flat|smooth|centroid|highp|mediump|lowp)\s+)*(?:in)?out\s",
    )
    .unwrap()
});
static VERSION: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^\s*#\s*version\b").unwrap());
static EXTENSION: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^#\s*extension\b").unwrap());
static IFDEF: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^#\s*ifdef\s+\w+$").unwrap());
static ENDIF: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^#\s*endif\b").unwrap());
static READS_GRAB: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(&format!(r"\b{GRAB_TEXTURE}\b")).unwrap());

const EXTENSIONS: &str = "#ifdef GL_EXT_shader_framebuffer_fetch\n\
#extension GL_EXT_shader_framebuffer_fetch : enable\n\
#elif defined(GL_ARM_shader_framebuffer_fetch)\n\
#extension GL_ARM_shader_framebuffer_fetch : enable\n\
#else\n\
#error framebuffer fetch: the driver has neither GL_EXT_shader_framebuffer_fetch nor GL_ARM_shader_framebuffer_fetch\n\
#endif\n";

/// Offset after a stage's #version line and the extension directives that follow it.
fn header_end(src: &str) -> R<usize> {
    let lines = py::splitlines(src);
    let (mut pos, mut i) = (0, 0);
    while i < lines.len() && py::strip(lines[i]).is_empty() {
        pos += lines[i].len();
        i += 1;
    }
    if i >= lines.len() || !VERSION.is_match(lines[i]) {
        return not("no #version line");
    }
    pos += lines[i].len();
    i += 1;
    while i < lines.len() {
        let s = py::strip(lines[i]);
        if EXTENSION.is_match(s) {
            pos += lines[i].len();
            i += 1;
        } else if IFDEF.is_match(s)
            && i + 2 < lines.len()
            && EXTENSION.is_match(py::strip(lines[i + 1]))
            && ENDIF.is_match(py::strip(lines[i + 2]))
        {
            pos += lines[i..i + 3].iter().map(|x| x.len()).sum::<usize>();
            i += 3;
        } else {
            break;
        }
    }
    Ok(pos)
}

/// The fragment stage reading the framebuffer: its one output `inout` where the driver has
/// GL_EXT_shader_framebuffer_fetch, else gl_LastFragColorARM; each sample becomes that colour.
fn rewrite_fragment(src: &str, frag: &Stage) -> R<String> {
    let outs = ANY_OUT.find_iter(&glsl::strip_comments(src)).count();
    let m: Vec<regex::Captures> = OUT_DECL.captures_iter(src).collect();
    if outs != 1 || m.len() != 1 {
        return not("the fragment stage does not have exactly one vec4 output at location 0");
    }
    let m = &m[0];
    if src.contains(FETCHED) {
        return not(format!("the fragment stage already names {FETCHED}"));
    }
    let all = m.get(0).unwrap();
    let out_name = &m["name"];
    let he = header_end(src)?;
    let mut edits: Vec<(usize, usize, String)> = vec![
        (he, he, format!("{MARK}\n{EXTENSIONS}")),
        (
            all.start(),
            all.end(),
            format!(
                "#ifdef GL_EXT_shader_framebuffer_fetch\n{}inout{}\n#else\n{}\n#endif",
                &m["pre"],
                &m["post"],
                all.as_str()
            ),
        ),
        (
            frag.body_start,
            frag.body_start,
            format!(
                "\n#ifdef GL_EXT_shader_framebuffer_fetch\n    vec4 {FETCHED} = {out_name};\n#else\n    vec4 {FETCHED} = gl_LastFragColorARM;\n#endif"
            ),
        ),
    ];
    for g in &frag.grab_calls {
        edits.push((g.start, g.end, FETCHED.to_string()));
    }
    edits.sort_by(|x, y| (y.0, y.1).cmp(&(x.0, x.1))); // (stable: as sorted(..., reverse=True))
    let mut out = src.to_string();
    for (a, b, text) in edits {
        out = format!("{}{}{}", &out[..a], text, &out[b..]);
    }
    Ok(out)
}

/// A GLES3 program (every stage block) with its fragment stage reading the framebuffer instead
/// of _GrabTexture; Fail::Not (why) where it does not sample it at its screen position.
pub fn rewrite_program(code: &str) -> Result<String, Fail> {
    let mut blocks: BTreeMap<String, (usize, usize)> = BTreeMap::new();
    for (stage, a, b) in glsl::stage_blocks(code) {
        blocks.insert(stage, (a, b));
    }
    if blocks.len() != 2 || !blocks.contains_key("VERTEX") || !blocks.contains_key("FRAGMENT") {
        return not("not a vertex + fragment program");
    }
    let (va, vb) = blocks["VERTEX"];
    let (fa, fb) = blocks["FRAGMENT"];
    let frag = screen_uv(&code[va..vb], &code[fa..fb])?;
    Ok(format!(
        "{}{}{}",
        &code[..fa],
        rewrite_fragment(&code[fa..fb], &frag)?,
        &code[fb..]
    ))
}

/// Whether a program's fragment stage names _GrabTexture (outside comments).
pub fn reads_grab(code: &str) -> bool {
    let mut frag = None;
    for (stage, a, b) in glsl::stage_blocks(code) {
        if stage == "FRAGMENT" {
            frag = Some(&code[a..b]);
        }
    }
    READS_GRAB.is_match(&glsl::strip_comments(frag.unwrap_or("")))
}

#[cfg(test)]
mod tests {
    use super::*;

    const VS: &str = "#version 300 es\n\
uniform vec4 hlslcc_mtx4x4unity_ObjectToWorld[4];\n\
uniform vec4 hlslcc_mtx4x4unity_MatrixVP[4];\n\
in highp vec4 in_POSITION0;\n\
out highp vec4 vs_TEXCOORD0;\n\
vec4 u_xlat0;\n\
vec4 u_xlat1;\n\
void main()\n\
{\n\
    u_xlat0 = in_POSITION0.yyyy * hlslcc_mtx4x4unity_ObjectToWorld[1];\n\
    u_xlat0 = hlslcc_mtx4x4unity_ObjectToWorld[0] * in_POSITION0.xxxx + u_xlat0;\n\
    u_xlat0 = u_xlat0 + hlslcc_mtx4x4unity_ObjectToWorld[3];\n\
    u_xlat1 = u_xlat0.yyyy * hlslcc_mtx4x4unity_MatrixVP[1];\n\
    u_xlat1 = hlslcc_mtx4x4unity_MatrixVP[0] * u_xlat0.xxxx + u_xlat1;\n\
    u_xlat0 = hlslcc_mtx4x4unity_MatrixVP[3] * u_xlat0.wwww + u_xlat1;\n\
    gl_Position = u_xlat0;\n\
    u_xlat0.y = u_xlat0.y * _ProjectionParams.x;\n\
    u_xlat1.xzw = u_xlat0.xwy * vec3(0.5, 0.5, 0.5);\n\
    vs_TEXCOORD0.zw = u_xlat0.zw;\n\
    vs_TEXCOORD0.xy = u_xlat1.zz + u_xlat1.xw;\n\
    return;\n\
}\n";

    fn fs(uv: &str) -> String {
        format!(
            "#version 300 es\n\
precision highp float;\n\
uniform mediump sampler2D _GrabTexture;\n\
in highp vec4 vs_TEXCOORD0;\n\
layout(location = 0) out mediump vec4 SV_Target0;\n\
vec2 u_xlat0;\n\
void main()\n\
{{\n\
    u_xlat0.xy = {uv};\n\
    SV_Target0 = texture(_GrabTexture, u_xlat0.xy);\n\
    return;\n\
}}\n"
        )
    }

    fn program(vs: &str, fs: &str) -> String {
        format!("#ifdef VERTEX\n{vs}#endif\n#ifdef FRAGMENT\n{fs}#endif\n")
    }

    #[test]
    fn polynomials() {
        let x = Poly::atom("x");
        let y = Poly::atom("y");
        let p = x.add(&y).mul(&x.sub(&y));
        assert_eq!(p.key(), "1*x*x + -1*y*y");
        let h = Poly::constant(fraction("0.5").unwrap());
        assert_eq!(h.key(), "1/2*1");
        assert_eq!(Poly::default().key(), "0");
        assert_eq!(
            q_str(&fraction("9.99999975e-05").unwrap()),
            "39999999/400000000000"
        );
        assert_eq!(q_str(&fraction("2.").unwrap()), "2");
        assert_eq!(q_str(&fraction(".25").unwrap()), "1/4");
        assert_eq!(q_str(&fraction("1e3").unwrap()), "1000");
    }

    #[test]
    fn screen_position() {
        // ComputeGrabScreenPos on OpenGL without the y flip: (P.xy * 0.5 + P.w * 0.5) / P.w
        let vs = VS.replace("u_xlat0.y = u_xlat0.y * _ProjectionParams.x;\n", "");
        let code = program(&vs, &fs("vs_TEXCOORD0.xy / vs_TEXCOORD0.ww"));
        assert!(reads_grab(&code));
        let out = rewrite_program(&code).unwrap();
        assert!(out.contains(MARK));
        assert!(out.contains("SV_Target0 = ua_FramebufferColor;"));
        assert!(out.contains("inout mediump vec4 SV_Target0;"));
        assert!(out.contains("vec4 ua_FramebufferColor = gl_LastFragColorARM;"));
        // an offset is refused, with the Python's reason
        let code = program(
            &vs,
            &fs("vs_TEXCOORD0.xy / vs_TEXCOORD0.ww + vec2(0.01, 0.0)"),
        );
        assert_eq!(
            rewrite_program(&code),
            Err(Fail::Not(
                "texture(_GrabTexture): its u is not the fragment's screen position (0.5 * P.x / P.w + 0.5)"
                    .into()
            ))
        );
        // the y flip by a uniform: not a linear function of gl_Position
        let code = program(VS, &fs("vs_TEXCOORD0.xy / vs_TEXCOORD0.ww"));
        assert_eq!(
            rewrite_program(&code),
            Err(Fail::Not(
                "varying vs_TEXCOORD0.y is not a linear function of gl_Position".into()
            ))
        );
        // not divided at all
        let code = program(&vs, &fs("vs_TEXCOORD0.xy"));
        assert!(
            matches!(rewrite_program(&code), Err(Fail::Not(m)) if m.contains("its u is not the fragment's screen position (a varying"))
        );
    }

    #[test]
    fn tokens_and_errors() {
        let s = "a += 0x1Fu; // c\n#if X\n1.5e3f";
        let t = tokens(s, 0, s.len()).unwrap();
        let k: Vec<(Kind, &str)> = t.iter().map(|t| (t.kind, &*t.text)).collect();
        assert_eq!(
            k,
            [
                (Kind::Id, "a"),
                (Kind::Op, "+="),
                (Kind::Num, "0x1Fu"),
                (Kind::Op, ";"),
                (Kind::PpIf, "#if X"),
                (Kind::Num, "1.5e3f"),
            ]
        );
        assert_eq!(
            tokens("a @ b", 0, 5).unwrap_err(),
            Fail::Not("cannot read the GLSL at '@ b'".into())
        );
        assert_eq!(
            rewrite_program("#ifdef VERTEX\n#version 300 es\nvoid main() {}\n#endif\n"),
            Err(Fail::Not("not a vertex + fragment program".into()))
        );
    }
}
