//! The readers macpkg.py chains to get at an installer's files: a region of the package file, the
//! xar encodings (zlib, bzip2), the payload framings (gzip, Apple's pbzx with xz chunks) and a
//! read-ahead thread. Each one reads what the Python's does and fails where it fails: errors past
//! the point a reader stops at (the cpio trailer) are never seen, as in the Python.
use std::fs::File;
use std::io::{self, Cursor, Read};
use std::os::unix::fs::FileExt;
use std::sync::mpsc::{sync_channel, Receiver};
use std::sync::Arc;
use std::thread::JoinHandle;

use flate2::{Decompress, FlushDecompress, Status};

use crate::py;

/// What a payload stream is: any reader that can move to the read-ahead thread.
pub type Stream = Box<dyn Read + Send>;

const PIECE: usize = 1 << 20;

/// An error as the Python's exception reads (its class and message).
pub fn err(msg: impl Into<String>) -> io::Error {
    io::Error::other(msg.into())
}

const EOF_MARKER: &str =
    "EOFError: Compressed file ended before the end-of-stream marker was reached";

/// Reads up to n bytes (fewer only at the end), as Python's read(n) on these streams.
pub fn read_upto(r: &mut dyn Read, n: usize) -> io::Result<Vec<u8>> {
    let mut v = Vec::new();
    r.take(n as u64).read_to_end(&mut v)?;
    Ok(v)
}

/// Bytes [start, start+len) of an open file (macpkg.Region), read in place (no shared position).
#[derive(Clone)]
pub struct Region {
    file: Arc<File>,
    start: u64,
    len: u64,
    pos: u64,
}

impl Region {
    pub fn new(file: Arc<File>, start: u64, len: u64) -> Self {
        Region {
            file,
            start,
            len,
            pos: 0,
        }
    }
}

impl Read for Region {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = (self.len - self.pos).min(buf.len() as u64) as usize;
        if n == 0 {
            return Ok(0);
        }
        let got = self.file.read_at(&mut buf[..n], self.start + self.pos)?;
        self.pos += got as u64;
        Ok(got)
    }
}

/// Input read in 1 MB blocks, consumed from the front (what zlib's decompressobj keeps as its
/// unconsumed input), with single bytes and exact reads for headers.
struct Input<R> {
    r: R,
    buf: Box<[u8]>,
    pos: usize,
    end: usize,
}

impl<R: Read> Input<R> {
    fn new(r: R) -> Self {
        Input {
            r,
            buf: vec![0; PIECE].into_boxed_slice(),
            pos: 0,
            end: 0,
        }
    }

    /// The bytes waiting; empty at the end of the input.
    fn fill(&mut self) -> io::Result<&[u8]> {
        if self.pos == self.end {
            self.end = loop {
                match self.r.read(&mut self.buf) {
                    Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                    r => break r?,
                }
            };
            self.pos = 0;
        }
        Ok(&self.buf[self.pos..self.end])
    }

    fn byte(&mut self) -> io::Result<Option<u8>> {
        let b = self.fill()?.first().copied();
        if b.is_some() {
            self.pos += 1;
        }
        Ok(b)
    }

    /// Up to n bytes (fewer at the end).
    fn upto(&mut self, n: usize) -> io::Result<Vec<u8>> {
        let mut v = Vec::with_capacity(n.min(PIECE));
        while v.len() < n {
            let b = self.fill()?;
            if b.is_empty() {
                break;
            }
            let k = b.len().min(n - v.len());
            v.extend_from_slice(&b[..k]);
            self.pos += k;
        }
        Ok(v)
    }

    /// gzip's _read_exact: n bytes, or the EOFError of a stream cut short.
    fn exact(&mut self, n: usize) -> io::Result<Vec<u8>> {
        let v = self.upto(n)?;
        if v.len() < n {
            return Err(err(EOF_MARKER));
        }
        Ok(v)
    }
}

/// zlib's error message as Python's zlib.error gives it.
fn zlib_err(e: flate2::DecompressError) -> io::Error {
    err(format!(
        "zlib.error: Error -3 while decompressing data: {e}"
    ))
}

/// One inflate step: (status, bytes consumed, bytes produced).
fn inflate(z: &mut Decompress, inp: &[u8], out: &mut [u8]) -> io::Result<(Status, usize, usize)> {
    let (i0, o0) = (z.total_in(), z.total_out());
    let st = z
        .decompress(inp, out, FlushDecompress::None)
        .map_err(zlib_err)?;
    Ok((
        st,
        (z.total_in() - i0) as usize,
        (z.total_out() - o0) as usize,
    ))
}

/// xar's "application/x-gzip" encoding, which is a zlib stream (macpkg.Inflate): what follows the
/// stream's end is ignored, and a stream cut short ends where its data does (decompressobj's
/// flush() raises nothing).
pub struct Inflate<R> {
    input: Input<R>,
    z: Decompress,
    ended: bool,
}

impl<R: Read> Inflate<R> {
    pub fn new(r: R) -> Self {
        Inflate {
            input: Input::new(r),
            z: Decompress::new(true),
            ended: false,
        }
    }
}

impl<R: Read> Read for Inflate<R> {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        if out.is_empty() {
            return Ok(0);
        }
        while !self.ended {
            let inp = self.input.fill()?;
            let at_end = inp.is_empty();
            let (st, used, made) = inflate(&mut self.z, inp, out)?;
            self.input.pos += used;
            if st == Status::StreamEnd {
                self.ended = true;
            }
            if made > 0 {
                return Ok(made);
            }
            if at_end {
                break;
            }
        }
        Ok(0)
    }
}

#[derive(PartialEq)]
enum Gz {
    Header,
    Data,
    Trailer,
    End,
}

/// A gzip file as Python's gzip.GzipFile reads it: members one after another (zeros between them
/// skipped), each one's CRC and length checked at its end, and the same errors where the stream
/// is not gzip or ends early.
pub struct Gzip<R> {
    input: Input<R>,
    z: Decompress,
    state: Gz,
    crc: crc32fast::Hasher,
    size: u64,
}

impl<R: Read> Gzip<R> {
    pub fn new(r: R) -> Self {
        Gzip {
            input: Input::new(r),
            z: Decompress::new(false),
            state: Gz::Header,
            crc: crc32fast::Hasher::new(),
            size: 0,
        }
    }

    /// gzip._read_gzip_header: false at the end of the file.
    fn header(&mut self) -> io::Result<bool> {
        let magic = self.input.upto(2)?;
        if magic.is_empty() {
            return Ok(false);
        }
        if magic != b"\x1f\x8b" {
            return Err(err(format!(
                "gzip.BadGzipFile: Not a gzipped file ({})",
                py::bytes_repr(&magic)
            )));
        }
        let h = self.input.exact(8)?;
        if h[0] != 8 {
            return Err(err("gzip.BadGzipFile: Unknown compression method"));
        }
        let flag = h[1];
        if flag & 4 != 0 {
            let n = self.input.exact(2)?;
            self.input
                .exact(u16::from_le_bytes([n[0], n[1]]) as usize)?;
        }
        for bit in [8, 16] {
            if flag & bit != 0 {
                while let Some(c) = self.input.byte()? {
                    if c == 0 {
                        break;
                    }
                }
            }
        }
        if flag & 2 != 0 {
            self.input.exact(2)?;
        }
        Ok(true)
    }

    /// gzip's _read_eof: the member's CRC and length, then the zeros that may pad it.
    fn trailer(&mut self) -> io::Result<()> {
        let t = self.input.exact(8)?;
        let crc = u32::from_le_bytes(t[0..4].try_into().unwrap());
        let isize = u32::from_le_bytes(t[4..8].try_into().unwrap());
        let have = std::mem::take(&mut self.crc).finalize();
        if crc != have {
            return Err(err(format!(
                "gzip.BadGzipFile: CRC check failed {crc:#x} != {have:#x}"
            )));
        }
        if isize != self.size as u32 {
            return Err(err("gzip.BadGzipFile: Incorrect length of data produced"));
        }
        while let Some(c) = self.input.byte()? {
            if c != 0 {
                self.input.pos -= 1; // (prepended back, as _PaddedFile.prepend)
                break;
            }
        }
        Ok(())
    }
}

impl<R: Read> Read for Gzip<R> {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        if out.is_empty() {
            return Ok(0);
        }
        loop {
            match self.state {
                Gz::End => return Ok(0),
                Gz::Header => {
                    if !self.header()? {
                        self.state = Gz::End;
                        continue;
                    }
                    self.z.reset(false);
                    self.crc = crc32fast::Hasher::new();
                    self.size = 0;
                    self.state = Gz::Data;
                }
                Gz::Trailer => {
                    self.trailer()?;
                    self.state = Gz::Header;
                }
                Gz::Data => {
                    let inp = self.input.fill()?;
                    let at_end = inp.is_empty();
                    let (st, used, made) = inflate(&mut self.z, inp, out)?;
                    self.input.pos += used;
                    self.crc.update(&out[..made]);
                    self.size += made as u64;
                    if st == Status::StreamEnd {
                        self.state = Gz::Trailer;
                    }
                    if made > 0 {
                        return Ok(made);
                    }
                    if at_end && self.state == Gz::Data {
                        return Err(err(EOF_MARKER));
                    }
                }
            }
        }
    }
}

/// xar's "application/x-bzip2" encoding as Python's bz2.BZ2File reads it: streams one after
/// another; data after the last one that is not a stream is ignored.
pub struct Bzip2<R> {
    input: Input<R>,
    z: bzip2::Decompress,
    ended: bool,
    eof: bool,
}

impl<R: Read> Bzip2<R> {
    pub fn new(r: R) -> Self {
        Bzip2 {
            input: Input::new(r),
            z: bzip2::Decompress::new(false),
            ended: false,
            eof: false,
        }
    }

    fn step(&mut self, out: &mut [u8]) -> io::Result<usize> {
        let inp = self.input.fill()?;
        let (i0, o0) = (self.z.total_in(), self.z.total_out());
        let st = self
            .z
            .decompress(inp, out)
            .map_err(|e| err(format!("OSError: {e}")))?;
        self.input.pos += (self.z.total_in() - i0) as usize;
        if matches!(st, bzip2::Status::StreamEnd) {
            self.ended = true;
        }
        Ok((self.z.total_out() - o0) as usize)
    }
}

impl<R: Read> Read for Bzip2<R> {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        if out.is_empty() {
            return Ok(0);
        }
        while !self.eof {
            let made = if self.ended {
                if self.input.fill()?.is_empty() {
                    self.eof = true;
                    break;
                }
                self.z = bzip2::Decompress::new(false);
                self.ended = false;
                match self.step(out) {
                    Ok(n) => n,
                    Err(_) => {
                        self.eof = true; // (trailing data that is not a stream: ignored)
                        break;
                    }
                }
            } else {
                let at_end = self.input.fill()?.is_empty();
                let n = self.step(out)?;
                if n == 0 && at_end && !self.ended {
                    return Err(err(EOF_MARKER));
                }
                n
            };
            if made > 0 {
                return Ok(made);
            }
        }
        Ok(0)
    }
}

const XZ_MAGIC: &[u8] = b"\xfd7zXZ\x00";

const ENDED: &str =
    "_lzma.LZMAError: Compressed data ended before the end-of-stream marker was reached";

/// What liblzma's format detection makes of data that does not start an xz stream: an .lzma
/// (LZMA_Alone) header, checked as its picky mode does. None: too short to tell.
fn lzma_alone_header(h: &[u8]) -> Option<bool> {
    if *h.first()? >= 225 {
        return Some(false);
    }
    let dict = u32::from_le_bytes(h.get(1..5)?.try_into().unwrap());
    if dict != u32::MAX {
        // only 2^n or 2^n + 2^(n-1)
        let mut d = dict.wrapping_sub(1);
        for s in [2, 3, 4, 8, 16] {
            d |= d >> s;
        }
        if d.wrapping_add(1) != dict {
            return Some(false);
        }
    }
    let size = u64::from_le_bytes(h.get(5..13)?.try_into().unwrap());
    Some(size == u64::MAX || size < 1 << 38)
}

/// lzma.decompress of one pbzx chunk (format auto): its streams one after another; data after the
/// first that is no stream is ignored, a stream cut short is an error.
pub fn xz_decompress(data: &[u8]) -> io::Result<Vec<u8>> {
    let mut out = Vec::new();
    let mut rest = data;
    let mut first = true;
    while !rest.is_empty() {
        let mut cur = Cursor::new(rest);
        let mut piece = Vec::new();
        let r = if rest[0] == 0xfd {
            lzma_rust2::XzReader::new(&mut cur, false).read_to_end(&mut piece)
        } else {
            match lzma_alone_header(rest) {
                None => return Err(err(ENDED)),
                Some(false) if !first => break,
                Some(false) => {
                    return Err(err(
                        "_lzma.LZMAError: Input format not supported by decoder",
                    ))
                }
                Some(true) if rest.len() < 18 => return Err(err(ENDED)),
                Some(true) => lzma_rust2::LzmaReader::new_mem_limit(&mut cur, u32::MAX, None)
                    .and_then(|mut r| r.read_to_end(&mut piece)),
            }
        };
        match r {
            Ok(_) => {}
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => return Err(err(ENDED)),
            Err(_) if !first => break,
            Err(e) => return Err(err(format!("_lzma.LZMAError: {e}"))),
        }
        out.extend_from_slice(&piece);
        rest = &rest[cur.position() as usize..];
        first = false;
    }
    Ok(out)
}

/// Apple's pbzx payload framing (macpkg.Pbzx): 'pbzx', flags, then (flags, length, data) chunks,
/// each an xz stream or stored as is. One chunk at a time in memory, as the Python.
pub struct Pbzx<R> {
    raw: R,
    buf: Vec<u8>,
    pos: usize,
    done: bool,
}

impl<R: Read> Pbzx<R> {
    pub fn new(mut raw: R) -> io::Result<Self> {
        let head = read_upto(&mut raw, 12)?;
        if !head.starts_with(b"pbzx") {
            return Err(err("ValueError: not a pbzx stream"));
        }
        Ok(Pbzx {
            raw,
            buf: Vec::new(),
            pos: 0,
            done: false,
        })
    }

    fn chunk(&mut self) -> io::Result<()> {
        let h = read_upto(&mut self.raw, 16)?;
        if h.len() < 16 {
            self.done = true;
            return Ok(());
        }
        let length = u64::from_be_bytes(h[8..16].try_into().unwrap());
        let mut data = Vec::new();
        (&mut self.raw).take(length).read_to_end(&mut data)?;
        self.buf = if data.starts_with(XZ_MAGIC) {
            xz_decompress(&data)?
        } else {
            data
        };
        self.pos = 0;
        Ok(())
    }
}

impl<R: Read> Read for Pbzx<R> {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        while self.pos == self.buf.len() && !self.done {
            self.chunk()?;
        }
        let n = out.len().min(self.buf.len() - self.pos);
        out[..n].copy_from_slice(&self.buf[self.pos..self.pos + n]);
        self.pos += n;
        Ok(n)
    }
}

/// macpkg.payload_stream: gzip, pbzx or an uncompressed cpio, by the first bytes.
pub fn payload_stream(mut raw: Stream) -> io::Result<Stream> {
    let head = read_upto(&mut raw, 4)?;
    let gz = head.starts_with(b"\x1f\x8b");
    let pbzx = head == b"pbzx";
    let p = Cursor::new(head).chain(raw);
    Ok(if gz {
        Box::new(Gzip::new(p))
    } else if pbzx {
        Box::new(Pbzx::new(p)?)
    } else {
        Box::new(p)
    })
}

/// A stream read ahead on a thread of its own (macpkg.Ahead): the decompression of the next
/// megabytes runs while the caller writes the last ones. At most 4 pieces of 1 MB wait; an error
/// reaches the caller only when it reads up to it.
pub struct Ahead {
    rx: Option<Receiver<io::Result<Vec<u8>>>>,
    cur: Vec<u8>,
    pos: usize,
    eof: bool,
    t: Option<JoinHandle<()>>,
}

impl Ahead {
    pub fn new(mut s: Stream) -> Self {
        let (tx, rx) = sync_channel::<io::Result<Vec<u8>>>(4);
        let t = std::thread::spawn(move || loop {
            let mut b = vec![0u8; PIECE];
            let mut n = 0;
            let mut fault = None;
            while n < b.len() {
                match s.read(&mut b[n..]) {
                    Ok(0) => break,
                    Ok(k) => n += k,
                    Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
                    Err(e) => {
                        fault = Some(e);
                        break;
                    }
                }
            }
            b.truncate(n);
            let end = n == 0;
            if n > 0 && tx.send(Ok(b)).is_err() {
                return;
            }
            if let Some(e) = fault {
                let _ = tx.send(Err(e));
                return;
            }
            if end {
                let _ = tx.send(Ok(Vec::new()));
                return;
            }
        });
        Ahead {
            rx: Some(rx),
            cur: Vec::new(),
            pos: 0,
            eof: false,
            t: Some(t),
        }
    }
}

impl Read for Ahead {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        loop {
            if self.pos < self.cur.len() {
                let n = out.len().min(self.cur.len() - self.pos);
                out[..n].copy_from_slice(&self.cur[self.pos..self.pos + n]);
                self.pos += n;
                return Ok(n);
            }
            if self.eof {
                return Ok(0);
            }
            match self.rx.as_ref().map(|rx| rx.recv()) {
                Some(Ok(Ok(b))) if !b.is_empty() => {
                    self.cur = b;
                    self.pos = 0;
                }
                Some(Ok(Err(e))) => {
                    self.eof = true;
                    return Err(e);
                }
                _ => self.eof = true,
            }
        }
    }
}

impl Drop for Ahead {
    /// Stops the thread (it ends at its next piece) and waits for it, as Ahead.close().
    fn drop(&mut self) {
        self.rx.take();
        if let Some(t) = self.t.take() {
            let _ = t.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::write::{GzEncoder, ZlibEncoder};
    use flate2::Compression;
    use std::io::Write;

    fn gz(data: &[u8]) -> Vec<u8> {
        let mut e = GzEncoder::new(Vec::new(), Compression::default());
        e.write_all(data).unwrap();
        e.finish().unwrap()
    }

    fn all(mut r: impl Read) -> io::Result<Vec<u8>> {
        let mut v = Vec::new();
        r.read_to_end(&mut v)?;
        Ok(v)
    }

    #[test]
    fn gzip_members_and_padding() {
        let mut d = gz(b"hello ");
        d.extend(gz(b"world"));
        d.extend([0u8; 7]);
        assert_eq!(all(Gzip::new(&d[..])).unwrap(), b"hello world");
        assert_eq!(all(Gzip::new(&b""[..])).unwrap(), b"");
    }

    #[test]
    fn gzip_errors() {
        let d = gz(&[7u8; 5000]);
        let cut = all(Gzip::new(&d[..d.len() - 4])).unwrap_err().to_string();
        assert!(cut.starts_with("EOFError"), "{cut}");
        let mut bad = d.clone();
        let n = bad.len();
        bad[n - 8] ^= 1;
        let e = all(Gzip::new(&bad[..])).unwrap_err().to_string();
        assert!(e.contains("CRC check failed"), "{e}");
        let mut junk = d.clone();
        junk.extend(b"xy");
        let e = all(Gzip::new(&junk[..])).unwrap_err().to_string();
        assert!(e.contains("Not a gzipped file (b'xy')"), "{e}");
    }

    #[test]
    fn inflate_is_lenient() {
        let mut e = ZlibEncoder::new(Vec::new(), Compression::default());
        e.write_all(&[1u8; 100_000]).unwrap();
        let mut z = e.finish().unwrap();
        z.extend(b"trailing");
        assert_eq!(all(Inflate::new(&z[..])).unwrap(), vec![1u8; 100_000]);
        let cut = all(Inflate::new(&z[..z.len() / 2])).unwrap();
        assert!(cut.len() < 100_000 && cut.iter().all(|&b| b == 1));
    }

    #[test]
    fn bzip2_streams_and_trailing_data() {
        let enc = |d: &[u8]| {
            let mut e = bzip2::write::BzEncoder::new(Vec::new(), bzip2::Compression::default());
            e.write_all(d).unwrap();
            e.finish().unwrap()
        };
        let mut d = enc(b"one ");
        d.extend(enc(b"two"));
        d.extend(b"not bzip2");
        assert_eq!(all(Bzip2::new(&d[..])).unwrap(), b"one two");
        let one = enc(&[3u8; 10_000]);
        assert!(all(Bzip2::new(&one[..one.len() - 3])).is_err());
    }

    #[test]
    fn pbzx_chunks() {
        let mut xz = Vec::new();
        let mut w =
            lzma_rust2::XzWriter::new(&mut xz, lzma_rust2::XzOptions::with_preset(1)).unwrap();
        w.write_all(b"compressed ").unwrap();
        w.finish().unwrap();
        let mut d = b"pbzx".to_vec();
        d.extend(1u64.to_be_bytes());
        for c in [&xz[..], b"stored"] {
            d.extend(0u64.to_be_bytes());
            d.extend((c.len() as u64).to_be_bytes());
            d.extend(c);
        }
        d.extend([0u8; 5]); // (a short header: the end)
        let s = payload_stream(Box::new(Cursor::new(d))).unwrap();
        assert_eq!(all(s).unwrap(), b"compressed stored");
        // as lzma.decompress: two streams; data after them that is no stream ignored, unless it
        // could start one (zeros: an .lzma header)
        let cat = |tail: &[u8]| [&xz[..], tail].concat();
        assert_eq!(xz_decompress(&cat(&xz)).unwrap(), b"compressed compressed ");
        assert_eq!(
            xz_decompress(&cat(b"garbage that is long enough")).unwrap(),
            b"compressed "
        );
        assert_eq!(xz_decompress(&cat(&[0xff; 20])).unwrap(), b"compressed ");
        for bad in [&[0u8; 4][..], &[0u8; 13], &xz[..20]] {
            assert!(xz_decompress(&cat(bad))
                .unwrap_err()
                .to_string()
                .contains("ended before"));
        }
        assert!(xz_decompress(&xz[..xz.len() - 5]).is_err());
    }

    #[test]
    fn ahead_delivers_then_fails() {
        struct Fails(usize);
        impl Read for Fails {
            fn read(&mut self, b: &mut [u8]) -> io::Result<usize> {
                if self.0 == 0 {
                    return Err(err("boom"));
                }
                let n = b.len().min(self.0);
                b[..n].fill(9);
                self.0 -= n;
                Ok(n)
            }
        }
        let mut a = Ahead::new(Box::new(Fails(3 << 20)));
        let mut v = vec![0u8; 3 << 20];
        a.read_exact(&mut v).unwrap();
        assert!(v.iter().all(|&b| b == 9));
        assert_eq!(a.read(&mut [0u8; 1]).unwrap_err().to_string(), "boom");
        let mut b = Ahead::new(Box::new(Fails(10 << 20)));
        b.read_exact(&mut [0u8; 10]).unwrap();
        drop(b); // (stops the thread while it waits to hand pieces over)
    }
}
