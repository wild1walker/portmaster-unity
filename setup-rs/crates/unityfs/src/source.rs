//! Random-access byte sources: a file (read where needed, never whole), bytes in memory, or a
//! range of another source (a bundle node).
use std::fs::File;
use std::io;
use std::os::unix::fs::FileExt;
use std::sync::Arc;

/// Bytes read at offsets.
pub trait Source: Send + Sync {
    fn len(&self) -> u64;
    /// Fills buf from offset off (an error when the source ends first).
    fn read_at(&self, off: u64, buf: &mut [u8]) -> io::Result<()>;

    fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// n bytes at off.
    fn read_vec(&self, off: u64, n: usize) -> io::Result<Vec<u8>> {
        let mut v = vec![0u8; n];
        self.read_at(off, &mut v)?;
        Ok(v)
    }

    /// Bytes [off, off + n) clipped to the end (a slice of a Python bytes/memoryview).
    fn read_clipped(&self, off: u64, n: usize) -> io::Result<Vec<u8>> {
        let end = (off.saturating_add(n as u64)).min(self.len());
        if off >= end {
            return Ok(Vec::new());
        }
        self.read_vec(off, (end - off) as usize)
    }

    /// Copies [off, off + n) to w in pieces of at most 1 MB.
    fn copy_to(&self, off: u64, n: u64, w: &mut dyn io::Write) -> io::Result<()> {
        let mut buf = vec![0u8; (1 << 20).min(n as usize)];
        let mut p = 0;
        while p < n {
            let k = (n - p).min(buf.len() as u64) as usize;
            self.read_at(off + p, &mut buf[..k])?;
            w.write_all(&buf[..k])?;
            p += k as u64;
        }
        Ok(())
    }
}

pub type SharedSource = Arc<dyn Source>;

/// A file, read with pread.
pub struct FileSource {
    f: File,
    len: u64,
}

impl FileSource {
    pub fn open(path: impl AsRef<std::path::Path>) -> io::Result<Self> {
        let f = File::open(path)?;
        let len = f.metadata()?.len();
        Ok(FileSource { f, len })
    }
}

impl Source for FileSource {
    fn len(&self) -> u64 {
        self.len
    }
    fn read_at(&self, off: u64, buf: &mut [u8]) -> io::Result<()> {
        self.f.read_exact_at(buf, off)
    }
}

/// Bytes in memory.
pub struct MemSource(pub Vec<u8>);

impl Source for MemSource {
    fn len(&self) -> u64 {
        self.0.len() as u64
    }
    fn read_at(&self, off: u64, buf: &mut [u8]) -> io::Result<()> {
        let s = usize::try_from(off)
            .ok()
            .and_then(|o| self.0.get(o..o.checked_add(buf.len())?));
        match s {
            Some(s) => {
                buf.copy_from_slice(s);
                Ok(())
            }
            None => Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "read past the end",
            )),
        }
    }
}

/// [offset, offset + size) of another source.
pub struct SubSource {
    pub inner: SharedSource,
    pub offset: u64,
    pub size: u64,
}

impl Source for SubSource {
    fn len(&self) -> u64 {
        self.size
    }
    fn read_at(&self, off: u64, buf: &mut [u8]) -> io::Result<()> {
        if off
            .checked_add(buf.len() as u64)
            .is_none_or(|e| e > self.size)
        {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "read past the end of the node",
            ));
        }
        self.inner.read_at(self.offset + off, buf)
    }
}

/// A source read sequentially (io::Read), from an offset.
pub struct SourceReader<'a> {
    pub src: &'a dyn Source,
    pub pos: u64,
    pub end: u64,
}

impl io::Read for SourceReader<'_> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = (self.end.saturating_sub(self.pos)).min(buf.len() as u64) as usize;
        if n == 0 {
            return Ok(0);
        }
        self.src.read_at(self.pos, &mut buf[..n])?;
        self.pos += n as u64;
        Ok(n)
    }
}
