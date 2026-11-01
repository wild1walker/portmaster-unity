//! `pmu player donor`: Unity's Android player package (the layout `android-data --pkg` reads) out
//! of donor APKs, a Unity game's own Android build of the same version (player.py `donor`):
//!   Variations/<backend>/Release/Libs/<abi>/libmain.so, libunity.so
//!   Variations/mono/Release/MonoLibs/<abi>/libmono*.so        (mono)
//!   Variations/<backend>/Managed/UnityEngine*.dll              (mono, or il2cpp with --managed)
//!   Data/unity default resources
//!   player.json
use std::fs::{self, File};
use std::io::{Read, Write};

use crate::py::{self, Json};
use crate::zip::{copy_entry, Archive};
use crate::{io_at, Error, Result};

/// assets/bin/Data/Managed/UnityEngine(\.\w+Module)?\.dll: the engine's assemblies (the class
/// libraries are the game's).
fn engine_dll(n: &str) -> bool {
    let Some(rest) = n.strip_prefix("assets/bin/Data/Managed/UnityEngine") else {
        return false;
    };
    let Some(mid) = rest.strip_suffix(".dll") else {
        return false;
    };
    if mid.is_empty() {
        return true;
    }
    // \.\w+Module: a dot, then word characters ending in "Module" (\w is Unicode in Python)
    let Some(m) = mid.strip_prefix('.') else {
        return false;
    };
    m.len() > "Module".len()
        && m.ends_with("Module")
        && m.chars().all(|c| c.is_alphanumeric() || c == '_')
}

/// Whether a stream holds `needle` (bytes in v), read to its end (the whole entry is read, so a
/// CRC error stops it as z.read() would).
fn contains(r: &mut dyn Read, needle: &[u8]) -> Result<bool> {
    let mut buf = vec![0u8; 1 << 20];
    let keep = needle.len().saturating_sub(1);
    let mut carry: Vec<u8> = Vec::new();
    let mut found = needle.is_empty();
    loop {
        let n = r.read(&mut buf)?;
        if n == 0 {
            return Ok(found);
        }
        if !found {
            carry.extend_from_slice(&buf[..n]);
            found = py::find(&carry, needle).is_some();
            let cut = carry.len().saturating_sub(keep);
            carry.drain(..cut);
        }
    }
}

/// player.py `donor`: the player package layout in `out` from the donors' lib/ and assets/bin/Data/.
pub fn donor(
    version: &str,
    out: &str,
    apks: &[String],
    backend: &str,
    abi: &str,
    managed: bool,
    log: &mut dyn FnMut(&str),
) -> Result<()> {
    let mut got: Vec<String> = Vec::new();
    for apk in apks {
        let z = Archive::open(apk)?;
        let names: Vec<&str> = z.names().collect();
        let libunity = format!("lib/{abi}/libunity.so");
        let lib_abi = names.contains(&libunity.as_str());
        if lib_abi && !contains(&mut z.reader(z.info(&libunity)?)?, version.as_bytes())? {
            return Err(Error::Exit(format!(
                "{apk}: its player is not Unity {version}"
            )));
        }
        for n in names {
            let base = n.rsplit('/').next().unwrap_or(n);
            let rel = if lib_abi && (n == format!("lib/{abi}/libmain.so") || n == libunity) {
                format!("Variations/{backend}/Release/Libs/{abi}/{base}")
            } else if lib_abi && backend == "mono" && n.starts_with(&format!("lib/{abi}/libmono")) {
                format!("Variations/mono/Release/MonoLibs/{abi}/{base}")
            } else if engine_dll(n) && (backend == "mono" || managed) {
                format!("Variations/{backend}/Managed/{base}")
            } else if n == "assets/bin/Data/unity default resources" {
                "Data/unity default resources".to_string()
            } else {
                continue;
            };
            let dst = rel
                .split('/')
                .fold(out.to_string(), |p, part| py::join(&p, part));
            fs::create_dir_all(py::dirname(&dst)).map_err(io_at(py::dirname(&dst)))?;
            let part = format!("{dst}.part");
            let mut r = z.reader(z.info(n)?)?;
            let mut f = File::create(&part).map_err(io_at(&part))?;
            // (the Python writes z.read(n), whole: a CRC error leaves the .part empty)
            if let Err(e) = copy_entry(&mut r, &mut f) {
                let _ = f.set_len(0);
                return Err(e);
            }
            drop(f);
            fs::rename(&part, &dst).map_err(io_at(&dst))?;
            got.push(rel);
        }
    }
    let libs = format!("Variations/{backend}/Release/Libs/{abi}/");
    let mut need = vec![
        format!("{libs}libmain.so"),
        format!("{libs}libunity.so"),
        "Data/unity default resources".into(),
    ];
    if backend == "mono" || managed {
        need.push(format!("Variations/{backend}/Managed/UnityEngine.dll"));
    }
    if backend == "mono" {
        need.push(format!("Variations/mono/Release/MonoLibs/{abi}/libmono.so"));
    }
    let missing: Vec<&str> = need
        .iter()
        .filter(|n| !got.contains(n) && !got.iter().any(|g| g.starts_with(n.as_str())))
        .map(String::as_str)
        .collect();
    if !missing.is_empty() {
        return Err(Error::Exit(format!(
            "the donors have no {} for {backend}/{abi}",
            missing.join(", ")
        )));
    }
    let info = Json::Obj(vec![
        ("version".into(), Json::Str(version.into())),
        ("backend".into(), Json::Str(backend.into())),
        ("abi".into(), Json::Str(abi.into())),
        (
            "donors".into(),
            Json::List(
                apks.iter()
                    .map(|a| Json::Str(py::basename(a).into()))
                    .collect(),
            ),
        ),
        ("files".into(), Json::Int(got.len() as i64)),
    ]);
    let pj = py::join(out, "player.json");
    File::create(&pj)
        .map_err(io_at(&pj))?
        .write_all(py::json_dump(&info).as_bytes())?;
    log(&format!(
        "player {version} ({backend}, {abi}) from {} donor(s): {} files -> {out}",
        apks.len(),
        got.len()
    ));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::zipwrite::{write_zip, Src};

    #[test]
    fn engine_dlls() {
        assert!(engine_dll("assets/bin/Data/Managed/UnityEngine.dll"));
        assert!(engine_dll(
            "assets/bin/Data/Managed/UnityEngine.UIModule.dll"
        ));
        assert!(!engine_dll("assets/bin/Data/Managed/UnityEngine.UI.dll"));
        assert!(!engine_dll(
            "assets/bin/Data/Managed/UnityEngine.Module.dll"
        ));
        assert!(!engine_dll(
            "assets/bin/Data/Managed/UnityEngine.A.BModule.dll"
        ));
        assert!(!engine_dll("assets/bin/Data/Managed/mscorlib.dll"));
    }

    #[test]
    fn finds_across_pieces() {
        let mut data = vec![b'x'; (1 << 20) - 3];
        data.extend(b"2017.4.9f1");
        assert!(contains(&mut &data[..], b"2017.4.9f1").unwrap());
        assert!(!contains(&mut &data[..], b"2017.4.40f1").unwrap());
    }

    #[test]
    fn layout_from_made_up_donors() {
        let dir = std::env::temp_dir().join(format!("pkgzip-player-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let d = |n: &str| dir.join(n).to_string_lossy().into_owned();
        let b = |s: &[u8]| Src::Bytes(s.to_vec());
        let mono = vec![
            (
                "lib/armeabi-v7a/libunity.so".to_string(),
                b(b"...Unity 2017.4.9f1..."),
            ),
            ("lib/armeabi-v7a/libmain.so".to_string(), b(b"main")),
            ("lib/armeabi-v7a/libmonobdwgc-2.0.so".to_string(), b(b"m2")),
            ("lib/armeabi-v7a/libmono.so".to_string(), b(b"m")),
            (
                "assets/bin/Data/Managed/UnityEngine.dll".to_string(),
                b(b"ue"),
            ),
            (
                "assets/bin/Data/Managed/Assembly-CSharp.dll".to_string(),
                b(b"game"),
            ),
            (
                "assets/bin/Data/unity default resources".to_string(),
                b(b"res"),
            ),
        ];
        write_zip(&d("mono.apk"), &mono, &mut |_| {}, 4).unwrap();
        let mut said = String::new();
        let apks = [d("mono.apk")];
        donor(
            "2017.4.9f1",
            &d("out"),
            &apks,
            "mono",
            "armeabi-v7a",
            false,
            &mut |m| said = m.into(),
        )
        .unwrap();
        let got = |p: &str| fs::read(dir.join("out").join(p)).unwrap();
        assert_eq!(
            got("Variations/mono/Release/Libs/armeabi-v7a/libmain.so"),
            b"main"
        );
        assert_eq!(
            got("Variations/mono/Release/MonoLibs/armeabi-v7a/libmonobdwgc-2.0.so"),
            b"m2"
        );
        assert_eq!(got("Variations/mono/Managed/UnityEngine.dll"), b"ue");
        assert_eq!(got("Data/unity default resources"), b"res");
        assert!(!dir
            .join("out/Variations/mono/Managed/Assembly-CSharp.dll")
            .exists());
        let pj = String::from_utf8(got("player.json")).unwrap();
        assert!(
            pj.starts_with("{\n \"version\": \"2017.4.9f1\",\n \"backend\": \"mono\",")
                && pj.ends_with("\"files\": 6\n}")
        );
        assert_eq!(
            said,
            format!(
                "player 2017.4.9f1 (mono, armeabi-v7a) from 1 donor(s): 6 files -> {}",
                d("out")
            )
        );
        let e = donor(
            "2017.4.40f1",
            &d("out2"),
            &apks,
            "mono",
            "armeabi-v7a",
            false,
            &mut |_| {},
        )
        .unwrap_err();
        assert!(e
            .to_string()
            .ends_with("its player is not Unity 2017.4.40f1"));
        let e = donor(
            "2017.4.9f1",
            &d("out3"),
            &apks,
            "il2cpp",
            "arm64-v8a",
            true,
            &mut |_| {},
        )
        .unwrap_err();
        assert_eq!(
            e.to_string(),
            "the donors have no Variations/il2cpp/Release/Libs/arm64-v8a/libmain.so, \
             Variations/il2cpp/Release/Libs/arm64-v8a/libunity.so for il2cpp/arm64-v8a"
        );
        fs::remove_dir_all(&dir).unwrap();
    }
}
