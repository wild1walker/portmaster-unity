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

// ---------------------------------------------------------------- fetched from Unity (player.py)

/// Unity's release API.
pub const API: &str = "https://services.api.unity.com/unity/editor/release/v1/releases";
/// The Android Build Support installer on Unity's CDN, by changeset and version.
pub fn cdn(rev: &str, ver: &str) -> String {
    format!(
        "https://download.unity3d.com/download_unity/{rev}/TargetSupportInstaller/UnitySetup-Android-Support-for-Editor-{ver}.exe"
    )
}

/// release(): Unity's release record of a version, or None if Unity does not list it.
pub fn release(version: &str) -> Result<Option<serde_json::Value>> {
    let q = format!("version={}&limit=1", crate::http::quote_plus(version));
    let body = crate::http::fetch(&format!("{API}?{q}"))?;
    let d: serde_json::Value = serde_json::from_slice(&body)
        .map_err(|e| Error::Fail(format!("json.decoder.JSONDecodeError: {e}")))?;
    let recs = d
        .get("results")
        .and_then(|r| r.as_array())
        .cloned()
        .unwrap_or_default();
    Ok(recs
        .into_iter()
        .find(|r| r.get("version").and_then(|v| v.as_str()) == Some(version)))
}

/// str(x) of a JSON value as the Python's str() gives it for the strings it compares.
fn jstr(v: Option<&serde_json::Value>) -> String {
    match v {
        Some(serde_json::Value::String(s)) => s.clone(),
        Some(serde_json::Value::Null) | None => "None".into(),
        Some(v) => v.to_string(),
    }
}

/// installer_url(): the record's Windows Android Build Support installer (its module URL, else
/// the CDN path from the changeset).
pub fn installer_url(rec: &serde_json::Value) -> Result<String> {
    let empty = Vec::new();
    for dl in rec
        .get("downloads")
        .and_then(|d| d.as_array())
        .unwrap_or(&empty)
    {
        if dl.get("platform").and_then(|p| p.as_str()) != Some("WINDOWS") {
            continue;
        }
        for m in dl
            .get("modules")
            .and_then(|m| m.as_array())
            .unwrap_or(&empty)
        {
            let url = m.get("url").map(|u| jstr(Some(u))).unwrap_or_default();
            if m.get("id").and_then(|i| i.as_str()) == Some("android") && url.ends_with(".exe") {
                return Ok(url);
            }
        }
    }
    match rec
        .get("shortRevision")
        .and_then(|r| r.as_str())
        .filter(|r| !r.is_empty())
    {
        Some(rev) => Ok(cdn(rev, &jstr(rec.get("version")))),
        None => Err(Error::Exit(format!(
            "Unity's release record of {} has no Android installer or changeset",
            jstr(rec.get("version"))
        ))),
    }
}

/// package_path(): an installer path relative to the AndroidPlayer folder (its first part is
/// the install folder's variable).
pub fn package_path(nsis_path: &str) -> Option<String> {
    let mut parts = nsis_path.split('/');
    let first = parts.next()?;
    first
        .starts_with('$')
        .then(|| parts.collect::<Vec<_>>().join("/"))
}

pub const DEFAULT_RESOURCES: [&str; 2] = [
    "Data/unity default resources",
    "Data/Resources/unity default resources",
];

/// The globs of the files fetched (player.wanted).
pub fn wanted(
    backend: &str,
    abi: &str,
    symbols: bool,
    symbols_only: bool,
    managed: bool,
) -> Vec<String> {
    let sym = vec![format!(
        "Variations/{backend}/Release/Symbols/{abi}/*.sym.so"
    )];
    if symbols_only {
        return sym;
    }
    let mut g: Vec<String> = DEFAULT_RESOURCES.iter().map(|s| s.to_string()).collect();
    g.push(format!("Variations/{backend}/Release/Libs/{abi}/*.so"));
    g.push("Apk/MemoryAdvice/*".into());
    if backend == "mono" {
        g.push("Variations/mono/Managed/*.dll".into());
        g.push(format!("Variations/mono/Release/MonoLibs/{abi}/*.so"));
    } else if managed {
        g.push(format!("Variations/{backend}/Managed/*.dll"));
    }
    if symbols {
        g.extend(sym);
    }
    g
}

/// fnmatch.fnmatchcase with `*`, `?` and `[...]` (`*` matches `/` too).
pub fn fnmatch(name: &str, pat: &str) -> bool {
    fn m(n: &[char], p: &[char]) -> bool {
        match p.first() {
            None => n.is_empty(),
            Some('*') => (0..=n.len()).any(|k| m(&n[k..], &p[1..])),
            Some('?') => !n.is_empty() && m(&n[1..], &p[1..]),
            Some('[') => {
                let Some(close) = p.iter().skip(2).position(|&c| c == ']').map(|i| i + 2) else {
                    return n.first() == Some(&'[') && m(&n[1..], &p[1..]);
                };
                let Some(&c) = n.first() else { return false };
                let set = &p[1..close];
                let (neg, set) = match set.first() {
                    Some('!') => (true, &set[1..]),
                    _ => (false, set),
                };
                let mut hit = false;
                let mut i = 0;
                while i < set.len() {
                    if i + 2 < set.len() && set[i + 1] == '-' {
                        hit |= set[i] <= c && c <= set[i + 2];
                        i += 3;
                    } else {
                        hit |= set[i] == c;
                        i += 1;
                    }
                }
                hit != neg && m(&n[1..], &p[close + 1..])
            }
            Some(&c) => n.first() == Some(&c) && m(&n[1..], &p[1..]),
        }
    }
    let n: Vec<char> = name.chars().collect();
    let p: Vec<char> = pat.chars().collect();
    m(&n, &p)
}

/// player.py `fetch`: the player files a port needs, from Unity's installer (a URL, a local
/// copy, else the one Unity's release API names), into `out` with player.json.
#[allow(clippy::too_many_arguments)]
pub fn fetch(
    version: &str,
    out: &str,
    backend: &str,
    abi: &str,
    installer: Option<&str>,
    symbols: bool,
    symbols_only: bool,
    managed: bool,
    log: &mut dyn FnMut(&str),
) -> Result<()> {
    let mut rec = None;
    let installer = match installer.filter(|i| !i.is_empty()) {
        Some(i) => i.to_string(),
        None => {
            let Some(r) = release(version)? else {
                return Err(Error::Exit(format!(
                    "Unity does not list version {version} (check `python -m pmunity version <data>`)"
                )));
            };
            let u = installer_url(&r)?;
            rec = Some(r);
            u
        }
    };
    log(&format!(
        "player {version} ({backend}, {abi}) from {installer}"
    ));
    let mut ns = crate::nsis::Nsis::open(&installer, Box::new(|m| eprintln!("{m}")))?;
    let globs = wanted(backend, abi, symbols, symbols_only, managed);
    let mut files = ns.files()?;
    files.sort();
    let mut got: Vec<String> = Vec::new();
    for (path, off, _) in files {
        let Some(rel) = package_path(&path) else {
            continue;
        };
        if !globs.iter().any(|g| fnmatch(&rel, g)) {
            continue;
        }
        if (rel.ends_with(".sym.so") || rel.ends_with(".dbg.so")) && !rel.contains("/Symbols/") {
            continue;
        }
        let data = ns.extract(off)?;
        let mut dst = out.to_string();
        for part in rel.split('/') {
            dst = py::join(&dst, part);
        }
        fs::create_dir_all(py::dirname(&dst)).map_err(io_at(py::dirname(&dst)))?;
        let part = format!("{dst}.part");
        fs::write(&part, &data).map_err(io_at(&part))?;
        fs::rename(&part, &dst).map_err(io_at(&dst))?;
        got.push(rel);
    }
    let libs = format!("Variations/{backend}/Release/Libs/{abi}/");
    let mut missing: Vec<String> = ["libmain.so", "libunity.so"]
        .iter()
        .filter(|n| !got.contains(&format!("{libs}{n}")))
        .map(|n| n.to_string())
        .collect();
    if !DEFAULT_RESOURCES.iter().any(|n| got.iter().any(|g| g == n)) {
        missing.push("Data/unity default resources".into());
    }
    if symbols_only {
        missing = if got.iter().any(|r| r.ends_with("/libunity.sym.so")) {
            Vec::new()
        } else {
            vec!["libunity.sym.so".into()]
        };
    }
    if !missing.is_empty() {
        return Err(Error::Exit(format!(
            "the installer has no {} for {backend}/{abi} (Unity ships arm64-v8a players only with IL2CPP, from 2017.4.16 / 2018.2)",
            missing.join(", ")
        )));
    }
    let changeset = match rec.as_ref().and_then(|r| r.get("shortRevision")) {
        Some(serde_json::Value::String(s)) => Json::Str(s.clone()),
        _ => Json::Null,
    };
    let mut info = vec![
        ("version".to_string(), Json::Str(version.into())),
        ("changeset".to_string(), changeset),
        ("backend".to_string(), Json::Str(backend.into())),
        ("abi".to_string(), Json::Str(abi.into())),
        ("installer".to_string(), Json::Str(installer.clone())),
        ("files".to_string(), Json::Int(got.len() as i64)),
    ];
    if symbols || symbols_only {
        let mut s: Vec<&String> = got.iter().filter(|r| r.contains("/Symbols/")).collect();
        s.sort();
        info.push((
            "symbols".into(),
            Json::List(s.into_iter().map(|x| Json::Str(x.clone())).collect()),
        ));
    }
    if !symbols_only {
        let p = py::join(out, "player.json");
        fs::write(&p, py::json_dump(&Json::Obj(info))).map_err(io_at(&p))?;
    }
    log(&format!(
        "{} files, {:.1} MB read ({} requests) -> {out}",
        got.len(),
        ns.s.fetched as f64 / 1e6,
        ns.s.requests
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
