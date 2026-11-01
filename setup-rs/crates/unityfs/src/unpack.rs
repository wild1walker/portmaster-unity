//! A compressed build's data.unity3d unpacked (unpack.py): the UnityFS archive's files written
//! next to it (or to another folder), one block at a time (memory: one block), each file to
//! `<name>.tmp` first and then renamed. Blocks stored, LZ4 or LZMA compressed; formats 6 - 8.
use crate::compress;
use crate::error::{Error, Result};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Component, Path, PathBuf};

pub const ARCHIVE: &str = "data.unity3d";

fn value(m: impl Into<String>) -> Error {
    Error::Value(m.into())
}

fn cstr(b: &[u8], p: usize) -> Result<(&[u8], usize)> {
    let e = b
        .get(p..)
        .and_then(|r| r.iter().position(|&c| c == 0))
        .ok_or_else(|| value("subsection not found"))?;
    Ok((&b[p..p + e], p + e + 1))
}

fn be<const N: usize>(b: &[u8], p: usize) -> Result<[u8; N]> {
    b.get(p..p + N)
        .map(|s| s.try_into().unwrap())
        .ok_or_else(|| value(format!("unpack requires a buffer of {N} bytes")))
}

/// A block: (offset in the file, compressed size, uncompressed size, flags).
type Block = (u64, u32, u32, u16);

/// A UnityFS archive's block and file tables.
pub struct Archive {
    pub path: PathBuf,
    pub format: u32,
    pub unity: Vec<u8>,
    pub blocks: Vec<Block>,
    /// (offset, size, flags, name)
    pub files: Vec<(u64, u64, u32, String)>,
}

impl Archive {
    pub fn open(path: &Path) -> Result<Archive> {
        let mut f = File::open(path)?;
        let mut head = Vec::new();
        (&mut f).take(4096).read_to_end(&mut head)?;
        let (sig, mut p) = cstr(&head, 0)?;
        if sig != b"UnityFS" {
            return Err(value(format!("{}: not a UnityFS archive", path.display())));
        }
        let format = u32::from_be_bytes(be(&head, p)?);
        p += 4;
        let (_, q) = cstr(&head, p)?; // "5.x.x"
        let (unity, q) = cstr(&head, q)?;
        let unity = unity.to_vec();
        p = q;
        let size = i64::from_be_bytes(be(&head, p)?);
        let ci_size = u32::from_be_bytes(be(&head, p + 8)?);
        let ui_size = u32::from_be_bytes(be(&head, p + 12)?);
        let flags = u32::from_be_bytes(be(&head, p + 16)?);
        p += 20;
        if format >= 7 {
            p = (p + 15) & !15;
        }
        let mut ci = vec![0u8; ci_size as usize];
        let mut data_start;
        if flags & 0x80 != 0 {
            // the block table at the end of the file
            f.seek(SeekFrom::Start((size - ci_size as i64) as u64))?;
            let n = read_some(&mut f, &mut ci)?;
            ci.truncate(n);
            data_start = p as u64;
        } else {
            f.seek(SeekFrom::Start(p as u64))?;
            let n = read_some(&mut f, &mut ci)?;
            ci.truncate(n);
            data_start = p as u64 + ci_size as u64;
        }
        if flags & 0x200 != 0 {
            // (2020+) padding before the first block
            data_start = (data_start + 15) & !15;
        }
        let bi = decompress(&ci, ui_size as usize, flags & 0x3f)?;
        let n = i32::from_be_bytes(be(&bi, 16)?);
        let mut blocks = Vec::new();
        let (mut q, mut fpos) = (20usize, data_start);
        for _ in 0..n.max(0) {
            let usz = u32::from_be_bytes(be(&bi, q)?);
            let csz = u32::from_be_bytes(be(&bi, q + 4)?);
            let bfl = u16::from_be_bytes(be(&bi, q + 8)?);
            q += 10;
            blocks.push((fpos, csz, usz, bfl));
            fpos += csz as u64;
        }
        let nn = i32::from_be_bytes(be(&bi, q)?);
        q += 4;
        let mut files = Vec::new();
        for _ in 0..nn.max(0) {
            let off = i64::from_be_bytes(be(&bi, q)?);
            let sz = i64::from_be_bytes(be(&bi, q + 8)?);
            let fl = u32::from_be_bytes(be(&bi, q + 16)?);
            q += 20;
            let (name, r) = cstr(&bi, q)?;
            q = r;
            let name = String::from_utf8(name.to_vec())
                .map_err(|_| value("'utf-8' codec can't decode a file name"))?;
            files.push((off as u64, sz as u64, fl, name));
        }
        Ok(Archive {
            path: path.to_path_buf(),
            format,
            unity,
            blocks,
            files,
        })
    }

    /// Writes every file of the archive under out_dir (`log`: a line for each); returns their
    /// names.
    pub fn extract(&self, out_dir: &Path, log: &mut dyn FnMut(&str)) -> Result<Vec<String>> {
        let root = std::fs::canonicalize(out_dir)?;
        let mut outs: Vec<(u64, u64, PathBuf, File)> = Vec::new();
        let mut names = Vec::new();
        for (off, sz, _, name) in &self.files {
            let dst = realpath(&out_dir.join(name));
            if !dst.starts_with(&root) || dst == root {
                return Err(value(format!(
                    "{}: file name {} leaves the folder",
                    self.path.display(),
                    repr(name)
                )));
            }
            if let Some(d) = dst.parent() {
                std::fs::create_dir_all(d)?;
            }
            let fo = File::create(tmp(&dst))?;
            outs.push((*off, off + sz, dst, fo));
            names.push(name.clone());
        }
        let mut upos = 0u64;
        let mut f = File::open(&self.path)?;
        for &(fpos, csz, usz, bfl) in &self.blocks {
            f.seek(SeekFrom::Start(fpos))?;
            let mut data = vec![0u8; csz as usize];
            let n = read_some(&mut f, &mut data)?;
            data.truncate(n);
            let plain = decompress(&data, usz as usize, (bfl & 0x3f) as u32)?;
            for (a, b, _, fo) in &mut outs {
                let lo = (*a).max(upos);
                let hi = (*b).min(upos + usz as u64);
                if lo < hi {
                    let s = (lo - upos) as usize;
                    let e = ((hi - upos) as usize).min(plain.len());
                    if s < e {
                        fo.write_all(&plain[s..e])?;
                    }
                }
            }
            upos += usz as u64;
        }
        for (_, _, _, fo) in &mut outs {
            fo.flush()?;
        }
        for (a, b, dst, fo) in outs {
            drop(fo);
            let t = tmp(&dst);
            if std::fs::metadata(&t)?.len() != b - a {
                return Err(value(format!(
                    "{}: {} came out short",
                    self.path.display(),
                    dst.display()
                )));
            }
            std::fs::rename(&t, &dst)?;
            let rel = dst.strip_prefix(&root).unwrap_or(&dst);
            log(&format!("unpack: {} ({} bytes)", rel.display(), b - a));
        }
        Ok(names)
    }
}

fn tmp(p: &Path) -> PathBuf {
    let mut s = p.as_os_str().to_owned();
    s.push(".tmp");
    PathBuf::from(s)
}

/// os.path.realpath of a path whose last parts may not exist yet.
fn realpath(p: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for c in p.components() {
        match c {
            Component::ParentDir => {
                out.pop();
            }
            Component::CurDir => {}
            c => {
                out.push(c);
                if let Ok(r) = std::fs::canonicalize(&out) {
                    out = r;
                }
            }
        }
    }
    out
}

fn repr(s: &str) -> String {
    format!("'{}'", s.replace('\\', "\\\\").replace('\'', "\\'"))
}

fn read_some(f: &mut File, buf: &mut [u8]) -> Result<usize> {
    let mut n = 0;
    while n < buf.len() {
        let k = f.read(&mut buf[n..])?;
        if k == 0 {
            break;
        }
        n += k;
    }
    Ok(n)
}

/// unpack._decompress: stored, LZMA (1) or LZ4 (2, 3) blocks.
fn decompress(data: &[u8], usize: usize, comp: u32) -> Result<Vec<u8>> {
    match comp {
        0 => Ok(data.to_vec()),
        1..=3 => compress::decompress(data, usize, comp),
        c => Err(value(format!("unknown block compression {c}"))),
    }
}

/// unpack: data.unity3d of a data folder unpacked into out_dir (default: the folder), the
/// archive removed after with `remove`. Lines go to `log`.
pub fn unpack(
    data_dir: &Path,
    out_dir: Option<&Path>,
    remove: bool,
    log: &mut dyn FnMut(&str),
) -> Result<Vec<String>> {
    let src = data_dir.join(ARCHIVE);
    if !src.is_file() {
        return Err(Error::Unsupported(format!(
            "{}: no {ARCHIVE} (not a compressed build)",
            data_dir.display()
        )));
    }
    let names = Archive::open(&src)?.extract(out_dir.unwrap_or(data_dir), log)?;
    if remove {
        std::fs::remove_file(&src)?;
    }
    log(&format!(
        "unpack: {} files from {}{}",
        names.len(),
        src.display(),
        if remove { " (archive removed)" } else { "" }
    ));
    Ok(names)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A made-up data.unity3d (format 7, the block table after the header, LZ4), its files
    /// across two blocks: a stored one and an LZ4 one.
    fn archive(files: &[(&str, &[u8])], flags_extra: u32) -> Vec<u8> {
        let mut plain = Vec::new();
        let mut table = Vec::new();
        for (name, data) in files {
            table.push((plain.len() as i64, data.len() as i64, *name));
            plain.extend_from_slice(data);
        }
        let cut = plain.len() / 2;
        let b0 = plain[..cut].to_vec();
        let b1 = compress::compress_lz4(&plain[cut..]).unwrap();
        let mut bi = vec![0u8; 16];
        bi.extend_from_slice(&2i32.to_be_bytes());
        for (u, c, fl) in [(cut, b0.len(), 0u16), (plain.len() - cut, b1.len(), 3)] {
            bi.extend_from_slice(&(u as u32).to_be_bytes());
            bi.extend_from_slice(&(c as u32).to_be_bytes());
            bi.extend_from_slice(&fl.to_be_bytes());
        }
        bi.extend_from_slice(&(table.len() as i32).to_be_bytes());
        for (o, s, n) in &table {
            bi.extend_from_slice(&o.to_be_bytes());
            bi.extend_from_slice(&s.to_be_bytes());
            bi.extend_from_slice(&4u32.to_be_bytes());
            bi.extend_from_slice(n.as_bytes());
            bi.push(0);
        }
        let ci = compress::compress_lz4(&bi).unwrap();
        let mut out = b"UnityFS\0".to_vec();
        out.extend_from_slice(&7u32.to_be_bytes());
        out.extend_from_slice(b"5.x.x\0");
        out.extend_from_slice(b"2019.4.40f1\0");
        let size_at = out.len();
        out.extend_from_slice(&0i64.to_be_bytes());
        out.extend_from_slice(&(ci.len() as u32).to_be_bytes());
        out.extend_from_slice(&(bi.len() as u32).to_be_bytes());
        out.extend_from_slice(&(3 | 0x40 | flags_extra).to_be_bytes());
        while out.len() % 16 != 0 {
            out.push(0);
        }
        out.extend_from_slice(&ci);
        if flags_extra & 0x200 != 0 {
            while out.len() % 16 != 0 {
                out.push(0);
            }
        }
        out.extend_from_slice(&b0);
        out.extend_from_slice(&b1);
        let n = out.len() as i64;
        out[size_at..size_at + 8].copy_from_slice(&n.to_be_bytes());
        out
    }

    #[test]
    fn unpacks_a_made_up_archive() {
        let d = std::env::temp_dir().join(format!("pmu-unpack-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        let ggm: Vec<u8> = (0..5000u32).map(|i| (i % 251) as u8).collect();
        let res = vec![7u8; 3000];
        for extra in [0, 0x200] {
            std::fs::write(
                d.join(ARCHIVE),
                archive(
                    &[("globalgamemanagers", &ggm), ("level0.resS", &res)],
                    extra,
                ),
            )
            .unwrap();
            let mut lines = Vec::new();
            let names = unpack(&d, None, true, &mut |l| lines.push(l.to_string())).unwrap();
            assert_eq!(names, ["globalgamemanagers", "level0.resS"]);
            assert_eq!(std::fs::read(d.join("globalgamemanagers")).unwrap(), ggm);
            assert_eq!(std::fs::read(d.join("level0.resS")).unwrap(), res);
            assert!(!d.join(ARCHIVE).exists());
            assert_eq!(lines[0], "unpack: globalgamemanagers (5000 bytes)");
            assert!(
                lines[2].starts_with("unpack: 2 files from ")
                    && lines[2].ends_with(" (archive removed)")
            );
        }
        // a name that leaves the folder
        std::fs::write(d.join(ARCHIVE), archive(&[("../x", b"abc")], 0)).unwrap();
        let e = unpack(&d, None, false, &mut |_| {}).unwrap_err();
        assert!(
            e.to_string()
                .ends_with("file name '../x' leaves the folder"),
            "{e}"
        );
        let _ = std::fs::remove_dir_all(&d);
        assert!(unpack(&d, None, false, &mut |_| {}).is_err());
    }
}
