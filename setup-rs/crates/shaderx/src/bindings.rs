//! Unity's binding info of a program blob entry and the binding info Unity's GL compiler gives a GL
//! program (shaders/dxbc.py's Bindings, entry_bindings, gl_rest and gl_bindings; its docstring has
//! the layouts): constant buffers with their members, textures, constant buffer bindings and inline
//! samplers, read from an entry's bytes after its code (2021.2+: and its parameter entry) and
//! written back byte for byte; Unity 6000.6's typed layout (each binding an (encoded data, slot)
//! pair, lists by kind) read where asked for (the Vulkan path: entry_bindings_typed) and written
//! back in it.
use crate::blob::Entry;
use crate::glsl;
use regex::Regex;
use std::collections::HashMap;
use std::sync::LazyLock;

/// A program this path cannot give binding info (dxbc.Unsupported).
#[derive(Clone, Debug, PartialEq)]
pub struct Unsupported(pub String);

impl std::fmt::Display for Unsupported {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

type R<T> = Result<T, Unsupported>;

/// Serializer from which a texture binding's two ints are (sampler index, dimension << 1 |
/// multisampled) (Unity 2018.2+); before: (dimension, sampler index).
pub const PACKED_TEXTURE_FROM: i32 = 201802150;
/// 2021.2+: an entry's parameters are an entry of their own.
pub const SEPARATE_PARAMS_FROM: i32 = 202012090;
/// Unity 6000.6's typed binding info.
pub const TYPED_FROM: i32 = 202605290;

pub const KIND_TEXTURE: i32 = 0;
pub const KIND_CBUFFER: i32 = 1;
pub const KIND_BUFFER: i32 = 2;
pub const KIND_UAV: i32 = 3;
pub const KIND_SAMPLER: i32 = 4;
/// 6000.6's lists, in their order (Binding.TYPED_ORDER).
pub const TYPED_ORDER: [i32; 5] = [
    KIND_TEXTURE,
    KIND_UAV,
    KIND_CBUFFER,
    KIND_BUFFER,
    KIND_SAMPLER,
];

/// A constant buffer member.
#[derive(Clone, Debug, PartialEq)]
pub struct Param {
    pub name: String,
    pub typ: i32,
    pub rows: i32,
    pub cols: i32,
    pub is_matrix: i32,
    pub array: i32,
    pub offset: i32,
}

/// A struct member of a constant buffer (Unity's GPU instancing data).
#[derive(Clone, Debug, PartialEq)]
pub struct StructParam {
    pub name: String,
    pub offset: i32,
    pub array: i32,
    pub size: i32,
    pub params: Vec<Param>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ConstantBuffer {
    pub name: String,
    pub size: i32,
    pub params: Vec<Param>,
    pub structs: Vec<StructParam>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Binding {
    pub name: String,
    pub kind: i32,
    pub index: i32,
    pub extra: Vec<i32>,
    pub packed: bool,
    /// 6000.6 (typed): the encoded data of the binding (a texture's: and of its sampler's; a
    /// UAV's: and of the original binding); extra is then: texture [sampler slot, dimension << 1
    /// | multisampled], UAV [original slot], buffer [2 ints], inline sampler [state], constant
    /// buffer none.
    pub enc: Option<Vec<i32>>,
}

impl Binding {
    /// A texture's dimension (Unity's TextureDimension: 2 2D, 3 3D, 4 cube, 5 2D array).
    pub fn dim(&self) -> Option<i32> {
        if self.kind != KIND_TEXTURE {
            return None;
        }
        Some(if self.packed {
            self.extra[1] >> 1
        } else {
            self.extra[0]
        })
    }

    /// A texture's sampler index (-1: an inline sampler state).
    pub fn sampler(&self) -> Option<i32> {
        if self.kind != KIND_TEXTURE {
            return None;
        }
        Some(if self.packed {
            self.extra[0]
        } else {
            self.extra[1]
        })
    }

    /// The ints after the index, by kind (Binding.EXTRA).
    fn extra_count(kind: i32) -> Option<usize> {
        match kind {
            KIND_TEXTURE => Some(2),
            KIND_CBUFFER | KIND_SAMPLER => Some(1),
            _ => None,
        }
    }
}

/// Unity's binding info of one entry.
#[derive(Clone, Debug, PartialEq)]
pub struct Bindings {
    pub source_map: i32,
    pub channels: Vec<(i32, i32)>,
    pub cbuffers: Vec<ConstantBuffer>,
    pub bindings: Vec<Binding>,
    /// In Unity 6000.6's layout (read so, written back so).
    pub typed: bool,
}

fn pad4(n: usize) -> usize {
    (n + 3) & !3
}

struct Reader<'a> {
    d: &'a [u8],
    p: usize,
}

impl Reader<'_> {
    fn i32(&mut self) -> Result<i32, String> {
        let b = self
            .d
            .get(self.p..self.p + 4)
            .ok_or_else(|| "unpack_from requires a buffer of at least 4 bytes".to_string())?;
        self.p += 4;
        Ok(i32::from_le_bytes(b.try_into().unwrap()))
    }

    fn count(&mut self, limit: i32) -> Result<usize, String> {
        let n = self.i32()?;
        if !(0..=limit).contains(&n) {
            return Err(format!("count {n}"));
        }
        Ok(n as usize)
    }

    fn string(&mut self) -> Result<String, String> {
        let n = self.count(1024)?;
        if self.p + n > self.d.len() {
            return Err("string past the end".into());
        }
        let b = &self.d[self.p..self.p + n];
        if !b.is_ascii() {
            return Err("'ascii' codec can't decode".into());
        }
        let s = String::from_utf8(b.to_vec()).unwrap();
        self.p = pad4(self.p + n);
        Ok(s)
    }

    fn params(&mut self) -> Result<Vec<Param>, String> {
        let n = self.count(4096)?;
        let mut out = Vec::with_capacity(n);
        for _ in 0..n {
            let name = self.string()?;
            let v: Vec<i32> = (0..6).map(|_| self.i32()).collect::<Result<_, _>>()?;
            out.push(Param {
                name,
                typ: v[0],
                rows: v[1],
                cols: v[2],
                is_matrix: v[3],
                array: v[4],
                offset: v[5],
            });
        }
        Ok(out)
    }
}

impl Reader<'_> {
    /// 6000.6: a constant buffer's (or struct's) vectors, then its matrices (dxbc._typed_members).
    fn typed_members(&mut self) -> Result<Vec<Param>, String> {
        let mut out = Vec::new();
        for matrix in [false, true] {
            for _ in 0..self.count(4096)? {
                let name = self.string()?;
                let (packed, array, offset) = (self.i32()?, self.i32()?, self.i32()?);
                let (n, typ) = (packed & 0xffff, packed >> 16);
                out.push(if matrix {
                    Param {
                        name,
                        typ,
                        rows: n,
                        cols: 4,
                        is_matrix: 1,
                        array,
                        offset,
                    }
                } else {
                    Param {
                        name,
                        typ,
                        rows: 1,
                        cols: n,
                        is_matrix: 0,
                        array,
                        offset,
                    }
                });
            }
        }
        Ok(out)
    }
}

/// A Python tuple's repr of ints: (1,) or (1, 2).
fn tuple_repr(v: &[i32]) -> String {
    match v {
        [x] => format!("({x},)"),
        _ => format!(
            "({})",
            v.iter().map(i32::to_string).collect::<Vec<_>>().join(", ")
        ),
    }
}

/// A Python list's repr of ints.
fn list_repr(v: &[i32]) -> String {
    format!(
        "[{}]",
        v.iter().map(i32::to_string).collect::<Vec<_>>().join(", ")
    )
}

/// 6000.6: members as vectors then matrices (dxbc._typed_members_bytes).
fn typed_members_bytes(out: &mut Vec<u8>, params: &[Param]) -> R<()> {
    for matrix in [false, true] {
        let ps: Vec<&Param> = params
            .iter()
            .filter(|p| (p.is_matrix != 0) == matrix)
            .collect();
        ints(out, &[ps.len() as i32]);
        for p in ps {
            if matrix && (p.cols != 4 || p.is_matrix != 1) {
                return Err(Unsupported(format!(
                    "matrix {} of {}x{}: not in the 6000.6 layout",
                    p.name, p.rows, p.cols
                )));
            }
            if !matrix && (p.rows != 1 || p.is_matrix != 0) {
                return Err(Unsupported(format!(
                    "vector {} of {} rows: not in the 6000.6 layout",
                    p.name, p.rows
                )));
            }
            let n = if matrix { p.rows } else { p.cols };
            if !(0..=0xffff).contains(&n) {
                return Err(Unsupported(format!(
                    "parameter {}: size {n} not in the 6000.6 layout",
                    p.name
                )));
            }
            wstr(out, &p.name);
            ints(out, &[n | p.typ.wrapping_shl(16), p.array, p.offset]);
        }
    }
    Ok(())
}

fn wstr(out: &mut Vec<u8>, s: &str) {
    let b = s.as_bytes();
    out.extend((b.len() as i32).to_le_bytes());
    out.extend(b);
    out.resize(out.len() + pad4(b.len()) - b.len(), 0);
}

fn ints(out: &mut Vec<u8>, v: &[i32]) {
    for x in v {
        out.extend(x.to_le_bytes());
    }
}

fn param_bytes(out: &mut Vec<u8>, p: &Param) {
    wstr(out, &p.name);
    ints(
        out,
        &[p.typ, p.rows, p.cols, p.is_matrix, p.array, p.offset],
    );
}

impl Bindings {
    /// Bindings.parse (the layouts before 6000.6): serializer, the entry's version.
    pub fn parse(data: &[u8], serializer: Option<i32>) -> R<Bindings> {
        // (6000.6's typed layout is read only where asked for: TYPED_FROM data as the rest here)
        let packed = serializer.is_some_and(|s| s >= PACKED_TEXTURE_FROM);
        let mut r = Reader { d: data, p: 0 };
        let parsed = (|| -> Result<Bindings, Unsupported> {
            let e = |m: String| {
                Unsupported(format!(
                    "binding info does not match the known layout ({m})"
                ))
            };
            let source_map = r.i32().map_err(e)?;
            let nch = r.count(64).map_err(e)?;
            let mut channels = Vec::with_capacity(nch);
            for _ in 0..nch {
                channels.push((r.i32().map_err(e)?, r.i32().map_err(e)?));
            }
            let mut cbs = Vec::new();
            for _ in 0..r.count(256).map_err(e)? {
                let name = r.string().map_err(e)?;
                let size = r.i32().map_err(e)?;
                let params = r.params().map_err(e)?;
                let mut structs = Vec::new();
                for _ in 0..r.count(64).map_err(e)? {
                    let name = r.string().map_err(e)?;
                    let (offset, array, size) = (
                        r.i32().map_err(e)?,
                        r.i32().map_err(e)?,
                        r.i32().map_err(e)?,
                    );
                    structs.push(StructParam {
                        name,
                        offset,
                        array,
                        size,
                        params: r.params().map_err(e)?,
                    });
                }
                cbs.push(ConstantBuffer {
                    name,
                    size,
                    params,
                    structs,
                });
            }
            let mut binds = Vec::new();
            for _ in 0..r.count(256).map_err(e)? {
                let name = r.string().map_err(e)?;
                let (kind, index) = (r.i32().map_err(e)?, r.i32().map_err(e)?);
                let Some(n) = Binding::extra_count(kind) else {
                    return Err(Unsupported(format!(
                        "binding {} of kind {kind} (buffer or UAV?): not supported",
                        crate::py::repr(&name)
                    )));
                };
                let extra = (0..n)
                    .map(|_| r.i32())
                    .collect::<Result<_, _>>()
                    .map_err(e)?;
                binds.push(Binding {
                    name,
                    kind,
                    index,
                    extra,
                    packed,
                    enc: None,
                });
            }
            Ok(Bindings {
                source_map,
                channels,
                cbuffers: cbs,
                bindings: binds,
                typed: false,
            })
        })()?;
        if r.p != data.len() || parsed.serialize() != data {
            return Err(Unsupported(
                "binding info does not match the known layout (round trip)".into(),
            ));
        }
        Ok(parsed)
    }

    /// Bindings.parse with typed=True on data of 6000.6's serializer (dxbc.Bindings._parse_typed).
    pub fn parse_typed(data: &[u8]) -> R<Bindings> {
        let mut r = Reader { d: data, p: 0 };
        let parsed = (|| -> Result<Bindings, String> {
            let source_map = r.i32()?;
            let nch = r.count(64)?;
            let mut channels = Vec::with_capacity(nch);
            for _ in 0..nch {
                channels.push((r.i32()?, r.i32()?));
            }
            let mut cbs = Vec::new();
            for _ in 0..r.count(256)? {
                let name = r.string()?;
                let size = r.i32()?;
                let params = r.typed_members()?;
                let mut structs = Vec::new();
                for _ in 0..r.count(64)? {
                    let name = r.string()?;
                    let (offset, array, size) = (r.i32()?, r.i32()?, r.i32()?);
                    structs.push(StructParam {
                        name,
                        offset,
                        array,
                        size,
                        params: r.typed_members()?,
                    });
                }
                cbs.push(ConstantBuffer {
                    name,
                    size,
                    params,
                    structs,
                });
            }
            let mut binds = Vec::new();
            for kind in TYPED_ORDER {
                for _ in 0..r.count(256)? {
                    let name = if kind != KIND_SAMPLER {
                        r.string()?
                    } else {
                        String::new()
                    };
                    let (enc, slot) = (r.i32()?, r.i32()?);
                    let (extra, enc) = match kind {
                        KIND_TEXTURE => {
                            let (senc, sslot, dim) = (r.i32()?, r.i32()?, r.i32()?);
                            (vec![sslot, dim], vec![enc, senc])
                        }
                        KIND_UAV => {
                            let (oenc, oslot) = (r.i32()?, r.i32()?);
                            (vec![oslot], vec![enc, oenc])
                        }
                        KIND_BUFFER => (vec![r.i32()?, r.i32()?], vec![enc]),
                        KIND_SAMPLER => (vec![r.i32()?], vec![enc]),
                        _ => (Vec::new(), vec![enc]),
                    };
                    binds.push(Binding {
                        name,
                        kind,
                        index: slot,
                        extra,
                        packed: true,
                        enc: Some(enc),
                    });
                }
            }
            Ok(Bindings {
                source_map,
                channels,
                cbuffers: cbs,
                bindings: binds,
                typed: true,
            })
        })()
        .map_err(|m| {
            Unsupported(format!(
                "binding info does not match the 6000.6 layout ({m})"
            ))
        })?;
        if r.p != data.len() || parsed.try_serialize()? != data {
            return Err(Unsupported(
                "binding info does not match the 6000.6 layout (round trip)".into(),
            ));
        }
        for x in &parsed.bindings {
            if x.kind == KIND_BUFFER || x.kind == KIND_UAV {
                return Err(Unsupported(format!(
                    "binding {} of kind {} ({}): not supported",
                    crate::py::repr(&x.name),
                    x.kind,
                    if x.kind == KIND_BUFFER {
                        "buffer"
                    } else {
                        "UAV"
                    }
                )));
            }
            let enc = x.enc.as_deref().unwrap_or(&[]);
            if enc.iter().any(|&v| v != 0) {
                return Err(Unsupported(format!(
                    "binding {} with encoded data {} (6000.6): not understood",
                    crate::py::repr(&x.name),
                    tuple_repr(enc)
                )));
            }
        }
        Ok(parsed)
    }

    /// 6000.6's constant buffers and bindings (dxbc.Bindings._params_bytes_typed).
    fn params_bytes_typed(&self) -> R<Vec<u8>> {
        let mut out = Vec::new();
        ints(&mut out, &[self.cbuffers.len() as i32]);
        for cb in &self.cbuffers {
            wstr(&mut out, &cb.name);
            ints(&mut out, &[cb.size]);
            typed_members_bytes(&mut out, &cb.params)?;
            ints(&mut out, &[cb.structs.len() as i32]);
            for st in &cb.structs {
                wstr(&mut out, &st.name);
                ints(&mut out, &[st.offset, st.array, st.size]);
                typed_members_bytes(&mut out, &st.params)?;
            }
        }
        let mut other: Vec<i32> = self
            .bindings
            .iter()
            .map(|b| b.kind)
            .filter(|k| !TYPED_ORDER.contains(k))
            .collect();
        if !other.is_empty() {
            other.sort();
            other.dedup();
            return Err(Unsupported(format!(
                "binding kinds {} not in the 6000.6 layout",
                list_repr(&other)
            )));
        }
        for kind in TYPED_ORDER {
            let bs: Vec<&Binding> = self.bindings.iter().filter(|b| b.kind == kind).collect();
            ints(&mut out, &[bs.len() as i32]);
            // (encoded data ints, extra ints)
            let want = match kind {
                KIND_TEXTURE => (2, 2),
                KIND_UAV => (2, 1),
                KIND_BUFFER => (1, 2),
                KIND_SAMPLER => (1, 1),
                _ => (1, 0),
            };
            for b in bs {
                let enc = b.enc.clone().unwrap_or_else(|| vec![0; want.0]);
                if enc.len() != want.0 || b.extra.len() != want.1 {
                    return Err(Unsupported(format!(
                        "binding {} (kind {kind}): {} not in the 6000.6 layout",
                        crate::py::repr(&b.name),
                        list_repr(&b.extra)
                    )));
                }
                if kind != KIND_SAMPLER {
                    wstr(&mut out, &b.name);
                }
                ints(&mut out, &[enc[0], b.index]);
                if kind == KIND_TEXTURE || kind == KIND_UAV {
                    ints(&mut out, &[enc[1], b.extra[0]]);
                    ints(&mut out, &b.extra[1..]);
                } else {
                    ints(&mut out, &b.extra);
                }
            }
        }
        Ok(out)
    }

    /// params_bytes(), and in 6000.6's layout what does not fit it as the Python's Unsupported.
    pub fn try_params_bytes(&self) -> R<Vec<u8>> {
        if self.typed {
            self.params_bytes_typed()
        } else {
            Ok(self.params_bytes())
        }
    }

    /// serialize(), and in 6000.6's layout what does not fit it as the Python's Unsupported.
    pub fn try_serialize(&self) -> R<Vec<u8>> {
        let mut out = self.head();
        out.extend(self.try_params_bytes()?);
        Ok(out)
    }

    /// The source map and vertex inputs (2021.2+: all of the code entry's binding info).
    pub fn head(&self) -> Vec<u8> {
        let mut out = Vec::new();
        ints(&mut out, &[self.source_map, self.channels.len() as i32]);
        for &(s, t) in &self.channels {
            ints(&mut out, &[s, t]);
        }
        out
    }

    /// Constant buffers and bindings (2021.2+: a parameter entry's data). In 6000.6's layout:
    /// try_params_bytes (this gives nothing for a typed one that does not fit it).
    pub fn params_bytes(&self) -> Vec<u8> {
        if self.typed {
            return self.params_bytes_typed().unwrap_or_default();
        }
        let mut out = Vec::new();
        ints(&mut out, &[self.cbuffers.len() as i32]);
        for cb in &self.cbuffers {
            wstr(&mut out, &cb.name);
            ints(&mut out, &[cb.size, cb.params.len() as i32]);
            for p in &cb.params {
                param_bytes(&mut out, p);
            }
            ints(&mut out, &[cb.structs.len() as i32]);
            for st in &cb.structs {
                wstr(&mut out, &st.name);
                ints(
                    &mut out,
                    &[st.offset, st.array, st.size, st.params.len() as i32],
                );
                for p in &st.params {
                    param_bytes(&mut out, p);
                }
            }
        }
        ints(&mut out, &[self.bindings.len() as i32]);
        for b in &self.bindings {
            wstr(&mut out, &b.name);
            ints(&mut out, &[b.kind, b.index]);
            ints(&mut out, &b.extra);
        }
        out
    }

    pub fn serialize(&self) -> Vec<u8> {
        let mut out = self.head();
        out.extend(self.params_bytes());
        out
    }
}

/// The Bindings of a blob entry (2021.2+: with its parameter entry's data).
pub fn entry_bindings(entry: &Entry, param: Option<&[u8]>) -> R<Bindings> {
    let mut data = entry.rest.clone();
    if let Some(p) = param {
        data.extend(p);
    }
    Bindings::parse(&data, Some(entry.version))
}

/// entry_bindings(entry, param, typed=True): an entry of 6000.6's serializer in its own layout,
/// any other as entry_bindings reads it.
pub fn entry_bindings_typed(entry: &Entry, param: Option<&[u8]>) -> R<Bindings> {
    if entry.version < TYPED_FROM {
        return entry_bindings(entry, param);
    }
    let mut data = entry.rest.clone();
    if let Some(p) = param {
        data.extend(p);
    }
    Bindings::parse_typed(&data)
}

/// The binding info of a GL entry up to Unity 2018 (dxbc.gl_rest).
pub fn gl_rest(source_map: i32) -> Vec<u8> {
    let mut out = Vec::new();
    ints(&mut out, &[source_map, 0, 1, 0, 0, 0, 0, 0]);
    out
}

// ---------------------------------------------------------------- GL binding info (2019.1+)

struct Patterns {
    loose: Regex,
    tex: Regex,
    block: Regex,
    member: Regex,
    mtx: Regex,
    sampler_prefix: Regex,
    storage: Regex,
    strukt: Regex,
}

static RX: LazyLock<Patterns> = LazyLock::new(|| {
    let prec = r"(?:(?:highp|mediump|lowp)\s+)?";
    Patterns {
        loose: glsl::re(&format!(
            r"(?m)^uniform\s+{prec}(\w+)\s+(\w+)(?:\[(\d+)\])?\s*;"
        )),
        tex: glsl::re(&format!(
            r"(?m)^(?:UNITY_LOCATION\((\d+)\)\s*)?uniform\s+{prec}(\w*sampler\w+)\s+(\w+)\s*;"
        )),
        block: glsl::re(
            r"(?ms)^(?:UNITY_BINDING\((\d+)\)\s*)?(?:layout\([^)]*\)\s*)?uniform\s+(\w+)\s*\{(.*?)^\s*(?:#endif\s*\n\s*)?(?:#if HLSLCC_ENABLE_UNIFORM_BUFFERS\s*\n)?\s*\}[^;]*;",
        ),
        member: glsl::re(&format!(
            r"(?m)^\s*(?:UNITY_UNIFORM\s+)?{prec}(\w+)\s+(\w+)(?:\[(\w+)\])?\s*;"
        )),
        mtx: glsl::re(r"\Ahlslcc_mtx(\d)x(\d)(\w+)\n?\z"),
        sampler_prefix: glsl::re(r"\A[iu]?sampler"),
        storage: glsl::re(
            r"(?m)^\s*(?:layout\([^)]*\)\s*)?(?:(?:readonly|writeonly|coherent|restrict)\s+)*buffer\s",
        ),
        strukt: glsl::re(r"(?m)^struct\s"),
    }
});

/// _GL_TYPES: (type, columns) of a GL uniform type.
fn gl_type(t: &str) -> Option<(i32, i32)> {
    Some(match t {
        "float" => (0, 1),
        "int" | "uint" | "bool" => (5, 1),
        _ => {
            let (p, n) = t.split_once("vec")?;
            let n: i32 = match n {
                "2" => 2,
                "3" => 3,
                "4" => 4,
                _ => return None,
            };
            match p {
                "" => (0, n),
                "i" | "u" | "b" => (5, n),
                _ => return None,
            }
        }
    })
}

/// _GL_SAMPLERS: GLSL sampler types -> Unity's TextureDimension.
fn gl_sampler_dim(t: &str) -> Option<i32> {
    Some(match t {
        "2D" | "2DShadow" | "2DMS" => 2,
        "3D" => 3,
        "Cube" | "CubeShadow" => 4,
        "2DArray" | "2DArrayShadow" | "2DMSArray" => 5,
        "CubeArray" => 6,
        _ => return None,
    })
}

const GL_STAGE_ORDER: [&str; 5] = ["FRAGMENT", "HULL", "DOMAIN", "GEOMETRY", "VERTEX"];

/// A Param (offset 0) of a GL uniform declaration.
fn gl_param(gltype: &str, name: &str, count: Option<&str>) -> R<Param> {
    let count_n = |c: Option<&str>| -> i32 { c.and_then(|c| c.parse().ok()).unwrap_or(0) };
    if let Some(m) = RX.mtx.captures(name) {
        let (r, c): (i32, i32) = (m[1].parse().unwrap(), m[2].parse().unwrap());
        let n = count.map_or(1, |c| c.parse().unwrap_or(0)) / c;
        return Ok(Param {
            name: m[3].to_string(),
            typ: 0,
            rows: r,
            cols: c,
            is_matrix: 1,
            array: if n > 1 { n } else { 0 },
            offset: 0,
        });
    }
    let Some((t, cols)) = gl_type(gltype) else {
        return Err(Unsupported(format!("uniform {name} of type {gltype}")));
    };
    Ok(Param {
        name: name.to_string(),
        typ: t,
        rows: 1,
        cols,
        is_matrix: 0,
        array: count_n(count),
        offset: 0,
    })
}

fn gl_size(p: &Param) -> i32 {
    if p.is_matrix != 0 {
        16 * p.rows * p.array.max(1)
    } else {
        p.cols * 4 * p.array.max(1)
    }
}

/// Offsets of a stage's loose uniforms in Unity's GL "$Globals" and its size (dxbc.gl_globals_layout).
pub fn gl_globals_layout(params: &mut [Param], unity: (u64, u64)) -> i32 {
    let mut off = 0;
    for p in params.iter_mut() {
        let sz = gl_size(p);
        if unity < (2020, 1) {
            if p.is_matrix != 0 || p.cols == 4 {
                off = (off + 15) & !15;
            } else if p.array == 0 && (off % 16) + sz > 16 {
                off += (off % 16) + sz - 16;
            }
        } else {
            let al = if p.is_matrix != 0 || p.array != 0 {
                16
            } else {
                match p.cols {
                    1 => 4,
                    2 => 8,
                    _ => 16,
                }
            };
            off = (off + al - 1) & !(al - 1);
        }
        p.offset = off;
        off += sz;
    }
    off
}

fn vectors_then_matrices(params: Vec<Param>) -> Vec<Param> {
    let (v, m): (Vec<Param>, Vec<Param>) = params.into_iter().partition(|p| p.is_matrix == 0);
    v.into_iter().chain(m).collect()
}

/// One stage's uniforms in declaration order (dxbc.gl_declarations).
enum Decl {
    Loose(Param),
    Block(String, i32, Vec<Param>),
    Texture(String, i32, i32, bool),
}

fn gl_declarations(src: &str, arrays: Option<&HashMap<String, i32>>) -> R<Vec<Decl>> {
    let r = &*RX;
    // (HLSLcc declares an array of one element as a plain variable; Unity lists it as an array of 1)
    let param = |gltype: &str, name: &str, count: Option<&str>| -> R<Param> {
        let mut p = gl_param(gltype, name, count)?;
        if count.is_none() && arrays.and_then(|a| a.get(&p.name)) == Some(&1) {
            p.array = 1;
        }
        Ok(p)
    };
    let mut items: Vec<(usize, Decl)> = Vec::new();
    let mut spans = Vec::new();
    for m in r.block.captures_iter(src) {
        let mut mem = Vec::new();
        for mm in r.member.captures_iter(&m[3]) {
            let count = mm.get(3).map(|x| x.as_str());
            if let Some(c) = count {
                if !c.chars().all(|ch| ch.is_ascii_digit()) {
                    return Err(Unsupported(format!(
                        "uniform block {}: array size {c} (instancing data)",
                        &m[2]
                    )));
                }
            }
            mem.push(param(&mm[1], &mm[2], count)?);
        }
        let all = m.get(0).unwrap();
        let binding = m.get(1).map_or(0, |x| x.as_str().parse().unwrap_or(0));
        items.push((all.start(), Decl::Block(m[2].to_string(), binding, mem)));
        spans.push((all.start(), all.end()));
    }
    for m in r.loose.captures_iter(src) {
        let s = m.get(0).unwrap().start();
        if !m[1].contains("sampler") && !spans.iter().any(|&(a, b)| a <= s && s < b) {
            items.push((
                s,
                Decl::Loose(param(&m[1], &m[2], m.get(3).map(|x| x.as_str()))?),
            ));
        }
    }
    for m in r.tex.captures_iter(src) {
        let st = r.sampler_prefix.replace(&m[2], "").into_owned();
        let Some(dim) = gl_sampler_dim(&st) else {
            return Err(Unsupported(format!("texture {} of type {}", &m[3], &m[2])));
        };
        let unit = m.get(1).map_or(0, |x| x.as_str().parse().unwrap_or(0));
        items.push((
            m.get(0).unwrap().start(),
            Decl::Texture(m[3].to_string(), unit, dim, st.contains("MS")),
        ));
    }
    if r.storage.is_match(src) {
        return Err(Unsupported("storage buffer".into()));
    }
    if r.strukt.is_match(src) {
        return Err(Unsupported("struct uniforms (instancing data)".into()));
    }
    items.sort_by_key(|(s, _)| *s);
    Ok(items.into_iter().map(|(_, d)| d).collect())
}

/// {stage: source} of a GL program (the later of two blocks of a stage).
fn gl_stages(code: &str) -> Vec<(String, String)> {
    let mut out: Vec<(String, String)> = Vec::new();
    for (st, a, b) in glsl::stage_blocks(code) {
        let src = code[a..b].to_string();
        match out.iter_mut().find(|(s, _)| *s == st) {
            Some(e) => e.1 = src,
            None => out.push((st, src)),
        }
    }
    out
}

/// The binding info Unity's GL compiler (2019.1+) gives a GL program (dxbc.gl_bindings). unity:
/// (major, minor). arrays: {name: array size} of the uniforms where known.
pub fn gl_bindings(
    code: &str,
    unity: (u64, u64),
    source_map: i32,
    arrays: Option<&HashMap<String, i32>>,
) -> R<Bindings> {
    gl_bindings_as(code, unity, source_map, arrays, false)
}

/// gl_bindings, typed: in 6000.6's layout (the same buffers and bindings, every binding's encoded
/// data 0, the "$Globals" binding at slot -1, as Unity's 6000.6 GL programs have it).
pub fn gl_bindings_as(
    code: &str,
    unity: (u64, u64),
    source_map: i32,
    arrays: Option<&HashMap<String, i32>>,
    typed: bool,
) -> R<Bindings> {
    let stages = gl_stages(code);
    let empty = |name: &str, size: i32, params: Vec<Param>| ConstantBuffer {
        name: name.into(),
        size,
        params,
        structs: Vec::new(),
    };
    let mut cbs = vec![empty("", 0, Vec::new())];
    let mut merged: Option<ConstantBuffer> = None;
    let mut texs: Vec<Binding> = Vec::new();
    let mut blocks: Vec<(ConstantBuffer, Binding)> = Vec::new();
    let mut seen_tex: Vec<String> = Vec::new();
    let mut seen_blocks: Vec<String> = Vec::new();
    for st in GL_STAGE_ORDER {
        let Some((_, src)) = stages.iter().find(|(s, _)| s == st) else {
            continue;
        };
        let items = gl_declarations(src, arrays)?;
        let mut loose: Vec<Param> = items
            .iter()
            .filter_map(|d| match d {
                Decl::Loose(p) => Some(p.clone()),
                _ => None,
            })
            .collect();
        let mut done_loose = false;
        for it in items {
            match it {
                Decl::Loose(_) => {
                    if done_loose {
                        continue;
                    }
                    done_loose = true;
                    let size = gl_globals_layout(&mut loose, unity);
                    let params = loose.clone();
                    if unity >= (2022, 1) {
                        match &mut merged {
                            None => {
                                let mut p = params;
                                p.sort_by_key(|p| p.offset);
                                merged = Some(empty("$Globals", size, p));
                            }
                            Some(m) => {
                                let taken: Vec<i32> = m.params.iter().map(|p| p.offset).collect();
                                let mut all = m.params.clone();
                                all.extend(
                                    params.into_iter().filter(|p| !taken.contains(&p.offset)),
                                );
                                all.sort_by_key(|p| p.offset);
                                m.params = all;
                            }
                        }
                    } else {
                        cbs.push(empty("$Globals", size, vectors_then_matrices(params)));
                    }
                }
                Decl::Block(name, binding, mut mem) => {
                    if seen_blocks.contains(&name) {
                        continue;
                    }
                    seen_blocks.push(name.clone());
                    let size = gl_globals_layout(&mut mem, unity.max((2020, 1)));
                    let params: Vec<Param> = mem
                        .into_iter()
                        .filter(|p| !p.name.starts_with("Xhlslcc_UnusedX"))
                        .collect();
                    blocks.push((
                        empty(&name, size, vectors_then_matrices(params)),
                        Binding {
                            name: name.clone(),
                            kind: KIND_CBUFFER,
                            index: binding,
                            extra: if typed { vec![] } else { vec![0] },
                            packed: true,
                            enc: typed.then(|| vec![0]),
                        },
                    ));
                    if unity < (2022, 1) {
                        cbs.push(blocks.last().unwrap().0.clone());
                    }
                }
                Decl::Texture(name, unit, dim, ms) => {
                    let name = name
                        .strip_prefix("hlslcc_zcmp")
                        .map(String::from)
                        .unwrap_or(name);
                    if !seen_tex.contains(&name) {
                        seen_tex.push(name.clone());
                        texs.push(Binding {
                            name,
                            kind: KIND_TEXTURE,
                            index: unit,
                            extra: vec![unit, dim << 1 | ms as i32],
                            packed: true,
                            enc: typed.then(|| vec![0, 0]),
                        });
                    }
                }
            }
        }
    }
    let mut binds = texs;
    if unity >= (2022, 1) {
        if let Some(mut m) = merged {
            m.params = vectors_then_matrices(m.params);
            cbs.push(m);
            binds.push(if typed {
                Binding {
                    name: "$Globals".into(),
                    kind: KIND_CBUFFER,
                    index: -1,
                    extra: vec![],
                    packed: true,
                    enc: Some(vec![0]),
                }
            } else {
                Binding {
                    name: "$Globals".into(),
                    kind: KIND_CBUFFER,
                    index: 0,
                    extra: vec![0],
                    packed: true,
                    enc: None,
                }
            });
        }
        for (cb, b) in blocks.into_iter().rev() {
            cbs.push(cb);
            binds.push(b);
        }
    } else {
        binds.extend(blocks.into_iter().map(|(_, b)| b));
    }
    Ok(Bindings {
        source_map,
        channels: Vec::new(),
        cbuffers: cbs,
        bindings: binds,
        typed,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn le(v: &[i32]) -> Vec<u8> {
        v.iter().flat_map(|x| x.to_le_bytes()).collect()
    }

    fn s(t: &str) -> Vec<u8> {
        let mut out = Vec::new();
        wstr(&mut out, t);
        out
    }

    /// 6000.6's layout written by hand (dxbc.py's docstring): read, written back the same, and
    /// its refusals (buffers, UAVs, encoded data, a damaged list).
    #[test]
    fn typed_layout() {
        let mut d = le(&[25, 2, 0, 13, 4, 14, 2]);
        d.extend(s(""));
        d.extend(le(&[0, 0, 0, 0]));
        d.extend(s("VGlobals1234"));
        d.extend(le(&[80, 1]));
        d.extend(s("_MainTex_ST"));
        d.extend(le(&[4, 0, 64])); // a float4 (dimension 4, type 0)
        d.extend(le(&[1]));
        d.extend(s("unity_ObjectToWorld"));
        d.extend(le(&[4, 0, 0])); // a 4-row matrix
        d.extend(le(&[0])); // no structs
        d.extend(le(&[1])); // textures
        d.extend(s("_MainTex"));
        d.extend(le(&[0, 0x08000001, 0, 0x08000000, 4]));
        d.extend(le(&[0])); // UAVs
        d.extend(le(&[1])); // constant buffer bindings
        d.extend(s("VGlobals1234"));
        d.extend(le(&[0, 0x04010001]));
        d.extend(le(&[0])); // buffers
        d.extend(le(&[1, 0, 3, 0x45])); // an inline sampler
        let b = Bindings::parse_typed(&d).unwrap();
        assert!(b.typed);
        assert_eq!(b.try_serialize().unwrap(), d);
        assert_eq!(b.serialize(), d);
        let k: Vec<(&str, i32, i32, Vec<i32>)> = b
            .bindings
            .iter()
            .map(|x| (x.name.as_str(), x.kind, x.index, x.extra.clone()))
            .collect();
        assert_eq!(
            k,
            vec![
                ("_MainTex", 0, 0x08000001, vec![0x08000000, 4]),
                ("VGlobals1234", 1, 0x04010001, vec![]),
                ("", 4, 3, vec![0x45]),
            ]
        );
        assert_eq!(b.bindings[0].sampler(), Some(0x08000000));
        assert_eq!(b.bindings[0].dim(), Some(2));
        let p = &b.cbuffers[1].params;
        assert_eq!(
            (p[0].cols, p[0].is_matrix, p[1].rows, p[1].cols),
            (4, 0, 4, 4)
        );
        // the layout before 6000.6 does not read it
        assert!(Bindings::parse(&d, Some(TYPED_FROM)).is_err());
        // encoded data, a UAV, a cut list
        let mut e = d.clone();
        let at = d.len() - 28; // (the constant buffer binding's encoded data)
        e[at..at + 4].copy_from_slice(&7i32.to_le_bytes());
        assert!(Bindings::parse_typed(&e)
            .unwrap_err()
            .0
            .contains("with encoded data (7,)"));
        assert!(Bindings::parse_typed(&d[..d.len() - 2])
            .unwrap_err()
            .0
            .starts_with("binding info does not match the 6000.6 layout"));
        let mut u = b.clone();
        u.bindings.push(Binding {
            name: "_Out".into(),
            kind: KIND_UAV,
            index: 1,
            extra: vec![1],
            packed: true,
            enc: Some(vec![0, 0]),
        });
        let ub = u.try_serialize().unwrap();
        assert!(Bindings::parse_typed(&ub)
            .unwrap_err()
            .0
            .contains("of kind 3 (UAV): not supported"));
        // a member the layout cannot hold
        let mut m = b.clone();
        m.cbuffers[1].params[0].rows = 2;
        assert!(m
            .try_params_bytes()
            .unwrap_err()
            .0
            .contains("vector _MainTex_ST of 2 rows"));
    }

    #[test]
    fn gl_bindings_typed() {
        let code = "#ifdef VERTEX\n#version 300 es\nuniform \tvec4 hlslcc_mtx4x4unity_ObjectToWorld[4];\nuniform \tvec4 _MainTex_ST;\nvoid main()\n{\n}\n#endif\n#ifdef FRAGMENT\n#version 300 es\nUNITY_LOCATION(0) uniform mediump sampler2D _MainTex;\nvoid main()\n{\n}\n#endif\n";
        let b = gl_bindings_as(code, (6000, 6), 0, None, true).unwrap();
        assert!(b.typed);
        let k: Vec<(&str, i32, i32)> = b
            .bindings
            .iter()
            .map(|x| (x.name.as_str(), x.kind, x.index))
            .collect();
        assert_eq!(k, vec![("_MainTex", 0, 0), ("$Globals", 1, -1)]);
        let data = b.try_serialize().unwrap();
        assert_eq!(Bindings::parse_typed(&data).unwrap(), b);
        let untyped = gl_bindings(code, (6000, 6), 0, None).unwrap();
        assert_eq!(untyped.bindings[1].index, 0);
        assert!(!untyped.typed);
    }

    #[test]
    fn round_trip_and_layout() {
        let code = "#ifdef VERTEX\n#version 300 es\nuniform \tvec4 hlslcc_mtx4x4unity_ObjectToWorld[4];\nuniform \tvec4 _MainTex_ST;\nin highp vec4 in_POSITION0;\nvoid main()\n{\n}\n#endif\n#ifdef FRAGMENT\n#version 300 es\nuniform \tfloat _Intensity;\nuniform \tvec3 _Tint;\nUNITY_LOCATION(0) uniform mediump sampler2D _MainTex;\nlayout(location = 0) out vec4 SV_Target0;\nvoid main()\n{\n}\n#endif\n";
        let b = gl_bindings(code, (2021, 3), 0, None).unwrap();
        let names: Vec<&str> = b.cbuffers.iter().map(|c| c.name.as_str()).collect();
        assert_eq!(names, vec!["", "$Globals", "$Globals"]);
        assert_eq!(b.cbuffers[1].size, 28); // (float at 0, vec3 at 16: 2020.1+ alignments)
        assert_eq!(b.cbuffers[1].params[1].offset, 16);
        assert_eq!(b.cbuffers[2].params[0].name, "_MainTex_ST");
        assert_eq!(b.cbuffers[2].params[1].name, "unity_ObjectToWorld");
        assert_eq!(b.bindings[0].extra, vec![0, 4]);
        let data = b.serialize();
        assert_eq!(Bindings::parse(&data, Some(201806140)).unwrap(), b);
        let b22 = gl_bindings(code, (2022, 3), 0, None).unwrap();
        assert_eq!(b22.cbuffers.len(), 2);
        assert_eq!(b22.bindings.last().unwrap().name, "$Globals");
        assert!(Bindings::parse(&data[..data.len() - 1], Some(201806140)).is_err());
    }
}
