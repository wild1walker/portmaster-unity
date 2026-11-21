//! The members of a tar file (gzip-compressed or not) as Python's tarfile lists them: ustar names
//! with their prefix, GNU long names, pax paths; `isfile()` for regular files. Enough for the
//! licences in the setup's Python bundle (port.py's pybundle_licenses).
use std::fs::File;
use std::io::{BufReader, Read};

use flate2::bufread::MultiGzDecoder;
use pkgzip::{io_at, Error, Result};

pub struct Member {
    pub name: String,
    pub is_file: bool,
    pub data: Vec<u8>,
}

fn octal(f: &[u8]) -> u64 {
    if f.first().is_some_and(|b| b & 0x80 != 0) {
        // base-256 (GNU)
        let mut n: u64 = (f[0] & 0x7f) as u64;
        for &b in &f[1..] {
            n = (n << 8) | b as u64;
        }
        return n;
    }
    let s: String = f.iter().take_while(|b| **b != 0).map(|&b| b as char).collect();
    u64::from_str_radix(s.trim(), 8).unwrap_or(0)
}

fn cstr(f: &[u8]) -> String {
    let n = f.iter().position(|b| *b == 0).unwrap_or(f.len());
    String::from_utf8_lossy(&f[..n]).into_owned()
}

/// Every member with its data (files only; the archive read once, start to end).
pub fn members(path: &str) -> Result<Vec<Member>> {
    let f = File::open(path).map_err(io_at(path))?;
    let mut head = [0u8; 2];
    let mut br = BufReader::new(f);
    {
        use std::io::BufRead;
        let b = br.fill_buf().map_err(io_at(path))?;
        head[..b.len().min(2)].copy_from_slice(&b[..b.len().min(2)]);
    }
    let mut r: Box<dyn Read> = if head == [0x1f, 0x8b] { Box::new(MultiGzDecoder::new(br)) } else { Box::new(br) };
    let mut out = Vec::new();
    let (mut long_name, mut pax_path): (Option<String>, Option<String>) = (None, None);
    loop {
        let mut h = [0u8; 512];
        if !read_full(&mut r, &mut h).map_err(io_at(path))? || h.iter().all(|b| *b == 0) {
            break;
        }
        let size = octal(&h[124..136]);
        let typ = h[156];
        let mut data = vec![0u8; size as usize];
        if !read_full(&mut r, &mut data).map_err(io_at(path))? {
            return Err(Error::Fail(format!("{path}: unexpected end of data")));
        }
        let pad = (512 - size % 512) % 512;
        let mut skip = vec![0u8; pad as usize];
        read_full(&mut r, &mut skip).map_err(io_at(path))?;
        match typ {
            b'L' => {
                long_name = Some(cstr(&data));
                continue;
            }
            b'x' => {
                pax_path = pax(&data);
                continue;
            }
            b'g' => continue,
            _ => {}
        }
        let mut name = cstr(&h[0..100]);
        if &h[257..262] == b"ustar" {
            let prefix = cstr(&h[345..500]);
            if !prefix.is_empty() {
                name = format!("{prefix}/{name}");
            }
        }
        if let Some(n) = long_name.take() {
            name = n;
        }
        if let Some(n) = pax_path.take() {
            name = n;
        }
        let is_dir = typ == b'5';
        if is_dir {
            name = name.trim_end_matches('/').to_string();
        }
        let is_file = matches!(typ, b'0' | 0 | b'7' | b'S');
        out.push(Member { name, is_file, data: if is_file { data } else { Vec::new() } });
    }
    Ok(out)
}

fn read_full(r: &mut dyn Read, buf: &mut [u8]) -> std::io::Result<bool> {
    let mut n = 0;
    while n < buf.len() {
        let k = r.read(&mut buf[n..])?;
        if k == 0 {
            return Ok(n == buf.len());
        }
        n += k;
    }
    Ok(true)
}

/// A pax header's path.
fn pax(d: &[u8]) -> Option<String> {
    let mut i = 0;
    let mut path = None;
    while i < d.len() {
        let sp = d[i..].iter().position(|b| *b == b' ')? + i;
        let len: usize = std::str::from_utf8(&d[i..sp]).ok()?.parse().ok()?;
        if len == 0 || i + len > d.len() {
            break;
        }
        let rec = &d[sp + 1..i + len - 1];
        if let Some(v) = rec.strip_prefix(b"path=") {
            path = Some(String::from_utf8_lossy(v).into_owned());
        }
        i += len;
    }
    path
}
