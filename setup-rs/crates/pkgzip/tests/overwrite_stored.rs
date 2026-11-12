//! Archive::overwrite_stored: a stored entry's bytes replaced in place (as the video step does in
//! an installed game's APK and OBB); the archive reads back with the new data and CRCs, the other
//! entries as they were; a length change or a compressed entry is refused.
use pkgzip::zip::Archive;
use pkgzip::zipwrite::{write_zip, Src};

#[test]
fn overwrite_in_place() {
    let dir = std::env::temp_dir().join(format!("pkgzip-overwrite-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("t.obb").to_string_lossy().into_owned();
    let entries = vec![
        (
            "assets/bin/Data/a.assets".to_string(),
            Src::Bytes(b"first entry".to_vec()),
        ),
        (
            "assets/bin/Data/a.resource".to_string(),
            Src::Bytes(vec![7u8; 4096]),
        ),
        (
            "assets/bin/Data/z".to_string(),
            Src::Bytes(b"last".to_vec()),
        ),
    ];
    write_zip(&path, &entries, &mut |_| {}, 4).unwrap();
    let size = std::fs::metadata(&path).unwrap().len();
    let mut new = vec![1u8; 1000];
    new.resize(4096, 0);
    let z = Archive::open(&path).unwrap();
    z.overwrite_stored(&path, "assets/bin/Data/a.resource", &new)
        .unwrap();
    assert!(z
        .overwrite_stored(&path, "assets/bin/Data/z", b"longer!")
        .is_err());
    drop(z);
    assert_eq!(std::fs::metadata(&path).unwrap().len(), size);
    let z = Archive::open(&path).unwrap();
    // read() checks each entry's CRC-32 against the central directory's
    assert_eq!(z.read("assets/bin/Data/a.resource").unwrap(), new);
    assert_eq!(
        z.info("assets/bin/Data/a.resource").unwrap().crc,
        crc32fast::hash(&new)
    );
    assert_eq!(z.read("assets/bin/Data/a.assets").unwrap(), b"first entry");
    assert_eq!(z.read("assets/bin/Data/z").unwrap(), b"last");
    // the local header's CRC too
    let raw = std::fs::read(&path).unwrap();
    let at = z.info("assets/bin/Data/a.resource").unwrap().header_offset as usize;
    assert_eq!(&raw[at + 14..at + 18], &crc32fast::hash(&new).to_le_bytes());
    std::fs::remove_dir_all(&dir).ok();
}

/// A reader that gives its bytes a few at a time (a file read in pieces).
struct Drip<'a>(&'a [u8], usize);

impl std::io::Read for Drip<'_> {
    fn read(&mut self, out: &mut [u8]) -> std::io::Result<usize> {
        let n = self.1.min(out.len()).min(self.0.len());
        out[..n].copy_from_slice(&self.0[..n]);
        self.0 = &self.0[n..];
        Ok(n)
    }
}

/// overwrite_stored_from (the data read a piece at a time) leaves the archive byte for byte as
/// overwrite_stored (the data whole) does; a source that ends early is an error.
#[test]
fn overwrite_from_a_reader_is_the_same() {
    let dir = std::env::temp_dir().join(format!("pkgzip-overwrite-from-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    // (more than one of the function's 1 MiB pieces)
    let len = (2 << 20) + 12345;
    let entries = vec![
        (
            "assets/bin/Data/a.assets".to_string(),
            Src::Bytes(b"first entry".to_vec()),
        ),
        (
            "assets/bin/Data/a.resource".to_string(),
            Src::Bytes(vec![7u8; len]),
        ),
        (
            "assets/bin/Data/z".to_string(),
            Src::Bytes(b"last".to_vec()),
        ),
    ];
    let new: Vec<u8> = (0..len).map(|i| (i * 31 % 251) as u8).collect();
    let (whole, piecewise) = (
        dir.join("whole.obb").to_string_lossy().into_owned(),
        dir.join("pieces.obb").to_string_lossy().into_owned(),
    );
    for p in [&whole, &piecewise] {
        write_zip(p, &entries, &mut |_| {}, 4).unwrap();
    }
    Archive::open(&whole)
        .unwrap()
        .overwrite_stored(&whole, "assets/bin/Data/a.resource", &new)
        .unwrap();
    Archive::open(&piecewise)
        .unwrap()
        .overwrite_stored_from(
            &piecewise,
            "assets/bin/Data/a.resource",
            &mut Drip(&new, 70_001),
            len as u64,
        )
        .unwrap();
    assert!(std::fs::read(&whole).unwrap() == std::fs::read(&piecewise).unwrap());
    let z = Archive::open(&piecewise).unwrap();
    assert!(z.read("assets/bin/Data/a.resource").unwrap() == new);
    assert!(z
        .overwrite_stored_from(
            &piecewise,
            "assets/bin/Data/a.resource",
            &mut Drip(&new[..100], 64),
            len as u64,
        )
        .is_err());
    std::fs::remove_dir_all(&dir).ok();
}
