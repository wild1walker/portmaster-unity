//! `shaders.fused_post`'s program (shaders/fusedpost.py; its docstring has the rules): a chain of
//! full-screen post-processing passes' GLES3 programs as one program, the last pass's vertex stage
//! and a fragment stage that runs every pass's main() in turn. The Shader object's side (the pass
//! appended to the chain's last shader) is the convert crate's.
use crate::glsl;
use crate::py;
use regex::{NoExpand, Regex};
use std::sync::LazyLock;

pub const MARK: &str = "// pmunity fused_post:";
pub const DEFAULT_NAME: &str = "PMUNITY_FUSED_POST";
pub const QUANTIZE: [&str; 2] = ["rgba8", "none"];
const MAIN_TEX: &str = "_MainTex";
const STORE_FN: &str = "pmunity_store";

/// Why a chain is left as it is (fusedpost.NotFusable).
#[derive(Clone, Debug, PartialEq)]
pub struct NotFusable(pub String);

impl std::fmt::Display for NotFusable {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

type R<T> = Result<T, NotFusable>;

fn nf<T>(m: String) -> R<T> {
    Err(NotFusable(m))
}

struct Patterns {
    uniform: Regex,
    input: Regex,
    output: Regex,
    global: Regex,
    precision: Regex,
    main: Regex,
    ret: Regex,
    own_read: Regex,
    vs_pass: Regex,
    comment: Regex,
    space: Regex,
}

static RX: LazyLock<Patterns> = LazyLock::new(|| Patterns {
    uniform: glsl::re(
        r"\A\s*(?:UNITY_LOCATION\(\s*\d+\s*\)\s+)?uniform\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*(\[\s*\d+\s*\])?\s*;\s*\n?\z",
    ),
    input: glsl::re(
        r"\A\s*(?:flat\s+|centroid\s+)?in\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*;\s*\n?\z",
    ),
    output: glsl::re(
        r"\A\s*layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*out\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*;\s*\n?\z",
    ),
    global: glsl::re(
        r"\A\s*(?:(?:lowp|mediump|highp)\s+)?((?:[biud]?vec[234]|mat[234](?:x[234])?|float|int|uint|bool))\s+(\w+)\s*(\[\s*\d+\s*\])?\s*;\s*\n?\z",
    ),
    precision: glsl::re(r"\A\s*precision\s+(lowp|mediump|highp)\s+(\w+)\s*;\s*\n?\z"),
    main: glsl::re(r"(?m)^\s*void\s+main\s*\(\s*\)\s*\{"),
    ret: glsl::re(r"\breturn\s*;"),
    own_read: glsl::re(&format!(
        r"texture\s*\(\s*{MAIN_TEX}\s*,\s*vs_TEXCOORD0(?:\.xy)?\s*\)"
    )),
    vs_pass: glsl::re(r"\bvs_TEXCOORD0(?:\.xy)?\s*=\s*in_TEXCOORD0(?:\.xy)?\s*;"),
    comment: glsl::re(r"//[^\n]*"),
    space: glsl::re(r"\s+"),
});

const SAMPLER_TYPES: [&str; 12] = [
    "sampler2D",
    "sampler3D",
    "samplerCube",
    "sampler2DArray",
    "sampler2DShadow",
    "samplerCubeShadow",
    "isampler2D",
    "usampler2D",
    "isampler3D",
    "usampler3D",
    "isampler2DArray",
    "usampler2DArray",
];

fn word(w: &str) -> Regex {
    glsl::re(&format!(r"\b{}\b", regex::escape(w)))
}

/// One pass's fragment stage, taken apart (fusedpost.Fragment).
pub struct Fragment {
    label: String,
    version: Option<String>,
    precision: Vec<String>,
    extensions: Vec<String>,
    macros: Vec<String>,
    uniforms: Vec<(String, String, String)>,
    samplers: Vec<(String, String)>,
    ins: Vec<(String, String)>,
    globals: Vec<(String, String, String)>,
    output: Option<(String, String)>,
    body: String,
}

impl Fragment {
    pub fn new(src: &str, label: &str) -> R<Fragment> {
        let r = &*RX;
        let Some(m) = r.main.find(src) else {
            return nf(format!("{label}: no main()"));
        };
        let (head, rest) = (&src[..m.start()], &src[m.end()..]);
        let trimmed = py::rstrip(rest);
        let end = trimmed.rfind('}');
        let Some(end) = end.filter(|&e| py::strip(&trimmed[e..]) == "}") else {
            return nf(format!("{label}: main() does not end the stage"));
        };
        let mut f = Fragment {
            label: label.to_string(),
            version: None,
            precision: Vec::new(),
            extensions: Vec::new(),
            macros: Vec::new(),
            uniforms: Vec::new(),
            samplers: Vec::new(),
            ins: Vec::new(),
            globals: Vec::new(),
            output: None,
            body: rest[..end].to_string(),
        };
        f.read_head(head)?;
        if f.output.is_none() {
            return nf(format!("{label}: no colour output"));
        }
        for bad in [
            "gl_FragCoord",
            "gl_FragDepth",
            "discard",
            "gl_PointCoord",
            "gl_FrontFacing",
            "gl_SampleID",
        ] {
            if word(bad).is_match(&f.body) {
                return nf(format!("{label}: uses {bad}"));
            }
        }
        Ok(f)
    }

    fn read_head(&mut self, head: &str) -> R<()> {
        let r = &*RX;
        for line in py::splitlines(head) {
            let s = py::strip(line);
            if s.is_empty() || s.starts_with("//") {
                continue;
            }
            if s.starts_with('#') {
                if s.starts_with("#version") {
                    self.version = Some(s.to_string());
                } else if s.starts_with("#extension") {
                    self.extensions.push(s.to_string());
                } else {
                    self.macros.push(s.to_string());
                }
                continue;
            }
            if r.precision.is_match(s) {
                self.precision.push(s.to_string());
                continue;
            }
            if let Some(m) = r.uniform.captures(s) {
                let (typ, name) = (m[1].to_string(), m[2].to_string());
                let arr = m
                    .get(3)
                    .map_or(String::new(), |a| a.as_str().replace(' ', ""));
                if SAMPLER_TYPES.contains(&typ.as_str()) {
                    if m.get(3).is_some_and(|a| !a.as_str().is_empty()) {
                        return nf(format!("{}: sampler array {name}", self.label));
                    }
                    self.samplers.push((typ, name));
                } else {
                    self.uniforms.push((typ, name, arr));
                }
                continue;
            }
            if let Some(m) = r.output.captures(s) {
                if self.output.is_some() || &m[1] != "0" {
                    return nf(format!("{}: more than one output", self.label));
                }
                self.output = Some((m[2].to_string(), m[3].to_string()));
                continue;
            }
            if let Some(m) = r.input.captures(s) {
                self.ins.push((m[1].to_string(), m[2].to_string()));
                continue;
            }
            if let Some(m) = r.global.captures(s) {
                let arr = m
                    .get(3)
                    .map_or(String::new(), |a| a.as_str().replace(' ', ""));
                self.globals.push((m[1].to_string(), m[2].to_string(), arr));
                continue;
            }
            if s.contains("uniform") && s.contains('{')
                || s.starts_with("layout") && s.contains("uniform")
            {
                return nf(format!("{}: uniform block ({})", self.label, prefix(s, 40)));
            }
            return nf(format!(
                "{}: declaration not understood: {}",
                self.label,
                prefix(s, 60)
            ));
        }
        Ok(())
    }

    fn names(&self) -> Vec<&str> {
        self.uniforms
            .iter()
            .map(|(_, n, _)| n.as_str())
            .chain(self.samplers.iter().map(|(_, n)| n.as_str()))
            .collect()
    }

    /// The stage's main() as `<type> pmunity_pass<index>([vec4 pmunity_in])`.
    fn function(&self, index: usize) -> R<String> {
        let r = &*RX;
        let (otype, oname) = self.output.clone().unwrap();
        let mut body = word(&oname)
            .replace_all(&self.body, NoExpand("pmunity_out"))
            .into_owned();
        if index > 0 {
            let n = r.own_read.find_iter(&body).count();
            if n == 0 {
                return nf(format!(
                    "{}: does not read {MAIN_TEX} at its own pixel",
                    self.label
                ));
            }
            body = r
                .own_read
                .replace_all(&body, NoExpand("pmunity_in"))
                .into_owned();
            if word(MAIN_TEX).is_match(&body) {
                return nf(format!(
                    "{}: reads {MAIN_TEX} other than at its own pixel",
                    self.label
                ));
            }
            for bad in [
                "dFdx",
                "dFdy",
                "fwidth",
                "dFdxFine",
                "dFdyFine",
                "dFdxCoarse",
                "dFdyCoarse",
                "fwidthFine",
                "fwidthCoarse",
            ] {
                if word(bad).is_match(&body) {
                    return nf(format!("{}: uses {bad}", self.label));
                }
            }
        }
        let n = r.ret.find_iter(&body).count();
        let body = r
            .ret
            .replace_all(&body, NoExpand("return pmunity_out;"))
            .into_owned();
        let mut lines = vec![
            format!(
                "{otype} pmunity_pass{index}({})",
                if index > 0 { "vec4 pmunity_in" } else { "" }
            ),
            "{".to_string(),
        ];
        for (t, nm, arr) in &self.globals {
            lines.push(format!("    {t} {nm}{arr};"));
        }
        lines.push(format!("    {otype} pmunity_out;"));
        lines.push(body.trim_matches('\n').to_string());
        if n == 0 {
            lines.push("    return pmunity_out;".into());
        }
        lines.push("}".into());
        Ok(lines.join("\n") + "\n")
    }
}

/// `s[:n]` in characters.
fn prefix(s: &str, n: usize) -> String {
    s.chars().take(n).collect()
}

/// The fused fragment stage of the passes' fragment stages, in chain order (fuse_fragment).
pub fn fuse_fragment(frags: &[Fragment], quantize: &str) -> R<String> {
    let first = &frags[0];
    for f in &frags[1..] {
        if f.version != first.version {
            return nf(format!(
                "{}: #version differs from {}",
                f.label, first.label
            ));
        }
        let (mut a, mut b) = (f.precision.clone(), first.precision.clone());
        a.sort();
        b.sort();
        if a != b {
            return nf(format!(
                "{}: precision statements differ from {}",
                f.label, first.label
            ));
        }
        if f.output.as_ref().map(|o| &o.0) != first.output.as_ref().map(|o| &o.0) {
            return nf(format!(
                "{}: output type differs from {}",
                f.label, first.label
            ));
        }
    }
    let mut owners: Vec<(String, String)> = Vec::new();
    let (mut uniforms, mut samplers, mut ins, mut exts) =
        (Vec::new(), Vec::new(), Vec::new(), Vec::new());
    for (i, f) in frags.iter().enumerate() {
        for name in f.names() {
            if name == MAIN_TEX {
                if i > 0 {
                    continue;
                }
            } else if i > 0 && name.starts_with(MAIN_TEX) {
                return nf(format!("{}: reads {name} (of the pass before it)", f.label));
            } else if let Some((_, o)) = owners.iter().find(|(n, _)| n == name) {
                return nf(format!("{}: {name} is also {o}'s", f.label));
            }
            match owners.iter_mut().find(|(n, _)| n == name) {
                Some(e) => e.1 = f.label.clone(),
                None => owners.push((name.to_string(), f.label.clone())),
            }
        }
        uniforms.extend(
            f.uniforms
                .iter()
                .filter(|u| i == 0 || u.1 != MAIN_TEX)
                .cloned(),
        );
        samplers.extend(
            f.samplers
                .iter()
                .filter(|s| i == 0 || s.1 != MAIN_TEX)
                .cloned(),
        );
        // (`ins += [v for v in f.ins if v not in ins]`: against the list before this pass's)
        let new_ins: Vec<_> = f
            .ins
            .iter()
            .filter(|v| !ins.contains(*v))
            .cloned()
            .collect();
        ins.extend(new_ins);
        let new_exts: Vec<_> = f
            .extensions
            .iter()
            .filter(|e| !exts.contains(*e))
            .cloned()
            .collect();
        exts.extend(new_exts);
    }
    if !first.samplers.iter().any(|(_, n)| n == MAIN_TEX) {
        return nf(format!("{}: no {MAIN_TEX} sampler", first.label));
    }
    samplers.sort_by_key(|s| s.1 != MAIN_TEX);
    let (otype, oname) = first.output.clone().unwrap();
    let mut out: Vec<String> = Vec::new();
    out.push(first.version.clone().map_or("None".into(), |v| v));
    out.extend(first.precision.iter().cloned());
    out.extend(exts);
    out.extend(first.macros.iter().cloned());
    out.push(format!(
        "{MARK} {} drawn as one pass{}",
        frags
            .iter()
            .map(|f| f.label.as_str())
            .collect::<Vec<_>>()
            .join(", "),
        if quantize == "rgba8" {
            " (the stores between them rounded to 8 bits)"
        } else {
            ""
        }
    ));
    for (t, n, a) in &uniforms {
        out.push(format!("uniform {t} {n}{a};"));
    }
    for (i, (t, n)) in samplers.iter().enumerate() {
        out.push(format!("UNITY_LOCATION({i}) uniform {t} {n};"));
    }
    for (t, n) in &ins {
        out.push(format!("in {t} {n};"));
    }
    out.push(format!("layout(location = 0) out {otype} {oname};"));
    if quantize == "rgba8" {
        out.push(format!(
            "{otype} {STORE_FN}({otype} c) {{ return floor(clamp(c, 0.0, 1.0) * 255.0 + 0.5) / 255.0; }}"
        ));
    }
    for (i, f) in frags.iter().enumerate() {
        out.push(f.function(i)?);
    }
    out.push("void main()".into());
    out.push("{".into());
    out.push(format!("    {otype} pmunity_c = pmunity_pass0();"));
    for i in 1..frags.len() {
        if quantize == "rgba8" {
            out.push(format!("    pmunity_c = {STORE_FN}(pmunity_c);"));
        }
        out.push(format!("    pmunity_c = pmunity_pass{i}(pmunity_c);"));
    }
    out.push(format!("    {oname} = pmunity_c;"));
    out.push("    return;".into());
    out.push("}".into());
    out.push(String::new());
    Ok(out.join("\n"))
}

fn canon(src: &str) -> String {
    let r = &*RX;
    let s = r.comment.replace_all(src, "");
    py::strip(&r.space.replace_all(&s, " ")).to_string()
}

/// The fused GLES3 program of the passes' programs: the last pass's vertex stage, the fused
/// fragment stage (fuse_program).
pub fn fuse_program(codes: &[String], labels: &[String], quantize: &str) -> R<String> {
    let mut stages: Vec<(String, String)> = Vec::new();
    for (code, label) in codes.iter().zip(labels) {
        let blocks = glsl::stage_blocks(code);
        let mut vert = None;
        let mut frag = None;
        let mut names = std::collections::BTreeSet::new();
        for (st, a, b) in blocks {
            names.insert(st.clone());
            match st.as_str() {
                "VERTEX" => vert = Some(code[a..b].to_string()),
                "FRAGMENT" => frag = Some(code[a..b].to_string()),
                _ => {}
            }
        }
        if names.len() != 2 || vert.is_none() || frag.is_none() {
            return nf(format!("{label}: not a vertex + fragment program"));
        }
        stages.push((vert.unwrap(), frag.unwrap()));
    }
    let last_label = labels.last().unwrap();
    let vert = canon(&stages.last().unwrap().0);
    for ((v, _), label) in stages.iter().zip(labels) {
        if canon(v) != vert {
            return nf(format!(
                "{label}: its vertex stage differs from {last_label}'s"
            ));
        }
    }
    if !RX.vs_pass.is_match(&vert) {
        return nf(format!(
            "{last_label}: the vertex stage does not pass the quad's texcoord through (vs_TEXCOORD0 = in_TEXCOORD0)"
        ));
    }
    let frags = stages
        .iter()
        .zip(labels)
        .map(|((_, f), label)| Fragment::new(f, label))
        .collect::<R<Vec<_>>>()?;
    let last = codes.last().unwrap();
    let (fa, fb) = glsl::stage_blocks(last)
        .into_iter()
        .find(|(st, _, _)| st == "FRAGMENT")
        .map(|(_, a, b)| (a, b))
        .unwrap();
    Ok(format!(
        "{}{}{}",
        &last[..fa],
        fuse_fragment(&frags, quantize)?,
        &last[fb..]
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    const VS: &str = "#ifdef VERTEX\n#version 300 es\nin highp vec4 in_POSITION0;\nin highp vec2 in_TEXCOORD0;\nout highp vec2 vs_TEXCOORD0;\nvoid main()\n{\n    vs_TEXCOORD0.xy = in_TEXCOORD0.xy;\n    gl_Position = in_POSITION0;\n    return;\n}\n#endif\n";

    fn pass(frag_body: &str, uniforms: &str) -> String {
        format!("{VS}#ifdef FRAGMENT\n#version 300 es\nprecision highp float;\nprecision highp int;\n{uniforms}UNITY_LOCATION(0) uniform mediump sampler2D _MainTex;\nin highp vec2 vs_TEXCOORD0;\nlayout(location = 0) out mediump vec4 SV_Target0;\nvec4 u_xlat0;\nvoid main()\n{{\n{frag_body}    return;\n}}\n\n#endif\n")
    }

    #[test]
    fn fuses_two_passes() {
        let a = pass(
            "    u_xlat0 = texture(_MainTex, vs_TEXCOORD0.xy);\n    SV_Target0 = u_xlat0 * _A;\n",
            "uniform \tfloat _A;\n",
        );
        let b = pass(
            "    u_xlat0 = texture(_MainTex, vs_TEXCOORD0.xy);\n    SV_Target0 = u_xlat0 + _B;\n",
            "uniform \tfloat _B;\n",
        );
        let out = fuse_program(&[a.clone(), b], &["a".into(), "b".into()], "rgba8").unwrap();
        assert!(out.contains("// pmunity fused_post: a, b drawn as one pass (the stores between them rounded to 8 bits)"));
        assert!(out.contains("vec4 pmunity_pass1(vec4 pmunity_in)\n{\n    vec4 u_xlat0;\n    vec4 pmunity_out;\n    u_xlat0 = pmunity_in;"));
        assert!(out.contains("    pmunity_c = pmunity_store(pmunity_c);\n    pmunity_c = pmunity_pass1(pmunity_c);\n    SV_Target0 = pmunity_c;"));
        let c = pass(
            "    SV_Target0 = texture(_MainTex, vs_TEXCOORD0.xy + vec2(0.1));\n",
            "uniform \tfloat _C;\n",
        );
        assert_eq!(
            fuse_program(&[a, c], &["a".into(), "c".into()], "rgba8")
                .unwrap_err()
                .0,
            "c: does not read _MainTex at its own pixel"
        );
    }
}
