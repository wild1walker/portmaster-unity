//! A UA_GL_REC_FILE trace (runtime/host/glrec.c), read record by record from `gzip -dc`.
use std::io::{BufReader, Read};
use std::process::{Child, Command, Stdio};

pub const TR_TAG: u64 = 1 << 63;
pub const TR_FRAME: u32 = 0xFFFFF0;

pub enum Blob {
    Data(Vec<u8>),
    Zero(usize),
}
impl Blob {
    pub fn len(&self) -> usize {
        match self {
            Blob::Data(d) => d.len(),
            Blob::Zero(n) => *n,
        }
    }
}

pub struct Rec {
    pub op: u32,
    pub thread: u8,
    pub a: Vec<u64>,
    pub ret: Option<u64>,
    pub blobs: Vec<Blob>,
}
impl Rec {
    /// the blob slot `i` points at
    pub fn blob(&self, i: usize) -> Option<&Blob> {
        let v = *self.a.get(i)?;
        if v & TR_TAG == 0 {
            return None;
        }
        self.blobs.get((v & !TR_TAG) as usize)
    }
    pub fn bytes(&self, i: usize) -> Option<&[u8]> {
        match self.blob(i)? {
            Blob::Data(d) => Some(d),
            Blob::Zero(_) => None,
        }
    }
    pub fn u(&self, i: usize) -> u32 {
        self.a.get(i).copied().unwrap_or(0) as u32
    }
    pub fn i(&self, i: usize) -> i64 {
        self.a.get(i).copied().unwrap_or(0) as i64
    }
}

pub struct Trace {
    r: BufReader<std::process::ChildStdout>,
    child: Child,
    pub names: Vec<String>,
}

impl Trace {
    pub fn open(path: &str) -> Result<Trace, String> {
        let mut child = Command::new("gzip")
            .args(["-dc", path])
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .map_err(|e| format!("gzip: {e}"))?;
        let mut r = BufReader::with_capacity(1 << 20, child.stdout.take().unwrap());
        let mut magic = [0u8; 8];
        let mut hdr = [0u8; 12];
        if r.read_exact(&mut magic).is_err() || &magic != b"UAGLTRC1" || r.read_exact(&mut hdr).is_err() {
            return Err(format!("{path}: not a UA_GL_REC_FILE trace"));
        }
        let nops = u32::from_le_bytes(hdr[4..8].try_into().unwrap()) as usize;
        let mut names = Vec::with_capacity(nops);
        for _ in 0..nops {
            let mut l = [0u8; 2];
            r.read_exact(&mut l).map_err(|e| e.to_string())?;
            let mut n = vec![0u8; u16::from_le_bytes(l) as usize];
            r.read_exact(&mut n).map_err(|e| e.to_string())?;
            names.push(String::from_utf8_lossy(&n).into_owned());
        }
        Ok(Trace { r, child, names })
    }

    /// the next record; None at the end (a trace cut short ends at its last whole record)
    pub fn next(&mut self) -> Option<Rec> {
        let mut w = [0u8; 8];
        self.r.read_exact(&mut w).ok()?;
        let h = u64::from_le_bytes(w);
        let op = (h & 0xffffff) as u32;
        let nargs = (h >> 24 & 0xff) as usize;
        let nblobs = (h >> 32 & 0xffff) as usize;
        let hasret = (h >> 48 & 1) as usize;
        let thread = (h >> 49 & 31) as u8;
        let mut raw = vec![0u8; (nargs + hasret) * 8];
        self.r.read_exact(&mut raw).ok()?;
        let mut a: Vec<u64> = raw.chunks_exact(8).map(|c| u64::from_le_bytes(c.try_into().unwrap())).collect();
        let ret = if hasret == 1 { a.pop() } else { None };
        let mut blobs = Vec::with_capacity(nblobs);
        for _ in 0..nblobs {
            self.r.read_exact(&mut w).ok()?;
            let len = u64::from_le_bytes(w);
            let n = (len & !TR_TAG) as usize;
            if len & TR_TAG != 0 {
                blobs.push(Blob::Zero(n));
            } else {
                let mut d = vec![0u8; (n + 7) / 8 * 8];
                self.r.read_exact(&mut d).ok()?;
                d.truncate(n);
                blobs.push(Blob::Data(d));
            }
        }
        Some(Rec { op, thread, a, ret, blobs })
    }
}
impl Drop for Trace {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

pub fn h64(seed: u64, b: &[u8]) -> u64 {
    // (the seed is stirred before the first word meets it: h64(a, b) is not h64(b, a))
    let mut h = (seed ^ 0x9E37_79B9_7F4A_7C15).wrapping_mul(0xc4ce_b9fe_1a85_ec53).rotate_left(31) ^ (b.len() as u64).wrapping_mul(0xff51_afd7_ed55_8ccd);
    let mut c = b.chunks_exact(8);
    for w in &mut c {
        h = (h ^ u64::from_le_bytes(w.try_into().unwrap())).wrapping_mul(0x0000_0100_0000_01B3).rotate_left(29);
    }
    let mut t = 0u64;
    for (i, x) in c.remainder().iter().enumerate() {
        t |= (*x as u64) << (8 * i);
    }
    h = (h ^ t).wrapping_mul(0x0000_0100_0000_01B3).rotate_left(29);
    h ^= h >> 32;
    h = h.wrapping_mul(0xd6e8_feb8_6659_fd93);
    h ^ h >> 32
}
pub fn hmix(a: u64, b: u64) -> u64 {
    h64(a, &b.to_le_bytes())
}
