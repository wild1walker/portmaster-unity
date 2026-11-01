//! A serialized file's target platform set in place (android_data.py): in a plain file one
//! word; in a bundle the word's bytes in their blocks, so nothing moves: a literal byte of an LZ4
//! block changed, the matches that copied it pointed elsewhere or the block re-encoded to the
//! same length, else (rarely) that block rewritten as LZ4 HC and the bundle with it. Its own
//! small bundle reader (android_data.Bundle), whose rules are the ones these functions use.
use crate::bin::Writer;
use crate::compress;
use crate::error::{bad, Error, Result};
use crate::serialized::platform_offset;
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

/// BuildTarget.Android.
pub const ANDROID: u32 = 13;

/// android_data.Bundle: a minimal UnityFS (format 6/7) reader.
#[derive(Clone, Debug)]
pub struct MiniBundle {
    pub format: u32,
    pub uv: Vec<u8>,
    pub ur: Vec<u8>,
    pub size: i64,
    pub ci_size: u32,
    pub ui_size: u32,
    pub flags: u32,
    pub header_end: u64,
    pub data_start: u64,
    pub hash: Vec<u8>,
    /// [usize, csize, flags]
    pub blocks: Vec<[u32; 3]>,
    /// (offset, size, flags, name)
    pub nodes: Vec<(i64, i64, u32, String)>,
    /// (file offset, uncompressed offset) of each block
    pub block_pos: Vec<(u64, u64)>,
}

fn cstr(b: &[u8], p: usize) -> Result<(&[u8], usize)> {
    let e = b
        .get(p..)
        .and_then(|r| r.iter().position(|&c| c == 0))
        .ok_or_else(|| Error::Format("substring not found".into()))?;
    Ok((&b[p..p + e], p + e + 1))
}

fn be_u32(b: &[u8], p: usize) -> Result<u32> {
    Ok(u32::from_be_bytes(
        b.get(p..p + 4)
            .ok_or_else(|| Error::Format("short".into()))?
            .try_into()
            .unwrap(),
    ))
}

fn be_i64(b: &[u8], p: usize) -> Result<i64> {
    Ok(i64::from_be_bytes(
        b.get(p..p + 8)
            .ok_or_else(|| Error::Format("short".into()))?
            .try_into()
            .unwrap(),
    ))
}

fn read_n(f: &mut File, n: usize) -> Result<Vec<u8>> {
    let mut v = Vec::with_capacity(n);
    f.take(n as u64).read_to_end(&mut v)?;
    Ok(v)
}

fn lzma_block(comp: &[u8], usize: usize) -> Result<Vec<u8>> {
    // (LZMADecompressor.decompress(comp[5:], max_length=usize): up to usize bytes)
    let mut out = Vec::with_capacity(usize);
    compress::lzma_decompress_to(&mut &comp[..], usize as u64, &mut out)?;
    Ok(out)
}

impl MiniBundle {
    pub fn open(path: &Path) -> Result<Self> {
        let mut f = File::open(path)?;
        let head = read_n(&mut f, 4096)?;
        let (sig, mut p) = cstr(&head, 0)?;
        if sig != b"UnityFS" {
            return bad("not a UnityFS bundle");
        }
        let format = be_u32(&head, p)?;
        p += 4;
        let (uv, q) = cstr(&head, p)?;
        let (ur, q) = cstr(&head, q)?;
        p = q;
        let size = be_i64(&head, p)?;
        let ci_size = be_u32(&head, p + 8)?;
        let ui_size = be_u32(&head, p + 12)?;
        let flags = be_u32(&head, p + 16)?;
        p += 20;
        if format >= 7 {
            p = (p + 15) & !15;
        }
        let header_end = p as u64;
        let (ci, data_start) = if flags & 0x80 != 0 {
            f.seek(SeekFrom::Start((size - ci_size as i64).max(0) as u64))?;
            (read_n(&mut f, ci_size as usize)?, p as u64)
        } else {
            f.seek(SeekFrom::Start(p as u64))?;
            let mut ds = p as u64 + ci_size as u64;
            if flags & 0x200 != 0 {
                ds = (ds + 15) & !15;
            }
            (read_n(&mut f, ci_size as usize)?, ds)
        };
        let ub = match flags & 0x3F {
            0 => ci,
            2 | 3 => compress::lz4_decompress(&ci, ui_size as usize)?,
            1 => lzma_block(&ci, ui_size as usize)?,
            c => return bad(format!("blocks info compression {c}")),
        };
        let hash = ub.get(..16).unwrap_or(&ub).to_vec();
        let n = be_u32(&ub, 16)? as i32;
        let mut blocks = Vec::new();
        let mut p = 20;
        for _ in 0..n.max(0) {
            let u = be_u32(&ub, p)?;
            let c = be_u32(&ub, p + 4)?;
            let fl = u16::from_be_bytes(
                ub.get(p + 8..p + 10)
                    .ok_or_else(|| Error::Format("short".into()))?
                    .try_into()
                    .unwrap(),
            ) as u32;
            blocks.push([u, c, fl]);
            p += 10;
        }
        let nn = be_u32(&ub, p)? as i32;
        p += 4;
        let mut nodes = Vec::new();
        for _ in 0..nn.max(0) {
            let off = be_i64(&ub, p)?;
            let sz = be_i64(&ub, p + 8)?;
            let fl = be_u32(&ub, p + 16)?;
            p += 20;
            let (name, q) = cstr(&ub, p)?;
            p = q;
            nodes.push((off, sz, fl, String::from_utf8_lossy(name).into_owned()));
        }
        let mut block_pos = Vec::new();
        let (mut fpos, mut upos) = (data_start, 0u64);
        for b in &blocks {
            block_pos.push((fpos, upos));
            fpos += b[1] as u64;
            upos += b[0] as u64;
        }
        Ok(MiniBundle {
            format,
            uv: uv.to_vec(),
            ur: ur.to_vec(),
            size,
            ci_size,
            ui_size,
            flags,
            header_end,
            data_start,
            hash,
            blocks,
            nodes,
            block_pos,
        })
    }

    pub fn block_of(&self, uoff: u64) -> Result<usize> {
        for (i, b) in self.blocks.iter().enumerate() {
            let upos = self.block_pos[i].1;
            if upos <= uoff && uoff < upos + b[0] as u64 {
                return Ok(i);
            }
        }
        bad("offset beyond data")
    }

    /// (raw, plain) bytes of block i.
    pub fn read_block(&self, f: &mut File, i: usize) -> Result<(Vec<u8>, Vec<u8>)> {
        let [usz, csz, bfl] = self.blocks[i];
        f.seek(SeekFrom::Start(self.block_pos[i].0))?;
        let raw = read_n(f, csz as usize)?;
        let plain = match bfl & 0x3F {
            0 => raw.clone(),
            2 | 3 => compress::lz4_decompress(&raw, usz as usize)?,
            1 => lzma_block(&raw, usz as usize)?,
            c => return bad(format!("block compression {c} not handled")),
        };
        Ok((raw, plain))
    }

    pub fn read_plain(&self, f: &mut File, mut uoff: u64, mut n: usize) -> Result<Vec<u8>> {
        let mut out = Vec::new();
        while n > 0 {
            let i = self.block_of(uoff)?;
            let (_, plain) = self.read_block(f, i)?;
            let s = (uoff - self.block_pos[i].1) as usize;
            let take = &plain[s.min(plain.len())..(s + n).min(plain.len())];
            out.extend_from_slice(take);
            n -= take.len();
            uoff += take.len() as u64;
            if take.is_empty() {
                break;
            }
        }
        Ok(out)
    }
}

/// patch_sf_platform: sets a plain serialized file's target platform; returns the old one.
pub fn patch_sf_platform(path: &Path, platform: u32) -> Result<u32> {
    let mut f = OpenOptions::new().read(true).write(true).open(path)?;
    let head = read_n(&mut f, 128)?;
    let (off, big, cur, _) = platform_offset(&head)?;
    if cur != platform {
        f.seek(SeekFrom::Start(off as u64))?;
        f.write_all(&if big {
            platform.to_be_bytes()
        } else {
            platform.to_le_bytes()
        })?;
    }
    Ok(cur)
}

/// bundle_platforms: (node, target platform) of each serialized file (flag 4) in a bundle.
pub fn bundle_platforms(path: &Path) -> Result<Vec<(String, u32)>> {
    let b = MiniBundle::open(path)?;
    let mut f = File::open(path)?;
    let mut res = Vec::new();
    for (off, _, fl, name) in &b.nodes {
        if fl & 4 != 0 {
            let head = b.read_plain(&mut f, *off as u64, 128)?;
            res.push((name.clone(), platform_offset(&head)?.2));
        }
    }
    Ok(res)
}

/// patch_bundle_platform: every serialized file's target platform in a bundle, set in place.
/// Returns (node, old platform, method: already, literal, reencode or rewrite).
pub fn patch_bundle_platform(
    path: &Path,
    platform: u32,
) -> Result<Vec<(String, u32, &'static str)>> {
    let b = MiniBundle::open(path)?;
    let mut done = Vec::new();
    let mut f = OpenOptions::new().read(true).write(true).open(path)?;
    for (off, _, fl, name) in &b.nodes {
        if fl & 4 == 0 {
            continue;
        }
        let head = b.read_plain(&mut f, *off as u64, 128)?;
        let (poff, big, cur, _) = platform_offset(&head)?;
        if cur == platform {
            done.push((name.clone(), cur, "already"));
            continue;
        }
        let (new, old) = if big {
            (platform.to_be_bytes(), cur.to_be_bytes())
        } else {
            (platform.to_le_bytes(), cur.to_le_bytes())
        };
        let mut method = "literal";
        for k in 0..4 {
            if old[k] == new[k] {
                continue;
            }
            let u = *off as u64 + poff as u64 + k as u64;
            let i = b.block_of(u)?;
            let (raw, plain) = b.read_block(&mut f, i)?;
            let rel = (u - b.block_pos[i].1) as usize;
            let c = b.blocks[i][2] & 0x3F;
            let pos;
            if c == 0 {
                pos = rel;
            } else if c == 1 {
                drop(f);
                rewrite_bundle_block(path, &b, i, rel, new[k])?;
                done.push((name.clone(), cur, "rewrite"));
                done.extend(patch_bundle_platform(path, platform)?);
                return Ok(done);
            } else {
                match lz4_literal_pos(&raw, rel)? {
                    None => {
                        drop(f);
                        rewrite_bundle_block(path, &b, i, rel, new[k])?;
                        done.push((name.clone(), cur, "rewrite"));
                        done.extend(patch_bundle_platform(path, platform)?);
                        return Ok(done);
                    }
                    Some(p) => {
                        let mut patched = raw.clone();
                        patched[p] = new[k];
                        let mut expect = plain.clone();
                        expect[rel] = new[k];
                        if compress::lz4_decompress(&patched, b.blocks[i][0] as usize)? != expect {
                            let enc = match lz4_repoint(&patched, &expect, 64)? {
                                Some(e) => Some(e),
                                None => lz4_exact(&expect, raw.len())?,
                            };
                            match enc {
                                None => {
                                    drop(f);
                                    rewrite_bundle_block(path, &b, i, rel, new[k])?;
                                    done.push((name.clone(), cur, "rewrite"));
                                    done.extend(patch_bundle_platform(path, platform)?);
                                    return Ok(done);
                                }
                                Some(enc) => {
                                    f.seek(SeekFrom::Start(b.block_pos[i].0))?;
                                    f.write_all(&enc)?;
                                    method = "reencode";
                                    continue;
                                }
                            }
                        }
                        pos = p;
                    }
                }
            }
            f.seek(SeekFrom::Start(b.block_pos[i].0 + pos as u64))?;
            f.write_all(&[new[k]])?;
        }
        done.push((name.clone(), cur, method));
    }
    Ok(done)
}

/// rewrite_bundle_block: block i re-encoded (LZ4 HC) with plaintext[rel] = value and the bundle
/// rewritten (the other blocks copied as they are, the header and blocks info with the new sizes).
pub fn rewrite_bundle_block(
    path: &Path,
    b: &MiniBundle,
    i: usize,
    rel: usize,
    value: u8,
) -> Result<()> {
    let mut f = File::open(path)?;
    let (_, plain) = b.read_block(&mut f, i)?;
    let mut p = plain;
    p[rel] = value;
    let newraw = compress::lz4hc(&p, 9)?;
    let mut blocks = b.blocks.clone();
    blocks[i][1] = newraw.len() as u32;
    blocks[i][2] = (blocks[i][2] & !0x3F) | 3;
    let mut bi = Writer::new(true);
    bi.bytes(&b.hash);
    bi.i32(blocks.len() as i32);
    for bl in &blocks {
        bi.u32(bl[0]);
        bi.u32(bl[1]);
        bi.u16(bl[2] as u16);
    }
    bi.i32(b.nodes.len() as i32);
    for (off, sz, fl, name) in &b.nodes {
        bi.i64(*off);
        bi.i64(*sz);
        bi.u32(*fl);
        bi.cstr(name.as_bytes());
    }
    let ci = compress::lz4hc(&bi.buf, 9)?;
    let flags = (b.flags & !0x3F) | 3;
    f.seek(SeekFrom::Start(0))?;
    let mut head = read_n(&mut f, b.header_end as usize)?;
    let tmp = path.with_file_name(format!(
        "{}.tmp",
        path.file_name().unwrap().to_string_lossy()
    ));
    {
        let mut o = std::io::BufWriter::new(File::create(&tmp)?);
        o.write_all(&vec![0u8; b.header_end as usize])?;
        let mut pos = b.header_end;
        if flags & 0x80 == 0 {
            o.write_all(&ci)?;
            pos += ci.len() as u64;
            if flags & 0x200 != 0 {
                let pad = (16 - pos % 16) % 16;
                o.write_all(&vec![0u8; pad as usize])?;
                pos += pad;
            }
        }
        for (j, bl) in b.blocks.iter().enumerate() {
            if j == i {
                o.write_all(&newraw)?;
                pos += newraw.len() as u64;
            } else {
                f.seek(SeekFrom::Start(b.block_pos[j].0))?;
                let d = read_n(&mut f, bl[1] as usize)?;
                o.write_all(&d)?;
                pos += d.len() as u64;
            }
        }
        if flags & 0x80 != 0 {
            o.write_all(&ci)?;
            pos += ci.len() as u64;
        }
        let total = pos;
        let p0 = 8 + 4 + b.uv.len() + 1 + b.ur.len() + 1;
        let mut s = Writer::new(true);
        s.i64(total as i64);
        s.u32(ci.len() as u32);
        s.u32(bi.len() as u32);
        s.u32(flags);
        if head.len() < p0 + 20 {
            return bad("bundle header shorter than its fields");
        }
        head[p0..p0 + 20].copy_from_slice(&s.buf);
        o.seek(SeekFrom::Start(0))?;
        o.write_all(&head)?;
        o.flush()?;
    }
    std::fs::rename(&tmp, path)?;
    Ok(())
}

// ------------------------------------------------------------------------- raw LZ4 blocks

fn at(comp: &[u8], i: usize) -> Result<u8> {
    comp.get(i)
        .copied()
        .ok_or_else(|| Error::Format("index out of range".into()))
}

fn ext_len(comp: &[u8], ip: &mut usize, mut n: usize) -> Result<usize> {
    loop {
        let b = at(comp, *ip)?;
        *ip += 1;
        n += b as usize;
        if b != 255 {
            return Ok(n);
        }
    }
}

/// lz4_literal_pos: the position in a raw LZ4 block of the literal byte that becomes
/// plaintext[target], or None when that byte comes from a match.
pub fn lz4_literal_pos(comp: &[u8], target: usize) -> Result<Option<usize>> {
    let (mut ip, mut op) = (0usize, 0usize);
    let n = comp.len();
    while ip < n {
        let token = comp[ip];
        ip += 1;
        let mut lit = (token >> 4) as usize;
        if lit == 15 {
            lit = ext_len(comp, &mut ip, lit)?;
        }
        if op <= target && target < op + lit {
            return Ok(Some(ip + (target - op)));
        }
        ip += lit;
        op += lit;
        if ip >= n {
            break;
        }
        ip += 2;
        let mut ml = (token & 15) as usize;
        if ml == 15 {
            ml = ext_len(comp, &mut ip, ml)?;
        }
        ml += 4;
        if op <= target && target < op + ml {
            return Ok(None);
        }
        op += ml;
    }
    Ok(None)
}

/// A sequence: literals, match offset (None for the last), match length, the match's bytes.
#[derive(Clone, Debug)]
struct Seq {
    lits: Vec<u8>,
    off: Option<usize>,
    ml: usize,
    mbytes: Vec<u8>,
}

/// _lz4_sequences: a raw LZ4 block's sequences and plaintext.
fn lz4_sequences(comp: &[u8]) -> Result<(Vec<Seq>, Vec<u8>)> {
    let mut seqs = Vec::new();
    let mut out: Vec<u8> = Vec::new();
    let (mut ip, n) = (0usize, comp.len());
    while ip < n {
        let token = comp[ip];
        ip += 1;
        let mut lit = (token >> 4) as usize;
        if lit == 15 {
            lit = ext_len(comp, &mut ip, lit)?;
        }
        let lits = comp[ip.min(n)..(ip + lit).min(n)].to_vec();
        ip += lit;
        out.extend_from_slice(&lits);
        if ip >= n {
            seqs.push(Seq {
                lits,
                off: None,
                ml: 0,
                mbytes: Vec::new(),
            });
            break;
        }
        let off = at(comp, ip)? as usize | (at(comp, ip + 1)? as usize) << 8;
        ip += 2;
        let mut ml = (token & 15) as usize;
        if ml == 15 {
            ml = ext_len(comp, &mut ip, ml)?;
        }
        ml += 4;
        if off > out.len() || off == 0 {
            return bad("LZ4 match before the start");
        }
        let start = out.len() - off;
        for j in 0..ml {
            let c = out[start + j];
            out.push(c);
        }
        let mbytes = out[out.len() - ml..].to_vec();
        seqs.push(Seq {
            lits,
            off: Some(off),
            ml,
            mbytes,
        });
    }
    Ok((seqs, out))
}

fn ext(n: usize) -> usize {
    if n < 15 {
        0
    } else {
        1 + (n - 15) / 255
    }
}

fn seq_size(q: &Seq) -> usize {
    let mut s = 1 + ext(q.lits.len()) + q.lits.len();
    if q.off.is_some() {
        s += 2 + ext(q.ml - 4);
    }
    s
}

fn lz4_emit(seqs: &[Seq]) -> Vec<u8> {
    let mut o = Vec::new();
    let put_ext = |o: &mut Vec<u8>, n: usize| {
        let mut r = n - 15;
        while r >= 255 {
            o.push(255);
            r -= 255;
        }
        o.push(r as u8);
    };
    for q in seqs {
        let ml = if q.off.is_some() { q.ml - 4 } else { 0 };
        let l = q.lits.len();
        o.push(((l.min(15) as u8) << 4) | if q.off.is_some() { ml.min(15) as u8 } else { 0 });
        if l >= 15 {
            put_ext(&mut o, l);
        }
        o.extend_from_slice(&q.lits);
        if let Some(off) = q.off {
            o.push((off & 255) as u8);
            o.push((off >> 8) as u8);
            if ml >= 15 {
                put_ext(&mut o, ml);
            }
        }
    }
    o
}

/// _lz4_decode_trace: plaintext and (output start, match length, position of the offset bytes)
/// of each match; None for a match reaching before the start.
#[allow(clippy::type_complexity)]
fn lz4_decode_trace(comp: &[u8]) -> Result<Option<(Vec<u8>, Vec<(usize, usize, usize)>)>> {
    let mut out: Vec<u8> = Vec::new();
    let mut matches = Vec::new();
    let (mut ip, n) = (0usize, comp.len());
    while ip < n {
        let token = comp[ip];
        ip += 1;
        let mut lit = (token >> 4) as usize;
        if lit == 15 {
            lit = ext_len(comp, &mut ip, lit)?;
        }
        out.extend_from_slice(&comp[ip.min(n)..(ip + lit).min(n)]);
        ip += lit;
        if ip >= n {
            break;
        }
        let opos = ip;
        let off = at(comp, ip)? as usize | (at(comp, ip + 1)? as usize) << 8;
        ip += 2;
        let mut ml = (token & 15) as usize;
        if ml == 15 {
            ml = ext_len(comp, &mut ip, ml)?;
        }
        ml += 4;
        let start = out.len();
        if off > start {
            return Ok(None);
        }
        let s0 = start - off;
        for j in 0..ml {
            let c = out[s0 + j];
            out.push(c);
        }
        matches.push((start, ml, opos));
    }
    Ok(Some((out, matches)))
}

/// bytes.rfind(target, lo, hi): the last place target lies wholly within [lo, hi).
fn rfind(hay: &[u8], target: &[u8], lo: usize, hi: usize) -> Option<usize> {
    let hi = hi.min(hay.len());
    if target.len() > hi || lo > hi - target.len() {
        return None;
    }
    (lo..=hi - target.len())
        .rev()
        .find(|&q| &hay[q..q + target.len()] == target)
}

/// lz4_repoint: a raw LZ4 block whose literal was patched made to decode to `expect` without
/// changing its length: each match that copied the old byte gets the offset of an identical
/// earlier run.
pub fn lz4_repoint(comp: &[u8], expect: &[u8], tries: usize) -> Result<Option<Vec<u8>>> {
    let mut comp = comp.to_vec();
    for _ in 0..tries {
        let Some((out, matches)) = lz4_decode_trace(&comp)? else {
            return Ok(None);
        };
        if out.len() != expect.len() {
            return Ok(None);
        }
        if out == expect {
            return Ok(Some(comp));
        }
        let badi = (0..out.len()).find(|&i| out[i] != expect[i]).unwrap();
        let Some(&(start, ml, opos)) = matches.iter().find(|m| m.0 <= badi && badi < m.0 + m.1)
        else {
            return Ok(None);
        };
        let target = &expect[start..(start + ml).min(expect.len())];
        let q = match rfind(expect, target, start.saturating_sub(65535), start + ml - 1) {
            Some(q) => q,
            None => return Ok(None),
        };
        if q >= start || start - q > 65535 {
            return Ok(None);
        }
        comp[opos] = ((start - q) & 255) as u8;
        comp[opos + 1] = ((start - q) >> 8) as u8;
    }
    Ok(None)
}

/// lz4_exact: a raw LZ4 block of `plain` exactly `size` bytes long, or None: compressed tighter
/// than needed (HC 12, 9, 4, then fast), then matches turned back into literals until it fits.
pub fn lz4_exact(plain: &[u8], size: usize) -> Result<Option<Vec<u8>>> {
    for (hc, level) in [(true, 12), (true, 9), (true, 4), (false, 0)] {
        let c = if hc {
            compress::lz4hc(plain, level)?
        } else {
            compress::lz4_fast(plain)?
        };
        if c.len() > size {
            continue;
        }
        let (mut seqs, check) = lz4_sequences(&c)?;
        if check != plain {
            continue;
        }
        let mut deficit = size - c.len();
        for _ in 0..8 {
            if deficit == 0 {
                break;
            }
            let mut out = Vec::new();
            let mut cur = seqs[0].clone();
            for nxt in seqs[1..].iter() {
                if deficit > 0 && cur.off.is_some() {
                    let mut lits = cur.lits.clone();
                    lits.extend_from_slice(&cur.mbytes);
                    lits.extend_from_slice(&nxt.lits);
                    let merged = Seq {
                        lits,
                        off: nxt.off,
                        ml: nxt.ml,
                        mbytes: nxt.mbytes.clone(),
                    };
                    let d = seq_size(&merged) as isize
                        - seq_size(&cur) as isize
                        - seq_size(nxt) as isize;
                    if 0 < d && d as usize <= deficit {
                        cur = merged;
                        deficit -= d as usize;
                        continue;
                    }
                }
                out.push(cur);
                cur = nxt.clone();
            }
            out.push(cur);
            seqs = out;
        }
        if deficit > 0 {
            continue;
        }
        let res = lz4_emit(&seqs);
        if res.len() == size
            && compress::lz4_decompress(&res, plain.len()).ok().as_deref() == Some(plain)
        {
            return Ok(Some(res));
        }
    }
    Ok(None)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Vec<u8> {
        let mut v = Vec::new();
        for i in 0..4000u32 {
            v.extend_from_slice(format!("field{} value {} ", i % 37, i % 11).as_bytes());
        }
        v
    }

    #[test]
    fn sequences_round_trip() {
        let p = sample();
        let c = compress::lz4hc(&p, 9).unwrap();
        let (seqs, out) = lz4_sequences(&c).unwrap();
        assert_eq!(out, p);
        assert_eq!(lz4_emit(&seqs), c);
        assert_eq!(seqs.iter().map(seq_size).sum::<usize>(), c.len());
    }

    #[test]
    fn exact_sizes() {
        let p = sample();
        let c = compress::lz4hc(&p, 9).unwrap();
        let e = lz4_exact(&p, c.len()).unwrap().expect("its own size fits");
        assert_eq!(e.len(), c.len());
        let mut fitted = 0;
        for extra in [1, 2, 3, 5, 40] {
            if let Some(e) = lz4_exact(&p, c.len() + extra).unwrap() {
                assert_eq!(e.len(), c.len() + extra);
                assert_eq!(compress::lz4_decompress(&e, p.len()).unwrap(), p);
                fitted += 1;
            }
        }
        assert!(fitted > 0);
        assert!(lz4_exact(&p, 10).unwrap().is_none());
    }

    #[test]
    fn literal_and_repoint() {
        let p = sample();
        let c = compress::lz4hc(&p, 9).unwrap();
        // the first bytes are literals: their positions after the token (and its length bytes)
        let at0 = lz4_literal_pos(&c, 0).unwrap().unwrap();
        assert_eq!(c[at0..at0 + 4], p[..4]);
        assert_eq!(lz4_literal_pos(&c, 3).unwrap(), Some(at0 + 3));
        // a byte far into the text comes from a match
        assert_eq!(lz4_literal_pos(&c, p.len() - 100).unwrap(), None);
        let mut expect = p.clone();
        expect[2] ^= 0x20;
        let mut patched = c.clone();
        patched[at0 + 2] ^= 0x20;
        let r = lz4_repoint(&patched, &expect, 64).unwrap();
        if let Some(r) = r {
            assert_eq!(r.len(), c.len());
            assert_eq!(compress::lz4_decompress(&r, p.len()).unwrap(), expect);
        }
    }
}
