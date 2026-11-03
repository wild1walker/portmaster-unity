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
        ("assets/bin/Data/a.assets".to_string(), Src::Bytes(b"first entry".to_vec())),
        ("assets/bin/Data/a.resource".to_string(), Src::Bytes(vec![7u8; 4096])),
        ("assets/bin/Data/z".to_string(), Src::Bytes(b"last".to_vec())),
    ];
    write_zip(&path, &entries, &mut |_| {}, 4).unwrap();
    let size = std::fs::metadata(&path).unwrap().len();
    let mut new = vec![1u8; 1000];
    new.resize(4096, 0);
    let z = Archive::open(&path).unwrap();
    z.overwrite_stored(&path, "assets/bin/Data/a.resource", &new).unwrap();
    assert!(z.overwrite_stored(&path, "assets/bin/Data/z", b"longer!").is_err());
    drop(z);
    assert_eq!(std::fs::metadata(&path).unwrap().len(), size);
    let z = Archive::open(&path).unwrap();
    // read() checks each entry's CRC-32 against the central directory's
    assert_eq!(z.read("assets/bin/Data/a.resource").unwrap(), new);
    assert_eq!(z.info("assets/bin/Data/a.resource").unwrap().crc, crc32fast::hash(&new));
    assert_eq!(z.read("assets/bin/Data/a.assets").unwrap(), b"first entry");
    assert_eq!(z.read("assets/bin/Data/z").unwrap(), b"last");
    // the local header's CRC too
    let raw = std::fs::read(&path).unwrap();
    let at = z.info("assets/bin/Data/a.resource").unwrap().header_offset as usize;
    assert_eq!(&raw[at + 14..at + 18], &crc32fast::hash(&new).to_le_bytes());
    std::fs::remove_dir_all(&dir).ok();
}
