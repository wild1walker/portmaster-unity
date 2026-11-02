//! NSIS installers (nsis.py): their files listed from the script's header and extracted one at a
//! time, from a local file or over HTTP(S) with one range request each (http.rs), so of Unity's
//! 200 - 700 MB installers only the header and the files wanted are read. LZMA (with or without
//! the x86 BCJ filter byte) and deflate blocks; Unity 6's installers' 2 MiB LZMA chunks, per-file
//! (block words of 8 bytes) or solid (the files one stream of chunks, walked by their lengths).
//! Old-style solid installers (one LZMA stream for everything) are refused.
use crate::http::Client;
use crate::{Error, Result};
use std::collections::BTreeMap;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};

const FH_SIG: &[u8] = b"\xef\xbe\xad\xdeNullsoftInst";
const VARS: [&str; 32] = [
    "$0",
    "$1",
    "$2",
    "$3",
    "$4",
    "$5",
    "$6",
    "$7",
    "$8",
    "$9",
    "$R0",
    "$R1",
    "$R2",
    "$R3",
    "$R4",
    "$R5",
    "$R6",
    "$R7",
    "$R8",
    "$R9",
    "$CMDLINE",
    "$INSTDIR",
    "$OUTDIR",
    "$EXEDIR",
    "$LANGUAGE",
    "$TEMP",
    "$PLUGINSDIR",
    "$EXEPATH",
    "$EXEFILE",
    "$HWNDPARENT",
    "$_CLICK",
    "$_OUTDIR",
];
const EW_CREATEDIR: u32 = 11;
const EW_EXTRACTFILE: u32 = 20;
/// Unity 6 installers: the uncompressed size of every LZMA chunk but a stream's last.
const CHUNK: u64 = 2 << 20;

fn exit<T>(m: impl Into<String>) -> Result<T> {
    Err(Error::Exit(m.into()))
}

fn u32le(b: &[u8], p: usize) -> Result<u32> {
    b.get(p..p + 4)
        .map(|s| u32::from_le_bytes(s.try_into().unwrap()))
        .ok_or_else(|| Error::Fail("struct.error: unpack_from requires a buffer of 4 bytes".into()))
}

fn u64le(b: &[u8], p: usize) -> Result<u64> {
    b.get(p..p + 8)
        .map(|s| u64::from_le_bytes(s.try_into().unwrap()))
        .ok_or_else(|| Error::Fail("struct.error: unpack_from requires a buffer of 8 bytes".into()))
}

fn u16le(b: &[u8], p: usize) -> Result<u16> {
    b.get(p..p + 2)
        .map(|s| u16::from_le_bytes(s.try_into().unwrap()))
        .ok_or_else(|| Error::Fail("struct.error: unpack_from requires a buffer of 2 bytes".into()))
}

fn le3(b: &[u8]) -> u64 {
    b.iter()
        .take(3)
        .enumerate()
        .fold(0, |a, (i, &x)| a | (x as u64) << (8 * i))
}

/// Random access to a local file or an http(s) URL (nsis.Source).
pub struct Source {
    src: String,
    file: Option<File>,
    client: Client,
    /// bytes read over the network, and the requests (for the log)
    pub fetched: u64,
    pub requests: u64,
    log: Box<dyn FnMut(&str)>,
}

impl Source {
    pub fn open(src: &str, log: Box<dyn FnMut(&str)>) -> Result<Source> {
        let url = src.starts_with("http://") || src.starts_with("https://");
        let file = if url {
            None
        } else {
            Some(File::open(src).map_err(|e| Error::Fail(format!("{e}: {src:?}")))?)
        };
        Ok(Source {
            src: src.to_string(),
            file,
            client: Client::default(),
            fetched: 0,
            requests: 0,
            log,
        })
    }

    fn get(&mut self, off: u64, n: u64) -> std::io::Result<Vec<u8>> {
        let rng = format!("bytes={}-{}", off, off.saturating_add(n - 1));
        // (the range's bytes, and room for a redirect's or an error's page)
        self.client.max_body = n.saturating_add(1 << 16);
        let r = self.client.get_following(
            &self.src.clone(),
            &[("Range", &rng), ("User-Agent", "pmunity")],
            5,
        )?;
        if r.status != 206 {
            return Err(std::io::Error::other(format!(
                "server ignored the Range request (status {})",
                r.status
            )));
        }
        Ok(r.body)
    }

    /// `n` bytes at `off` (fewer at the end of a local file; an empty file's block: none).
    pub fn read(&mut self, off: u64, n: u64) -> Result<Vec<u8>> {
        if n == 0 {
            return Ok(Vec::new());
        }
        if let Some(f) = &mut self.file {
            f.seek(SeekFrom::Start(off))?;
            let mut out = Vec::new();
            f.take(n).read_to_end(&mut out)?;
            return Ok(out);
        }
        for attempt in 0..6u64 {
            let r = self.get(off, n).and_then(|d| {
                if d.len() as u64 != n {
                    Err(std::io::Error::other(format!(
                        "short read {} of {n}",
                        d.len()
                    )))
                } else {
                    Ok(d)
                }
            });
            match r {
                Ok(d) => {
                    self.fetched += n;
                    self.requests += 1;
                    return Ok(d);
                }
                Err(e) => {
                    self.client.close();
                    if attempt == 5 {
                        return Err(Error::Fail(e.to_string()));
                    }
                    (self.log)(&format!("retry {} at {off}: {e}", attempt + 1));
                    std::thread::sleep(std::time::Duration::from_secs(3 * (attempt + 1)));
                }
            }
        }
        unreachable!()
    }
}

/// The most a file (or the script header) may decompress to: it is held in memory, as the
/// Python holds it, and a few bytes of a damaged block could make gigabytes.
pub const MAX_FILE: u64 = 1 << 30;

fn too_big<T>(what: &str) -> Result<T> {
    Err(Error::Fail(format!(
        "nsis: {what} of more than {MAX_FILE} bytes (a damaged installer)"
    )))
}

/// An NSIS LZMA block (props byte, 32-bit dictionary size, the raw stream; a 0 / 1 byte first:
/// the x86 BCJ filter off / on) or raw deflate, decompressed (nsis.decompress): its first `max`
/// bytes (the dictionary allocated for no more than that).
pub fn decompress(blk: &[u8], max: u64) -> Result<Vec<u8>> {
    let lz = blk.first() == Some(&0x5d)
        || (matches!(blk.first(), Some(0 | 1)) && blk.get(1) == Some(&0x5d));
    let mut out = Vec::new();
    if lz {
        let (bcj, b) = match blk[0] {
            0 | 1 => (blk[0] == 1, &blk[1..]),
            _ => (false, blk),
        };
        let props = b[0];
        // (no match reaches further back than the output: a smaller dictionary is exact)
        let dict = u32le(b, 1)?
            .min(crate::MAX_DICT)
            .min(u32::try_from(max).unwrap_or(u32::MAX));
        let reader = lzma_rust2::LzmaReader::new_with_props(&b[5..], u64::MAX, props, dict, None)
            .map_err(|e| Error::Fail(format!("LZMAError: {e}")))?;
        let res = if bcj {
            lzma_rust2::filter::bcj::BcjReader::new_x86(reader, 0)
                .take(max)
                .read_to_end(&mut out)
        } else {
            reader.take(max).read_to_end(&mut out)
        };
        // (a stream without an end marker: what it holds, as Python's LZMADecompressor gives it)
        if let Err(e) = res {
            if out.is_empty() {
                return Err(Error::Fail(format!("LZMAError: {e}")));
            }
        }
        return Ok(out);
    }
    let mut d = flate2::read::DeflateDecoder::new(blk).take(max);
    match d.read_to_end(&mut out) {
        Ok(_) => Ok(out),
        Err(_) if !out.is_empty() => Ok(out),
        Err(e) => Err(Error::Fail(format!("zlib.error: {e}"))),
    }
}

/// A file's block decompressed whole (at most MAX_FILE bytes).
fn whole(blk: &[u8]) -> Result<Vec<u8>> {
    let out = decompress(blk, MAX_FILE + 1)?;
    if out.len() as u64 > MAX_FILE {
        return too_big("a block");
    }
    Ok(out)
}

/// A Unity 6 block body: LZMA chunks, each a 3-byte length and a stream, up to a zero length
/// (at most MAX_FILE bytes in all).
pub fn dechunk(blk: &[u8]) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    let mut p = 0;
    while p + 3 <= blk.len() {
        let n = le3(&blk[p..]) as usize;
        if n == 0 {
            break;
        }
        let end = (p + 3 + n).min(blk.len());
        out.extend(decompress(
            &blk[p + 3..end],
            MAX_FILE + 1 - out.len() as u64,
        )?);
        if out.len() as u64 > MAX_FILE {
            return too_big("a block");
        }
        p += 3 + n;
    }
    Ok(out)
}

/// An installer's script header and where its files are (nsis.Nsis).
pub struct Nsis {
    pub s: Source,
    fh: u64,
    total: u64,
    chunked: bool,
    solid: bool,
    usize: u64,
    h: Vec<u8>,
    data_base: u64,
    chunk_pos: Vec<u64>,
    chunk_len: Vec<u64>,
    end: u64,
    cache: BTreeMap<u64, Vec<u8>>,
    blocks: Vec<(u32, u32)>,
    entsize: usize,
    unicode: bool,
    /// Unicode string codes: code -> lang | shell | var | skip
    codes: BTreeMap<u16, &'static str>,
}

impl Nsis {
    pub fn open(src: &str, log: Box<dyn FnMut(&str)>) -> Result<Nsis> {
        let mut s = Source::open(src, log)?;
        let mut head = s.read(0, 1 << 20)?;
        let mut pos = 0;
        let i;
        loop {
            let found = crate::py::find(&head[pos.min(head.len())..], FH_SIG).map(|k| k + pos);
            match found {
                None if head.len() < 8 << 20 => {
                    let more = s.read(head.len() as u64, head.len() as u64)?;
                    if more.is_empty() {
                        return exit("no NSIS first header in the first 8 MiB");
                    }
                    head.extend(more);
                    continue;
                }
                None => return exit("no NSIS first header in the first 8 MiB"),
                Some(k) if k >= 4 && (k - 4) % 512 == 0 => {
                    i = k;
                    break;
                }
                Some(k) => pos = k + 1,
            }
        }
        let fh = i - 4;
        let flags = u32le(&head, fh)?;
        let hdr_len = u32le(&head, fh + 20)?;
        if hdr_len as u64 > MAX_FILE {
            return too_big("a script header");
        }
        let mut total = u32le(&head, fh + 24)? as u64;
        let chunked = flags & 0x40 != 0;
        let mut n = Nsis {
            s,
            fh: fh as u64,
            total,
            chunked,
            solid: false,
            usize: 0,
            h: Vec::new(),
            data_base: 0,
            chunk_pos: Vec::new(),
            chunk_len: Vec::new(),
            end: 0,
            cache: BTreeMap::new(),
            blocks: Vec::new(),
            entsize: 28,
            unicode: false,
            codes: BTreeMap::new(),
        };
        // get(p, k): from the first MiB read, else from the source
        let get = |n: &mut Nsis, head: &[u8], p: u64, k: u64| -> Result<Vec<u8>> {
            if p + k <= head.len() as u64 {
                Ok(head[p as usize..(p + k) as usize].to_vec())
            } else {
                n.s.read(p, k)
            }
        };
        let fhsize: u64;
        if chunked {
            n.usize = u64le(&head, fh + 28)?;
            if n.usize != 0 {
                // solid: header chunks, then the data stream's chunks
                n.solid = true;
                let mut p = fh as u64 + 36;
                let mut h = Vec::new();
                let mut blk = Vec::new();
                let mut k = 0u64;
                while h.len() < 8 + hdr_len as usize {
                    k = le3(&get(&mut n, &head, p, 3)?);
                    if k == 0 {
                        break;
                    }
                    blk = get(&mut n, &head, p, 3 + k + 3)?;
                    // (one byte more than the header needs: a chunk making more fails below)
                    let room = 8 + hdr_len as u64 + 1 - h.len() as u64;
                    let c = blk.get(3..(3 + k as usize).min(blk.len())).unwrap_or(&[]);
                    h.extend(decompress(c, room)?);
                    p += 3 + k;
                }
                if h.len() < 8 || u64le(&h, 0)? != hdr_len as u64 || h.len() != 8 + hdr_len as usize
                {
                    return exit("unknown Unity 6 installer layout (header chunk)");
                }
                n.h = h[8..].to_vec();
                n.chunk_pos = vec![p];
                n.chunk_len = vec![le3(blk.get(3 + k as usize..).unwrap_or(&[]))];
                n.end = fh as u64 + 28 + total;
                fhsize = 36;
            } else {
                let w = u64le(&head, fh + 36)?;
                let k = w & 0x7fff_ffff_ffff_ffff;
                if k > MAX_FILE {
                    return too_big("a script header");
                }
                let blk = get(&mut n, &head, fh as u64 + 44, k)?;
                let h = if w >> 63 != 0 { dechunk(&blk)? } else { blk };
                n.h = h[..(hdr_len as usize).min(h.len())].to_vec();
                n.data_base = fh as u64 + 44 + k;
                fhsize = 36;
            }
        } else {
            let mut fs = 28u64;
            if head.get(fh + 28..fh + 36) == Some(&[0u8; 8][..])
                && head.get(fh + 39).is_some_and(|b| b & 0x80 != 0)
            {
                fs = 36; // large-installer variant: 64-bit data length
            }
            fhsize = fs;
            let w = u32le(&head, fh + fs as usize)?;
            if w & 0x8000_0000 == 0 {
                return exit("solid installer: use 7-Zip (this tool needs per-file blocks)");
            }
            let k = (w & 0x7fff_ffff) as u64;
            if k > MAX_FILE {
                return too_big("a script header");
            }
            let p = fh as u64 + fs + 4;
            let blk = get(&mut n, &head, p, k)?;
            // (its first hdr_len bytes are the header)
            let h = decompress(&blk, hdr_len as u64)?;
            n.h = h[..(hdr_len as usize).min(h.len())].to_vec();
            n.data_base = p + k;
        }
        total += fhsize - 28; // end of data relative to fh + 28 below
        n.total = total;
        n.blocks = (0..8)
            .map(|k| Ok((u32le(&n.h, 4 + 8 * k)?, u32le(&n.h, 8 + 8 * k)?)))
            .collect::<Result<_>>()?;
        let so = n.blocks[3].0 as usize;
        let (eo, en) = n.blocks[2];
        // (Python's integers: a table before the entries gives a negative size)
        let entsize = if en != 0 {
            (so as i64 - eo as i64).div_euclid(en as i64)
        } else {
            28
        };
        if entsize != 28 && entsize != 36 {
            return exit(format!("unknown script entry size {entsize}"));
        }
        n.entsize = entsize as usize;
        n.unicode = n.h.get(so..so + 2) == Some(&[0, 0][..]) && n.h.get(so + 3) == Some(&0);
        if n.unicode {
            let end = n.blocks[4].0 as i64;
            let stop = end - (end - so as i64).rem_euclid(2);
            let words: Vec<u16> = (so..stop.max(so as i64) as usize)
                .step_by(2)
                .map(|p| u16le(&n.h, p))
                .collect::<Result<_>>()?;
            let new = words.iter().filter(|&&w| (1..=4).contains(&w)).count();
            let old = words
                .iter()
                .filter(|&&w| (0xE000..=0xE003).contains(&w))
                .count();
            n.codes = if new > old {
                BTreeMap::from([(1, "lang"), (2, "shell"), (3, "var"), (4, "skip")])
            } else {
                BTreeMap::from([
                    (0xE000, "skip"),
                    (0xE001, "var"),
                    (0xE002, "shell"),
                    (0xE003, "lang"),
                ])
            };
        }
        Ok(n)
    }

    fn var(idx: usize) -> String {
        VARS.get(idx).map_or(format!("$_{idx}_"), |v| v.to_string())
    }

    /// A string of the script's string table.
    pub fn string(&self, off: u32) -> Result<String> {
        let h = &self.h;
        let so = self.blocks[3].0 as usize;
        let mut out = String::new();
        if self.unicode {
            let mut p = so + 2 * off as usize;
            loop {
                let c = u16le(h, p)?;
                p += 2;
                if c == 0 {
                    break;
                }
                if let Some(&code) = self.codes.get(&c) {
                    let v = u16le(h, p)?;
                    p += 2;
                    if code == "skip" {
                        out.push(char::from_u32(v as u32).unwrap_or('\u{fffd}'));
                        continue;
                    }
                    let idx = (((v >> 8) & 0x7f) as usize) << 7 | (v & 0x7f) as usize;
                    match code {
                        "var" => out.push_str(&Self::var(idx)),
                        "shell" => out.push_str(&format!("$SHELL{}", v & 0xff)),
                        _ => out.push_str(&format!("$LANG{idx}")),
                    }
                    continue;
                }
                out.push(char::from_u32(c as u32).unwrap_or('\u{fffd}'));
            }
        } else {
            let mut p = so + off as usize;
            let at = |p: usize| -> Result<u8> {
                h.get(p)
                    .copied()
                    .ok_or_else(|| Error::Fail("IndexError: index out of range".into()))
            };
            while at(p)? != 0 {
                let c = at(p)?;
                p += 1;
                if (252..=255).contains(&c) {
                    let (a, b) = (at(p)?, at(p + 1)?);
                    let v = a as u16 | (b as u16) << 8;
                    let idx = (a & 0x7f) as usize | ((b & 0x7f) as usize) << 7;
                    p += 2;
                    match c {
                        252 => {
                            out.push(char::from(at(p - 2)?));
                            p -= 1;
                        }
                        253 => out.push_str(&Self::var(idx)),
                        254 => out.push_str(&format!("$SHELL{}", v & 0xff)),
                        _ => out.push_str(&format!("$LANG{idx}")),
                    }
                    continue;
                }
                out.push(char::from(c));
            }
        }
        Ok(out)
    }

    /// The installer's files: (path with / separators, block offset, compressed size), in the
    /// script's order, each path once (its first offset).
    pub fn files(&self) -> Result<Vec<(String, u64, i64)>> {
        let (eo, en) = self.blocks[2];
        let mut outdir = "$OUTDIR".to_string();
        let mut seen: Vec<(String, u64)> = Vec::new();
        let mut index: std::collections::HashMap<String, usize> = std::collections::HashMap::new();
        let wide = self.entsize == 36;
        for k in 0..en as usize {
            let base = eo as usize + self.entsize * k;
            let w: Vec<u32> = (0..self.entsize / 4)
                .map(|j| u32le(&self.h, base + 4 * j))
                .collect::<Result<_>>()?;
            let (op, pa) = (w[0], &w[1..]);
            if op == EW_CREATEDIR && pa[1] != 0 {
                outdir = self.string(pa[0])?;
            } else if op == EW_EXTRACTFILE {
                let mut off = pa[2] as u64;
                if wide {
                    off |= (pa[3] as u64) << 32;
                }
                let name = self.string(pa[1])?;
                let path = if name.contains('\\') && name.starts_with('$') {
                    name
                } else {
                    format!("{outdir}\\{name}")
                };
                let path = path.replace('\\', "/");
                if !index.contains_key(&path) {
                    index.insert(path.clone(), seen.len());
                    seen.push((path, off));
                }
            }
        }
        let mut offs: Vec<u64> = seen.iter().map(|x| x.1).collect();
        offs.sort();
        offs.dedup();
        // (Python's integers: a header that says less data than there is gives a negative size)
        let end = if self.solid {
            self.usize as i128
        } else {
            (self.fh + 28 + self.total) as i128 - self.data_base as i128
        };
        let word = if self.chunked { 8 } else { 4 };
        Ok(seen
            .into_iter()
            .map(|(path, o)| {
                let j = offs.binary_search(&o).unwrap();
                let e = offs.get(j + 1).map_or(end, |&x| x as i128);
                (path, o, (e - o as i128 - word) as i64)
            })
            .collect())
    }

    /// Data chunk k of a solid Unity 6 installer, uncompressed (the last two kept).
    fn chunk(&mut self, k: u64) -> Result<Vec<u8>> {
        if let Some(c) = self.cache.get(&k) {
            return Ok(c.clone());
        }
        while self.chunk_pos.len() as u64 <= k {
            // walk the chunk lengths up to chunk k
            let (p, n) = (
                *self.chunk_pos.last().unwrap(),
                *self.chunk_len.last().unwrap(),
            );
            if n == 0 || p + 3 + n >= self.end {
                return exit("offset beyond the installer data");
            }
            let q = p + 3 + n;
            self.chunk_pos.push(q);
            let l = self.s.read(q, 3)?;
            self.chunk_len.push(le3(&l));
        }
        let (p, n) = (self.chunk_pos[k as usize], self.chunk_len[k as usize]);
        let blk = self
            .s
            .read(p + 3, if p + 6 + n <= self.end { n + 3 } else { n })?;
        if self.chunk_pos.len() as u64 == k + 1 && blk.len() as u64 == n + 3 {
            self.chunk_pos.push(p + 3 + n);
            self.chunk_len.push(le3(&blk[n as usize..]));
        }
        // (every chunk but the last makes CHUNK bytes: the offsets of the files count on it)
        let data = decompress(&blk[..(n as usize).min(blk.len())], CHUNK + 1)?;
        if data.len() as u64 > CHUNK {
            return exit(format!("chunk {k} makes more than {CHUNK} bytes"));
        }
        if self.cache.len() >= 2 {
            let first = *self.cache.keys().next().unwrap();
            self.cache.remove(&first);
        }
        self.cache.insert(k, data.clone());
        Ok(data)
    }

    fn stream(&mut self, mut off: u64, mut n: u64) -> Result<Vec<u8>> {
        let mut out = Vec::new();
        while n > 0 {
            let (k, r) = (off / CHUNK, off % CHUNK);
            let c = self.chunk(k)?;
            let piece = c.get(r as usize..).unwrap_or(&[]);
            let piece = &piece[..(n as usize).min(piece.len())];
            if piece.is_empty() {
                return exit(format!("short chunk {k}"));
            }
            out.extend_from_slice(piece);
            off += piece.len() as u64;
            n -= piece.len() as u64;
        }
        Ok(out)
    }

    /// The file whose block is at `off` (files()'s offset), uncompressed.
    pub fn extract(&mut self, off: u64) -> Result<Vec<u8>> {
        // (the offset and the sizes are the installer's: a file is at most MAX_FILE bytes)
        let at = self
            .data_base
            .checked_add(off)
            .filter(|a| a.checked_add(16).is_some())
            .ok_or_else(|| Error::Exit("offset beyond the installer data".into()))?;
        if self.solid {
            let n = u64le(&self.stream(off, 8)?, 0)?;
            if n > MAX_FILE {
                return too_big("a file");
            }
            return self.stream(off + 8, n);
        }
        if self.chunked {
            let w = u64le(&self.s.read(at, 8)?, 0)?;
            let n = w & 0x7fff_ffff_ffff_ffff;
            if n > MAX_FILE {
                return too_big("a file");
            }
            let blk = self.s.read(at + 8, n)?;
            return if w >> 63 != 0 { dechunk(&blk) } else { Ok(blk) };
        }
        let w = u32le(&self.s.read(at, 4)?, 0)?;
        let n = (w & 0x7fff_ffff) as u64;
        if n > MAX_FILE {
            return too_big("a file");
        }
        let blk = self.s.read(at + 4, n)?;
        if w & 0x8000_0000 != 0 {
            whole(&blk)
        } else {
            Ok(blk)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    const STRINGS: &[u8] = b"\0$INSTDIR\\Data\0a.txt\0";

    /// A made-up installer of one stored file (a.txt in $INSTDIR\Data): the first header at 0,
    /// the script header (`edit` changes it first) deflated, the file's block. `total`: the first
    /// header's data length, else the real one.
    fn installer(edit: impl FnOnce(&mut Vec<u8>), total: Option<u32>) -> Vec<u8> {
        let (eo, so) = (68u32, 68 + 2 * 28);
        let lo = so + STRINGS.len() as u32;
        let mut h = vec![0u8; 4];
        for (o, n) in [
            (0, 0),
            (0, 0),
            (eo, 2),
            (so, 0),
            (lo, 0),
            (lo, 0),
            (lo, 0),
            (lo, 0),
        ] {
            h.extend(o.to_le_bytes());
            h.extend((n as u32).to_le_bytes());
        }
        for w in [
            [EW_CREATEDIR, 1, 1, 0, 0, 0, 0],
            [EW_EXTRACTFILE, 0, 15, 0, 0, 0, 0],
        ] {
            for x in w {
                h.extend(x.to_le_bytes());
            }
        }
        h.extend(STRINGS);
        edit(&mut h);
        let mut e = flate2::write::DeflateEncoder::new(Vec::new(), flate2::Compression::default());
        e.write_all(&h).unwrap();
        let hb = e.finish().unwrap();
        let mut out = 0u32.to_le_bytes().to_vec();
        out.extend(FH_SIG);
        out.extend((h.len() as u32).to_le_bytes());
        out.extend(0u32.to_le_bytes());
        out.extend((hb.len() as u32 | 0x8000_0000).to_le_bytes());
        out.extend(&hb);
        out.extend(5u32.to_le_bytes());
        out.extend(b"hello");
        let t = total.unwrap_or((out.len() - 28) as u32);
        out[24..28].copy_from_slice(&t.to_le_bytes());
        out
    }

    fn open(data: &[u8], tag: &str) -> Result<Nsis> {
        let p = std::env::temp_dir().join(format!("pkgzip-nsis-{tag}-{}", std::process::id()));
        std::fs::write(&p, data).unwrap();
        let n = Nsis::open(p.to_str().unwrap(), Box::new(|_| {}));
        let _ = std::fs::remove_file(&p);
        n
    }

    #[test]
    fn made_up_installer() {
        let mut n = open(&installer(|_| {}, None), "ok").unwrap();
        assert_eq!(
            n.files().unwrap(),
            vec![("$INSTDIR/Data/a.txt".to_string(), 0, 5)]
        );
        assert_eq!(n.extract(0).unwrap(), b"hello");
    }

    #[test]
    fn damaged_headers_are_errors() {
        // (the fuzz target's findings: subtractions of the header's numbers, as Python's
        // integers make them, never an overflow) entries after the strings: a negative size
        let i = installer(|h| h[20..24].copy_from_slice(&200u32.to_le_bytes()), None);
        let e = open(&i, "ent").err().unwrap().to_string();
        assert_eq!(e, "unknown script entry size -38");
        // a first header that says less data than follows it: a negative file size
        let n = open(&installer(|_| {}, Some(0)), "total").unwrap();
        assert!(n.files().unwrap()[0].2 < 0);
        // Unicode strings whose table ends before it begins: no strings to look at
        let i = installer(
            |h| {
                h[125] = 0;
                h[127] = 0;
                h[36..40].copy_from_slice(&0u32.to_le_bytes());
            },
            None,
        );
        assert!(open(&i, "uni").is_ok());
        // a script header of 4 GB: refused before it is read
        let mut i = installer(|_| {}, None);
        i[20..24].copy_from_slice(&u32::MAX.to_le_bytes());
        let e = open(&i, "len").err().unwrap().to_string();
        assert!(e.contains("a script header of more than"), "{e}");
        // Unity 6's solid layout, cut short in its first header chunk
        let mut i = installer(|_| {}, None);
        i[..4].copy_from_slice(&0x40u32.to_le_bytes());
        i.truncate(38);
        assert!(open(&i, "cut").is_err());
        // a file's offset that wraps around the data's
        let mut n = open(&installer(|_| {}, None), "wrap").unwrap();
        assert!(n.extract(u64::MAX - 8).is_err());
    }
}
