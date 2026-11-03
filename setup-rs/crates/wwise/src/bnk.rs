//! A soundbank's chunks, read from the file without loading its media: BKHD (version, id), DIDX
//! (the media index), DATA (where the media are: read one at a time later), HIRC and STMG (read).
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

use crate::rd::{Rd, Result};

pub struct Bank {
    pub path: PathBuf,
    pub version: u32,
    pub id: u32,
    /// (media id, offset in DATA, size)
    pub media: Vec<(u32, u32, u32)>,
    /// DATA's payload: its offset in the file and its size
    pub data: (u64, u64),
    pub hirc: Vec<u8>,
    pub stmg: Vec<u8>,
}

fn io<T>(p: &Path, r: std::io::Result<T>) -> Result<T> {
    r.map_err(|e| format!("{}: {e}", p.display()))
}

/// Reads the chunk headers and the small chunks (HIRC, STMG, DIDX); DATA is only located.
pub fn open(path: &Path) -> Result<Bank> {
    let mut f = io(path, File::open(path))?;
    let len = io(path, f.metadata())?.len();
    let mut b = Bank {
        path: path.to_path_buf(),
        version: 0,
        id: 0,
        media: Vec::new(),
        data: (0, 0),
        hirc: Vec::new(),
        stmg: Vec::new(),
    };
    let mut pos = 0u64;
    while pos + 8 <= len {
        let mut h = [0u8; 8];
        io(path, f.seek(SeekFrom::Start(pos)))?;
        io(path, f.read_exact(&mut h))?;
        let tag = &h[..4];
        let size = u32::from_le_bytes(h[4..].try_into().unwrap()) as u64;
        let body = pos + 8;
        if body + size > len {
            return Err(format!(
                "{}: chunk {} past the end of the file",
                path.display(),
                String::from_utf8_lossy(tag)
            ));
        }
        let mut read = |n: u64| -> Result<Vec<u8>> {
            let mut v = vec![0u8; n as usize];
            io(path, f.read_exact(&mut v))?;
            Ok(v)
        };
        match tag {
            b"BKHD" => {
                let v = read(size.min(64))?;
                let r = &mut Rd::new(&v);
                b.version = r.u32()?;
                b.id = r.u32()?;
            }
            b"DIDX" => {
                let v = read(size)?;
                b.media = v
                    .chunks_exact(12)
                    .map(|c| {
                        let u = |k: usize| u32::from_le_bytes(c[k..k + 4].try_into().unwrap());
                        (u(0), u(4), u(8))
                    })
                    .collect();
            }
            b"DATA" => b.data = (body, size),
            b"HIRC" => b.hirc = read(size)?,
            b"STMG" => b.stmg = read(size)?,
            _ => {}
        }
        if pos == 0 && tag != b"BKHD" {
            return Err(format!(
                "{}: not a Wwise soundbank (no BKHD)",
                path.display()
            ));
        }
        pos = body + size;
    }
    if b.version == 0 {
        return Err(format!("{}: not a Wwise soundbank", path.display()));
    }
    for &(id, off, size) in &b.media {
        if off as u64 + size as u64 > b.data.1 {
            return Err(format!(
                "{}: media {id} past the end of DATA",
                path.display()
            ));
        }
    }
    Ok(b)
}

impl Bank {
    /// One media's bytes (a WEM), read from the file.
    pub fn read_media(&self, f: &mut File, off: u32, size: u32, buf: &mut Vec<u8>) -> Result<()> {
        buf.resize(size as usize, 0);
        io(
            &self.path,
            f.seek(SeekFrom::Start(self.data.0 + off as u64)),
        )?;
        io(&self.path, f.read_exact(buf))
    }
}

/// The Init bank's global settings (STMG, version 128): the state groups' fade times, the switch
/// groups driven by an RTPC, the game parameters' start values.
#[derive(Default, Debug)]
pub struct Globals {
    pub state_groups: Vec<(u32, u32, Vec<(u32, u32, u32)>)>,
    pub switch_rtpc: Vec<(u32, u32, Vec<(f32, u32)>)>,
    pub rtpc: Vec<(u32, f32)>,
}

pub fn globals(stmg: &[u8]) -> Result<Globals> {
    let r = &mut Rd::new(stmg);
    let mut g = Globals::default();
    r.f32()?; // volume threshold
    r.u16()?; // max voices
    r.u16()?; // max dangerous virtual voices
    let n = r.u32()?;
    for _ in 0..r.count(n, 12)? {
        let id = r.u32()?;
        let fade = r.u32()?;
        let nt = r.u32()?;
        let mut t = Vec::new();
        for _ in 0..r.count(nt, 12)? {
            t.push((r.u32()?, r.u32()?, r.u32()?));
        }
        g.state_groups.push((id, fade, t));
    }
    let n = r.u32()?;
    for _ in 0..r.count(n, 13)? {
        let id = r.u32()?;
        let rtpc = r.u32()?;
        r.u8()?; // rtpc type
        let np = r.u32()?;
        let np = r.count(np, 12)?;
        let mut p = Vec::new();
        for _ in 0..np {
            let x = r.f32()?;
            let sw = r.u32()?; // (a switch id where an RTPC curve has its value)
            r.u32()?;
            p.push((x, sw));
        }
        g.switch_rtpc.push((id, rtpc, p));
    }
    let n = r.u32()?;
    for _ in 0..r.count(n, 21)? {
        let id = r.u32()?;
        let v = r.f32()?;
        r.skip(13)?; // ramp type, up, down, built-in parameter
        g.rtpc.push((id, v));
    }
    Ok(g)
}
