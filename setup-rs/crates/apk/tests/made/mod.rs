//! Made-up Android builds for the tests (no game or Unity files): binary and protobuf manifests,
//! APKs with the players' libraries and the data's first file, OBBs. Used by tests/apk.rs and by
//! pmu's tests of the apk route's setup (pmu/tests/setup_templates.rs).
#![allow(dead_code)]
use std::collections::HashMap;
use std::io::Cursor;
use std::path::{Path, PathBuf};

use apk::zipw::{align_extra, ZipOut};
use pkgzip::zip::{DEFLATED, STORED};

pub type Chunk = dyn Fn(u16, u16, &[u8]) -> Vec<u8>;

pub const NS: &str = "http://schemas.android.com/apk/res/android";
pub const BID: &str = "0123abcd-0000-1111-2222-333344445555";

pub fn res_id(k: &str) -> Option<u32> {
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
pub enum V {
    S(String),
    I(i64),
    B(bool),
}

#[derive(Clone)]
pub struct El(pub String, pub Vec<(String, V)>, pub Vec<El>);

pub fn s(x: &str) -> V {
    V::S(x.into())
}

/// The binary XML aapt writes, reduced to what the parser reads (test_apk.py axml).
pub fn axml(tree: &El, utf8: bool) -> Vec<u8> {
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

pub fn varint(mut n: u64) -> Vec<u8> {
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

pub fn pf(num: u64, payload: &[u8]) -> Vec<u8> {
    let mut v = varint((num << 3) | 2);
    v.extend(varint(payload.len() as u64));
    v.extend(payload);
    v
}

pub fn pi(num: u64, x: u64) -> Vec<u8> {
    let mut v = varint(num << 3);
    v.extend(varint(x));
    v
}

/// An App Bundle's protobuf XmlNode (test_apk.py proto_xml).
pub fn proto_xml(t: &El) -> Vec<u8> {
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

pub fn manifest(pkg: &str, code: i64, split: Option<&str>, meta: &[(&str, V)]) -> El {
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

pub fn sf_head(version: &str) -> Vec<u8> {
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
pub fn zip(path: &Path, entries: &[(&str, Vec<u8>, u16)]) -> PathBuf {
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

pub fn make_apk(
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

pub fn obb(path: &Path, build_id: &str) -> PathBuf {
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

