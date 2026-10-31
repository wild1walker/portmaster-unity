//! cpio archives (a package's Payload and Scripts), as macpkg.cpio_members reads them: odc (octal
//! fields) and newc (hex fields, 4-byte alignment), member after member up to TRAILER!!!.
use std::io::{self, Read};

use crate::{py, Error, Result};

/// One member: its name (UTF-8, invalid bytes replaced), mode and data size. Its data follows in
/// the stream; the reader takes exactly `size` bytes of it before asking for the next member.
pub struct Member {
    pub name: String,
    pub mode: u32,
    pub size: u64,
}

pub struct Cpio<R> {
    s: R,
    pad: usize,
}

/// macpkg._exact: n bytes, else the EOFError of an archive cut short.
pub fn exact(s: &mut impl Read, n: usize, what: &str) -> Result<Vec<u8>> {
    let mut v = Vec::new();
    s.take(n as u64).read_to_end(&mut v)?;
    if v.len() < n {
        return Err(Error::Fail(format!("EOFError: {what}")));
    }
    Ok(v)
}

fn field(h: &[u8], radix: u32) -> Result<i128> {
    py::int_bytes(h, radix).ok_or_else(|| {
        Error::Fail(format!(
            "ValueError: invalid literal for int() with base {radix}: {}",
            py::bytes_repr(h)
        ))
    })
}

impl<R: Read> Cpio<R> {
    pub fn new(s: R) -> Self {
        Cpio { s, pad: 0 }
    }

    /// The stream, for the member's data.
    pub fn data(&mut self) -> &mut R {
        &mut self.s
    }

    /// The next member, or None after the trailer.
    pub fn next_member(&mut self) -> Result<Option<Member>> {
        const SHORT: &str = "cpio archive ends early";
        if self.pad > 0 {
            exact(&mut self.s, self.pad, SHORT)?;
            self.pad = 0;
        }
        let magic = exact(&mut self.s, 6, SHORT)?;
        let (mode, size, name);
        if magic == b"070707" {
            let h = exact(&mut self.s, 70, SHORT)?;
            mode = field(&h[12..18], 8)?;
            let namesize = field(&h[53..59], 8)?;
            size = field(&h[59..70], 8)?;
            name = self.name(namesize)?;
        } else if magic == b"070701" || magic == b"070702" {
            let h = exact(&mut self.s, 104, SHORT)?;
            mode = field(&h[8..16], 16)?;
            size = field(&h[48..56], 16)?;
            let namesize = field(&h[88..96], 16)?;
            name = self.name(namesize)?;
            exact(
                &mut self.s,
                (-(110 + namesize)).rem_euclid(4) as usize,
                SHORT,
            )?;
            self.pad = (-size).rem_euclid(4) as usize;
        } else {
            return Err(Error::Fail(format!(
                "ValueError: unknown cpio header {}",
                py::bytes_repr(&magic)
            )));
        }
        if name == "TRAILER!!!" {
            self.pad = 0;
            return Ok(None);
        }
        let mode = u32::try_from(mode)
            .map_err(|_| Error::Fail(format!("ValueError: cpio mode {mode}")))?;
        let size = u64::try_from(size)
            .map_err(|_| Error::Fail(format!("ValueError: cpio size {size}")))?;
        Ok(Some(Member { name, mode, size }))
    }

    fn name(&mut self, namesize: i128) -> Result<String> {
        let n = usize::try_from(namesize.max(0)).unwrap_or(usize::MAX);
        let mut b = exact(&mut self.s, n, "cpio archive ends early")?;
        b.pop();
        Ok(String::from_utf8_lossy(&b).into_owned())
    }

    /// Reads past n bytes of data (macpkg._skip).
    pub fn skip(&mut self, n: u64) -> Result<()> {
        let got = io::copy(&mut (&mut self.s).take(n), &mut io::sink())?;
        if got < n {
            return Err(Error::Fail("EOFError: payload ends early".into()));
        }
        Ok(())
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// odc cpio of (name, data or None for a folder), as test_macpkg_synthetic.cpio.
    pub fn odc(entries: &[(&str, Option<&[u8]>)]) -> Vec<u8> {
        let mut out = Vec::new();
        for (name, data) in entries
            .iter()
            .copied()
            .chain([("TRAILER!!!", Some(&b""[..]))])
        {
            let mode = if data.is_none() { 0o040755 } else { 0o100644 };
            let data = data.unwrap_or(b"");
            let n = name.len() + 1;
            out.extend(
                format!(
                    "070707{:06o}{:06o}{mode:06o}{:06o}{:06o}{:06o}{:06o}{:011o}{n:06o}{:011o}",
                    0,
                    0,
                    0,
                    0,
                    1,
                    0,
                    0,
                    data.len()
                )
                .bytes(),
            );
            out.extend(name.bytes());
            out.push(0);
            out.extend(data);
        }
        out
    }

    /// newc cpio of (name, mode, data).
    pub fn newc(entries: &[(&str, u32, &[u8])]) -> Vec<u8> {
        let mut out = Vec::new();
        for (name, mode, data) in entries.iter().copied().chain([("TRAILER!!!", 0, &b""[..])]) {
            let n = name.len() + 1;
            out.extend(b"070701");
            for v in [0, mode as usize, 0, 0, 1, 0, data.len(), 0, 0, 0, 0, n, 0] {
                out.extend(format!("{v:08X}").bytes());
            }
            out.extend(name.bytes());
            out.push(0);
            while out.len() % 4 != 0 {
                out.push(0);
            }
            out.extend(data);
            while out.len() % 4 != 0 {
                out.push(0);
            }
        }
        out
    }

    fn members(a: &[u8]) -> Vec<(String, u32, Vec<u8>)> {
        let mut c = Cpio::new(a);
        let mut v = Vec::new();
        while let Some(m) = c.next_member().unwrap() {
            let d = exact(c.data(), m.size as usize, "x").unwrap();
            v.push((m.name, m.mode, d));
        }
        v
    }

    #[test]
    fn odc_and_newc() {
        let a = odc(&[(".", None), ("./a", Some(b"abc"))]);
        let m = members(&a);
        assert_eq!(m.len(), 2);
        assert_eq!(
            (m[1].0.as_str(), m[1].1, &m[1].2[..]),
            ("./a", 0o100644, &b"abc"[..])
        );
        let b = newc(&[
            ("x", 0o100600, b"12345"),
            ("dir", 0o040755, b""),
            ("y", 0o100644, b"z"),
        ]);
        let m = members(&b);
        assert_eq!(
            m.iter().map(|x| x.0.as_str()).collect::<Vec<_>>(),
            ["x", "dir", "y"]
        );
        assert_eq!(m[0].2, b"12345");
        assert_eq!(m[2].2, b"z");
    }

    #[test]
    fn errors() {
        let e = Cpio::new(&b"123456"[..])
            .next_member()
            .err()
            .unwrap()
            .to_string();
        assert_eq!(e, "ValueError: unknown cpio header b'123456'");
        let a = odc(&[("./a", Some(b"abc"))]);
        let e = Cpio::new(&a[..40]).next_member().err().unwrap().to_string();
        assert_eq!(e, "EOFError: cpio archive ends early");
    }
}
