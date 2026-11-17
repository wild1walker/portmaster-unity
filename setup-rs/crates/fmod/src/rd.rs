//! A little-endian reader over a byte slice; every read past the end is an error. FMOD's counts
//! (X16) and lists are read as the bank format 0x67 has them (docs/fmod-player.md).
pub type Result<T> = std::result::Result<T, String>;

pub struct Rd<'a> {
    pub b: &'a [u8],
    pub p: usize,
}

impl<'a> Rd<'a> {
    pub fn new(b: &'a [u8]) -> Self {
        Rd { b, p: 0 }
    }
    pub fn left(&self) -> usize {
        self.b.len() - self.p
    }
    pub fn take(&mut self, n: usize) -> Result<&'a [u8]> {
        if n > self.left() {
            return Err(format!("{n} bytes past the end at {}", self.p));
        }
        let s = &self.b[self.p..self.p + n];
        self.p += n;
        Ok(s)
    }
    pub fn skip(&mut self, n: usize) -> Result<()> {
        self.take(n).map(|_| ())
    }
    pub fn u8(&mut self) -> Result<u8> {
        Ok(self.take(1)?[0])
    }
    pub fn u16(&mut self) -> Result<u16> {
        Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap()))
    }
    pub fn i16(&mut self) -> Result<i16> {
        Ok(self.u16()? as i16)
    }
    pub fn u32(&mut self) -> Result<u32> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    pub fn i32(&mut self) -> Result<i32> {
        Ok(self.u32()? as i32)
    }
    pub fn f32(&mut self) -> Result<f32> {
        Ok(f32::from_bits(self.u32()?))
    }
    pub fn f64(&mut self) -> Result<f64> {
        Ok(f64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
    pub fn u64(&mut self) -> Result<u64> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
    /// A GUID: its 16 bytes as the bank stores them (FMOD's GUID struct, little-endian fields:
    /// the same bytes as a .NET Guid in memory).
    pub fn guid(&mut self) -> Result<[u8; 16]> {
        Ok(self.take(16)?.try_into().unwrap())
    }
    /// A bank's count: 15 bits, or 30 when the top bit of the first u16 is set (the next u16 has
    /// the high bits).
    pub fn x16(&mut self) -> Result<u32> {
        let lo = self.u16()? as u32;
        if lo & 0x8000 != 0 {
            let hi = self.u16()? as u32;
            return Ok((lo & 0x7fff) | (hi << 15));
        }
        Ok(lo)
    }
    /// A string: its X16 length, then UTF-8 bytes.
    pub fn string(&mut self) -> Result<String> {
        let n = self.x16()? as usize;
        Ok(String::from_utf8_lossy(self.take(n)?).into_owned())
    }
    /// A list: X16 (count * 2 + 1), then (when not empty) a u16 element size and the elements.
    pub fn list<T>(&mut self, mut f: impl FnMut(&mut Self) -> Result<T>) -> Result<Vec<T>> {
        let n = (self.x16()? >> 1) as usize;
        if n == 0 {
            return Ok(Vec::new());
        }
        let size = self.u16()? as usize;
        if n.saturating_mul(size.max(1)) > self.left() {
            return Err(format!("a list of {n} x {size} bytes does not fit at {}", self.p));
        }
        (0..n).map(|_| f(self)).collect()
    }
    /// A list whose elements each carry their own u16 size.
    pub fn vlist<T>(&mut self, mut f: impl FnMut(&mut Self) -> Result<T>) -> Result<Vec<T>> {
        let n = (self.x16()? >> 1) as usize;
        let mut v = Vec::new();
        for _ in 0..n {
            let size = self.u16()? as usize;
            let end = self.p + size;
            let x = f(self)?;
            if self.p != end {
                return Err(format!("an element of {size} bytes read as {}", self.p + size - end));
            }
            v.push(x);
        }
        Ok(v)
    }
}
