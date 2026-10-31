//! Endian-aware reading from a byte slice and writing to a byte vector (UnityPy's
//! EndianBinaryReader / EndianBinaryWriter): alignment is relative to the start of the slice or
//! vector, as UnityPy's is to its stream's.
use crate::error::{bad, Result};

/// A cursor over bytes; `big` selects big-endian numbers.
#[derive(Clone)]
pub struct Reader<'a> {
    pub data: &'a [u8],
    pub pos: usize,
    pub big: bool,
}

macro_rules! rd {
    ($name:ident, $t:ty) => {
        pub fn $name(&mut self) -> Result<$t> {
            const N: usize = std::mem::size_of::<$t>();
            let b: [u8; N] = self.take(N)?.try_into().unwrap();
            Ok(if self.big {
                <$t>::from_be_bytes(b)
            } else {
                <$t>::from_le_bytes(b)
            })
        }
    };
}

impl<'a> Reader<'a> {
    pub fn new(data: &'a [u8], big: bool) -> Self {
        Reader { data, pos: 0, big }
    }

    pub fn remaining(&self) -> usize {
        self.data.len().saturating_sub(self.pos)
    }

    /// The next n bytes.
    pub fn take(&mut self, n: usize) -> Result<&'a [u8]> {
        match self.pos.checked_add(n) {
            Some(end) if end <= self.data.len() => {
                let s = &self.data[self.pos..end];
                self.pos = end;
                Ok(s)
            }
            _ => bad(format!(
                "read of {n} bytes at {} past the end ({})",
                self.pos,
                self.data.len()
            )),
        }
    }

    rd!(u16, u16);
    rd!(i16, i16);
    rd!(u32, u32);
    rd!(i32, i32);
    rd!(u64, u64);
    rd!(i64, i64);

    pub fn u8(&mut self) -> Result<u8> {
        Ok(self.take(1)?[0])
    }

    pub fn bool(&mut self) -> Result<bool> {
        Ok(self.u8()? != 0)
    }

    /// Advances to a multiple of n from the start.
    pub fn align(&mut self, n: usize) {
        self.pos += (n - self.pos % n) % n;
    }

    /// A NUL-terminated string's bytes (without the NUL); UnityPy's read_string_to_null stops at
    /// the end of the data too.
    pub fn cstr(&mut self) -> Result<&'a [u8]> {
        let rest = &self.data[self.pos.min(self.data.len())..];
        match rest.iter().position(|&c| c == 0) {
            Some(i) => {
                self.pos += i + 1;
                Ok(&rest[..i])
            }
            None => bad(format!("string at {} not terminated", self.pos)),
        }
    }

    /// A NUL-terminated string as text.
    pub fn cstring(&mut self) -> Result<String> {
        let b = self.cstr()?;
        match std::str::from_utf8(b) {
            Ok(s) => Ok(s.to_string()),
            Err(_) => bad(format!("string {b:?} is not UTF-8")),
        }
    }
}

/// Bytes being written; `big` selects big-endian numbers.
#[derive(Default, Clone)]
pub struct Writer {
    pub buf: Vec<u8>,
    pub big: bool,
}

macro_rules! wr {
    ($name:ident, $t:ty) => {
        pub fn $name(&mut self, v: $t) {
            if self.big {
                self.buf.extend_from_slice(&v.to_be_bytes())
            } else {
                self.buf.extend_from_slice(&v.to_le_bytes())
            }
        }
    };
}

impl Writer {
    pub fn new(big: bool) -> Self {
        Writer {
            buf: Vec::new(),
            big,
        }
    }

    wr!(u16, u16);
    wr!(i16, i16);
    wr!(u32, u32);
    wr!(i32, i32);
    wr!(u64, u64);
    wr!(i64, i64);

    pub fn u8(&mut self, v: u8) {
        self.buf.push(v);
    }

    pub fn bytes(&mut self, b: &[u8]) {
        self.buf.extend_from_slice(b);
    }

    /// A NUL-terminated string.
    pub fn cstr(&mut self, b: &[u8]) {
        self.buf.extend_from_slice(b);
        self.buf.push(0);
    }

    /// Zeros up to a multiple of n.
    pub fn align(&mut self, n: usize) {
        let pad = (n - self.buf.len() % n) % n;
        self.buf.resize(self.buf.len() + pad, 0);
    }

    pub fn len(&self) -> usize {
        self.buf.len()
    }

    pub fn is_empty(&self) -> bool {
        self.buf.is_empty()
    }
}
