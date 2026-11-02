//! tar archives read as a stream, as Python's tarfile reads them: ustar names with their prefix,
//! GNU long names and links ('L', 'K'), pax headers ('x', 'g': path, linkpath, size), numbers in
//! octal or base-256. Debian packages' data.tar.xz and Microsoft's .NET tarball.
use std::io::{self, Read};

/// A member: its name (as TarInfo.name), mode, type byte and size.
#[derive(Clone, Debug)]
pub struct Member {
    pub name: String,
    pub mode: u32,
    pub kind: u8,
    pub size: u64,
}

impl Member {
    /// TarInfo.isfile(): regular files (REGTYPE, AREGTYPE, CONTTYPE, GNU sparse).
    pub fn isfile(&self) -> bool {
        matches!(self.kind, b'0' | 0 | b'7' | b'S')
    }
}

/// A tar stream: `next()` gives each member, then `data()` reads its bytes (or they are skipped).
pub struct Tar<R: Read> {
    r: R,
    /// bytes of the current member not read yet, and its padding
    left: u64,
    pad: u64,
    globals: Vec<(String, String)>,
    done: bool,
}

fn nts(b: &[u8]) -> String {
    let e = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    String::from_utf8_lossy(&b[..e]).into_owned()
}

fn nti(b: &[u8]) -> io::Result<u64> {
    if b[0] & 0x80 != 0 {
        // base-256
        let mut n: u64 = 0;
        for &c in &b[1..] {
            n = n.checked_shl(8).unwrap_or(u64::MAX) | c as u64;
        }
        return Ok(n);
    }
    let s = nts(b);
    let s = s.trim_matches(|c: char| c == ' ' || c == '\0');
    if s.is_empty() {
        return Ok(0);
    }
    u64::from_str_radix(s, 8).map_err(|_| io::Error::other("tarfile.ReadError: invalid header"))
}

fn pax(data: &[u8]) -> Vec<(String, String)> {
    let mut out = Vec::new();
    let mut p = 0;
    while p < data.len() {
        let Some(sp) = data[p..].iter().position(|&c| c == b' ') else {
            break;
        };
        let Ok(n) = std::str::from_utf8(&data[p..p + sp])
            .unwrap_or("")
            .parse::<usize>()
        else {
            break;
        };
        if n == 0 || p + n > data.len() {
            break;
        }
        let rec = &data[p + sp + 1..p + n];
        let rec = rec.strip_suffix(b"\n").unwrap_or(rec);
        if let Some(eq) = rec.iter().position(|&c| c == b'=') {
            out.push((
                String::from_utf8_lossy(&rec[..eq]).into_owned(),
                String::from_utf8_lossy(&rec[eq + 1..]).into_owned(),
            ));
        }
        p += n;
    }
    out
}

impl<R: Read> Tar<R> {
    pub fn new(r: R) -> Self {
        Tar {
            r,
            left: 0,
            pad: 0,
            globals: Vec::new(),
            done: false,
        }
    }

    fn skip(&mut self, n: u64) -> io::Result<()> {
        let k = io::copy(&mut (&mut self.r).take(n), &mut io::sink())?;
        if k != n {
            return Err(io::Error::other(
                "tarfile.ReadError: unexpected end of data",
            ));
        }
        Ok(())
    }

    fn read_block(&mut self) -> io::Result<Option<[u8; 512]>> {
        let mut b = [0u8; 512];
        let mut got = 0;
        while got < 512 {
            let n = self.r.read(&mut b[got..])?;
            if n == 0 {
                break;
            }
            got += n;
        }
        if got == 0 {
            return Ok(None);
        }
        if got < 512 {
            return Err(io::Error::other("tarfile.ReadError: truncated header"));
        }
        Ok(Some(b))
    }

    fn read_data(&mut self, size: u64) -> io::Result<Vec<u8>> {
        if size > 64 << 20 {
            return Err(io::Error::other("tarfile: an extended header too big"));
        }
        let mut d = vec![0u8; size as usize];
        self.r.read_exact(&mut d)?;
        self.skip((512 - size % 512) % 512)?;
        Ok(d)
    }

    /// The next member, or None at the end.
    pub fn next_member(&mut self) -> io::Result<Option<Member>> {
        if self.done {
            return Ok(None);
        }
        let (l, p) = (self.left, self.pad);
        self.skip(l + p)?;
        self.left = 0;
        self.pad = 0;
        let mut long_name: Option<String> = None;
        let mut long_link: Option<String> = None;
        let mut pax_h: Vec<(String, String)> = Vec::new();
        loop {
            let Some(b) = self.read_block()? else {
                self.done = true;
                return Ok(None);
            };
            if b.iter().all(|&c| c == 0) {
                // the end (two zero blocks; tarfile stops at the first)
                self.done = true;
                return Ok(None);
            }
            let kind = b[156];
            let size = nti(&b[124..136])?;
            match kind {
                b'L' => {
                    long_name = Some(nts(&self.read_data(size)?));
                    continue;
                }
                b'K' => {
                    long_link = Some(nts(&self.read_data(size)?));
                    continue;
                }
                b'x' => {
                    pax_h = pax(&self.read_data(size)?);
                    continue;
                }
                b'g' => {
                    let g = pax(&self.read_data(size)?);
                    self.globals.extend(g);
                    continue;
                }
                _ => {}
            }
            let _ = long_link;
            let mut name = nts(&b[0..100]);
            let mode = nti(&b[100..108])? as u32;
            let prefix = nts(&b[345..500]);
            let mut kind = kind;
            if kind == 0 && name.ends_with('/') {
                kind = b'5';
            }
            if kind == b'5' {
                name = name.trim_end_matches('/').to_string();
            }
            const GNU_TYPES: [u8; 3] = [b'L', b'K', b'S'];
            if !prefix.is_empty() && !GNU_TYPES.contains(&kind) {
                name = format!("{prefix}/{name}");
            }
            let mut size = size;
            for (k, v) in self.globals.iter().chain(pax_h.iter()) {
                match k.as_str() {
                    "path" => name = v.clone(),
                    "size" => size = v.parse().unwrap_or(size),
                    _ => {}
                }
            }
            if let Some(n) = long_name.take() {
                name = n;
            }
            if kind == b'5' {
                name = name.trim_end_matches('/').to_string();
            }
            // (data follows regular files and other members with a size, except links and dirs)
            let has_data = !matches!(kind, b'1' | b'2' | b'3' | b'4' | b'5' | b'6');
            if has_data {
                self.left = size;
                self.pad = (512 - size % 512) % 512;
            }
            return Ok(Some(Member {
                name,
                mode,
                kind,
                size,
            }));
        }
    }

    /// The current member's bytes, as a reader.
    pub fn data(&mut self) -> impl Read + '_ {
        Data { t: self }
    }
}

struct Data<'a, R: Read> {
    t: &'a mut Tar<R>,
}

impl<R: Read> Read for Data<'_, R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        if self.t.left == 0 {
            return Ok(0);
        }
        let n = (buf.len() as u64).min(self.t.left) as usize;
        let k = self.t.r.read(&mut buf[..n])?;
        if k == 0 {
            return Err(io::Error::other(
                "tarfile.ReadError: unexpected end of data",
            ));
        }
        self.t.left -= k as u64;
        Ok(k)
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;

    /// A ustar header block.
    pub fn header(name: &str, kind: u8, size: usize, mode: u32) -> Vec<u8> {
        let mut h = vec![0u8; 512];
        h[..name.len()].copy_from_slice(name.as_bytes());
        h[100..107].copy_from_slice(format!("{mode:07o}").as_bytes());
        h[124..135].copy_from_slice(format!("{size:011o}").as_bytes());
        h[156] = kind;
        h[257..263].copy_from_slice(b"ustar\0");
        h
    }

    /// A tar of (name, kind, data, mode) members (a long name as GNU's 'L' entry).
    pub fn make(members: &[(&str, u8, &[u8], u32)]) -> Vec<u8> {
        let mut out = Vec::new();
        let pad = |out: &mut Vec<u8>| {
            while out.len() % 512 != 0 {
                out.push(0);
            }
        };
        for (name, kind, data, mode) in members {
            if name.len() >= 100 {
                out.extend(header("././@LongLink", b'L', name.len() + 1, 0));
                out.extend(name.as_bytes());
                out.push(0);
                pad(&mut out);
                out.extend(header(&name[..99], *kind, data.len(), *mode));
            } else {
                out.extend(header(name, *kind, data.len(), *mode));
            }
            out.extend(*data);
            pad(&mut out);
        }
        out.extend([0u8; 1024]);
        out
    }

    #[test]
    fn members() {
        let long = format!("./usr/lib/{}/x.h", "d".repeat(120));
        let t = make(&[
            ("./usr/", b'5', b"", 0o755),
            ("./usr/bin/clang", b'0', b"ELF", 0o755),
            (&long, b'0', b"hdr", 0o644),
            ("./usr/bin/clang++", b'2', b"", 0o777),
            ("./usr/bin/hard", b'1', b"", 0o755),
            ("./last", b'0', &[1u8; 600], 0o600),
        ]);
        let mut tar = Tar::new(&t[..]);
        let mut seen = Vec::new();
        while let Some(m) = tar.next_member().unwrap() {
            let mut d = Vec::new();
            if m.isfile() && m.name.ends_with("x.h") {
                tar.data().read_to_end(&mut d).unwrap();
                assert_eq!(d, b"hdr");
            }
            seen.push((m.name.clone(), m.isfile(), m.mode));
        }
        assert_eq!(seen.len(), 6);
        assert_eq!(seen[0], ("./usr".into(), false, 0o755));
        assert_eq!(seen[1], ("./usr/bin/clang".into(), true, 0o755));
        assert_eq!(seen[2].0, long);
        assert!(!seen[3].1 && !seen[4].1);
        assert_eq!(seen[5], ("./last".into(), true, 0o600));
    }
}
