//! The split path on made-up data (no Unity or game files): a Unity 2021.3 serialized file
//! (format 22, no type trees, as a player build has it) whose textures and a mesh stream from its
//! .resS, too big for one archive inlined at a lowered limit: the .resS is cut into parts at
//! stream boundaries, the StreamingInfo of what moved is rewritten, the parts are placed over the
//! APK, the main and the patch OBB within the limit, and every texture and the mesh read back
//! from the archives (each entry looked up in every archive, as the player's VFS does) give
//! their original bytes. The class database is UnityPy's lzma.tpk ($PMU_TPK; skipped without).
use androiddata::pack::{apk_margin, limit_from, obb_margin, place_archives};
use androiddata::ress_split::{split_serialized, SplitRule};
use pkgzip::zip::Archive;
use pkgzip::zipwrite::{write_zip, Src, ZIP_LIMIT};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use unityfs::bin::Writer;
use unityfs::inline::{stream_refs, PLANNED};
use unityfs::typetree::Kind;
use unityfs::{ClassDb, MemSource, SerializedFile, Struct, TypeTree, UnityVersion, Value};

const VERSION: &str = "2021.3.30f1";

fn db() -> Option<Arc<ClassDb>> {
    std::env::var_os("PMU_TPK")?;
    Some(Arc::new(ClassDb::default_db().expect("PMU_TPK")))
}

fn tree(db: &ClassDb, class_id: i32) -> Arc<TypeTree> {
    db.typetree(class_id, UnityVersion::parse(VERSION).unwrap())
        .unwrap()
}

/// A node's default value: zeros, empty strings and arrays.
fn default_of(t: &TypeTree, i: usize) -> Value {
    let n = &t.nodes[i];
    match n.kind {
        Kind::Bool => Value::Bool(false),
        Kind::I8 => Value::I8(0),
        Kind::U8 => Value::U8(0),
        Kind::I16 => Value::I16(0),
        Kind::U16 => Value::U16(0),
        Kind::I32 => Value::I32(0),
        Kind::U32 => Value::U32(0),
        Kind::I64 => Value::I64(0),
        Kind::U64 => Value::U64(0),
        Kind::F32 => Value::F32(0.0),
        Kind::F64 => Value::F64(0.0),
        Kind::Str => Value::Str(Vec::new()),
        Kind::Bytes => Value::Bytes(Vec::new()),
        Kind::Pair => Value::Pair(Box::new((
            default_of(t, n.children[0]),
            default_of(t, n.children[1]),
        ))),
        _ if n
            .children
            .first()
            .is_some_and(|&c| &*t.nodes[c].type_name == "Array") =>
        {
            Value::Array(Vec::new())
        }
        _ => Value::Struct(Struct {
            fields: n
                .children
                .iter()
                .map(|&c| (t.nodes[c].name.clone(), default_of(t, c)))
                .collect(),
        }),
    }
}

/// An integer field set, keeping its kind.
fn set_int(v: &mut Value, path: &[&str], x: u64) {
    let (last, head) = path.split_last().unwrap();
    let s = v.path_mut(head).unwrap();
    let n = match s.get(last).unwrap() {
        Value::U64(_) => Value::U64(x),
        Value::I64(_) => Value::I64(x as i64),
        Value::I32(_) => Value::I32(x as i32),
        Value::U32(_) => Value::U32(x as u32),
        o => panic!("{last}: {o:?}"),
    };
    s.set(last, n).unwrap();
}

fn set(v: &mut Value, path: &[&str], x: Value) {
    let (last, head) = path.split_last().unwrap();
    v.path_mut(head).unwrap().set(last, x).unwrap();
}

/// A Texture2D (class 28) or Mesh (43) streaming `size` bytes at `off` of `path`, or a texture
/// with its pixels inline.
fn object(
    db: &ClassDb,
    class_id: i32,
    name: &str,
    stream: Option<(u64, u64, &str)>,
    inline: &[u8],
) -> Value {
    let mut v = default_of(&tree(db, class_id), 0);
    set(&mut v, &["m_Name"], Value::str(name));
    if class_id == 28 {
        set(&mut v, &["m_Width"], Value::I32(16));
        set(&mut v, &["m_Height"], Value::I32(16));
        set(&mut v, &["m_TextureFormat"], Value::I32(4));
        set(&mut v, &["m_MipCount"], Value::I32(1));
        set(&mut v, &["m_ImageCount"], Value::I32(1));
        set(&mut v, &["m_TextureDimension"], Value::I32(2));
        set(&mut v, &["image data"], Value::Bytes(inline.to_vec()));
        let n = stream.map_or(inline.len() as u64, |s| s.1);
        set_int(&mut v, &["m_CompleteImageSize"], n);
    }
    if let Some((off, size, path)) = stream {
        set_int(&mut v, &["m_StreamData", "offset"], off);
        set_int(&mut v, &["m_StreamData", "size"], size);
        set(&mut v, &["m_StreamData", "path"], Value::str(path));
    }
    v
}

/// A serialized file of format 22 (Unity 2021.3, little endian, no type trees) holding the
/// objects (class id, path id, value).
fn serialized22(db: &ClassDb, objs: &[(i32, i64, Value)]) -> Vec<u8> {
    let mut classes: Vec<i32> = Vec::new();
    for o in objs {
        if !classes.contains(&o.0) {
            classes.push(o.0);
        }
    }
    let mut m = Writer::new(false);
    m.cstr(VERSION.as_bytes());
    m.i32(24); // StandaloneLinux64
    m.u8(0);
    m.i32(classes.len() as i32);
    for &c in &classes {
        m.i32(c);
        m.u8(0);
        m.i16(-1);
        m.bytes(&(c as u32).to_le_bytes().repeat(4));
    }
    let mut data = Vec::new();
    let mut table = Vec::new();
    for (c, pid, v) in objs {
        let b = unityfs::value::write(&tree(db, *c), v, false, None).unwrap();
        while data.len() % 8 != 0 {
            data.push(0);
        }
        let ti = classes.iter().position(|x| x == c).unwrap();
        table.push((*pid, data.len() as u64, b.len() as u32, ti as i32));
        data.extend_from_slice(&b);
    }
    m.i32(table.len() as i32);
    for (pid, off, size, ti) in &table {
        while (m.len() + 48) % 4 != 0 {
            m.u8(0);
        }
        m.i64(*pid);
        m.u64(*off);
        m.u32(*size);
        m.i32(*ti);
    }
    m.i32(0); // script types
    m.i32(0); // externals
    m.i32(0); // ref types
    m.cstr(b"");
    let data_offset = (48 + m.len()).div_ceil(16) * 16;
    let mut f = Writer::new(true);
    f.u32(0);
    f.u32(0);
    f.u32(22);
    f.u32(0);
    f.bytes(&[0, 0, 0, 0]);
    f.u32(m.len() as u32);
    f.u64((data_offset + data.len()) as u64);
    f.u64(data_offset as u64);
    f.i64(0);
    f.bytes(&m.buf);
    f.buf.resize(data_offset, 0);
    f.bytes(&data);
    f.buf
}

/// Bytes that differ from blob to blob.
fn noise(seed: u64, n: usize) -> Vec<u8> {
    let mut x = seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1;
    (0..n)
        .map(|_| {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            (x >> 24) as u8
        })
        .collect()
}

fn tmpdir(tag: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("ress-split-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

/// The made-up file: textures and a mesh streaming from resources.assets.resS (their blobs at
/// 16-byte aligned offsets, not in path id order, a gap of unread bytes), and a texture inline.
/// Returns (path id -> its blob's bytes).
fn make(db: &ClassDb, dir: &Path) -> Vec<(i64, Vec<u8>)> {
    let res = "resources.assets.resS";
    let mut stream = Vec::new();
    let mut objs = Vec::new();
    let mut blobs = Vec::new();
    // (path id, class, size) in the stream's order
    let layout: Vec<(i64, i32, usize)> = vec![
        (7, 28, 2100),
        (3, 28, 1700),
        (5, 28, 2900),
        (2, 28, 800),
        (9, 28, 2400),
        (4, 28, 3100),
        (11, 28, 1200),
        (8, 28, 2600),
        (6, 28, 2000),
        (12, 43, 1500),
        (10, 28, 3000),
        (13, 28, 1900),
    ];
    for (k, (pid, class, size)) in layout.into_iter().enumerate() {
        while stream.len() % 16 != 0 {
            stream.push(0);
        }
        if k == 6 {
            stream.extend_from_slice(&[0xAA; 48]); // read by nothing
        }
        let b = noise(pid as u64, size);
        let off = stream.len() as u64;
        stream.extend_from_slice(&b);
        let name = format!("o{pid}");
        objs.push((
            class,
            pid,
            object(db, class, &name, Some((off, size as u64, res)), &[]),
        ));
        blobs.push((pid, b));
    }
    objs.push((28, 1, object(db, 28, "inline", None, &noise(1, 1024))));
    objs.sort_by_key(|o| o.1);
    std::fs::write(dir.join("resources.assets"), serialized22(db, &objs)).unwrap();
    std::fs::write(dir.join(res), &stream).unwrap();
    blobs
}

fn open(path: &Path, db: &Arc<ClassDb>) -> SerializedFile {
    let b = std::fs::read(path).unwrap();
    SerializedFile::open(
        "resources.assets",
        Arc::new(MemSource(b)),
        Some(db.clone()),
        None,
    )
    .unwrap()
}

/// Every object's value with its m_StreamData's path and offset left out (all else must stay).
fn without_stream(sf: &SerializedFile) -> Vec<(i64, Value)> {
    sf.objects
        .iter()
        .map(|o| {
            let mut v = sf.read_typetree(o.path_id).unwrap();
            if let Some(sd) = v.get_mut("m_StreamData") {
                sd.set("path", Value::Str(Vec::new())).unwrap();
                sd.set("offset", Value::U64(0)).unwrap();
            }
            (o.path_id, v)
        })
        .collect()
}

/// (path id -> (file, offset, size)) of every stream.
fn streams(sf: &SerializedFile) -> Vec<(i64, String, u64, u64)> {
    let mut v: Vec<_> = stream_refs(sf, PLANNED)
        .unwrap()
        .into_iter()
        .map(|r| (r.path_id, r.file(), r.offset, r.size))
        .collect();
    v.sort();
    v
}

#[test]
fn split_place_read_back() {
    let Some(db) = db() else {
        eprintln!("PMU_TPK not set: skipped");
        return;
    };
    let d = tmpdir("a");
    let (src, out) = (d.join("src"), d.join("out"));
    std::fs::create_dir_all(&src).unwrap();
    let blobs = make(&db, &src);
    let srcf = src.join("resources.assets");
    let res = std::fs::read(src.join("resources.assets.resS")).unwrap();
    let before = open(&srcf, &db);
    let refs = stream_refs(&before, PLANNED).unwrap();
    assert_eq!(refs.len(), 12);

    // the archive limit lowered (as PMU_ZIP_LIMIT does) and parts of at most 10000 bytes (as
    // PMU_RESS_SPLIT): the file passes them inlined
    let limit = limit_from(Some("16384")).unwrap();
    let rule = SplitRule::new(limit, Some("10000")).unwrap();
    let inlined =
        std::fs::metadata(&srcf).unwrap().len() + refs.iter().map(|r| r.size).sum::<u64>();
    assert!(rule.wants("resources.assets", inlined, None, true));
    assert!(SplitRule::new(limit, None)
        .unwrap()
        .wants("resources.assets", inlined, None, true));

    let mut sf = open(&srcf, &db);
    let dst = out.join("resources.assets");
    let sp = split_serialized(
        &mut sf,
        refs,
        srcf.to_str().unwrap(),
        dst.to_str().unwrap(),
        rule.part,
        false,
        &|_| Ok(()),
    )
    .unwrap();
    drop(sf);
    let names: Vec<&str> = sp.parts.iter().map(|p| p.0.as_str()).collect();
    assert_eq!(
        names,
        [
            "resources.assets.resS",
            "resources.assets.1.resS",
            "resources.assets.2.resS"
        ]
    );
    assert_eq!((sp.textures, sp.meshes), (11, 1));
    assert!(sp.unread >= 48 && sp.unread < 48 + 12 * 16, "{}", sp.unread);
    // the parts are the .resS cut in three, each within the part size
    let mut joined = Vec::new();
    for (n, z) in &sp.parts {
        let b = std::fs::read(out.join(n)).unwrap();
        assert_eq!(b.len() as u64, *z);
        assert!(*z <= rule.part, "{n}: {z} bytes");
        joined.extend(b);
    }
    assert_eq!(joined, res);
    // the source is left as it was (no consume)
    assert_eq!(
        std::fs::read(src.join("resources.assets.resS")).unwrap(),
        res
    );

    // the new file: Android, the same objects, only m_StreamData's path and offset changed
    let after = open(&dst, &db);
    assert_eq!(after.target_platform, 13);
    assert_eq!(without_stream(&after), without_stream(&before));
    let old = streams(&before);
    let new = streams(&after);
    let mut moved = 0;
    for (o, n) in old.iter().zip(&new) {
        assert_eq!((o.0, o.3), (n.0, n.3));
        if n.1 == "resources.assets.resS" {
            assert_eq!(o.2, n.2, "part 0 keeps its offsets");
        } else {
            moved += 1;
        }
    }
    assert_eq!(moved, sp.moved);
    let mesh = new.iter().find(|x| x.0 == 12).unwrap();
    assert_ne!(
        mesh.1, "resources.assets.resS",
        "the mesh moves to a later part"
    );
    assert!(moved >= 4);
    // each blob read from its part on disk
    for (pid, file, off, size) in &new {
        let b = std::fs::read(out.join(file)).unwrap();
        let want = &blobs.iter().find(|x| x.0 == *pid).unwrap().1;
        assert_eq!(
            &b[*off as usize..(*off + *size) as usize],
            &want[..],
            "object {pid}"
        );
    }

    // packed: bin/Data in the APK until it passes the lowered limit, the rest over both OBBs
    let file = |n: &str| Src::Path(out.join(n).to_string_lossy().into_owned());
    let filler = |n: &str, size: usize| {
        let p = d.join(n);
        std::fs::write(&p, noise(size as u64, size)).unwrap();
        Src::Path(p.to_string_lossy().into_owned())
    };
    let mut apk: Vec<(String, Src)> = vec![
        (
            "assets/bin/Data/Managed/Game.dll".into(),
            filler("dll", 2000),
        ),
        (
            "assets/bin/Data/globalgamemanagers".into(),
            filler("ggm", 900),
        ),
        ("assets/bin/Data/level0".into(), filler("level0", 700)),
        (
            "assets/bin/Data/resources.assets".into(),
            file("resources.assets"),
        ),
    ];
    for n in &names {
        apk.push((format!("assets/bin/Data/{n}"), file(n)));
    }
    apk.sort_by(|a, b| a.0.cmp(&b.0));
    let obb = vec![
        ("assets/AssetBundles/a".into(), filler("a", 6000)),
        ("assets/AssetBundles/b".into(), filler("b", 6000)),
    ];
    let marker = vec![("assets/build-id".to_string(), Src::Bytes(Vec::new()))];
    let anames = [
        "Game.apk".to_string(),
        "main.1.x.obb".into(),
        "patch.1.x.obb".into(),
    ];
    let mut log = Vec::new();
    let (a, m, p) = place_archives(apk, obb, &marker, limit, "all", &anames, &mut |l| {
        log.push(l.to_string())
    })
    .unwrap();
    assert!(
        log[0].starts_with("pack: the data passes 2 GiB: "),
        "{log:?}"
    );
    assert!(log[1].contains("spread by size"), "{log:?}");
    assert_eq!(
        log[2],
        "pack: resources.assets.resS split in 3 parts (its file too big for one archive with its textures inlined): \
         resources.assets.resS in Game.apk, resources.assets.1.resS in main.1.x.obb, resources.assets.2.resS in patch.1.x.obb"
    );
    let mut patch = marker
        .iter()
        .map(|(n, s)| (n.clone(), clone(s)))
        .collect::<Vec<_>>();
    patch.extend(p);
    let mut archives = Vec::new();
    for (n, e) in [
        ("Game.apk", a),
        ("main.1.x.obb", m),
        ("patch.1.x.obb", patch),
    ] {
        let z = d.join(n).to_string_lossy().into_owned();
        write_zip(&z, &e, &mut |_| {}, 4).unwrap();
        let size = std::fs::metadata(&z).unwrap().len();
        assert!(size <= limit, "{n}: {size} bytes");
        archives.push(Archive::open(&z).unwrap());
    }
    // read back as the player does: each file looked up in every archive
    let find = |name: &str| -> Vec<u8> {
        let e = format!("assets/bin/Data/{name}");
        let hits: Vec<Vec<u8>> = archives.iter().filter_map(|z| z.read(&e).ok()).collect();
        assert_eq!(hits.len(), 1, "{e}");
        hits.into_iter().next().unwrap()
    };
    let packed = SerializedFile::open(
        "resources.assets",
        Arc::new(MemSource(find("resources.assets"))),
        Some(db.clone()),
        None,
    )
    .unwrap();
    for (pid, file, off, size) in streams(&packed) {
        let b = find(&file);
        let want = &blobs.iter().find(|x| x.0 == pid).unwrap().1;
        assert_eq!(
            &b[off as usize..(off + size) as usize],
            &want[..],
            "object {pid}"
        );
    }

    // consumed: the source gone, part 0 its .resS cut short
    let out2 = d.join("out2");
    let mut sf = open(&srcf, &db);
    let refs = stream_refs(&sf, PLANNED).unwrap();
    let dst2 = out2.join("resources.assets");
    let sp2 = split_serialized(
        &mut sf,
        refs,
        srcf.to_str().unwrap(),
        dst2.to_str().unwrap(),
        rule.part,
        true,
        &|_| Ok(()),
    )
    .unwrap();
    drop(sf);
    assert_eq!(sp2.parts, sp.parts);
    assert!(!srcf.exists() && !src.join("resources.assets.resS").exists());
    assert_eq!(std::fs::read(&dst2).unwrap(), std::fs::read(&dst).unwrap());
    for (n, _) in &sp.parts {
        assert_eq!(
            std::fs::read(out2.join(n)).unwrap(),
            std::fs::read(out.join(n)).unwrap()
        );
    }
    let _ = std::fs::remove_dir_all(&d);
}

fn clone(s: &Src) -> Src {
    match s {
        Src::Path(p) => Src::Path(p.clone()),
        Src::Bytes(b) => Src::Bytes(b.clone()),
    }
}

#[test]
fn limits() {
    assert_eq!(limit_from(None).unwrap(), ZIP_LIMIT);
    assert_eq!(limit_from(Some("16384")).unwrap(), 16384);
    assert!(limit_from(Some("x")).is_err() && limit_from(Some("10")).is_err());
    assert_eq!(apk_margin(ZIP_LIMIT), 64 << 20);
    assert_eq!(obb_margin(ZIP_LIMIT), 16 << 20);
    assert_eq!(
        SplitRule::new(ZIP_LIMIT, None).unwrap().part,
        ZIP_LIMIT - (64 << 20)
    );
}

/// What fits in order is placed as before: the main OBB in order, the rest in the patch OBB.
#[test]
fn in_order_unchanged() {
    let d = tmpdir("b");
    let f = |n: &str, size: usize| {
        let p = d.join(n);
        std::fs::write(&p, vec![0u8; size]).unwrap();
        Src::Path(p.to_string_lossy().into_owned())
    };
    let limit = 16384;
    let apk = vec![("assets/bin/Data/level0".to_string(), f("l0", 100))];
    let obb: Vec<(String, Src)> = (0..5)
        .map(|k| {
            (
                format!("assets/AssetBundles/b{k}"),
                f(&format!("b{k}"), 5000),
            )
        })
        .collect();
    let marker = vec![("assets/id".to_string(), Src::Bytes(Vec::new()))];
    let n = ["a.apk".to_string(), "m.obb".into(), "p.obb".into()];
    let mut log = Vec::new();
    let (a, m, p) = place_archives(apk, obb, &marker, limit, "all", &n, &mut |l| {
        log.push(l.to_string())
    })
    .unwrap();
    let names = |v: &[(String, Src)]| v.iter().map(|e| e.0.clone()).collect::<Vec<_>>();
    assert_eq!(names(&a), ["assets/bin/Data/level0"]);
    assert_eq!(
        names(&m),
        [
            "assets/id",
            "assets/AssetBundles/b0",
            "assets/AssetBundles/b1",
            "assets/AssetBundles/b2"
        ]
    );
    assert_eq!(
        names(&p),
        ["assets/AssetBundles/b3", "assets/AssetBundles/b4"]
    );
    assert!(log.is_empty(), "{log:?}");
    let _ = std::fs::remove_dir_all(&d);
}

/// A file that fits in some archive whole is not split and packs as before: one in the old
/// 48 MiB window (under the APK's margin, over the OBBs') alone in the main OBB, the largest
/// that fits at all alone in the patch OBB (sparse files: nothing is written).
#[test]
fn whole_files_not_split() {
    let d = tmpdir("c");
    let sparse = |n: &str, size: u64| {
        let p = d.join(n);
        std::fs::File::create(&p).unwrap().set_len(size).unwrap();
        Src::Path(p.to_string_lossy().into_owned())
    };
    let rule = SplitRule::new(ZIP_LIMIT, None).unwrap();
    let name = "resources.assets";
    let bid = "0123abcd-0000-1111-2222-333344445555";
    let marker = vec![
        (format!("assets/{bid}"), Src::Bytes(Vec::new())),
        (
            "unity_obb_guid".to_string(),
            Src::Bytes(bid.as_bytes().to_vec()),
        ),
    ];
    let n = ["a.apk".to_string(), "m.obb".into(), "p.obb".into()];
    let names = |v: &[(String, Src)]| v.iter().map(|e| e.0.clone()).collect::<Vec<_>>();
    let window = ZIP_LIMIT - (40 << 20);
    let max = ZIP_LIMIT - 22 - 170 - 148 - 84 - 2 * 32;
    for (size, main_has_it) in [(window, true), (max, false)] {
        assert!(!rule.wants(name, size, None, true), "{size}");
        let apk = vec![
            (
                "assets/bin/Data/level0".to_string(),
                sparse("level0", 1 << 20),
            ),
            (format!("assets/bin/Data/{name}"), sparse(name, size)),
        ];
        let obb = if main_has_it {
            vec![("assets/AssetBundles/a".to_string(), sparse("a", 100 << 20))]
        } else {
            Vec::new()
        };
        let mut log = Vec::new();
        let (a, m, p) = place_archives(apk, obb, &marker, ZIP_LIMIT, "all", &n, &mut |l| {
            log.push(l.to_string())
        })
        .unwrap();
        assert_eq!(names(&a), ["assets/bin/Data/level0"]);
        assert_eq!(log.len(), 1, "{log:?}"); // the move to the OBBs, nothing spread
        let it = vec![format!("assets/bin/Data/{name}")];
        if main_has_it {
            assert_eq!(names(&m)[2..], it[..]);
            assert_eq!(names(&p), ["assets/AssetBundles/a"]);
        } else {
            assert_eq!(m.len(), 2);
            assert_eq!(names(&p), it);
            // the patch OBB is then exactly the limit
            let mut all: Vec<&(String, Src)> = marker.iter().collect();
            all.extend(p.iter());
            let total: u64 = 22
                + all
                    .iter()
                    .map(|(a, s)| {
                        84 + 2 * a.len() as u64
                            + match s {
                                Src::Bytes(b) => b.len() as u64,
                                Src::Path(p) => std::fs::metadata(p).unwrap().len(),
                            }
                    })
                    .sum::<u64>();
            assert_eq!(total, ZIP_LIMIT);
        }
    }
    assert!(rule.wants(name, max + 1, None, true));
    let _ = std::fs::remove_dir_all(&d);
}
