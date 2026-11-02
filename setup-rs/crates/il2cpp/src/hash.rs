//! hashlib's sha256 (the sha2 crate) and sha1 (Bee's input digests), as hex.
use crate::util::at;
use crate::Result;
use sha2::Digest;
use std::io::Read;

pub fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

pub fn sha256_hex(data: &[u8]) -> String {
    hex(&sha2::Sha256::digest(data))
}

/// A sha256 fed piece by piece.
pub struct Sha256(sha2::Sha256);

impl Default for Sha256 {
    fn default() -> Self {
        Sha256(sha2::Sha256::new())
    }
}

impl Sha256 {
    pub fn update(&mut self, b: &[u8]) {
        self.0.update(b);
    }

    pub fn hexdigest(self) -> String {
        hex(&self.0.finalize())
    }

    /// The file's contents, read in 1 MiB pieces.
    pub fn update_file(&mut self, p: &str) -> Result<()> {
        each_piece(p, |b| self.0.update(b))
    }
}

fn each_piece(p: &str, mut f: impl FnMut(&[u8])) -> Result<()> {
    let mut file = std::fs::File::open(p).map_err(at(p))?;
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = file.read(&mut buf).map_err(at(p))?;
        if n == 0 {
            return Ok(());
        }
        f(&buf[..n]);
    }
}

/// sha_file: a file's sha256.
pub fn sha256_file(p: &str) -> Result<String> {
    let mut h = Sha256::default();
    h.update_file(p)?;
    Ok(h.hexdigest())
}

/// SHA-1, fed piece by piece.
pub struct Sha1 {
    h: [u32; 5],
    buf: Vec<u8>,
    len: u64,
}

impl Default for Sha1 {
    fn default() -> Self {
        Sha1 {
            h: [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0],
            buf: Vec::with_capacity(64),
            len: 0,
        }
    }
}

impl Sha1 {
    pub fn update(&mut self, mut d: &[u8]) {
        self.len += d.len() as u64;
        if !self.buf.is_empty() {
            let k = (64 - self.buf.len()).min(d.len());
            self.buf.extend_from_slice(&d[..k]);
            d = &d[k..];
            if self.buf.len() == 64 {
                let b: [u8; 64] = self.buf[..].try_into().unwrap();
                self.block(&b);
                self.buf.clear();
            }
        }
        while d.len() >= 64 {
            let b: [u8; 64] = d[..64].try_into().unwrap();
            self.block(&b);
            d = &d[64..];
        }
        self.buf.extend_from_slice(d);
    }

    fn block(&mut self, chunk: &[u8; 64]) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes(chunk[4 * i..4 * i + 4].try_into().unwrap());
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let [mut a, mut b, mut c, mut d, mut e] = self.h;
        for (i, wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | (!b & d), 0x5A827999),
                20..=39 => (b ^ c ^ d, 0x6ED9EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1BBCDC),
                _ => (b ^ c ^ d, 0xCA62C1D6),
            };
            let t = a
                .rotate_left(5)
                .wrapping_add(f)
                .wrapping_add(e)
                .wrapping_add(k)
                .wrapping_add(*wi);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = t;
        }
        for (x, y) in self.h.iter_mut().zip([a, b, c, d, e]) {
            *x = x.wrapping_add(y);
        }
    }

    pub fn hexdigest(mut self) -> String {
        let bits = self.len.wrapping_mul(8);
        let mut pad = vec![0x80u8];
        while (self.buf.len() + pad.len()) % 64 != 56 {
            pad.push(0);
        }
        pad.extend(bits.to_be_bytes());
        let len = self.len;
        self.update(&pad);
        self.len = len;
        self.h.iter().map(|x| format!("{x:08x}")).collect()
    }
}

/// A file's sha1 (Bee's Digests).
pub fn sha1_file(p: &str) -> Result<String> {
    let mut h = Sha1::default();
    each_piece(p, |b| h.update(b))?;
    Ok(h.hexdigest())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sha1_known() {
        let mut h = Sha1::default();
        h.update(b"abc");
        assert_eq!(h.hexdigest(), "a9993e364706816aba3e25717850c26c9cd0d89d");
        let mut h = Sha1::default();
        let data: Vec<u8> = (0..1000u32).map(|i| (i * 7) as u8).collect();
        for c in data.chunks(13) {
            h.update(c);
        }
        assert_eq!(h.hexdigest(), androiddata::sha1::hex(&data));
        assert_eq!(
            sha256_hex(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }
}
