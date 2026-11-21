//! pkgzip::arch (`pmu arch-check`): an aarch64-only release (port.aarch64_only, docs/arm64.md
//! "Shipping aarch64 only") made of synthetic ELF files and donor APKs: the release that passes has
//! no EM_ARM file anywhere (the donors' entries included) and every piece the mono-arm64 route
//! needs; each armhf piece put back, and each 64-bit piece taken away or built for the wrong
//! machine, fails it, by name. The same check on the release as a zip.
use std::fs;
use std::path::{Path, PathBuf};

use pkgzip::arch::{check_release, check_release_zip, scan, EM_AARCH64, EM_ARM};
use pkgzip::zipwrite::{write_zip, Src};

const V: &str = "6000.0.61f1";

/// A made-up ELF file: the header of a shared object for the machine (class 2 for aarch64, 1 for
/// 32-bit ARM), then some bytes.
fn elf(machine: u16) -> Vec<u8> {
    let mut h = vec![0u8; 64];
    h[..4].copy_from_slice(b"\x7fELF");
    h[4] = if machine == EM_AARCH64 { 2 } else { 1 };
    h[5] = 1; // little-endian
    h[6] = 1;
    h[16] = 3; // ET_DYN
    h[18..20].copy_from_slice(&machine.to_le_bytes());
    h.extend_from_slice(&[0x5a; 200]);
    h
}

fn put(root: &Path, rel: &str, data: &[u8]) {
    let p = root.join(rel);
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    fs::write(p, data).unwrap();
}

fn apk(root: &Path, rel: &str, entries: Vec<(&str, Vec<u8>)>) {
    let p = root.join(rel);
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    let e: Vec<(String, Src)> = entries
        .into_iter()
        .map(|(n, d)| (n.to_string(), Src::Bytes(d)))
        .collect();
    write_zip(&p.to_string_lossy(), &e, &mut |_| {}, 4).unwrap();
}

/// The Mono donor's engine assemblies (a managed DLL is no ELF file) and, unless slimmed, Unity's
/// 32-bit Mono player beside them, as in a donor APK as Unity builds it.
fn mono_donor(root: &Path, slim: bool) {
    let mut e = vec![
        ("AndroidManifest.xml", b"<manifest/>".to_vec()),
        (
            "assets/bin/Data/Managed/UnityEngine.dll",
            b"MZ\x90\0managed".to_vec(),
        ),
        (
            "assets/bin/Data/Managed/UnityEngine.CoreModule.dll",
            b"MZ\x90\0managed".to_vec(),
        ),
        (
            "assets/bin/Data/Managed/mscorlib.dll",
            b"MZ\x90\0managed".to_vec(),
        ),
        ("assets/bin/Data/unity default resources", vec![7u8; 300]),
    ];
    if !slim {
        e.push(("lib/armeabi-v7a/libmain.so", elf(EM_ARM)));
        e.push(("lib/armeabi-v7a/libunity.so", elf(EM_ARM)));
        e.push(("lib/armeabi-v7a/libmonobdwgc-2.0.so", elf(EM_ARM)));
    }
    apk(root, &format!("game/donor/donor-{V}-mono-armv7.apk"), e);
}

/// An aarch64-only mono-arm64 release, unpacked: what `port package` writes with
/// port.aarch64_only = true.
fn release(name: &str) -> PathBuf {
    let root = std::env::temp_dir().join(format!("pkgzip-arch-{name}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();
    put(&root, "Game.sh", b"#!/bin/bash\nexit 0\n");
    put(&root, "game/port.json", b"{\"version\": 4}\n");
    put(
        &root,
        "game/setup/setup.env",
        format!(
            "# Written by `pmunity port new`\nPORT_NAME=game\nPORT_TITLE='A '\"'\"'Game'\nPORT_ROUTE=mono-arm64\n\
             UNITY_VERSIONS={V}\nPLAYER_BACKEND=il2cpp\nPLAYER_ABI=arm64-v8a\n\
             DONORS='donor-{V}-il2cpp-arm64.apk donor-{V}-mono-armv7.apk'\n"
        )
        .as_bytes(),
    );
    put(&root, "game/setup/patchscript", b"#!/bin/bash\n");
    put(&root, "game/ua_host", &elf(EM_AARCH64));
    put(&root, "game/uaflipbook.dll", b"MZ\x90\0managed");
    put(&root, "game/setup/pmu", &elf(EM_AARCH64));
    put(&root, "game/setup/asmpatch", &elf(EM_AARCH64));
    put(
        &root,
        "game/setup/mono64/6000.0/libil2cpp.so",
        &elf(EM_AARCH64),
    );
    put(
        &root,
        "game/setup/mono64/6000.0/libmonobdwgc-2.0.so",
        &elf(EM_AARCH64),
    );
    put(
        &root,
        "game/setup/mono64/6000.0/libMonoPosixHelper.so",
        &elf(EM_AARCH64),
    );
    put(&root, "game/licenses/LICENSE.libhybris.txt", b"Apache 2.0");
    apk(
        &root,
        &format!("game/donor/donor-{V}-il2cpp-arm64.apk"),
        vec![
            ("lib/arm64-v8a/libmain.so", elf(EM_AARCH64)),
            ("lib/arm64-v8a/libunity.so", elf(EM_AARCH64)),
            ("lib/arm64-v8a/libil2cpp.so", elf(EM_AARCH64)),
            ("assets/bin/Data/unity default resources", vec![7u8; 300]),
        ],
    );
    mono_donor(&root, true);
    root
}

#[test]
fn aarch64_only_release_passes() {
    let root = release("pass");
    let r = check_release(&root).unwrap();
    assert_eq!(r.port, "game");
    assert_eq!(r.route, "mono-arm64");
    assert!(r.armhf.is_empty(), "{:?}", r.armhf);
    assert!(r.missing.is_empty(), "{:?}", r.missing);
    assert!(r.ok());
    // every ELF file is aarch64, the donor's player included
    assert!(r
        .elves
        .iter()
        .all(|e| e.machine == EM_AARCH64 && e.class == 2));
    let names: Vec<&str> = r.elves.iter().map(|e| e.path.as_str()).collect();
    assert!(names.contains(&"game/ua_host"));
    assert!(names.contains(
        &format!("game/donor/donor-{V}-il2cpp-arm64.apk!lib/arm64-v8a/libunity.so").as_str()
    ));
    assert_eq!(scan(&root).unwrap(), r.elves);
    fs::remove_dir_all(&root).unwrap();
}

#[test]
fn the_32bit_player_in_a_donor_fails() {
    let root = release("donor");
    mono_donor(&root, false);
    let r = check_release(&root).unwrap();
    assert!(!r.ok());
    assert!(r.missing.is_empty(), "{:?}", r.missing);
    let d = format!("game/donor/donor-{V}-mono-armv7.apk!lib/armeabi-v7a/");
    assert_eq!(
        r.armhf,
        ["libmain.so", "libmonobdwgc-2.0.so", "libunity.so"].map(|n| format!("{d}{n}"))
    );
    fs::remove_dir_all(&root).unwrap();
}

#[test]
fn any_armhf_file_fails() {
    let root = release("stray");
    put(&root, "game/pmhwdec/libpmhwdec.so.1", &elf(EM_ARM));
    put(&root, "game/ua_host32", &elf(EM_ARM));
    let r = check_release(&root).unwrap();
    assert_eq!(r.armhf, ["game/pmhwdec/libpmhwdec.so.1", "game/ua_host32"]);
    assert!(r.missing.is_empty());
    fs::remove_dir_all(&root).unwrap();
}

#[test]
fn the_64bit_route_pieces_are_needed() {
    // ua_host built for armhf: both an armhf file and a missing 64-bit runtime
    let root = release("pieces");
    put(&root, "game/ua_host", &elf(EM_ARM));
    fs::remove_file(root.join("game/setup/mono64/6000.0/libmonobdwgc-2.0.so")).unwrap();
    fs::remove_file(root.join(format!("game/donor/donor-{V}-il2cpp-arm64.apk"))).unwrap();
    let r = check_release(&root).unwrap();
    assert_eq!(r.armhf, ["game/ua_host"]);
    let m = r.missing.join("\n");
    assert!(
        m.contains("game/ua_host (the runtime) is armhf (32-bit), not aarch64"),
        "{m}"
    );
    assert!(
        m.contains("game/setup/mono64/6000.0/libmonobdwgc-2.0.so"),
        "{m}"
    );
    assert!(
        m.contains(&format!(
            "game/donor/donor-{V}-il2cpp-arm64.apk (Unity {V}'s arm64 player)"
        )),
        "{m}"
    );
    assert_eq!(r.missing.len(), 3, "{m}");
    // a Mono donor without the engine's assemblies, a 64-bit donor without libil2cpp.so
    let root2 = release("pieces2");
    apk(
        &root2,
        &format!("game/donor/donor-{V}-mono-armv7.apk"),
        vec![("AndroidManifest.xml", b"<m/>".to_vec())],
    );
    apk(
        &root2,
        &format!("game/donor/donor-{V}-il2cpp-arm64.apk"),
        vec![
            ("lib/arm64-v8a/libmain.so", elf(EM_AARCH64)),
            ("lib/arm64-v8a/libunity.so", elf(EM_AARCH64)),
        ],
    );
    let r = check_release(&root2).unwrap();
    let m = r.missing.join("\n");
    assert!(
        m.contains("UnityEngine.dll (the engine's assemblies)"),
        "{m}"
    );
    assert!(
        m.contains("!lib/arm64-v8a/libil2cpp.so (Unity's arm64 player)"),
        "{m}"
    );
    assert_eq!(r.missing.len(), 2, "{m}");
    fs::remove_dir_all(&root).unwrap();
    fs::remove_dir_all(&root2).unwrap();
}

#[test]
fn the_armhf_setup_is_not_aarch64_only() {
    // the android route's release: its setup runs Unity's 32-bit player
    let root = release("route");
    let env = root.join("game/setup/setup.env");
    let t = fs::read_to_string(&env)
        .unwrap()
        .replace("PORT_ROUTE=mono-arm64", "PORT_ROUTE=android")
        .replace("PLAYER_BACKEND=il2cpp", "PLAYER_BACKEND=mono")
        .replace("PLAYER_ABI=arm64-v8a", "PLAYER_ABI=armeabi-v7a");
    fs::write(&env, t).unwrap();
    let r = check_release(&root).unwrap();
    let m = r.missing.join("\n");
    assert!(m.contains("PORT_ROUTE=android"), "{m}");
    assert!(m.contains("setup.env says mono/armeabi-v7a"), "{m}");
    fs::remove_dir_all(&root).unwrap();
}

#[test]
fn a_release_zip() {
    let root = release("zip");
    let mut files = Vec::new();
    let mut todo = vec![root.clone()];
    while let Some(d) = todo.pop() {
        for e in fs::read_dir(&d).unwrap() {
            let p = e.unwrap().path();
            if p.is_dir() {
                todo.push(p);
            } else {
                files.push(p);
            }
        }
    }
    files.sort();
    let zip = std::env::temp_dir().join(format!("pkgzip-arch-zip-{}.zip", std::process::id()));
    let entries: Vec<(String, Src)> = files
        .iter()
        .map(|p| {
            (
                p.strip_prefix(&root)
                    .unwrap()
                    .to_string_lossy()
                    .into_owned(),
                Src::Path(p.to_string_lossy().into_owned()),
            )
        })
        .collect();
    write_zip(&zip.to_string_lossy(), &entries, &mut |_| {}, 4).unwrap();
    let tmp = std::env::temp_dir();
    let r = check_release_zip(&zip.to_string_lossy(), &tmp).unwrap();
    assert!(r.ok(), "{:?} {:?}", r.armhf, r.missing);
    // the same zip with the full Mono donor
    mono_donor(&root, false);
    let entries: Vec<(String, Src)> = files
        .iter()
        .map(|p| {
            (
                p.strip_prefix(&root)
                    .unwrap()
                    .to_string_lossy()
                    .into_owned(),
                Src::Path(p.to_string_lossy().into_owned()),
            )
        })
        .collect();
    write_zip(&zip.to_string_lossy(), &entries, &mut |_| {}, 4).unwrap();
    let r = check_release_zip(&zip.to_string_lossy(), &tmp).unwrap();
    assert_eq!(r.armhf.len(), 3);
    fs::remove_file(&zip).unwrap();
    fs::remove_dir_all(&root).unwrap();
}
