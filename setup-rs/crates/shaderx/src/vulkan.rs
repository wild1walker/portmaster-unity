//! Vulkan programs (SPIR-V) of Unity Shader assets -> GLSL ES programs with Khronos' SPIRV-Cross
//! (pmspirvcross), as shaders/vulkan.py does it (its docstring has the layouts and names): the
//! entry's stage modules decoded from SMOL-V, their interface and resources given the names
//! Unity's GL programs have (spirv.rs), compiled to GLSL ES and written in Unity's GL form.
use crate::bindings::{
    Bindings, ConstantBuffer, Param, KIND_CBUFFER, KIND_SAMPLER, KIND_TEXTURE, PACKED_TEXTURE_FROM,
};
use crate::glsl;
use crate::py;
use crate::spirv::{self as sp, Module};
use regex::{NoExpand, Regex};
use std::collections::BTreeMap;
use std::io::{BufRead, BufReader, Read, Write};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::LazyLock;

/// A program this path cannot give a GL twin (vulkan.Unsupported), or (needs_es31) one whose
/// stage uses what GLSL ES 3.10 has (glsl.NeedsES31 out of _check_es).
#[derive(Clone, Debug, PartialEq)]
pub struct Unsupported {
    pub msg: String,
    pub needs_es31: bool,
    /// Raised by glsl's rules (glsl.Unsupported: not retried as GLSL ES 3.10).
    pub glsl: bool,
}

impl std::fmt::Display for Unsupported {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.msg)
    }
}

type R<T> = Result<T, Unsupported>;

fn uns<T>(m: impl Into<String>) -> R<T> {
    Err(Unsupported {
        msg: m.into(),
        needs_es31: false,
        glsl: false,
    })
}

pub const GPU_SPIRV: i64 = 25;
const MAX_SLOTS: usize = 9;
const ATTRIB_BASE: i32 = 13;
const ATTRIB_BASE_2017: i32 = 12;
const NEW_CHANNELS_FROM: i32 = 201802150;

fn channels(new: bool) -> Vec<String> {
    if new {
        let mut v: Vec<String> = ["in_POSITION0", "in_NORMAL0", "in_TANGENT0", "in_COLOR0"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        v.extend((0..8).map(|i| format!("in_TEXCOORD{i}")));
        v.push("in_BLENDWEIGHTS0".into());
        v.push("in_BLENDINDICES0".into());
        v
    } else {
        [
            "in_POSITION0",
            "in_NORMAL0",
            "in_COLOR0",
            "in_TEXCOORD0",
            "in_TEXCOORD1",
            "in_TEXCOORD2",
            "in_TEXCOORD3",
            "in_TANGENT0",
        ]
        .iter()
        .map(|s| s.to_string())
        .collect()
    }
}

/// HLSL's fine / coarse derivatives -> GLSL ES's one kind.
const DERIVATIVES: [(u32, u32); 6] = [
    (210, 207),
    (213, 207),
    (211, 208),
    (214, 208),
    (212, 209),
    (215, 209),
];

/// A Vulkan entry's code -> (flags, [(slot, module bytes)]).
#[allow(clippy::type_complexity)]
pub fn split_code(code: &[u8]) -> R<(i32, Vec<(usize, Vec<u8>)>)> {
    if code.len() < 12 {
        return uns("Vulkan program code too short");
    }
    let i32_at = |o: usize| i32::from_le_bytes(code[o..o + 4].try_into().unwrap());
    let flags = i32_at(0);
    let (mut first, mut p, mut mods, mut slot) = (code.len(), 4usize, Vec::new(), 0usize);
    while p + 8 <= first && slot < MAX_SLOTS {
        let (off, size) = (i32_at(p), i32_at(p + 4));
        if size != 0 {
            if size < 0
                || (off as i64) < (p + 8) as i64
                || off as i64 + size as i64 > code.len() as i64
            {
                return uns(format!(
                    "Vulkan program code: stage table entry {slot} out of range"
                ));
            }
            first = first.min(off as usize);
            mods.push((
                slot,
                code[off as usize..off as usize + size as usize].to_vec(),
            ));
        }
        p += 8;
        slot += 1;
    }
    if mods.is_empty() {
        return uns("Vulkan program without stages");
    }
    Ok((flags, mods))
}

static GLOBALS: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"^(?:\$Globals|[A-Z]Globals\d*)\n?\z").unwrap());

pub fn is_globals(name: &str) -> bool {
    GLOBALS.is_match(name)
}

// ---------------------------------------------------------------- pmspirvcross

/// $PMSPIRVCROSS, else pmspirvcross on PATH.
pub fn find_spirv_cross() -> Option<String> {
    if let Ok(p) = std::env::var("PMSPIRVCROSS") {
        if !p.is_empty() {
            return Some(p);
        }
    }
    let path = std::env::var("PATH").ok()?;
    path.split(':')
        .map(|d| std::path::Path::new(d).join("pmspirvcross"))
        .find(|p| p.is_file())
        .map(|p| p.to_string_lossy().into_owned())
}

/// A pmspirvcross server process (restarted if it dies on a module).
pub struct Translator {
    exe: String,
    proc: Option<(Child, ChildStdin, BufReader<ChildStdout>)>,
    pub crashes: usize,
}

impl Translator {
    pub fn new(exe: Option<String>) -> Result<Translator, String> {
        match exe.or_else(find_spirv_cross) {
            Some(e) if std::path::Path::new(&e).exists() => Ok(Translator { exe: e, proc: None, crashes: 0 }),
            _ => Err("pmspirvcross not found: build it with tools/spirvcross/build.sh or set PMSPIRVCROSS".into()),
        }
    }

    pub fn close(&mut self) {
        if let Some((mut child, mut stdin, _)) = self.proc.take() {
            let _ = stdin.write_all(b"q 0 0\n");
            drop(stdin);
            let _ = child.wait();
        }
    }

    fn request(&mut self, op: char, arg: i64, data: &[u8]) -> R<Vec<u8>> {
        if self.proc.is_none() {
            let mut child = Command::new(&self.exe)
                .arg("-s")
                .stdin(Stdio::piped())
                .stdout(Stdio::piped())
                .spawn()
                .map_err(|e| Unsupported {
                    msg: format!("pmspirvcross: {e}"),
                    needs_es31: false,
                    glsl: false,
                })?;
            let stdin = child.stdin.take().unwrap();
            let stdout = BufReader::new(child.stdout.take().unwrap());
            self.proc = Some((child, stdin, stdout));
        }
        let res = (|| -> Option<(i64, Vec<u8>, String)> {
            let (_, stdin, stdout) = self.proc.as_mut().unwrap();
            stdin
                .write_all(format!("{op} {arg} {}\n", data.len()).as_bytes())
                .ok()?;
            stdin.write_all(data).ok()?;
            stdin.flush().ok()?;
            let mut line = String::new();
            stdout.read_line(&mut line).ok()?;
            let parts: Vec<&str> = line.split_whitespace().collect();
            if parts.len() < 3 {
                return None;
            }
            let (status, n, k): (i64, usize, usize) = (
                parts[0].parse().ok()?,
                parts[1].parse().ok()?,
                parts[2].parse().ok()?,
            );
            let mut out = vec![0u8; n];
            stdout.read_exact(&mut out).ok()?;
            let mut log = vec![0u8; k];
            stdout.read_exact(&mut log).ok()?;
            Some((status, out, String::from_utf8_lossy(&log).into_owned()))
        })();
        let Some((status, out, log)) = res else {
            if let Some((mut child, _, _)) = self.proc.take() {
                let _ = child.kill();
                let _ = child.wait();
            }
            self.crashes += 1;
            return uns("SPIRV-Cross crashed on this module");
        };
        if status != 0 {
            let lines: Vec<&str> = log
                .lines()
                .map(|l| l.strip_prefix("error: ").unwrap_or(l))
                .collect();
            let m = if lines.is_empty() {
                "failed".to_string()
            } else {
                lines.join("; ")
            };
            return uns(format!("SPIRV-Cross: {m}"));
        }
        Ok(out)
    }

    /// SMOL-V (or SPIR-V) -> SPIR-V bytes (older: Unity 2017 - 2019's encoding tried too).
    pub fn decode(&mut self, module: &[u8], older: bool) -> R<Vec<u8>> {
        self.request('d', older as i64, module)
    }

    /// SPIR-V -> GLSL ES text (es 300 or 310).
    pub fn compile(&mut self, spirv: &[u8], es: i64) -> R<String> {
        let out = self.request('c', es, spirv)?;
        String::from_utf8(out).map_err(|_| Unsupported {
            msg: "SPIRV-Cross: not UTF-8".into(),
            needs_es31: false,
            glsl: false,
        })
    }
}

impl Drop for Translator {
    fn drop(&mut self) {
        self.close();
    }
}

// ---------------------------------------------------------------- naming

struct Block {
    cb_name: String,
    globals: bool,
}

/// (base kind, components, array length) of a member type.
fn vec_shape(m: &Module, tid: u32) -> (&'static str, u32, u32) {
    let Some(mut t) = m.types.get(&tid) else {
        return ("?", 0, 0);
    };
    let mut n = 0;
    if t.kind == "array" {
        n = m.array_length(t).unwrap_or(0);
        match m.types.get(&t.elem) {
            Some(e) => t = e,
            None => return ("?", 0, n),
        }
    }
    if t.kind == "vector" {
        return (m.types.get(&t.elem).map_or("?", |e| e.kind), t.count, n);
    }
    if matches!(t.kind, "float" | "int" | "bool") {
        return (t.kind, 1, n);
    }
    (t.kind, 0, n)
}

fn member_name(
    m: &Module,
    k: usize,
    mt: u32,
    p: Option<&Param>,
    where_: &str,
) -> R<Option<String>> {
    let (kind, comps, n) = vec_shape(m, mt);
    if kind == "matrix" {
        return uns(format!(
            "{where_}: matrix member {} (SPIR-V not from HLSLcc, e.g. DXC)",
            p.map_or(k.to_string(), |p| p.name.clone())
        ));
    }
    let Some(p) = p else { return Ok(None) };
    if p.is_matrix != 0 {
        let want = (p.cols * p.array.max(1)) as u32;
        if comps != 4 || n < want || (p.cols != 0 && n % p.cols as u32 != 0) {
            return uns(format!(
                "{where_}: matrix {}: SPIR-V member {}, expected vec4[{want}]",
                p.name,
                m.describe(mt)
            ));
        }
        return Ok(Some(format!("hlslcc_mtx{}x{}{}", p.rows, p.cols, p.name)));
    }
    if comps != p.cols as u32 || (p.array > 1 && n < p.array as u32) {
        return uns(format!(
            "{where_}: {}: SPIR-V member {}, Unity says {} components x {}",
            p.name,
            m.describe(mt),
            p.cols,
            if p.array != 0 { p.array } else { 1 }
        ));
    }
    Ok(Some(p.name.clone()))
}

/// What the program's stages are named by (vulkan.Plan).
struct Plan<'a> {
    cb_at: BTreeMap<i32, String>,
    cbs: BTreeMap<String, &'a ConstantBuffer>,
    tex_at: BTreeMap<i32, &'a crate::bindings::Binding>,
    sampler_at: BTreeMap<i32, String>,
    packed: bool,
    loc_channel: BTreeMap<i32, String>,
}

impl<'a> Plan<'a> {
    fn new(b: &'a Bindings, serializer: Option<i32>) -> Plan<'a> {
        let mut p = Plan {
            cb_at: BTreeMap::new(),
            cbs: BTreeMap::new(),
            tex_at: BTreeMap::new(),
            sampler_at: BTreeMap::new(),
            packed: serializer.is_none_or(|s| s >= PACKED_TEXTURE_FROM),
            loc_channel: BTreeMap::new(),
        };
        for x in &b.bindings {
            if x.kind == KIND_CBUFFER {
                p.cb_at.insert(x.index & 0xffffff, x.name.clone());
            }
        }
        for cb in &b.cbuffers {
            p.cbs.insert(cb.name.clone(), cb);
        }
        for x in &b.bindings {
            if x.kind == KIND_TEXTURE {
                p.tex_at.insert(x.index & 0xffffff, x);
            }
        }
        for x in &b.bindings {
            if x.kind == KIND_TEXTURE {
                if let Some(s) = x.sampler().filter(|&s| s >= 0) {
                    p.sampler_at
                        .entry(s & 0xffffff)
                        .or_insert(format!("sampler{}", x.name));
                }
            }
        }
        for x in &b.bindings {
            if x.kind == KIND_SAMPLER {
                p.sampler_at
                    .entry(x.index & 0xffffff)
                    .or_insert(format!("sampler_inline{}", x.index & 0xffff));
            }
        }
        let new = serializer.is_none_or(|s| s >= NEW_CHANNELS_FROM);
        let (names, base) = (
            channels(new),
            if new { ATTRIB_BASE } else { ATTRIB_BASE_2017 },
        );
        for &(src, target) in &b.channels {
            if src >= 0 && (src as usize) < names.len() {
                p.loc_channel
                    .insert(target - base, names[src as usize].clone());
            }
        }
        p
    }
}

fn interp(m: &Module, v: u32) -> Vec<&'static str> {
    [
        ("flat", sp::DEC_FLAT),
        ("noperspective", sp::DEC_NOPERSPECTIVE),
        ("centroid", sp::DEC_CENTROID),
        ("sample", sp::DEC_SAMPLE),
    ]
    .iter()
    .filter(|(_, d)| m.decoration(v, *d).is_some())
    .map(|(q, _)| *q)
    .collect()
}

type Varyings = BTreeMap<(u32, u32), (String, String, Vec<&'static str>)>;

struct Named {
    blocks: Vec<Block>,
    varyings: Varyings,
}

fn se(e: sp::SpirvError) -> Unsupported {
    Unsupported {
        msg: e.0,
        needs_es31: false,
        glsl: false,
    }
}

/// Gives the stage's interface and resources their GL names (vulkan.name_stage).
fn name_stage(m: &mut Module, plan: &Plan, stage: &str) -> R<Named> {
    let mut out = Named {
        blocks: Vec::new(),
        varyings: BTreeMap::new(),
    };
    let w = stage;
    if stage == "VERTEX" {
        for v in m.interface(sp::SC_INPUT).map_err(se)? {
            let loc = m.decoration(v, sp::DEC_LOCATION);
            let Some(name) = loc.and_then(|l| plan.loc_channel.get(&(l as i32))).cloned() else {
                return uns(format!(
                    "{w}: vertex input at location {} has no vertex channel",
                    loc.map_or("None".into(), |l| l.to_string())
                ));
            };
            m.set_name(v, &name);
        }
    }
    for v in m
        .interface(if stage == "VERTEX" {
            sp::SC_OUTPUT
        } else {
            sp::SC_INPUT
        })
        .map_err(se)?
    {
        let Some(loc) = m.decoration(v, sp::DEC_LOCATION) else {
            return uns(format!("{w}: varying without a location"));
        };
        let comp = m.decoration(v, sp::DEC_COMPONENT).unwrap_or(0);
        let name = if comp != 0 {
            format!("vs_LOCATION{loc}_{comp}")
        } else {
            format!("vs_LOCATION{loc}")
        };
        m.set_name(v, &name);
        let (t, _) = m.pointee(v).map_err(se)?;
        out.varyings
            .insert((loc, comp), (name, m.describe(t.id), interp(m, v)));
    }
    if stage == "FRAGMENT" {
        for v in m.interface(sp::SC_OUTPUT).map_err(se)? {
            let loc = m.decoration(v, sp::DEC_LOCATION).unwrap_or(0);
            if m.decoration(v, sp::DEC_INDEX).is_some_and(|x| x != 0) {
                return uns(format!("{w}: dual-source blending output"));
            }
            m.set_name(v, &format!("SV_Target{loc}"));
        }
    }
    let mut nblock = 0;
    let vars = m.globals(&[
        sp::SC_UNIFORM,
        sp::SC_UNIFORM_CONSTANT,
        sp::SC_STORAGE_BUFFER,
        sp::SC_PUSH_CONSTANT,
    ]);
    for v in vars {
        let (t, sc) = m.pointee(v).map_err(se)?;
        let mut base = t.clone();
        while base.kind == "array" {
            base = m
                .types
                .get(&base.elem)
                .cloned()
                .ok_or_else(|| se(sp::SpirvError("KeyError".into())))?;
        }
        let at = ((m.decoration(v, sp::DEC_DESCRIPTOR_SET).unwrap_or(0) << 16)
            | m.decoration(v, sp::DEC_BINDING).unwrap_or(0)) as i32;
        if sc == sp::SC_PUSH_CONSTANT {
            return uns(format!("{w}: push constants"));
        }
        if sc == sp::SC_STORAGE_BUFFER
            || (base.kind == "struct" && m.decoration(base.id, sp::DEC_BUFFER_BLOCK).is_some())
        {
            return uns(format!("{w}: storage buffer"));
        }
        if base.kind == "struct" {
            if base.id != t.id {
                return uns(format!("{w}: array of uniform blocks"));
            }
            let cbname = plan.cb_at.get(&at).cloned();
            let Some(cb) = cbname.as_ref().and_then(|n| plan.cbs.get(n)) else {
                return uns(format!(
                    "{w}: uniform block at set {} binding {} not in the binding info",
                    at >> 16,
                    at & 0xffff
                ));
            };
            let cbname = cbname.unwrap();
            if !cb.structs.is_empty() {
                return uns(format!(
                    "{w}: constant buffer {} has struct members (GPU instancing data)",
                    py::repr(&cbname)
                ));
            }
            let mut by_off: BTreeMap<i32, &Param> = BTreeMap::new();
            for p in &cb.params {
                by_off.insert(p.offset, p);
            }
            for (k, &mt) in t.members.iter().enumerate() {
                let off = m.member_decoration(t.id, k as u32, sp::DEC_OFFSET);
                let p = off.and_then(|o| by_off.get(&(o as i32))).copied();
                let name = member_name(m, k, mt, p, &format!("{w}: {cbname}"))?;
                let name = name.unwrap_or_else(|| {
                    format!(
                        "Xhlslcc_UnusedX_{cbname}_{}",
                        off.map_or("None".into(), |o| o.to_string())
                    )
                });
                m.set_member_name(t.id, k as u32, &name);
            }
            let g = is_globals(&cbname);
            m.set_name(
                t.id,
                &if g {
                    format!("PMUNITY_GLOBALS{nblock}")
                } else {
                    cbname.clone()
                },
            );
            m.set_name(v, &format!("PMUNITY_CB{nblock}"));
            nblock += 1;
            out.blocks.push(Block {
                cb_name: cbname,
                globals: g,
            });
        } else if base.kind == "image" {
            if base.dim == 6 {
                return uns(format!("{w}: subpass input (framebuffer fetch)"));
            }
            if base.dim == 5 {
                return uns(format!("{w}: texel buffer"));
            }
            if base.sampled == 2 {
                return uns(format!("{w}: storage image (UAV)"));
            }
            let Some(x) = plan.tex_at.get(&at) else {
                return uns(format!(
                    "{w}: texture at set {} binding {} not in the binding info",
                    at >> 16,
                    at & 0xffff
                ));
            };
            if base.id != t.id {
                return uns(format!("{w}: texture array {}", x.name));
            }
            m.set_name(v, &x.name);
        } else if base.kind == "sampled_image" {
            let x = plan.tex_at.get(&at);
            let pair = plan.tex_at.get(&(at - 1));
            if x.is_none() && pair.is_some() && !plan.packed {
                m.set_name(v, &format!("{}_PMPAIR", pair.unwrap().name));
                continue;
            }
            let Some(x) = x else {
                return uns(format!(
                    "{w}: texture at set {} binding {} not in the binding info",
                    at >> 16,
                    at & 0xffff
                ));
            };
            m.set_name(v, &x.name);
        } else if base.kind == "sampler" {
            let n = plan.sampler_at.get(&at).cloned().unwrap_or(format!(
                "sampler_{}_{}",
                at >> 16,
                at & 0xffff
            ));
            m.set_name(v, &n);
        } else {
            return uns(format!("{w}: uniform of type {}", base.kind));
        }
    }
    Ok(out)
}

// ---------------------------------------------------------------- SPIRV-Cross's GLSL -> Unity's GL form

const UNIFORM_LOCATION_BLOCK: &str = "#define UNITY_SUPPORTS_UNIFORM_LOCATION 1\n#if UNITY_SUPPORTS_UNIFORM_LOCATION\n#define UNITY_LOCATION(x) layout(location = x)\n#define UNITY_BINDING(x) layout(binding = x, std140)\n#else\n#define UNITY_LOCATION(x)\n#define UNITY_BINDING(x) layout(std140)\n#endif\n";

struct Patterns {
    flip_y: Regex,
    block_decl: Regex,
    member_line: Regex,
    sampler_decl: Regex,
    header_end: Regex,
    pmsampler: Regex,
    widen_base: Regex,
}

static RX: LazyLock<Patterns> = LazyLock::new(|| Patterns {
    flip_y: glsl::re(r"(?m)^[ \t]*gl_Position\.y = -gl_Position\.y;[ \t]*\n"),
    block_decl: glsl::re(
        r"(?ms)^(?:layout\([^)]*\)\s*)?uniform (\w+)\n\{\n(.*?)\n\} (PMUNITY_CB\d+);\n",
    ),
    member_line: glsl::re(
        r"\A\s*((?:(?:highp|mediump|lowp)\s+)?\w+)\s+(\w+)((?:\[\d+\])?);\s*\n?\z",
    ),
    sampler_decl: glsl::re(
        r"(?m)^uniform\s+((?:(?:highp|mediump|lowp)\s+)?)(\w*sampler\w+)\s+(\w+);\n",
    ),
    header_end: glsl::re(
        r"\A(#version[^\n]*\n(?:#extension[^\n]*\n|#if[^\n]*\n|#endif[^\n]*\n|#define[^\n]*\n|#else[^\n]*\n|precision[^\n]*\n|\s*\n)*)",
    ),
    pmsampler: glsl::re(r"_PMSAMPLER\d+\n?\z"),
    widen_base: glsl::re(r"\A(?:[iub]?vec[234]|float|int|uint|bool)\n?\z"),
});

fn word(w: &str) -> Regex {
    glsl::re(&format!(r"\b{}\b", regex::escape(w)))
}

/// The PMUNITY_CB<n> blocks of a stage as plain uniforms or unnamed-instance blocks (_flatten_blocks).
#[allow(clippy::type_complexity)]
fn flatten_blocks(src: &str, flatten_named: bool) -> R<(String, Vec<(String, Vec<String>)>)> {
    let r = &*RX;
    let matches: Vec<(String, String, String, String)> = r
        .block_decl
        .captures_iter(src)
        .map(|c| {
            (
                c[0].to_string(),
                c[1].to_string(),
                c[2].to_string(),
                c[3].to_string(),
            )
        })
        .collect();
    let mut src = src.to_string();
    let mut named = Vec::new();
    for (whole, tname, body, inst) in matches {
        let lines: Vec<String> = body
            .split('\n')
            .filter(|l| !py::strip(l).is_empty())
            .map(String::from)
            .collect();
        let rest = src.replacen(&whole, "", usize::MAX);
        if tname.starts_with("PMUNITY_GLOBALS") || flatten_named {
            let mut decls = String::new();
            for l in &lines {
                let Some(ml) = r.member_line.captures(l) else {
                    return uns(format!("uniform block member {}", py::repr(py::strip(l))));
                };
                if glsl::re(&format!(r"\b{inst}\.{}\b", &ml[2])).is_match(&rest) {
                    if ml[2].starts_with("Xhlslcc_UnusedX") {
                        return uns(format!(
                            "a uniform the binding info does not name is read ({})",
                            &ml[2][16.min(ml[2].len())..]
                        ));
                    }
                    decls += &format!("uniform {} {}{};\n", &ml[1], &ml[2], &ml[3]);
                }
            }
            src = src.replace(&whole, &decls);
        } else {
            src = src.replace(&whole, &format!("@@BLOCK {tname}@@\n"));
            named.push((tname, lines));
        }
        src = glsl::re(&format!(r"\b{inst}\."))
            .replace_all(&src, "")
            .into_owned();
    }
    Ok((src, named))
}

fn write_blocks(
    mut src: String,
    named: &[(String, Vec<String>)],
    binding_of: &BTreeMap<String, usize>,
    macros: bool,
) -> String {
    for (tname, lines) in named {
        let body: String = lines
            .iter()
            .map(|l| format!("\t{}\n", py::strip(l)))
            .collect();
        let head = if macros {
            format!("UNITY_BINDING({}) uniform {tname} {{\n", binding_of[tname])
        } else {
            format!("layout(std140) uniform {tname} {{\n")
        };
        src = src.replace(
            &format!("@@BLOCK {tname}@@\n"),
            &format!("{head}{body}}};\n"),
        );
    }
    src
}

/// Combined samplers as Unity's GL programs name them (_samplers): (src, the sampler uniforms in
/// declaration order).
fn samplers(src: &str, precision: &str) -> R<(String, Vec<String>)> {
    let r = &*RX;
    let mut src = src.to_string();
    let firsts: Vec<(String, String, String, String)> = r
        .sampler_decl
        .captures_iter(&src)
        .map(|c| {
            (
                c[0].to_string(),
                c[1].to_string(),
                c[2].to_string(),
                c[3].to_string(),
            )
        })
        .collect();
    for (whole, _, typ, name) in firsts {
        let base = r.pmsampler.replace(&name, "").into_owned();
        if base != name {
            let others: Vec<String> = r
                .sampler_decl
                .captures_iter(&src)
                .filter(|c| c[3] == *base)
                .map(|c| c[2].to_string())
                .collect();
            if others.first() != Some(&typ) {
                return uns(format!(
                    "texture {base} sampled with two samplers of different kinds"
                ));
            }
            src = src.replacen(&whole, "", usize::MAX);
            src = word(&name).replace_all(&src, NoExpand(&base)).into_owned();
        }
    }
    let found: Vec<(String, String, String)> = r
        .sampler_decl
        .captures_iter(&src)
        .map(|c| (c[1].to_string(), c[2].to_string(), c[3].to_string()))
        .collect();
    let mut fin: Vec<(String, String)> = Vec::new();
    for (_, typ, name) in &found {
        let base = name.strip_suffix("_PMPAIR").unwrap_or(name);
        let v = format!(
            "{}{base}",
            if typ.contains("Shadow") {
                "hlslcc_zcmp"
            } else {
                ""
            }
        );
        match fin.iter_mut().find(|(n, _)| n == name) {
            Some(e) => e.1 = v,
            None => fin.push((name.clone(), v)),
        }
    }
    let vals: std::collections::BTreeSet<&String> = fin.iter().map(|(_, v)| v).collect();
    if vals.len() != fin.len() {
        return uns("a texture declared twice with the same kind of sampler");
    }
    let final_of = |n: &str| fin.iter().find(|(x, _)| x == n).unwrap().1.clone();
    let mut plain: std::collections::BTreeSet<String> = found
        .iter()
        .filter(|(_, t, _)| !t.contains("Shadow"))
        .map(|(_, _, n)| final_of(n))
        .collect();
    let (mut decls, mut texts) = (Vec::new(), Vec::new());
    let drop_prec = |t: &str| precision != "highp" && (t == "sampler2D" || t == "samplerCube");
    for (k, (prec, typ, name)) in found.iter().enumerate() {
        let gl = final_of(name);
        let p = if drop_prec(typ) { "" } else { prec.as_str() };
        let mut decl = format!("uniform {p}{typ} {gl};\n");
        if let Some(pn) = gl.strip_prefix("hlslcc_zcmp") {
            if !plain.contains(pn) {
                let ptyp = typ.replace("Shadow", "");
                let pp = if drop_prec(&ptyp) { "" } else { prec.as_str() };
                decl = format!("uniform {pp}{ptyp} {pn};\n{decl}");
                decls.push(pn.to_string());
                plain.insert(pn.to_string());
            }
        }
        decls.push(gl.clone());
        texts.push(decl);
        let rx = glsl::re(&format!(
            r"(?m)^uniform\s+(?:(?:highp|mediump|lowp)\s+)?{}\s+{};\n",
            regex::escape(typ),
            regex::escape(name)
        ));
        src = rx
            .replacen(&src, 1, NoExpand(&format!("@@SAMPLER{k}@@\n")))
            .into_owned();
    }
    let renamed: Vec<(usize, String)> = found
        .iter()
        .enumerate()
        .filter(|(_, (_, _, n))| final_of(n) != *n)
        .map(|(k, (_, _, n))| (k, n.clone()))
        .collect();
    for (k, name) in &renamed {
        src = word(name)
            .replace_all(&src, NoExpand(&format!("@@NAME{k}@@")))
            .into_owned();
    }
    for (k, name) in &renamed {
        src = src.replace(&format!("@@NAME{k}@@"), &final_of(name));
    }
    for (k, decl) in texts.iter().enumerate() {
        src = src.replace(&format!("@@SAMPLER{k}@@\n"), decl);
    }
    Ok((src, decls))
}

/// Each sampler uniform with its location in Unity's macro (_locations).
fn locations(src: &str, units: &BTreeMap<String, usize>) -> String {
    RX.sampler_decl
        .replace_all(src, |c: &regex::Captures| {
            format!(
                "UNITY_LOCATION({}) {}",
                units.get(&c[3]).copied().unwrap_or(0),
                &c[0]
            )
        })
        .into_owned()
}

fn ncomp(t: &str) -> usize {
    let s = t.trim_start_matches(['i', 'u', 'b']);
    s.strip_prefix("vec")
        .and_then(|n| n.parse().ok())
        .unwrap_or(1)
}

fn base_of(t: &str) -> String {
    if t.contains("vec") {
        match t.chars().next() {
            Some('i') => "int",
            Some('u') => "uint",
            Some('b') => "bool",
            _ => "float",
        }
        .to_string()
    } else {
        t.to_string()
    }
}

/// One Vulkan entry (code, its Bindings with the parameter entry and shared parameters put back)
/// -> (GL program text, GPU program type 4 GLES3 or 3 GLES31) (vulkan.translate_program).
pub fn translate_program(
    tr: &mut Translator,
    code: &[u8],
    b: &Bindings,
    serializer: Option<i32>,
    unity: (u64, u64),
    precision: &str,
    notes: &mut Vec<String>,
) -> R<(String, i64)> {
    let macros = unity >= (2019, 1);
    let (_, mods) = split_code(code)?;
    let plan = Plan::new(b, serializer);
    let mut stages: Vec<(&'static str, Module)> = Vec::new();
    for (slot, data) in mods {
        let decoded = tr.decode(&data, unity < (2020, 1))?;
        let mut m = Module::new(&decoded).map_err(|e| Unsupported {
            msg: format!("stage {slot}: {}", e.0),
            needs_es31: false,
            glsl: false,
        })?;
        let st = m.stage();
        if !matches!(st, Some("VERTEX") | Some("FRAGMENT")) {
            return uns(format!(
                "{} stage (not in GLSL ES 3.00)",
                st.unwrap_or("unknown")
            ));
        }
        let st = st.unwrap();
        if stages.iter().any(|(s, _)| *s == st) {
            return uns(format!("two {st} stages"));
        }
        if m.source_language == Some(5) {
            return uns(format!(
                "{st}: SPIR-V compiled from HLSL (DXC): not translated"
            ));
        }
        if m.replace_opcodes(&DERIVATIVES) > 0 {
            m.remove_capability(sp::CAP_DERIVATIVE_CONTROL);
        }
        stages.push((st, m));
    }
    let names: Vec<&str> = {
        let mut v: Vec<&str> = stages.iter().map(|(s, _)| *s).collect();
        v.sort();
        v
    };
    if names != ["FRAGMENT", "VERTEX"] {
        return uns(format!(
            "expected a vertex and a fragment stage, got {}",
            names.join(", ")
        ));
    }
    let mut named: BTreeMap<&str, Named> = BTreeMap::new();
    for (st, m) in stages.iter_mut() {
        named.insert(st, name_stage(m, &plan, st)?);
    }
    let (vout, fin) = (&named["VERTEX"].varyings, &named["FRAGMENT"].varyings);
    let mut widen: Vec<(String, String, String)> = Vec::new();
    for (key, (name, typ, interp)) in fin {
        let Some((_, vtyp, vinterp)) = vout.get(key) else {
            return uns(format!(
                "fragment input at location {} is not written by the vertex stage",
                key.0
            ));
        };
        if vinterp != interp {
            return uns(format!(
                "varying at location {}: interpolation ({}) / ({})",
                key.0,
                vinterp
                    .iter()
                    .map(|x| py::repr(x))
                    .collect::<Vec<_>>()
                    .join(", "),
                interp
                    .iter()
                    .map(|x| py::repr(x))
                    .collect::<Vec<_>>()
                    .join(", ")
            ));
        }
        if vtyp != typ {
            if !RX.widen_base.is_match(vtyp)
                || ncomp(typ) >= ncomp(vtyp)
                || base_of(typ) != base_of(vtyp)
            {
                return uns(format!(
                    "varying at location {}: {vtyp} written, {typ} read",
                    key.0
                ));
            }
            widen.push((name.clone(), vtyp.clone(), "xyzw"[..ncomp(typ)].to_string()));
        }
    }
    let mut blocks: Vec<String> = Vec::new();
    for st in ["VERTEX", "FRAGMENT"] {
        for bl in &named[st].blocks {
            if !bl.globals && !blocks.contains(&bl.cb_name) {
                blocks.push(bl.cb_name.clone());
            }
        }
    }
    let binding_of: BTreeMap<String, usize> = blocks
        .iter()
        .enumerate()
        .map(|(i, n)| (n.clone(), i))
        .collect();
    let flatten_named = !macros;
    let mut compile_all = |es: i64| -> R<BTreeMap<&'static str, String>> {
        let mut texts: BTreeMap<&'static str, String> = BTreeMap::new();
        let mut decls: BTreeMap<&'static str, Vec<String>> = BTreeMap::new();
        for (st, m) in stages.iter() {
            let mut src = tr.compile(&m.serialize(), es)?;
            if *st == "VERTEX" {
                let n = RX.flip_y.find_iter(&src).count();
                src = RX.flip_y.replace_all(&src, "").into_owned();
                if n == 0 {
                    return uns("VERTEX: no Vulkan clip-space y flip");
                }
            }
            let (s2, nb) = flatten_blocks(&src, flatten_named)?;
            src = write_blocks(s2, &nb, &binding_of, macros);
            let (s3, d) = samplers(&src, precision)?;
            src = s3;
            decls.insert(st, d);
            if *st == "FRAGMENT" {
                for (name, vtyp, swz) in &widen {
                    let decl = glsl::re(&format!(
                        r"(?m)^((?:(?:flat|centroid|smooth)\s+)*in\s+(?:(?:highp|mediump|lowp)\s+)?)\w+(\s+{name};)$"
                    ));
                    let n = decl.find_iter(&src).count();
                    src = decl
                        .replace_all(&src, |c: &regex::Captures| {
                            format!("{}{vtyp}{}", &c[1], &c[2])
                        })
                        .into_owned();
                    if n != 1 {
                        return uns(format!("FRAGMENT: input {name} not found"));
                    }
                    let (head, body) = match src.split_once("void main()") {
                        Some((h, b)) => (h.to_string(), b.to_string()),
                        None => (src.clone(), String::new()),
                    };
                    let body = word(name)
                        .replace_all(&body, NoExpand(&format!("{name}.{swz}")))
                        .into_owned();
                    src = format!("{head}void main(){body}");
                }
            }
            texts.insert(st, src);
        }
        let mut units: BTreeMap<String, usize> = BTreeMap::new();
        for st in ["FRAGMENT", "VERTEX"] {
            for n in &decls[st] {
                let l = units.len();
                units.entry(n.clone()).or_insert(l);
            }
        }
        let order: Vec<&'static str> = stages.iter().map(|(s, _)| *s).collect();
        for st in order {
            let mut src = texts[st].clone();
            if macros {
                src = locations(&src, &units);
                let Some(hm) = RX.header_end.find(&src) else {
                    return uns(format!("{st}: no #version line"));
                };
                let e = hm.end();
                src = format!("{}{UNIFORM_LOCATION_BLOCK}{}", &src[..e], &src[e..]);
            }
            glsl::vulkan_check(&src, st, es == 310).map_err(|u| Unsupported {
                msg: u.msg,
                needs_es31: u.needs_es31,
                glsl: !u.needs_es31,
            })?;
            texts.insert(st, src);
        }
        Ok(texts)
    };
    let (mut texts, typ) = match compile_all(300) {
        Ok(t) => (t, 4),
        Err(u) if u.needs_es31 => (compile_all(310)?, 3),
        Err(u) if !u.glsl && u.msg.contains("310") => (compile_all(310)?, 3),
        Err(u) => return Err(u),
    };
    let (vs, flips) = uv_origin(&texts["VERTEX"]);
    texts.insert("VERTEX", vs);
    if flips > 0 {
        notes.push(format!("VERTEX uv-origin {flips}"));
    }
    Ok((
        format!(
            "#ifdef VERTEX\n{}\n#endif\n#ifdef FRAGMENT\n{}\n#endif\n",
            texts["VERTEX"], texts["FRAGMENT"]
        ),
        typ,
    ))
}

// ---------------------------------------------------------------- full-screen texture coordinates

struct UvPatterns {
    stmt: Regex,
    oper: Regex,
    vertex_id: Regex,
    flip: Regex,
    ctor: Regex,
    and_k: Regex,
    shr: Regex,
    num: Regex,
    flipy_decl: Regex,
    flipy: Regex,
}

static UV: LazyLock<UvPatterns> = LazyLock::new(|| {
    let decl = r"(?:(?:const\s+)?(?:(?:highp|mediump|lowp)\s+)?(?:float|int|uint|bool|[iub]?vec[234])\s+)?";
    UvPatterns {
        stmt: glsl::re(&format!(
            r"\A(\s*)({decl})(\w+)(?:\.([xyzw]+))?\s*([-+]?=)\s*(.+?);\s*\n?\z"
        )),
        oper: glsl::re(r"\A(\w+)(?:\.([xyzw]+))?\n?\z"),
        vertex_id: glsl::re(r"\A(?:u?int\()?(?:uint\(gl_VertexID\)|gl_VertexID)\)?\n?\z"),
        flip: glsl::re(
            r"\A\(?-(\w+(?:\.[xyzw]+)?)\)?\s*\+\s*(?:1\.0|vec[234]\((?:1\.0,\s*)*1\.0\))\n?\z",
        ),
        ctor: glsl::re(r"(?s)\A(u?int|float|vec[234]|[iu]vec[234])\((.*)\)\n?\z"),
        and_k: glsl::re(r"\A\s*(?:0x)?[12]u?\s*\n?\z"),
        shr: glsl::re(r"(?s)\A(.*)>>\s*1u?\n?\z"),
        num: glsl::re(r"\A-?\d"),
        flipy_decl: glsl::re(r"(?m)^\s*uniform\s+(?:highp\s+|mediump\s+)?float\s+_FlipY\s*;"),
        flipy: glsl::re(r"\b_FlipY\s*<\s*0\.5\b"),
    }
});

fn closing(s: &[u8], i: usize) -> Option<usize> {
    let mut depth = 0i64;
    for (j, &c) in s.iter().enumerate().skip(i) {
        depth += (c == b'(') as i64;
        depth -= (c == b')') as i64;
        if depth == 0 {
            return Some(j);
        }
    }
    None
}

fn strip_parens(s: &str) -> String {
    let mut s = py::strip(s).to_string();
    while s.starts_with('(') && s.ends_with(')') && closing(s.as_bytes(), 0) == Some(s.len() - 1) {
        s = py::strip(&s[1..s.len() - 1]).to_string();
    }
    s
}

fn top_split(s: &str, sep: char) -> Vec<String> {
    let (mut out, mut depth, mut cur) = (Vec::new(), 0i64, String::new());
    for ch in s.chars() {
        depth += (ch == '(') as i64;
        depth -= (ch == ')') as i64;
        if ch == sep && depth == 0 {
            out.push(std::mem::take(&mut cur));
        } else {
            cur.push(ch);
        }
    }
    out.push(cur);
    out
}

type Tags = BTreeMap<(String, String), Option<String>>;

fn operand(tags: &Tags, s: &str) -> Option<Vec<Option<String>>> {
    let c = UV.oper.captures(py::strip(s))?;
    let var = c[1].to_string();
    match c.get(2) {
        Some(swz) => Some(
            swz.as_str()
                .chars()
                .map(|ch| tags.get(&(var.clone(), ch.to_string())).cloned().flatten())
                .collect(),
        ),
        None => Some(vec![tags
            .get(&(var.clone(), String::new()))
            .cloned()
            .unwrap_or_else(|| {
                tags.get(&(var.clone(), "x".into())).cloned().flatten()
            })]),
    }
}

fn value(tags: &Tags, rhs: &str) -> Option<Vec<Option<String>>> {
    let u = &*UV;
    let r = strip_parens(rhs);
    if u.vertex_id.is_match(&r) {
        return Some(vec![Some("id".into())]);
    }
    if let Some(m) = u.ctor.captures(&r) {
        let f = m[1].to_string();
        if closing(r.as_bytes(), f.len()) == Some(r.len() - 1) {
            let mut flat = Vec::new();
            for a in top_split(&m[2], ',') {
                flat.extend(value(tags, &a)?);
            }
            if f == "int" || f == "uint" || f.starts_with("ivec") || f.starts_with("uvec") {
                return Some(flat);
            }
            return Some(
                flat.into_iter()
                    .map(|x| match x.as_deref() {
                        Some("tri.f") | Some("tri") => Some("tri.f".into()),
                        Some("quad.f") | Some("quad") => Some("quad.f".into()),
                        _ => None,
                    })
                    .collect(),
            );
        }
    }
    let parts = top_split(&r, '+');
    if parts.len() == 2 {
        if let Some(fm) = u.flip.captures(&r) {
            if let Some(t) = operand(tags, &fm[1]) {
                if !t.is_empty()
                    && t.iter()
                        .all(|x| matches!(x.as_deref(), Some("tri.f") | Some("quad.f")))
                {
                    return Some(vec![Some("flip".into()); t.len()]);
                }
            }
        }
        let (a, b) = (value(tags, &parts[0]), value(tags, &parts[1]));
        if let (Some(a), Some(b)) = (a, b) {
            if a.len() == 1 && b.len() == 1 {
                let set: std::collections::BTreeSet<Option<String>> =
                    [a[0].clone(), b[0].clone()].into_iter().collect();
                let want: std::collections::BTreeSet<Option<String>> =
                    [Some("top".into()), Some("bot".into())]
                        .into_iter()
                        .collect();
                if set == want {
                    return Some(vec![Some("sum".into())]);
                }
            }
        }
        return None;
    }
    let parts = top_split(&r, '&');
    if parts.len() == 2 && u.and_k.is_match(&parts[1]) {
        let a = value(tags, &parts[0]);
        let ks = py::strip(&parts[1]).trim_end_matches('u').to_string();
        let k = if let Some(h) = ks.strip_prefix("0x") {
            i64::from_str_radix(h, 16).ok()?
        } else {
            ks.parse::<i64>().ok()?
        };
        if let Some(a) = a.filter(|a| a.len() == 1) {
            let t = match (a[0].as_deref(), k) {
                (Some("id"), 2) => Some("tri".to_string()),
                (Some("id"), 1) => Some("bot".to_string()),
                (Some("sum"), 1) => Some("quad".to_string()),
                _ => None,
            };
            return Some(vec![t]);
        }
        return None;
    }
    if let Some(m) = u.shr.captures(&r) {
        let a = value(tags, &m[1]);
        return if a == Some(vec![Some("id".into())]) {
            Some(vec![Some("top".into())])
        } else {
            None
        };
    }
    if u.num.is_match(&r) {
        return Some(vec![None]);
    }
    operand(tags, &r)
}

/// The vertex stage with the SRP core library's full-screen texture coordinate flips undone
/// (vulkan.uv_origin): (text, flips undone).
pub fn uv_origin(vs: &str) -> (String, usize) {
    if !vs.contains("gl_VertexID") {
        return (vs.to_string(), 0);
    }
    let u = &*UV;
    let mut tags: Tags = BTreeMap::new();
    let mut n = 0;
    let mut out = Vec::new();
    for line in vs.split('\n') {
        let mut line = line.to_string();
        let c = u.stmt.captures(&line).map(|c| {
            (1..=6)
                .map(|i| c.get(i).map(|m| m.as_str().to_string()))
                .collect::<Vec<_>>()
        });
        if let Some(g) = c.filter(|_| !line.trim_start_matches(py::isspace).starts_with('#')) {
            let (ind, decl, var, swz, op, rhs) = (
                g[0].clone().unwrap(),
                g[1].clone().unwrap_or_default(),
                g[2].clone().unwrap(),
                g[3].clone(),
                g[4].clone().unwrap(),
                g[5].clone().unwrap(),
            );
            let dot = swz.as_ref().map_or(String::new(), |s| format!(".{s}"));
            let mut v = if op == "-=" {
                None
            } else if op == "+=" {
                value(&tags, &format!("{var}{dot} + {rhs}"))
            } else {
                value(&tags, &rhs)
            };
            if v.as_ref()
                .and_then(|v| v.first().cloned().flatten())
                .as_deref()
                == Some("flip")
            {
                let src = u.flip.captures(&strip_parens(&rhs)).unwrap()[1].to_string();
                line = format!("{ind}{decl}{var}{dot} = {src};");
                v = operand(&tags, &src);
                n += 1;
            }
            let keys: Vec<(String, String)> = match &swz {
                Some(s) => s.chars().map(|ch| (var.clone(), ch.to_string())).collect(),
                None => {
                    let keys = match &v {
                        Some(vv) if vv.len() > 1 => "xyzw"[..vv.len().min(4)]
                            .chars()
                            .map(|ch| (var.clone(), ch.to_string()))
                            .collect(),
                        _ => vec![(var.clone(), String::new())],
                    };
                    for ch in ["x", "y", "z", "w"] {
                        tags.remove(&(var.clone(), ch.to_string()));
                    }
                    tags.remove(&(var.clone(), String::new()));
                    keys
                }
            };
            if let Some(vv) = &v {
                if vv.len() == 1 && keys.len() > 1 {
                    v = Some(vec![vv[0].clone(); keys.len()]);
                }
            }
            for (i, k) in keys.into_iter().enumerate() {
                let t = v.as_ref().and_then(|v| v.get(i).cloned()).flatten();
                tags.insert(k, t);
            }
        }
        out.push(line);
    }
    let mut text = out.join("\n");
    if u.flipy_decl.is_match(&text) {
        let k = u.flipy.find_iter(&text).count();
        text = u.flipy.replace_all(&text, NoExpand("false")).into_owned();
        n += k;
    }
    (text, n)
}
