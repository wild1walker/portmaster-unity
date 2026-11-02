//! Metal programs (MSL text) of Unity Shader assets -> GLSL ES, as shaders/metal.py translates
//! them (its docstring has the subset): Unity's MSL is HLSLcc's toMetal output, read here token by
//! token with the types its expressions have, and written as HLSLcc's GL form of the same program.
use crate::glsl;
use crate::vulkan;
use regex::Regex;
use std::collections::{BTreeMap, BTreeSet};
use std::sync::LazyLock;

/// Why a program has no GL twin (metal.Unsupported), or a fault the Python does not catch.
#[derive(Clone, Debug)]
pub enum Fault {
    Uns(String),
    /// glsl.NeedsES31 out of the ES check (retried as GLSL ES 3.10)
    Es31(String),
    Crash(String),
}

type R<T> = Result<T, Fault>;

fn uns<T>(m: impl Into<String>) -> R<T> {
    Err(Fault::Uns(m.into()))
}

fn crash<T>(m: impl Into<String>) -> R<T> {
    Err(Fault::Crash(m.into()))
}

fn re(p: &str) -> Regex {
    glsl::re(p)
}

// ---------------------------------------------------------------- tokens

static TOKEN: LazyLock<Regex> = LazyLock::new(|| {
    re(concat!(
        r"\A(?:(?P<ws>\s+)|",
        r"(?P<num>0[xX][0-9a-fA-F]+[uU]?|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?[fFhHuU]?)|",
        r"(?P<id>[A-Za-z_][A-Za-z_0-9]*(?:::[A-Za-z_][A-Za-z_0-9]*)*)|",
        r"(?P<op>\[\[|\]\]|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||\+\+|--|\+=|-=|\*=|/=|%=|&=|\|=|\^=|->|[-+*/%&|^~!<>=?:;,.(){}\[\]#]))"
    ))
});

pub fn tokenize(src: &str) -> R<Vec<String>> {
    let mut out = Vec::new();
    let mut pos = 0;
    while pos < src.len() {
        let Some(c) = TOKEN.captures(&src[pos..]) else {
            let end = src[pos..]
                .char_indices()
                .nth(20)
                .map_or(src.len(), |(i, _)| pos + i);
            return uns(format!(
                "MSL: cannot read {}",
                crate::py::repr(&src[pos..end])
            ));
        };
        let m = c.get(0).unwrap();
        if m.end() == 0 {
            return uns("MSL: cannot read");
        }
        pos += m.end();
        if c.name("ws").is_none() {
            out.push(m.as_str().to_string());
        }
    }
    Ok(out)
}

static PRE: LazyLock<(Regex, Regex, Regex, Regex, Regex)> = LazyLock::new(|| {
    (
        re(r"(?s)//[^\n]*|/\*.*?\*/"),
        re(r"\A#\s*if\b"),
        re(r"\A#\s*ifn?def\b"),
        re(r"\A#\s*else\b"),
        re(r"\A#\s*endif\b"),
    )
});

/// Comments out, `#if __METAL_VERSION__ >= n` blocks dropped, other preprocessor lines dropped.
pub fn preprocess(src: &str) -> String {
    let (com, if_, ifdef, else_, endif) = &*PRE;
    let src = com.replace_all(src, " ");
    let mut out = Vec::new();
    let mut skip: Vec<bool> = Vec::new();
    for line in src.split('\n') {
        let s = crate::py::strip(line);
        if s.starts_with('#') {
            if if_.is_match(s) {
                skip.push(s.contains("__METAL_VERSION__"));
            } else if ifdef.is_match(s) {
                skip.push(false);
            } else if else_.is_match(s) {
                if let Some(last) = skip.last_mut() {
                    if s.contains("__METAL") || *last {
                        *last = !*last;
                    }
                }
            } else if endif.is_match(s) {
                skip.pop();
            }
            continue;
        }
        if skip.iter().any(|&x| x) {
            continue;
        }
        out.push(line);
    }
    out.join("\n")
}

// ---------------------------------------------------------------- types

/// A value type (metal.T).
#[derive(Clone, Debug)]
pub struct T {
    kind: &'static str,
    base: &'static str,
    n: u32,
    rows: u32,
    elem: Option<Box<T>>,
    length: i64,
    name: String,
}

impl PartialEq for T {
    fn eq(&self, o: &T) -> bool {
        (
            self.kind,
            self.base,
            self.n,
            self.rows,
            &self.name,
            self.length,
        ) == (o.kind, o.base, o.n, o.rows, &o.name, o.length)
            && self.elem == o.elem
    }
}

impl T {
    fn new(kind: &'static str) -> T {
        T {
            kind,
            base: "",
            n: 1,
            rows: 0,
            elem: None,
            length: 0,
            name: String::new(),
        }
    }

    fn named(kind: &'static str, name: &str) -> T {
        T {
            name: name.to_string(),
            ..T::new(kind)
        }
    }

    fn array(elem: T, length: i64) -> T {
        T {
            elem: Some(Box::new(elem)),
            length,
            ..T::new("array")
        }
    }

    fn glsl(&self) -> String {
        match self.kind {
            "vec" => {
                if self.n == 1 {
                    return self.base.to_string();
                }
                let p = match self.base {
                    "float" => "",
                    "int" => "i",
                    "uint" => "u",
                    _ => "b",
                };
                format!("{p}vec{}", self.n)
            }
            "mat" => {
                if self.n == self.rows {
                    format!("mat{}", self.n)
                } else {
                    format!("mat{}x{}", self.n, self.rows)
                }
            }
            "array" => self.elem.as_ref().unwrap().glsl(),
            k => {
                if self.name.is_empty() {
                    k.to_string()
                } else {
                    self.name.clone()
                }
            }
        }
    }
}

fn vec(base: &'static str, n: u32) -> T {
    T {
        base,
        n,
        ..T::new("vec")
    }
}

fn float() -> T {
    vec("float", 1)
}
fn int() -> T {
    vec("int", 1)
}
fn uint() -> T {
    vec("uint", 1)
}
fn boolean() -> T {
    vec("bool", 1)
}

static TYPE_RE: LazyLock<Regex> = LazyLock::new(|| {
    re(
        r"\A(float|half|int|uint|bool|short|ushort|uint32_t|int32_t|char|uchar)([234])?(?:x([234]))?\n?\z",
    )
});

fn base_of(n: &str) -> &'static str {
    match n {
        "float" | "half" => "float",
        "int" | "short" | "int32_t" | "char" => "int",
        "uint" | "ushort" | "uint32_t" | "uchar" => "uint",
        _ => "bool",
    }
}

/// A scalar / vector / matrix type name -> T.
fn parse_type(name: &str) -> Option<T> {
    let name = name.replace("metal::", "");
    let m = TYPE_RE.captures(&name)?;
    let base = base_of(&m[1]);
    let n: u32 = m.get(2).map_or(1, |x| x.as_str().parse().unwrap());
    if let Some(r) = m.get(3) {
        return Some(T {
            base: "float",
            n,
            rows: r.as_str().parse().unwrap(),
            ..T::new("mat")
        });
    }
    Some(vec(base, n))
}

fn tex_of(kind: &str) -> Option<&'static str> {
    Some(match kind {
        "texture2d" => "sampler2D",
        "texture3d" => "sampler3D",
        "texturecube" => "samplerCube",
        "texture2d_array" => "sampler2DArray",
        "depth2d" => "sampler2DShadow",
        "depthcube" => "samplerCubeShadow",
        "depth2d_array" => "sampler2DArrayShadow",
        "texture2d_ms" => "sampler2DMS",
        _ => return None,
    })
}

// ---------------------------------------------------------------- expressions

/// An expression: GLSL text, type, precedence (metal.E).
#[derive(Clone, Debug)]
struct E {
    s: String,
    t: T,
    p: i32,
}

fn e(s: impl Into<String>, t: T) -> E {
    E {
        s: s.into(),
        t,
        p: 100,
    }
}

fn ep(s: impl Into<String>, t: T, p: i32) -> E {
    E { s: s.into(), t, p }
}

impl E {
    fn wrap(&self, p: i32) -> String {
        if self.p >= p {
            self.s.clone()
        } else {
            format!("({})", self.s)
        }
    }
}

fn prec(op: &str) -> Option<i32> {
    Some(match op {
        "||" => 10,
        "&&" => 11,
        "|" => 12,
        "^" => 13,
        "&" => 14,
        "==" | "!=" => 15,
        "<" | "<=" | ">" | ">=" => 16,
        "<<" | ">>" => 17,
        "+" | "-" => 18,
        "*" | "/" | "%" => 19,
        _ => return None,
    })
}

fn cmp_fn(op: &str) -> Option<&'static str> {
    Some(match op {
        "<" => "lessThan",
        "<=" => "lessThanEqual",
        ">" => "greaterThan",
        ">=" => "greaterThanEqual",
        "==" => "equal",
        "!=" => "notEqual",
        _ => return None,
    })
}

const FUNCS_SAME: [&str; 34] = [
    "abs",
    "floor",
    "ceil",
    "fract",
    "sqrt",
    "exp2",
    "log2",
    "exp",
    "log",
    "sin",
    "cos",
    "tan",
    "asin",
    "acos",
    "trunc",
    "sign",
    "normalize",
    "min",
    "max",
    "clamp",
    "mix",
    "step",
    "smoothstep",
    "pow",
    "reflect",
    "refract",
    "cross",
    "atan",
    "sinh",
    "cosh",
    "tanh",
    "radians",
    "degrees",
    "abs",
];

fn renamed(n: &str) -> Option<&'static str> {
    Some(match n {
        "rsqrt" => "inversesqrt",
        "rint" => "roundEven",
        "round" => "round",
        "dfdx" => "dFdx",
        "dfdy" => "dFdy",
        "fwidth" => "fwidth",
        "atan2" => "atan",
        "isnan" => "isnan",
        "isinf" => "isinf",
        "all" => "all",
        "any" => "any",
        _ => return None,
    })
}

struct Scope {
    vars: Vec<BTreeMap<String, (String, T)>>,
}

impl Scope {
    fn get(&self, name: &str) -> Option<(String, T)> {
        self.vars.iter().rev().find_map(|m| m.get(name).cloned())
    }

    fn set(&mut self, name: &str, v: (String, T)) {
        self.vars.last_mut().unwrap().insert(name.to_string(), v);
    }

    fn push(&mut self) {
        self.vars.push(BTreeMap::new());
    }

    fn pop(&mut self) {
        self.vars.pop();
    }
}

/// One stage being translated (metal.Stage).
#[derive(Default)]
struct Stage {
    stage: Option<&'static str>,
    uniforms: Vec<String>,
    ins: Vec<String>,
    outs: Vec<String>,
    consts: Vec<String>,
    helpers: BTreeSet<String>,
    structs: BTreeMap<String, Vec<(String, T, Vec<String>)>>,
    cbuffers: BTreeMap<String, String>,
    textures: BTreeMap<String, (&'static str, String)>,
    globals: BTreeMap<String, (String, T)>,
    io: BTreeMap<(String, String), (String, T)>,
    fconst: BTreeMap<String, String>,
    needs_es31: Option<&'static str>,
}

struct Parser<'a> {
    t: Vec<String>,
    i: usize,
    st: &'a mut Stage,
}

static LIT_START: LazyLock<Regex> = LazyLock::new(|| re(r"\A(0[xX][0-9a-fA-F]+|\d|\.\d)"));
static IDENT_START: LazyLock<Regex> = LazyLock::new(|| re(r"\A[A-Za-z_]"));
static DIGITS: LazyLock<Regex> = LazyLock::new(|| re(r"\A\d+\n?\z"));
static SWZ: LazyLock<Regex> = LazyLock::new(|| re(r"\A[xyzw]{1,4}\n?\z|\A[rgba]{1,4}\n?\z"));
static XR: LazyLock<Regex> = LazyLock::new(|| re(r"\A[xr]+\n?\z"));
static WS: LazyLock<Regex> = LazyLock::new(|| re(r"\s"));

fn py_int0(s: &str) -> R<i64> {
    // int(s, 0)
    let t = s.trim();
    let r = if let Some(h) = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        i64::from_str_radix(h, 16).ok()
    } else if t.len() > 1 && t.starts_with('0') && !t.bytes().all(|b| b == b'0') {
        None
    } else {
        t.parse().ok()
    };
    r.ok_or_else(|| {
        Fault::Crash(format!(
            "invalid literal for int() with base 0: {}",
            crate::py::repr(s)
        ))
    })
}

impl Parser<'_> {
    fn peek(&self, k: usize) -> Option<&str> {
        self.t.get(self.i + k).map(String::as_str)
    }

    fn next(&mut self) -> R<String> {
        match self.t.get(self.i) {
            None => uns("MSL: unexpected end"),
            Some(x) => {
                self.i += 1;
                Ok(x.clone())
            }
        }
    }

    fn expect(&mut self, tok: &str) -> R<()> {
        let got = self.next()?;
        if got != tok {
            let a = self.i.saturating_sub(8);
            let b = (self.i + 4).min(self.t.len());
            return uns(format!(
                "MSL: expected {}, got {} near {}",
                crate::py::repr(tok),
                crate::py::repr(&got),
                self.t[a..b].join(" ")
            ));
        }
        Ok(())
    }

    fn accept(&mut self, tok: &str) -> bool {
        if self.peek(0) == Some(tok) {
            self.i += 1;
            true
        } else {
            false
        }
    }

    fn unary(&mut self, sc: &mut Scope) -> R<E> {
        if matches!(self.peek(0), Some("-") | Some("!") | Some("~") | Some("+")) {
            let op = self.next()?;
            let a = self.unary(sc)?;
            if op == "!" && a.t.kind == "vec" && a.t.n > 1 {
                return Ok(e(format!("not({})", a.s), a.t));
            }
            if op == "!" && a.t.base != "bool" {
                return Ok(ep(format!("!{}", bool_of(&a)?.wrap(90)), boolean(), 90));
            }
            if op == "+" {
                return Ok(a);
            }
            return Ok(ep(format!("{op}{}", a.wrap(90)), a.t.clone(), 90));
        }
        self.postfix(sc)
    }

    fn expr(&mut self, sc: &mut Scope, min_prec: i32) -> R<E> {
        let mut a = self.unary(sc)?;
        loop {
            let op = self.peek(0).map(String::from);
            if op.as_deref() == Some("?") && min_prec <= 5 {
                self.next()?;
                let b = self.expr(sc, 0)?;
                self.expect(":")?;
                let c = self.expr(sc, 5)?;
                if a.t.kind == "vec" && a.t.n > 1 {
                    return uns("MSL: ?: with a vector condition");
                }
                let t = if b.t == c.t {
                    b.t.clone()
                } else {
                    unify(&b.t, &c.t)
                };
                a = ep(
                    format!(
                        "{} ? {} : {}",
                        bool_of(&a)?.wrap(6),
                        convert(&b, &t)?.wrap(6),
                        convert(&c, &t)?.wrap(5)
                    ),
                    t,
                    5,
                );
                continue;
            }
            let Some(opv) = op else { return Ok(a) };
            let Some(p) = prec(&opv) else { return Ok(a) };
            if p < min_prec || ((opv == "<" || opv == ">") && self.peek(1) == Some(">")) {
                return Ok(a);
            }
            self.next()?;
            let b = self.expr(sc, p + 1)?;
            a = binary(&opv, &a, &b)?;
        }
    }

    fn postfix(&mut self, sc: &mut Scope) -> R<E> {
        let mut a = self.primary(sc)?;
        loop {
            match self.peek(0) {
                Some(".") => {
                    self.next()?;
                    let name = self.next()?;
                    if self.peek(0) == Some("(") {
                        a = self.method(sc, &a, &name)?;
                        continue;
                    }
                    a = member(self.st, &a, &name)?;
                }
                Some("[") => {
                    self.next()?;
                    let mut idx = self.expr(sc, 0)?;
                    self.expect("]")?;
                    if idx.t.kind == "vec" && idx.t.base == "uint" {
                        idx = e(format!("int({})", idx.s), int());
                    }
                    a = match a.t.kind {
                        "array" => e(
                            format!("{}[{}]", a.wrap(100), idx.s),
                            (**a.t.elem.as_ref().unwrap()).clone(),
                        ),
                        "mat" => e(
                            format!("{}[{}]", a.wrap(100), idx.s),
                            vec("float", a.t.rows),
                        ),
                        "vec" if a.t.n > 1 => {
                            e(format!("{}[{}]", a.wrap(100), idx.s), vec(a.t.base, 1))
                        }
                        _ => return uns(format!("MSL: indexing {}", a.t.glsl())),
                    };
                }
                Some(tok @ ("++" | "--")) => {
                    let tok = tok.to_string();
                    self.next()?;
                    a = ep(format!("{}{tok}", a.s), a.t.clone(), 95);
                }
                _ => return Ok(a),
            }
        }
    }

    fn args(&mut self, sc: &mut Scope) -> R<Vec<E>> {
        self.expect("(")?;
        let mut out = Vec::new();
        if self.accept(")") {
            return Ok(out);
        }
        loop {
            out.push(self.expr(sc, 0)?);
            if self.accept(")") {
                return Ok(out);
            }
            self.expect(",")?;
        }
    }

    fn primary(&mut self, sc: &mut Scope) -> R<E> {
        let tok = self.next()?;
        if tok == "(" {
            let x = self.expr(sc, 0)?;
            self.expect(")")?;
            return Ok(x);
        }
        if LIT_START.is_match(&tok) {
            return literal(&tok);
        }
        if tok == "true" || tok == "false" {
            return Ok(e(tok, boolean()));
        }
        if let Some(c) = tok.strip_prefix("component::") {
            if let Some(k) = "xyzw".find(c).filter(|_| c.len() == 1) {
                return Ok(e(format!("component:{k}"), T::new("opt")));
            }
        }
        if tok == "as_type" {
            self.expect("<")?;
            let t = parse_type(&self.next()?);
            self.expect(">")?;
            let a = self.args(sc)?;
            if a.len() != 1 {
                return crash("ValueError: as_type takes one value");
            }
            let Some(t) = t else {
                return crash("AttributeError: as_type of a type that is not one");
            };
            return bitcast(&a[0], &t);
        }
        if let Some(t) = parse_type(&tok) {
            if self.peek(0) == Some("(") {
                let a = self.args(sc)?;
                return construct(&t, &a);
            }
        }
        if let Some(v) = self.st.fconst.get(&tok).cloned() {
            if self.peek(0) != Some("(") {
                let t = if v == "true" || v == "false" {
                    boolean()
                } else {
                    uint()
                };
                return Ok(e(v, t));
            }
        }
        if self.peek(0) == Some("(") {
            let a = self.args(sc)?;
            return call(self.st, &tok, a);
        }
        match sc.get(&tok) {
            None => uns(format!("MSL: unknown name {}", crate::py::repr(&tok))),
            Some((s, t)) => Ok(e(s, t)),
        }
    }

    fn method(&mut self, sc: &mut Scope, obj: &E, name: &str) -> R<E> {
        let Some((gl, _kind)) = self.st.textures.get(&obj.s).cloned() else {
            return uns(format!("MSL: method {name} on {}", obj.s));
        };
        let all = self.args(sc)?;
        let opts: Vec<E> = all.iter().filter(|a| a.t.kind == "opt").cloned().collect();
        let args: Vec<E> = all.iter().filter(|a| a.t.kind != "opt").cloned().collect();
        let shadow = gl.contains("Shadow");
        let ms = gl.contains("MS");
        let dims = match gl {
            "sampler2D" | "sampler2DShadow" | "sampler2DMS" => 2,
            _ => 3,
        };
        let rt = if shadow && name != "gather" {
            float()
        } else {
            vec("float", 4)
        };
        let opt = |pre: &str| opts.iter().find(|o| o.s.starts_with(pre)).cloned();
        if name == "sample" || name == "sample_compare" {
            if args.len() < 2 {
                return uns("MSL: sample without coordinates");
            }
            let mut coord = args[1].clone();
            let mut args = args.clone();
            if gl == "sampler2DArray" || gl == "sampler2DArrayShadow" {
                if args.len() < 3 {
                    return uns("MSL: array sample without a layer");
                }
                coord = e(
                    format!(
                        "vec3({}, {})",
                        convert(&coord, &vec("float", 2))?.s,
                        convert(&args[2], &float())?.s
                    ),
                    vec("float", 3),
                );
                let rest: Vec<E> = args[3..].to_vec();
                args.truncate(2);
                args.extend(rest);
            }
            coord = convert(&coord, &vec("float", dims))?;
            let name_gl = if shadow {
                format!("hlslcc_zcmp{}", obj.s)
            } else {
                obj.s.clone()
            };
            if name == "sample_compare" {
                if !shadow {
                    return uns("MSL: sample_compare on a texture without depth");
                }
                let Some(r) = args.get(2) else {
                    return crash("IndexError: list index out of range");
                };
                let r = convert(r, &float())?;
                coord = e(
                    format!("vec{}({}, {})", dims + 1, coord.s, r.s),
                    vec("float", dims + 1),
                );
                if gl == "samplerCubeShadow" || gl == "sampler2DArrayShadow" {
                    coord = e(coord.s, vec("float", 4));
                }
            } else if shadow {
                return uns("MSL: sample (not compare) of a depth texture");
            }
            if let Some(lod) = opt("level:") {
                if gl == "samplerCubeShadow" || gl == "sampler2DArrayShadow" {
                    return Ok(e(format!("texture({name_gl}, {})", coord.s), rt));
                }
                return Ok(e(
                    format!("textureLod({name_gl}, {}, {})", coord.s, &lod.s[6..]),
                    rt,
                ));
            }
            if let Some(g) = opt("gradient:") {
                return Ok(e(
                    format!("textureGrad({name_gl}, {}, {})", coord.s, &g.s[9..]),
                    rt,
                ));
            }
            if let Some(b) = opt("bias:") {
                return Ok(e(
                    format!("texture({name_gl}, {}, {})", coord.s, &b.s[5..]),
                    rt,
                ));
            }
            return Ok(e(format!("texture({name_gl}, {})", coord.s), rt));
        }
        if name == "read" {
            if args.is_empty() {
                return uns("MSL: read without coordinates");
            }
            let n = match gl {
                "sampler2D" | "sampler2DMS" => 2,
                "sampler3D" | "sampler2DArray" => 3,
                _ => return uns(format!("MSL: read of {gl}")),
            };
            let _ = n;
            let mut c = convert(&args[0], &vec("int", if gl != "sampler3D" { 2 } else { 3 }))?;
            let mut rest: Vec<E> = args[1..].to_vec();
            if gl == "sampler2DArray" {
                let Some(l) = rest.first() else {
                    return crash("IndexError: list index out of range");
                };
                c = e(
                    format!("ivec3({}, {})", c.s, convert(l, &int())?.s),
                    vec("int", 3),
                );
                rest.remove(0);
            }
            let lod = match rest.first() {
                Some(x) => convert(x, &int())?.s,
                None => "0".into(),
            };
            if ms {
                self.st.needs_es31 = Some("multisampled texture");
            }
            return Ok(e(
                format!("texelFetch({}, {}, {lod})", obj.s, c.s),
                vec("float", 4),
            ));
        }
        if name == "gather" {
            self.st.needs_es31 = Some("textureGather");
            let comp = opt("component:");
            let Some(a1) = args.get(1) else {
                return crash("IndexError: list index out of range");
            };
            let coord = convert(a1, &vec("float", dims))?;
            let c = comp.map_or(String::new(), |c| format!(", {}", &c.s[10..]));
            if let Some(off) = args.get(2) {
                let flat = WS.replace_all(&off.s, "");
                if flat != "ivec2(0)" && flat != "ivec2(0,0)" {
                    return Ok(e(
                        format!(
                            "textureGatherOffset({}, {}, {}{c})",
                            obj.s,
                            coord.s,
                            convert(off, &vec("int", 2))?.s
                        ),
                        vec("float", 4),
                    ));
                }
            }
            return Ok(e(
                format!("textureGather({}, {}{c})", obj.s, coord.s),
                vec("float", 4),
            ));
        }
        if let Some(k) = match name {
            "get_width" => Some("x"),
            "get_height" => Some("y"),
            "get_depth" => Some("z"),
            _ => None,
        } {
            let lod = match args.first() {
                Some(a) => convert(a, &int())?.s,
                None => "0".into(),
            };
            return Ok(e(
                format!("uint(textureSize({}, {lod}).{k})", obj.s),
                uint(),
            ));
        }
        uns(format!("MSL: texture method {name}"))
    }

    // ---- statements
    fn block(&mut self, sc: &mut Scope, out: &mut Vec<String>, ind: usize) -> R<()> {
        self.expect("{")?;
        sc.push();
        let r = (|| {
            while !self.accept("}") {
                self.statement(sc, out, ind)?;
            }
            Ok(())
        })();
        sc.pop();
        r
    }

    fn has_else(&self) -> bool {
        if self.peek(0) != Some("{") {
            return false;
        }
        let (mut j, mut depth) = (self.i, 0i64);
        while j < self.t.len() {
            depth += (self.t[j] == "{") as i64;
            depth -= (self.t[j] == "}") as i64;
            j += 1;
            if depth == 0 {
                break;
            }
        }
        j < self.t.len() && self.t[j] == "else"
    }

    fn body(&mut self, sc: &mut Scope, out: &mut Vec<String>, ind: usize) -> R<()> {
        if self.peek(0) == Some("{") {
            self.block(sc, out, ind + 1)
        } else {
            sc.push();
            let r = self.statement(sc, out, ind + 1);
            sc.pop();
            r
        }
    }

    fn statement(&mut self, sc: &mut Scope, out: &mut Vec<String>, ind: usize) -> R<()> {
        let tok = self.peek(0).map(String::from);
        let pad = "    ".repeat(ind);
        match tok.as_deref() {
            Some("{") => {
                out.push(pad.clone() + "{");
                self.block(sc, out, ind + 1)?;
                out.push(pad + "}");
                Ok(())
            }
            Some(";") => {
                self.next()?;
                Ok(())
            }
            Some("if") => {
                self.next()?;
                self.expect("(")?;
                let c = self.expr(sc, 0)?;
                self.expect(")")?;
                if c.s == "false" && (self.peek(0) != Some("{") || !self.has_else()) {
                    let mut sink = Vec::new();
                    return self.body(sc, &mut sink, ind);
                }
                out.push(format!("{pad}if({}){{", bool_of(&c)?.s));
                self.body(sc, out, ind)?;
                while self.peek(0) == Some("else") {
                    self.next()?;
                    if self.peek(0) == Some("if") {
                        self.next()?;
                        self.expect("(")?;
                        let c = self.expr(sc, 0)?;
                        self.expect(")")?;
                        out.push(format!("{pad}}} else if({}){{", bool_of(&c)?.s));
                        self.body(sc, out, ind)?;
                    } else {
                        out.push(format!("{pad}}} else {{"));
                        self.body(sc, out, ind)?;
                    }
                }
                out.push(pad + "}");
                Ok(())
            }
            Some("while") => {
                self.next()?;
                self.expect("(")?;
                let c = self.expr(sc, 0)?;
                self.expect(")")?;
                out.push(format!("{pad}while({}){{", bool_of(&c)?.s));
                self.body(sc, out, ind)?;
                out.push(pad + "}");
                Ok(())
            }
            Some("for") => {
                self.next()?;
                self.expect("(")?;
                sc.push();
                let r = (|| -> R<()> {
                    let mut init = Vec::new();
                    if !self.accept(";") {
                        self.simple(sc, &mut init, 0)?;
                        self.expect(";")?;
                    }
                    let cond = if self.peek(0) == Some(";") {
                        String::new()
                    } else {
                        bool_of(&self.expr(sc, 0)?)?.s
                    };
                    self.expect(";")?;
                    let step = if self.peek(0) == Some(")") {
                        String::new()
                    } else {
                        self.assign_expr(sc)?
                    };
                    self.expect(")")?;
                    let i0 = init
                        .first()
                        .map_or(String::new(), |x| crate::py::strip(x).to_string());
                    out.push(format!("{pad}for({i0} ; {cond} ; {step}){{"));
                    self.body(sc, out, ind)?;
                    out.push(pad.clone() + "}");
                    Ok(())
                })();
                sc.pop();
                r
            }
            Some("switch") => {
                self.next()?;
                self.expect("(")?;
                let c = convert(&self.expr(sc, 0)?, &int())?;
                self.expect(")")?;
                out.push(format!("{pad}switch({}){{", c.s));
                self.expect("{")?;
                sc.push();
                let r = (|| -> R<()> {
                    while !self.accept("}") {
                        if self.accept("case") {
                            let v = self.expr(sc, 0)?;
                            self.expect(":")?;
                            out.push(format!("{pad}    case {}:", convert(&v, &int())?.s));
                        } else if self.accept("default") {
                            self.expect(":")?;
                            out.push(format!("{pad}    default:"));
                        } else {
                            self.statement(sc, out, ind + 2)?;
                        }
                    }
                    Ok(())
                })();
                sc.pop();
                r?;
                out.push(pad + "}");
                Ok(())
            }
            Some(t @ ("break" | "continue")) => {
                let t = t.to_string();
                self.next()?;
                self.expect(";")?;
                out.push(format!("{pad}{t};"));
                Ok(())
            }
            Some("return") => {
                self.next()?;
                if !self.accept(";") {
                    self.expr(sc, 0)?;
                    self.expect(";")?;
                }
                out.push(format!("{pad}return;"));
                Ok(())
            }
            Some("discard_fragment") => {
                self.next()?;
                self.expect("(")?;
                self.expect(")")?;
                self.expect(";")?;
                out.push(format!("{pad}discard;"));
                Ok(())
            }
            Some("constexpr") => {
                self.next()?;
                self.next()?;
                let n = self.next()?;
                sc.set(&n, ("sampler".into(), T::new("sampler")));
                while self.next()? != ";" {}
                Ok(())
            }
            _ => {
                let mut line = Vec::new();
                self.simple(sc, &mut line, ind)?;
                self.expect(";")?;
                out.extend(line.into_iter().map(|l| l + ";"));
                Ok(())
            }
        }
    }

    /// A declaration or an expression statement (without its ';').
    fn simple(&mut self, sc: &mut Scope, out: &mut Vec<String>, ind: usize) -> R<()> {
        let pad = "    ".repeat(ind);
        let mut quals = Vec::new();
        while matches!(
            self.peek(0),
            Some("const") | Some("thread") | Some("constant")
        ) {
            quals.push(self.next()?);
        }
        let tok = self.peek(0).map(String::from);
        if let Some(tk) = &tok {
            if self.st.structs.contains_key(tk) && IDENT_START.is_match(self.peek(1).unwrap_or(""))
            {
                self.next()?;
                let name = self.next()?;
                if name != "output" {
                    return uns(format!("MSL: local struct {tk} {name}"));
                }
                return Ok(());
            }
        }
        let t = tok.as_deref().and_then(parse_type);
        if let Some(t) = t {
            if !matches!(self.peek(1), Some("(") | None)
                && IDENT_START.is_match(self.peek(1).unwrap_or(""))
            {
                self.next()?;
                loop {
                    let name = self.next()?;
                    let mut vt = t.clone();
                    let mut arr = String::new();
                    if self.accept("[") {
                        let n = py_int0(&self.next()?)?;
                        self.expect("]")?;
                        vt = T::array(t.clone(), n);
                        arr = format!("[{n}]");
                    }
                    let mut init = String::new();
                    if self.accept("=") {
                        if self.peek(0) == Some("{") {
                            return uns("MSL: brace initializer in a function");
                        }
                        init = format!(" = {}", convert(&self.expr(sc, 0)?, &t)?.s);
                    }
                    sc.set(&name, (name.clone(), vt));
                    let q = if quals.iter().any(|q| q == "const") && !init.is_empty() {
                        "const "
                    } else {
                        ""
                    };
                    out.push(format!("{pad}{q}{} {name}{arr}{init}", t.glsl()));
                    if !self.accept(",") {
                        break;
                    }
                }
                return Ok(());
            }
        }
        let a = self.assign_expr(sc)?;
        out.push(pad + &a);
        Ok(())
    }

    fn assign_expr(&mut self, sc: &mut Scope) -> R<String> {
        if matches!(self.peek(0), Some("++") | Some("--")) {
            let op = self.next()?;
            let x = self.expr(sc, 90)?;
            return Ok(format!("{op}{}", x.s));
        }
        let lhs = self.expr(sc, 90)?;
        let op = self.peek(0).map(String::from);
        if let Some(op) = op.filter(|o| {
            [
                "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
            ]
            .contains(&o.as_str())
        }) {
            self.next()?;
            let rhs = self.expr(sc, 0)?;
            if op == "=" {
                return Ok(format!("{} = {}", lhs.s, convert(&rhs, &lhs.t)?.s));
            }
            let b = binary(&op[..op.len() - 1], &lhs, &rhs)?;
            return Ok(format!("{} = {}", lhs.s, convert(&b, &lhs.t)?.s));
        }
        Ok(lhs.s)
    }
}

// ---------------------------------------------------------------- typed operations

fn literal(tok: &str) -> R<E> {
    let mut t = tok.to_string();
    if t.starts_with("0x") || t.starts_with("0X") {
        if t.ends_with(['u', 'U']) {
            return Ok(e(t, uint()));
        }
        let v = u128::from_str_radix(&t[2..], 16)
            .map_err(|_| Fault::Crash("invalid hex literal".into()))?;
        return Ok(if v < 1 << 31 {
            e(v.to_string(), int())
        } else {
            e(format!("{v}u"), uint())
        });
    }
    if t.ends_with(['u', 'U']) {
        t.pop();
        return Ok(e(t + "u", uint()));
    }
    if t.ends_with(['f', 'F', 'h', 'H']) {
        t.pop();
    }
    if DIGITS.is_match(&t) {
        return Ok(e(t, int()));
    }
    if t.ends_with('.') {
        t.push('0');
    }
    if t.starts_with('.') {
        t.insert(0, '0');
    }
    Ok(e(t, float()))
}

fn order(b: &str) -> usize {
    ["bool", "int", "uint", "float"]
        .iter()
        .position(|x| *x == b)
        .unwrap_or(0)
}

fn unify(a: &T, b: &T) -> T {
    if a.kind != "vec" || b.kind != "vec" {
        return a.clone();
    }
    let n = a.n.max(b.n);
    let base = if order(a.base) >= order(b.base) {
        a.base
    } else {
        b.base
    };
    vec(base, n)
}

/// e as type t (metal.convert).
fn convert(x: &E, t: &T) -> R<E> {
    if x.t == *t || t.kind != "vec" || x.t.kind != "vec" {
        return Ok(x.clone());
    }
    if x.t.base == t.base && x.t.n == t.n {
        return Ok(x.clone());
    }
    let mut x = x.clone();
    if x.t.n == 1 && t.n > 1 {
        if x.t.base != t.base {
            x = e(format!("{}({})", t.base, x.s), vec(t.base, 1));
        }
        return Ok(e(format!("{}({})", t.glsl(), x.s), t.clone()));
    }
    if x.t.n != t.n {
        if x.t.n > t.n {
            x = e(
                format!("{}.{}", x.wrap(100), &"xyzw"[..t.n as usize]),
                vec(x.t.base, t.n),
            );
        } else {
            return uns(format!("MSL: {} used as {}", x.t.glsl(), t.glsl()));
        }
    }
    if x.t.base != t.base {
        return Ok(e(format!("{}({})", t.glsl(), x.s), t.clone()));
    }
    Ok(x)
}

fn bool_of(x: &E) -> R<E> {
    if x.t.kind == "vec" && x.t.base == "bool" {
        return Ok(x.clone());
    }
    if x.t.kind == "vec" && x.t.n == 1 {
        let zero = match x.t.base {
            "float" => "0.0",
            "int" => "0",
            _ => "0u",
        };
        return Ok(ep(format!("{} != {zero}", x.wrap(12)), boolean(), 11));
    }
    uns(format!("MSL: {} as a condition", x.t.glsl()))
}

fn binary(op: &str, a: &E, b: &E) -> R<E> {
    let Some(p) = prec(op) else {
        return crash(format!("KeyError: {op}"));
    };
    if a.t.kind == "mat" || b.t.kind == "mat" {
        if op != "*" {
            return uns(format!("MSL: matrix {op}"));
        }
        let t = if a.t.kind == "mat" && b.t.kind == "vec" {
            vec("float", a.t.rows)
        } else if b.t.kind == "mat" && a.t.kind == "vec" {
            vec("float", b.t.n)
        } else if a.t.kind == "mat" {
            a.t.clone()
        } else {
            b.t.clone()
        };
        return Ok(ep(format!("{} * {}", a.wrap(p), b.wrap(p + 1)), t, p));
    }
    if a.t.kind != "vec" || b.t.kind != "vec" {
        return uns(format!("MSL: {op} on {} and {}", a.t.glsl(), b.t.glsl()));
    }
    if op == "&&" || op == "||" {
        if a.t.n > 1 || b.t.n > 1 {
            let n = a.t.n.max(b.t.n);
            let (a2, b2) = (convert(a, &vec("bool", n))?, convert(b, &vec("bool", n))?);
            let parts: Vec<String> = "xyzw"[..n as usize]
                .chars()
                .map(|k| format!("{}.{k} {op} {}.{k}", a2.wrap(100), b2.wrap(100)))
                .collect();
            return Ok(e(format!("bvec{n}({})", parts.join(", ")), vec("bool", n)));
        }
        return Ok(ep(
            format!("{} {op} {}", bool_of(a)?.wrap(p), bool_of(b)?.wrap(p + 1)),
            boolean(),
            p,
        ));
    }
    let t = unify(&a.t, &b.t);
    if let Some(f) = cmp_fn(op) {
        let (a2, b2) = (convert(a, &t)?, convert(b, &t)?);
        if t.n > 1 {
            return Ok(e(format!("{f}({}, {})", a2.s, b2.s), vec("bool", t.n)));
        }
        return Ok(ep(
            format!("{} {op} {}", a2.wrap(p), b2.wrap(p + 1)),
            boolean(),
            p,
        ));
    }
    if (op == "&" || op == "|" || op == "^") && t.base == "bool" {
        if t.n > 1 {
            return uns(format!("MSL: {op} on bool vectors"));
        }
        let o = if op == "^" {
            "^^".to_string()
        } else {
            op.repeat(2)
        };
        return Ok(ep(
            format!("{} {o} {}", a.wrap(p), b.wrap(p + 1)),
            boolean(),
            p,
        ));
    }
    if op == "<<" || op == ">>" {
        let bb = if b.t.n == a.t.n || b.t.n == 1 {
            b.clone()
        } else {
            convert(b, &vec(b.t.base, a.t.n))?
        };
        return Ok(ep(
            format!("{} {op} {}", a.wrap(p), bb.wrap(p + 1)),
            a.t.clone(),
            p,
        ));
    }
    if op == "%" && t.base == "float" {
        let (a2, b2) = (convert(a, &t)?, convert(b, &t)?);
        return Ok(e(
            format!(
                "({} - {} * trunc({} / {}))",
                a2.wrap(100),
                b2.wrap(100),
                a2.s,
                b2.wrap(15)
            ),
            t,
        ));
    }
    let a2 = convert(a, &vec(t.base, a.t.n))?;
    let b2 = convert(b, &vec(t.base, b.t.n))?;
    Ok(ep(format!("{} {op} {}", a2.wrap(p), b2.wrap(p + 1)), t, p))
}

fn construct(t: &T, args: &[E]) -> R<E> {
    if t.kind == "mat" {
        let parts = args
            .iter()
            .map(|a| convert(a, &if a.t.n == 1 { float() } else { a.t.clone() }).map(|x| x.s))
            .collect::<R<Vec<_>>>()?;
        return Ok(e(format!("{}({})", t.glsl(), parts.join(", ")), t.clone()));
    }
    if args.len() == 1 {
        let mut a = args[0].clone();
        if a.t.kind == "vec" && a.t.n == t.n && a.t.base == t.base {
            return Ok(ep(a.s, t.clone(), a.p));
        }
        if a.t.kind == "vec" && a.t.n > t.n {
            a = e(
                format!("{}.{}", a.wrap(100), &"xyzw"[..t.n as usize]),
                vec(a.t.base, t.n),
            );
        }
        return Ok(e(format!("{}({})", t.glsl(), a.s), t.clone()));
    }
    let mut parts = Vec::new();
    for a in args {
        if a.t.kind != "vec" {
            return uns(format!("MSL: {} from {}", t.glsl(), a.t.glsl()));
        }
        parts.push(if a.t.base != t.base {
            convert(a, &vec(t.base, a.t.n))?.s
        } else {
            a.s.clone()
        });
    }
    Ok(e(format!("{}({})", t.glsl(), parts.join(", ")), t.clone()))
}

fn bitcast(x: &E, t: &T) -> R<E> {
    let (s, d) = (x.t.base, t.base);
    if x.t.n != t.n {
        return uns(format!("MSL: as_type<{}> of {}", t.glsl(), x.t.glsl()));
    }
    if s == d {
        return Ok(x.clone());
    }
    if s == "float" {
        let f = if d == "int" {
            "floatBitsToInt"
        } else {
            "floatBitsToUint"
        };
        return Ok(e(format!("{f}({})", x.s), t.clone()));
    }
    if d == "float" {
        let f = if s == "int" {
            "intBitsToFloat"
        } else {
            "uintBitsToFloat"
        };
        return Ok(e(format!("{f}({})", x.s), t.clone()));
    }
    Ok(e(format!("{}({})", t.glsl(), x.s), t.clone()))
}

fn three(args: &[E]) -> R<(&E, &E, &E)> {
    match args {
        [a, b, c] => Ok((a, b, c)),
        _ => crash("ValueError: not enough values to unpack"),
    }
}

fn call(st: &mut Stage, name: &str, args: Vec<E>) -> R<E> {
    let name = name
        .replace("metal::", "")
        .replace("precise::", "")
        .replace("fast::", "");
    if name == "fma" {
        let (a, b, c) = three(&args)?;
        let t = unify(&unify(&a.t, &b.t), &c.t);
        return Ok(ep(
            format!(
                "{} * {} + {}",
                convert(a, &t)?.wrap(30),
                convert(b, &t)?.wrap(30),
                convert(c, &t)?.wrap(28)
            ),
            t,
            28,
        ));
    }
    if name == "saturate" {
        let [a] = args.as_slice() else {
            return crash("ValueError: too many values to unpack");
        };
        return Ok(e(format!("clamp({}, 0.0, 1.0)", a.s), a.t.clone()));
    }
    if name == "select" {
        let (a, b, c) = three(&args)?;
        let t = unify(&a.t, &b.t);
        let (a, b) = (convert(a, &t)?, convert(b, &t)?);
        if t.n == 1 {
            return Ok(ep(
                format!("{} ? {} : {}", bool_of(c)?.wrap(6), b.wrap(6), a.wrap(5)),
                t,
                5,
            ));
        }
        let c = if c.t.base == "bool" {
            convert(c, &vec("bool", t.n))?
        } else {
            e(format!("bvec{}({})", t.n, c.s), vec("bool", t.n))
        };
        if t.base == "float" {
            return Ok(e(format!("mix({}, {}, {})", a.s, b.s, c.s), t));
        }
        let comps: Vec<String> = "xyzw"[..t.n as usize]
            .chars()
            .map(|k| {
                format!(
                    "({}.{k} ? {}.{k} : {}.{k})",
                    c.wrap(100),
                    b.wrap(100),
                    a.wrap(100)
                )
            })
            .collect();
        return Ok(e(format!("{}({})", t.glsl(), comps.join(", ")), t));
    }
    if name == "level" || name == "bias" || name == "min_lod_clamp" {
        let [a] = args.as_slice() else {
            return crash("ValueError: too many values to unpack");
        };
        return Ok(e(
            format!("{name}:{}", convert(a, &float())?.s),
            T::new("opt"),
        ));
    }
    if name == "gradient2d" || name == "gradient3d" || name == "gradientcube" {
        let n = if name == "gradient2d" { 2 } else { 3 };
        let parts = args
            .iter()
            .map(|a| convert(a, &vec("float", n)).map(|x| x.s))
            .collect::<R<Vec<_>>>()?;
        return Ok(e(format!("gradient:{}", parts.join(", ")), T::new("opt")));
    }
    if name.starts_with("component") {
        return uns("MSL: gather component");
    }
    if name == "dot" || name == "length" || name == "distance" {
        let (Some(f), Some(l)) = (args.first(), args.last()) else {
            return crash("IndexError: list index out of range");
        };
        let t = unify(&f.t, &l.t);
        let parts = args
            .iter()
            .map(|a| convert(a, &t).map(|x| x.s))
            .collect::<R<Vec<_>>>()?;
        return Ok(e(format!("{name}({})", parts.join(", ")), vec(t.base, 1)));
    }
    if name == "bitFieldInsert" {
        let [w, o, a, b] = args.as_slice() else {
            return crash("ValueError: not enough values to unpack");
        };
        let t = unify(&unify(&w.t, &o.t), &unify(&a.t, &b.t));
        st.helpers.insert(t.glsl());
        let parts = [w, o, a, b]
            .iter()
            .map(|x| convert(x, &t).map(|x| x.s))
            .collect::<R<Vec<_>>>()?;
        return Ok(e(format!("pm_bitFieldInsert({})", parts.join(", ")), t));
    }
    if [
        "popcount",
        "reverse_bits",
        "extract_bits",
        "insert_bits",
        "ctz",
        "clz",
    ]
    .contains(&name.as_str())
    {
        return uns(format!("MSL: {name}"));
    }
    if FUNCS_SAME.contains(&name.as_str()) || renamed(&name).is_some() {
        let gl = renamed(&name).map_or(name.clone(), String::from);
        let Some(a0) = args.first() else {
            return crash("IndexError: list index out of range");
        };
        if name == "all" || name == "any" {
            return Ok(e(format!("{gl}({})", a0.s), boolean()));
        }
        if name == "isnan" || name == "isinf" {
            return Ok(e(format!("{gl}({})", a0.s), vec("bool", a0.t.n)));
        }
        let mut t = a0.t.clone();
        for a in &args[1..] {
            t = unify(&t, &a.t);
        }
        if [
            "mix",
            "clamp",
            "min",
            "max",
            "step",
            "smoothstep",
            "pow",
            "atan2",
        ]
        .contains(&name.as_str())
        {
            let mut c = Vec::new();
            for a in &args {
                let to = if a.t.n == t.n || a.t.n > 1 {
                    t.clone()
                } else {
                    vec(t.base, 1)
                };
                c.push(convert(a, &to)?.s);
            }
            return Ok(e(format!("{gl}({})", c.join(", ")), t));
        }
        let parts = args
            .iter()
            .map(|a| convert(a, &t).map(|x| x.s))
            .collect::<R<Vec<_>>>()?;
        return Ok(e(format!("{gl}({})", parts.join(", ")), t));
    }
    uns(format!("MSL: function {name}"))
}

fn member(st: &Stage, obj: &E, name: &str) -> R<E> {
    let t = &obj.t;
    if t.kind == "struct" {
        if let Some((n, mt)) = st.io.get(&(obj.s.clone(), name.to_string())) {
            return Ok(e(n.clone(), mt.clone()));
        }
        let Some(mem) = st.structs.get(&t.name) else {
            return uns(format!("MSL: struct {}", t.name));
        };
        for (mn, mt, _) in mem {
            if mn == name {
                if st.cbuffers.contains_key(&obj.s) {
                    return Ok(e(name, mt.clone()));
                }
                return Ok(e(format!("{}.{name}", obj.wrap(100)), mt.clone()));
            }
        }
        return uns(format!("MSL: {} has no {name}", t.name));
    }
    if t.kind == "vec" {
        if !SWZ.is_match(name) || t.n == 1 && name != "x" && name != "r" {
            if t.n == 1 && XR.is_match(name) {
                let l = name.len() as u32;
                let s = if l == 1 {
                    format!("{}({})", t.glsl(), obj.s)
                } else {
                    format!("{}({})", vec(t.base, l).glsl(), obj.s)
                };
                return Ok(e(s, vec(t.base, l)));
            }
            return uns(format!("MSL: .{name} of {}", t.glsl()));
        }
        if t.n == 1 {
            return Ok(obj.clone());
        }
        return Ok(e(
            format!("{}.{name}", obj.wrap(100)),
            vec(t.base, name.len() as u32),
        ));
    }
    uns(format!("MSL: .{name} of {}", t.glsl()))
}

// ---------------------------------------------------------------- top level

fn attrs(p: &mut Parser) -> R<Vec<String>> {
    let mut out = Vec::new();
    while p.accept("[[") {
        let (mut cur, mut depth) = (Vec::new(), 0i64);
        loop {
            let tok = p.next()?;
            if tok == "]]" && depth == 0 {
                break;
            }
            if tok == "(" {
                depth += 1;
            } else if tok == ")" {
                depth -= 1;
            }
            if tok == "," && depth == 0 {
                out.push(cur.concat());
                cur.clear();
                continue;
            }
            cur.push(tok);
        }
        if !cur.is_empty() {
            out.push(cur.concat());
        }
    }
    Ok(out)
}

fn skip_braces(p: &mut Parser) -> R<()> {
    p.expect("{")?;
    let mut depth = 1;
    while depth > 0 {
        let tok = p.next()?;
        depth += (tok == "{") as i64;
        depth -= (tok == "}") as i64;
    }
    Ok(())
}

static GLOBAL_RES: LazyLock<(Regex, Regex, Regex, Regex)> = LazyLock::new(|| {
    (
        re(r"\b(\w+) \[\[ function_constant \( (\d+) \) \]\]\n?\z"),
        re(r"\Aconstant const uint (rp_output_remap_(\d+)) = "),
        re(r"\Aconstexpr sampler (\w+) \("),
        re(r"\Aconstant (\w+) (\w+) \[ (\d+) \] = \{ (.*) \}\n?\z"),
    )
});

/// One stage's MSL -> (Stage with its GLSL parts, body lines) (metal.translate_stage).
fn translate_stage(src: &str) -> R<(Stage, Vec<String>)> {
    let toks = tokenize(&preprocess(src))?;
    let mut st = Stage::default();
    let mut body: Option<Vec<String>> = None;
    let mut p = Parser {
        t: toks,
        i: 0,
        st: &mut st,
    };
    while let Some(tok) = p.peek(0).map(String::from) {
        match tok.as_str() {
            "using" => while p.next()? != ";" {},
            "struct" => {
                p.next()?;
                let name = p.next()?;
                p.expect("{")?;
                let mut mem = Vec::new();
                while !p.accept("}") {
                    let mut ty = p.next()?;
                    while ty == "constant" || ty == "const" {
                        ty = p.next()?;
                    }
                    if matches!(p.peek(0), Some("*") | Some("&")) {
                        p.next()?;
                        let mn = p.next()?;
                        p.expect(";")?;
                        mem.push((mn, T::new("ptr"), Vec::new()));
                        continue;
                    }
                    let t = match parse_type(&ty) {
                        Some(t) => t,
                        None if p.st.structs.contains_key(&ty) => T::named("struct", &ty),
                        None => return uns(format!("MSL: struct member type {ty}")),
                    };
                    let mn = p.next()?;
                    let mut t = t;
                    if p.accept("[") {
                        let n = py_int0(&p.next()?)?;
                        p.expect("]")?;
                        t = T::array(t, n);
                    }
                    let at = attrs(&mut p)?;
                    p.expect(";")?;
                    mem.push((mn, t, at));
                }
                p.expect(";")?;
                p.st.structs.insert(name, mem);
            }
            "template" => {
                while p.next()? != "{" {}
                p.i -= 1;
                skip_braces(&mut p)?;
                p.accept(";");
            }
            "constant" | "constexpr" => {
                let mut decl = Vec::new();
                while p.peek(0) != Some(";") {
                    decl.push(p.next()?);
                }
                p.next()?;
                let text = decl.join(" ");
                let (fc, remap, csampler, carray) = &*GLOBAL_RES;
                if let Some(m) = fc.captures(&text) {
                    let v = if decl.get(1).map(String::as_str) == Some("bool") {
                        "false"
                    } else {
                        "0x76543210u"
                    };
                    p.st.fconst.insert(m[1].to_string(), v.to_string());
                    continue;
                }
                if let Some(m) = remap.captures(&text) {
                    p.st.fconst.insert(m[1].to_string(), format!("{}u", &m[2]));
                    continue;
                }
                if text.contains("xlt_remap_o") {
                    continue;
                }
                if let Some(m) = csampler.captures(&text) {
                    p.st.globals
                        .insert(m[1].to_string(), ("sampler".into(), T::new("sampler")));
                    continue;
                }
                if let Some(m) = carray.captures(&text) {
                    let Some(t) = parse_type(&m[1]) else {
                        return crash("AttributeError: a constant array of no type");
                    };
                    let toks = tokenize(&m[4])?;
                    let globals = p.st.globals.clone();
                    let mut vals = Vec::new();
                    {
                        let mut inner = Parser {
                            t: toks,
                            i: 0,
                            st: p.st,
                        };
                        let mut sc = Scope {
                            vars: vec![globals],
                        };
                        while inner.peek(0).is_some() {
                            vals.push(convert(&inner.expr(&mut sc, 0)?, &t)?.s);
                            inner.accept(",");
                        }
                    }
                    let n: i64 = m[3].parse().unwrap();
                    p.st.consts.push(format!(
                        "const {g} {}[{n}] = {g}[{n}]({});",
                        &m[2],
                        vals.join(", "),
                        g = t.glsl()
                    ));
                    p.st.globals
                        .insert(m[2].to_string(), (m[2].to_string(), T::array(t, n)));
                    continue;
                }
                return uns(format!(
                    "MSL: global {}",
                    text.chars().take(80).collect::<String>()
                ));
            }
            "vertex" | "fragment" => {
                p.st.stage = Some(if p.next()? == "vertex" {
                    "VERTEX"
                } else {
                    "FRAGMENT"
                });
                let ret = p.next()?;
                p.expect("xlatMtlMain")?;
                p.expect("(")?;
                let mut params = Vec::new();
                while !p.accept(")") {
                    let mut ptoks = Vec::new();
                    let mut depth = 0i64;
                    loop {
                        let tk = p.peek(0).map(String::from);
                        match tk.as_deref() {
                            Some("[[") => break,
                            Some("<") => depth += 1,
                            Some(">") => depth -= 1,
                            _ => {}
                        }
                        if matches!(tk.as_deref(), Some(",") | Some(")")) && depth == 0 {
                            break;
                        }
                        ptoks.push(p.next()?);
                    }
                    let at = attrs(&mut p)?;
                    params.push((ptoks, at));
                    p.accept(",");
                }
                declare(p.st, &ret, &params)?;
                let mut out = Vec::new();
                p.expect("{")?;
                let mut sc = Scope {
                    vars: vec![p.st.globals.clone(), BTreeMap::new()],
                };
                while !p.accept("}") {
                    p.statement(&mut sc, &mut out, 1)?;
                }
                body = Some(out);
            }
            _ => return uns(format!("MSL: top level {}", crate::py::repr(&tok))),
        }
    }
    let Some(body) = body else {
        return uns("MSL: no entry point");
    };
    Ok((st, body))
}

static COLOR: LazyLock<(Regex, Regex, Regex)> = LazyLock::new(|| {
    (
        re(r"\Acolor\(([\w\[\]]+)\)"),
        re(r"\Axlt_remap_o\[(\d+)\]"),
        re(r"\Auser\((\w+)\)"),
    )
});

fn declare(st: &mut Stage, ret: &str, params: &[(Vec<String>, Vec<String>)]) -> R<()> {
    let (color, remap, user) = &*COLOR;
    let stage = st.stage.unwrap();
    if stage == "VERTEX" && ret != "Mtl_VertexOut"
        || stage == "FRAGMENT" && ret != "Mtl_FragmentOut"
    {
        return uns(format!("MSL: entry returns {ret}"));
    }
    for (mn, mt, at) in st.structs.get(ret).cloned().unwrap_or_default() {
        let a = at.join(",");
        if stage == "VERTEX" {
            if a.starts_with("position") {
                st.io
                    .insert(("output".into(), mn), ("gl_Position".into(), mt));
            } else if a.starts_with("user(") {
                let Some(sem) = user.captures(&a) else {
                    return crash("AttributeError: user()");
                };
                let sem = sem[1].to_string();
                let interp = if a.contains("flat") { "flat " } else { "" };
                st.outs.push(format!("{interp}out {} vs_{sem};", mt.glsl()));
                st.io
                    .insert(("output".into(), mn), (format!("vs_{sem}"), mt));
            } else if a.starts_with("point_size") {
                st.io
                    .insert(("output".into(), mn), ("gl_PointSize".into(), mt));
            } else {
                return uns(format!("MSL: vertex output [[ {a} ]]"));
            }
        } else if let Some(m) = color.captures(&a) {
            let raw = m[1].to_string();
            let loc0 = st.fconst.get(&raw).cloned().unwrap_or(raw.clone());
            let mut loc = Some(loc0.trim_end_matches('u').to_string());
            if !loc.as_ref().unwrap().chars().all(|c| c.is_ascii_digit())
                || loc.as_ref().unwrap().is_empty()
            {
                loc = remap.captures(&raw).map(|m2| m2[1].to_string());
            }
            let Some(loc) = loc else {
                return uns(format!("MSL: output [[ {a} ]]"));
            };
            st.outs
                .push(format!("layout(location = {loc}) out {} {mn};", mt.glsl()));
            st.io.insert(("output".into(), mn.clone()), (mn, mt));
        } else if a.starts_with("depth") {
            st.io
                .insert(("output".into(), mn), ("gl_FragDepth".into(), mt));
        } else {
            return uns(format!("MSL: fragment output [[ {a} ]]"));
        }
    }
    st.globals
        .insert("output".into(), ("output".into(), T::named("struct", ret)));
    for (ptoks, at) in params {
        let a = at.join(",");
        if a.contains("function_constant") {
            if let Some(last) = ptoks.last() {
                st.globals.insert(last.clone(), ("0u".into(), uint()));
            }
            continue;
        }
        let Some(name) = ptoks.last().cloned() else {
            return crash("IndexError: list index out of range");
        };
        let first = ptoks[0].as_str();
        if first == "constant" && ptoks.len() >= 2 && ptoks[ptoks.len() - 2] == "&" {
            let sname = ptoks[1].clone();
            st.cbuffers.insert(name.clone(), sname.clone());
            st.globals
                .insert(name.clone(), (name.clone(), T::named("struct", &sname)));
            let Some(mems) = st.structs.get(&sname).cloned() else {
                return crash(format!("KeyError: {sname}"));
            };
            for (mn, mt, _) in mems {
                if mt.kind == "struct"
                    || mt.kind == "array" && mt.elem.as_ref().unwrap().kind == "struct"
                {
                    return uns(format!(
                        "MSL: constant buffer {sname} has struct members (GPU instancing data)"
                    ));
                }
                let arr = if mt.kind == "array" {
                    format!("[{}]", mt.length)
                } else {
                    String::new()
                };
                st.uniforms
                    .push(format!("uniform {} {mn}{arr};", mt.glsl()));
            }
            continue;
        }
        if first == "device" || first == "const" || ptoks.iter().any(|x| x == "*") {
            return uns(format!("MSL: device buffer {name}"));
        }
        if let Some(gl) = tex_of(first) {
            let acc = ptoks.join(" ");
            if acc.contains("access::write") || acc.contains("access::read_write") {
                return uns(format!("MSL: writable texture {name}"));
            }
            st.textures.insert(name.clone(), (gl, first.to_string()));
            st.globals
                .insert(name.clone(), (name.clone(), T::named("texture", gl)));
            if gl.contains("Shadow") {
                st.uniforms
                    .push(format!("uniform {} {name};", gl.replace("Shadow", "")));
                st.uniforms.push(format!("uniform {gl} hlslcc_zcmp{name};"));
            } else {
                st.uniforms.push(format!("uniform {gl} {name};"));
            }
            if gl == "sampler2DMS" {
                st.needs_es31 = Some("multisampled texture");
            }
            continue;
        }
        if first == "sampler" {
            st.globals.insert(name.clone(), (name, T::new("sampler")));
            continue;
        }
        if a == "stage_in" {
            let sname = first.to_string();
            let Some(mems) = st.structs.get(&sname).cloned() else {
                return crash(format!("KeyError: {sname}"));
            };
            for (mn, mt, mat) in mems {
                let ma = mat.join(",");
                if stage == "VERTEX" {
                    if !ma.starts_with("attribute(") {
                        return uns(format!("MSL: vertex input [[ {ma} ]]"));
                    }
                    st.ins.push(format!("in {} in_{mn};", mt.glsl()));
                    st.io
                        .insert((name.clone(), mn.clone()), (format!("in_{mn}"), mt));
                } else if let Some(m) = user.captures(&ma) {
                    let interp = if ma.contains("flat") { "flat " } else { "" };
                    st.ins
                        .push(format!("{interp}in {} vs_{};", mt.glsl(), &m[1]));
                    st.io
                        .insert((name.clone(), mn.clone()), (format!("vs_{}", &m[1]), mt));
                } else if ma.starts_with("position") {
                    st.io
                        .insert((name.clone(), mn.clone()), ("gl_FragCoord".into(), mt));
                } else {
                    return uns(format!("MSL: fragment input [[ {ma} ]]"));
                }
            }
            st.globals
                .insert(name.clone(), (name, T::named("struct", &sname)));
            continue;
        }
        let builtin = match a.as_str() {
            "vertex_id" => Some(("gl_VertexID", int())),
            "instance_id" => Some(("gl_InstanceID", int())),
            "position" => Some(("gl_FragCoord", vec("float", 4))),
            "front_facing" => Some(("gl_FrontFacing", boolean())),
            _ => None,
        };
        if let Some((g, gt)) = builtin {
            let t = parse_type(first).unwrap_or_else(int);
            let s = if gt != t {
                format!("{}({g})", t.glsl())
            } else {
                g.to_string()
            };
            st.globals.insert(name, (s, t));
            continue;
        }
        return uns(format!("MSL: parameter {} [[ {a} ]]", ptoks.join(" ")));
    }
    Ok(())
}

const HELPERS: &str = "{t} pm_bitFieldInsert({t} width, {t} offset, {t} src2, {t} src3)\n{\n    {t} bitmask = ((({t}(1u) << width) - {t}(1u)) << offset);\n    return ((src2 << offset) & bitmask) | (src3 & ~bitmask);\n}\n";

static UNIFORM_NAME: LazyLock<Regex> = LazyLock::new(|| re(r"(\w+)(?:\[\d+\])?;\n?\z"));
static SAMPLER_WORD: LazyLock<Regex> = LazyLock::new(|| re(r"\bsampler"));

/// GLSL ES text of a translated stage (metal.emit).
fn emit(st: &Stage, body: &[String], precision: &str, es: &str) -> String {
    let mut head = vec![format!("#version {es} es")];
    let text = st
        .consts
        .iter()
        .chain(body)
        .cloned()
        .collect::<Vec<_>>()
        .join("\n");
    let uniforms: Vec<String> = st
        .uniforms
        .iter()
        .filter(|u| {
            let n = UNIFORM_NAME
                .captures(u)
                .map(|c| c[1].to_string())
                .unwrap_or_default();
            re(&format!(r"\b{}\b", regex::escape(&n))).is_match(&text) || SAMPLER_WORD.is_match(u)
        })
        .cloned()
        .collect();
    let code = uniforms
        .iter()
        .chain(&st.ins)
        .chain(&st.outs)
        .chain(&st.consts)
        .chain(body)
        .cloned()
        .collect::<Vec<_>>()
        .join("\n");
    head.push(
        glsl::precision_block(&code, precision, false)
            .trim_end_matches('\n')
            .to_string(),
    );
    let mut out: Vec<String> = head;
    out.extend(uniforms);
    out.extend(st.consts.iter().cloned());
    out.extend(st.ins.iter().cloned());
    out.extend(st.outs.iter().cloned());
    for t in &st.helpers {
        out.push(
            HELPERS
                .replace("{t}", t)
                .replace("{{", "{")
                .replace("}}", "}"),
        );
    }
    out.push("void main()\n{".into());
    out.extend(body.iter().cloned());
    out.push("}".into());
    out.join("\n") + "\n"
}

static SAMPLER_LINE: LazyLock<Regex> =
    LazyLock::new(|| re(r"(?m)^uniform\s+(?:(?:highp|mediump|lowp)\s+)?\w*sampler\w+\s+(\w+);"));
static HEAD: LazyLock<Regex> = LazyLock::new(|| re(r"\A(#version[^\n]*\n(?:precision[^\n]*\n)*)"));
static LAST_WORD: LazyLock<Regex> = LazyLock::new(|| re(r"(\w+);\n?\z"));

#[allow(clippy::too_many_arguments)]
fn program(
    v: &Stage,
    vb: &[String],
    f: &Stage,
    fb: &[String],
    precision: &str,
    unity: (u64, u64),
    es: &str,
    notes: &mut Vec<String>,
) -> R<(String, i64)> {
    let mut texts: Vec<(&str, String)> = vec![
        ("VERTEX", emit(v, vb, precision, es)),
        ("FRAGMENT", emit(f, fb, precision, es)),
    ];
    let (vs, flips) = vulkan::uv_origin(&texts[0].1);
    texts[0].1 = vs;
    if flips > 0 {
        notes.push(format!("VERTEX uv-origin {flips}"));
    }
    if unity >= (2019, 1) {
        let mut units: BTreeMap<String, usize> = BTreeMap::new();
        for k in [1, 0] {
            for c in SAMPLER_LINE.captures_iter(&texts[k].1) {
                let l = units.len();
                units.entry(c[1].to_string()).or_insert(l);
            }
        }
        for (_, t) in texts.iter_mut() {
            let s = SAMPLER_LINE
                .replace_all(t, |c: &regex::Captures| {
                    format!("UNITY_LOCATION({}) {}", units[&c[1]], &c[0])
                })
                .into_owned();
            let e = HEAD.find(&s).map_or(0, |m| m.end());
            *t = format!("{}{}{}", &s[..e], vulkan::UNIFORM_LOCATION_BLOCK, &s[e..]);
        }
    }
    for (st, t) in &texts {
        glsl::vulkan_check(t, st, es == "310").map_err(|u| {
            if u.needs_es31 {
                Fault::Es31(u.msg)
            } else {
                Fault::Uns(u.msg)
            }
        })?;
    }
    Ok((
        format!(
            "#ifdef VERTEX\n{}#endif\n#ifdef FRAGMENT\n{}#endif\n",
            texts[0].1, texts[1].1
        ),
        if es == "310" { 3 } else { 4 },
    ))
}

/// MSL of a vertex and a fragment stage -> (GL program text, gpu type 4 or 3) (metal.translate_program).
pub fn translate_program(
    vs_src: &str,
    fs_src: &str,
    precision: &str,
    unity: (u64, u64),
    notes: &mut Vec<String>,
) -> R<(String, i64)> {
    let (mut v, vb) = translate_stage(vs_src)?;
    let (f, fb) = translate_stage(fs_src)?;
    if v.stage != Some("VERTEX") || f.stage != Some("FRAGMENT") {
        return uns("MSL: expected a vertex and a fragment stage");
    }
    let mut vout: Vec<(String, String)> = Vec::new();
    for l in &v.outs {
        if let Some(c) = LAST_WORD.captures(l) {
            vout.push((c[1].to_string(), l.clone()));
        }
    }
    for l in &f.ins {
        let name = LAST_WORD
            .captures(l)
            .map(|c| c[1].to_string())
            .unwrap_or_default();
        let Some(vl) = vout
            .iter()
            .rev()
            .find(|(n, _)| *n == name)
            .map(|(_, l)| l.clone())
        else {
            return uns(format!("MSL: fragment input {name} is not a vertex output"));
        };
        let vt = vl.split_whitespace().rev().nth(1).unwrap_or("");
        let ft = l.split_whitespace().rev().nth(1).unwrap_or("");
        if vt != ft {
            return uns(format!("MSL: varying {name}: {vt} written, {ft} read"));
        }
        if l.starts_with("flat ") && !vl.starts_with("flat ") {
            if let Some(i) = v.outs.iter().position(|x| *x == vl) {
                v.outs[i] = format!("flat {vl}");
            }
        }
    }
    let es = if v.needs_es31.is_some() || f.needs_es31.is_some() {
        "310"
    } else {
        "300"
    };
    match program(&v, &vb, &f, &fb, precision, unity, es, notes) {
        Err(Fault::Es31(_)) => program(&v, &vb, &f, &fb, precision, unity, "310", notes),
        r => r,
    }
}

/// The MSL text of a Metal entry's code (after Unity's header).
pub fn msl_of(code: &[u8]) -> R<String> {
    let Some(i) = code.windows(8).position(|w| w == b"#include") else {
        return uns("Metal program without MSL text");
    };
    String::from_utf8(code[i..].to_vec())
        .map_err(|_| Fault::Crash("UnicodeDecodeError: MSL text".into()))
}
