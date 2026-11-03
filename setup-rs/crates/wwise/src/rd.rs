//! A little-endian reader over a byte slice; every read past the end is an error.
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
    /// Wwise's variable-length count: 7 bits a byte, most significant first, the top bit set on
    /// every byte but the last.
    pub fn var(&mut self) -> Result<u32> {
        let mut v = 0u32;
        for _ in 0..5 {
            let c = self.u8()?;
            v = (v << 7) | (c & 0x7f) as u32;
            if c & 0x80 == 0 {
                return Ok(v);
            }
        }
        Err(format!("a variable-length number too long at {}", self.p))
    }
    /// A count of items at least `min` bytes each: an error when the rest cannot hold them.
    pub fn count(&mut self, n: u32, min: usize) -> Result<usize> {
        let n = n as usize;
        if n.saturating_mul(min) > self.left() {
            return Err(format!("{n} items do not fit at {}", self.p));
        }
        Ok(n)
    }
    pub fn ids(&mut self, n: u32) -> Result<Vec<u32>> {
        let n = self.count(n, 4)?;
        (0..n).map(|_| self.u32()).collect()
    }
}
