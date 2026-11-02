//! il2cpp.py's reads of Unity's and Google's archives: HTTP range requests (`_get`), neighbouring
//! pieces read with one request (`_spans`), six requests at once (`_parallel`); files of an NSIS
//! installer (`nsis_fetch`) and members of a zip (`RemoteZip`).
use crate::util::{self, at, join};
use crate::{fail, log, Error, Result};
use pkgzip::nsis::Nsis;
use std::io::Read;

fn is_url(src: &str) -> bool {
    src.starts_with("http://") || src.starts_with("https://")
}

/// _get: `n` bytes at `off` of a URL, by a range request; 6 tries (3, 6, 9 ... s apart), the
/// status must be 206 and the length right.
pub fn get(url: &str, off: u64, n: u64) -> Result<Vec<u8>> {
    if n == 0 {
        return Ok(Vec::new());
    }
    let tries = 6;
    let mut attempt = 0;
    loop {
        let r = get_once(url, off, n);
        match r {
            Ok(d) => return Ok(d),
            Err(e) => {
                if attempt == tries - 1 {
                    return fail(e);
                }
                log(&format!("  retry {} at {off}: {e}", attempt + 1));
                std::thread::sleep(std::time::Duration::from_secs(3 * (attempt + 1)));
                attempt += 1;
            }
        }
    }
}

fn get_once(url: &str, off: u64, n: u64) -> std::result::Result<Vec<u8>, String> {
    let mut c = pkgzip::http::Client::default();
    c.max_body = n.saturating_add(1 << 16);
    let rng = format!("bytes={}-{}", off, off + n - 1);
    let r = c
        .get_following(
            url,
            &[("Range", &rng), ("User-Agent", "Python-urllib/3.11")],
            10,
        )
        .map_err(|e| format!("<urlopen error {e}>"))?;
    if r.status >= 400 {
        return Err(format!("HTTP Error {}", r.status));
    }
    if r.status != 206 {
        return Err(format!(
            "server ignored the Range request (status {})",
            r.status
        ));
    }
    if r.body.len() as u64 != n {
        return Err(format!("short read {} of {n}", r.body.len()));
    }
    Ok(r.body)
}

/// _local: `n` bytes at `off` of a file (fewer at its end).
pub fn local(path: &str, off: u64, n: u64) -> Result<Vec<u8>> {
    use std::io::{Seek, SeekFrom};
    let mut f = std::fs::File::open(path).map_err(at(path))?;
    f.seek(SeekFrom::Start(off)).map_err(at(path))?;
    let mut out = Vec::new();
    f.take(n).read_to_end(&mut out).map_err(at(path))?;
    Ok(out)
}

/// A URL's or a file's bytes.
pub fn read_at(src: &str, off: u64, n: u64) -> Result<Vec<u8>> {
    if is_url(src) {
        get(src, off, n)
    } else {
        local(src, off, n)
    }
}

/// One read: (start, end, [(offset, size, key)]) of the pieces it covers.
pub type Span<K> = (i64, i64, Vec<(i64, i64, K)>);

/// _spans: pieces sorted, those within `gap` bytes of each other read together (one read at most
/// `limit` bytes).
pub fn spans<K: Clone>(mut items: Vec<(i64, i64, K)>, gap: i64, limit: i64) -> Vec<Span<K>> {
    items.sort_by_key(|x| (x.0, x.1));
    let mut out: Vec<Span<K>> = Vec::new();
    for (off, size, key) in items {
        if let Some(last) = out.last_mut() {
            if off - last.1 <= gap && off + size - last.0 <= limit {
                last.1 = last.1.max(off + size);
                last.2.push((off, size, key));
                continue;
            }
        }
        out.push((off, off + size, vec![(off, size, key)]));
    }
    out
}

/// _parallel: `f` of each job on six threads, the results taken in the jobs' order (a batch of
/// six at a time: the Python's pool runs ahead; what is read is the same).
pub fn parallel<J: Sync, R: Send>(
    jobs: &[J],
    f: impl Fn(&J) -> Result<R> + Sync,
    mut each: impl FnMut(R) -> Result<()>,
) -> Result<()> {
    for batch in jobs.chunks(6) {
        let results: Vec<Result<R>> = std::thread::scope(|s| {
            let hs: Vec<_> = batch.iter().map(|j| s.spawn(|| f(j))).collect();
            hs.into_iter()
                .map(|h| h.join().unwrap_or_else(|_| fail("a read thread panicked")))
                .collect()
        });
        for r in results {
            each(r?)?;
        }
    }
    Ok(())
}

fn u32le(b: &[u8], p: usize) -> Result<u32> {
    b.get(p..p + 4)
        .map(|s| u32::from_le_bytes(s.try_into().unwrap()))
        .ok_or_else(|| Error::Fail("struct.error: unpack_from requires a buffer of 4 bytes".into()))
}

fn u16le(b: &[u8], p: usize) -> Result<u16> {
    b.get(p..p + 2)
        .map(|s| u16::from_le_bytes(s.try_into().unwrap()))
        .ok_or_else(|| Error::Fail("struct.error: unpack_from requires a buffer of 2 bytes".into()))
}

fn u64le(b: &[u8], p: usize) -> Result<u64> {
    b.get(p..p + 8)
        .map(|s| u64::from_le_bytes(s.try_into().unwrap()))
        .ok_or_else(|| Error::Fail("struct.error: unpack_from requires a buffer of 8 bytes".into()))
}

/// Opens an installer (a path or URL) as nsis.Nsis does.
pub fn open_nsis(src: &str) -> Result<Nsis> {
    Ok(Nsis::open(src, Box::new(log))?)
}

/// nsis_fetch: the files of an installer select(path) chooses (-> their path under `dest`),
/// written there; neighbouring blocks read with one request, six at once (a solid installer:
/// file by file). Returns the relative paths written, in the order written.
pub fn nsis_fetch(
    ns: &mut Nsis,
    select: &dyn Fn(&str) -> Option<String>,
    dest: &str,
) -> Result<Vec<String>> {
    let src = ns.s.src().to_string();
    // off -> (size, [rels]) in the order of the files' first appearance
    let mut want: Vec<(u64, i64, Vec<String>)> = Vec::new();
    for (path, off, size) in ns.files()? {
        if let Some(rel) = select(&path) {
            match want.iter_mut().find(|w| w.0 == off) {
                Some(w) => w.2.push(rel),
                None => want.push((off, size, vec![rel])),
            }
        }
    }
    if want.is_empty() {
        return Ok(Vec::new());
    }
    let (data_base, chunked, solid) = ns.layout();
    let mut got = Vec::new();
    let mut total: u64 = 0;
    if solid {
        let mut sorted = want.clone();
        sorted.sort_by(|a, b| (a.0, a.1, &a.2).cmp(&(b.0, b.1, &b.2)));
        for (off, _, rels) in sorted {
            let data = ns.extract(off)?;
            total += data.len() as u64;
            for rel in rels {
                util::write_file(&join(dest, &rel), &data, None)?;
                got.push(rel);
            }
        }
        log(&format!(
            "  {} files, {:.1} MB (uncompressed) read",
            got.len(),
            total as f64 / 1e6
        ));
        return Ok(got);
    }
    let word: i64 = if chunked { 8 } else { 4 };
    let items: Vec<(i64, i64, u64)> = want
        .iter()
        .map(|(off, size, _)| (data_base as i64 + *off as i64, size + word + 64, *off))
        .collect();
    let sp = spans(items, 1 << 16, 24 << 20);
    let read_span = |s: &Span<u64>| -> Result<(i64, Vec<u8>, Vec<(i64, i64, u64)>)> {
        let buf = read_at(&src, s.0 as u64, (s.1 - s.0) as u64)?;
        Ok((s.0, buf, s.2.clone()))
    };
    let block = |buf: &[u8], p: usize, off: u64| -> Result<Vec<u8>> {
        let (n, packed) = if word == 8 {
            let w = u64le(buf, p)?;
            (w & 0x7fff_ffff_ffff_ffff, w >> 63 != 0)
        } else {
            let w = u32le(buf, p)?;
            ((w & 0x7fff_ffff) as u64, w & 0x8000_0000 != 0)
        };
        let from = (p + word as usize).min(buf.len());
        let to = (p as u64 + word as u64)
            .saturating_add(n)
            .min(buf.len() as u64) as usize;
        let mut blk = buf[from..to.max(from)].to_vec();
        if (blk.len() as u64) < n {
            // (the size estimate was short (last block): read it alone)
            let at = data_base + off + word as u64;
            blk = read_at(&src, at, n)?;
        }
        if !packed {
            return Ok(blk);
        }
        Ok(if chunked {
            pkgzip::nsis::dechunk(&blk)?
        } else {
            pkgzip::nsis::whole(&blk)?
        })
    };
    parallel(&sp, read_span, |(start, buf, lst)| {
        total += buf.len() as u64;
        for (absoff, _, off) in lst {
            let data = block(&buf, (absoff - start) as usize, off)?;
            let rels = &want.iter().find(|w| w.0 == off).unwrap().2;
            for rel in rels {
                util::write_file(&join(dest, rel), &data, None)?;
                got.push(rel.clone());
            }
        }
        Ok(())
    })?;
    log(&format!(
        "  {} files, {:.1} MB read",
        got.len(),
        total as f64 / 1e6
    ));
    Ok(got)
}

/// One member of a zip's central directory: (name, method, compressed size, size, local header
/// offset, Unix mode, crc).
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct ZipEntry {
    pub name: String,
    pub meth: u16,
    pub cs: u64,
    pub us: u64,
    pub lho: u64,
    pub mode: u32,
    pub crc: u32,
}

/// RemoteZip: a zip's central directory read with range requests; members extracted by reading
/// only their bytes.
pub struct RemoteZip {
    pub src: String,
    pub size: u64,
    pub entries: Vec<ZipEntry>,
}

impl RemoteZip {
    pub fn open(src: &str) -> Result<RemoteZip> {
        let size = if is_url(src) {
            url_size(src)?
        } else {
            util::getsize(src)?
        };
        let mut z = RemoteZip {
            src: src.to_string(),
            size,
            entries: Vec::new(),
        };
        let tail = z.read(size.saturating_sub(65558), size.min(65558))?;
        let Some(i) = rfind(&tail, b"PK\x05\x06", tail.len()) else {
            return fail(format!("ValueError: {src}: no zip end record"));
        };
        let mut n = u16le(&tail, i + 10)? as u64;
        let mut cdsize = u32le(&tail, i + 12)? as u64;
        let mut cdoff = u32le(&tail, i + 16)? as u64;
        if cdsize == 0xffff_ffff || cdoff == 0xffff_ffff || n == 0xffff {
            let j = rfind(&tail, b"PK\x06\x07", i)
                .ok_or_else(|| Error::Fail("struct.error: no zip64 locator".into()))?;
            let z64 = u64le(&tail, j + 8)?;
            let rec = z.read(z64, 56)?;
            n = u64le(&rec, 32)?;
            cdsize = u64le(&rec, 40)?;
            cdoff = u64le(&rec, 48)?;
        }
        let _ = n;
        let cd = z.read(cdoff, cdsize)?;
        let mut p = 0usize;
        while p + 46 <= cd.len() && &cd[p..p + 4] == b"PK\x01\x02" {
            let meth = u16le(&cd, p + 10)?;
            let crc = u32le(&cd, p + 16)?;
            let mut cs = u32le(&cd, p + 20)? as u64;
            let mut us = u32le(&cd, p + 24)? as u64;
            let nl = u16le(&cd, p + 28)? as usize;
            let el = u16le(&cd, p + 30)? as usize;
            let cl = u16le(&cd, p + 32)? as usize;
            let ea = u32le(&cd, p + 38)?;
            let mut lho = u32le(&cd, p + 42)? as u64;
            let name_end = (p + 46 + nl).min(cd.len());
            let name = String::from_utf8_lossy(&cd[p + 46..name_end]).into_owned();
            let ex_end = (p + 46 + nl + el).min(cd.len());
            let extra = &cd[name_end..ex_end.max(name_end)];
            if us == 0xffff_ffff || cs == 0xffff_ffff || lho == 0xffff_ffff {
                let mut q = 0;
                while q + 4 <= extra.len() {
                    let hid = u16le(extra, q)?;
                    let hl = u16le(extra, q + 2)? as usize;
                    if hid == 1 {
                        let mut f64s: Vec<u64> = (0..hl / 8)
                            .map(|k| u64le(extra, q + 4 + 8 * k))
                            .collect::<Result<_>>()?;
                        let mut pop = || -> Result<u64> {
                            if f64s.is_empty() {
                                return fail("IndexError: pop from empty list");
                            }
                            Ok(f64s.remove(0))
                        };
                        if us == 0xffff_ffff {
                            us = pop()?;
                        }
                        if cs == 0xffff_ffff {
                            cs = pop()?;
                        }
                        if lho == 0xffff_ffff {
                            lho = pop()?;
                        }
                        break;
                    }
                    q += 4 + hl;
                }
            }
            z.entries.push(ZipEntry {
                name,
                meth,
                cs,
                us,
                lho,
                mode: ea >> 16,
                crc,
            });
            p += 46 + nl + el + cl;
        }
        Ok(z)
    }

    pub fn read(&self, off: u64, n: u64) -> Result<Vec<u8>> {
        read_at(&self.src, off, n)
    }

    /// The members select(name) chooses (-> their path under `dest`); symbolic links made as
    /// links. Returns the relative paths written.
    pub fn extract(
        &self,
        select: &dyn Fn(&str) -> Option<String>,
        dest: &str,
    ) -> Result<Vec<String>> {
        let mut want: Vec<(i64, i64, (ZipEntry, String))> = Vec::new();
        for e in &self.entries {
            if e.name.ends_with('/') {
                continue;
            }
            if let Some(rel) = select(&e.name) {
                want.push((e.lho as i64, e.cs as i64 + 30 + 1024, (e.clone(), rel)));
            }
        }
        let mut got = Vec::new();
        let mut total = 0u64;
        let sp = spans(want, 1 << 16, 24 << 20);
        let size = self.size as i64;
        let read_span = |s: &Span<(ZipEntry, String)>| -> Result<(i64, Vec<u8>)> {
            let n = s.1.min(size) - s.0;
            Ok((s.0, self.read(s.0 as u64, n.max(0) as u64)?))
        };
        let mut k = 0usize;
        let sp_ref = &sp;
        parallel(sp_ref, read_span, |(start, buf)| {
            let lst = &sp_ref[k].2;
            k += 1;
            total += buf.len() as u64;
            for (off, _, (e, rel)) in lst {
                let p = (off - start) as usize;
                let nl = u16le(&buf, p + 26)? as usize;
                let el = u16le(&buf, p + 28)? as usize;
                let d0 = p + 30 + nl + el;
                let mut blk = buf
                    .get(d0..(d0 as u64 + e.cs).min(buf.len() as u64) as usize)
                    .unwrap_or(&[])
                    .to_vec();
                if (blk.len() as u64) < e.cs {
                    blk = self.read(*off as u64 + 30 + nl as u64 + el as u64, e.cs)?;
                }
                let data = if e.meth == 0 {
                    blk
                } else {
                    let mut out = Vec::new();
                    let _ = flate2::read::DeflateDecoder::new(&blk[..]).read_to_end(&mut out);
                    out
                };
                let mut crc = flate2::Crc::new();
                crc.update(&data);
                if data.len() as u64 != e.us || crc.sum() != e.crc {
                    return fail(format!("OSError: {}: bad size or CRC", e.name));
                }
                let dst = join(dest, rel);
                if e.mode & 0o170000 == 0o120000 {
                    util::makedirs(util::dirname(&dst))?;
                    if util::lexists(&dst) {
                        util::unlink(&dst)?;
                    }
                    let target = String::from_utf8_lossy(&data).into_owned();
                    std::os::unix::fs::symlink(&target, &dst).map_err(at(&dst))?;
                } else {
                    let mode = e.mode & 0o777;
                    util::write_file(&dst, &data, (mode != 0).then_some(mode))?;
                }
                got.push(rel.clone());
            }
            Ok(())
        })?;
        log(&format!(
            "  {} files, {:.1} MB read",
            got.len(),
            total as f64 / 1e6
        ));
        Ok(got)
    }
}

/// bytes.rfind(needle, 0, end)
fn rfind(hay: &[u8], needle: &[u8], end: usize) -> Option<usize> {
    let end = end.min(hay.len());
    if needle.len() > end {
        return None;
    }
    (0..=end - needle.len())
        .rev()
        .find(|&i| &hay[i..i + needle.len()] == needle)
}

/// The size of a file at a URL (the HEAD request's Content-Length; here a one-byte range
/// request's Content-Range, which the same servers answer).
fn url_size(url: &str) -> Result<u64> {
    let mut c = pkgzip::http::Client::default();
    c.max_body = 1 << 16;
    let r = c
        .get_following(
            url,
            &[("Range", "bytes=0-0"), ("User-Agent", "Python-urllib/3.11")],
            10,
        )
        .map_err(|e| Error::Fail(format!("<urlopen error {e}>")))?;
    if r.status >= 400 {
        return fail(format!("HTTP Error {}: {url}", r.status));
    }
    if let Some(cr) = r.header("content-range") {
        if let Some(t) = cr.rsplit('/').next().and_then(|t| t.trim().parse().ok()) {
            return Ok(t);
        }
    }
    match r
        .header("content-length")
        .and_then(|v| v.trim().parse().ok())
    {
        Some(n) if r.status == 200 => Ok(n),
        _ => fail(format!("{url}: no Content-Length")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn spans_merge() {
        let s = spans(
            vec![(100, 10, 1), (0, 10, 0), (200_000, 5, 2), (115, 1, 3)],
            1 << 16,
            24 << 20,
        );
        assert_eq!(s.len(), 2);
        assert_eq!((s[0].0, s[0].1), (0, 116));
        assert_eq!(
            s[0].2.iter().map(|x| x.2).collect::<Vec<_>>(),
            vec![0, 1, 3]
        );
        assert_eq!((s[1].0, s[1].1), (200_000, 200_005));
        // the limit splits
        let s = spans(vec![(0, 10, 0), (20, 10, 1)], 1 << 16, 25);
        assert_eq!(s.len(), 2);
    }

    #[test]
    fn local_zip() {
        // a zip written by pkgzip's writer, read back member by member
        let d = std::env::temp_dir().join(format!("il2cpp-zip-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        let zp = d.join("t.zip");
        {
            use std::io::Write;
            let mut f = std::fs::File::create(&zp).unwrap();
            // stored member "a/b.txt" and a deflated "c.bin", written by hand
            let mut central = Vec::new();
            let mut off = 0u32;
            for (name, data, meth) in [
                ("top/a/b.txt", b"hello".to_vec(), 0u16),
                ("top/c.bin", vec![7u8; 3000], 8u16),
            ] {
                let comp = if meth == 0 {
                    data.clone()
                } else {
                    let mut e = flate2::write::DeflateEncoder::new(
                        Vec::new(),
                        flate2::Compression::default(),
                    );
                    e.write_all(&data).unwrap();
                    e.finish().unwrap()
                };
                let mut crc = flate2::Crc::new();
                crc.update(&data);
                let mut lh = Vec::new();
                lh.extend(b"PK\x03\x04");
                lh.extend(20u16.to_le_bytes());
                lh.extend(0u16.to_le_bytes());
                lh.extend(meth.to_le_bytes());
                lh.extend([0u8; 4]);
                lh.extend(crc.sum().to_le_bytes());
                lh.extend((comp.len() as u32).to_le_bytes());
                lh.extend((data.len() as u32).to_le_bytes());
                lh.extend((name.len() as u16).to_le_bytes());
                lh.extend(0u16.to_le_bytes());
                lh.extend(name.as_bytes());
                f.write_all(&lh).unwrap();
                f.write_all(&comp).unwrap();
                let mut c = Vec::new();
                c.extend(b"PK\x01\x02");
                c.extend(0x031eu16.to_le_bytes());
                c.extend(20u16.to_le_bytes());
                c.extend(0u16.to_le_bytes());
                c.extend(meth.to_le_bytes());
                c.extend([0u8; 4]);
                c.extend(crc.sum().to_le_bytes());
                c.extend((comp.len() as u32).to_le_bytes());
                c.extend((data.len() as u32).to_le_bytes());
                c.extend((name.len() as u16).to_le_bytes());
                c.extend([0u8; 8]);
                c.extend(((0o100755u32) << 16).to_le_bytes());
                c.extend(off.to_le_bytes());
                c.extend(name.as_bytes());
                central.extend(c);
                off += (lh.len() + comp.len()) as u32;
            }
            f.write_all(&central).unwrap();
            let mut e = Vec::new();
            e.extend(b"PK\x05\x06");
            e.extend([0u8; 4]);
            e.extend(2u16.to_le_bytes());
            e.extend(2u16.to_le_bytes());
            e.extend((central.len() as u32).to_le_bytes());
            e.extend(off.to_le_bytes());
            e.extend(0u16.to_le_bytes());
            f.write_all(&e).unwrap();
        }
        let z = RemoteZip::open(zp.to_str().unwrap()).unwrap();
        assert_eq!(z.entries.len(), 2);
        let out = d.join("out");
        let got = z
            .extract(
                &|n| n.split_once('/').map(|x| x.1.to_string()),
                out.to_str().unwrap(),
            )
            .unwrap();
        assert_eq!(got, vec!["a/b.txt", "c.bin"]);
        assert_eq!(std::fs::read(out.join("a/b.txt")).unwrap(), b"hello");
        assert_eq!(std::fs::read(out.join("c.bin")).unwrap(), vec![7u8; 3000]);
        assert!(util::executable(out.join("c.bin").to_str().unwrap()));
        let _ = std::fs::remove_dir_all(&d);
    }
}
