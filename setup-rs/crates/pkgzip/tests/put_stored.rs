//! Archive::put_stored: entries given new data after the archive's end (an installed game's APK
//! patched by setup/update); the archive reads back with the new data, the other entries as they
//! were, none of the old bytes changed; an interrupted put is cut back by recover_put.
use pkgzip::zip::{recover_put, Archive};
use pkgzip::zipwrite::{write_zip, Src};

fn sample(name: &str) -> (std::path::PathBuf, String) {
    let dir = std::env::temp_dir().join(format!("pkgzip-put-{name}-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("t.apk").to_string_lossy().into_owned();
    let entries = vec![
        (
            "assets/bin/Data/Managed/A.dll".to_string(),
            Src::Bytes(vec![1u8; 3000]),
        ),
        (
            "assets/bin/Data/Managed/B.dll".to_string(),
            Src::Bytes(vec![2u8; 5000]),
        ),
        (
            "assets/bin/Data/data.unity3d".to_string(),
            Src::Bytes(b"the rest".to_vec()),
        ),
    ];
    write_zip(&path, &entries, &mut |_| {}, 4).unwrap();
    (dir, path)
}

#[test]
fn put_after_the_end() {
    let (dir, path) = sample("end");
    let before = std::fs::read(&path).unwrap();
    let longer = vec![9u8; 4001];
    let shorter = b"short".to_vec();
    let z = Archive::open(&path).unwrap();
    z.put_stored(
        &path,
        &[
            ("assets/bin/Data/Managed/A.dll", &longer),
            ("assets/bin/Data/Managed/B.dll", &shorter),
        ],
    )
    .unwrap();
    assert!(z
        .put_stored(&path, &[("assets/bin/Data/Managed/C.dll", b"new")])
        .is_err());
    drop(z);
    let after = std::fs::read(&path).unwrap();
    assert_eq!(&after[..before.len()], &before[..]);
    assert!(!std::path::Path::new(&format!("{path}.put")).exists());
    let z = Archive::open(&path).unwrap();
    // read() checks each entry's CRC-32 against the central directory's
    assert_eq!(z.read("assets/bin/Data/Managed/A.dll").unwrap(), longer);
    assert_eq!(z.read("assets/bin/Data/Managed/B.dll").unwrap(), shorter);
    assert_eq!(z.read("assets/bin/Data/data.unity3d").unwrap(), b"the rest");
    assert_eq!(z.names().count(), 3);
    for n in ["assets/bin/Data/Managed/A.dll", "assets/bin/Data/Managed/B.dll"] {
        let e = z.info(n).unwrap();
        assert!(e.header_offset as usize >= before.len());
        assert_eq!(z.data_offset(e).unwrap() % 4, 0);
    }
    // and again: the archive put to before puts as well
    z.put_stored(&path, &[("assets/bin/Data/Managed/A.dll", b"third")])
        .unwrap();
    drop(z);
    let z = Archive::open(&path).unwrap();
    assert_eq!(z.read("assets/bin/Data/Managed/A.dll").unwrap(), b"third");
    assert_eq!(z.read("assets/bin/Data/Managed/B.dll").unwrap(), shorter);
    std::fs::remove_dir_all(&dir).ok();
}

#[test]
fn interrupted_put_cut_back() {
    let (dir, path) = sample("cut");
    let before = std::fs::read(&path).unwrap();
    assert!(!recover_put(&path).unwrap());
    // what a put stopped halfway leaves: its journal and some of the new bytes
    std::fs::write(format!("{path}.put"), before.len().to_string()).unwrap();
    let mut half = before.clone();
    half.extend(b"PK\x03\x04 half an entry");
    std::fs::write(&path, &half).unwrap();
    assert!(recover_put(&path).unwrap());
    assert_eq!(std::fs::read(&path).unwrap(), before);
    assert!(!std::path::Path::new(&format!("{path}.put")).exists());
    Archive::open(&path).unwrap();
    std::fs::remove_dir_all(&dir).ok();
}
