//! `apk` on made-up builds (convert/tests/test_apk.py's, written here): binary and protobuf
//! manifests, ABI choice, OBBs, splits and containers, an App Bundle, refusals; the zips it writes
//! read back (aligned stored data, deflate as zlib's). tests/golden/apk_golden.py compares the
//! whole command with the Python on these and on real builds.
use std::collections::HashMap;
use std::fs;
use std::io::Cursor;
use std::path::{Path, PathBuf};

use apk::json::Json;
use apk::zipw::{align_extra, ZipOut};
use apk::{Fail, Opts};
use pkgzip::zip::{Archive, DEFLATED, STORED};

type Chunk = dyn Fn(u16, u16, &[u8]) -> Vec<u8>;

const NS: &str = "http://schemas.android.com/apk/res/android";
const BID: &str = "0123abcd-0000-1111-2222-333344445555";

fn res_id(k: &str) -> Option<u32> {
    Some(match k {
        "versionCode" => 0x0101021b,
        "versionName" => 0x0101021c,
        "minSdkVersion" => 0x0101020c,
        "targetSdkVersion" => 0x01010270,
        "name" => 0x01010003,
        "value" => 0x01010024,
        "glEsVersion" => 0x01010281,
        _ => return None,
    })
}

#[derive(Clone)]
enum V {
    S(String),
    I(i64),
    B(bool),
}

#[derive(Clone)]
struct El(String, Vec<(String, V)>, Vec<El>);

fn s(x: &str) -> V {
    V::S(x.into())
}

/// The binary XML aapt writes, reduced to what the parser reads (test_apk.py axml).
fn axml(tree: &El, utf8: bool) -> Vec<u8> {
    let mut strings: Vec<String> = Vec::new();
    let mut idx: HashMap<String, usize> = HashMap::new();
    let mut names: Vec<String> = Vec::new();
    fn walk(t: &El, names: &mut Vec<String>) {
        for (k, _) in &t.1 {
            if res_id(k).is_some() && !names.contains(k) {
                names.push(k.clone());
            }
        }
        for c in &t.2 {
            walk(c, names);
        }
    }
    walk(tree, &mut names);
    fn intern(x: &str, strings: &mut Vec<String>, idx: &mut HashMap<String, usize>) -> u32 {
        if let Some(&i) = idx.get(x) {
            return i as u32;
        }
        idx.insert(x.to_string(), strings.len());
        strings.push(x.to_string());
        (strings.len() - 1) as u32
    }
    for k in &names {
        intern(k, &mut strings, &mut idx);
    }
    let resmap: Vec<u32> = names.iter().map(|k| res_id(k).unwrap()).collect();
    intern(NS, &mut strings, &mut idx);
    intern("android", &mut strings, &mut idx);
    let chunk = |ctype: u16, hsize: u16, payload: &[u8]| -> Vec<u8> {
        let mut v = Vec::new();
        v.extend(ctype.to_le_bytes());
        v.extend(hsize.to_le_bytes());
        v.extend((8 + payload.len() as u32).to_le_bytes());
        v.extend(payload);
        v
    };
    let mut body = Vec::new();
    fn element(
        t: &El,
        names: &[String],
        strings: &mut Vec<String>,
        idx: &mut HashMap<String, usize>,
        body: &mut Vec<Vec<u8>>,
        chunk: &Chunk,
    ) {
        let mut ab = Vec::new();
        for (k, v) in &t.1 {
            let ns = if k == "package" || k == "split" {
                0xffff_ffff
            } else {
                intern(NS, strings, idx)
            };
            let name = match res_id(k) {
                Some(_) => names.iter().position(|n| n == k).unwrap() as u32,
                None => intern(k, strings, idx),
            };
            let (raw, dt, data) = match v {
                V::B(b) => (0xffff_ffffu32, 0x12u8, if *b { 0xffff_ffff } else { 0 }),
                V::I(i) => (0xffff_ffff, 0x10, *i as u32),
                V::S(x) => {
                    let r = intern(x, strings, idx);
                    (r, 0x03, r)
                }
            };
            ab.extend(ns.to_le_bytes());
            ab.extend(name.to_le_bytes());
            ab.extend(raw.to_le_bytes());
            ab.extend(8u16.to_le_bytes());
            ab.push(0);
            ab.push(dt);
            ab.extend(data.to_le_bytes());
        }
        let tag = intern(&t.0, strings, idx);
        let mut p = Vec::new();
        p.extend(1u32.to_le_bytes());
        p.extend(0xffff_ffffu32.to_le_bytes());
        p.extend(0xffff_ffffu32.to_le_bytes());
        p.extend(tag.to_le_bytes());
        for x in [20u16, 20, t.1.len() as u16, 0, 0, 0] {
            p.extend(x.to_le_bytes());
        }
        p.extend(ab);
        body.push(chunk(0x0102, 16, &p));
        for c in &t.2 {
            element(c, names, strings, idx, body, chunk);
        }
        let mut e = Vec::new();
        for x in [1u32, 0xffff_ffff, 0xffff_ffff, tag] {
            e.extend(x.to_le_bytes());
        }
        body.push(chunk(0x0103, 16, &e));
    }
    element(tree, &names, &mut strings, &mut idx, &mut body, &chunk);
    let mut data = Vec::new();
    let mut offs = Vec::new();
    for x in &strings {
        offs.push(data.len() as u32);
        if utf8 {
            let e = x.as_bytes();
            data.push(x.chars().count() as u8);
            data.push(e.len() as u8);
            data.extend(e);
            data.push(0);
        } else {
            let u: Vec<u16> = x.encode_utf16().collect();
            data.extend((u.len() as u16).to_le_bytes());
            for w in u {
                data.extend(w.to_le_bytes());
            }
            data.extend([0, 0]);
        }
    }
    while data.len() % 4 != 0 {
        data.push(0);
    }
    let n = strings.len() as u32;
    let mut pool = Vec::new();
    pool.extend(1u16.to_le_bytes());
    pool.extend(28u16.to_le_bytes());
    pool.extend((28 + 4 * n + data.len() as u32).to_le_bytes());
    for x in [n, 0, if utf8 { 0x100 } else { 0 }, 28 + 4 * n, 0] {
        pool.extend(x.to_le_bytes());
    }
    for o in offs {
        pool.extend(o.to_le_bytes());
    }
    pool.extend(data);
    let mut rm = Vec::new();
    for r in resmap {
        rm.extend(r.to_le_bytes());
    }
    let mut inner = pool;
    inner.extend(chunk(0x0180, 8, &rm));
    for b in body {
        inner.extend(b);
    }
    let mut out = Vec::new();
    out.extend(3u16.to_le_bytes());
    out.extend(8u16.to_le_bytes());
    out.extend((8 + inner.len() as u32).to_le_bytes());
    out.extend(inner);
    out
}

fn varint(mut n: u64) -> Vec<u8> {
    let mut out = Vec::new();
    loop {
        let b = (n & 0x7f) as u8;
        n >>= 7;
        out.push(b | if n != 0 { 0x80 } else { 0 });
        if n == 0 {
            return out;
        }
    }
}

fn pf(num: u64, payload: &[u8]) -> Vec<u8> {
    let mut v = varint((num << 3) | 2);
    v.extend(varint(payload.len() as u64));
    v.extend(payload);
    v
}

fn pi(num: u64, x: u64) -> Vec<u8> {
    let mut v = varint(num << 3);
    v.extend(varint(x));
    v
}

/// An App Bundle's protobuf XmlNode (test_apk.py proto_xml).
fn proto_xml(t: &El) -> Vec<u8> {
    fn el(t: &El) -> Vec<u8> {
        let mut b = pf(3, t.0.as_bytes());
        for (k, v) in &t.1 {
            let mut a = pf(2, k.as_bytes());
            match v {
                V::B(x) => a.extend(pf(6, &pf(7, &pi(8, *x as u64)))),
                V::I(x) => {
                    a.extend(pf(3, x.to_string().as_bytes()));
                    a.extend(pf(6, &pf(7, &pi(6, *x as u64))));
                }
                V::S(x) => a.extend(pf(3, x.as_bytes())),
            }
            if let Some(r) = res_id(k) {
                a.extend(pi(5, r as u64));
            }
            b.extend(pf(4, &a));
        }
        for c in &t.2 {
            b.extend(pf(5, &pf(1, &el(c))));
        }
        b
    }
    pf(1, &el(t))
}

fn manifest(pkg: &str, code: i64, split: Option<&str>, meta: &[(&str, V)]) -> El {
    let mut attrs = vec![
        ("versionCode".into(), V::I(code)),
        ("versionName".into(), s("1.2")),
        ("package".into(), s(pkg)),
    ];
    if let Some(sp) = split {
        attrs.push(("split".into(), s(sp)));
    }
    let mut app = vec![];
    for (k, v) in meta {
        app.push(El(
            "meta-data".into(),
            vec![("name".into(), s(k)), ("value".into(), v.clone())],
            vec![],
        ));
    }
    app.push(El(
        "activity".into(),
        vec![("name".into(), s("com.unity3d.player.UnityPlayerActivity"))],
        vec![],
    ));
    El(
        "manifest".into(),
        attrs,
        vec![
            El(
                "uses-sdk".into(),
                vec![
                    ("minSdkVersion".into(), V::I(22)),
                    ("targetSdkVersion".into(), V::I(30)),
                ],
                vec![],
            ),
            El("application".into(), vec![], app),
        ],
    )
}

fn sf_head(version: &str) -> Vec<u8> {
    let mut h = Vec::new();
    for x in [0u32, 0, 22, 0] {
        h.extend(x.to_be_bytes());
    }
    h.extend([0u8; 4]);
    h.extend([0u8; 28]);
    h.extend(version.as_bytes());
    h.push(0);
    h.extend([0u8; 64]);
    h
}

/// A zip of (name, bytes, method) written with this crate's writer.
fn zip(path: &Path, entries: &[(&str, Vec<u8>, u16)]) -> PathBuf {
    let mut z = ZipOut::create(path.to_str().unwrap()).unwrap();
    for (n, b, m) in entries {
        let extra = if *m == STORED {
            align_extra(z.tell(), n)
        } else {
            vec![]
        };
        z.add(
            n,
            0,
            0x5021,
            *m,
            0o644 << 16,
            extra,
            &mut Cursor::new(b.clone()),
        )
        .unwrap();
    }
    z.finish().unwrap();
    path.to_path_buf()
}

fn make_apk(
    path: &Path,
    mf: &El,
    abis: &[&str],
    backend: Option<&str>,
    version: &str,
    data: bool,
) -> PathBuf {
    let mut e: Vec<(String, Vec<u8>, u16)> =
        vec![("AndroidManifest.xml".into(), axml(mf, false), DEFLATED)];
    for abi in abis {
        let mut libs = vec!["libmain.so", "libunity.so"];
        match backend {
            Some("il2cpp") => libs.push("libil2cpp.so"),
            Some("mono") => libs.push("libmonobdwgc-2.0.so"),
            _ => {}
        }
        for n in libs {
            let mut b = b"\x7fELF".to_vec();
            b.extend(abi.as_bytes());
            e.push((format!("lib/{abi}/{n}"), b, DEFLATED));
        }
    }
    if data {
        e.push((
            "assets/bin/Data/globalgamemanagers".into(),
            sf_head(version),
            STORED,
        ));
        if backend == Some("il2cpp") {
            let mut m = 0xFAB1_1BAFu32.to_le_bytes().to_vec();
            m.extend(29u32.to_le_bytes());
            e.push((
                "assets/bin/Data/Managed/Metadata/global-metadata.dat".into(),
                m,
                STORED,
            ));
        }
        if backend == Some("mono") {
            e.push((
                "assets/bin/Data/Managed/etc/mono/config".into(),
                b"<configuration/>".to_vec(),
                STORED,
            ));
        }
    }
    let refs: Vec<(&str, Vec<u8>, u16)> = e
        .iter()
        .map(|(n, b, m)| (n.as_str(), b.clone(), *m))
        .collect();
    zip(path, &refs)
}

fn obb(path: &Path, build_id: &str) -> PathBuf {
    zip(
        path,
        &[
            (&format!("assets/{build_id}"), vec![], STORED),
            (
                "assets/bin/Data/sharedassets0.assets",
                b"x".to_vec(),
                STORED,
            ),
        ],
    )
}

struct Dir(PathBuf);

impl Dir {
    fn new(name: &str) -> Dir {
        let base = std::env::var("PMU_SCRATCH")
            .ok()
            .filter(|s| !s.is_empty() && Path::new(s).is_dir())
            .map(PathBuf::from)
            .unwrap_or_else(std::env::temp_dir);
        let d = base.join(format!("apk-test-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        Dir(d)
    }
    fn p(&self, n: &str) -> PathBuf {
        self.0.join(n)
    }
}

impl Drop for Dir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn run(inputs: &[&Path], out: &Path, target: &str) -> Result<Json, Fail> {
    let o = Opts {
        target: target.into(),
        ..Default::default()
    };
    let ins: Vec<String> = inputs
        .iter()
        .map(|p| p.to_string_lossy().into_owned())
        .collect();
    apk::run(&ins, out.to_str().unwrap(), &o, &mut |_| {})
}

fn refused(r: Result<Json, Fail>, text: &str) {
    match r {
        Err(Fail::Refused(m)) => assert!(m.contains(text), "{m}"),
        other => panic!("not refused ({text}): {other:?}"),
    }
}

fn get<'a>(j: &'a Json, k: &str) -> &'a Json {
    j.get(k).unwrap_or_else(|| panic!("no {k}"))
}

#[test]
fn il2cpp_arm64_and_armhf() {
    let d = Dir::new("il2cpp");
    let mf = manifest(
        "com.test.game",
        7,
        None,
        &[
            ("unity.splash-mode", V::I(0)),
            ("unity.splash-enable", V::B(true)),
            ("unity.build-id", s(BID)),
        ],
    );
    let p = make_apk(
        &d.p("g.apk"),
        &mf,
        &["arm64-v8a", "armeabi-v7a"],
        Some("il2cpp"),
        "2021.3.4f1",
        true,
    );
    let out = d.p("out");
    let r = run(&[&p], &out, "arm64").unwrap();
    assert_eq!(get(&r, "abi"), &Json::str("arm64-v8a"));
    assert_eq!(get(&r, "il2cpp_metadata"), &Json::Int(29));
    assert_eq!(get(&r, "build_id"), &Json::str(BID));
    assert_eq!(
        fs::read(out.join("lib/libunity.so")).unwrap(),
        b"\x7fELFarm64-v8a"
    );
    assert_eq!(
        fs::read(out.join("base.apk")).unwrap(),
        fs::read(&p).unwrap()
    );
    let pj = fs::read_to_string(out.join("android_package.json")).unwrap();
    assert!(
        pj.starts_with("{\n \"package\": \"com.test.game\",\n \"version_code\": 7,"),
        "{pj}"
    );
    assert!(!fs::read_dir(&out).unwrap().any(|e| e
        .unwrap()
        .file_name()
        .to_string_lossy()
        .starts_with(".apk-")));
    let r = run(&[&p], &out, "armhf").unwrap();
    assert_eq!(get(&r, "abi"), &Json::str("armeabi-v7a"));
    assert_eq!(
        fs::read(out.join("lib/libunity.so")).unwrap(),
        b"\x7fELFarmeabi-v7a"
    );
    // the list's first player the build has
    assert_eq!(
        get(&run(&[&p], &out, "armhf,arm64").unwrap(), "abi"),
        &Json::str("armeabi-v7a")
    );
    refused(run(&[&p], &out, "x86"), "--target x86");
}

#[test]
fn mono_and_refusals() {
    let d = Dir::new("mono");
    let mf = manifest("com.test.game", 7, None, &[]);
    let m = make_apk(
        &d.p("m.apk"),
        &mf,
        &["armeabi-v7a", "x86"],
        Some("mono"),
        "2019.4.18f1",
        true,
    );
    let out = d.p("out");
    refused(run(&[&m], &out, "arm64"), "needs a 32-bit (armhf) userland");
    let r = run(&[&m], &out, "armhf").unwrap();
    assert_eq!(get(&r, "backend"), &Json::str("mono"));
    assert!(out
        .join("assets/bin/Data/Managed/etc/mono/config")
        .is_file());
    // --il2cpp on a 64-bit target: the compile is the Python's
    let o = Opts {
        target: "arm64".into(),
        il2cpp: true,
        ..Default::default()
    };
    let r = apk::run(
        &[m.to_string_lossy().into_owned()],
        out.to_str().unwrap(),
        &o,
        &mut |_| {},
    );
    assert!(matches!(r, Err(Fail::NotPorted(_))), "{r:?}");
    for (name, abis, backend, version, data, text) in [
        (
            "x86",
            vec!["x86"],
            Some("il2cpp"),
            "2021.3.4f1",
            true,
            "only for x86",
        ),
        (
            "nolib",
            vec![],
            Some("il2cpp"),
            "2021.3.4f1",
            true,
            "no native libraries",
        ),
        (
            "noscript",
            vec!["arm64-v8a"],
            None,
            "2021.3.4f1",
            true,
            "neither libil2cpp.so",
        ),
        (
            "old",
            vec!["arm64-v8a"],
            Some("il2cpp"),
            "5.5.4f1",
            true,
            "Unity 5.5.4f1",
        ),
        (
            "nodata",
            vec!["arm64-v8a"],
            Some("il2cpp"),
            "2021.3.4f1",
            false,
            "give the .obb",
        ),
    ] {
        let a = make_apk(
            &d.p(&format!("{name}.apk")),
            &mf,
            &abis,
            backend,
            version,
            data,
        );
        refused(run(&[&a], &d.p(&format!("o-{name}")), "arm64"), text);
    }
    let junk = d.p("junk.apk");
    fs::write(&junk, b"not a zip").unwrap();
    refused(run(&[&junk], &out, "arm64"), "not a zip archive");
    refused(run(&[&d.p("nope.apk")], &out, "arm64"), "no such file");
}

#[test]
fn obbs() {
    let d = Dir::new("obbs");
    let b = d.p("build");
    fs::create_dir_all(&b).unwrap();
    let mf = manifest("com.test.game", 12, None, &[("unity.build-id", s(BID))]);
    let g = make_apk(
        &b.join("Game.apk"),
        &mf,
        &["arm64-v8a"],
        Some("il2cpp"),
        "2021.3.4f1",
        true,
    );
    obb(&b.join("Game.main.obb"), BID);
    let out = d.p("out");
    let r = run(&[&g], &out, "arm64").unwrap();
    assert_eq!(get(&r, "main_obb"), &Json::str("main.12.com.test.game.obb"));
    assert!(out.join("main.12.com.test.game.obb").is_file());
    fs::create_dir_all(d.p("o")).unwrap();
    let other = obb(
        &d.p("o/main.12.com.test.game.obb"),
        &BID.replace("0123", "9999"),
    );
    refused(
        run(&[&g, &other], &out, "arm64"),
        "belongs to another build",
    );
    let wrong = obb(&d.p("o/main.12.com.other.obb"), BID);
    refused(run(&[&g, &wrong], &out, "arm64"), "an OBB of com.other");
    let m = obb(&d.p("o/main.11.com.test.game.obb"), BID);
    let q = obb(&d.p("o/x.patch.obb"), BID);
    let r = run(&[&g, &m, &q], &out, "arm64").unwrap();
    assert_eq!(
        (get(&r, "main_obb"), get(&r, "patch_obb")),
        (
            &Json::str("main.12.com.test.game.obb"),
            &Json::str("patch.12.com.test.game.obb")
        )
    );
}

#[test]
fn splits_containers_and_merged_asset_packs() {
    let d = Dir::new("splits");
    let mf = manifest("com.test.game", 7, None, &[("unity.build-id", s(BID))]);
    let base = make_apk(
        &d.p("base.apk"),
        &mf,
        &[],
        Some("il2cpp"),
        "2021.3.4f1",
        true,
    );
    let cfg = zip(
        &d.p("config.apk"),
        &[
            (
                "AndroidManifest.xml",
                axml(
                    &manifest("com.test.game", 7, Some("config.arm64_v8a"), &[]),
                    true,
                ),
                DEFLATED,
            ),
            ("lib/arm64-v8a/libmain.so", b"split".to_vec(), STORED),
            ("lib/arm64-v8a/libunity.so", b"split".to_vec(), STORED),
            ("lib/arm64-v8a/libil2cpp.so", b"split".to_vec(), DEFLATED),
        ],
    );
    let o = obb(&d.p("o.obb"), BID);
    let x = zip(
        &d.p("game.xapk"),
        &[
            ("com.test.game.apk", fs::read(&base).unwrap(), DEFLATED),
            ("config.arm64_v8a.apk", fs::read(&cfg).unwrap(), STORED),
            (
                "Android/obb/com.test.game/main.7.com.test.game.obb",
                fs::read(&o).unwrap(),
                STORED,
            ),
            ("manifest.json", b"{}".to_vec(), DEFLATED),
        ],
    );
    let out = d.p("out");
    let r = run(&[&x], &out, "arm64").unwrap();
    assert_eq!(get(&r, "main_obb"), &Json::str("main.7.com.test.game.obb"));
    assert_eq!(fs::read(out.join("lib/libil2cpp.so")).unwrap(), b"split");
    refused(run(&[&base], &out, "arm64"), "split APK");
    // an asset pack split: merged into base.apk, its stored data aligned
    let base2 = make_apk(
        &d.p("base2.apk"),
        &mf,
        &["arm64-v8a"],
        Some("il2cpp"),
        "2021.3.4f1",
        false,
    );
    let big: Vec<u8> = (0..300_000u32).map(|i| (i % 251) as u8).collect();
    let pack = zip(
        &d.p("pack.apk"),
        &[
            (
                "AndroidManifest.xml",
                axml(
                    &manifest("com.test.game", 7, Some("UnityDataAssetPack"), &[]),
                    false,
                ),
                DEFLATED,
            ),
            (
                "assets/bin/Data/globalgamemanagers",
                sf_head("2022.3.10f1"),
                STORED,
            ),
            (
                "assets/bin/Data/Managed/Metadata/global-metadata.dat",
                [0xFAB1_1BAFu32.to_le_bytes(), 31u32.to_le_bytes()].concat(),
                STORED,
            ),
            ("assets/bin/Data/data.unity3d", big.clone(), DEFLATED),
            ("assets/x", b"odd".to_vec(), STORED),
        ],
    );
    let r = run(&[&base2, &pack], &out, "arm64").unwrap();
    assert_eq!(get(&r, "unity_version"), &Json::str("2022.3.10f1"));
    assert_eq!(get(&r, "il2cpp_metadata"), &Json::Int(31));
    let z = Archive::open(out.join("base.apk").to_str().unwrap()).unwrap();
    assert_eq!(z.read("assets/bin/Data/data.unity3d").unwrap(), big);
    for e in &z.entries {
        if e.method == STORED {
            assert_eq!(z.data_offset(e).unwrap() % 4, 0, "{}", e.name);
        }
    }
}

#[test]
fn app_bundle() {
    let d = Dir::new("aab");
    let mf = manifest("com.test.game", 7, None, &[("unity.build-id", s(BID))]);
    let pack = El(
        "manifest".into(),
        vec![
            ("package".into(), s("com.test.game")),
            ("split".into(), s("UnityDataAssetPack")),
        ],
        vec![El(
            "dist:module".into(),
            vec![],
            vec![El(
                "dist:delivery".into(),
                vec![],
                vec![El("dist:install-time".into(), vec![], vec![])],
            )],
        )],
    );
    let aab = zip(
        &d.p("game.aab"),
        &[
            ("BundleConfig.pb", vec![], STORED),
            (
                "base/manifest/AndroidManifest.xml",
                proto_xml(&mf),
                DEFLATED,
            ),
            ("base/lib/arm64-v8a/libmain.so", b"aab".repeat(50), DEFLATED),
            ("base/lib/arm64-v8a/libunity.so", b"aab".to_vec(), STORED),
            ("base/lib/arm64-v8a/libil2cpp.so", b"aab".to_vec(), STORED),
            ("base/dex/classes.dex", b"dex".to_vec(), STORED),
            ("base/root/unity_extra", b"r".to_vec(), STORED),
            (
                "base/assets/bin/Data/Managed/Metadata/global-metadata.dat",
                [0xFAB1_1BAFu32.to_le_bytes(), 29u32.to_le_bytes()].concat(),
                STORED,
            ),
            (
                "UnityDataAssetPack/manifest/AndroidManifest.xml",
                proto_xml(&pack),
                STORED,
            ),
            (
                "UnityDataAssetPack/assets/bin/Data/globalgamemanagers",
                sf_head("2021.3.20f1"),
                DEFLATED,
            ),
        ],
    );
    let out = d.p("out");
    let r = run(&[&aab], &out, "arm64").unwrap();
    assert_eq!(get(&r, "unity_version"), &Json::str("2021.3.20f1"));
    assert_eq!(get(&r, "build_id"), &Json::str(BID));
    let z = Archive::open(out.join("base.apk").to_str().unwrap()).unwrap();
    let names: Vec<&str> = z.names().collect();
    for n in [
        "AndroidManifest.xml",
        "unity_extra",
        "lib/arm64-v8a/libunity.so",
        "assets/bin/Data/globalgamemanagers",
    ] {
        assert!(names.contains(&n), "{n}: {names:?}");
    }
    assert!(!names.contains(&"dex/classes.dex"));
    assert_eq!(
        z.read("lib/arm64-v8a/libmain.so").unwrap(),
        b"aab".repeat(50)
    );
}

#[test]
fn deflate_is_zlibs() {
    // (Python: zlib.compressobj(-1, 8, -15) of the same bytes)
    let d = Dir::new("deflate");
    let data = b"hello hello hello hello".to_vec();
    let p = zip(&d.p("z.zip"), &[("a", data.clone(), DEFLATED)]);
    let z = Archive::open(p.to_str().unwrap()).unwrap();
    let e = z.info("a").unwrap();
    let raw = fs::read(&p).unwrap();
    let off = z.data_offset(e).unwrap() as usize;
    assert_eq!(
        &raw[off..off + e.compress_size as usize],
        &[0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x27, 0x01]
    );
    assert_eq!(z.read("a").unwrap(), data);
    assert_eq!(
        (
            e.create_version,
            e.create_system,
            e.extract_version,
            e.external_attr
        ),
        (20, 3, 20, 0o644 << 16)
    );
}
