//! A bank's sound data (FSB5) and its samples rebuilt as standard Ogg Vorbis.
//!
//! FSB5 (public layout): "FSB5", version, sample count, sample-header size, name-table size, data
//! size, codec (15 = Vorbis); 60 bytes of header (version 1); per sample a u64 (bit 0: chunks
//! follow, bits 1-4: rate index, bits 5-6: channels 1 / 2 / 6 / 8, bits 7-33: data offset / 32, bits 34-63: samples)
//! and its chunks (u32: bit 0 more, bits 1-24 size, bits 25-31 type: 1 channels, 2 rate, 3 loop,
//! 11 Vorbis: the setup header's id (u32) and a seek table); then the names, then the data. A
//! Vorbis sample's data is its audio packets, each after a u16 size. The identification and comment
//! headers are not stored (the rate, channels and block sizes 256 / 2048 are FMOD's), nor the setup
//! header: FMOD's library holds those, named by the id. They are read from the player's own copy
//! of the game's FMOD library (`setup_headers`): nothing of FMOD's is in the tools.
use std::collections::HashMap;

use lewton::header::{read_header_ident, read_header_setup, IdentHeader, SetupHeader};

use crate::rd::{Rd, Result};

pub const CODEC_VORBIS: u32 = 15;
const RATES: [u32; 10] = [0, 8000, 11000, 11025, 16000, 22050, 24000, 32000, 44100, 48000];

#[derive(Clone, Debug, Default)]
pub struct Sample {
    pub rate: u32,
    pub channels: u32,
    pub samples: u32,
    /// the sample's data: offset in the bank file, length
    pub data: (usize, usize),
    pub loop_: Option<(u32, u32)>,
    pub setup_id: Option<u32>,
    pub name: String,
}

#[derive(Clone, Debug, Default)]
pub struct Fsb {
    pub codec: u32,
    pub samples: Vec<Sample>,
}

/// The FSB5 at `at` in the bank file `d`.
pub fn read(d: &[u8], at: usize, len: usize) -> Result<Fsb> {
    let f = d.get(at..at + len).ok_or("sound data past the file")?;
    let mut r = Rd::new(f);
    if r.take(4)? != b"FSB5" {
        return Err("no FSB5 header".into());
    }
    let ver = r.u32()?;
    let n = r.u32()? as usize;
    let shs = r.u32()? as usize;
    let nms = r.u32()? as usize;
    let ds = r.u32()? as usize;
    let codec = r.u32()?;
    let hs = if ver == 0 { 0x40 } else { 0x3c };
    r.p = hs;
    let mut out = Vec::with_capacity(n.min(65536));
    for _ in 0..n {
        let raw = r.u64()?;
        let mut s = Sample {
            rate: RATES.get(((raw >> 1) & 0xf) as usize).copied().unwrap_or(0),
            channels: [1, 2, 6, 8][((raw >> 5) & 3) as usize],
            samples: ((raw >> 34) & 0x3fff_ffff) as u32,
            data: ((((raw >> 7) & 0x07ff_ffff) * 32) as usize, 0),
            ..Default::default()
        };
        let mut more = raw & 1 != 0;
        while more {
            let c = r.u32()?;
            more = c & 1 != 0;
            let size = ((c >> 1) & 0xff_ffff) as usize;
            let typ = (c >> 25) & 0x7f;
            let body = r.take(size)?;
            let mut b = Rd::new(body);
            match typ {
                1 => s.channels = b.u8()? as u32,
                2 => s.rate = b.u32()?,
                3 => s.loop_ = Some((b.u32()?, b.u32()?)),
                11 => s.setup_id = Some(b.u32()?),
                _ => {}
            }
        }
        out.push(s);
    }
    if hs + shs + nms + ds > f.len() {
        return Err("FSB5 sizes past its data".into());
    }
    let names = hs + shs;
    if nms >= 4 * n && n > 0 {
        for (k, s) in out.iter_mut().enumerate() {
            let o = u32::from_le_bytes(f[names + 4 * k..names + 4 * k + 4].try_into().unwrap()) as usize;
            if let Some(t) = f.get(names + o..names + nms) {
                let e = t.iter().position(|&c| c == 0).unwrap_or(t.len());
                s.name = String::from_utf8_lossy(&t[..e]).into_owned();
            }
        }
    }
    let base = names + nms;
    // (samples are not in the order of their data: each ends where the next one by offset starts)
    let mut starts: Vec<usize> = out.iter().map(|s| s.data.0).collect();
    starts.sort_unstable();
    starts.dedup();
    for k in 0..out.len() {
        let start = out[k].data.0;
        let end = starts.iter().copied().find(|&x| x > start).unwrap_or(ds);
        if start > end || end > ds {
            return Err(format!("sample {k}: data {start}..{end} outside {ds}"));
        }
        out[k].data = (at + base + start, end - start);
    }
    Ok(Fsb { codec, samples: out })
}

/// FMOD's Vorbis identification header for a sample (block sizes 256 and 2048).
pub fn ident(channels: u32, rate: u32) -> Vec<u8> {
    let mut v = b"\x01vorbis".to_vec();
    v.extend_from_slice(&0u32.to_le_bytes());
    v.push(channels as u8);
    v.extend_from_slice(&rate.to_le_bytes());
    v.extend_from_slice(&[0; 12]);
    v.push(0xb8);
    v.push(1);
    v
}

fn comment() -> Vec<u8> {
    let vendor = b"ua_host fmod";
    let mut v = b"\x03vorbis".to_vec();
    v.extend_from_slice(&(vendor.len() as u32).to_le_bytes());
    v.extend_from_slice(vendor);
    v.extend_from_slice(&0u32.to_le_bytes());
    v.push(1);
    v
}

/// A setup header found in the game's library: its bytes and lewton's reading of it.
pub struct Setup {
    pub bytes: Vec<u8>,
    pub parsed: HashMap<u32, (IdentHeader, SetupHeader)>,
}

/// ELF: a virtual address to a file offset (its PT_LOAD segment).
fn elf_offset(lib: &[u8], va: u64) -> Option<usize> {
    if lib.len() < 64 || &lib[0..4] != b"\x7fELF" {
        return None;
    }
    let is64 = lib[4] == 2;
    let rd32 = |o: usize| lib.get(o..o + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()) as u64);
    let rd64 = |o: usize| lib.get(o..o + 8).map(|b| u64::from_le_bytes(b.try_into().unwrap()));
    let rd16 = |o: usize| lib.get(o..o + 2).map(|b| u16::from_le_bytes(b.try_into().unwrap()) as u64);
    let (phoff, phentsize, phnum) = if is64 { (rd64(0x20)?, rd16(0x36)?, rd16(0x38)?) } else { (rd32(0x1c)?, rd16(0x2a)?, rd16(0x2c)?) };
    for i in 0..phnum {
        let p = (phoff + i * phentsize) as usize;
        if rd32(p)? != 1 {
            continue;
        }
        let (off, vaddr, filesz) = if is64 { (rd64(p + 8)?, rd64(p + 16)?, rd64(p + 32)?) } else { (rd32(p + 4)?, rd32(p + 8)?, rd32(p + 16)?) };
        if va >= vaddr && va < vaddr + filesz {
            return Some((va - vaddr + off) as usize);
        }
    }
    None
}

/// The Vorbis setup headers the samples name (`ids`), from the game's FMOD library `lib` (its
/// file's bytes; an ELF: libfmod.so of the Linux build). In the library each header is a block of
/// read-only data starting "\x05vorbis", and a table pairs each header's id (u32) with a pointer to
/// it (the next pointer-sized word after the id); the header's length is the shortest that reads
/// as a whole Vorbis setup header for the sample's channels (Blasphemous's libfmod.so, FMOD 1.10:
/// that length is also stored 28 bytes after the id; the block runs on into the next header's
/// table data, so a longer read also parses). Returns the headers found (by id) and the ids not
/// found.
pub fn setup_headers(lib: &[u8], ids: &[(u32, u32)]) -> (HashMap<u32, Setup>, Vec<u32>) {
    let mut found: HashMap<u32, Setup> = HashMap::new();
    let mut missing = Vec::new();
    let is64 = lib.get(4) == Some(&2);
    let word = |o: usize| -> Option<u64> {
        if is64 {
            lib.get(o..o + 8).map(|b| u64::from_le_bytes(b.try_into().unwrap()))
        } else {
            lib.get(o..o + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()) as u64)
        }
    };
    let u32at = |o: usize| lib.get(o..o + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()) as usize);
    for &(id, channels) in ids {
        if let Some(s) = found.get_mut(&id) {
            if !s.parsed.contains_key(&channels) {
                if let Ok(p) = parse_setup(&s.bytes, channels) {
                    s.parsed.insert(channels, p);
                }
            }
            continue;
        }
        let pat = id.to_le_bytes();
        let mut hit = None;
        let mut at = 0;
        while let Some(i) = lib[at..].windows(4).position(|w| w == pat) {
            let o = at + i;
            at = o + 1;
            let Some(fo) = word(o + 4).and_then(|p| elf_offset(lib, p)) else { continue };
            let Some(rest) = lib.get(fo..) else { continue };
            if !rest.starts_with(b"\x05vorbis") {
                continue;
            }
            let max = rest.len().min(65536);
            let reads = |n: usize| n <= max && parse_setup(&rest[..n], channels).is_ok();
            // the stored lengths first, then every length
            let mut len = [u32at(o + 28), u32at(o + 12)].into_iter().flatten().find(|&n| n > 16 && reads(n) && !reads(n - 1));
            if len.is_none() {
                len = (16..max.min(16384)).find(|&n| reads(n));
            }
            if let Some(n) = len {
                let mut parsed = HashMap::new();
                parsed.insert(channels, parse_setup(&rest[..n], channels).unwrap());
                hit = Some(Setup { bytes: rest[..n].to_vec(), parsed });
                break;
            }
        }
        match hit {
            Some(s) => {
                found.insert(id, s);
            }
            None => missing.push(id),
        }
    }
    (found, missing)
}

fn parse_setup(blk: &[u8], channels: u32) -> std::result::Result<(IdentHeader, SetupHeader), String> {
    let id = read_header_ident(&ident(channels, 48000)).map_err(|e| format!("{e:?}"))?;
    let s = read_header_setup(blk, channels as u8, (id.blocksize_0, id.blocksize_1)).map_err(|e| format!("{e:?}"))?;
    Ok((id, s))
}

/// The audio packets of a Vorbis sample's data (each after a u16 size; a size 0 ends them).
pub fn packets(data: &[u8]) -> Result<Vec<&[u8]>> {
    let mut v = Vec::new();
    let mut p = 0;
    while p + 2 <= data.len() {
        let n = u16::from_le_bytes([data[p], data[p + 1]]) as usize;
        p += 2;
        if n == 0 {
            break;
        }
        v.push(data.get(p..p + n).ok_or("a Vorbis packet past the sample's data")?);
        p += n;
    }
    Ok(v)
}

fn crc_table() -> [u32; 256] {
    let mut t = [0u32; 256];
    for (i, e) in t.iter_mut().enumerate() {
        let mut r = (i as u32) << 24;
        for _ in 0..8 {
            r = if r & 0x8000_0000 != 0 { (r << 1) ^ 0x04c1_1db7 } else { r << 1 };
        }
        *e = r;
    }
    t
}

struct Ogg {
    out: Vec<u8>,
    seq: u32,
    serial: u32,
    crc: [u32; 256],
}

impl Ogg {
    fn page(&mut self, lace: &[u8], data: &[u8], granule: u64, flags: u8) {
        let start = self.out.len();
        self.out.extend_from_slice(b"OggS\0");
        self.out.push(flags);
        self.out.extend_from_slice(&granule.to_le_bytes());
        self.out.extend_from_slice(&self.serial.to_le_bytes());
        self.out.extend_from_slice(&self.seq.to_le_bytes());
        self.out.extend_from_slice(&[0; 4]);
        self.out.push(lace.len() as u8);
        self.out.extend_from_slice(lace);
        self.out.extend_from_slice(data);
        let mut c = 0u32;
        for &b in &self.out[start..] {
            c = (c << 8) ^ self.crc[((c >> 24) as u8 ^ b) as usize];
        }
        self.out[start + 22..start + 26].copy_from_slice(&c.to_le_bytes());
        self.seq += 1;
    }

    /// Packets (bytes, granule after it) on pages of up to 4 KB; a packet may span pages.
    fn pages(&mut self, packets: &[(&[u8], u64)], first: bool, last: bool) {
        let (mut lace, mut data) = (Vec::new(), Vec::new());
        let (mut granule, mut cont, mut first) = (u64::MAX, false, first);
        for (k, (p, g)) in packets.iter().enumerate() {
            let mut rest: &[u8] = p;
            loop {
                let mut done = false;
                while lace.len() < 255 && data.len() < 4096 {
                    let n = rest.len().min(255);
                    lace.push(n as u8);
                    data.extend_from_slice(&rest[..n]);
                    rest = &rest[n..];
                    if n < 255 {
                        done = true;
                        granule = *g;
                        break;
                    }
                }
                let full = lace.len() == 255 || data.len() >= 4096;
                if done && !full {
                    break;
                }
                let end = done && last && k + 1 == packets.len();
                let fl = cont as u8 | if first { 2 } else { 0 } | if end { 4 } else { 0 };
                self.page(&lace, &data, granule, fl);
                first = false;
                lace.clear();
                data.clear();
                granule = u64::MAX;
                cont = !done;
                if done {
                    break;
                }
            }
        }
        if !lace.is_empty() {
            let fl = cont as u8 | if first { 2 } else { 0 } | if last { 4 } else { 0 };
            self.page(&lace, &data, granule, fl);
        }
    }
}

/// A Vorbis sample as a standard Ogg Vorbis stream: the identification and comment headers, the
/// library's setup header, the audio packets with their granule positions (the last page's is the
/// sample count, so a decoder trims the end to it). No decoding: lewton reads each packet's block
/// size from its first bits.
pub fn to_ogg(d: &[u8], s: &Sample, setup: &Setup, serial: u32) -> Result<Vec<u8>> {
    let (id, st) = setup.parsed.get(&s.channels).ok_or("no setup header for these channels")?;
    let data = d.get(s.data.0..s.data.0 + s.data.1).ok_or("sample data past the file")?;
    let pk = packets(data)?;
    let mut list = Vec::with_capacity(pk.len());
    let mut g: u64 = 0;
    let total = s.samples as u64;
    for (k, p) in pk.iter().enumerate() {
        let n = lewton::audio::get_decoded_sample_count(id, st, p).map_err(|e| format!("packet {k}: {e:?}"))? as u64;
        if k > 0 {
            g += n;
        }
        list.push((*p, g.min(total)));
    }
    // packets past the sample count are dropped; the last one gets a page of its own with the sample
    // count as its granule, so a decoder trims the end to it (FMOD's short samples carry up to a few
    // hundred samples of padding past their count)
    if let Some(k) = list.iter().position(|x| x.1 >= total) {
        list.truncate(k + 1);
    }
    if let Some(l) = list.last_mut() {
        l.1 = total;
    }
    let mut o = Ogg { out: Vec::with_capacity(data.len() + data.len() / 30 + 8192), seq: 0, serial, crc: crc_table() };
    let idh = ident(s.channels, s.rate);
    let cm = comment();
    o.pages(&[(&idh, 0)], true, false);
    o.pages(&[(&cm, 0), (&setup.bytes, 0)], false, false);
    let last = list.pop();
    o.pages(&list, false, last.is_none());
    if let Some(l) = last {
        o.pages(&[l], false, true);
    }
    Ok(o.out)
}
