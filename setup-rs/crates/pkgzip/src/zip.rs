//! Zip archives read as Python's zipfile reads them (APKs, OBBs, the setup's own zips): the end
//! record (zip64 too, data before the archive allowed), the central directory with zipfile's
//! checks, each entry's local header and data (stored, deflate, bzip2, lzma) with its CRC checked
//! at the end; and `python -m zipfile -e <zip> <dir>` (extract_all).
use std::collections::HashMap;
use std::fs::File;
use std::io::{self, Read, Write};
use std::os::unix::fs::FileExt;
use std::sync::Arc;

use flate2::{Decompress, FlushDecompress, Status};

use crate::streams::Region;
use crate::{io_at, py, Error, Result};

pub const STORED: u16 = 0;
pub const DEFLATED: u16 = 8;
pub const BZIP2: u16 = 12;
pub const LZMA: u16 = 14;

const UTF8_NAME: u16 = 0x800;

/// One entry of the central directory (zipfile.ZipInfo).
#[derive(Clone, Debug)]
pub struct Entry {
    /// The name, cut at a NUL (ZipInfo.filename).
    pub name: String,
    /// The name as the directory gives it (ZipInfo.orig_filename).
    pub orig_name: String,
    pub create_version: u8,
    pub create_system: u8,
    pub extract_version: u8,
    pub flag_bits: u16,
    pub method: u16,
    pub time: u16,
    pub date: u16,
    pub crc: u32,
    pub compress_size: u64,
    pub file_size: u64,
    pub internal_attr: u16,
    pub external_attr: u32,
    /// Where its local header is (with the data that precedes the archive, if any).
    pub header_offset: i64,
    /// Where the next entry (or the central directory) begins: an entry may not reach past it.
    end_offset: i64,
}

impl Entry {
    /// ZipInfo.is_dir(): the name ends with '/'.
    pub fn is_dir(&self) -> bool {
        self.name.ends_with('/')
    }
}

pub struct Archive {
    file: Arc<File>,
    pub entries: Vec<Entry>,
    by_name: HashMap<String, usize>,
    /// Where the central directory begins.
    pub start_dir: u64,
}

const CP437: &str = "ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜ¢£¥₧ƒáíóúñÑªº¿⌐¬½¼¡«»░▒▓│┤╡╢╖╕╣║╗╝╜╛┐└┴┬├─┼╞╟╚╔╩╦╠═╬╧╨╤╥╙╘╒╓╫╪┘┌█▄▌▐▀αßΓπΣσµτΦΘΩδ∞φε∩≡±≥≤⌠⌡÷≈°∙·√ⁿ²■\u{a0}";

/// A name as zipfile decodes it: UTF-8 with the flag, else cp437.
pub fn decode_name(b: &[u8], flags: u16) -> Result<String> {
    if flags & UTF8_NAME != 0 {
        return String::from_utf8(b.to_vec()).map_err(|e| {
            Error::Fail(format!(
                "UnicodeDecodeError: 'utf-8' codec can't decode {}: {e}",
                py::bytes_repr(b)
            ))
        });
    }
    let high: Vec<char> = CP437.chars().collect();
    Ok(b.iter()
        .map(|&c| {
            if c < 0x80 {
                c as char
            } else {
                high[c as usize - 0x80]
            }
        })
        .collect())
}

fn bad(msg: impl std::fmt::Display) -> Error {
    Error::Fail(format!("zipfile.BadZipFile: {msg}"))
}

fn u16_at(b: &[u8], i: usize) -> u16 {
    u16::from_le_bytes([b[i], b[i + 1]])
}

fn u32_at(b: &[u8], i: usize) -> u32 {
    u32::from_le_bytes(b[i..i + 4].try_into().unwrap())
}

fn u64_at(b: &[u8], i: usize) -> u64 {
    u64::from_le_bytes(b[i..i + 8].try_into().unwrap())
}

/// Reads up to n bytes at off (fewer at the end of the file).
fn read_at(f: &File, off: u64, n: usize) -> io::Result<Vec<u8>> {
    let mut v = vec![0u8; n];
    let mut got = 0;
    while got < n {
        match f.read_at(&mut v[got..], off + got as u64) {
            Ok(0) => break,
            Ok(k) => got += k,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(e) => return Err(e),
        }
    }
    v.truncate(got);
    Ok(v)
}

/// The end record: (signature, central directory size, its offset, where the record is).
type EndRecord = ([u8; 4], u64, u64, u64);

/// Finds the end record as zipfile._EndRecData (and _EndRecData64) do.
fn end_record(f: &File, size: u64) -> Result<Option<EndRecord>> {
    if size < 22 {
        return Ok(None);
    }
    let tail = read_at(f, size - 22, 22)?;
    let (rec, loc) = if tail.starts_with(b"PK\x05\x06") && tail.ends_with(b"\0\0") {
        (tail, size - 22)
    } else {
        let start = size.saturating_sub((1 << 16) + 22);
        let data = read_at(f, start, (size - start) as usize)?;
        let Some(i) = data.windows(4).rposition(|w| w == b"PK\x05\x06") else {
            return Ok(None);
        };
        if data.len() - i < 22 {
            return Ok(None);
        }
        (data[i..i + 22].to_vec(), start + i as u64)
    };
    let mut out = (
        *b"PK\x05\x06",
        u32_at(&rec, 12) as u64,
        u32_at(&rec, 16) as u64,
        loc,
    );
    // zip64: the locator just before the record, the zip64 record before it
    if loc >= 20 {
        let l = read_at(f, loc - 20, 20)?;
        if l.len() == 20 && l.starts_with(b"PK\x06\x07") {
            if u32_at(&l, 4) != 0 || u32_at(&l, 16) > 1 {
                return Err(bad("zipfiles that span multiple disks are not supported"));
            }
            if loc < 76 {
                return Err(bad("File is not a zip file"));
            }
            let r = read_at(f, loc - 76, 56)?;
            if r.len() == 56 && r.starts_with(b"PK\x06\x06") {
                out = (*b"PK\x06\x06", u64_at(&r, 40), u64_at(&r, 48), loc);
            }
        }
    }
    Ok(Some(out))
}

/// zipfile.is_zipfile(path): an end record is there. What the Python raises past its OSError
/// (a zip64 locator naming several disks) is an Err.
pub fn is_zipfile(path: &str) -> Result<bool> {
    let Ok(f) = File::open(path) else {
        return Ok(false);
    };
    let Ok(m) = f.metadata() else {
        return Ok(false);
    };
    match end_record(&f, m.len()) {
        Ok(r) => Ok(r.is_some()),
        Err(e) if e.to_string().contains("span multiple disks") => Err(e),
        // (a zip64 record before the file's start: an OSError for is_zipfile)
        Err(_) => Ok(false),
    }
}

impl Archive {
    /// Reads the central directory (ZipFile(path, 'r')).
    pub fn open(path: &str) -> Result<Archive> {
        let f = File::open(path).map_err(io_at(path))?;
        let size = f.metadata()?.len();
        let Some((sig, size_cd, offset_cd, loc)) = end_record(&f, size)? else {
            return Err(bad("File is not a zip file"));
        };
        let mut concat = loc as i128 - size_cd as i128 - offset_cd as i128;
        if &sig == b"PK\x06\x06" {
            concat -= 56 + 20;
        }
        let start_dir = offset_cd as i128 + concat;
        if start_dir < 0 {
            return Err(bad("Bad offset for central directory"));
        }
        let start_dir = start_dir as u64;
        let data = read_at(
            &f,
            start_dir,
            size_cd.min(size.saturating_sub(start_dir)) as usize,
        )?;
        let mut entries = Vec::new();
        let mut by_name = HashMap::new();
        let (mut p, mut total) = (0usize, 0u64);
        let take = |p: &mut usize, n: usize| {
            let s = &data[(*p).min(data.len())..(*p + n).min(data.len())];
            *p += n;
            s
        };
        while total < size_cd {
            let c = take(&mut p, 46);
            if c.len() != 46 {
                return Err(bad("Truncated central directory"));
            }
            if !c.starts_with(b"PK\x01\x02") {
                return Err(bad("Bad magic number for central directory"));
            }
            let c = c.to_vec();
            let (n_len, x_len, k_len) = (
                u16_at(&c, 28) as usize,
                u16_at(&c, 30) as usize,
                u16_at(&c, 32) as usize,
            );
            let flag_bits = u16_at(&c, 8);
            let orig_name = decode_name(take(&mut p, n_len), flag_bits)?;
            let extra = take(&mut p, x_len).to_vec();
            take(&mut p, k_len);
            let extract_version = c[6];
            if extract_version > 63 {
                return Err(Error::Fail(format!(
                    "NotImplementedError: zip file version {:.1}",
                    extract_version as f64 / 10.0
                )));
            }
            let mut e = Entry {
                name: orig_name.split('\0').next().unwrap_or("").to_string(),
                orig_name,
                create_version: c[4],
                create_system: c[5],
                extract_version,
                flag_bits,
                method: u16_at(&c, 10),
                time: u16_at(&c, 12),
                date: u16_at(&c, 14),
                crc: u32_at(&c, 16),
                compress_size: u32_at(&c, 20) as u64,
                file_size: u32_at(&c, 24) as u64,
                internal_attr: u16_at(&c, 36),
                external_attr: u32_at(&c, 38),
                header_offset: u32_at(&c, 42) as i64,
                end_offset: 0,
            };
            decode_extra(&mut e, &extra)?;
            // (Python's integers; an offset past i64 is past the end of any file)
            e.header_offset =
                (e.header_offset as i128 + concat).clamp(i64::MIN as i128, i64::MAX as i128) as i64;
            by_name.insert(e.name.clone(), entries.len());
            entries.push(e);
            total += 46 + (n_len + x_len + k_len) as u64;
        }
        // each entry ends where the next one (by offset) or the central directory begins
        let mut order: Vec<usize> = (0..entries.len()).collect();
        order.sort_by(|&a, &b| entries[b].header_offset.cmp(&entries[a].header_offset));
        let mut end = start_dir as i64;
        for i in order {
            entries[i].end_offset = end;
            end = entries[i].header_offset;
        }
        Ok(Archive {
            file: Arc::new(f),
            entries,
            by_name,
            start_dir,
        })
    }

    /// The names in the directory's order, repeats included (namelist()).
    pub fn names(&self) -> impl Iterator<Item = &str> {
        self.entries.iter().map(|e| e.name.as_str())
    }

    /// The entry of a name, the last one where it repeats (getinfo()).
    pub fn get(&self, name: &str) -> Option<&Entry> {
        self.by_name.get(name).map(|&i| &self.entries[i])
    }

    /// getinfo() with its KeyError.
    pub fn info(&self, name: &str) -> Result<&Entry> {
        self.get(name).ok_or_else(|| {
            Error::Fail(format!(
                "KeyError: \"There is no item named {} in the archive\"",
                py::str_repr(name)
            ))
        })
    }

    /// Overwrites a stored entry's data in place with as many new bytes (an installed game's APK
    /// or OBB: no other entry moves) and its CRC-32 in the local header and the central
    /// directory. A compressed entry, one of another length or one with a data descriptor is
    /// refused. `path` is this archive's file, opened again for writing.
    pub fn overwrite_stored(&self, path: &str, name: &str, data: &[u8]) -> Result<()> {
        self.overwrite_stored_from(path, name, &mut &data[..], data.len() as u64)
    }

    /// overwrite_stored with the new bytes read from `src`, `n` of them, a piece at a time (a
    /// resource file of hundreds of MB on a 1 GB handheld is never whole in memory). The same
    /// bytes reach the archive in the same order: the data, then its CRC-32 twice. A source
    /// that ends before `n` bytes is an error, the entry then part new: the caller writes it
    /// again.
    pub fn overwrite_stored_from(
        &self,
        path: &str,
        name: &str,
        src: &mut dyn Read,
        n: u64,
    ) -> Result<()> {
        let e = self.info(name)?;
        if e.method != 0 || e.compress_size != n || e.file_size != n {
            return Err(Error::Fail(format!(
                "{name}: not a stored entry of {n} bytes (method {}, {} bytes)",
                e.method, e.file_size
            )));
        }
        if e.flag_bits & 0x08 != 0 {
            return Err(Error::Fail(format!(
                "{name}: an entry with a data descriptor"
            )));
        }
        let at = self.data_offset(e)?;
        let hdr = e.header_offset as u64;
        // its central directory record: the records in order from the directory's start
        let mut rec = self.start_dir;
        let mut found = None;
        for _ in 0..self.entries.len() {
            let c = read_at(&self.file, rec, 46)?;
            if c.len() != 46 || !c.starts_with(b"PK\x01\x02") {
                return Err(bad("Bad magic number for central directory"));
            }
            let (n_len, x_len, k_len) = (
                u16_at(&c, 28) as u64,
                u16_at(&c, 30) as u64,
                u16_at(&c, 32) as u64,
            );
            let nm = read_at(&self.file, rec + 46, n_len as usize)?;
            if decode_name(&nm, u16_at(&c, 8))? == e.orig_name {
                found = Some(rec);
                break;
            }
            rec += 46 + n_len + x_len + k_len;
        }
        let rec = found.ok_or_else(|| bad(format!("{name}: no central directory record")))?;
        let w = std::fs::OpenOptions::new()
            .write(true)
            .open(path)
            .map_err(io_at(path))?;
        let mut crc = crc32fast::Hasher::new();
        let mut buf = vec![0u8; (n.min(1 << 20) as usize).max(1)];
        let mut done = 0u64;
        while done < n {
            let want = (n - done).min(buf.len() as u64) as usize;
            let got = match src.read(&mut buf[..want]) {
                Ok(0) => {
                    return Err(Error::Fail(format!(
                        "{name}: the new data ends after {done} of {n} bytes"
                    )))
                }
                Ok(k) => k,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(e) => return Err(e.into()),
            };
            w.write_all_at(&buf[..got], at + done)?;
            crc.update(&buf[..got]);
            done += got as u64;
        }
        let crc = crc.finalize().to_le_bytes();
        w.write_all_at(&crc, hdr + 14)?;
        w.write_all_at(&crc, rec + 16)?;
        w.sync_all()?;
        Ok(())
    }

    /// New data for entries (name, bytes) of an installed game's APK, without a byte that is there
    /// overwritten: each goes after the archive's end as a stored entry (its data 4-byte aligned
    /// as apk.py aligns it), then the central directory again with those records pointing at
    /// them, then the end record. The old data, directory and end record stay as dead bytes
    /// (readers take the last end record). Before anything is written `<path>.put` holds the
    /// archive's length: an interrupted put leaves it and recover_put cuts the archive back to
    /// it. Refused: a name not in the archive, a zip64 one, one that would pass 2 GiB (zip's
    /// offsets; the 32-bit player's reads). `path` is this archive's file.
    pub fn put_stored(&self, path: &str, puts: &[(&str, &[u8])]) -> Result<()> {
        let size = self.file.metadata()?.len();
        let Some((sig, size_cd, offset_cd, loc)) = end_record(&self.file, size)? else {
            return Err(bad("File is not a zip file"));
        };
        if &sig != b"PK\x05\x06" {
            return Err(Error::Fail(format!("{path}: a zip64 archive")));
        }
        // (from the end record as it is now: an earlier put moved the directory)
        let Some(start_dir) = loc.checked_sub(size_cd) else {
            return Err(bad("Bad offset for central directory"));
        };
        let Some(concat) = start_dir.checked_sub(offset_cd) else {
            return Err(bad("Bad offset for central directory"));
        };
        let mut cd = read_at(&self.file, start_dir, size_cd as usize)?;
        let tail = read_at(&self.file, loc, (size - loc) as usize)?;
        // the directory's records: where each starts, by name (the last one where a name repeats)
        let mut recs = HashMap::new();
        let mut p = 0usize;
        while p + 46 <= cd.len() && cd[p..].starts_with(b"PK\x01\x02") {
            let (n_len, x_len, k_len) = (
                u16_at(&cd, p + 28) as usize,
                u16_at(&cd, p + 30) as usize,
                u16_at(&cd, p + 32) as usize,
            );
            let nm = decode_name(&cd[p + 46..p + 46 + n_len], u16_at(&cd, p + 8))?;
            recs.insert(nm.split('\0').next().unwrap_or("").to_string(), p);
            p += 46 + n_len + x_len + k_len;
        }
        let mut blob = Vec::new();
        for (name, data) in puts {
            let &r = recs
                .get(*name)
                .ok_or_else(|| Error::Fail(format!("{name}: not in {path}")))?;
            if [20, 24, 42].iter().any(|&i| u32_at(&cd, r + i) == 0xFFFF_FFFF) {
                return Err(Error::Fail(format!("{name}: a zip64 entry")));
            }
            let at = size + blob.len() as u64;
            let n_len = u16_at(&cd, r + 28) as usize;
            let name_b = cd[r + 46..r + 46 + n_len].to_vec();
            let pad = (4 - (at + 30 + n_len as u64 + 4) % 4) % 4;
            let mut extra = vec![0x35, 0xD9, pad as u8, 0];
            extra.resize(4 + pad as usize, 0);
            let flags = u16_at(&cd, r + 8) & !0x08;
            let crc = crc32fast::hash(data);
            let n = (data.len() as u32).to_le_bytes();
            blob.extend(b"PK\x03\x04");
            blob.extend(&cd[r + 6..r + 8]);
            blob.extend(flags.to_le_bytes());
            blob.extend(STORED.to_le_bytes());
            blob.extend(&cd[r + 12..r + 16]);
            blob.extend(crc.to_le_bytes());
            blob.extend(n);
            blob.extend(n);
            blob.extend((n_len as u16).to_le_bytes());
            blob.extend((extra.len() as u16).to_le_bytes());
            blob.extend(&name_b);
            blob.extend(&extra);
            blob.extend(*data);
            cd[r + 8..r + 10].copy_from_slice(&flags.to_le_bytes());
            cd[r + 10..r + 12].copy_from_slice(&STORED.to_le_bytes());
            cd[r + 16..r + 20].copy_from_slice(&crc.to_le_bytes());
            cd[r + 20..r + 24].copy_from_slice(&n);
            cd[r + 24..r + 28].copy_from_slice(&n);
            cd[r + 42..r + 46].copy_from_slice(&((at - concat) as u32).to_le_bytes());
        }
        let new_cd = size + blob.len() as u64;
        let end = new_cd + cd.len() as u64 + tail.len() as u64;
        if end > (1 << 31) - 1 {
            return Err(Error::Fail(format!(
                "{path}: would pass 2 GiB ({end} bytes) with the new data"
            )));
        }
        let mut tail = tail;
        tail[16..20].copy_from_slice(&((new_cd - concat) as u32).to_le_bytes());
        let journal = format!("{path}.put");
        let mut j = File::create(&journal).map_err(io_at(&journal))?;
        j.write_all(size.to_string().as_bytes())?;
        j.sync_all()?;
        let w = std::fs::OpenOptions::new()
            .write(true)
            .open(path)
            .map_err(io_at(path))?;
        w.write_all_at(&blob, size)?;
        w.write_all_at(&cd, new_cd)?;
        w.write_all_at(&tail, new_cd + cd.len() as u64)?;
        w.sync_all()?;
        std::fs::remove_file(&journal).map_err(io_at(&journal))?;
        Ok(())
    }

    /// Where an entry's data begins (after its local header and that header's extra field).
    pub fn data_offset(&self, e: &Entry) -> Result<u64> {
        Ok(self.local(e)?.0)
    }

    /// The local header's checks (ZipFile.open): (data offset, its name bytes).
    fn local(&self, e: &Entry) -> Result<(u64, Vec<u8>, u16)> {
        let at = u64::try_from(e.header_offset)
            .map_err(|_| Error::Fail("OSError: [Errno 22] Invalid argument".into()))?;
        let h = read_at(&self.file, at, 30)?;
        if h.len() != 30 {
            return Err(bad("Truncated file header"));
        }
        if !h.starts_with(b"PK\x03\x04") {
            return Err(bad("Bad magic number for file header"));
        }
        let fname = read_at(&self.file, at + 30, u16_at(&h, 26) as usize)?;
        let x = read_at(
            &self.file,
            at + 30 + fname.len() as u64,
            u16_at(&h, 28) as usize,
        )?;
        Ok((
            at + 30 + (fname.len() + x.len()) as u64,
            fname,
            u16_at(&h, 6),
        ))
    }

    /// A reader of an entry's data (ZipFile.open(entry)), its CRC checked when it ends.
    pub fn reader(&self, e: &Entry) -> Result<EntryReader> {
        let (data, fname, lflags) = self.local(e)?;
        if e.flag_bits & 0x20 != 0 {
            return Err(Error::Fail(
                "NotImplementedError: compressed patched data (flag bit 5)".into(),
            ));
        }
        if e.flag_bits & 0x40 != 0 {
            return Err(Error::Fail(
                "NotImplementedError: strong encryption (flag bit 6)".into(),
            ));
        }
        if decode_name(&fname, lflags)? != e.orig_name {
            return Err(bad(format!(
                "File name in directory {} and header {} differ.",
                py::str_repr(&e.orig_name),
                py::bytes_repr(&fname)
            )));
        }
        if data as i128 + e.compress_size as i128 > e.end_offset as i128 {
            return Err(bad(format!(
                "Overlapped entries: {} (possible zip bomb)",
                py::str_repr(&e.orig_name)
            )));
        }
        if e.flag_bits & 1 != 0 {
            return Err(Error::Fail(format!(
                "RuntimeError: File {} is encrypted, password required for extraction",
                py::str_repr(&e.name)
            )));
        }
        let raw = Region::new(self.file.clone(), data, e.compress_size);
        let dec = match e.method {
            STORED => Dec::Stored,
            DEFLATED => Dec::Deflate(Box::new(Decompress::new(false))),
            BZIP2 => Dec::Bzip2(Box::new(bzip2::Decompress::new(false))),
            LZMA => Dec::Lzma(None),
            _ => {
                return Err(Error::Fail(
                    "NotImplementedError: That compression method is not supported".into(),
                ))
            }
        };
        Ok(EntryReader {
            raw,
            left_in: e.compress_size,
            inbuf: Vec::new(),
            in_pos: 0,
            dec,
            left: e.file_size,
            crc: crc32fast::Hasher::new(),
            expect: e.crc,
            name: e.name.clone(),
            eof: false,
            drain: false,
        })
    }

    /// An entry's data, whole (ZipFile.read(name)).
    pub fn read(&self, name: &str) -> Result<Vec<u8>> {
        let mut v = Vec::new();
        self.reader(self.info(name)?)?.read_to_end(&mut v)?;
        Ok(v)
    }
}

/// ZipInfo._decodeExtra: the zip64 sizes and offset, and the checks of the extra field.
fn decode_extra(e: &mut Entry, mut extra: &[u8]) -> Result<()> {
    while extra.len() >= 4 {
        let (tp, ln) = (u16_at(extra, 0), u16_at(extra, 2) as usize);
        if ln + 4 > extra.len() {
            return Err(bad(format!("Corrupt extra field {tp:04x} (size={ln})")));
        }
        if tp == 1 {
            let mut d = &extra[4..ln + 4];
            let mut next = |field: &str| -> Result<u64> {
                if d.len() < 8 {
                    return Err(bad(format!(
                        "Corrupt zip64 extra field. {field} not found."
                    )));
                }
                let v = u64_at(d, 0);
                d = &d[8..];
                Ok(v)
            };
            if e.file_size == 0xFFFF_FFFF {
                e.file_size = next("File size")?;
            }
            if e.compress_size == 0xFFFF_FFFF {
                e.compress_size = next("Compress size")?;
            }
            if e.header_offset == 0xFFFF_FFFF {
                e.header_offset = i64::try_from(next("Header offset")?).unwrap_or(i64::MAX);
            }
        }
        extra = &extra[ln + 4..];
    }
    Ok(())
}

enum Dec {
    Stored,
    Deflate(Box<Decompress>),
    Bzip2(Box<bzip2::Decompress>),
    /// (made once the 4-byte header and the properties are read)
    Lzma(Option<Box<lzma_rust2::LzmaReader<io::Take<Region>>>>),
}

/// An entry's data as zipfile.ZipExtFile gives it: at most file_size bytes, from at most
/// compress_size bytes of input; at its end the CRC is checked, and a mismatch fails the read that
/// brings the last bytes (as ZipExtFile's read does).
pub struct EntryReader {
    raw: Region,
    left_in: u64,
    inbuf: Vec<u8>,
    in_pos: usize,
    dec: Dec,
    left: u64,
    crc: crc32fast::Hasher,
    expect: u32,
    name: String,
    eof: bool,
    drain: bool,
}

impl EntryReader {
    /// More input when the last is used up; zipfile raises a bare EOFError when the file ends first.
    fn input(&mut self) -> io::Result<()> {
        if self.in_pos == self.inbuf.len() && self.left_in > 0 {
            let n = self.left_in.min(1 << 16) as usize;
            self.inbuf.resize(n, 0);
            let got = self.raw.read(&mut self.inbuf)?;
            if got == 0 {
                return Err(io::Error::other(Error::Fail("EOFError".into())));
            }
            self.inbuf.truncate(got);
            self.in_pos = 0;
            self.left_in -= got as u64;
        }
        Ok(())
    }

    fn input_done(&self) -> bool {
        self.left_in == 0 && self.in_pos == self.inbuf.len()
    }

    /// One step: up to out.len() bytes (0 with eof set at the end).
    fn step(&mut self, out: &mut [u8]) -> io::Result<usize> {
        if self.left == 0 {
            self.eof = true;
            return Ok(0);
        }
        let lim = (out.len() as u64).min(self.left) as usize;
        let made = match &mut self.dec {
            Dec::Stored => {
                if self.left_in == 0 {
                    self.eof = true;
                    return Ok(0);
                }
                let n = self.left_in.min(lim as u64) as usize;
                let got = self.raw.read(&mut out[..n])?;
                if got == 0 {
                    return Err(io::Error::other(Error::Fail("EOFError".into())));
                }
                self.left_in -= got as u64;
                if self.left_in == 0 {
                    self.eof = true;
                }
                got
            }
            Dec::Deflate(_) | Dec::Bzip2(_) => {
                if !self.drain {
                    self.input()?;
                }
                let inp = &self.inbuf[self.in_pos..];
                let (used, made, end) = match &mut self.dec {
                    Dec::Deflate(z) => {
                        let (i0, o0) = (z.total_in(), z.total_out());
                        let st = z
                            .decompress(inp, &mut out[..lim], FlushDecompress::None)
                            .map_err(|e| {
                                io::Error::other(Error::Fail(format!(
                                    "zlib.error: Error -3 while decompressing data: {e}"
                                )))
                            })?;
                        (
                            (z.total_in() - i0) as usize,
                            (z.total_out() - o0) as usize,
                            st == Status::StreamEnd,
                        )
                    }
                    Dec::Bzip2(z) => {
                        let (i0, o0) = (z.total_in(), z.total_out());
                        let st = z
                            .decompress(inp, &mut out[..lim])
                            .map_err(|e| io::Error::other(Error::Fail(format!("OSError: {e}"))))?;
                        (
                            (z.total_in() - i0) as usize,
                            (z.total_out() - o0) as usize,
                            matches!(st, bzip2::Status::StreamEnd),
                        )
                    }
                    _ => unreachable!(),
                };
                self.in_pos += used;
                let was_draining = self.drain;
                self.drain = self.input_done();
                // the end: the stream's, or the input's once nothing more comes out of it
                if end || (was_draining && made == 0) {
                    self.eof = true;
                }
                made
            }
            Dec::Lzma(r) => {
                if r.is_none() {
                    // zipfile's LZMA: version (2 bytes), properties size (2), the LZMA1 properties
                    let mut head = Vec::new();
                    (&mut self.raw).take(4).read_to_end(&mut head)?;
                    let psize = if head.len() == 4 {
                        u16_at(&head, 2) as u64
                    } else {
                        0
                    };
                    let mut props = Vec::new();
                    (&mut self.raw).take(psize).read_to_end(&mut props)?;
                    if head.len() < 4 || props.len() < 5 || (props.len() as u64) < psize {
                        self.eof = true; // (its decompressor waits for more and gets none)
                        return Ok(0);
                    }
                    let rest = self.left_in - 4 - psize;
                    // (the reader holds no more dictionary than the entry's size; the size is the
                    // directory's, so a damaged one is bounded too)
                    let dict =
                        u32::from_le_bytes(props[1..5].try_into().unwrap()).min(crate::MAX_DICT);
                    let rd = lzma_rust2::LzmaReader::new_with_props(
                        self.raw.clone().take(rest),
                        self.left,
                        props[0],
                        dict,
                        None,
                    )
                    .map_err(|e| io::Error::other(Error::Fail(format!("_lzma.LZMAError: {e}"))))?;
                    *r = Some(Box::new(rd));
                }
                let made =
                    r.as_mut().unwrap().read(&mut out[..lim]).map_err(|e| {
                        io::Error::other(Error::Fail(format!("_lzma.LZMAError: {e}")))
                    })?;
                if made == 0 {
                    self.eof = true;
                }
                made
            }
        };
        self.left -= made as u64;
        if self.left == 0 {
            self.eof = true;
        }
        Ok(made)
    }
}

impl Read for EntryReader {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        if out.is_empty() {
            return Ok(0);
        }
        while !self.eof {
            let made = self.step(out)?;
            self.crc.update(&out[..made]);
            if self.eof && self.crc.clone().finalize() != self.expect {
                return Err(io::Error::other(bad(format!(
                    "Bad CRC-32 for file {}",
                    py::str_repr(&self.name)
                ))));
            }
            if made > 0 {
                return Ok(made);
            }
        }
        Ok(0)
    }
}

/// Copies an entry's data in 64 KiB pieces, as shutil.copyfileobj: a piece that fails (the last,
/// when the CRC does not match) is not written.
pub fn copy_entry(r: &mut EntryReader, w: &mut dyn Write) -> Result<u64> {
    let mut buf = vec![0u8; 1 << 16];
    let mut total = 0;
    loop {
        let mut n = 0;
        while n < buf.len() {
            match r.read(&mut buf[n..]) {
                Ok(0) => break,
                Ok(k) => n += k,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
                Err(e) => return Err(e.into()),
            }
        }
        if n == 0 {
            return Ok(total);
        }
        w.write_all(&buf[..n])?;
        total += n as u64;
    }
}

/// An archive a put_stored was interrupted in (`<path>.put` there) cut back to the length the
/// journal holds (put_stored only adds bytes past it), the journal removed. Whether it was.
pub fn recover_put(path: &str) -> Result<bool> {
    let journal = format!("{path}.put");
    let Ok(s) = std::fs::read_to_string(&journal) else {
        return Ok(false);
    };
    let len: u64 = s
        .trim()
        .parse()
        .map_err(|_| Error::Fail(format!("{journal}: no length in it")))?;
    let f = std::fs::OpenOptions::new()
        .write(true)
        .open(path)
        .map_err(io_at(path))?;
    if f.metadata()?.len() < len {
        return Err(Error::Fail(format!(
            "{path}: shorter than the {len} bytes {journal} names"
        )));
    }
    f.set_len(len)?;
    f.sync_all()?;
    std::fs::remove_file(&journal).map_err(io_at(&journal))?;
    Ok(true)
}

/// `python -m zipfile -e <zip> <dir>` (ZipFile.extractall): every name in the directory's order
/// (a repeated name: its last entry, each time), its path made relative ('', '.' and '..' parts
/// dropped), folders made as needed; files and folders get the default modes (the archive's
/// are not applied).
pub fn extract_all(zip: &str, dir: &str) -> Result<()> {
    let z = Archive::open(zip)?;
    for name in z.names() {
        let e = z.info(name)?;
        let arc: Vec<&str> = e
            .name
            .split('/')
            .filter(|x| !matches!(*x, "" | "." | ".."))
            .collect();
        let target = py::normpath(&py::join(dir, &arc.join("/")));
        let up = py::dirname(&target);
        if !up.is_empty() && !std::path::Path::new(up).exists() {
            std::fs::create_dir_all(up).map_err(io_at(up))?;
        }
        if e.name.is_empty() {
            return Err(Error::Fail("IndexError: string index out of range".into()));
        }
        if e.is_dir() {
            if !std::path::Path::new(&target).is_dir() {
                std::fs::create_dir(&target).map_err(io_at(&target))?;
            }
            continue;
        }
        let mut r = z.reader(e)?;
        let mut f = File::create(&target).map_err(io_at(&target))?;
        copy_entry(&mut r, &mut f)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::write::DeflateEncoder;
    use flate2::Compression;

    /// A zip of (name bytes, flags, method, data, crc fix): local headers, central directory, end.
    fn zip(
        entries: &[(&[u8], u16, u16, &[u8])],
        prefix: &[u8],
        comment: &[u8],
        crc_xor: u32,
    ) -> Vec<u8> {
        let mut out = prefix.to_vec();
        let mut cd = Vec::new();
        for (name, flags, method, data) in entries {
            let packed = if *method == DEFLATED {
                let mut e = DeflateEncoder::new(Vec::new(), Compression::default());
                e.write_all(data).unwrap();
                e.finish().unwrap()
            } else {
                data.to_vec()
            };
            let crc = crc32fast::hash(data) ^ crc_xor;
            let at = (out.len() - prefix.len()) as u32;
            let mut h = b"PK\x03\x04".to_vec();
            h.extend([20, 0]);
            h.extend(flags.to_le_bytes());
            h.extend(method.to_le_bytes());
            h.extend([0, 0, 0x21, 0]);
            h.extend(crc.to_le_bytes());
            h.extend((packed.len() as u32).to_le_bytes());
            h.extend((data.len() as u32).to_le_bytes());
            h.extend((name.len() as u16).to_le_bytes());
            h.extend(0u16.to_le_bytes());
            let mut c = b"PK\x01\x02".to_vec();
            c.extend([20, 3]);
            c.extend(&h[4..30]);
            c.extend([0; 6]);
            c.extend((0o100755u32 << 16).to_le_bytes());
            c.extend(at.to_le_bytes());
            c.extend(*name);
            cd.extend(c);
            out.extend(h);
            out.extend(*name);
            out.extend(packed);
        }
        let start = (out.len() - prefix.len()) as u32;
        let n = entries.len() as u16;
        let size = cd.len() as u32;
        out.extend(cd);
        out.extend(b"PK\x05\x06\0\0\0\0");
        out.extend(n.to_le_bytes());
        out.extend(n.to_le_bytes());
        out.extend(size.to_le_bytes());
        out.extend(start.to_le_bytes());
        out.extend((comment.len() as u16).to_le_bytes());
        out.extend(comment);
        out
    }

    struct Tmp(std::path::PathBuf);
    impl Tmp {
        fn new(tag: &str) -> Tmp {
            let p = std::env::temp_dir().join(format!("pkgzip-zip-{tag}-{}", std::process::id()));
            let _ = std::fs::remove_dir_all(&p);
            std::fs::create_dir_all(&p).unwrap();
            Tmp(p)
        }
        fn put(&self, name: &str, data: &[u8]) -> String {
            let p = self.0.join(name);
            std::fs::write(&p, data).unwrap();
            p.to_string_lossy().into_owned()
        }
        fn path(&self, rel: &str) -> String {
            self.0.join(rel).to_string_lossy().into_owned()
        }
    }
    impl Drop for Tmp {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn reads_and_extracts_as_zipfile() {
        let t = Tmp::new("x");
        let big: Vec<u8> = (0..200_000u32).map(|i| (i * 7 % 13) as u8).collect();
        let z = zip(
            &[
                (b"d/", 0, STORED, b""),
                (b"d/big", 0, DEFLATED, &big),
                (b"../up.txt", 0, STORED, b"up"),
                (b"/abs/./x.txt", 0, STORED, b"x"),
                (b"n\x80\xe9", 0, STORED, b"cp437"),
                ("é".as_bytes(), UTF8_NAME, STORED, b"utf8"),
                (b"dup", 0, STORED, b"one"),
                (b"dup", 0, STORED, b"two"),
            ],
            b"stub before the archive",
            b"comment",
            0,
        );
        let p = t.put("a.zip", &z);
        let a = Archive::open(&p).unwrap();
        assert_eq!(
            a.names().collect::<Vec<_>>(),
            [
                "d/",
                "d/big",
                "../up.txt",
                "/abs/./x.txt",
                "nÇΘ",
                "é",
                "dup",
                "dup"
            ]
        );
        assert_eq!(a.read("d/big").unwrap(), big);
        assert_eq!(a.read("dup").unwrap(), b"two");
        extract_all(&p, &t.path("out")).unwrap();
        let r = |n: &str| std::fs::read(t.0.join("out").join(n)).unwrap();
        assert_eq!(r("d/big"), big);
        assert_eq!(r("up.txt"), b"up");
        assert_eq!(r("abs/x.txt"), b"x");
        assert_eq!(r("nÇΘ"), b"cp437");
        assert_eq!(r("é"), b"utf8");
        assert_eq!(r("dup"), b"two");
        use std::os::unix::fs::PermissionsExt;
        let mode = std::fs::metadata(t.0.join("out/up.txt"))
            .unwrap()
            .permissions()
            .mode();
        assert_eq!(mode & 0o111, 0); // (the archive's modes are not applied)
    }

    #[test]
    fn damaged_directory_offsets() {
        // (the fuzz target's) a zip64 header offset of 2^63 - 1 with data before the archive:
        // no overflow, the entry's header is past the end of the file
        let t = Tmp::new("off");
        let mut cd = b"PK\x01\x02".to_vec();
        cd.extend([20, 3, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
        cd.extend([0; 12]); // crc, sizes
        cd.extend(1u16.to_le_bytes());
        cd.extend(12u16.to_le_bytes());
        cd.extend([0; 10]); // comment length, disk, attributes
        cd.extend(u32::MAX.to_le_bytes());
        cd.push(b'a');
        cd.extend(1u16.to_le_bytes());
        cd.extend(8u16.to_le_bytes());
        cd.extend((i64::MAX as u64).to_le_bytes());
        let mut z = b"stub".to_vec();
        z.extend(&cd);
        z.extend(b"PK\x05\x06\0\0\0\0\x01\0\x01\0");
        z.extend((cd.len() as u32).to_le_bytes());
        z.extend(0u32.to_le_bytes());
        z.extend(0u16.to_le_bytes());
        let a = Archive::open(&t.put("z64.zip", &z)).unwrap();
        assert_eq!(a.entries[0].header_offset, i64::MAX);
        assert!(a.read("a").is_err());
    }

    #[test]
    fn errors_as_zipfile() {
        let t = Tmp::new("e");
        let big = vec![5u8; 150_000];
        let p = t.put(
            "crc.zip",
            &zip(
                &[(b"a", 0, STORED, b"fine"), (b"b", 0, STORED, &big)],
                b"",
                b"",
                1,
            ),
        );
        let e = extract_all(&p, &t.path("o")).unwrap_err().to_string();
        assert_eq!(e, "zipfile.BadZipFile: Bad CRC-32 for file 'a'");
        assert_eq!(std::fs::read(t.0.join("o/a")).unwrap(), b""); // (the failing piece is not written)
        let p = t.put("crc2.zip", &zip(&[(b"b", 0, DEFLATED, &big)], b"", b"", 1));
        assert!(extract_all(&p, &t.path("o2")).is_err());
        assert_eq!(std::fs::metadata(t.0.join("o2/b")).unwrap().len(), 2 << 16); // (whole 64 KiB pieces)
        let mut enc = zip(&[(b"a", 1, STORED, b"x")], b"", b"", 0);
        let e = Archive::open(&t.put("enc.zip", &enc))
            .unwrap()
            .read("a")
            .unwrap_err()
            .to_string();
        assert!(e.starts_with("RuntimeError: File 'a' is encrypted"), "{e}");
        enc = zip(&[(b"a", 0, 99, b"x")], b"", b"", 0);
        let e = Archive::open(&t.put("m.zip", &enc))
            .unwrap()
            .read("a")
            .unwrap_err()
            .to_string();
        assert_eq!(
            e,
            "NotImplementedError: That compression method is not supported"
        );
        let e = Archive::open(&t.put("not.zip", b"not a zip"))
            .err()
            .unwrap()
            .to_string();
        assert_eq!(e, "zipfile.BadZipFile: File is not a zip file");
        let e = Archive::open(&p)
            .unwrap()
            .read("nope")
            .unwrap_err()
            .to_string();
        assert_eq!(
            e,
            "KeyError: \"There is no item named 'nope' in the archive\""
        );
    }
}
