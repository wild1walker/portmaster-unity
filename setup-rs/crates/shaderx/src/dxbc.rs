//! Direct3D 11 programs (DXBC) of Unity Shader assets -> GL programs with Unity's HLSLcc, as
//! shaders/dxbc.py does it (its docstring has the layouts): the DXBC container's declarations read
//! (constant buffers, textures, samplers, the constant buffer locations its instructions read), an
//! RDEF chunk rebuilt from Unity's binding info (bindings.rs) so HLSLcc names every uniform as
//! Unity's own GL programs do, the stages translated by pmhlslcc (a server process), and the GLSL
//! written as Unity stores a GL program of the data's version.
use crate::bindings::{Bindings, KIND_CBUFFER, KIND_TEXTURE};
use crate::glsl;
use regex::{Captures, NoExpand, Regex};
use std::collections::{BTreeMap, BTreeSet};
use std::io::{BufRead, BufReader, Read, Write};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::LazyLock;

/// A program this path cannot give a GL twin (dxbc.Unsupported).
#[derive(Clone, Debug, PartialEq)]
pub struct Unsupported(pub String);

impl std::fmt::Display for Unsupported {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl From<crate::bindings::Unsupported> for Unsupported {
    fn from(e: crate::bindings::Unsupported) -> Self {
        Unsupported(e.0)
    }
}

/// A fault the Python does not catch (struct.error, IndexError on a short container): the run
/// stops, as the Python's does.
#[derive(Clone, Debug)]
pub enum Fault {
    Unsupported(Unsupported),
    Crash(String),
}

impl From<Unsupported> for Fault {
    fn from(e: Unsupported) -> Self {
        Fault::Unsupported(e)
    }
}

impl From<crate::bindings::Unsupported> for Fault {
    fn from(e: crate::bindings::Unsupported) -> Self {
        Fault::Unsupported(e.into())
    }
}

type R<T> = Result<T, Fault>;

fn uns<T>(m: impl Into<String>) -> R<T> {
    Err(Fault::Unsupported(Unsupported(m.into())))
}

fn crash<T>(m: impl Into<String>) -> R<T> {
    Err(Fault::Crash(m.into()))
}

pub const VERTEX_TYPES: [i64; 2] = [15, 16];
pub const PIXEL_TYPES: [i64; 2] = [17, 18];
pub const OTHER_STAGE_TYPES: [i64; 4] = [19, 20, 21, 22];
const HEADER_SIZES: [usize; 2] = [6, 38];
pub const SERIALIZERS: [i32; 4] = [201708220, 201802150, 201806140, 202012090];

const HLSLCC_FLAG_DISABLE_GLOBALS_STRUCT: u32 = 0x800;
const HLSLCC_FLAG_TRANSLATE_MATRICES: u32 = 0x20000;
const HLSLCC_FLAG_FORCE_EXPLICIT_LOCATIONS: u32 = 0x8000000;
pub const FLAGS: u32 = HLSLCC_FLAG_TRANSLATE_MATRICES | HLSLCC_FLAG_DISABLE_GLOBALS_STRUCT;
pub const FLAGS_2019: u32 = FLAGS | HLSLCC_FLAG_FORCE_EXPLICIT_LOCATIONS;
const LANG: &str = "150";

fn u32_at(d: &[u8], o: usize) -> R<u32> {
    match d.get(o..o + 4) {
        Some(b) => Ok(u32::from_le_bytes(b.try_into().unwrap())),
        None => crash("unpack_from requires a buffer of at least 4 bytes"),
    }
}

// ---------------------------------------------------------------- the DXBC container

/// The size of Unity's header before an entry's DXBC container (6 or 38), or None.
pub fn header_size(code: &[u8]) -> Option<usize> {
    HEADER_SIZES
        .into_iter()
        .find(|&n| code.len() >= n + 32 && &code[n..n + 4] == b"DXBC")
}

/// A D3D11 entry's code -> (Unity's header, the DXBC container).
pub fn split_code(code: &[u8]) -> R<(Vec<u8>, Vec<u8>)> {
    let Some(n) = header_size(code) else {
        return uns("not a D3D11 program (no DXBC container after the header)");
    };
    let size = u32_at(code, n + 24)? as usize;
    if size != code.len() - n {
        return uns(format!("DXBC container size {size} != {}", code.len() - n));
    }
    Ok((code[..n].to_vec(), code[n..].to_vec()))
}

/// [(fourcc, data)] of a DXBC container.
pub fn chunks(dxbc: &[u8]) -> R<Vec<(String, Vec<u8>)>> {
    let n = u32_at(dxbc, 28)? as usize;
    let mut out = Vec::new();
    for i in 0..n {
        let off = u32_at(dxbc, 32 + 4 * i)? as usize;
        let fourcc: String = dxbc
            .get(off..(off + 4).min(dxbc.len()))
            .unwrap_or(&[])
            .iter()
            .map(|&b| if b < 0x80 { b as char } else { '\u{fffd}' })
            .collect();
        let size = u32_at(dxbc, off + 4)? as usize;
        let a = (off + 8).min(dxbc.len());
        let b = (off + 8 + size).min(dxbc.len());
        out.push((fourcc, dxbc[a..b].to_vec()));
    }
    Ok(out)
}

fn pad4(n: usize) -> usize {
    (n + 3) & !3
}

/// [(fourcc, data)] -> a DXBC container (the checksum left zero: HLSLcc does not check it).
pub fn build_container(chs: &[(String, Vec<u8>)]) -> Vec<u8> {
    let head = 32 + 4 * chs.len();
    let mut body = Vec::new();
    let mut offs = Vec::new();
    for (fourcc, data) in chs {
        offs.push((head + body.len()) as u32);
        body.extend(fourcc.as_bytes());
        body.extend((data.len() as u32).to_le_bytes());
        body.extend(data);
        body.resize(pad4(body.len()), 0);
    }
    let total = (head + body.len()) as u32;
    let mut out = b"DXBC".to_vec();
    out.extend([0u8; 16]);
    for x in [1u32, total, chs.len() as u32] {
        out.extend(x.to_le_bytes());
    }
    for o in offs {
        out.extend(o.to_le_bytes());
    }
    out.extend(body);
    out
}

// ---------------------------------------------------------------- SHDR / SHEX declarations

fn stage_name(s: u32) -> &'static str {
    match s {
        0 => "pixel",
        1 => "vertex",
        2 => "geometry",
        3 => "hull",
        4 => "domain",
        5 => "compute",
        _ => "?",
    }
}

const OP_CUSTOMDATA: u32 = 53;
const OP_DCL_RESOURCE: u32 = 88;
const OP_DCL_CB: u32 = 89;
const OP_DCL_SAMPLER: u32 = 90;

fn op_buffer(op: u32) -> Option<&'static str> {
    Some(match op {
        156 => "UAV",
        157 => "raw UAV",
        158 => "structured UAV",
        159 | 160 => "group shared memory",
        161 => "raw buffer",
        162 => "structured buffer",
        _ => return None,
    })
}

/// RESOURCE_DIMENSION -> (D3D_SRV_DIMENSION for RDEF, Unity's TextureDimension or None).
fn dims(d: u32) -> Option<(u32, Option<i32>)> {
    Some(match d {
        1 => (1, None),
        2 => (2, None),
        3 => (4, Some(2)),
        4 => (6, Some(2)),
        5 => (8, Some(3)),
        6 => (9, Some(4)),
        7 => (3, None),
        8 => (5, Some(5)),
        9 => (7, Some(5)),
        10 => (10, Some(6)),
        _ => return None,
    })
}

fn dim_name(d: u32) -> &'static str {
    match d {
        1 => "buffer",
        2 => "1D",
        3 => "2D",
        4 => "2D MS",
        5 => "3D",
        6 => "cube",
        7 => "1D array",
        8 => "2D array",
        9 => "2D MS array",
        10 => "cube array",
        _ => "?",
    }
}

const ES3_DIMS: [u32; 5] = [3, 4, 5, 6, 8];

fn is_declaration(op: u32) -> bool {
    (88..=106).contains(&op) || (143..=162).contains(&op) || op == 206
}

/// What the bytecode declares (dxbc.Program).
pub struct Program {
    pub minor: u32,
    pub major: u32,
    pub stage: u32,
    pub cbuffers: BTreeMap<u32, u32>,
    pub textures: BTreeMap<u32, (u32, u32)>,
    pub samplers: BTreeMap<u32, u32>,
    pub buffers: Vec<&'static str>,
    /// None if an instruction could not be walked.
    pub cb_reads: Option<BTreeSet<(u32, u32, u32)>>,
}

/// An operand walk that went wrong (the Python's IndexError / ValueError there).
struct Bad;

impl Program {
    pub fn new(shdr: &[u8]) -> R<Program> {
        let w: Vec<u32> = shdr
            .chunks_exact(4)
            .map(|c| u32::from_le_bytes(c.try_into().unwrap()))
            .collect();
        if w.len() < 2 {
            return uns("empty shader code chunk");
        }
        let mut p = Program {
            minor: w[0] & 0xf,
            major: (w[0] >> 4) & 0xf,
            stage: w[0] >> 16,
            cbuffers: BTreeMap::new(),
            textures: BTreeMap::new(),
            samplers: BTreeMap::new(),
            buffers: Vec::new(),
            cb_reads: Some(BTreeSet::new()),
        };
        let end = (w[1] as usize).min(w.len());
        let mut i = 2;
        let at = |k: usize| -> R<u32> {
            match w.get(k) {
                Some(&x) => Ok(x),
                None => crash("tuple index out of range"),
            }
        };
        while i < end {
            let tok = w[i];
            let op = tok & 0x7ff;
            let n = if op == OP_CUSTOMDATA {
                at(i + 1)? as usize
            } else {
                ((tok >> 24) & 0x7f) as usize
            };
            if n == 0 || i + n > end {
                return uns("shader code chunk: bad instruction length");
            }
            if op != OP_CUSTOMDATA && !is_declaration(op) && p.cb_reads.is_some() {
                p.walk(&w, i, i + n);
            }
            if op == OP_DCL_CB {
                let (idx, _) = p.operand(&w, i + 1)?;
                p.cbuffers.insert(idx[0], idx.get(1).copied().unwrap_or(0));
            } else if op == OP_DCL_RESOURCE {
                let (idx, q) = p.operand(&w, i + 1)?;
                p.textures
                    .insert(idx[0], ((tok >> 11) & 0x1f, at(q)? & 0xf));
            } else if op == OP_DCL_SAMPLER {
                let (idx, _) = p.operand(&w, i + 1)?;
                p.samplers.insert(idx[0], (tok >> 11) & 0xf);
            } else if let Some(b) = op_buffer(op) {
                p.buffers.push(b);
            }
            i += n;
        }
        Ok(p)
    }

    /// Records the immediate constant buffer reads of the instruction at w[i:end].
    fn walk(&mut self, w: &[u32], i: usize, end: usize) {
        let mut p = i + 1;
        let mut ext = w[i] >> 31;
        while ext != 0 && p < end {
            ext = w[p] >> 31;
            p += 1;
        }
        let mut reads = self.cb_reads.take().unwrap();
        let mut ok = true;
        while p < end {
            match Self::operand_reads(w, p, end, &mut reads) {
                Ok(q) => p = q,
                Err(Bad) => {
                    ok = false;
                    break;
                }
            }
        }
        self.cb_reads = if ok && p == end { Some(reads) } else { None };
    }

    fn operand_reads(
        w: &[u32],
        mut p: usize,
        end: usize,
        reads: &mut BTreeSet<(u32, u32, u32)>,
    ) -> Result<usize, Bad> {
        let get = |k: usize| w.get(k).copied().ok_or(Bad);
        let tok = get(p)?;
        p += 1;
        let mut ext = tok >> 31;
        while ext != 0 {
            if p >= end {
                return Err(Bad);
            }
            ext = get(p)? >> 31;
            p += 1;
        }
        let (ncomp, otype, dim) = (tok & 3, (tok >> 12) & 0xff, (tok >> 20) & 3);
        if otype == 4 || otype == 5 {
            let n = match ncomp {
                1 => 1,
                2 => 4,
                _ => 0,
            };
            return Ok(p + n * if otype == 4 { 1 } else { 2 });
        }
        let mut idx: Vec<Option<u32>> = Vec::new();
        for k in 0..dim {
            let rep = (tok >> (22 + 3 * k)) & 7;
            match rep {
                0 => {
                    idx.push(Some(get(p)?));
                    p += 1;
                }
                1 => {
                    idx.push(None);
                    p += 2;
                }
                2..=4 => {
                    p += [0, 1, 2][(rep - 2) as usize];
                    idx.push(None);
                    p = Self::operand_reads(w, p, end, reads)?;
                }
                _ => return Err(Bad),
            }
        }
        if otype == 8 && dim == 2 && idx.iter().all(Option::is_some) && ncomp == 2 {
            let mode = (tok >> 2) & 3;
            let comp = if mode != 0 {
                (tok >> 4) & 3
            } else {
                (0..4).find(|c| (tok >> (4 + c)) & 1 != 0).unwrap_or(0)
            };
            reads.insert((idx[0].unwrap(), idx[1].unwrap(), comp));
        }
        if p > end {
            return Err(Bad);
        }
        Ok(p)
    }

    /// Immediate register indices of a declaration's operand; the position after it.
    fn operand(&self, w: &[u32], mut p: usize) -> R<(Vec<u32>, usize)> {
        let get = |k: usize| -> R<u32> {
            match w.get(k) {
                Some(&x) => Ok(x),
                None => crash("tuple index out of range"),
            }
        };
        let tok = get(p)?;
        p += 1;
        if tok >> 31 != 0 {
            p += 1;
        }
        let mut idx = Vec::new();
        for k in 0..((tok >> 20) & 3) {
            let rep = (tok >> (22 + 3 * k)) & 7;
            if rep != 0 {
                return uns("declaration with a relative register index");
            }
            idx.push(get(p)?);
            p += 1;
        }
        if self.major == 5 && self.minor == 1 {
            return uns("shader model 5.1 (register spaces)");
        }
        if idx.is_empty() {
            return crash("list index out of range");
        }
        Ok((idx, p))
    }
}

fn rdef_program_type(stage: u32) -> u32 {
    match stage {
        0 => 0xffff,
        1 => 0xfffe,
        2 => 0x4753,
        3 => 0x4853,
        4 => 0x4453,
        5 => 0x4353,
        _ => 0,
    }
}

/// (D3D_SHADER_VARIABLE_CLASS, D3D_SHADER_VARIABLE_TYPE, size in bytes) of a Unity param.
fn var_layout(p: &crate::bindings::Param) -> R<(u32, u32, u32)> {
    let typ = match p.typ {
        0 => 3,
        1 => 2,
        2 => 1,
        5 => 19,
        t => {
            return uns(format!(
                "constant {} of Unity type {t}",
                crate::py::repr(&p.name)
            ))
        }
    };
    let (cls, regs, last) = if p.is_matrix != 0 {
        (3, p.cols, p.rows * 4)
    } else {
        (
            if p.cols == 1 && p.rows == 1 { 0 } else { 1 },
            p.rows,
            p.cols * 4,
        )
    };
    let one = (regs - 1) * 16 + last;
    let size = if p.array <= 1 {
        one
    } else {
        (p.array - 1) * regs * 16 + one
    };
    Ok((cls, typ, size as u32))
}

/// An RDEF chunk as HLSLcc reads it.
fn rdef(
    prog: &Program,
    cbs: &[(u32, &crate::bindings::ConstantBuffer)],
    textures: &[(u32, String)],
    samplers: &[(u32, String)],
) -> R<Vec<u8>> {
    let sm5 = prog.major >= 5;
    let var_words = if sm5 { 10 } else { 6 };
    let mut strings: Vec<u8> = Vec::new();
    let mut str_at: BTreeMap<String, usize> = BTreeMap::new();
    let mut s = |text: &str| -> usize {
        if let Some(&o) = str_at.get(text) {
            return o;
        }
        let o = strings.len();
        str_at.insert(text.to_string(), o);
        strings.extend(text.as_bytes());
        strings.push(0);
        o
    };
    // (name, type, return type, dimension, samples, bind point, count, flags)
    let mut binds: Vec<(String, [u32; 7])> = Vec::new();
    for (reg, name) in samplers {
        binds.push((
            name.clone(),
            [
                3,
                0,
                0,
                0,
                *reg,
                1,
                if prog.samplers[reg] == 1 { 2 } else { 0 },
            ],
        ));
    }
    for (reg, name) in textures {
        let (dim, ret) = prog.textures[reg];
        let ms = dim == 4 || dim == 9;
        binds.push((
            name.clone(),
            [
                2,
                ret,
                dims(dim).unwrap().0,
                if ms { 0 } else { 0xffffffff },
                *reg,
                1,
                0xc,
            ],
        ));
    }
    for (reg, cb) in cbs {
        binds.push((cb.name.clone(), [0, 0, 0, 0, *reg, 1, 0]));
    }
    let nvars: usize = cbs.iter().map(|(_, cb)| cb.params.len()).sum();
    let head = 7 * 4;
    let bind_off = head;
    let cb_off = bind_off + 32 * binds.len();
    let var_off = cb_off + 24 * cbs.len();
    let type_off = var_off + 4 * var_words * nvars;
    let str_base = type_off + 16 * nvars;
    let target = prog.minor | (prog.major << 8) | (rdef_program_type(prog.stage) << 16);
    let mut out: Vec<u8> = Vec::new();
    let put = |out: &mut Vec<u8>, v: &[u32]| {
        for x in v {
            out.extend(x.to_le_bytes());
        }
    };
    let creator = s("pmunity");
    put(
        &mut out,
        &[
            cbs.len() as u32,
            cb_off as u32,
            binds.len() as u32,
            bind_off as u32,
            target,
            0,
            (str_base + creator) as u32,
        ],
    );
    for (name, rest) in &binds {
        let o = s(name);
        put(&mut out, &[(str_base + o) as u32]);
        put(&mut out, rest);
    }
    let mut v = 0usize;
    for (_, cb) in cbs {
        let o = s(&cb.name);
        put(
            &mut out,
            &[
                (str_base + o) as u32,
                cb.params.len() as u32,
                (var_off + 4 * var_words * v) as u32,
                cb.size as u32,
                0,
                0,
            ],
        );
        v += cb.params.len();
    }
    let mut types: Vec<u8> = Vec::new();
    v = 0;
    for (_, cb) in cbs {
        for p in &cb.params {
            let (cls, typ, size) = var_layout(p)?;
            let o = s(&p.name);
            put(
                &mut out,
                &[
                    (str_base + o) as u32,
                    p.offset as u32,
                    size,
                    2,
                    (type_off + 16 * v) as u32,
                    0,
                ],
            );
            if sm5 {
                put(&mut out, &[0xffffffff, 0, 0xffffffff, 0]);
            }
            for h in [
                cls,
                typ,
                p.rows as u32,
                p.cols as u32,
                if p.array > 1 { p.array as u32 } else { 0 },
                0,
            ] {
                types.extend((h as u16).to_le_bytes());
            }
            types.extend(0u32.to_le_bytes());
            v += 1;
        }
    }
    out.extend(types);
    assert_eq!(out.len(), str_base);
    out.extend(strings);
    Ok(out)
}

/// The DXBC container with an RDEF chunk rebuilt from Unity's binding info (dxbc.with_reflection).
pub fn with_reflection(dxbc: &[u8], b: &Bindings) -> R<(Vec<u8>, Program)> {
    let chs: Vec<(String, Vec<u8>)> = chunks(dxbc)?
        .into_iter()
        .filter(|(f, _)| f != "RDEF")
        .collect();
    let Some(code) = chs.iter().find(|(f, _)| f == "SHDR" || f == "SHEX") else {
        return uns("no shader code chunk");
    };
    let prog = Program::new(&code.1)?;
    if !prog.buffers.is_empty() {
        let mut bs: Vec<&str> = prog.buffers.clone();
        bs.sort();
        bs.dedup();
        return uns(format!(
            "{} shader uses {}",
            stage_name(prog.stage),
            bs.join(", ")
        ));
    }
    let mut by_name: BTreeMap<&str, &crate::bindings::ConstantBuffer> = BTreeMap::new();
    for cb in &b.cbuffers {
        by_name.insert(&cb.name, cb);
    }
    let mut cb_regs: BTreeMap<i32, &str> = BTreeMap::new();
    let mut tex_regs: BTreeMap<i32, &crate::bindings::Binding> = BTreeMap::new();
    for x in &b.bindings {
        if x.kind == KIND_CBUFFER {
            cb_regs.insert(x.index, &x.name);
        } else if x.kind == KIND_TEXTURE {
            tex_regs.insert(x.index, x);
        }
    }
    let mut cbs = Vec::new();
    for &reg in prog.cbuffers.keys() {
        let name = cb_regs.get(&(reg as i32)).copied();
        let Some(cb) = name.and_then(|n| by_name.get(n)) else {
            return uns(format!(
                "constant buffer cb{reg} is not named in the binding info"
            ));
        };
        if !cb.structs.is_empty() {
            return uns(format!(
                "constant buffer {} has struct members (GPU instancing data): not translated",
                crate::py::repr(&cb.name)
            ));
        }
        cbs.push((reg, *cb));
    }
    let mut extents: BTreeMap<u32, Vec<(u32, u32)>> = BTreeMap::new();
    for (reg, cb) in &cbs {
        let mut e = Vec::new();
        for p in &cb.params {
            e.push((p.offset as u32, p.offset as u32 + var_layout(p)?.2));
        }
        extents.insert(*reg, e);
    }
    for &(reg, vec, comp) in prog.cb_reads.iter().flatten() {
        let at = vec * 16 + comp * 4;
        if !extents
            .get(&reg)
            .is_some_and(|e| e.iter().any(|&(a, b)| a <= at && at < b))
        {
            return uns(format!(
                "cb{reg}[{vec}].{} is read but the binding info has no constant there",
                ["x", "y", "z", "w"][comp as usize]
            ));
        }
    }
    let mut textures = Vec::new();
    for (&reg, &(dim, _)) in &prog.textures {
        let Some(bi) = tex_regs.get(&(reg as i32)) else {
            return uns(format!("texture t{reg} is not named in the binding info"));
        };
        let Some((_, udim)) = dims(dim) else {
            return uns(format!(
                "texture {}: resource dimension {dim}",
                crate::py::repr(&bi.name)
            ));
        };
        if !ES3_DIMS.contains(&dim) {
            return uns(format!(
                "{} texture {} (not in GLSL ES 3.10)",
                dim_name(dim),
                crate::py::repr(&bi.name)
            ));
        }
        if let Some(u) = udim {
            if bi.dim() != Some(u) {
                return uns(format!(
                    "texture {}: declared {} but Unity says dimension {}",
                    crate::py::repr(&bi.name),
                    dim_name(dim),
                    bi.dim().map_or("None".into(), |d| d.to_string())
                ));
            }
        }
        textures.push((reg, bi.name.clone()));
    }
    let names: BTreeMap<u32, &str> = textures.iter().map(|(r, n)| (*r, n.as_str())).collect();
    let samplers: Vec<(u32, String)> = prog
        .samplers
        .keys()
        .map(|&reg| {
            (
                reg,
                names
                    .get(&reg)
                    .map_or(format!("sampler{reg}"), |n| format!("sampler{n}")),
            )
        })
        .collect();
    let rd = rdef(&prog, &cbs, &textures, &samplers)?;
    let mut all = vec![("RDEF".to_string(), rd)];
    all.extend(chs);
    Ok((build_container(&all), prog))
}

// ---------------------------------------------------------------- HLSLcc (pmhlslcc)

/// $PMHLSLCC, else pmhlslcc on PATH (dxbc.find_hlslcc; a setup gives $PMHLSLCC).
pub fn find_hlslcc() -> Option<String> {
    if let Ok(p) = std::env::var("PMHLSLCC") {
        if !p.is_empty() {
            return Some(p);
        }
    }
    let path = std::env::var("PATH").ok()?;
    path.split(':')
        .map(|d| std::path::Path::new(d).join("pmhlslcc"))
        .find(|p| p.is_file())
        .map(|p| p.to_string_lossy().into_owned())
}

/// A pmhlslcc server process (restarted if HLSLcc crashes on a program).
pub struct Translator {
    exe: String,
    flags: u32,
    proc: Option<(Child, ChildStdin, BufReader<ChildStdout>)>,
    pub crashes: usize,
}

impl Translator {
    pub fn new(exe: Option<String>) -> Result<Translator, String> {
        let exe = exe.or_else(find_hlslcc);
        match exe {
            Some(e) if std::path::Path::new(&e).exists() => Ok(Translator {
                exe: e,
                flags: FLAGS,
                proc: None,
                crashes: 0,
            }),
            _ => Err(
                "pmhlslcc not found: build it with tools/hlslcc/build.sh or set PMHLSLCC".into(),
            ),
        }
    }

    pub fn close(&mut self) {
        if let Some((mut child, mut stdin, _)) = self.proc.take() {
            let _ = stdin.write_all(b"0 0 x\n");
            drop(stdin);
            let _ = child.wait();
        }
    }

    /// [DXBC container] -> [GLSL text] (same order); Unsupported with HLSLcc's log.
    pub fn translate(
        &mut self,
        stages: &[Vec<u8>],
        flags: Option<u32>,
    ) -> Result<Vec<String>, Unsupported> {
        if self.proc.is_none() {
            let mut child = Command::new(&self.exe)
                .arg("-s")
                .stdin(Stdio::piped())
                .stdout(Stdio::piped())
                .spawn()
                .map_err(|e| Unsupported(format!("pmhlslcc: {e}")))?;
            let stdin = child.stdin.take().unwrap();
            let stdout = BufReader::new(child.stdout.take().unwrap());
            self.proc = Some((child, stdin, stdout));
        }
        let flags = flags.unwrap_or(self.flags);
        let mut head = format!("{} {flags:#x} {LANG}", stages.len());
        for s in stages {
            head += &format!(" {}", s.len());
        }
        head.push('\n');
        let res = (|| -> Option<(i64, Vec<String>, String)> {
            let (_, stdin, stdout) = self.proc.as_mut().unwrap();
            stdin.write_all(head.as_bytes()).ok()?;
            for s in stages {
                stdin.write_all(s).ok()?;
            }
            stdin.flush().ok()?;
            let mut line = String::new();
            stdout.read_line(&mut line).ok()?;
            let parts: Vec<&str> = line.split_whitespace().collect();
            if parts.is_empty() {
                return None;
            }
            let status: i64 = parts[0].parse().ok()?;
            let n: usize = parts.get(1)?.parse().ok()?;
            let lens: Vec<usize> = parts[2..]
                .iter()
                .map(|x| x.parse().ok())
                .collect::<Option<_>>()?;
            let mut texts = Vec::new();
            for &k in lens.iter().take(n) {
                let mut b = vec![0u8; k];
                stdout.read_exact(&mut b).ok()?;
                texts.push(String::from_utf8(b).ok()?);
            }
            let mut lb = vec![0u8; *lens.get(n)?];
            stdout.read_exact(&mut lb).ok()?;
            Some((status, texts, String::from_utf8_lossy(&lb).into_owned()))
        })();
        let Some((status, texts, log)) = res else {
            if let Some((mut child, _, _)) = self.proc.take() {
                let _ = child.kill();
                let _ = child.wait();
            }
            self.crashes += 1;
            return Err(Unsupported("HLSLcc crashed on this program".into()));
        };
        if status != 0 {
            let errs: Vec<&str> = log.lines().filter(|l| l.contains("error")).collect();
            let msg = if !errs.is_empty() {
                errs.join("; ")
            } else if !crate::py::strip(&log).is_empty() {
                crate::py::strip(&log).to_string()
            } else {
                "failed".into()
            };
            return Err(Unsupported(format!("HLSLcc: {msg}")));
        }
        Ok(texts)
    }
}

impl Drop for Translator {
    fn drop(&mut self) {
        self.close();
    }
}

const DEFINE_BLOCKS: [&str; 2] = [
    "#define HLSLCC_ENABLE_UNIFORM_BUFFERS 1\n#if HLSLCC_ENABLE_UNIFORM_BUFFERS\n#define UNITY_UNIFORM\n#else\n#define UNITY_UNIFORM uniform\n#endif\n",
    "#define UNITY_SUPPORTS_UNIFORM_LOCATION 1\n#if UNITY_SUPPORTS_UNIFORM_LOCATION\n#define UNITY_LOCATION(x) layout(location = x)\n#define UNITY_BINDING(x) layout(binding = x, std140)\n#else\n#define UNITY_LOCATION(x)\n#define UNITY_BINDING(x) layout(std140)\n#endif\n",
];

struct Patterns {
    ifdef_ext: Regex,
    location: Regex,
    binding: Regex,
    unity_uniform: Regex,
    uniform_line: Regex,
    no_es: Regex,
    cube_shadow: Regex,
    assign: Regex,
    flipy_decl: Regex,
    flipy: Regex,
}

const OPERAND: &str = r"(\w+)(?:\.([xyzw]+))?";
const VID: &str = r"(?:uint|int)\(gl_VertexID\)|gl_VertexID";

static RX: LazyLock<Patterns> = LazyLock::new(|| Patterns {
    ifdef_ext: glsl::re(r"(?m)^#ifdef (\w+)\n(#extension (\w+) : enable\n)#endif\n"),
    location: glsl::re(r"\bUNITY_LOCATION\(\s*\d+\s*\)\s*"),
    binding: glsl::re(r"\bUNITY_BINDING\(\s*\d+\s*\)"),
    unity_uniform: glsl::re(r"\bUNITY_UNIFORM\b"),
    uniform_line: glsl::re(
        r"\Auniform\s+(?:(?:highp|mediump|lowp)\s+)?\w+\s+(\w+)(?:\[\d+\])?\s*;\n?\z",
    ),
    no_es: glsl::re(r"\b(textureSamples|imageSamples|textureQueryLevels|textureQueryLod)\s*\("),
    cube_shadow: glsl::re(
        r"(?m)^\s*(?:UNITY_LOCATION\(\d+\)\s*)?uniform\s+(?:(?:highp|mediump|lowp)\s+)?samplerCubeShadow\s+(\w+)\s*;",
    ),
    assign: glsl::re(r"\A(\s*)(\w+)(?:\.([xyzw]+))?\s*=\s*(.+?);\s*\n?\z"),
    flipy_decl: glsl::re(r"(?m)^\s*uniform\s+(?:highp\s+|mediump\s+)?float\s+_FlipY\s*;"),
    flipy: glsl::re(r"\b_FlipY\s*<\s*0\.5\b"),
});

/// HLSLcc's GLSL as Unity 2017-2018 stored it (dxbc.tidy).
pub fn tidy(src: &str) -> String {
    let r = &*RX;
    // ^#ifdef (\w+)\n(#extension \1 : enable\n)#endif\n -> \2 (the back reference checked here)
    let mut s = String::with_capacity(src.len());
    let mut last = 0;
    let mut pos = 0;
    while let Some(c) = r.ifdef_ext.captures_at(src, pos) {
        let m = c.get(0).unwrap();
        if c[1] == c[3] {
            s.push_str(&src[last..m.start()]);
            s.push_str(&c[2]);
            last = m.end();
            pos = m.end();
        } else {
            pos = m.start() + 1;
        }
        if pos > src.len() {
            break;
        }
    }
    s.push_str(&src[last..]);
    for b in DEFINE_BLOCKS {
        s = s.replace(b, "");
    }
    let s = r.location.replace_all(&s, "");
    let s = r.binding.replace_all(&s, NoExpand("layout(std140)"));
    r.unity_uniform
        .replace_all(&s, NoExpand("uniform"))
        .into_owned()
}

/// {stage: glsl} -> one GL program as Unity stores it (dxbc.gl_program).
pub fn gl_program(vs: &str, ps: &str, tidy_up: bool) -> String {
    let mut out = String::new();
    for (st, src) in [("VERTEX", vs), ("FRAGMENT", ps)] {
        let t = if tidy_up { tidy(src) } else { src.to_string() };
        out += &format!("#ifdef {st}\n{t}\n#endif\n");
    }
    out
}

/// (major, minor) of the oldest Unity version that writes a serializer's entries.
pub fn unity_line(serializer: Option<i32>) -> (u64, u64) {
    match serializer {
        Some(201708220) => (2017, 3),
        Some(201802150) => (2018, 2),
        Some(201806140) => (2019, 1),
        Some(202012090) => (2021, 2),
        _ => (2017, 3),
    }
}

/// The stage's loose uniforms in the order Unity's own GL compile declares them.
fn unity_uniform_order(src: &str, b: &Bindings, prog: &Program) -> String {
    let r = &*RX;
    let mut regs: BTreeMap<i32, &str> = BTreeMap::new();
    for x in &b.bindings {
        if x.kind == KIND_CBUFFER {
            regs.insert(x.index, &x.name);
        }
    }
    let mut by_name: BTreeMap<&str, &crate::bindings::ConstantBuffer> = BTreeMap::new();
    for cb in &b.cbuffers {
        by_name.insert(&cb.name, cb);
    }
    let mut rank: BTreeMap<String, (usize, usize)> = BTreeMap::new();
    for (n, reg) in prog.cbuffers.keys().enumerate() {
        let cb = regs.get(&(*reg as i32)).and_then(|nm| by_name.get(nm));
        if let Some(cb) = cb {
            for p in &cb.params {
                let gl = if p.is_matrix != 0 {
                    format!("hlslcc_mtx{}x{}{}", p.rows, p.cols, p.name)
                } else {
                    p.name.clone()
                };
                rank.entry(gl)
                    .or_insert((if cb.name == "$Globals" { 1 } else { 0 }, n));
            }
        }
    }
    let mut lines: Vec<String> = src.split('\n').map(String::from).collect();
    let at: Vec<usize> = lines
        .iter()
        .enumerate()
        .filter(|(_, l)| r.uniform_line.is_match(l) && !l.contains("sampler"))
        .map(|(i, _)| i)
        .collect();
    if at.is_empty() || at != (at[0]..at[0] + at.len()).collect::<Vec<_>>() {
        return src.to_string();
    }
    let mut block: Vec<String> = at.iter().map(|&i| lines[i].clone()).collect();
    let default = (1, rank.len());
    block.sort_by_key(|l| {
        let name = r.uniform_line.captures(l).unwrap()[1].to_string();
        *rank.get(&name).unwrap_or(&default)
    });
    for (k, l) in block.into_iter().enumerate() {
        lines[at[0] + k] = l;
    }
    lines.join("\n")
}

/// HLSLcc's GLSL 1.50 where GLSL ES has no such call (dxbc.gl_fixups).
pub fn gl_fixups(src: &str) -> R<String> {
    let r = &*RX;
    if let Some(m) = r.no_es.captures(src) {
        return uns(format!("{}() (not in GLSL ES)", &m[1]));
    }
    let cubes: Vec<String> = r
        .cube_shadow
        .captures_iter(src)
        .map(|c| c[1].to_string())
        .collect();
    let mut s = src.to_string();
    for name in cubes {
        let rx = glsl::re(&format!(
            r"\btextureLod\(\s*{name}\s*,\s*([^;]*?),\s*0\.0\s*\)"
        ));
        s = rx
            .replace_all(&s, |c: &Captures| format!("texture({name}, {})", &c[1]))
            .into_owned();
    }
    Ok(s)
}

fn full(rx: &str) -> Regex {
    glsl::re(&format!(r"\A(?:{rx})\z"))
}

struct UvPatterns {
    tri: Regex,
    tri2: Regex,
    bot: Regex,
    top: Regex,
    sum: Regex,
    quad: Regex,
    cast: Regex,
    one: Regex,
    flip: Regex,
    operand: Regex,
}

static UV: LazyLock<UvPatterns> = LazyLock::new(|| UvPatterns {
    tri: full(&format!(r"(?:{VID})\s*&\s*2u?")),
    tri2: full(&format!(r"(?:u?int)\((?:{VID})\s*&\s*2u?\)")),
    bot: full(&format!(r"(?:u?int\()?(?:{VID})\s*&\s*1u?\)?")),
    top: full(&format!(r"(?:u?int\()?(?:{VID})\s*>>\s*1u?\)?")),
    sum: full(&format!(
        r"(?:u?int\()?({OPERAND})\)?\s*\+\s*(?:u?int\()?({OPERAND})\)?"
    )),
    quad: full(&format!(r"(?:u?int\()?({OPERAND})\)?\s*&\s*1u?")),
    cast: full(&format!(r"(?:float|vec[234])\(({OPERAND})\)")),
    one: full(r"float\(1\.0\)"),
    flip: full(&format!(
        r"\(-({OPERAND})\)\s*\+\s*(?:1\.0|vec[234]\((?:1\.0,\s*)*1\.0\))"
    )),
    operand: full(OPERAND),
});

type Tags = BTreeMap<(String, String), Option<String>>;

fn comps(var: &str, swz: Option<&str>, n: usize) -> Vec<(String, String)> {
    match swz {
        Some(s) if !s.is_empty() => s
            .chars()
            .map(|c| (var.to_string(), c.to_string()))
            .collect(),
        _ => vec![(var.to_string(), String::new()); n],
    }
}

fn operand_tags(tags: &Tags, text: &str) -> Option<Vec<Option<String>>> {
    let c = UV.operand.captures(crate::py::strip(text))?;
    Some(
        comps(&c[1], c.get(2).map(|m| m.as_str()), 1)
            .into_iter()
            .map(|k| tags.get(&k).cloned().flatten())
            .collect(),
    )
}

fn scalar(tags: &Tags, text: &str) -> Option<String> {
    let t = operand_tags(tags, text)?;
    if t.len() == 1 {
        t[0].clone()
    } else {
        None
    }
}

/// Per-component tags of a right-hand side, or None (gl_uv_origin's value()).
fn value(tags: &Tags, rhs: &str) -> Option<Vec<Option<String>>> {
    let u = &*UV;
    let r = crate::py::strip(rhs);
    let one = |s: &str| Some(vec![Some(s.to_string())]);
    if u.tri.is_match(r) || u.tri2.is_match(r) {
        return one("tri");
    }
    if u.bot.is_match(r) {
        return one("bot");
    }
    if u.top.is_match(r) {
        return one("top");
    }
    if let Some(m) = u.sum.captures(r) {
        let a = scalar(tags, &m[1]);
        let b = scalar(tags, &m[4]);
        let set: BTreeSet<Option<String>> = [a, b].into_iter().collect();
        let want: BTreeSet<Option<String>> = [Some("top".to_string()), Some("bot".to_string())]
            .into_iter()
            .collect();
        if set == want {
            return one("sum");
        }
    }
    if let Some(m) = u.quad.captures(r) {
        if scalar(tags, &m[1]).as_deref() == Some("sum") {
            return one("quad");
        }
    }
    if let Some(m) = u.cast.captures(r) {
        let t = operand_tags(tags, &m[1])?;
        return Some(
            t.into_iter()
                .map(|x| match x.as_deref() {
                    Some("tri") => Some("tri.f".into()),
                    Some("quad") => Some("quad.f".into()),
                    _ => None,
                })
                .collect(),
        );
    }
    if u.one.is_match(r) {
        return Some(vec![None]);
    }
    if let Some(m) = u.flip.captures(r) {
        if let Some(t) = operand_tags(tags, &m[1]) {
            if t.iter()
                .all(|x| matches!(x.as_deref(), Some("tri.f") | Some("quad.f")))
            {
                return Some(vec![Some(format!("flip:{}", &m[1])); t.len()]);
            }
        }
    }
    None
}

/// The vertex stage with the SRP core library's texture coordinate flips for Direct3D's origin
/// taken out: (text, flips undone) (dxbc.gl_uv_origin).
pub fn gl_uv_origin(vs: &str) -> (String, usize) {
    if !vs.contains("gl_VertexID") {
        return (vs.to_string(), 0);
    }
    let r = &*RX;
    let mut tags: Tags = BTreeMap::new();
    let mut n = 0;
    let mut out = Vec::new();
    for line in vs.split('\n') {
        let mut line = line.to_string();
        let lstripped = line.trim_start_matches(crate::py::isspace);
        if let Some(m) = r
            .assign
            .captures(&line.clone())
            .filter(|_| !lstripped.starts_with("//") && !lstripped.starts_with('#'))
        {
            let (ind, var, swz, rhs) = (
                m[1].to_string(),
                m[2].to_string(),
                m.get(3).map(|x| x.as_str().to_string()),
                m[4].to_string(),
            );
            let mut v = value(&tags, &rhs);
            let keys = comps(&var, swz.as_deref(), 1);
            if let Some(src) = v
                .as_ref()
                .and_then(|v| v.first().cloned().flatten())
                .and_then(|s| s.strip_prefix("flip:").map(String::from))
            {
                line = format!(
                    "{ind}{var}{} = {src};",
                    swz.as_ref().map_or(String::new(), |s| format!(".{s}"))
                );
                v = operand_tags(&tags, &src);
                n += 1;
            }
            if let Some(vv) = &v {
                if vv.len() == 1 && keys.len() > 1 {
                    v = Some(vec![vv[0].clone(); keys.len()]);
                }
            }
            for (i, k) in keys.into_iter().enumerate() {
                let t = v.as_ref().and_then(|v| v.get(i).cloned()).flatten();
                tags.insert(k, t);
            }
            if swz.is_none() {
                for c in ["x", "y", "z", "w"] {
                    tags.remove(&(var.clone(), c.to_string()));
                }
            }
        }
        out.push(line);
    }
    let mut text = out.join("\n");
    if r.flipy_decl.is_match(&text) {
        let k = r.flipy.find_iter(&text).count();
        text = r.flipy.replace_all(&text, NoExpand("false")).into_owned();
        n += k;
    }
    (text, n)
}

/// The D3D11 vertex and pixel entries (code, binding info) -> GL program text, GLSL 1.50 as Unity's
/// GLCore programs (dxbc.translate_program). notes gets 'VERTEX uv-origin <n>'.
pub fn translate_program(
    tr: &mut Translator,
    vs: (&[u8], &Bindings),
    ps: (&[u8], &Bindings),
    unity: (u64, u64),
    notes: &mut Vec<String>,
) -> R<String> {
    let new = unity >= (2019, 1);
    let mut stages = Vec::new();
    for (code, b) in [vs, ps] {
        let (_, dxbc) = split_code(code)?;
        let (cont, prog) = with_reflection(&dxbc, b)?;
        stages.push((prog, cont, b));
    }
    if stages[0].0.stage != 1 || stages[1].0.stage != 0 {
        return uns(format!(
            "expected a vertex and a pixel shader, got {}",
            stages
                .iter()
                .map(|s| stage_name(s.0.stage))
                .collect::<Vec<_>>()
                .join(", ")
        ));
    }
    let texts = tr
        .translate(
            &[stages[0].1.clone(), stages[1].1.clone()],
            if new { Some(FLAGS_2019) } else { None },
        )
        .map_err(Fault::Unsupported)?;
    let (vs_glsl, ps_glsl) = (gl_fixups(&texts[0])?, gl_fixups(&texts[1])?);
    let (mut vs_glsl, flips) = gl_uv_origin(&vs_glsl);
    if flips > 0 {
        notes.push(format!("VERTEX uv-origin {flips}"));
    }
    let mut ps_glsl = ps_glsl;
    if new {
        vs_glsl = unity_uniform_order(&vs_glsl, stages[0].2, &stages[0].0);
        ps_glsl = unity_uniform_order(&ps_glsl, stages[1].2, &stages[1].0);
    }
    Ok(gl_program(&vs_glsl, &ps_glsl, !new))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn container_round_trip() {
        let chs = vec![
            ("SHDR".to_string(), vec![0x40, 0, 1, 0, 2, 0, 0, 0]),
            ("ISGN".to_string(), vec![1, 2, 3]),
        ];
        let c = build_container(&chs);
        assert_eq!(&c[..4], b"DXBC");
        assert_eq!(chunks(&c).unwrap(), chs);
        let mut code = vec![1, 0, 0, 0, 0, 0];
        code.extend(&c);
        let (h, d) = split_code(&code).unwrap();
        assert_eq!(h.len(), 6);
        assert_eq!(d, c);
    }

    #[test]
    fn tidy_and_uv() {
        let s = "#version 150\n#ifdef GL_ARB_x\n#extension GL_ARB_x : enable\n#endif\nUNITY_LOCATION(3) uniform sampler2D t;\n";
        assert_eq!(
            tidy(s),
            "#version 150\n#extension GL_ARB_x : enable\nuniform sampler2D t;\n"
        );
        let vs = "void main()\n{\n    u_xlati0 = int(uint(gl_VertexID) & 2u);\n    u_xlat0.y = float(u_xlati0);\n    vs_TEXCOORD0.y = (-u_xlat0.y) + 1.0;\n}\n";
        let (out, n) = gl_uv_origin(vs);
        assert_eq!(n, 1);
        assert!(out.contains("    vs_TEXCOORD0.y = u_xlat0.y;"));
    }
}
