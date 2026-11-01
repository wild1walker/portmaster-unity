//! The archives android-data packs for Unity's Android player (android_data.py `_write_zip`,
//! `verify_zip`): STORE, no zip64 (libunity reads them with 32-bit offsets), every file's data
//! 4-byte aligned by a zipalign-style extra field, a fixed date; byte for byte what Python's
//! zipfile writes for the same entries.
use std::collections::HashMap;
use std::fs::{self, File};
use std::io::{self, Read, Seek, SeekFrom, Write};

use crate::pyrandom::Random;
use crate::zip::{Archive, STORED};
use crate::{fail, io_at, py, Error, Result};

/// libunity reads archives with fopen/fseek(long)/ftell: 32-bit offsets.
pub const ZIP_LIMIT: u64 = (1 << 31) - 1;

/// An entry's data: a file's, or bytes of its own.
pub enum Src {
    Path(String),
    Bytes(Vec<u8>),
}

impl Src {
    fn size(&self) -> Result<u64> {
        match self {
            Src::Bytes(b) => Ok(b.len() as u64),
            Src::Path(p) => Ok(fs::metadata(p).map_err(io_at(p))?.len()),
        }
    }
}

/// (2018, 8, 2, 0, 0, 0) as a DOS date and time.
const DOS_DATE: u16 = (2018 - 1980) << 9 | 8 << 5 | 2;
const DOS_TIME: u16 = 0;

struct Written {
    name: Vec<u8>,
    flags: u16,
    extra: Vec<u8>,
    crc: u32,
    size: u32,
    offset: u32,
}

fn local_header(w: &Written) -> Vec<u8> {
    let mut h = Vec::with_capacity(30 + w.name.len() + w.extra.len());
    h.extend(b"PK\x03\x04");
    h.extend([20, 0]);
    h.extend(w.flags.to_le_bytes());
    h.extend(0u16.to_le_bytes());
    h.extend(DOS_TIME.to_le_bytes());
    h.extend(DOS_DATE.to_le_bytes());
    h.extend(w.crc.to_le_bytes());
    h.extend(w.size.to_le_bytes());
    h.extend(w.size.to_le_bytes());
    h.extend((w.name.len() as u16).to_le_bytes());
    h.extend((w.extra.len() as u16).to_le_bytes());
    h.extend(&w.name);
    h.extend(&w.extra);
    h
}

/// The central directory and end record (ZipFile.close).
fn end_records(f: &mut impl Write, done: &[Written], start: u64) -> io::Result<()> {
    let mut cd = Vec::new();
    for w in done {
        cd.extend(b"PK\x01\x02");
        cd.extend([20, 3, 20, 0]);
        cd.extend(w.flags.to_le_bytes());
        cd.extend(0u16.to_le_bytes());
        cd.extend(DOS_TIME.to_le_bytes());
        cd.extend(DOS_DATE.to_le_bytes());
        cd.extend(w.crc.to_le_bytes());
        cd.extend(w.size.to_le_bytes());
        cd.extend(w.size.to_le_bytes());
        cd.extend((w.name.len() as u16).to_le_bytes());
        cd.extend((w.extra.len() as u16).to_le_bytes());
        cd.extend([0, 0, 0, 0, 0, 0]); // comment length, disk, internal attributes
        cd.extend((0o644u32 << 16).to_le_bytes());
        cd.extend(w.offset.to_le_bytes());
        cd.extend(&w.name);
        cd.extend(&w.extra);
    }
    let (n, cd_size) = (done.len() as u16, cd.len() as u32);
    cd.extend(b"PK\x05\x06");
    cd.extend([0, 0, 0, 0]);
    cd.extend(n.to_le_bytes());
    cd.extend(n.to_le_bytes());
    cd.extend(cd_size.to_le_bytes());
    cd.extend((start as u32).to_le_bytes());
    cd.extend(0u16.to_le_bytes());
    f.write_all(&cd)
}

/// Uncompressed (STORE) zip, no zip64, every file's data aligned to `align` bytes (zipalign-style
/// extra field 0xD935), written to path.tmp then renamed; Error::Exit past the 2 GiB limit. Returns
/// the archive's size.
pub fn write_zip(
    path: &str,
    entries: &[(String, Src)],
    log: &mut dyn FnMut(&str),
    align: u64,
) -> Result<u64> {
    let mut total = 22u64;
    for (a, s) in entries {
        total += 30 + 46 + 2 * a.len() as u64 + 8 + s.size()?;
    }
    if total > ZIP_LIMIT {
        return Err(Error::Exit(format!(
            "{path}: {total} bytes would exceed the 2 GiB limit"
        )));
    }
    let tmp = format!("{path}.tmp");
    let mut f = File::options()
        .read(true)
        .write(true)
        .create(true)
        .truncate(true)
        .open(&tmp)
        .map_err(io_at(&tmp))?;
    let mut done: Vec<Written> = Vec::new();
    let mut names: HashMap<&str, ()> = HashMap::new();
    let mut pos = 0u64;
    let mut buf = vec![0u8; 1 << 20];
    let mut result = Ok(());
    for (arc, src) in entries {
        // ZipInfo cuts a name at a NUL; the padding is reckoned on the whole name, as the Python does
        let name = arc.split('\0').next().unwrap_or("");
        let off = pos + 30 + arc.len() as u64;
        let pad = (align - (off + 4) % align) % align;
        let mut extra = vec![0x35, 0xD9];
        extra.extend((pad as u16).to_le_bytes());
        extra.resize(4 + pad as usize, 0);
        let size = match src.size() {
            Ok(s) => s,
            Err(e) => {
                result = Err(e);
                break;
            }
        };
        if names.insert(name, ()).is_some() {
            eprintln!("UserWarning: Duplicate name: {}", py::str_repr(name));
        }
        let why = if done.len() >= 0xFFFF {
            Some("Files count")
        } else if size > ZIP_LIMIT {
            Some("Filesize")
        } else if pos > ZIP_LIMIT {
            Some("Zipfile size")
        } else {
            None
        };
        if let Some(why) = why {
            result = fail(format!(
                "zipfile.LargeZipFile: {why} would require ZIP64 extensions"
            ));
            break;
        }
        let mut w = Written {
            name: name.as_bytes().to_vec(),
            flags: if name.is_ascii() { 0 } else { 0x800 },
            extra,
            crc: 0,
            size: 0,
            offset: pos as u32,
        };
        let step = (|| -> Result<()> {
            let mut src_file = match src {
                Src::Path(p) => Some(File::open(p).map_err(io_at(p))?),
                Src::Bytes(_) => None,
            };
            let head = local_header(&w);
            f.write_all(&head)?;
            let mut crc = crc32fast::Hasher::new();
            let mut n = 0u64;
            match (src, &mut src_file) {
                (Src::Bytes(b), _) => {
                    f.write_all(b)?;
                    crc.update(b);
                    n = b.len() as u64;
                }
                (_, Some(sf)) => loop {
                    let k = sf.read(&mut buf)?;
                    if k == 0 {
                        break;
                    }
                    f.write_all(&buf[..k])?;
                    crc.update(&buf[..k]);
                    n += k as u64;
                },
                _ => unreachable!(),
            }
            if n > ZIP_LIMIT {
                return fail("RuntimeError: File size too large, try using force_zip64");
            }
            w.crc = crc.finalize();
            w.size = n as u32;
            f.seek(SeekFrom::Start(pos))?;
            f.write_all(&local_header(&w))?;
            pos += head.len() as u64 + n;
            f.seek(SeekFrom::Start(pos))?;
            Ok(())
        })();
        if let Err(e) = step {
            result = Err(e);
            break;
        }
        done.push(w);
    }
    // (on an error too, as the Python's `with ZipFile` closes the archive)
    f.seek(SeekFrom::Start(pos))?;
    end_records(&mut f, &done, pos)?;
    drop(f);
    result?;
    let size = fs::metadata(&tmp)?.len();
    if size > ZIP_LIMIT {
        let _ = fs::remove_file(&tmp);
        return Err(Error::Exit(format!(
            "{path}: {size} bytes exceeds the 2 GiB limit"
        )));
    }
    fs::rename(&tmp, path).map_err(io_at(path))?;
    log(&format!(
        "pack: {path}: {} entries, {:.1} MiB",
        entries.len(),
        size as f64 / (1u64 << 20) as f64
    ));
    Ok(size)
}

/// verify_zip: the central directory as libunity's reader needs it (zip32, STORE) and the data in
/// place, aligned, for the entries random.Random(1).sample() picks and the last three.
pub fn verify_zip(
    path: &str,
    entries: &[(String, Src)],
    log: &mut dyn FnMut(&str),
    sample: usize,
) -> Result<()> {
    let z = Archive::open(path)?;
    let mut f = File::open(path).map_err(io_at(path))?;
    let infos = &z.entries;
    if infos.len() != entries.len() {
        return fail(format!(
            "AssertionError: ({}, {})",
            infos.len(),
            entries.len()
        ));
    }
    let bad: Vec<&str> = infos
        .iter()
        .filter(|i| i.method != STORED || i.file_size != i.compress_size)
        .map(|i| i.name.as_str())
        .collect();
    if !bad.is_empty() {
        let list: Vec<String> = bad.iter().take(5).map(|n| py::str_repr(n)).collect();
        return fail(format!("AssertionError: [{}]", list.join(", ")));
    }
    let src: HashMap<&str, &Src> = entries.iter().map(|(a, s)| (a.as_str(), s)).collect();
    let mut pick = Random::new(1).sample(infos.len(), sample.min(infos.len()));
    pick.extend(infos.len().saturating_sub(3)..infos.len());
    for i in pick.into_iter().map(|k| &infos[k]) {
        let at = i.header_offset as u64;
        f.seek(SeekFrom::Start(at))?;
        let mut h = Vec::new();
        (&mut f).take(30).read_to_end(&mut h)?;
        if h.len() < 30 {
            return fail("struct.error: unpack_from requires a buffer of at least 30 bytes for unpacking 4 bytes at offset 26 (actual buffer size is smaller)");
        }
        let (n, e) = (
            u16::from_le_bytes([h[26], h[27]]) as u64,
            u16::from_le_bytes([h[28], h[29]]) as u64,
        );
        let data_off = at + 30 + n + e;
        if data_off % 4 != 0 {
            return fail(format!("AssertionError: {}", i.name));
        }
        f.seek(SeekFrom::Start(data_off))?;
        let mut got = Vec::new();
        (&mut f)
            .take(i.file_size.min(1 << 16))
            .read_to_end(&mut got)?;
        let want = match src.get(i.name.as_str()) {
            None => return fail(format!("KeyError: {}", py::str_repr(&i.name))),
            Some(Src::Bytes(b)) => b[..got.len().min(b.len())].to_vec(),
            Some(Src::Path(p)) => {
                let mut v = Vec::new();
                File::open(p)
                    .map_err(io_at(p))?
                    .take(got.len() as u64)
                    .read_to_end(&mut v)?;
                v
            }
        };
        if got != want {
            return fail(format!("AssertionError: {}", i.name));
        }
    }
    log(&format!(
        "pack: {} verified (zip32, stored, aligned, data in place)",
        py::basename(path)
    ));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn writes_aligned_and_verifies() {
        let dir = std::env::temp_dir().join(format!("pkgzip-zipwrite-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let file = dir.join("big.bin").to_string_lossy().into_owned();
        fs::write(
            &file,
            (0..200_000u32).map(|i| (i % 251) as u8).collect::<Vec<_>>(),
        )
        .unwrap();
        let entries = vec![
            ("assets/build-id".to_string(), Src::Bytes(Vec::new())),
            ("assets/bin/Data/x".to_string(), Src::Path(file.clone())),
            ("assets/é".to_string(), Src::Bytes(b"abc".to_vec())),
        ];
        let out = dir.join("t.obb").to_string_lossy().into_owned();
        let mut lines = Vec::new();
        let size = write_zip(&out, &entries, &mut |m| lines.push(m.to_string()), 4).unwrap();
        verify_zip(&out, &entries, &mut |m| lines.push(m.to_string()), 40).unwrap();
        assert_eq!(size, fs::metadata(&out).unwrap().len());
        assert!(!fs::exists(format!("{out}.tmp")).unwrap());
        let z = Archive::open(&out).unwrap();
        assert_eq!(
            z.names().collect::<Vec<_>>(),
            ["assets/build-id", "assets/bin/Data/x", "assets/é"]
        );
        for e in &z.entries {
            assert_eq!(z.data_offset(e).unwrap() % 4, 0);
            assert_eq!(e.flag_bits, if e.name.is_ascii() { 0 } else { 0x800 });
        }
        assert_eq!(
            z.read("assets/bin/Data/x").unwrap(),
            fs::read(&file).unwrap()
        );
        assert_eq!(
            lines[1],
            "pack: t.obb verified (zip32, stored, aligned, data in place)"
        );
        fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn fails_without_its_folder() {
        let one = vec![("a".to_string(), Src::Bytes(vec![0; 16]))];
        let e = write_zip("/nonexistent/x.zip", &one, &mut |_| {}, 4).unwrap_err();
        assert_eq!(e.status(), 1);
    }
}
