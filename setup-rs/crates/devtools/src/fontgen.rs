//! `pmu-dev fontgen <font.pcf[.gz]> > osk_font.h`: runtime/host/osk_font.h (the on-screen keyboard's
//! font) from an X11 PCF bitmap font (was runtime/host/fontgen.py; the same header but its first line).
//!
//! osk_font.h is made from misc-fixed 6x13 (6x13-ISO8859-1.pcf.gz of Debian's xfonts-base, upstream
//! font-misc-misc: "Public domain font.  Share and enjoy."):
//!   docker run --rm -v "$PWD":/out debian:bookworm-slim sh -c 'cd /tmp && apt-get update -qq &&
//!     apt-get download -qq xfonts-base && dpkg-deb -x xfonts-base_*.deb x &&
//!     cp x/usr/share/fonts/X11/misc/6x13-ISO8859-1.pcf.gz /out/'
//! Code points 32-126 and 160-255, one row byte per scan line (bit 7 = leftmost pixel), all glyphs in
//! one cell of the font's width and height (ascent + descent).
use std::collections::HashMap;
use std::fmt::Write as _;
use std::io::Read;

const PCF_PROPERTIES: u32 = 1 << 0;
const PCF_ACCELERATORS: u32 = 1 << 1;
const PCF_METRICS: u32 = 1 << 2;
const PCF_BITMAPS: u32 = 1 << 3;
const PCF_BDF_ENCODINGS: u32 = 1 << 5;
const PCF_BDF_ACCELERATORS: u32 = 1 << 8;
const PCF_COMPRESSED_METRICS: u32 = 0x100;

struct Pcf<'a> {
    d: &'a [u8],
}

impl Pcf<'_> {
    fn get(&self, off: usize, n: usize) -> Result<&[u8], String> {
        self.d.get(off..off + n).ok_or_else(|| "the font ends early".to_string())
    }
    fn u32le(&self, off: usize) -> Result<u32, String> {
        Ok(u32::from_le_bytes(self.get(off, 4)?.try_into().unwrap()))
    }
    fn u32(&self, off: usize, be: bool) -> Result<u32, String> {
        let b: [u8; 4] = self.get(off, 4)?.try_into().unwrap();
        Ok(if be { u32::from_be_bytes(b) } else { u32::from_le_bytes(b) })
    }
    fn u16(&self, off: usize, be: bool) -> Result<u16, String> {
        let b: [u8; 2] = self.get(off, 2)?.try_into().unwrap();
        Ok(if be { u16::from_be_bytes(b) } else { u16::from_le_bytes(b) })
    }
    /// (format, big-endian)
    fn fmt(&self, off: usize) -> Result<(u32, bool), String> {
        let f = self.u32le(off)?;
        Ok((f, f & 4 != 0))
    }
}

enum Prop {
    Str(String),
    Int(u32),
}

fn latin1(b: &[u8]) -> String {
    b.iter().map(|&c| c as char).collect()
}

pub fn run(args: &[String]) -> Result<(), String> {
    let [path] = args else {
        return Err("usage: pmu-dev fontgen <font.pcf[.gz]>".into());
    };
    let raw = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
    let data = if raw.starts_with(b"\x1f\x8b") {
        let mut v = Vec::new();
        flate2::read::MultiGzDecoder::new(&raw[..]).read_to_end(&mut v).map_err(|e| format!("{path}: {e}"))?;
        v
    } else {
        raw
    };
    print!("{}", header(&data, path.rsplit('/').next().unwrap_or(path))?);
    Ok(())
}

/// osk_font.h's text from a PCF font's bytes (name: the font file's name, for the first line).
pub fn header(data: &[u8], name: &str) -> Result<String, String> {
    let p = Pcf { d: data };
    if p.get(0, 4)? != b"\x01fcp" {
        return Err("not a PCF font".into());
    }
    let n = p.u32le(4)? as usize;
    let mut t: HashMap<u32, usize> = HashMap::new();
    for i in 0..n {
        let typ = p.u32le(8 + 16 * i)?;
        let off = p.u32le(8 + 16 * i + 12)? as usize;
        t.insert(typ, off);
    }
    let tab = |k: u32| t.get(&k).copied().ok_or_else(|| format!("no table {k:#x}"));
    // properties
    let off = tab(PCF_PROPERTIES)?;
    let (_, be) = p.fmt(off)?;
    let np = p.u32(off + 4, be)? as usize;
    let mut raw_props = Vec::new();
    let mut q = off + 8;
    for _ in 0..np {
        let name = p.u32(q, be)? as usize;
        let isstr = p.get(q + 4, 1)?[0];
        let val = p.u32(q + 5, be)?;
        raw_props.push((name, isstr, val));
        q += 9;
    }
    q += (4 - (np & 3)) & 3;
    let strsize = p.u32(q, be)? as usize;
    let strs = p.get(q + 4, strsize)?;
    let cstr = |o: usize| -> Result<String, String> {
        let s = strs.get(o..).ok_or("a property name past the strings")?;
        let e = s.iter().position(|&c| c == 0).ok_or("a property name without its end")?;
        Ok(latin1(&s[..e]))
    };
    let mut props: HashMap<String, Prop> = HashMap::new();
    for (nm, s, v) in raw_props {
        let val = if s != 0 { Prop::Str(cstr(v as usize)?) } else { Prop::Int(v) };
        props.insert(cstr(nm)?, val);
    }
    // metrics
    let off = tab(PCF_METRICS)?;
    let (fmt, be) = p.fmt(off)?;
    let mut mets: Vec<[i32; 5]> = Vec::new();
    if fmt & PCF_COMPRESSED_METRICS != 0 {
        let n = p.u16(off + 4, be)? as usize;
        for i in 0..n {
            let b = p.get(off + 6 + 5 * i, 5)?;
            mets.push([0, 1, 2, 3, 4].map(|k| b[k] as i32 - 0x80));
        }
    } else {
        let n = p.u32(off + 4, be)? as usize;
        for i in 0..n {
            let mut m = [0i32; 5];
            for (k, x) in m.iter_mut().enumerate() {
                *x = p.u16(off + 8 + 12 * i + 2 * k, be)? as i16 as i32;
            }
            mets.push(m);
        }
    }
    // bitmaps
    let off = tab(PCF_BITMAPS)?;
    let (fmt, be) = p.fmt(off)?;
    let nb = p.u32(off + 4, be)? as usize;
    let mut offs = Vec::with_capacity(nb);
    for i in 0..nb {
        offs.push(p.u32(off + 8 + 4 * i, be)? as usize);
    }
    let size = p.u32(off + 8 + 4 * nb + 4 * (fmt & 3) as usize, be)? as usize;
    let pad = 1usize << (fmt & 3);
    let base = off + 8 + 4 * nb + 16;
    let msbit = fmt & 8 != 0;
    let bits = p.get(base, size)?;
    // encodings
    let off = tab(PCF_BDF_ENCODINGS)?;
    let (_, be) = p.fmt(off)?;
    let [c1, c2, r1, r2] = [0, 1, 2, 3].map(|k| p.u16(off + 4 + 2 * k, be).unwrap_or(0) as usize);
    let cols = c2 + 1 - c1;
    let count = cols * (r2 + 1 - r1);
    let mut idx = Vec::with_capacity(count);
    for i in 0..count {
        idx.push(p.u16(off + 14 + 2 * i, be)?);
    }
    let glyph = |cp: usize| -> Option<usize> {
        let (r, c) = (cp >> 8, cp & 0xff);
        if !(r1 <= r && r <= r2 && c1 <= c && c <= c2) {
            return None;
        }
        let g = idx[(r - r1) * cols + c - c1];
        (g != 0xffff).then_some(g as usize)
    };
    let (ascent, descent) = match (props.get("FONT_ASCENT"), props.get("FONT_DESCENT")) {
        (Some(Prop::Int(a)), Some(Prop::Int(d))) => (*a as i32, *d as i32),
        _ => {
            let off = t.get(&PCF_BDF_ACCELERATORS).or_else(|| t.get(&PCF_ACCELERATORS)).copied().ok_or("no accelerators")?;
            let (_, be) = p.fmt(off)?;
            (p.u32(off + 12, be)? as i32, p.u32(off + 16, be)? as i32)
        }
    };
    let width = mets.iter().map(|m| m[2]).max().unwrap_or(0);
    let height = ascent + descent;
    if width > 8 {
        return Err("glyphs wider than 8 pixels are not supported".into());
    }
    let cps: Vec<usize> = (32..127).chain(160..256).collect();
    let mut out = String::new();
    let prop = |k: &str, dflt: &str| match props.get(k) {
        Some(Prop::Str(s)) => s.clone(),
        Some(Prop::Int(v)) => v.to_string(),
        None => dflt.to_string(),
    };
    writeln!(out, "/* Generated by pmu-dev fontgen from {name}: {} ({}). */", prop("FONT", "?"), prop("COPYRIGHT", "no copyright property")).unwrap();
    writeln!(out, "#define OSK_FONT_W {width}").unwrap();
    writeln!(out, "#define OSK_FONT_H {height}").unwrap();
    writeln!(out, "#define OSK_FONT_ASCENT {ascent}").unwrap();
    out += "/* code points 32-126, then 160-255; row bytes, bit 7 = leftmost pixel */\n";
    writeln!(out, "static const unsigned char osk_font[{}][OSK_FONT_H] = {{", cps.len()).unwrap();
    for cp in cps {
        let mut cell = vec![0u8; height.max(0) as usize];
        if let Some(g) = glyph(cp) {
            let [l, r, _w, a, d] = mets[g];
            let stride = (((r - l + 7) / 8) as usize + pad - 1) / pad * pad;
            for y in 0..(a + d).max(0) {
                for x in 0..(r - l).max(0) {
                    let byte = bits[offs[g] + y as usize * stride + x as usize / 8];
                    let on = if msbit { byte & (0x80 >> (x % 8)) } else { byte & (1 << (x % 8)) };
                    let (cx, cy) = (l + x, ascent - a + y);
                    if on != 0 && (0..8).contains(&cx) && (0..height).contains(&cy) {
                        cell[cy as usize] |= 0x80 >> cx;
                    }
                }
            }
        }
        let label = if cp > 126 || [42, 47, 92].contains(&cp) {
            format!("U+{cp:04X}")
        } else if cp == b'\'' as usize {
            "\"'\"".to_string()
        } else {
            format!("'{}'", cp as u8 as char)
        };
        let bytes: Vec<String> = cell.iter().map(|b| format!("0x{b:02x}")).collect();
        writeln!(out, "    {{ {} }},  /* {label} */", bytes.join(", ")).unwrap();
    }
    out += "};\n";
    Ok(out)
}
