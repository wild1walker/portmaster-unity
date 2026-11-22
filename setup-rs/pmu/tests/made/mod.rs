//! Made-up game files for pmu's tests (no game or Unity data): executables' headers (ELF with an
//! interpreter, DT_NEEDED names and symbol versions; PE), assemblies of metadata only, serialized
//! files without objects.
#![allow(dead_code)]
use std::fs;
use std::path::Path;

pub fn put(p: &Path, data: impl AsRef<[u8]>) {
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    fs::write(p, data).unwrap();
}

pub fn pad4(v: &mut Vec<u8>) {
    while v.len() % 4 != 0 {
        v.push(0);
    }
}

/// A 64-bit little-endian ELF: its interpreter, DT_NEEDED names, the symbol versions it asks
/// for, and more bytes (strings) after them; section headers for .dynstr, .dynamic and
/// .gnu.version_r.
pub fn elf(machine: u16, etype: u16, interp: Option<&str>, needed: &[&str], versions: &[&str], more: &[u8]) -> Vec<u8> {
    let mut d = vec![0u8; 64];
    d[..4].copy_from_slice(b"\x7fELF");
    d[4] = 2;
    d[5] = 1;
    d[6] = 1;
    d[16..18].copy_from_slice(&etype.to_le_bytes());
    d[18..20].copy_from_slice(&machine.to_le_bytes());
    let phoff = d.len();
    if interp.is_some() {
        d.extend([0u8; 56]);
    }
    if let Some(i) = interp {
        let off = d.len() as u64;
        d.extend(i.as_bytes());
        d.push(0);
        let ph = &mut d[phoff..phoff + 56];
        ph[..4].copy_from_slice(&3u32.to_le_bytes());
        ph[8..16].copy_from_slice(&off.to_le_bytes());
        ph[32..40].copy_from_slice(&(i.len() as u64 + 1).to_le_bytes());
    }
    let mut strtab = vec![0u8];
    let mut name = |s: &str| {
        let o = strtab.len() as u64;
        strtab.extend(s.as_bytes());
        strtab.push(0);
        o
    };
    let mut dynamic = Vec::new();
    for n in needed {
        dynamic.extend(1u64.to_le_bytes());
        dynamic.extend(name(n).to_le_bytes());
    }
    dynamic.extend([0u8; 16]);
    let mut verneed = Vec::new();
    if !versions.is_empty() {
        verneed.extend(1u16.to_le_bytes());
        verneed.extend((versions.len() as u16).to_le_bytes());
        verneed.extend(0u32.to_le_bytes());
        verneed.extend(16u32.to_le_bytes());
        verneed.extend(0u32.to_le_bytes());
        for (i, v) in versions.iter().enumerate() {
            verneed.extend(0u32.to_le_bytes());
            verneed.extend([0u8; 4]);
            verneed.extend((name(v) as u32).to_le_bytes());
            verneed.extend((if i + 1 < versions.len() { 16u32 } else { 0 }).to_le_bytes());
        }
    }
    let mut secs = vec![(0u32, 0u64, 0u64, 0u32)];
    for (typ, body, link) in [(3u32, &strtab, 0u32), (6, &dynamic, 1), (0x6fff_fffe, &verneed, 1)] {
        secs.push((typ, d.len() as u64, body.len() as u64, link));
        d.extend(body.iter());
    }
    d.extend(more);
    while d.len() % 8 != 0 {
        d.push(0);
    }
    let shoff = d.len() as u64;
    for (typ, off, size, link) in &secs {
        let mut sh = [0u8; 64];
        sh[4..8].copy_from_slice(&typ.to_le_bytes());
        sh[24..32].copy_from_slice(&off.to_le_bytes());
        sh[32..40].copy_from_slice(&size.to_le_bytes());
        sh[40..44].copy_from_slice(&link.to_le_bytes());
        d.extend(sh);
    }
    d[32..40].copy_from_slice(&(phoff as u64).to_le_bytes());
    d[40..48].copy_from_slice(&shoff.to_le_bytes());
    d[54..56].copy_from_slice(&56u16.to_le_bytes());
    d[56..58].copy_from_slice(&(interp.is_some() as u16).to_le_bytes());
    d[58..60].copy_from_slice(&64u16.to_le_bytes());
    d[60..62].copy_from_slice(&(secs.len() as u16).to_le_bytes());
    d
}

/// A PE file's first bytes for a machine.
pub fn pe(machine: u16) -> Vec<u8> {
    let mut d = vec![0u8; 0x200];
    d[..2].copy_from_slice(b"MZ");
    d[0x3c..0x40].copy_from_slice(&0x80u32.to_le_bytes());
    d[0x80..0x84].copy_from_slice(b"PE\0\0");
    d[0x84..0x86].copy_from_slice(&machine.to_le_bytes());
    d
}

/// What a made-up assembly defines and references.
#[derive(Default)]
pub struct Asm<'a> {
    /// (name, major version): the Assembly table's row
    pub assembly: Option<(&'a str, u16)>,
    /// (namespace, name)
    pub typerefs: &'a [(&'a str, &'a str)],
    pub typedefs: &'a [(&'a str, &'a str)],
    pub methods: &'a [&'a str],
    /// (type reference, 1-based; member)
    pub memberrefs: &'a [(u16, &'a str)],
    pub modulerefs: &'a [&'a str],
    /// (module reference, 1-based; entry point)
    pub pinvokes: &'a [(u16, &'a str)],
}

/// A .NET assembly with only metadata: a PE with one section, the CLI header, #~, #Strings and
/// #Blob (every index two bytes wide).
pub fn dll(a: &Asm) -> Vec<u8> {
    let mut strings = vec![0u8];
    let mut st = |s: &str| {
        if s.is_empty() {
            return 0u16;
        }
        let o = strings.len() as u16;
        strings.extend(s.as_bytes());
        strings.push(0);
        o
    };
    let mut tables: Vec<(u8, usize, Vec<u8>)> = Vec::new();
    let w = |v: &mut Vec<u8>, x: u16| v.extend(x.to_le_bytes());
    let mut rows = Vec::new();
    for (ns, n) in a.typerefs {
        let (n, ns) = (st(n), st(ns));
        w(&mut rows, 0);
        w(&mut rows, n);
        w(&mut rows, ns);
    }
    tables.push((0x01, a.typerefs.len(), std::mem::take(&mut rows)));
    for (ns, n) in a.typedefs {
        let (n, ns) = (st(n), st(ns));
        rows.extend(0u32.to_le_bytes());
        for x in [n, ns, 0, 1, 1] {
            w(&mut rows, x);
        }
    }
    tables.push((0x02, a.typedefs.len(), std::mem::take(&mut rows)));
    for m in a.methods {
        let n = st(m);
        rows.extend(0u32.to_le_bytes());
        for x in [0, 0, n, 0, 1] {
            w(&mut rows, x);
        }
    }
    tables.push((0x06, a.methods.len(), std::mem::take(&mut rows)));
    for (t, m) in a.memberrefs {
        let n = st(m);
        for x in [t << 3 | 1, n, 0] {
            w(&mut rows, x);
        }
    }
    tables.push((0x0A, a.memberrefs.len(), std::mem::take(&mut rows)));
    for m in a.modulerefs {
        let n = st(m);
        w(&mut rows, n);
    }
    tables.push((0x1A, a.modulerefs.len(), std::mem::take(&mut rows)));
    for (m, e) in a.pinvokes {
        let n = st(e);
        for x in [0, 0, n, *m] {
            w(&mut rows, x);
        }
    }
    tables.push((0x1C, a.pinvokes.len(), std::mem::take(&mut rows)));
    if let Some((name, major)) = a.assembly {
        let n = st(name);
        rows.extend(0u32.to_le_bytes());
        for x in [major, 0, 0, 0] {
            w(&mut rows, x);
        }
        rows.extend(0u32.to_le_bytes());
        for x in [0, n, 0] {
            w(&mut rows, x);
        }
        tables.push((0x20, 1, std::mem::take(&mut rows)));
    }
    tables.retain(|t| t.1 > 0);
    let mut tb = vec![0u8; 24];
    let valid = tables.iter().fold(0u64, |v, t| v | 1 << t.0);
    tb[8..16].copy_from_slice(&valid.to_le_bytes());
    for t in &tables {
        tb.extend((t.1 as u32).to_le_bytes());
    }
    for t in &tables {
        tb.extend(&t.2);
    }
    pad4(&mut tb);
    pad4(&mut strings);
    let blob = vec![0u8; 4];
    let mut md = Vec::new();
    md.extend(b"BSJB");
    md.extend([1, 0, 1, 0]);
    md.extend(0u32.to_le_bytes());
    md.extend(4u32.to_le_bytes());
    md.extend(b"v4\0\0");
    md.extend(0u16.to_le_bytes());
    md.extend(3u16.to_le_bytes());
    let mut off = md.len() + (8 + 4) + (8 + 12) + (8 + 8);
    for (name, len) in [(&b"#~\0\0"[..], tb.len()), (b"#Strings\0\0\0\0", strings.len()), (b"#Blob\0\0\0", blob.len())] {
        md.extend((off as u32).to_le_bytes());
        md.extend((len as u32).to_le_bytes());
        md.extend(name);
        off += len;
    }
    md.extend(&tb);
    md.extend(&strings);
    md.extend(&blob);
    let mut body = vec![0u8; 0x48];
    body[8..12].copy_from_slice(&0x2048u32.to_le_bytes());
    body[12..16].copy_from_slice(&(md.len() as u32).to_le_bytes());
    body.extend(&md);
    let mut d = pe(0x14c);
    let p = 0x80;
    d[p + 6..p + 8].copy_from_slice(&1u16.to_le_bytes());
    d[p + 20..p + 22].copy_from_slice(&224u16.to_le_bytes());
    let opt = p + 24;
    d[opt..opt + 2].copy_from_slice(&0x10bu16.to_le_bytes());
    let dirs = opt + 96 + 14 * 8;
    d[dirs..dirs + 4].copy_from_slice(&0x2000u32.to_le_bytes());
    d[dirs + 4..dirs + 8].copy_from_slice(&0x48u32.to_le_bytes());
    let sec = opt + 224;
    let size = body.len() as u32;
    d[sec + 8..sec + 12].copy_from_slice(&size.to_le_bytes());
    d[sec + 12..sec + 16].copy_from_slice(&0x2000u32.to_le_bytes());
    d[sec + 16..sec + 20].copy_from_slice(&size.to_le_bytes());
    d[sec + 20..sec + 24].copy_from_slice(&0x200u32.to_le_bytes());
    d.extend(body);
    d
}

/// A serialized file without objects (little endian; format 17 as Unity 2017.4 writes, 21 as
/// 2019.4): its header, the Unity version and the build target.
pub fn sf_empty(version: &str, format: u32, target: i32) -> Vec<u8> {
    let mut m = Vec::new();
    m.extend(version.as_bytes());
    m.push(0);
    m.extend(target.to_le_bytes());
    m.push(0);
    for _ in 0..(if format >= 20 { 5 } else { 4 }) {
        m.extend(0i32.to_le_bytes());
    }
    m.push(0);
    let total = (20 + m.len()).div_ceil(16) * 16;
    let mut d = Vec::new();
    for x in [m.len() as u32, total as u32, format, total as u32] {
        d.extend(x.to_be_bytes());
    }
    d.extend([0u8; 4]);
    d.extend(m);
    d.resize(total, 0);
    d
}

