//! apk.py's `--il2cpp` (a Mono build, armeabi-v7a only, for Unity's arm64 IL2CPP player): what
//! `il2cpp` reads of an Android build (il2cpp_stub), the build itself (il2cpp_build: crates/il2cpp,
//! natively on the handheld), and the APK without its 32-bit libraries and assemblies, with
//! IL2CPP's metadata (il2cpp_apk).
use crate::zipw::{self, ZipErr, ZipOut};
use crate::{refused, Fail};
use androiddata::pyjson::Json as PyJson;
use androiddata::uversion::Version;
use pkgzip::py;
use pkgzip::zip::{self as pz, Archive};
use std::fs;
use std::io::Read;

/// The work folder in <out> while the build runs (a rerun continues it).
pub const IL2CPP_WORK: &str = ".il2cpp";
pub const IL2CPP_PLAYER_LIBS: &str = "Variations/il2cpp/Release/Libs/arm64-v8a";
const GUID: &str = "assets/bin/Data/unity_app_guid";
/// zipfile.ZIP64_LIMIT, ZIP_FILECOUNT_LIMIT (base.apk is written without zip64)
const ZIP64_LIMIT: u64 = (1 << 31) - 1;
const FILECOUNT_LIMIT: usize = (1 << 16) - 1;

/// The `--il2cpp` options apk.py hands `pmunity il2cpp` ({home, pkg, jobs, on_device, toolchain,
/// link_xml, strip, keep_cpp}).
#[derive(Debug, Clone, Default)]
pub struct Il2cppOpts {
    pub home: Option<String>,
    pub pkg: Option<String>,
    pub jobs: Option<usize>,
    pub on_device: bool,
    pub toolchain: Option<String>,
    pub link_xml: Vec<String>,
    pub strip: Option<String>,
    pub keep_cpp: bool,
}

/// il2cpp_refusal: why a Mono build of `ver` cannot be compiled for the arm64 IL2CPP player.
pub fn il2cpp_refusal(ver: &Version) -> Option<String> {
    if ver.line() < (2017, 4) {
        return Some(format!(
            "Unity {ver}: the arm64 IL2CPP players start with 2017.4"
        ));
    }
    if let Some(r) = il2cpp::mix::refusal(ver) {
        return Some(r);
    }
    let first = il2cpp::tools::ARM64_2017.0;
    if ver.line() == (2017, 4) && ver.patch < first {
        return Some(format!(
            "Unity {ver} has no arm64 IL2CPP player (2017.4.{first}f1 and later have one); an Android build's data is \
             not moved to another release here"
        ));
    }
    None
}

/// ^assets/bin/Data/(Managed/[^/]+\.dll|boot\.config|ScriptingAssemblies\.json)$ -> the group.
fn stub_name(nm: &str) -> Option<&str> {
    let rest = nm.strip_prefix("assets/bin/Data/")?;
    let m = crate::py_end(rest);
    if m == "boot.config" || m == "ScriptingAssemblies.json" {
        return Some(m);
    }
    let dll = m.strip_prefix("Managed/")?;
    (dll.len() > 4 && dll.ends_with(".dll") && !dll.contains('/') && !dll.contains('\n'))
        .then_some(m)
}

/// il2cpp_stub: what `il2cpp` reads of a data folder, from an Android build (the assemblies,
/// boot.config, ScriptingAssemblies.json) into `dst`. Returns the assembly count.
pub fn il2cpp_stub(zips: &[&Archive], dst: &str) -> Result<usize, Fail> {
    let mut n = 0;
    for z in zips {
        let names: Vec<String> = z.names().map(str::to_string).collect();
        for nm in names {
            let Some(rel) = stub_name(&nm) else { continue };
            let to = py::join(dst, rel);
            if fs::symlink_metadata(&to).is_ok() {
                continue;
            }
            crate::makedirs(py::dirname(&to))?;
            crate::extract(z, &nm, &to)?;
            if nm.ends_with(".dll") {
                n += 1;
            }
        }
    }
    Ok(n)
}

fn il2cpp_fail(e: il2cpp::Error) -> Fail {
    match e {
        il2cpp::Error::Unsupported(m) => Fail::Refused(format!("IL2CPP: {m}")),
        il2cpp::Error::Exit(m) => Fail::Exit(m),
        il2cpp::Error::Fail(m) => Fail::Error(m),
        il2cpp::Error::NotPorted(m) => Fail::NotPorted(m),
    }
}

/// il2cpp_build: `il2cpp` on the build's assemblies into <out>/.il2cpp. Returns (il2cpp.json,
/// the IL2CPP player package).
#[allow(clippy::too_many_arguments)]
pub fn il2cpp_build(
    zips: &[&Archive],
    ver: &Version,
    out: &str,
    tmp: &str,
    opts: &Il2cppOpts,
    log: &mut dyn FnMut(&str),
    listed: Option<Vec<String>>,
) -> Result<(PyJson, String), Fail> {
    let stub = py::join(tmp, "il2cpp-data");
    let n = il2cpp_stub(zips, &stub)?;
    if n == 0 {
        return refused(
            "a Mono build without assemblies in assets/bin/Data/Managed/: nothing to compile",
        );
    }
    let home = crate::abspath(
        &opts
            .home
            .clone()
            .filter(|h| !h.is_empty())
            .unwrap_or_else(il2cpp::build::default_home),
    );
    let pkg = crate::abspath(
        &opts
            .pkg
            .clone()
            .filter(|p| !p.is_empty())
            .unwrap_or_else(|| py::join(&py::join(&home, &ver.text), "player-arm64-v8a")),
    );
    log(&format!(
        "IL2CPP: compiling the build's {n} assemblies for Unity {ver}'s arm64 IL2CPP player ({}; docs/il2cpp.md)",
        if opts.on_device {
            "on this machine"
        } else {
            "in Docker"
        }
    ));
    let mut b = il2cpp::BuildOpts::new(&stub, &py::join(out, IL2CPP_WORK));
    b.home = Some(home);
    b.pkg = Some(pkg.clone());
    b.jobs = opts.jobs;
    b.keep_cpp = opts.keep_cpp;
    b.strip = opts
        .strip
        .clone()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "low".into());
    b.on_device = opts.on_device;
    b.device_tc = opts.toolchain.clone();
    b.link_xml = opts.link_xml.iter().map(|x| crate::abspath(x)).collect();
    b.data_version = Some(ver.clone());
    b.listed = listed;
    let info = il2cpp::build(&b)
        .map_err(il2cpp_fail)?
        .ok_or_else(|| Fail::Error("TypeError: 'NoneType' object is not subscriptable".into()))?;
    let unity = info.get("unity").map(PyJson::py_str).unwrap_or_default();
    if unity != ver.text {
        return refused(format!(
            "IL2CPP built for Unity {unity}, the build is {ver}"
        ));
    }
    Ok((info, pkg))
}

/// ^META-INF/([^/]+\.(SF|RSA|DSA|EC)|MANIFEST\.MF)$, any case: the signature's files.
fn is_signature(nm: &str) -> bool {
    let m = crate::py_end(nm);
    let Some(rest) = m
        .get(..9)
        .filter(|p| p.eq_ignore_ascii_case("META-INF/"))
        .map(|_| &m[9..])
    else {
        return false;
    };
    if rest.eq_ignore_ascii_case("MANIFEST.MF") {
        return true;
    }
    if rest.contains('/') || rest.contains('\n') {
        return false;
    }
    let Some((stem, ext)) = rest.rsplit_once('.') else {
        return false;
    };
    !stem.is_empty()
        && ["SF", "RSA", "DSA", "EC"]
            .iter()
            .any(|e| ext.eq_ignore_ascii_case(e))
}

fn sha256_hex(b: &[u8]) -> String {
    il2cpp::hash::sha256_hex(b)
}

/// 6 * 10**17 + int(sha256 hex, 16) % 10**17: Unity 2020.x's buildDate from the metadata.
fn build_date(hex: &str) -> String {
    const M: u128 = 100_000_000_000_000_000;
    let mut r: u128 = 0;
    for c in hex.chars() {
        r = (r * 16 + c.to_digit(16).unwrap_or(0) as u128) % M;
    }
    (6 * M + r).to_string()
}

/// The base.apk being written: zipfile's entries with apk.py's add().
struct Out {
    zo: ZipOut,
    count: usize,
}

fn zip_fail(e: ZipErr) -> Fail {
    Fail::from(e)
}

impl Out {
    /// add(arc, data / src, info): the entry's time, date and method from `info` (else 2018-08-02,
    /// stored), mode 644, a stored entry's data 4-byte aligned.
    fn add(&mut self, arc: &str, info: Option<&pz::Entry>, src: &mut dyn Read) -> Result<(), Fail> {
        let (time, date, method) = match info {
            Some(e) => (e.time, e.date, e.method),
            None => (0, ((2018 - 1980) << 9) | (8 << 5) | 2, pz::STORED),
        };
        let extra = if method == pz::STORED {
            zipw::align_extra(self.zo.tell(), arc)
        } else {
            Vec::new()
        };
        self.zo
            .add(arc, time, date, method, 0o644 << 16, extra, src)
            .map_err(zip_fail)?;
        self.count += 1;
        Ok(())
    }
}

/// il2cpp_apk: the Mono build's APK for Unity's IL2CPP player: without lib/, the assemblies and
/// the signature; with IL2CPP's Managed/ (stored, aligned) and the stamp libunity copies it out
/// again by (2020.x boot.config's buildDate, 2021.x+ unity_app_guid, both from the metadata).
pub fn il2cpp_apk(
    srcs: &[(&Archive, Vec<String>)],
    out_apk: &str,
    il_out: &str,
    ver: &Version,
    log: &mut dyn FnMut(&str),
) -> Result<(), Fail> {
    let meta = format!("{il_out}/Data/Managed/Metadata/global-metadata.dat");
    let mb = fs::read(&meta).map_err(|e| Fail::Error(format!("{e}: {meta:?}")))?;
    let mhash = sha256_hex(&mb);
    drop(mb);
    let tmp = format!("{out_apk}.tmp");
    let zo = ZipOut::create(&tmp).map_err(|e| Fail::Error(format!("{e}: {tmp:?}")))?;
    let mut w = Out { zo, count: 0 };
    let mut n = 0usize;
    let mut seen: std::collections::HashSet<String> = Default::default();
    for (z, names) in srcs {
        for nm in names {
            if seen.contains(nm)
                || nm.ends_with('/')
                || nm.starts_with("lib/")
                || nm.starts_with("assets/bin/Data/Managed/")
                || is_signature(nm)
                || nm == GUID
            {
                continue;
            }
            seen.insert(nm.clone());
            let info = z.info(nm)?;
            if nm == "assets/bin/Data/boot.config" && ver.major == 2020 {
                let raw = z.read(nm)?;
                let mut txt = String::from_utf8_lossy(&raw).into_owned();
                if !txt.split('\n').any(|l| l.starts_with("buildDate=")) {
                    txt = format!(
                        "{}\nbuildDate={}\n",
                        txt.trim_end_matches('\n'),
                        build_date(&mhash)
                    );
                }
                w.add(nm, Some(info), &mut txt.as_bytes())?;
            } else {
                // (zipfile opens the entry for writing before the source is read)
                let mut r: Box<dyn Read> = Box::new(z.reader(info)?);
                w.add(nm, Some(info), &mut r)?;
            }
            n += 1;
        }
    }
    if ver.major >= 2021 {
        let h = &mhash;
        let guid = format!(
            "{}-{}-{}-{}-{}",
            &h[..8],
            &h[8..12],
            &h[12..16],
            &h[16..20],
            &h[20..32]
        );
        w.add(GUID, None, &mut guid.as_bytes())?;
    }
    let md = format!("{il_out}/Data/Managed");
    for (p, rel) in walk_sorted(&md) {
        let mut f = fs::File::open(&p).map_err(|e| Fail::Error(format!("{e}: {p:?}")))?;
        w.add(&format!("assets/bin/Data/Managed/{rel}"), None, &mut f)?;
        n += 1;
    }
    // (allowZip64=False: zipfile refuses at the end what would need zip64 records)
    let what = if w.count > FILECOUNT_LIMIT {
        Some("Files count")
    } else if w.zo.tell() > ZIP64_LIMIT {
        Some("Central directory offset")
    } else {
        None
    };
    if let Some(what) = what {
        return Err(Fail::Error(format!(
            "zipfile.LargeZipFile: {what} would require ZIP64 extensions"
        )));
    }
    w.zo.finish()
        .map_err(|e| Fail::Error(format!("{e}: {tmp:?}")))?;
    fs::rename(&tmp, out_apk).map_err(|e| Fail::Error(format!("{e}: {tmp:?}")))?;
    log(&format!(
        "  base.apk ({n} entries: the APK without its 32-bit libraries and assemblies, with IL2CPP's metadata; {:.1} MiB)",
        crate::getsize(out_apk)? as f64 / 1048576.0
    ));
    Ok(())
}

/// os.walk(top) with ds.sort() and sorted(fs): (path, path relative to top) of every file.
fn walk_sorted(top: &str) -> Vec<(String, String)> {
    fn go(dir: &str, rel: &str, out: &mut Vec<(String, String)>) {
        let Ok(rd) = fs::read_dir(dir) else { return };
        let (mut dirs, mut files) = (Vec::new(), Vec::new());
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            let p = py::join(dir, &n);
            if fs::metadata(&p).is_ok_and(|m| m.is_dir()) {
                dirs.push(n);
            } else {
                files.push(n);
            }
        }
        dirs.sort();
        files.sort();
        for f in files {
            let r = if rel.is_empty() {
                f.clone()
            } else {
                format!("{rel}/{f}")
            };
            out.push((py::join(dir, &f), r));
        }
        for d in dirs {
            let p = py::join(dir, &d);
            if fs::symlink_metadata(&p).is_ok_and(|m| m.file_type().is_symlink()) {
                continue;
            }
            let r = if rel.is_empty() {
                d.clone()
            } else {
                format!("{rel}/{d}")
            };
            go(&p, &r, out);
        }
    }
    let mut out = Vec::new();
    go(top, "", &mut out);
    out
}

/// The IL2CPP build's fields android_package.json keeps.
pub fn info_json(info: &PyJson) -> crate::json::Json {
    use crate::json::Json;
    let mut o = vec![("from".to_string(), Json::str("mono"))];
    for k in [
        "unity",
        "ndk",
        "dotnet_profile",
        "strip",
        "libil2cpp_bytes",
        "build_seconds",
    ] {
        if let Some(v) = info.get(k) {
            o.push((k.to_string(), py_to_json(v)));
        }
    }
    Json::Obj(o)
}

/// androiddata's JSON as apk's (the values il2cpp.json has: strings, integers, ...).
pub fn py_to_json(v: &PyJson) -> crate::json::Json {
    use crate::json::Json;
    match v {
        PyJson::Null => Json::Null,
        PyJson::Bool(b) => Json::Bool(*b),
        PyJson::Int(i) => Json::Int(*i),
        PyJson::Float(f) => Json::Str(f.to_string().into_bytes()),
        PyJson::Str(s) => Json::str(s),
        PyJson::List(l) => Json::List(l.iter().map(py_to_json).collect()),
        PyJson::Obj(o) => Json::Obj(o.iter().map(|(k, v)| (k.clone(), py_to_json(v))).collect()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names() {
        assert_eq!(
            stub_name("assets/bin/Data/Managed/A.dll"),
            Some("Managed/A.dll")
        );
        assert_eq!(stub_name("assets/bin/Data/Managed/x/A.dll"), None);
        assert_eq!(
            stub_name("assets/bin/Data/boot.config"),
            Some("boot.config")
        );
        assert_eq!(stub_name("assets/bin/Data/Managed/A.pdb"), None);
        assert!(is_signature("META-INF/CERT.RSA"));
        assert!(is_signature("meta-inf/cert.sf"));
        assert!(is_signature("META-INF/MANIFEST.MF"));
        assert!(!is_signature("META-INF/services/x"));
        assert!(!is_signature("META-INF/a.txt"));
        // (the Python's 6 * 10**17 + int(h, 16) % 10**17)
        assert_eq!(build_date(&"f".repeat(64)), "684007913129639935");
        assert_eq!(
            build_date(&"0123456789abcdef".repeat(4)),
            "600679889911926255"
        );
    }

    #[test]
    fn refusals() {
        let v = |s: &str| Version::parse(s).unwrap();
        assert!(il2cpp_refusal(&v("2017.3.1f1"))
            .unwrap()
            .contains("start with 2017.4"));
        assert!(il2cpp_refusal(&v("2017.4.9f1"))
            .unwrap()
            .contains("2017.4.16f1 and later"));
        assert!(il2cpp_refusal(&v("2017.4.40f1")).is_none());
        assert!(il2cpp_refusal(&v("6000.3.1f1"))
            .unwrap()
            .contains("version 39"));
        assert!(il2cpp_refusal(&v("2022.3.23f1")).is_none());
    }
}
