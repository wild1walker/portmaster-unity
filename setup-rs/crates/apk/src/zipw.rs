//! Zips as Python's zipfile writes them with `ZipFile(path, 'w', allowZip64=True)` and an entry
//! at a time through `zo.open(ZipInfo(name, date_time), 'w')` (apk.py copy_entries, merge_apks):
//! the local header written first and again with the CRC and sizes once the data is in, entries
//! stored or deflated (zlib's own deflate at its default level, raw, fed 1 MiB at a time, as
//! zlib.compressobj(-1, 8, -15) is by shutil.copyfileobj), the central directory with zip64
//! fields where an offset or a count passes zipfile's limits, the end records.
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::os::raw::c_int;

use pkgzip::zip::{DEFLATED, STORED};

/// zipfile.ZIP64_LIMIT and ZIP_FILECOUNT_LIMIT.
const ZIP64_LIMIT: u64 = (1 << 31) - 1;
const FILECOUNT_LIMIT: usize = (1 << 16) - 1;

/// An entry written (zipfile.ZipInfo after close).
struct Info {
    name: Vec<u8>,
    flags: u16,
    method: u16,
    time: u16,
    date: u16,
    crc: u32,
    csize: u64,
    size: u64,
    extra: Vec<u8>,
    external: u32,
    offset: u64,
}

/// What can go wrong: I/O, or what zipfile raises (the message names its exception).
#[derive(Debug)]
pub enum ZipErr {
    Io(io::Error),
    Py(String),
    /// A compression method zipfile writes with a library pmu does not have (bzip2, LZMA).
    NotPorted(String),
}

impl From<io::Error> for ZipErr {
    fn from(e: io::Error) -> Self {
        ZipErr::Io(e)
    }
}

pub type ZResult<T> = Result<T, ZipErr>;

/// A zip being written.
pub struct ZipOut {
    f: File,
    pos: u64,
    done: Vec<Info>,
    /// (warnings.warn's "Duplicate name" goes to stderr)
    names: std::collections::HashSet<Vec<u8>>,
}

/// zlib's allocator (Z_NULL's: calloc and free).
extern "C" fn zalloc(
    _: libz_sys::voidpf,
    items: libz_sys::uInt,
    size: libz_sys::uInt,
) -> libz_sys::voidpf {
    unsafe { libc::calloc(items as libc::size_t, size as libc::size_t) }
}

extern "C" fn zfree(_: libz_sys::voidpf, p: libz_sys::voidpf) {
    unsafe { libc::free(p) }
}

/// zlib's deflate, raw, Z_DEFAULT_COMPRESSION, memLevel 8, the default strategy.
struct Deflate {
    z: Box<libz_sys::z_stream>,
}

impl Deflate {
    fn new() -> ZResult<Self> {
        let mut z = Box::new(libz_sys::z_stream {
            next_in: std::ptr::null_mut(),
            avail_in: 0,
            total_in: 0,
            next_out: std::ptr::null_mut(),
            avail_out: 0,
            total_out: 0,
            msg: std::ptr::null_mut(),
            state: std::ptr::null_mut(),
            zalloc,
            zfree,
            opaque: std::ptr::null_mut(),
            data_type: 0,
            adler: 0,
            reserved: 0,
        });
        let r = unsafe {
            libz_sys::deflateInit2_(
                &mut *z,
                libz_sys::Z_DEFAULT_COMPRESSION,
                libz_sys::Z_DEFLATED,
                -15,
                8,
                libz_sys::Z_DEFAULT_STRATEGY,
                libz_sys::zlibVersion(),
                std::mem::size_of::<libz_sys::z_stream>() as c_int,
            )
        };
        if r != libz_sys::Z_OK {
            return Err(ZipErr::Py(format!(
                "zlib.error: Error {r} while compressing data"
            )));
        }
        Ok(Deflate { z })
    }

    /// Compresses `inp` (finish: and ends the stream), appending to out.
    fn run(&mut self, inp: &[u8], finish: bool, out: &mut Vec<u8>) -> ZResult<()> {
        let z = &mut *self.z;
        z.next_in = inp.as_ptr() as *mut u8;
        z.avail_in = inp.len() as libz_sys::uInt;
        let flush = if finish {
            libz_sys::Z_FINISH
        } else {
            libz_sys::Z_NO_FLUSH
        };
        loop {
            let room = (inp.len() / 2).max(1 << 16);
            out.reserve(room);
            let len = out.len();
            let spare = out.capacity() - len;
            z.next_out = unsafe { out.as_mut_ptr().add(len) };
            z.avail_out = spare as libz_sys::uInt;
            let r = unsafe { libz_sys::deflate(z, flush) };
            let made = spare - z.avail_out as usize;
            unsafe { out.set_len(len + made) };
            if r == libz_sys::Z_STREAM_END {
                return Ok(());
            }
            if r != libz_sys::Z_OK && r != libz_sys::Z_BUF_ERROR {
                return Err(ZipErr::Py(format!(
                    "zlib.error: Error {r} while compressing data"
                )));
            }
            if !finish && z.avail_in == 0 && z.avail_out != 0 {
                return Ok(());
            }
        }
    }
}

impl Drop for Deflate {
    fn drop(&mut self) {
        unsafe {
            libz_sys::deflateEnd(&mut *self.z);
        }
    }
}

/// ZipInfo._encodeFilenameFlags: ASCII as it is, else UTF-8 with flag bit 11.
fn encode_name(name: &str) -> (Vec<u8>, u16) {
    if name.is_ascii() {
        (name.as_bytes().to_vec(), 0)
    } else {
        (name.as_bytes().to_vec(), 0x800)
    }
}

fn le16(v: &mut Vec<u8>, x: u16) {
    v.extend(x.to_le_bytes());
}

fn le32(v: &mut Vec<u8>, x: u32) {
    v.extend(x.to_le_bytes());
}

/// ZipInfo.FileHeader(zip64=False): sizes past ZIP64_LIMIT are zipfile's LargeZipFile.
fn file_header(i: &Info) -> Vec<u8> {
    let mut h = Vec::with_capacity(30 + i.name.len() + i.extra.len());
    h.extend(b"PK\x03\x04");
    le16(&mut h, 20);
    le16(&mut h, i.flags);
    le16(&mut h, i.method);
    le16(&mut h, i.time);
    le16(&mut h, i.date);
    le32(&mut h, i.crc);
    le32(&mut h, i.csize as u32);
    le32(&mut h, i.size as u32);
    le16(&mut h, i.name.len() as u16);
    le16(&mut h, i.extra.len() as u16);
    h.extend(&i.name);
    h.extend(&i.extra);
    h
}

impl ZipOut {
    pub fn create(path: &str) -> io::Result<ZipOut> {
        let f = File::options()
            .read(true)
            .write(true)
            .create(true)
            .truncate(true)
            .open(path)?;
        Ok(ZipOut {
            f,
            pos: 0,
            done: Vec::new(),
            names: Default::default(),
        })
    }

    /// Where the next entry's local header goes (zo.fp.tell()).
    pub fn tell(&self) -> u64 {
        self.pos
    }

    /// One entry: `name` (cut at a NUL, as ZipInfo does), its DOS time and date, method (stored
    /// or deflated), external attributes and extra field, its data read from `src` in 1 MiB
    /// pieces.
    #[allow(clippy::too_many_arguments)]
    pub fn add(
        &mut self,
        name: &str,
        time: u16,
        date: u16,
        method: u16,
        external: u32,
        extra: Vec<u8>,
        src: &mut dyn Read,
    ) -> ZResult<()> {
        let cut = name.split('\0').next().unwrap_or("");
        let (bname, flags) = encode_name(cut);
        if !self.names.insert(bname.clone()) {
            eprintln!("UserWarning: Duplicate name: {}", pkgzip::py::str_repr(cut));
        }
        let mut deflate = match method {
            STORED => None,
            DEFLATED => Some(Deflate::new()?),
            12 | 14 => {
                return Err(ZipErr::NotPorted(format!(
                    "{}: an entry compressed with {} (zipfile writes it with {})",
                    cut,
                    if method == 12 { "bzip2" } else { "LZMA" },
                    if method == 12 { "bz2" } else { "lzma" }
                )))
            }
            _ => {
                return Err(ZipErr::Py(
                    "NotImplementedError: That compression method is not supported".into(),
                ))
            }
        };
        let mut info = Info {
            name: bname,
            flags,
            method,
            time,
            date,
            crc: 0,
            csize: 0,
            size: 0,
            extra,
            external,
            offset: self.pos,
        };
        self.f.seek(SeekFrom::Start(self.pos))?;
        let head = file_header(&info);
        self.f.write_all(&head)?;
        let mut at = self.pos + head.len() as u64;
        let mut crc = crc32fast::Hasher::new();
        let mut buf = vec![0u8; 1 << 20];
        let mut out = Vec::new();
        loop {
            // (copyfileobj's reads: a whole MiB but at the end)
            let mut n = 0;
            while n < buf.len() {
                match src.read(&mut buf[n..]) {
                    Ok(0) => break,
                    Ok(k) => n += k,
                    Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
                    Err(e) => return Err(e.into()),
                }
            }
            if n == 0 {
                break;
            }
            crc.update(&buf[..n]);
            info.size += n as u64;
            match deflate.as_mut() {
                Some(d) => {
                    out.clear();
                    d.run(&buf[..n], false, &mut out)?;
                    self.f.write_all(&out)?;
                    info.csize += out.len() as u64;
                    at += out.len() as u64;
                }
                None => {
                    self.f.write_all(&buf[..n])?;
                    info.csize += n as u64;
                    at += n as u64;
                }
            }
            if n < buf.len() {
                break;
            }
        }
        if let Some(d) = deflate.as_mut() {
            out.clear();
            d.run(&[], true, &mut out)?;
            self.f.write_all(&out)?;
            info.csize += out.len() as u64;
            at += out.len() as u64;
        }
        info.crc = crc.finalize();
        // (_ZipWriteFile.close: an entry opened without zip64 may not pass the limit)
        if info.size > ZIP64_LIMIT {
            return Err(ZipErr::Py(
                "RuntimeError: File size too large, try using force_zip64".into(),
            ));
        }
        if info.csize > ZIP64_LIMIT {
            return Err(ZipErr::Py(
                "RuntimeError: Compressed size too large, try using force_zip64".into(),
            ));
        }
        self.f.seek(SeekFrom::Start(info.offset))?;
        self.f.write_all(&file_header(&info))?;
        self.pos = at;
        self.done.push(info);
        Ok(())
    }

    /// ZipFile.close(): the central directory and end records.
    pub fn finish(mut self) -> io::Result<()> {
        let start = self.pos;
        let mut cd = Vec::new();
        for i in &self.done {
            let mut extra64: Vec<u64> = Vec::new();
            let (mut size, mut csize) = (i.size, i.csize);
            if i.size > ZIP64_LIMIT || i.csize > ZIP64_LIMIT {
                extra64.extend([i.size, i.csize]);
                size = 0xffff_ffff;
                csize = 0xffff_ffff;
            }
            let mut offset = i.offset;
            if i.offset > ZIP64_LIMIT {
                extra64.push(i.offset);
                offset = 0xffff_ffff;
            }
            let mut extra = Vec::new();
            let mut version = 20u8;
            if !extra64.is_empty() {
                le16(&mut extra, 1);
                le16(&mut extra, 8 * extra64.len() as u16);
                for x in &extra64 {
                    extra.extend(x.to_le_bytes());
                }
                extra.extend(strip_extra(&i.extra, 1));
                version = 45;
            } else {
                extra.extend(&i.extra);
            }
            cd.extend(b"PK\x01\x02");
            cd.extend([version, 3, version, 0]);
            le16(&mut cd, i.flags);
            le16(&mut cd, i.method);
            le16(&mut cd, i.time);
            le16(&mut cd, i.date);
            le32(&mut cd, i.crc);
            le32(&mut cd, csize as u32);
            le32(&mut cd, size as u32);
            le16(&mut cd, i.name.len() as u16);
            le16(&mut cd, extra.len() as u16);
            le16(&mut cd, 0); // comment
            le16(&mut cd, 0); // disk
            le16(&mut cd, 0); // internal attributes
            le32(&mut cd, i.external);
            le32(&mut cd, offset as u32);
            cd.extend(&i.name);
            cd.extend(&extra);
        }
        let pos2 = start + cd.len() as u64;
        let (mut count, mut size, mut offset) = (self.done.len() as u64, cd.len() as u64, start);
        if self.done.len() > FILECOUNT_LIMIT || start > ZIP64_LIMIT || size > ZIP64_LIMIT {
            cd.extend(b"PK\x06\x06");
            cd.extend(44u64.to_le_bytes());
            le16(&mut cd, 45);
            le16(&mut cd, 45);
            le32(&mut cd, 0);
            le32(&mut cd, 0);
            cd.extend(count.to_le_bytes());
            cd.extend(count.to_le_bytes());
            cd.extend(size.to_le_bytes());
            cd.extend(offset.to_le_bytes());
            cd.extend(b"PK\x06\x07");
            le32(&mut cd, 0);
            cd.extend(pos2.to_le_bytes());
            le32(&mut cd, 1);
            count = count.min(0xffff);
            size = size.min(0xffff_ffff);
            offset = offset.min(0xffff_ffff);
        }
        cd.extend(b"PK\x05\x06");
        le16(&mut cd, 0);
        le16(&mut cd, 0);
        le16(&mut cd, count as u16);
        le16(&mut cd, count as u16);
        le32(&mut cd, size as u32);
        le32(&mut cd, offset as u32);
        le16(&mut cd, 0);
        self.f.seek(SeekFrom::Start(start))?;
        self.f.write_all(&cd)?;
        let end = start + cd.len() as u64;
        self.f.set_len(end)?;
        self.f.flush()
    }
}

/// zipfile._strip_extra: the extra field without the blocks of the given id.
fn strip_extra(extra: &[u8], id: u16) -> Vec<u8> {
    let mut out = Vec::new();
    let mut i = 0;
    let mut start = 0;
    let mut cut = false;
    while i + 4 <= extra.len() {
        let xid = u16::from_le_bytes([extra[i], extra[i + 1]]);
        let xlen = u16::from_le_bytes([extra[i + 2], extra[i + 3]]) as usize;
        let j = i + 4 + xlen;
        if xid == id {
            if i != start {
                out.extend(&extra[start..i]);
            }
            start = j;
            cut = true;
        }
        i = j;
    }
    if !cut {
        return extra.to_vec();
    }
    if start < extra.len() {
        out.extend(&extra[start..]);
    }
    out
}

/// The zipalign-style extra field apk.py gives a stored entry whose header starts at `pos`:
/// its data 4-byte aligned (`len(arc.encode())`: the whole name, as the Python reckons it).
pub fn align_extra(pos: u64, name: &str) -> Vec<u8> {
    let off = pos + 30 + name.len() as u64;
    let pad = (4 - (off + 4) % 4) % 4;
    let mut e = vec![0x35, 0xD9];
    e.extend((pad as u16).to_le_bytes());
    e.resize(4 + pad as usize, 0);
    e
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strip_extra_as_zipfile() {
        let e = [1u8, 0, 2, 0, 9, 9, 0x35, 0xd9, 1, 0, 0];
        assert_eq!(strip_extra(&e, 1), vec![0x35, 0xd9, 1, 0, 0]);
        assert_eq!(strip_extra(&e, 7), e.to_vec());
    }

    #[test]
    fn deflate_empty_is_zlibs() {
        let mut d = Deflate::new().unwrap();
        let mut out = Vec::new();
        d.run(&[], true, &mut out).unwrap();
        assert_eq!(out, vec![0x03, 0x00]);
    }
}
