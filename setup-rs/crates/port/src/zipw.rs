//! Zips as Python's zipfile writes them with ZipFile.writestr(ZipInfo, data): each entry's local
//! header (its real sizes and CRC: the file is seekable, no data descriptor), the data, the central
//! directory, the end record; deflate is raw zlib deflate at the entry's level (zlib-rs).
use std::fs::File;
use std::io::{BufWriter, Write};

use flate2::{Compress, Compression, FlushCompress, Status};
use pkgzip::{io_at, Error, Result};

pub const STORED: u16 = 0;
pub const DEFLATED: u16 = 8;
const ZIP64_LIMIT: u64 = (1 << 31) - 1;

/// A ZipInfo's fields that reach the file.
#[derive(Clone, Debug)]
pub struct Info {
    pub name: String,
    pub time: u16,
    pub date: u16,
    pub method: u16,
    /// The deflate level (None: zlib's default, 6).
    pub level: Option<u32>,
    pub create_version: u8,
    pub create_system: u8,
    pub extract_version: u8,
    pub internal_attr: u16,
    pub external_attr: u32,
    pub extra: Vec<u8>,
    pub comment: Vec<u8>,
}

impl Info {
    /// ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0)) deflated at `level`, a Unix file of `mode`.
    pub fn file(name: &str, mode: u32, level: u32) -> Info {
        let (date, time) = dos((2020, 1, 1, 0, 0, 0));
        Info {
            name: name.to_string(),
            time,
            date,
            method: DEFLATED,
            level: Some(level),
            create_version: 20,
            create_system: 3,
            extract_version: 20,
            internal_attr: 0,
            external_attr: (0o100000 | mode) << 16,
            extra: Vec::new(),
            comment: Vec::new(),
        }
    }
}

/// (dosdate, dostime) of a date_time.
pub fn dos(dt: (u32, u32, u32, u32, u32, u32)) -> (u16, u16) {
    (
        (((dt.0 - 1980) << 9) | (dt.1 << 5) | dt.2) as u16,
        ((dt.3 << 11) | (dt.4 << 5) | (dt.5 / 2)) as u16,
    )
}

struct Central {
    info: Info,
    flags: u16,
    crc: u32,
    csize: u32,
    size: u32,
    offset: u32,
}

pub struct ZipOut {
    f: BufWriter<File>,
    path: String,
    pos: u64,
    entries: Vec<Central>,
}

/// zlib.compressobj(level, DEFLATED, -15): raw deflate of data, one call and a flush.
pub fn deflate(data: &[u8], level: u32) -> Result<Vec<u8>> {
    let mut c = Compress::new(Compression::new(level), false);
    let mut out = Vec::with_capacity(data.len() / 2 + 64);
    loop {
        let before = c.total_in() as usize;
        let st = c
            .compress_vec(&data[before..], &mut out, FlushCompress::Finish)
            .map_err(|e| Error::Fail(format!("deflate: {e}")))?;
        match st {
            Status::StreamEnd => return Ok(out),
            _ => out.reserve(out.capacity().max(4096)),
        }
    }
}

impl ZipOut {
    pub fn create(path: &str) -> Result<ZipOut> {
        let f = File::create(path).map_err(io_at(path))?;
        Ok(ZipOut { f: BufWriter::new(f), path: path.to_string(), pos: 0, entries: Vec::new() })
    }

    fn put(&mut self, b: &[u8]) -> Result<()> {
        self.f.write_all(b).map_err(io_at(&self.path))?;
        self.pos += b.len() as u64;
        Ok(())
    }

    /// writestr(info, data)
    pub fn add(&mut self, mut info: Info, data: &[u8]) -> Result<()> {
        if data.len() as f64 * 1.05 > ZIP64_LIMIT as f64 || self.pos > ZIP64_LIMIT {
            return Err(Error::Fail(format!("{}: {} would need zip64 (the packager writes none)", self.path, info.name)));
        }
        if info.external_attr == 0 {
            info.external_attr = 0o600 << 16;
        }
        let crc = crc32fast::hash(data);
        let packed;
        let body: &[u8] = match info.method {
            STORED => data,
            DEFLATED => {
                packed = deflate(data, info.level.unwrap_or(6))?;
                &packed
            }
            m => return Err(Error::Fail(format!("compression method {m} is not written by the packager"))),
        };
        let name = info.name.as_bytes().to_vec();
        let flags: u16 = if info.name.is_ascii() { 0 } else { 0x800 };
        let offset = self.pos as u32;
        let mut h = Vec::with_capacity(30 + name.len() + info.extra.len());
        h.extend_from_slice(b"PK\x03\x04");
        h.push(info.extract_version);
        h.push(0);
        h.extend_from_slice(&flags.to_le_bytes());
        h.extend_from_slice(&info.method.to_le_bytes());
        h.extend_from_slice(&info.time.to_le_bytes());
        h.extend_from_slice(&info.date.to_le_bytes());
        h.extend_from_slice(&crc.to_le_bytes());
        h.extend_from_slice(&(body.len() as u32).to_le_bytes());
        h.extend_from_slice(&(data.len() as u32).to_le_bytes());
        h.extend_from_slice(&(name.len() as u16).to_le_bytes());
        h.extend_from_slice(&(info.extra.len() as u16).to_le_bytes());
        h.extend_from_slice(&name);
        h.extend_from_slice(&info.extra);
        self.put(&h)?;
        self.put(body)?;
        self.entries.push(Central { flags, crc, csize: body.len() as u32, size: data.len() as u32, offset, info });
        Ok(())
    }

    /// close(): the central directory and the end record.
    pub fn finish(mut self) -> Result<()> {
        let start = self.pos;
        let entries = std::mem::take(&mut self.entries);
        for e in &entries {
            let i = &e.info;
            let name = i.name.as_bytes();
            let mut c = Vec::with_capacity(46 + name.len());
            c.extend_from_slice(b"PK\x01\x02");
            c.push(i.create_version);
            c.push(i.create_system);
            c.push(i.extract_version);
            c.push(0);
            c.extend_from_slice(&e.flags.to_le_bytes());
            c.extend_from_slice(&i.method.to_le_bytes());
            c.extend_from_slice(&i.time.to_le_bytes());
            c.extend_from_slice(&i.date.to_le_bytes());
            c.extend_from_slice(&e.crc.to_le_bytes());
            c.extend_from_slice(&e.csize.to_le_bytes());
            c.extend_from_slice(&e.size.to_le_bytes());
            c.extend_from_slice(&(name.len() as u16).to_le_bytes());
            c.extend_from_slice(&(i.extra.len() as u16).to_le_bytes());
            c.extend_from_slice(&(i.comment.len() as u16).to_le_bytes());
            c.extend_from_slice(&0u16.to_le_bytes());
            c.extend_from_slice(&i.internal_attr.to_le_bytes());
            c.extend_from_slice(&i.external_attr.to_le_bytes());
            c.extend_from_slice(&e.offset.to_le_bytes());
            c.extend_from_slice(name);
            c.extend_from_slice(&i.extra);
            c.extend_from_slice(&i.comment);
            self.put(&c)?;
        }
        let size = self.pos - start;
        if entries.len() > 0xFFFF || start > ZIP64_LIMIT || size > ZIP64_LIMIT {
            return Err(Error::Fail(format!("{}: would need zip64 (the packager writes none)", self.path)));
        }
        let mut end = Vec::with_capacity(22);
        end.extend_from_slice(b"PK\x05\x06");
        end.extend_from_slice(&[0, 0, 0, 0]);
        end.extend_from_slice(&(entries.len() as u16).to_le_bytes());
        end.extend_from_slice(&(entries.len() as u16).to_le_bytes());
        end.extend_from_slice(&(size as u32).to_le_bytes());
        end.extend_from_slice(&(start as u32).to_le_bytes());
        end.extend_from_slice(&0u16.to_le_bytes());
        self.put(&end)?;
        self.f.flush().map_err(io_at(&self.path))?;
        Ok(())
    }
}
