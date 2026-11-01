//! GLSL 1.50 as Unity's HLSLcc writes it for OpenGL Core -> GLSL ES 3.00 for Unity's GLES3
//! renderer, as shaders/glsl.py does it (its docstring has the rules): the #version line, the
//! desktop extensions ES has built in dropped, default precisions, the const-arrays and unroll
//! rewrites, and the checks that make a program Unsupported (no ES equivalent) or NeedsES31 (GLSL
//! ES 3.10). GLSL 4.x stages (GLCore41/43 programs) get the 4.x rules too. Compute kernels
//! (glsl.kernel_to_es) are not ported.
//!
//! The Python works with `re` on str; the patterns here are the same with the `regex` crate, with
//! Python's `\s` (which also takes \x1c - \x1f) and what `regex` lacks (look-ahead, back
//! references) written out where they are used, so every match is the one `re` finds.
use std::sync::LazyLock;

use regex::{Captures, NoExpand, Regex};

use crate::py;

/// Why a program has no GLES3 twin: no ES equivalent, or (needs_es31) what GLSL ES 3.10 has and
/// 3.00 lacks: it can be translated with `Es::Es310` (glsl.Unsupported, glsl.NeedsES31).
#[derive(Clone, Debug, PartialEq)]
pub struct Unsupported {
    pub needs_es31: bool,
    pub msg: String,
}

impl Unsupported {
    fn new(msg: impl Into<String>) -> Self {
        Unsupported {
            needs_es31: false,
            msg: msg.into(),
        }
    }

    /// The Python exception's class name.
    pub fn class(&self) -> &'static str {
        if self.needs_es31 {
            "NeedsES31"
        } else {
            "Unsupported"
        }
    }
}

impl std::fmt::Display for Unsupported {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.msg)
    }
}

impl std::error::Error for Unsupported {}

/// The GLSL ES version written: 3.00 (GLES3 programs) or 3.10 (GLES 3.1 programs).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Es {
    Es300,
    Es310,
}

impl Es {
    pub fn as_str(self) -> &'static str {
        match self {
            Es::Es300 => "300",
            Es::Es310 => "310",
        }
    }
}

/// The rewrites applied (glsl.OPTIMIZE: both by default).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Optimize {
    pub const_arrays: bool,
    pub unroll: bool,
}

impl Optimize {
    pub const ALL: Optimize = Optimize {
        const_arrays: true,
        unroll: true,
    };

    /// From the config's names ('const-arrays', 'unroll'; others are ignored, as in the Python).
    pub fn from_names<S: AsRef<str>>(names: &[S]) -> Optimize {
        Optimize {
            const_arrays: names.iter().any(|n| n.as_ref() == "const-arrays"),
            unroll: names.iter().any(|n| n.as_ref() == "unroll"),
        }
    }
}

const VERSIONS: [&str; 2] = ["150", "330"];
const VERSIONS_4X: [&str; 6] = ["400", "410", "420", "430", "440", "450"];
const DROP_EXTENSIONS: [&str; 2] = [
    "GL_ARB_explicit_attrib_location",
    "GL_ARB_shader_bit_encoding",
];
const DROP_EXTENSIONS_4X: [&str; 5] = [
    "GL_ARB_explicit_attrib_location",
    "GL_ARB_shader_bit_encoding",
    "GL_ARB_shading_language_420pack",
    "GL_ARB_shader_storage_buffer_object",
    "GL_ARB_shader_image_load_store",
];
pub const SAMPLER_TYPES: [&str; 18] = [
    "sampler2D",
    "sampler3D",
    "samplerCube",
    "sampler2DShadow",
    "samplerCubeShadow",
    "sampler2DArray",
    "sampler2DArrayShadow",
    "isampler2D",
    "isampler3D",
    "isamplerCube",
    "isampler2DArray",
    "usampler2D",
    "usampler3D",
    "usamplerCube",
    "usampler2DArray",
    "sampler2DMS",
    "isampler2DMS",
    "usampler2DMS",
];
pub const IMAGE_TYPES: [&str; 12] = [
    "image2D",
    "image3D",
    "imageCube",
    "image2DArray",
    "iimage2D",
    "iimage3D",
    "iimageCube",
    "iimage2DArray",
    "uimage2D",
    "uimage3D",
    "uimageCube",
    "uimage2DArray",
];
const IMAGE_FORMATS: [&str; 13] = [
    "rgba32f",
    "rgba16f",
    "r32f",
    "rgba8",
    "rgba8_snorm",
    "rgba32i",
    "rgba16i",
    "rgba8i",
    "r32i",
    "rgba32ui",
    "rgba16ui",
    "rgba8ui",
    "r32ui",
];
pub const UNROLL_MAX: i128 = 16;
pub const UNROLL_STATEMENTS: i128 = 120;
const NUM: &str = r"[-+]?(?:\d+\.?\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?)[uU]?";
const TYPES: &str = r"(?:float|int|uint|bool|[biu]?vec[234]|mat[234](?:x[234])?)";

/// A Python `re` pattern as a `regex` one: `\s` takes what Python's does (\x1c - \x1f too).
fn py_pattern(p: &str) -> String {
    let mut out = String::with_capacity(p.len() + 16);
    let (mut class, mut chars) = (false, p.chars());
    while let Some(c) = chars.next() {
        match c {
            '\\' => {
                let n = chars.next().expect("pattern ends with a backslash");
                match (n, class) {
                    ('s', false) => out.push_str(r"[\s\x1c-\x1f]"),
                    ('s', true) => out.push_str(r"\s\x1c-\x1f"),
                    _ => {
                        out.push('\\');
                        out.push(n);
                    }
                }
            }
            '[' if !class => {
                class = true;
                out.push(c);
                // a ']' right after '[' or '[^' is a literal
                let rest = chars.as_str();
                let skip = if rest.starts_with("^]") {
                    2
                } else if rest.starts_with(']') {
                    1
                } else {
                    0
                };
                for _ in 0..skip {
                    let x = chars.next().unwrap();
                    if x == ']' {
                        out.push('\\');
                    }
                    out.push(x);
                }
            }
            ']' if class => {
                class = false;
                out.push(c);
            }
            _ => out.push(c),
        }
    }
    out
}

fn re(p: &str) -> Regex {
    Regex::new(&py_pattern(p)).unwrap_or_else(|e| panic!("pattern {p:?}: {e}"))
}

/// A rule that makes a stage Unsupported: the first match in the stage is reported (`group`: the
/// group that is the Python's whole match, where a look-ahead is matched as part of the pattern).
struct Rule {
    re: Regex,
    what: &'static str,
    group: usize,
}

fn rule(p: &str, what: &'static str) -> Rule {
    Rule {
        re: re(p),
        what,
        group: 0,
    }
}

struct Patterns {
    ifdef_stage: Regex,
    if_open: Regex,
    endif_line: Regex,
    version_head: Regex,
    comments: Regex,
    preprocessor: Regex,
    version_line: Regex,
    extension: Regex,
    ifdef_name: Regex,
    endif: Regex,
    word: Regex,
    unsupported: Vec<Rule>,
    unsupported_4x: Vec<Rule>,
    es31: Vec<Rule>,
    es31_4x: Vec<Rule>,
    image_decl: Regex,
    layout_args: Regex,
    image_format: Regex,
    precise_dummy: Regex,
    varying_vertex: Regex,
    varying_fragment: Regex,
    immcb_decl: Regex,
    main_open: Regex,
    unroll_for: Regex,
    unroll_jump: Regex,
    unroll_decl: Regex,
}

static RX: LazyLock<Patterns> = LazyLock::new(|| {
    let ff = r"\bgl_(Vertex|Normal|Color|SecondaryColor|MultiTexCoord\d|ModelView\w*|Projection\w*|NormalMatrix|TextureMatrix\w*|FragColor|FragData)\b";
    let tex110 = r"\b(texture1D\w*|texture2D\w*|texture3D\w*|textureCube\w*|shadow2D\w*)\s*\(";
    let double = r"\b(double|dvec\d|dmat\d)\b";
    // (?=\s+(...)) after the qualifier: matched as part of the pattern, the qualifier reported
    let qualifier = |types: &str| Rule {
        re: re(&format!(
            r"\b(noperspective|sample|patch|subroutine|precise)\b\s+(?:{types})"
        )),
        what: "qualifier",
        group: 1,
    };
    let es31 = vec![
        rule(r"\b[iu]?sampler2DMS\b", "sampler type"),
        rule(
            r"\b(textureGather\w*|bitfield\w+|bitCount|findLSB|findMSB|packUnorm4x8|packSnorm4x8|unpackUnorm4x8|unpackSnorm4x8|uaddCarry|usubBorrow|umulExtended|imulExtended|frexp|ldexp)\s*\(",
            "ES 3.1 function",
        ),
    ];
    let mut es31_4x = vec![
        rule(r"\b[iu]?sampler2DMS\b", "sampler type"),
        rule(
            r"\b(textureGather\w*|bitfield\w+|bitCount|findLSB|findMSB|packUnorm4x8|packSnorm4x8|unpackUnorm4x8|unpackSnorm4x8|uaddCarry|usubBorrow|umulExtended|imulExtended|frexp|ldexp)\s*\(",
            "ES 3.1 function",
        ),
    ];
    es31_4x.extend([
        rule(r"layout\s*\([^)]*\b(binding|std430|early_fragment_tests)\b", "layout qualifier"),
        rule(r"layout\s*\([^)]*\blocation\b[^)]*\)\s*(?:(?:highp|mediump|lowp)\s+)?uniform\b", "uniform location"),
        rule(r"\bbuffer\s+\w+\s*\{", "storage buffer"),
        rule(&format!(r"\b({})\b", IMAGE_TYPES.join("|")), "image"),
        rule(
            r"\b(imageLoad|imageStore|imageSize|atomic(?:Add|Min|Max|And|Or|Xor|Exchange|CompSwap)|memoryBarrier(?:Buffer|Image|AtomicCounter)?)\s*\(",
            "ES 3.1 function",
        ),
    ]);
    let varying = |io: &str| {
        re(&format!(
            r"(?m)^([ \t]*)layout\s*\(\s*location\s*=\s*\d+\s*\)[ \t]*((?:(?:invariant|flat|smooth|centroid)\s+)*{io}\s)"
        ))
    };
    Patterns {
        ifdef_stage: re(r"\A\s*#\s*ifdef\s+(VERTEX|FRAGMENT|GEOMETRY|HULL|DOMAIN)\b"),
        if_open: re(r"\A\s*#\s*(if|ifdef|ifndef)\b"),
        endif_line: re(r"\A\s*#\s*endif\b"),
        version_head: re(r"\A\s*#\s*version\s+(\d+)"),
        comments: re(r"(?s)//[^\n]*|/\*.*?\*/"),
        preprocessor: re(r"(?m)^[ \t]*#[^\n]*"),
        version_line: re(r"\A\s*#\s*version\s+(\d+)\s*(\w*)"),
        extension: re(r"\A#\s*extension\s+(\w+)\s*:\s*(\w+)"),
        ifdef_name: re(r"\A#\s*ifdef\s+(\w+)\z"),
        endif: re(r"\A#\s*endif\b"),
        word: re(r"\w+"),
        unsupported: vec![
            rule(ff, "fixed-function built-in"),
            rule(r"\b(sampler1D\w*|sampler2DRect\w*|samplerBuffer|[iu]?sampler2DMSArray|image\w+D\w*)\b", "sampler type"),
            rule(tex110, "GLSL 1.10 texture function"),
            rule(
                r"\b(textureQueryLod|textureQueryLevels|fma|imageLoad|imageStore|atomic\w+|memoryBarrier\w*|barrier|interpolateAt\w+)\s*\(",
                "ES 3.2+ / desktop function",
            ),
            qualifier("in|out|uniform|centroid|flat|smooth|vec|float|int|uint|mat|bool"),
            rule(r"layout\s*\([^)]*\b(binding|offset|std430|index)\b", "layout qualifier"),
            rule(r"\bbuffer\s+\w+\s*\{", "storage buffer"),
            rule(
                r"\b(gl_ClipDistance|gl_PrimitiveID|gl_Layer|gl_ViewportIndex|gl_SampleID|gl_SamplePosition|gl_SampleMask\w*)\b",
                "built-in variable",
            ),
            rule(double, "double precision"),
        ],
        unsupported_4x: vec![
            rule(ff, "fixed-function built-in"),
            rule(
                r"\b([iu]?sampler1D\w*|[iu]?sampler2DRect\w*|[iu]?samplerBuffer|[iu]?sampler2DMSArray|[iu]?samplerCubeArray\w*|[iu]?image(?:1D\w*|2DRect|Buffer|CubeArray|2DMS\w*))\b",
                "sampler type",
            ),
            rule(tex110, "GLSL 1.10 texture function"),
            rule(
                r"\b(textureQueryLod|textureQueryLevels|textureSamples|imageSamples|fma|imageAtomic\w+|atomicCounter\w*|barrier|groupMemoryBarrier|memoryBarrierShared|interpolateAt\w+)\s*\(",
                "ES 3.2+ / desktop function",
            ),
            qualifier("in|out|uniform|centroid|flat|smooth|[biud]?vec|float|int|uint|mat|bool|highp|mediump|lowp"),
            rule(
                r"layout\s*\([^)]*\b(index|component|xfb_\w+|origin_upper_left|pixel_center_integer|depth_\w+|offset|align|stream)\b",
                "layout qualifier",
            ),
            rule(
                r"(?m)^[ \t]*(?:layout\s*\([^)]*\)\s*)?(?:(?:flat|smooth|centroid)\s+)*(?:in|out)\s+\w+\s*\{",
                "interface block",
            ),
            rule(
                r"\b(gl_ClipDistance|gl_CullDistance|gl_PrimitiveID|gl_Layer|gl_ViewportIndex|gl_SampleID|gl_SamplePosition|gl_SampleMask\w*|gl_PerVertex)\b",
                "built-in variable",
            ),
            rule(double, "double precision"),
        ],
        es31,
        es31_4x,
        image_decl: re(&format!(r"(?m)^(.*)\b({})\s+(\w+)\s*[\[;]", IMAGE_TYPES.join("|"))),
        layout_args: re(r"layout\s*\(([^)]*)\)"),
        image_format: re(&format!(r"\b({})\b", IMAGE_FORMATS.join("|"))),
        // ^precise ([biu]?vec4) (u_xlat_precise_\1);  (the back reference checked where it is used)
        precise_dummy: re(r"(?m)^precise ([biu]?vec4) (u_xlat_precise_[biu]?vec4);[ \t]*\n"),
        varying_vertex: varying("out"),
        varying_fragment: varying("in"),
        immcb_decl: re(&format!(r"(?m)^({TYPES}) (ImmCB_\w+)\[(\d+)\];\n")),
        main_open: re(r"void main\(\)\s*\{\n"),
        // the loop header with \2 (the counter) and \1 (the indentation) as groups 4, 7 and 8
        unroll_for: re(
            r"(?m)^([ \t]*)for\(int (\w+) = (?:int\()?(-?\d+)\)? ?; ?(\w+) ?(<|<=) ?(-?\d+) ?; ?(\w+)\+\+\)\n([ \t]*)\{\n",
        ),
        unroll_jump: re(r"\b(break|continue|return|discard|for|while|do)\b"),
        unroll_decl: re(&format!(r"(?m)^\s*(const\s+)?(highp |mediump |lowp )?{TYPES}\s+\w+")),
    }
});

/// Whether `re` (with `=(?!=)` written as `=(?:[^=]|\z)`) matches: look-ahead only at the end of
/// the pattern, where consuming the character it looks at does not change whether there is a match.
fn assigns(var: &str, indexed: bool, text: &str) -> bool {
    let target = if indexed {
        format!(r"\b{var}\s*\[[^\]]*\](\.\w+)?")
    } else {
        format!(r"\b{var}")
    };
    re(&format!(r"{target}\s*([-+*/%&|^]?=(?:[^=]|\z)|\+\+|--)")).is_match(text)
}

/// The next match of `rx` from `pos` whose `ok` holds, trying every later start as `re` does
/// where a back reference fails (the groups `ok` checks do not depend on how the rest matched).
fn find_checked<'t>(
    rx: &Regex,
    text: &'t str,
    mut pos: usize,
    ok: impl Fn(&Captures) -> bool,
) -> Option<Captures<'t>> {
    while pos <= text.len() {
        let c = rx.captures_at(text, pos)?;
        if ok(&c) {
            return Some(c);
        }
        let s = c.get(0).unwrap().start();
        pos = s + text[s..].chars().next().map_or(1, char::len_utf8);
    }
    None
}

/// [(stage, start, end)]: the text between '#ifdef <STAGE>' and its matching #endif.
pub fn stage_blocks(code: &str) -> Vec<(String, usize, usize)> {
    let r = &*RX;
    let lines = py::splitlines(code);
    let mut out = Vec::new();
    let (mut pos, mut i) = (0, 0);
    while i < lines.len() {
        let Some(m) = r.ifdef_stage.captures(lines[i]) else {
            pos += lines[i].len();
            i += 1;
            continue;
        };
        let stage = m[1].to_string();
        let start = pos + lines[i].len();
        let mut depth = 1;
        pos = start;
        i += 1;
        while i < lines.len() && depth > 0 {
            if r.if_open.is_match(lines[i]) {
                depth += 1;
            } else if r.endif_line.is_match(lines[i]) {
                depth -= 1;
                if depth == 0 {
                    out.push((stage.clone(), start, pos));
                }
            }
            pos += lines[i].len();
            i += 1;
        }
    }
    out
}

/// glsl._strip_comments: each comment replaced by a space.
pub fn strip_comments(s: &str) -> String {
    RX.comments.replace_all(s, " ").into_owned()
}

/// A stage's source without comments and preprocessor lines.
fn code_lines(body: &str) -> String {
    RX.preprocessor.replace_all(body, "").into_owned()
}

/// HLSLcc's `precise vec4 u_xlat_precise_vec4;` lines (and ivec4, bvec4, uvec4), where nothing
/// else in the stage names them.
fn drop_precise_dummies(mut src: String) -> String {
    let r = &*RX;
    let mut found = Vec::new();
    let mut pos = 0;
    while let Some(c) = find_checked(&r.precise_dummy, &src, pos, |c| {
        c[2].strip_prefix("u_xlat_precise_") == Some(&c[1])
    }) {
        let m = c.get(0).unwrap();
        found.push((m.as_str().to_string(), c[2].to_string()));
        pos = m.end();
    }
    for (line, name) in found {
        let rest = src.replacen(&line, "", 1);
        if !re(&format!(r"\b{name}\b")).is_match(&rest) {
            src = rest;
        }
    }
    src
}

/// Unsupported for an image without a format qualifier (desktop GL can store to one, ES cannot).
fn check_images(code: &str, stage: &str) -> Result<(), Unsupported> {
    let r = &*RX;
    for m in r.image_decl.captures_iter(code) {
        let has_format = r
            .layout_args
            .captures_iter(&m[1])
            .any(|q| r.image_format.is_match(&q[1]));
        if !has_format {
            return Err(Unsupported::new(format!(
                "{stage}: image without a format qualifier {} (GLSL ES needs the format, the program does not give it)",
                py::repr(&m[3])
            )));
        }
    }
    Ok(())
}

fn first_match(rules: &[Rule], code: &str) -> Option<(&'static str, String)> {
    rules.iter().find_map(|r| {
        r.re.captures(code).map(|c| {
            (
                r.what,
                py::strip(c.get(r.group).unwrap().as_str()).to_string(),
            )
        })
    })
}

/// One stage's source (starting with its #version line) -> GLSL ES 3.00 (`Es310`: 3.10). `notes`
/// gets the rewrites applied ("<STAGE> const-arrays <n>", "<STAGE> unroll <n>").
pub fn stage_to_es(
    src: &str,
    stage: &str,
    precision: &str,
    optimize: Optimize,
    notes: &mut Vec<String>,
    es: Es,
) -> Result<String, Unsupported> {
    let r = &*RX;
    let gl4 = r
        .version_head
        .captures(src)
        .is_some_and(|h| VERSIONS_4X.contains(&&h[1]));
    let src = if gl4 {
        drop_precise_dummies(src.to_string())
    } else {
        src.to_string()
    };
    let body = strip_comments(&src);
    let code = code_lines(&body);
    if let Some((what, m)) = first_match(
        if gl4 {
            &r.unsupported_4x
        } else {
            &r.unsupported
        },
        &code,
    ) {
        return Err(Unsupported::new(format!(
            "{stage}: {what} {}",
            py::repr(&m)
        )));
    }
    if gl4 {
        check_images(&code, stage)?;
    }
    if es == Es::Es300 {
        if let Some((what, m)) = first_match(if gl4 { &r.es31_4x } else { &r.es31 }, &code) {
            return Err(Unsupported {
                needs_es31: true,
                msg: format!("{stage}: {what} {} (GLSL ES 3.10)", py::repr(&m)),
            });
        }
    }
    let lines = py::splitlines(&src);
    let mut out = String::with_capacity(src.len() + 256);
    let mut i = 0;
    while i < lines.len() && py::strip(lines[i]).is_empty() {
        i += 1;
    }
    let Some(m) = lines.get(i).and_then(|l| r.version_line.captures(l)) else {
        return Err(Unsupported::new(format!("{stage}: no #version line")));
    };
    if !(VERSIONS.contains(&&m[1]) && m[2].is_empty()
        || gl4 && (m[2].is_empty() || &m[2] == "core"))
    {
        return Err(Unsupported::new(
            py::rstrip(&format!("{stage}: #version {} {}", &m[1], &m[2])).to_string(),
        ));
    }
    let drop: &[&str] = if gl4 {
        &DROP_EXTENSIONS_4X
    } else {
        &DROP_EXTENSIONS
    };
    out.push_str(&format!("#version {} es\n", es.as_str()));
    i += 1;
    // the extension directives right after #version; Unity 2019+ wraps the optional ones:
    // #ifdef GL_ARB_shader_bit_encoding / #extension ... / #endif
    while i < lines.len() {
        let s = py::strip(lines[i]);
        if let Some(e) = r.extension.captures(s) {
            if !drop.contains(&&e[1]) {
                out.push_str(lines[i]);
            }
            i += 1;
        } else if let Some(w) = r.ifdef_name.captures(s).filter(|w| {
            i + 2 < lines.len()
                && re(&format!(r"\A#\s*extension\s+{}\s*:", &w[1]))
                    .is_match(py::strip(lines[i + 1]))
                && r.endif.is_match(py::strip(lines[i + 2]))
        }) {
            if !drop.contains(&&w[1]) {
                for l in &lines[i..i + 3] {
                    out.push_str(l);
                }
            }
            i += 3;
        } else if s.is_empty() {
            i += 1;
        } else {
            break;
        }
    }
    out.push_str(&precision_block(&body, precision, gl4));
    let mut rest: String = lines[i.min(lines.len())..].concat();
    if gl4 && es == Es::Es300 && (stage == "VERTEX" || stage == "FRAGMENT") {
        let rx = if stage == "VERTEX" {
            &r.varying_vertex
        } else {
            &r.varying_fragment
        };
        rest = rx.replace_all(&rest, "${1}${2}").into_owned();
    }
    if optimize.const_arrays {
        let (s, n) = const_arrays(&rest);
        rest = s;
        if n > 0 {
            notes.push(format!("{stage} const-arrays {n}"));
        }
    }
    if optimize.unroll {
        let (s, n) = unroll(&rest, UNROLL_MAX, UNROLL_STATEMENTS);
        rest = s;
        if n > 0 {
            notes.push(format!("{stage} unroll {n}"));
        }
    }
    out.push_str(&rest);
    Ok(out)
}

/// Default precision statements for one stage (body: its source without comments); images: also
/// for the image types it declares (GLSL 4.x stages).
pub fn precision_block(body: &str, precision: &str, images: bool) -> String {
    let words: std::collections::HashSet<&str> =
        RX.word.find_iter(body).map(|m| m.as_str()).collect();
    let mut p = String::from("precision highp float;\nprecision highp int;\n");
    let extra: &[&str] = if images { &IMAGE_TYPES } else { &[] };
    for t in SAMPLER_TYPES.iter().chain(extra) {
        if words.contains(t) && (precision == "highp" || (*t != "sampler2D" && *t != "samplerCube"))
        {
            p.push_str(&format!("precision highp {t};\n"));
        }
    }
    p
}

/// Global arrays that main() fills with literals before anything else -> const arrays.
pub fn const_arrays(src: &str) -> (String, usize) {
    let r = &*RX;
    let mut src = src.to_string();
    let decls: Vec<(String, String, String, String)> = r
        .immcb_decl
        .captures_iter(&src)
        .map(|m| {
            (
                m[0].to_string(),
                m[1].to_string(),
                m[2].to_string(),
                m[3].to_string(),
            )
        })
        .collect();
    let mut done = 0;
    for (whole, typ, name, count) in decls {
        let Ok(n) = count.parse::<u64>() else {
            continue;
        };
        // element assignments: '<name>[i] = <typ>(<literals>);' or '<name>[i] = <literal>;'
        let rx = re(&format!(
            r"(?m)^[ \t]*{name}\[(\d+)\] = ((?:{TYPES})\((?:\s*{NUM}\s*,)*\s*{NUM}\s*\)|{NUM});\n"
        ));
        let assigns_at: Vec<(usize, usize, String, String)> = rx
            .captures_iter(&src)
            .map(|a| {
                let m = a.get(0).unwrap();
                (m.start(), m.end(), a[1].to_string(), a[2].to_string())
            })
            .collect();
        let mut vals: std::collections::BTreeMap<u64, Vec<String>> = Default::default();
        let mut huge = false;
        for (_, _, i, v) in &assigns_at {
            match i.parse::<u64>() {
                Ok(i) => vals.entry(i).or_default().push(v.clone()),
                Err(_) => huge = true,
            }
        }
        if huge
            || vals.len() as u64 != n
            || vals.keys().copied().ne(0..n)
            || vals.values().any(|v| v.len() != 1)
        {
            continue;
        }
        let rest = rx.replace_all(&src, "").into_owned();
        // any other write to the array (assignment, compound assignment, ++/--, out argument) -> leave it
        if assigns(&name, true, &rest) {
            continue;
        }
        let other = rest.replace(&whole, "");
        let bare = re(&format!(r"\b{name}\b"));
        let follows_index = re(r"\A\s*\[");
        if bare
            .find_iter(&other)
            .any(|m| !follows_index.is_match(&other[m.end()..]))
        {
            continue;
        }
        // the assignments must come first in main(), before any statement that could read the array
        let Some(mm) = r.main_open.find(&src) else {
            continue;
        };
        if assigns_at.iter().any(|a| a.0 < mm.end()) {
            continue;
        }
        let last = assigns_at.iter().map(|a| a.1).max().unwrap_or(mm.end());
        let head = src.get(mm.end()..last).unwrap_or("");
        if !py::strip(&rx.replace_all(head, "")).is_empty() {
            continue;
        }
        let items: Vec<String> = (0..n)
            .map(|i| {
                let v = &vals[&i][0];
                if v.contains('(') || typ == "float" || typ == "int" || typ == "uint" {
                    v.clone()
                } else {
                    format!("{typ}({v})")
                }
            })
            .collect();
        let ctor = format!("{typ}[{n}]({})", items.join(", "));
        src = rest.replace(&whole, &format!("const {typ} {name}[{n}] = {ctor};\n"));
        done += 1;
    }
    (src, done)
}

/// Loops with literal bounds and a plain body -> one copy of the body per trip (at most `limit`
/// trips and `statements` statements written out).
pub fn unroll(src: &str, limit: i128, statements: i128) -> (String, usize) {
    let r = &*RX;
    let mut src = src.to_string();
    let mut done = 0;
    let mut pos = 0;
    loop {
        let Some(m) = find_checked(&r.unroll_for, &src, pos, |c| {
            c[4] == c[2] && c[7] == c[2] && c[8] == c[1]
        }) else {
            break;
        };
        let (m_start, m_end) = (m.get(0).unwrap().start(), m.get(0).unwrap().end());
        let (ind, var, op) = (m[1].to_string(), m[2].to_string(), m[5].to_string());
        let (a, b) = (m[3].parse::<i128>().ok(), m[6].parse::<i128>().ok());
        drop(m);
        let close = re(&format!(r"(?m)^{}\}}\n", regex::escape(&ind)));
        let Some(end) = close.find_at(&src, m_end) else {
            pos = m_end;
            continue;
        };
        let (e_start, e_end) = (end.start(), end.end());
        let body = &src[m_end..e_start];
        let trips = match (a, b) {
            (Some(a), Some(b)) => Some((a, (if op == "<=" { b + 1 } else { b } - a).max(0))),
            _ => None,
        };
        let bad = match trips {
            None => true,
            Some((_, n)) => {
                !(0 < n && n <= limit)
                    || n * body.matches(';').count() as i128 > statements
                    || body.contains('{')
                    || body.contains('}')
                    || r.unroll_jump.is_match(body)
                    || r.unroll_decl.is_match(body)
                    || assigns(&var, false, body)
                    || re(&format!(r"(\+\+|--)\s*{var}\b")).is_match(body)
            }
        };
        if bad {
            pos = m_end;
            continue;
        }
        let (a, n) = trips.unwrap();
        let counter = re(&format!(r"\b{var}\b"));
        let out: String = (a..a + n)
            .map(|k| {
                counter
                    .replace_all(body, NoExpand(&k.to_string()))
                    .into_owned()
            })
            .collect();
        src = format!("{}{}{}", &src[..m_start], out, &src[e_end..]);
        pos = m_start + out.len();
        done += 1;
    }
    (src, done)
}

/// A whole GL program (every stage block) -> its GLSL ES 3.00 twin (`Es310`: 3.10, for programs
/// that are NeedsES31). Text outside the stage blocks is kept. `notes` gets the rewrites applied,
/// also those of stages translated before one that is Unsupported.
pub fn program_to_es(
    code: &str,
    precision: &str,
    optimize: Optimize,
    notes: &mut Vec<String>,
    es: Es,
) -> Result<String, Unsupported> {
    let blocks = stage_blocks(code);
    if blocks.is_empty() {
        return Err(Unsupported::new("no stage blocks"));
    }
    let mut out = String::with_capacity(code.len() + 512);
    let mut last = 0;
    for (stage, a, b) in blocks {
        if stage != "VERTEX" && stage != "FRAGMENT" {
            return Err(Unsupported::new(format!("{stage} stage")));
        }
        out.push_str(&code[last..a]);
        out.push_str(&stage_to_es(
            &code[a..b],
            &stage,
            precision,
            optimize,
            notes,
            es,
        )?);
        last = b;
    }
    out.push_str(&code[last..]);
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    const VS: &str = "#ifdef VERTEX\n#version 150\n#extension GL_ARB_explicit_attrib_location : require\n#extension GL_ARB_shader_bit_encoding : enable\n\nuniform vec4 _MainTex_ST;\nin vec4 in_POSITION0;\nout vec2 vs_TEXCOORD0;\nvoid main()\n{\n    gl_Position = in_POSITION0;\n    return;\n}\n#endif\n";
    const FS_HEAD: &str = "#ifdef FRAGMENT\n#version 150\n#extension GL_ARB_explicit_attrib_location : require\n#extension GL_ARB_shader_bit_encoding : enable\n\n";

    fn es(code: &str, precision: &str) -> (Result<String, Unsupported>, Vec<String>) {
        let mut notes = Vec::new();
        let r = program_to_es(code, precision, Optimize::ALL, &mut notes, Es::Es300);
        (r, notes)
    }

    #[test]
    fn header_and_precision() {
        let fs = format!(
            "{FS_HEAD}uniform  sampler2D _MainTex;\nuniform  samplerCube _Cube;\nuniform  sampler3D _Vol;\nin vec2 vs_TEXCOORD0;\nlayout(location = 0) out vec4 SV_Target0;\nvoid main()\n{{\n    SV_Target0 = texture(_MainTex, vs_TEXCOORD0);\n    return;\n}}\n#endif\n"
        );
        let (out, notes) = es(&format!("{VS}{fs}"), "highp");
        let out = out.unwrap();
        assert!(notes.is_empty());
        assert!(out.starts_with("#ifdef VERTEX\n#version 300 es\nprecision highp float;\nprecision highp int;\nuniform vec4"));
        assert!(out.contains("#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp sampler3D;\nprecision highp samplerCube;\nuniform  sampler2D"));
        assert!(!out.contains("GL_ARB"));
        let (out, _) = es(&format!("{VS}{fs}"), "default-samplers");
        assert!(out
            .unwrap()
            .contains("precision highp int;\nprecision highp sampler3D;\nuniform  sampler2D"));
    }

    #[test]
    fn wrapped_extensions() {
        let vs = "#ifdef VERTEX\n#version 150\n#ifdef GL_ARB_shader_bit_encoding\n#extension GL_ARB_shader_bit_encoding : enable\n#endif\n#ifdef GL_EXT_x\n#extension GL_EXT_x : enable\n#endif\nvoid main()\n{\n}\n#endif\n";
        let (out, _) = es(vs, "highp");
        let out = out.unwrap();
        assert!(!out.contains("shader_bit_encoding"));
        assert!(out.contains("#version 300 es\n#ifdef GL_EXT_x\n#extension GL_EXT_x : enable\n#endif\nprecision highp float;"));
    }

    #[test]
    fn unsupported_and_es31() {
        let fs = format!("{FS_HEAD}void main()\n{{\n    gl_FragColor = vec4(1.0);\n}}\n#endif\n");
        let (out, _) = es(&format!("{VS}{fs}"), "highp");
        assert_eq!(
            out.unwrap_err().msg,
            "FRAGMENT: fixed-function built-in 'gl_FragColor'"
        );
        let fs = format!("{FS_HEAD}uniform sampler2D t;\nnoperspective in vec2 uv;\nvoid main()\n{{\n}}\n#endif\n");
        let (out, _) = es(&format!("{VS}{fs}"), "highp");
        assert_eq!(out.unwrap_err().msg, "FRAGMENT: qualifier 'noperspective'");
        let fs = format!("{FS_HEAD}uniform sampler2D t;\nout vec4 o;\nvoid main()\n{{\n    o = textureGather (t, vec2(0.0));\n}}\n#endif\n");
        let (out, _) = es(&format!("{VS}{fs}"), "highp");
        let e = out.unwrap_err();
        assert!(e.needs_es31);
        assert_eq!(
            e.msg,
            "FRAGMENT: ES 3.1 function 'textureGather (' (GLSL ES 3.10)"
        );
        let mut notes = Vec::new();
        let ok = program_to_es(
            &format!("{VS}{fs}"),
            "highp",
            Optimize::ALL,
            &mut notes,
            Es::Es310,
        )
        .unwrap();
        assert!(ok.contains("#version 310 es\n"));
        let (out, _) = es("#ifdef GEOMETRY\n#version 150\n#endif\n", "highp");
        assert_eq!(out.unwrap_err().msg, "GEOMETRY stage");
        let (out, _) = es("#ifdef VERTEX\n#version 120\n#endif\n", "highp");
        assert_eq!(out.unwrap_err().msg, "VERTEX: #version 120");
        let (out, _) = es("void main(){}", "highp");
        assert_eq!(out.unwrap_err().msg, "no stage blocks");
    }

    #[test]
    fn const_arrays_rewrite() {
        let body = "vec4 ImmCB_0_0_0[3];\nuniform vec4 x;\nvoid main()\n{\n    ImmCB_0_0_0[0] = vec4(1.0, 0.0, 0.0, 0.0);\n    ImmCB_0_0_0[1] = vec4(0.5, 0.0, 0.0, 0.0);\n    ImmCB_0_0_0[2] = vec4(0.25, 0.0, 0.0, 0.0);\n    u = ImmCB_0_0_0[int(x.x)];\n}\n";
        let (out, n) = const_arrays(body);
        assert_eq!(n, 1);
        assert_eq!(out, "const vec4 ImmCB_0_0_0[3] = vec4[3](vec4(1.0, 0.0, 0.0, 0.0), vec4(0.5, 0.0, 0.0, 0.0), vec4(0.25, 0.0, 0.0, 0.0));\nuniform vec4 x;\nvoid main()\n{\n    u = ImmCB_0_0_0[int(x.x)];\n}\n");
        // a float array of bare literals, and one written again later (left as it is)
        let f = "float ImmCB_1[2];\nvoid main()\n{\n    ImmCB_1[0] = 1.0;\n    ImmCB_1[1] = -2.5e-3;\n    y = ImmCB_1[i];\n}\n";
        assert_eq!(const_arrays(f).0, "const float ImmCB_1[2] = float[2](1.0, -2.5e-3);\nvoid main()\n{\n    y = ImmCB_1[i];\n}\n");
        let w = "float ImmCB_1[2];\nvoid main()\n{\n    ImmCB_1[0] = 1.0;\n    ImmCB_1[1] = 2.0;\n    ImmCB_1[i] += 1.0;\n}\n";
        assert_eq!(const_arrays(w), (w.to_string(), 0));
        let cmp = "float ImmCB_1[2];\nvoid main()\n{\n    ImmCB_1[0] = 1.0;\n    ImmCB_1[1] = 2.0;\n    b = ImmCB_1[i] == 1.0;\n}\n";
        assert_eq!(const_arrays(cmp).1, 1);
        let bare = "vec2 ImmCB_2[1];\nvoid main()\n{\n    ImmCB_2[0] = 1.0;\n    f(ImmCB_2);\n}\n";
        assert_eq!(const_arrays(bare).1, 0);
        let late =
            "vec2 ImmCB_2[1];\nvoid main()\n{\n    a = 1;\n    ImmCB_2[0] = vec2(1.0, 2.0);\n}\n";
        assert_eq!(const_arrays(late).1, 0);
    }

    #[test]
    fn unroll_rewrite() {
        let s = "    u = 0.0;\n    for(int i = 0 ; i<3 ; i++)\n    {\n        u += w[i] * texture(t, uv + o[i]).x;\n    }\n    o = u;\n";
        let (out, n) = unroll(s, UNROLL_MAX, UNROLL_STATEMENTS);
        assert_eq!(n, 1);
        assert_eq!(out, "    u = 0.0;\n        u += w[0] * texture(t, uv + o[0]).x;\n        u += w[1] * texture(t, uv + o[1]).x;\n        u += w[2] * texture(t, uv + o[2]).x;\n    o = u;\n");
        // the counter written, a declaration, too many trips, a mismatched counter: loops kept
        for body in [
            "    for(int i = 0 ; i<3 ; i++)\n    {\n        i = 2;\n    }\n",
            "    for(int i = 0 ; i<3 ; i++)\n    {\n        float x = 1.0;\n    }\n",
            "    for(int i = 0 ; i<17 ; i++)\n    {\n        a += 1.0;\n    }\n",
            "    for(int i = 0 ; j<3 ; i++)\n    {\n        a += 1.0;\n    }\n",
            "    for(int i = 0 ; i<3 ; i++)\n      {\n        a += 1.0;\n    }\n",
        ] {
            assert_eq!(
                unroll(body, UNROLL_MAX, UNROLL_STATEMENTS),
                (body.to_string(), 0),
                "{body}"
            );
        }
        // <= and int() bounds; a comparison with the counter is not a write
        let s = "for(int j = int(-1) ; j<=1 ; j++)\n{\n    b = j == 0 ? x : y;\n}\n";
        assert_eq!(
            unroll(s, UNROLL_MAX, UNROLL_STATEMENTS).0,
            "    b = -1 == 0 ? x : y;\n    b = 0 == 0 ? x : y;\n    b = 1 == 0 ? x : y;\n"
        );
    }

    #[test]
    fn notes_of_each_stage() {
        let fs = format!("{FS_HEAD}out vec4 o;\nvoid main()\n{{\n    for(int i = 0 ; i<2 ; i++)\n    {{\n        o += vec4(i);\n    }}\n}}\n#endif\n");
        let (out, notes) = es(&format!("{VS}{fs}"), "highp");
        assert!(out.is_ok());
        assert_eq!(notes, vec!["FRAGMENT unroll 1".to_string()]);
    }

    #[test]
    fn glsl4x_rules() {
        let vs = "#ifdef VERTEX\n#version 410\n#extension GL_ARB_shading_language_420pack : require\nprecise vec4 u_xlat_precise_vec4;\nprecise ivec4 u_xlat_precise_ivec4;\nlayout(location = 0) out vec2 vs_TEXCOORD0;\nvoid main()\n{\n    vs_TEXCOORD0 = vec2(0.0);\n}\n#endif\n";
        let (out, _) = es(vs, "highp");
        let out = out.unwrap();
        assert_eq!(out, "#ifdef VERTEX\n#version 300 es\nprecision highp float;\nprecision highp int;\nout vec2 vs_TEXCOORD0;\nvoid main()\n{\n    vs_TEXCOORD0 = vec2(0.0);\n}\n#endif\n");
        let img =
            "#ifdef FRAGMENT\n#version 450\nuniform image2D img;\nvoid main()\n{\n}\n#endif\n";
        assert_eq!(es(img, "highp").0.unwrap_err().msg, "FRAGMENT: image without a format qualifier 'img' (GLSL ES needs the format, the program does not give it)");
        let blk = "#ifdef VERTEX\n#version 410\nout gl_PerVertex {\n  vec4 gl_Position;\n};\nvoid main()\n{\n}\n#endif\n";
        assert_eq!(
            es(blk, "highp").0.unwrap_err().msg,
            "VERTEX: interface block 'out gl_PerVertex {'"
        );
    }

    #[test]
    fn python_whitespace_in_patterns() {
        assert_eq!(py_pattern(r"a\s*[\s,]"), r"a[\s\x1c-\x1f]*[\s\x1c-\x1f,]");
        assert_eq!(py_pattern(r"[^\]]\s"), r"[^\]][\s\x1c-\x1f]");
        assert!(
            stage_blocks("#ifdef VERTEX\n#if X\n#endif\n#endif\n#ifdef FRAGMENT\n#endif\n").len()
                == 2
        );
    }
}
