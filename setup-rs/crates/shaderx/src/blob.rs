//! Unity's compiled shader programs, the per-platform program blob of a Shader asset, as
//! shaders/blob.py reads and writes it (its docstring has the format): `compressedBlob` holds an
//! LZ4 block per shader compiler platform (2019.3+: per segment of a platform), each decompressed
//! blob an entry table and the entries, each entry one serialized subprogram (version, GPU program
//! type, statistics, keyword lists, code, binding info) or, from 2021.2, a parameter entry.
use std::collections::{BTreeMap, BTreeSet};

use crate::lz4;
use crate::py;

// shader compiler platforms (ShaderCompilerPlatform)
pub const PLATFORM_D3D11: i64 = 4;
pub const PLATFORM_GLES20: i64 = 5;
pub const PLATFORM_GLES3PLUS: i64 = 9;
pub const PLATFORM_METAL: i64 = 14;
pub const PLATFORM_GLCORE: i64 = 15;
pub const PLATFORM_VULKAN: i64 = 18;
// GPU program types (ShaderGpuProgramType, the same numbers in Unity 5.5 - 2022.3)
pub const GPU_GLES31AEP: i64 = 2;
pub const GPU_GLES31: i64 = 3;
pub const GPU_GLES3: i64 = 4;
pub const GPU_GLES: i64 = 5;
pub const GPU_GLCORE32: i64 = 6;
pub const GPU_GLCORE41: i64 = 7;
pub const GPU_GLCORE43: i64 = 8;
pub const GPU_METAL_VS: i64 = 23;
pub const GPU_METAL_FS: i64 = 24;
pub const GPU_SPIRV: i64 = 25;
/// 0xf00dcafe: Unity's header before a Metal program's MSL.
const METAL_MAGIC: &[u8] = b"\xfe\xca\x0d\xf0";

/// What a serializer version writes: (statistics ints, keyword lists). blob.SERIALIZERS.
fn layout(version: i32) -> Option<(usize, usize)> {
    match version {
        201609010 | 201708220 | 201802150 | 202012090 | 202605290 => Some((4, 1)),
        201806140 => Some((4, 2)),
        _ => None,
    }
}

/// The layouts tried for an unknown serializer version, most likely first.
const CANDIDATES: [(usize, usize); 8] = [
    (4, 1),
    (4, 2),
    (3, 1),
    (3, 2),
    (5, 1),
    (5, 2),
    (2, 1),
    (2, 2),
];

/// A blob that cannot be read: what makes the Python raise (and the step stop).
#[derive(Clone, Debug, PartialEq)]
pub struct BlobError(pub String);

impl std::fmt::Display for BlobError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for BlobError {}

fn err<T>(msg: impl Into<String>) -> Result<T, BlobError> {
    Err(BlobError(msg.into()))
}

fn pad4(n: usize) -> usize {
    (n + 3) & !3
}

/// `struct.unpack_from('<i', data, p)` (None: struct.error, out of range).
fn i32_at(data: &[u8], p: usize) -> Option<i32> {
    data.get(p..p.checked_add(4)?)
        .map(|b| i32::from_le_bytes(b.try_into().unwrap()))
}

/// D3D11 program code: Unity's header, then a DXBC container at byte 6 or later of the first 64.
fn is_d3d11(code: &[u8]) -> bool {
    let w = &code[..code.len().min(64)];
    match w.windows(4).position(|x| x == b"DXBC") {
        Some(i) => i >= 6 && code.len() >= i + 32,
        None => false,
    }
}

/// Vulkan program code: int32 flags, a table of (offset, size) per stage, SMOL-V / SPIR-V modules.
fn is_vulkan(code: &[u8]) -> bool {
    if code.len() < 20 {
        return false;
    }
    let (mut first, mut p, mut found) = (code.len(), 4usize, false);
    while p + 8 <= first && p < 4 + 8 * 9 {
        let off = i32_at(code, p).unwrap() as i64;
        let size = i32_at(code, p + 4).unwrap() as i64;
        if size != 0 {
            let magic = py::slice(code, off, off + 4);
            if size < 0
                || off < p as i64 + 8
                || off + size > code.len() as i64
                || (magic != b"LOMS" && magic != b"\x03\x02\x23\x07")
            {
                return false;
            }
            first = first.min(off as usize);
            found = true;
        }
        p += 8;
    }
    found
}

/// A string list (int32 count, each: int32 length, ASCII bytes, padding to 4); None where the
/// Python's `_strings` gives None or raises.
fn strings(data: &[u8], mut p: usize) -> Option<(Vec<String>, usize)> {
    let n = i32_at(data, p)?;
    p += 4;
    if !(0..4096).contains(&n) {
        return None;
    }
    let mut out = Vec::with_capacity(n as usize);
    for _ in 0..n {
        let k = i32_at(data, p)?;
        if !(1..1024).contains(&k) || p + 4 + k as usize > data.len() {
            return None;
        }
        let b = &data[p + 4..p + 4 + k as usize];
        if !b.is_ascii() {
            return None;
        }
        out.push(String::from_utf8(b.to_vec()).unwrap());
        p = pad4(p + 4 + k as usize);
    }
    Some((out, p))
}

fn write_strings(out: &mut Vec<u8>, strs: &[String]) {
    out.extend_from_slice(&(strs.len() as i32).to_le_bytes());
    for k in strs {
        let b = k.as_bytes();
        out.extend_from_slice(&(b.len() as i32).to_le_bytes());
        out.extend_from_slice(b);
        out.resize(out.len() + pad4(b.len()) - b.len(), 0);
    }
}

/// One subprogram of a platform blob (local_keywords: None for layouts with one keyword list).
#[derive(Clone, Debug, PartialEq)]
pub struct Entry {
    pub version: i32,
    pub gpu_type: i32,
    pub stats: Vec<i32>,
    pub keywords: Vec<String>,
    pub local_keywords: Option<Vec<String>>,
    pub code: Vec<u8>,
    pub rest: Vec<u8>,
    /// Read with a layout tried for an unknown serializer version.
    pub guessed: bool,
}

/// Why an entry did not parse: `NotProgram` is the Python's ValueError (the entry may be kept
/// as raw bytes); `Broken` is what it does not catch (struct.error: too short to read).
#[derive(Debug)]
pub enum EntryError {
    NotProgram(String),
    Broken(String),
}

impl Entry {
    pub fn parse(data: &[u8]) -> Result<Entry, EntryError> {
        let (Some(version), Some(gpu_type)) = (i32_at(data, 0), i32_at(data, 4)) else {
            return Err(EntryError::Broken(
                "shader program entry: unpack requires a buffer of 8 bytes".into(),
            ));
        };
        if let Some((n, k)) = layout(version) {
            return Self::try_layout(data, version, gpu_type, n, k).ok_or_else(|| {
                EntryError::NotProgram(format!(
                    "shader program entry (serializer {version}, gpu type {gpu_type}) does not match its layout"
                ))
            });
        }
        for (n, k) in CANDIDATES {
            if let Some(mut e) = Self::try_layout(data, version, gpu_type, n, k) {
                e.guessed = true;
                return Ok(e);
            }
        }
        Err(EntryError::NotProgram(format!(
            "shader program entry (serializer {version}, gpu type {gpu_type}): unknown layout"
        )))
    }

    fn try_layout(
        data: &[u8],
        version: i32,
        gpu_type: i32,
        nstats: usize,
        nlists: usize,
    ) -> Option<Entry> {
        let mut p = 8;
        let mut stats = Vec::with_capacity(nstats);
        for _ in 0..nstats {
            stats.push(i32_at(data, p)?);
            p += 4;
        }
        let (keywords, q) = strings(data, p)?;
        p = q;
        let mut local = None;
        if nlists == 2 {
            let (l, q) = strings(data, p)?;
            local = Some(l);
            p = q;
        }
        let n = i32_at(data, p)? as i64;
        if !(0 <= n && n <= data.len() as i64 - p as i64 - 4) {
            return None;
        }
        let n = n as usize;
        let code = data[p + 4..p + 4 + n].to_vec();
        let rest = data.get(pad4(p + 4 + n)..).unwrap_or(&[]).to_vec();
        let gt = gpu_type as i64;
        if is_d3d11(&code)
            || gt == GPU_SPIRV && is_vulkan(&code)
            || (gt == GPU_METAL_VS || gt == GPU_METAL_FS) && code.starts_with(METAL_MAGIC)
        {
            // D3D11 (Unity's header + DXBC), Vulkan (stage table + SMOL-V), Metal (header + MSL)
        } else if !code.is_empty() && !matches!(py::bytes_lstrip(&code).first(), Some(b'#' | b'/'))
            || std::str::from_utf8(&code).is_err()
        {
            return None; // GL: GLSL text (or no code)
        }
        let has = |pat: &[u8]| rest.windows(pat.len()).any(|w| w == pat);
        if has(b"#version") || has(b"DXBC") {
            return None; // the code is further on: a list was taken for it
        }
        let e = Entry {
            version,
            gpu_type,
            stats,
            keywords,
            local_keywords: local,
            code,
            rest,
            guessed: false,
        };
        (e.serialize() == data).then_some(e)
    }

    pub fn serialize(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(self.code.len() + self.rest.len() + 64);
        out.extend_from_slice(&self.version.to_le_bytes());
        out.extend_from_slice(&self.gpu_type.to_le_bytes());
        for s in &self.stats {
            out.extend_from_slice(&s.to_le_bytes());
        }
        write_strings(&mut out, &self.keywords);
        if let Some(l) = &self.local_keywords {
            write_strings(&mut out, l);
        }
        out.extend_from_slice(&(self.code.len() as i32).to_le_bytes());
        out.extend_from_slice(&self.code);
        out.resize(out.len() + pad4(self.code.len()) - self.code.len(), 0);
        out.extend_from_slice(&self.rest);
        out
    }
}

/// A platform blob's entry: a program, 2021.2+ parameters (kept as bytes after the version), an
/// entry kept as its bytes (no subprogram names it as a program and it does not parse as one).
#[derive(Clone, Debug, PartialEq)]
pub enum Slot {
    Program(Entry),
    Params { version: i32, data: Vec<u8> },
    Raw(Vec<u8>),
}

impl Slot {
    pub fn serialize(&self) -> Vec<u8> {
        match self {
            Slot::Program(e) => e.serialize(),
            Slot::Params { version, data } => [&version.to_le_bytes()[..], data].concat(),
            Slot::Raw(d) => d.clone(),
        }
    }
}

/// [(offset, length, segment)] of a platform blob's entry table.
fn table(seg0: &[u8], nested: bool) -> Result<Vec<(i32, i32, i32)>, BlobError> {
    let broken = || BlobError("shader blob: entry table past the end of the blob".into());
    let count = i32_at(seg0, 0).ok_or_else(broken)?;
    let mut out = Vec::new();
    for i in 0..count.max(0) as usize {
        if nested {
            let p = 4 + 12 * i;
            out.push((
                i32_at(seg0, p).ok_or_else(broken)?,
                i32_at(seg0, p + 4).ok_or_else(broken)?,
                i32_at(seg0, p + 8).ok_or_else(broken)?,
            ));
        } else {
            let p = 4 + 8 * i;
            out.push((
                i32_at(seg0, p).ok_or_else(broken)?,
                i32_at(seg0, p + 4).ok_or_else(broken)?,
                0,
            ));
        }
    }
    Ok(out)
}

/// A decompressed platform blob: one block (up to 2019.2) or its segments (2019.3+).
#[derive(Clone, Debug, PartialEq)]
pub enum PlatformBlob {
    One(Vec<u8>),
    Segments(Vec<Vec<u8>>),
}

impl PlatformBlob {
    fn segs(&self) -> Vec<&[u8]> {
        match self {
            PlatformBlob::One(b) => vec![b.as_slice()],
            PlatformBlob::Segments(s) => s.iter().map(|x| x.as_slice()).collect(),
        }
    }
}

/// A decompressed platform blob -> its entries (None: an empty slot). `params`: the indices of
/// parameter entries; `programs`: the indices subprograms name as programs (any other entry that
/// does not parse as one is kept as Raw; None: every entry not in `params` must parse).
pub fn parse_blob(
    data: &PlatformBlob,
    params: &BTreeSet<i64>,
    nested: bool,
    programs: Option<&BTreeSet<i64>>,
) -> Result<Vec<Option<Slot>>, BlobError> {
    let segs = data.segs();
    let mut out = Vec::new();
    for (i, (off, length, seg)) in table(segs[0], nested)?.into_iter().enumerate() {
        if length == 0 {
            out.push(None);
            continue;
        }
        let Some(s) = usize::try_from(seg).ok().and_then(|s| segs.get(s)) else {
            return err(format!(
                "shader blob: entry {i} in segment {seg}, of {}",
                segs.len()
            ));
        };
        let b = py::slice(s, off as i64, off as i64 + length as i64);
        if params.contains(&(i as i64)) {
            let Some(version) = i32_at(b, 0) else {
                return err(format!(
                    "shader blob: parameter entry {i} shorter than 4 bytes"
                ));
            };
            out.push(Some(Slot::Params {
                version,
                data: b[4..].to_vec(),
            }));
            continue;
        }
        match Entry::parse(b) {
            Ok(e) => out.push(Some(Slot::Program(e))),
            Err(EntryError::Broken(m)) => return err(m),
            Err(EntryError::NotProgram(m)) => {
                if programs.is_none_or(|p| p.contains(&(i as i64))) {
                    return err(m);
                }
                out.push(Some(Slot::Raw(b.to_vec())));
            }
        }
    }
    Ok(out)
}

/// The segment of every entry of a segmented blob.
pub fn blob_segments(segs: &[Vec<u8>]) -> Result<Vec<i64>, BlobError> {
    Ok(table(&segs[0], true)?
        .into_iter()
        .map(|(_, _, s)| s as i64)
        .collect())
}

/// Entries -> a decompressed platform blob of one block (entries 4-aligned, in order).
pub fn build_blob(entries: &[Option<Slot>]) -> Vec<u8> {
    let head = 4 + 8 * entries.len();
    let mut body = Vec::new();
    let mut tab = Vec::with_capacity(entries.len());
    for e in entries {
        let b = e.as_ref().map(|e| e.serialize()).unwrap_or_default();
        tab.push(((head + body.len()) as i32, b.len() as i32));
        body.extend_from_slice(&b);
        body.resize(pad4(body.len()), 0);
    }
    let mut out = Vec::with_capacity(head + body.len());
    out.extend_from_slice(&(entries.len() as i32).to_le_bytes());
    for (off, n) in tab {
        out.extend_from_slice(&off.to_le_bytes());
        out.extend_from_slice(&n.to_le_bytes());
    }
    out.extend_from_slice(&body);
    out
}

/// Entries and the segment of each -> the decompressed segments of a 2019.3+ blob: segment 0
/// starts with the 12-byte entry table, each segment holds its entries in order.
pub fn build_segments(entries: &[Option<Slot>], segments: &[i64]) -> Vec<Vec<u8>> {
    let nseg = segments.iter().copied().max().unwrap_or(0) as usize + 1;
    let head = 4 + 12 * entries.len();
    let mut bodies = vec![Vec::new(); nseg];
    let mut tab = Vec::with_capacity(entries.len());
    for (e, &s) in entries.iter().zip(segments) {
        let s = s as usize;
        let b = e.as_ref().map(|e| e.serialize()).unwrap_or_default();
        tab.push((
            ((if s == 0 { head } else { 0 }) + bodies[s].len()) as i32,
            b.len() as i32,
            s as i32,
        ));
        bodies[s].extend_from_slice(&b);
        let n = pad4(bodies[s].len());
        bodies[s].resize(n, 0);
    }
    let mut seg0 = Vec::with_capacity(head + bodies[0].len());
    seg0.extend_from_slice(&(entries.len() as i32).to_le_bytes());
    for (a, b, c) in tab {
        seg0.extend_from_slice(&a.to_le_bytes());
        seg0.extend_from_slice(&b.to_le_bytes());
        seg0.extend_from_slice(&c.to_le_bytes());
    }
    seg0.extend_from_slice(&bodies[0]);
    let mut out = vec![seg0];
    out.extend(bodies.into_iter().skip(1));
    out
}

/// `offsets`, `compressedLengths`, `decompressedLengths`: a number per platform (up to 2019.2) or
/// a list per platform, one number per segment (2019.3+).
#[derive(Clone, Debug, PartialEq)]
pub enum Lengths {
    Flat(Vec<i64>),
    Nested(Vec<Vec<i64>>),
}

impl Lengths {
    fn len(&self) -> usize {
        match self {
            Lengths::Flat(v) => v.len(),
            Lengths::Nested(v) => v.len(),
        }
    }
}

/// A Shader's blob fields, as in its tree.
#[derive(Clone, Debug, PartialEq)]
pub struct BlobFields {
    pub platforms: Vec<i64>,
    pub offsets: Lengths,
    pub compressed_lengths: Lengths,
    pub decompressed_lengths: Lengths,
    pub compressed_blob: Vec<u8>,
    /// `stageCounts` (2021.2+), None where the tree has none.
    pub stage_counts: Option<Vec<i64>>,
}

/// The per-platform blobs of one Shader (blob.ShaderBlobs): decompressed on reading, compressed
/// again (every platform, ascending) by [`ShaderBlobs::store`].
#[derive(Clone, Debug)]
pub struct ShaderBlobs {
    pub nested: bool,
    pub platforms: BTreeMap<i64, PlatformBlob>,
    /// 2021.2+: per platform, the number of stages its subprograms are listed under.
    pub stage_counts: Option<BTreeMap<i64, i64>>,
}

impl ShaderBlobs {
    pub fn read(f: &BlobFields) -> Result<ShaderBlobs, BlobError> {
        let raw = &f.compressed_blob;
        let nested = matches!(&f.offsets, Lengths::Nested(v) if !v.is_empty());
        let mut platforms = BTreeMap::new();
        let n = f.platforms.len();
        if f.offsets.len() < n || f.compressed_lengths.len() < n || f.decompressed_lengths.len() < n
        {
            return err("shader blob: fewer offsets or lengths than platforms");
        }
        let block = |o: i64, c: i64, d: i64| -> Result<Vec<u8>, BlobError> {
            let size = usize::try_from(d)
                .map_err(|_| BlobError(format!("shader blob: decompressed length {d}")))?;
            lz4::decompress(py::slice(raw, o, o + c), size).map_err(BlobError)
        };
        for (i, &plat) in f.platforms.iter().enumerate() {
            let data = match (&f.offsets, &f.compressed_lengths, &f.decompressed_lengths) {
                (Lengths::Flat(o), Lengths::Flat(c), Lengths::Flat(d)) => {
                    PlatformBlob::One(block(o[i], c[i], d[i])?)
                }
                (Lengths::Nested(o), Lengths::Nested(c), Lengths::Nested(d)) => {
                    PlatformBlob::Segments(
                        o[i].iter()
                            .zip(&c[i])
                            .zip(&d[i])
                            .map(|((&a, &b), &n)| block(a, b, n))
                            .collect::<Result<_, _>>()?,
                    )
                }
                _ => return err("shader blob: offsets and lengths of different shapes"),
            };
            platforms.insert(plat, data);
        }
        let stage_counts = f.stage_counts.as_ref().map(|sc| {
            f.platforms
                .iter()
                .copied()
                .zip(sc.iter().copied())
                .collect()
        });
        Ok(ShaderBlobs {
            nested,
            platforms,
            stage_counts,
        })
    }

    /// A platform's entries (see [`parse_blob`]).
    pub fn entries(
        &self,
        platform: i64,
        params: &BTreeSet<i64>,
        programs: Option<&BTreeSet<i64>>,
    ) -> Result<Vec<Option<Slot>>, BlobError> {
        match self.platforms.get(&platform) {
            Some(d) => parse_blob(d, params, self.nested, programs),
            None => err(format!("shader blob: no platform {platform}")),
        }
    }

    /// The segment of each entry of a platform (all 0 up to 2019.2).
    pub fn segments(&self, platform: i64) -> Result<Vec<i64>, BlobError> {
        match self.platforms.get(&platform) {
            Some(PlatformBlob::Segments(s)) if self.nested => blob_segments(s),
            Some(PlatformBlob::One(b)) => Ok(vec![0; table(b, false)?.len()]),
            Some(PlatformBlob::Segments(s)) => Ok(vec![0; table(&s[0], false)?.len()]),
            None => err(format!("shader blob: no platform {platform}")),
        }
    }

    pub fn set_entries(
        &mut self,
        platform: i64,
        entries: &[Option<Slot>],
        segments: &[i64],
        stage_count: Option<i64>,
    ) {
        let data = if self.nested {
            PlatformBlob::Segments(build_segments(entries, segments))
        } else {
            PlatformBlob::One(build_blob(entries))
        };
        self.platforms.insert(platform, data);
        if let Some(sc) = &mut self.stage_counts {
            if stage_count.is_some() || !sc.contains_key(&platform) {
                sc.insert(platform, stage_count.unwrap_or(1));
            }
        }
    }

    pub fn remove(&mut self, platform: i64) {
        self.platforms.remove(&platform);
        if let Some(sc) = &mut self.stage_counts {
            sc.remove(&platform);
        }
    }

    /// The blob fields: every platform compressed again, in ascending order as Unity writes them.
    pub fn store(&self) -> Result<BlobFields, BlobError> {
        let mut blob = Vec::new();
        let mut plats = Vec::new();
        let (mut fo, mut fc, mut fd) = (Vec::new(), Vec::new(), Vec::new());
        let (mut no, mut nc, mut nd) = (Vec::new(), Vec::new(), Vec::new());
        for (&plat, data) in &self.platforms {
            plats.push(plat);
            if self.nested {
                let (mut o, mut c, mut d) = (Vec::new(), Vec::new(), Vec::new());
                for seg in data.segs() {
                    let z = lz4::compress_hc(seg);
                    o.push(blob.len() as i64);
                    c.push(z.len() as i64);
                    d.push(seg.len() as i64);
                    blob.extend_from_slice(&z);
                }
                no.push(o);
                nc.push(c);
                nd.push(d);
            } else {
                let seg = data.segs()[0];
                let z = lz4::compress_hc(seg);
                fo.push(blob.len() as i64);
                fc.push(z.len() as i64);
                fd.push(seg.len() as i64);
                blob.extend_from_slice(&z);
            }
        }
        let stage_counts = match &self.stage_counts {
            Some(sc) => Some(
                plats
                    .iter()
                    .map(|p| {
                        sc.get(p).copied().ok_or_else(|| {
                            BlobError(format!("shader blob: no stage count for platform {p}"))
                        })
                    })
                    .collect::<Result<Vec<_>, _>>()?,
            ),
            None => None,
        };
        let (offsets, compressed_lengths, decompressed_lengths) = if self.nested {
            (
                Lengths::Nested(no),
                Lengths::Nested(nc),
                Lengths::Nested(nd),
            )
        } else {
            (Lengths::Flat(fo), Lengths::Flat(fc), Lengths::Flat(fd))
        };
        Ok(BlobFields {
            platforms: plats,
            offsets,
            compressed_lengths,
            decompressed_lengths,
            compressed_blob: blob,
            stage_counts,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn gl_entry(code: &str, keywords: &[&str]) -> Entry {
        Entry {
            version: 201708220,
            gpu_type: GPU_GLCORE32 as i32,
            stats: vec![1, 2, 3, 4],
            keywords: keywords.iter().map(|k| k.to_string()).collect(),
            local_keywords: None,
            code: code.as_bytes().to_vec(),
            rest: vec![9, 0, 0, 0, 1, 0, 0, 0],
            guessed: false,
        }
    }

    #[test]
    fn entry_round_trip() {
        let e = gl_entry("#ifdef VERTEX\n#version 150\n#endif\n", &["FOG_ON", "X"]);
        let b = e.serialize();
        assert_eq!(b.len() % 4, 0);
        assert_eq!(Entry::parse(&b).unwrap(), e);
        // an unknown serializer: the layouts are tried, the first that writes back taken
        let mut g = e.clone();
        g.version = 999;
        let p = Entry::parse(&g.serialize()).unwrap();
        assert!(p.guessed && p.stats == g.stats);
    }

    #[test]
    fn entry_not_a_program() {
        let mut e = gl_entry("void main(){}", &[]);
        assert!(matches!(
            Entry::parse(&e.serialize()),
            Err(EntryError::NotProgram(_))
        ));
        e.code = b"#version 150\n".to_vec();
        e.rest = b"#version 150".to_vec(); // code in the rest: a list was taken for it
        assert!(matches!(
            Entry::parse(&e.serialize()),
            Err(EntryError::NotProgram(_))
        ));
        assert!(matches!(
            Entry::parse(&[1, 2, 3]),
            Err(EntryError::Broken(_))
        ));
    }

    #[test]
    fn blob_round_trip() {
        let entries = vec![
            Some(Slot::Program(gl_entry("#ifdef VERTEX\n#endif\n", &[]))),
            None,
            Some(Slot::Program(gl_entry("", &["A"]))),
        ];
        let b = build_blob(&entries);
        let none = BTreeSet::new();
        assert_eq!(
            parse_blob(&PlatformBlob::One(b.clone()), &none, false, None).unwrap(),
            entries
        );
        let segs = build_segments(&entries, &[0, 0, 1]);
        assert_eq!(segs.len(), 2);
        let back = parse_blob(&PlatformBlob::Segments(segs.clone()), &none, true, None).unwrap();
        assert_eq!(back, entries);
        assert_eq!(blob_segments(&segs).unwrap(), vec![0, 0, 1]);
        // a parameter entry, and an entry no subprogram names that does not parse
        let raw = vec![1, 2, 3, 4, 5, 6, 7, 8, 9];
        let mixed = vec![
            Some(Slot::Params {
                version: 7,
                data: vec![1, 2],
            }),
            Some(Slot::Raw(raw)),
        ];
        let b = build_blob(&mixed);
        let params = BTreeSet::from([0]);
        let progs = BTreeSet::new();
        assert_eq!(
            parse_blob(&PlatformBlob::One(b.clone()), &params, false, Some(&progs)).unwrap(),
            mixed
        );
        assert!(parse_blob(&PlatformBlob::One(b), &params, false, None).is_err());
    }

    #[test]
    fn shader_blobs_store_and_read() {
        let entries = vec![Some(Slot::Program(gl_entry(
            "#ifdef VERTEX\n#version 150\n#endif\n",
            &[],
        )))];
        let one = build_blob(&entries);
        let mut sb = ShaderBlobs {
            nested: false,
            platforms: BTreeMap::from([(15, PlatformBlob::One(one.clone()))]),
            stage_counts: None,
        };
        sb.set_entries(9, &entries, &[0], None);
        let f = sb.store().unwrap();
        assert_eq!(f.platforms, vec![9, 15]);
        let back = ShaderBlobs::read(&f).unwrap();
        assert_eq!(back.platforms[&9], PlatformBlob::One(one.clone()));
        assert_eq!(back.platforms[&15], PlatformBlob::One(one));
    }
}
