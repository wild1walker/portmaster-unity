//! `apk` (convert/pmunity/apk.py): a game the player owns as an Android build (APK, split APKs,
//! XAPK, APKS, AAB, OBBs) made ready for ua_host, with the same files, messages, JSON and exit
//! statuses as the Python. Nothing is converted: the ABI is picked, the native libraries extracted,
//! the APK kept as the package code path (split asset packs merged into it, an App Bundle rebuilt
//! as one APK), the OBBs named as the player looks for them, android_package.json written.
//!
//! `--il2cpp` where it compiles a Mono build for the arm64 IL2CPP player (`pmunity il2cpp`) is not
//! here: that run stops with status 2, before anything is written, for the Python to do it.
pub mod json;
pub mod manifest;
pub mod settings;
pub mod zipw;

use std::collections::BTreeSet;
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::Path;

use androiddata::uversion::{self as uv, Version};
use json::Json;
use manifest::Manifest;
use pkgzip::py;
use pkgzip::zip::{self as pz, Archive};
use unityfs::Value;
use zipw::{ZipErr, ZipOut};

pub const ABIS: [&str; 2] = ["arm64-v8a", "armeabi-v7a"];
const VR_LIBS: [&str; 8] = [
    "libvrapi.so",
    "libOVRPlugin.so",
    "libopenxr_loader.so",
    "libpxr_api.so",
    "libPvr_UnitySDK.so",
    "libovrplatformloader.so",
    "libUnityOpenXR.so",
    "libgvr.so",
];
const VR_CATEGORIES: [&str; 4] = [
    "com.oculus.intent.category.VR",
    "org.khronos.openxr.intent.category.IMMERSIVE_HMD",
    "com.google.intent.category.CARDBOARD",
    "com.google.intent.category.DAYDREAM",
];
const OTHER_ABIS: [&str; 6] = ["x86", "x86_64", "armeabi", "mips", "mips64", "riscv64"];
const IL2CPP_MAGIC: u32 = 0xFAB1_1BAF;
const METADATA: &str = "assets/bin/Data/Managed/Metadata/global-metadata.dat";

/// How a run ends when it does not succeed.
#[derive(Debug)]
pub enum Fail {
    /// A build ua_host cannot run, with the reason (`pmunity apk: refused: ...`, status 1).
    Refused(String),
    /// What the Python raises (its traceback; here the message): status 1.
    Error(String),
    /// What is not ported (compiling a Mono build with IL2CPP): status 2, the Python does it.
    NotPorted(String),
}

impl From<pkgzip::Error> for Fail {
    fn from(e: pkgzip::Error) -> Self {
        Fail::Error(e.to_string())
    }
}

impl From<io::Error> for Fail {
    fn from(e: io::Error) -> Self {
        Fail::Error(pkgzip::Error::from(e).to_string())
    }
}

impl From<ZipErr> for Fail {
    fn from(e: ZipErr) -> Self {
        match e {
            ZipErr::Io(e) => Fail::from(e),
            ZipErr::Py(m) => Fail::Error(m),
            ZipErr::NotPorted(m) => Fail::NotPorted(m),
        }
    }
}

fn refused<T>(msg: impl Into<String>) -> Result<T, Fail> {
    Err(Fail::Refused(msg.into()))
}

/// The options of `pmunity apk` this port acts on.
#[derive(Debug, Clone, Default)]
pub struct Opts {
    pub target: String,
    pub abi: Option<String>,
    pub any_version: bool,
    pub force: bool,
    pub dry_run: bool,
    /// --il2cpp given (a Mono build with only armeabi-v7a is then offered to a 64-bit target).
    pub il2cpp: bool,
}

// ------------------------------------------------------------------ Python's path helpers

fn exists_file(p: &str) -> bool {
    fs::metadata(p).is_ok_and(|m| m.is_file())
}

/// os.path.abspath.
fn abspath(p: &str) -> String {
    if p.starts_with('/') {
        py::normpath(p)
    } else {
        let cwd = std::env::current_dir()
            .map(|c| c.to_string_lossy().into_owned())
            .unwrap_or_default();
        py::normpath(&py::join(&cwd, p))
    }
}

/// os.path.commonpath([abspath(path), abspath(d)]) == abspath(d).
fn inside(path: &str, d: &str) -> bool {
    let (a, b) = (abspath(path), abspath(d));
    let pa: Vec<&str> = a.split('/').filter(|s| !s.is_empty()).collect();
    let pb: Vec<&str> = b.split('/').filter(|s| !s.is_empty()).collect();
    pa.len() >= pb.len() && pa[..pb.len()] == pb[..]
}

/// os.path.splitext(name)[0] of a base name.
fn stem(name: &str) -> &str {
    // (a leading dot does not start an extension)
    let lead = name.len() - name.trim_start_matches('.').len();
    match name[lead..].rfind('.') {
        Some(i) => &name[..lead + i],
        None => name,
    }
}

/// The text `$` matches before (Python's `$`: at the end, or before a newline that ends it).
fn py_end(s: &str) -> &str {
    s.strip_suffix('\n').unwrap_or(s)
}

fn getsize(p: &str) -> Result<u64, Fail> {
    Ok(fs::metadata(p)
        .map_err(|e| Fail::Error(format!("{e}: {p:?}")))?
        .len())
}

/// A directory of its own with a random name (tempfile.mkdtemp).
fn mkdtemp(prefix: &str, dir: &str) -> Result<String, Fail> {
    use std::os::unix::fs::DirBuilderExt;
    const CHARS: &[u8] = b"abcdefghijklmnopqrstuvwxyz0123456789_";
    let mut seed = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(0)
        ^ ((std::process::id() as u64) << 32);
    for _ in 0..100 {
        let mut name = String::from(prefix);
        for _ in 0..8 {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            name.push(CHARS[(seed % CHARS.len() as u64) as usize] as char);
        }
        let p = py::join(dir, &name);
        match fs::DirBuilder::new().mode(0o700).create(&p) {
            Ok(()) => return Ok(p),
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(Fail::Error(format!("{e}: {p:?}"))),
        }
    }
    Err(Fail::Error(
        "FileExistsError: No usable temporary directory name found".into(),
    ))
}

/// os.makedirs(p, exist_ok=True).
fn makedirs(p: &str) -> Result<(), Fail> {
    if p.is_empty() {
        return Ok(());
    }
    fs::create_dir_all(p).map_err(|e| Fail::Error(format!("{e}: {p:?}")))
}

// ------------------------------------------------------------------ inputs

/// An APK (apk.py Apk): its zip, names, manifest, ARM and other ABIs of lib/<abi>/*.so.
pub struct Apk {
    pub path: String,
    pub label: String,
    pub z: Archive,
    pub names: Vec<String>,
    pub manifest: Manifest,
    pub abis: Vec<String>,
}

impl Apk {
    pub fn open(path: &str, label: Option<String>) -> Result<Apk, Fail> {
        let label = label.unwrap_or_else(|| py::basename(path).to_string());
        let z = Archive::open(path)?;
        let names: Vec<String> = z.names().map(str::to_string).collect();
        if !names.iter().any(|n| n == "AndroidManifest.xml") {
            return refused(format!("{label}: no AndroidManifest.xml (not an APK)"));
        }
        let mb = z.read("AndroidManifest.xml")?;
        let manifest = Manifest::new(manifest::parse_manifest(&mb)?)?;
        let abis: BTreeSet<String> = names
            .iter()
            .filter(|n| n.starts_with("lib/") && n.matches('/').count() == 2 && n.ends_with(".so"))
            .map(|n| n.split('/').nth(1).unwrap().to_string())
            .collect();
        Ok(Apk {
            path: path.to_string(),
            label,
            z,
            names,
            manifest,
            abis: abis.into_iter().collect(),
        })
    }

    pub fn libs(&self, abi: &str) -> Vec<String> {
        let pre = format!("lib/{abi}/");
        self.names
            .iter()
            .filter(|n| n.starts_with(&pre) && n.ends_with(".so") && n.matches('/').count() == 2)
            .cloned()
            .collect()
    }

    /// Whether a name is in the APK (`n in names`).
    pub fn has(&self, n: &str) -> bool {
        self.z.get(n).is_some()
    }
}

fn is_obb_name(n: &str) -> bool {
    n.to_lowercase().ends_with(".obb")
}

/// An entry's data into a new file (z.open(n) -> open(dst, 'wb'): copyfileobj).
fn extract(z: &Archive, n: &str, dst: &str) -> Result<(), Fail> {
    let e = z.info(n)?;
    let mut r = z.reader(e)?;
    let mut f = File::create(dst).map_err(|e| Fail::Error(format!("{e}: {dst:?}")))?;
    pz::copy_entry(&mut r, &mut f)?;
    Ok(())
}

/// The inputs sorted out: APKs, OBB paths, App Bundle paths.
type Inputs = (Vec<Apk>, Vec<String>, Vec<String>);

/// unpack_inputs: the command line's files as APKs, OBBs and App Bundles, containers (.xapk,
/// .apks, .apkm, a zip of APKs) opened into tmp.
fn unpack_inputs(paths: &[String], tmp: &str, log: &mut dyn FnMut(&str)) -> Result<Inputs, Fail> {
    let (mut apks, mut obbs, mut aabs) = (Vec::new(), Vec::new(), Vec::new());
    for p in paths {
        if !exists_file(p) {
            return refused(format!("{p}: no such file"));
        }
        if is_obb_name(p) {
            obbs.push(p.clone());
            continue;
        }
        if !pz::is_zipfile(p)? {
            return refused(format!(
                "{p}: not a zip archive (APK, XAPK, APKS and AAB files are zips; an encrypted .apkm is not supported)"
            ));
        }
        let z = Archive::open(p)?;
        let names: Vec<String> = z.names().map(str::to_string).collect();
        let has = |n: &str| names.iter().any(|x| x == n);
        if has("AndroidManifest.xml") {
            apks.push(Apk::open(p, None)?);
        } else if has("base/manifest/AndroidManifest.xml") || has("BundleConfig.pb") {
            aabs.push(p.clone());
        } else {
            let mut inner: Vec<&String> = names
                .iter()
                .filter(|n| n.to_lowercase().ends_with(".apk") && !n.ends_with('/'))
                .collect();
            let inner_obb: Vec<&String> = names
                .iter()
                .filter(|n| is_obb_name(n) && !n.ends_with('/'))
                .collect();
            if inner.iter().any(|n| n.starts_with("splits/")) {
                // bundletool's .apks: splits/ and asset-slices/; standalones/ repeat the app
                inner.retain(|n| !n.starts_with("standalones/"));
            }
            if inner.is_empty() {
                return refused(format!(
                    "{p}: a zip without an APK, AAB or AndroidManifest.xml"
                ));
            }
            let d = mkdtemp("c", tmp)?;
            log(&format!(
                "{}: {} APK(s), {} OBB(s) inside",
                py::basename(p),
                inner.len(),
                inner_obb.len()
            ));
            for n in inner.iter().chain(inner_obb.iter()) {
                let safe = py::normpath(n);
                if safe.starts_with("..") || safe.starts_with('/') {
                    return refused(format!("{p}: unsafe path in archive: {n}"));
                }
                let dst = py::join(&d, &safe.replace('/', "__"));
                extract(&z, n, &dst)?;
                if is_obb_name(n) {
                    obbs.push(dst);
                } else {
                    apks.push(Apk::open(&dst, Some(format!("{}:{n}", py::basename(p))))?);
                }
            }
        }
    }
    Ok((apks, obbs, aabs))
}

fn split_kind(a: &Apk) -> &'static str {
    match a.manifest.split.as_deref() {
        None | Some("") => "base",
        Some(s) if s.starts_with("config.") => "config",
        Some(_) => "pack",
    }
}

/// pick_base: (the base APK's index, the others' indices).
fn pick_base(apks: &[Apk]) -> Result<(usize, Vec<usize>), Fail> {
    let base: Vec<usize> = (0..apks.len())
        .filter(|&i| split_kind(&apks[i]) == "base")
        .collect();
    if base.is_empty() {
        let names: Vec<String> = apks
            .iter()
            .map(|a| match a.manifest.split.as_deref() {
                Some(s) if !s.is_empty() => s.to_string(),
                _ => a.label.clone(),
            })
            .collect();
        return refused(format!(
            "no base APK among the inputs (only splits: {})",
            names.join(", ")
        ));
    }
    if base.len() > 1 {
        let pk: BTreeSet<&str> = base
            .iter()
            .map(|&i| apks[i].manifest.package.as_str())
            .collect();
        let labels: Vec<&str> = base.iter().map(|&i| apks[i].label.as_str()).collect();
        return refused(format!(
            "more than one base APK given ({}; packages {})",
            labels.join(", "),
            pk.into_iter().collect::<Vec<_>>().join(", ")
        ));
    }
    let b = base[0];
    let other: Vec<usize> = (0..apks.len()).filter(|&i| i != b).collect();
    for &i in &other {
        let a = &apks[i];
        if !a.manifest.package.is_empty() && a.manifest.package != apks[b].manifest.package {
            return refused(format!(
                "{} belongs to {}, not {}",
                a.label, a.manifest.package, apks[b].manifest.package
            ));
        }
    }
    Ok((b, other))
}

// ------------------------------------------------------------------ writing zips

/// (DOS time, date) of an entry, as ZipInfo(date_time=zi.date_time) writes it back.
fn dos(e: &pz::Entry) -> (u16, u16) {
    (e.time, e.date)
}

/// copy_entries: [(new name, name in z)] into a new zip at `out` (via out.tmp), each entry's
/// data recompressed as zipfile does, stored ones 4-byte aligned.
fn copy_entries(srcs: &[(&Archive, Vec<(String, String)>)], out: &str) -> Result<(), Fail> {
    let tmp = format!("{out}.tmp");
    let mut zo = ZipOut::create(&tmp).map_err(|e| Fail::Error(format!("{e}: {tmp:?}")))?;
    for (z, entries) in srcs {
        for (arc, n) in entries {
            let zi = z.info(n)?;
            let (t, d) = dos(zi);
            let extra = if zi.method == pz::STORED {
                zipw::align_extra(zo.tell(), arc)
            } else {
                Vec::new()
            };
            // (zipfile opens the entry for writing before the source is read)
            let mut r: Box<dyn Read> = Box::new(z.reader(zi)?);
            zo.add(arc, t, d, zi.method, 0o644 << 16, extra, &mut r)?;
        }
    }
    zo.finish()?;
    fs::rename(&tmp, out).map_err(|e| Fail::Error(format!("{e}: {tmp:?}")))?;
    Ok(())
}

/// aab_to_apk: an App Bundle rebuilt as one APK: the base module's assets/, root/ and lib/, the
/// asset packs' assets/, the base manifest (protobuf).
fn aab_to_apk(path: &str, out_apk: &str, log: &mut dyn FnMut(&str)) -> Result<(), Fail> {
    let z = Archive::open(path)?;
    let names: Vec<String> = z.names().map(str::to_string).collect();
    let modules: BTreeSet<String> = names
        .iter()
        .filter(|n| n.ends_with("/manifest/AndroidManifest.xml"))
        .map(|n| n.split('/').next().unwrap().to_string())
        .collect();
    if !modules.contains("base") {
        return refused(format!("{path}: an App Bundle without a base module"));
    }
    let (mut entries, mut skipped) = (Vec::new(), Vec::new());
    for m in &modules {
        let mb = z.read(&format!("{m}/manifest/AndroidManifest.xml"))?;
        let mf = Manifest::new(manifest::parse_proto_xml(&mb)?)?;
        let mut delivery = "install-time";
        for el in mf.root.iter(None) {
            if el.tag.ends_with("delivery") {
                let kids: Vec<&str> = el
                    .children
                    .iter()
                    .map(|c| c.tag.rsplit(':').next().unwrap())
                    .collect();
                if let Some(k) = ["install-time", "fast-follow", "on-demand"]
                    .into_iter()
                    .find(|k| kids.contains(k))
                {
                    delivery = k;
                }
            }
        }
        if m != "base" && delivery != "install-time" {
            skipped.push(format!("{m} ({delivery})"));
        }
        let pre = format!("{m}/");
        for n in &names {
            if !n.starts_with(&pre) || n.ends_with('/') {
                continue;
            }
            let rel = &n[pre.len()..];
            if rel.starts_with("assets/")
                || (m == "base" && (rel.starts_with("lib/") || rel.starts_with("root/")))
            {
                let arc = rel.strip_prefix("root/").unwrap_or(rel);
                entries.push((arc.to_string(), n.clone()));
            } else if m == "base" && rel == "manifest/AndroidManifest.xml" {
                entries.push(("AndroidManifest.xml".to_string(), n.clone()));
            }
        }
    }
    if !skipped.is_empty() {
        log(&format!(
            "aab: asset packs delivered later by the store, included as well: {}",
            skipped.join(", ")
        ));
    }
    let n = entries.len();
    copy_entries(&[(&z, entries)], out_apk)?;
    log(&format!(
        "aab: {n} entries from {} module(s) -> {out_apk}",
        modules.len()
    ));
    Ok(())
}

/// merge_apks: base.apk plus the assets/ of asset-pack splits as one APK. (The Python writes each
/// source into a zip of its own first and copies those: the same entries, the same bytes.)
fn merge_apks(
    base: &Apk,
    packs: &[&Apk],
    out_apk: &str,
    log: &mut dyn FnMut(&str),
) -> Result<(), Fail> {
    let mut seen: BTreeSet<String> = base.names.iter().cloned().collect();
    let mut srcs: Vec<(&Archive, Vec<(String, String)>)> = vec![(
        &base.z,
        base.names.iter().map(|n| (n.clone(), n.clone())).collect(),
    )];
    for p in packs {
        let add: Vec<(String, String)> = p
            .names
            .iter()
            .filter(|n| n.starts_with("assets/") && !n.ends_with('/') && !seen.contains(n.as_str()))
            .map(|n| (n.clone(), n.clone()))
            .collect();
        for (n, _) in &add {
            seen.insert(n.clone());
        }
        log(&format!(
            "merge: {} assets of split {}",
            add.len(),
            p.manifest.split.as_deref().unwrap_or("None")
        ));
        srcs.push((&p.z, add));
    }
    // (the names a merged entry gets, from a part's own name list: cut at a NUL)
    let srcs: Vec<(&Archive, Vec<(String, String)>)> = srcs
        .into_iter()
        .map(|(z, v)| {
            (
                z,
                v.into_iter()
                    .map(|(a, n)| (a.split('\0').next().unwrap_or("").to_string(), n))
                    .collect(),
            )
        })
        .collect();
    copy_entries(&srcs, out_apk)
}

// ------------------------------------------------------------------ what the build is

/// version_of_head: the Unity version in a SerializedFile's or bundle's first bytes.
fn version_of_head(head: &[u8]) -> Result<Version, String> {
    let ascii = |b: &[u8]| -> Result<String, String> {
        match b.iter().position(|&c| c >= 0x80) {
            Some(i) => Err(format!(
                "'ascii' codec can't decode byte 0x{:02x} in position {i}: ordinal not in range(128)",
                b[i]
            )),
            None => Ok(String::from_utf8(b.to_vec()).unwrap()),
        }
    };
    if head.starts_with(b"UnityFS\0")
        || head.starts_with(b"UnityWeb\0")
        || head.starts_with(b"UnityRaw\0")
    {
        let rest = &head[12.min(head.len())..];
        let mut parts = rest.split(|&c| c == 0);
        parts.next();
        let Some(p1) = parts.next() else {
            return Err("\u{0}IndexError: list index out of range".into());
        };
        return Version::parse(&ascii(p1)?);
    }
    if head.len() < 40 {
        return Err("short file".into());
    }
    let fmt = u32::from_be_bytes(head[8..12].try_into().unwrap());
    if !(9..=23).contains(&fmt) {
        return Err(format!("unsupported SerializedFile format {fmt}"));
    }
    let p = if fmt >= 22 { 48 } else { 20 };
    let Some(end) = head.get(p..).and_then(|s| s.iter().position(|&c| c == 0)) else {
        return Err("subsection not found".into());
    };
    Version::parse(&ascii(&head[p..p + end])?)
}

/// The first `n` bytes of an entry (z.open(n).read(k)): the CRC is checked when they are all.
fn read_head(z: &Archive, name: &str, k: usize) -> Result<Vec<u8>, Fail> {
    let mut r = z.reader(z.info(name)?)?;
    let mut v = vec![0u8; k];
    let mut got = 0;
    while got < k {
        match r.read(&mut v[got..]) {
            Ok(0) => break,
            Ok(n) => got += n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(e) => return Err(e.into()),
        }
    }
    v.truncate(got);
    Ok(v)
}

/// uname's machine (platform.machine()).
fn machine() -> String {
    let mut u: libc::utsname = unsafe { std::mem::zeroed() };
    if unsafe { libc::uname(&mut u) } != 0 {
        return String::new();
    }
    let m = unsafe { std::ffi::CStr::from_ptr(u.machine.as_ptr()) };
    m.to_string_lossy().into_owned()
}

/// choose_abi: (ABI, why) for the build's ABIs `have`.
fn choose_abi(have: &[String], target: &str, abi: Option<&str>) -> Result<(String, String), Fail> {
    let arm: Vec<&str> = ABIS
        .iter()
        .copied()
        .filter(|a| have.iter().any(|h| h == a))
        .collect();
    if let Some(abi) = abi {
        if !have.iter().any(|h| h == abi) {
            let list = if have.is_empty() {
                "no native libraries".to_string()
            } else {
                have.join(", ")
            };
            return refused(format!("--abi {abi}: the build has {list}"));
        }
        return Ok((abi.to_string(), format!("--abi {abi}")));
    }
    if arm.is_empty() {
        if have.iter().any(|a| OTHER_ABIS.contains(&a.as_str())) {
            return refused(format!(
                "the build has native libraries only for {}: the runtime runs arm64-v8a or armeabi-v7a",
                have.join(", ")
            ));
        }
        return refused(
            "the build has no native libraries (lib/<abi>/): not a Unity Android build, or its libraries \
             are in a split APK (config.arm64_v8a / config.armeabi_v7a) that was not given",
        );
    }
    let t = if target.is_empty() { "auto" } else { target };
    let mut targets: Vec<String> = t
        .split(',')
        .map(|x| py::strip(x).to_string())
        .filter(|x| !x.is_empty())
        .collect();
    if targets.is_empty() {
        targets = vec!["auto".into()];
    }
    let known = |x: &str| x == "arm64" || x == "armhf";
    let bad = targets.iter().any(|x| !known(x) && x != "auto");
    if bad || (targets.iter().any(|x| x == "auto") && targets.len() > 1) {
        return refused(format!(
            "--target {target}: auto, arm64, armhf, or a list of arm64 and armhf (the first the build has)"
        ));
    }
    let why;
    if targets == ["auto"] {
        let m = machine().to_lowercase();
        if cfg!(target_os = "linux")
            && [
                "aarch64", "arm64", "armv8l", "armv7l", "armhf", "armv7", "arm",
            ]
            .contains(&m.as_str())
        {
            // (this program is 64-bit)
            let ptr = 8 * std::mem::size_of::<usize>();
            targets = vec![if (m == "aarch64" || m == "arm64") && ptr == 64 {
                "arm64".into()
            } else {
                "armhf".into()
            }];
            why = format!("this machine ({m}, {ptr}-bit userland)");
        } else {
            let pick = arm[0];
            return Ok((
                pick.to_string(),
                if arm.len() > 1 {
                    format!(
                        "{pick} (the build has {}; --target armhf or --abi picks the 32-bit one)",
                        arm.join(", ")
                    )
                } else {
                    format!("{pick}, the only ARM ABI of the build")
                },
            ));
        }
    } else {
        why = format!("--target {}", targets.join(","));
    }
    for t in &targets {
        let want = if t == "arm64" {
            "arm64-v8a"
        } else {
            "armeabi-v7a"
        };
        if have.iter().any(|h| h == want) {
            let mut w = format!("{want} for {why}");
            if targets.len() > 1 {
                w += &format!(" (the build has {})", arm.join(", "));
            }
            return Ok((want.to_string(), w));
        }
    }
    if targets.len() > 1 {
        return refused(format!(
            "the build has {}, a player for none of {why}",
            arm.join(", ")
        ));
    }
    if targets == ["arm64"] {
        return refused(format!(
            "the build has only armeabi-v7a (Unity: Mono builds, and IL2CPP builds without ARM64), which needs a \
             32-bit (armhf) userland; {why} is 64-bit. On a firmware with a 32-bit userland use --target armhf"
        ));
    }
    refused(format!(
        "the build has only arm64-v8a, which needs a 64-bit userland; {why} is 32-bit (--target arm64 for a \
         64-bit firmware)"
    ))
}

/// classify_obb: (main|patch, the name the player looks for, the versionCode the name has).
fn classify_obb(
    path: &str,
    package: &str,
    code: i128,
) -> Result<(&'static str, String, Option<i128>), Fail> {
    let b = py::basename(path);
    let m = py_end(b);
    let kind = if m.starts_with("main.") {
        Some("main")
    } else if m.starts_with("patch.") {
        Some("patch")
    } else {
        None
    };
    if let Some(kind) = kind {
        // ^(main|patch)\.(\d+)\.(.+)\.obb$
        let rest = &m[kind.len() + 1..];
        let digits = rest.bytes().take_while(u8::is_ascii_digit).count();
        if digits > 0 && rest[digits..].starts_with('.') {
            let tail = &rest[digits + 1..];
            if let Some(pkg) = tail.strip_suffix(".obb") {
                if !pkg.is_empty() && !pkg.contains('\n') {
                    if pkg != package {
                        return refused(format!("{b}: an OBB of {pkg}, not of {package}"));
                    }
                    let c: i128 = rest[..digits].parse().unwrap_or(i128::MAX);
                    return Ok((kind, b.to_string(), Some(c)));
                }
            }
        }
    }
    let low = m.to_lowercase();
    let patch = low.len() > "patch.obb".len()
        && low.ends_with("patch.obb")
        && matches!(low.as_bytes()[low.len() - 10], b'.' | b'_' | b'-');
    let kind = if patch { "patch" } else { "main" };
    Ok((kind, format!("{kind}.{code}.{package}.obb"), None))
}

/// ^assets/<8-4-4-4-12 lowercase hex>$
fn is_build_id_entry(n: &str) -> Option<&str> {
    let s = py_end(n).strip_prefix("assets/")?;
    let parts: Vec<&str> = s.split('-').collect();
    let ok = parts.len() == 5
        && parts.iter().zip([8, 4, 4, 4, 12]).all(|(p, l)| {
            p.len() == l
                && p.bytes()
                    .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
        });
    ok.then(|| &n["assets/".len()..])
}

/// obb_build_id: the build ids an OBB carries; its unity_obb_guid read (as the Python reads it).
fn obb_build_id(path: &str) -> Result<Vec<String>, Fail> {
    let not_zip = || {
        Fail::Refused(format!(
            "{}: not a zip (Unity OBBs are zips)",
            py::basename(path)
        ))
    };
    let bad = |e: pkgzip::Error| {
        if e.to_string().starts_with("zipfile.BadZipFile") {
            not_zip()
        } else {
            Fail::from(e)
        }
    };
    let z = Archive::open(path).map_err(bad)?;
    let ids: Vec<String> = z
        .names()
        .filter_map(is_build_id_entry)
        .map(str::to_string)
        .collect();
    if z.get("unity_obb_guid").is_some() {
        z.read("unity_obb_guid").map_err(bad)?;
    }
    Ok(ids)
}

/// place: dst = src, moved when src is this tool's own, else a hard link or a copy.
fn place(src: &str, dst: &str, log: &mut dyn FnMut(&str), mv: bool) -> Result<(), Fail> {
    let mut how = None;
    if mv && fs::rename(src, dst).is_ok() {
        how = Some("moved");
    }
    let how = match how {
        Some(h) => h,
        None => androiddata::android_data::clone(src, dst, true)
            .map_err(|e| Fail::Error(stop_message(e)))?,
    };
    log(&format!(
        "  {} ({how}, {:.1} MiB)",
        py::basename(dst),
        getsize(dst)? as f64 / 1048576.0
    ));
    Ok(())
}

fn stop_message(e: androiddata::Stop) -> String {
    use androiddata::Stop;
    match e {
        Stop::Exit(m) | Stop::Fail(m) | Stop::NotPorted(m) | Stop::Full(m) => m,
        Stop::Status(s) => format!("status {s}"),
    }
}

/// A value of the settings as the JSON's (a string's bytes; an int).
fn value_json(v: &Value) -> Json {
    match v {
        Value::Str(b) | Value::Bytes(b) => Json::Str(b.clone()),
        Value::Bool(b) => Json::Bool(*b),
        other => other.as_int().map_or(Json::Null, Json::Int),
    }
}

fn gfx_name(a: i128) -> String {
    match a {
        8 => "GLES2".into(),
        11 => "GLES3".into(),
        21 => "Vulkan".into(),
        17 => "Metal".into(),
        16 => "GLCore".into(),
        2 => "D3D11".into(),
        _ => a.to_string(),
    }
}

fn orientation(v: Option<&Value>) -> Json {
    let i = v.and_then(Value::as_int);
    // (a float or string is no key of ORIENTATIONS)
    let i = match v {
        Some(Value::Str(_) | Value::Bytes(_) | Value::F32(_) | Value::F64(_)) => None,
        _ => i,
    };
    match i {
        Some(0) => Json::str("portrait"),
        Some(1) => Json::str("portrait upside down"),
        Some(2) => Json::str("landscape right"),
        Some(3) => Json::str("landscape left"),
        Some(4) => Json::str("auto rotation"),
        _ => Json::Null,
    }
}

/// Where the data was found: the base APK, an asset pack (index into packs), an OBB.
#[derive(Clone, Copy, PartialEq)]
enum DataIn {
    Base,
    Pack(usize),
    Obb(usize),
}

/// `pmunity apk`'s run: the build imported into `out` (with dry_run only read); its
/// android_package.json fields, or the dry run's report.
pub fn run(
    inputs: &[String],
    out: &str,
    o: &Opts,
    log: &mut dyn FnMut(&str),
) -> Result<Json, Fail> {
    if !o.dry_run {
        makedirs(out)?;
    }
    let tdir = if o.dry_run {
        std::env::temp_dir().to_string_lossy().into_owned()
    } else {
        out.to_string()
    };
    let tmp = mkdtemp(".apk-", &tdir)?;
    let r = run_in(inputs, out, &tmp, o, log);
    let _ = fs::remove_dir_all(&tmp);
    r
}

fn run_in(
    inputs: &[String],
    out: &str,
    tmp: &str,
    o: &Opts,
    log: &mut dyn FnMut(&str),
) -> Result<Json, Fail> {
    let (mut apks, mut obbs, aabs) = unpack_inputs(inputs, tmp, log)?;
    if !aabs.is_empty() {
        if aabs.len() > 1 || !apks.is_empty() {
            return refused("give one App Bundle (.aab), without APKs");
        }
        let built = py::join(tmp, "from-aab.apk");
        aab_to_apk(&aabs[0], &built, log)?;
        apks = vec![Apk::open(&built, Some(py::basename(&aabs[0]).to_string()))?];
    }
    if apks.is_empty() {
        return refused("no APK among the inputs");
    }
    let (bi, others) = pick_base(&apks)?;
    let base = &apks[bi];
    let mf = &base.manifest;
    let configs: Vec<&Apk> = others
        .iter()
        .map(|&i| &apks[i])
        .filter(|a| split_kind(a) == "config")
        .collect();
    let packs: Vec<&Apk> = others
        .iter()
        .map(|&i| &apks[i])
        .filter(|a| split_kind(a) == "pack")
        .collect();
    // OBBs beside a single APK: Unity's "Split Application Binary" writes <name>.main.obb
    if obbs.is_empty() && inputs.len() == 1 && inputs[0].to_lowercase().ends_with(".apk") {
        let d = py::dirname(&abspath(&inputs[0])).to_string();
        let st = stem(py::basename(&inputs[0])).to_string();
        let mut names: Vec<String> = fs::read_dir(if d.is_empty() { "." } else { &d })
            .map_err(|e| Fail::Error(format!("{e}: {d:?}")))?
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .collect();
        names.sort();
        for n in names {
            if is_obb_name(&n)
                && (n.starts_with(&format!("{st}."))
                    || n.ends_with(&format!(".{}.obb", mf.package)))
            {
                obbs.push(py::join(&d, &n));
            }
        }
    }
    let code = mf.version_code;
    let sdk = |v: Option<i128>| v.map_or("None".to_string(), |x| x.to_string());
    log(&format!(
        "package {}, versionCode {code}, versionName {}, minSdk {}, targetSdk {}",
        mf.package,
        py::str_repr(&mf.version_name),
        sdk(mf.min_sdk),
        sdk(mf.target_sdk)
    ));
    if mf.package.is_empty() {
        return refused("the manifest names no package");
    }

    // native libraries: the base APK and its ABI splits
    let mut have: BTreeSet<String> = base.abis.iter().cloned().collect();
    for a in &configs {
        have.extend(a.abis.iter().cloned());
    }
    let have: Vec<String> = have.into_iter().collect();
    // --il2cpp: a build with only the 32-bit player is offered to a 64-bit target too
    let virt = o.il2cpp
        && have.iter().any(|a| a == "armeabi-v7a")
        && !have.iter().any(|a| a == "arm64-v8a");
    let offered: Vec<String> = if virt {
        let mut s: BTreeSet<String> = have.iter().cloned().collect();
        s.insert("arm64-v8a".into());
        s.into_iter().collect()
    } else {
        have.clone()
    };
    let (pick, why) = choose_abi(&offered, &o.target, o.abi.as_deref())?;
    if virt && pick == "arm64-v8a" {
        return Err(Fail::NotPorted(
            "--il2cpp: a Mono build compiled with Unity's IL2CPP for the arm64 player".into(),
        ));
    }
    let mut lib_src: Vec<(&Apk, String)> = Vec::new();
    for a in std::iter::once(base).chain(configs.iter().copied()) {
        for n in a.libs(&pick) {
            lib_src.push((a, n));
        }
    }
    let libnames: Vec<String> = lib_src
        .iter()
        .map(|(_, n)| py::basename(n).to_string())
        .collect::<BTreeSet<_>>()
        .into_iter()
        .collect();
    let missing: Vec<&str> = ["libmain.so", "libunity.so"]
        .into_iter()
        .filter(|n| !libnames.iter().any(|x| x == n))
        .collect();
    if !missing.is_empty() {
        let libs = if libnames.is_empty() {
            "none".to_string()
        } else {
            libnames.join(", ")
        };
        return refused(format!(
            "lib/{pick} has no {}: not a Unity Android build (libraries: {libs})",
            missing.join(" or ")
        ));
    }
    let has_lib = |n: &str| libnames.iter().any(|x| x == n);
    let backend = if has_lib("libil2cpp.so") {
        "il2cpp"
    } else if ["libmonobdwgc-2.0.so", "libmono.so", "libmonosgen-2.0.so"]
        .iter()
        .any(|n| has_lib(n))
    {
        "mono"
    } else {
        return refused(format!(
            "lib/{pick} has neither libil2cpp.so nor a Mono runtime (libmono*.so)"
        ));
    };
    log(&format!("ABI {why}; scripting backend {backend}"));

    // OBBs
    let mut obb_plan: Vec<(&'static str, String, String)> = Vec::new(); // (kind, path, name)
    for p in &obbs {
        let (kind, mut name, ocode) = classify_obb(p, &mf.package, code)?;
        if let Some(oc) = ocode {
            if oc != code {
                log(&format!(
                    "note: {} names versionCode {oc}; the APK has {code}: placed as {kind}.{code}.{}.obb",
                    py::basename(p),
                    mf.package
                ));
                name = format!("{kind}.{code}.{}.obb", mf.package);
            }
        }
        if let Some((_, q, _)) = obb_plan.iter().find(|(k, _, _)| *k == kind) {
            return refused(format!(
                "two {kind} OBBs: {} and {}",
                py::basename(q),
                py::basename(p)
            ));
        }
        obb_plan.push((kind, p.clone(), name));
    }
    obb_plan.sort_by(|a, b| a.0.cmp(b.0));
    let bid = mf
        .meta_get("unity.build-id")
        .filter(|b| !b.is_empty())
        .map(str::to_string);
    let mut obb_zips: Vec<(Archive, String)> = Vec::new();
    for (kind, p, name) in &obb_plan {
        let ids = obb_build_id(p)?;
        obb_zips.push((Archive::open(p)?, p.clone()));
        if let Some(b) = bid.as_deref() {
            if !ids.is_empty() && !ids.iter().any(|i| i == b) {
                let msg = format!(
                    "{} belongs to another build (its build id {}, the APK's {b}): the player would not mount it",
                    py::basename(p),
                    ids[0]
                );
                if !o.force {
                    return refused(format!("{msg} (--force places it anyway)"));
                }
                log(&format!("warning: {msg}"));
            }
        }
        log(&format!(
            "OBB {kind}: {} -> {name}{}",
            py::basename(p),
            if ids.is_empty() {
                String::new()
            } else {
                format!(" (build id {})", ids[0])
            }
        ));
    }

    // Unity data and version
    let mut data_zips: Vec<(DataIn, &Archive, &str)> = vec![(DataIn::Base, &base.z, &base.path)];
    for (i, a) in packs.iter().enumerate() {
        data_zips.push((DataIn::Pack(i), &a.z, &a.path));
    }
    for (i, (z, p)) in obb_zips.iter().enumerate() {
        data_zips.push((DataIn::Obb(i), z, p));
    }
    let mut found = None;
    'zips: for (w, z, p) in &data_zips {
        for n in [
            "assets/bin/Data/globalgamemanagers",
            "assets/bin/Data/data.unity3d",
            "assets/bin/Data/mainData",
        ] {
            if z.get(n).is_some() {
                found = Some((*w, *z, *p, n));
                break 'zips;
            }
        }
    }
    let Some((data_in, dz, dpath, dn)) = found else {
        let unity_activity = mf.activities.iter().any(|a| {
            a.as_deref() == Some("com.unity3d.player.UnityPlayerActivity")
                || a.as_deref().unwrap_or("").contains("unity3d")
        });
        if !unity_activity {
            return refused(
                "no Unity data (assets/bin/Data) and no Unity activity: not a Unity game",
            );
        }
        return refused(
            "the APK has no Unity data (assets/bin/Data/globalgamemanagers or data.unity3d): it is in an OBB or a \
             Play asset pack that was not given (store downloads); give the .obb (or the .xapk / .apks / .aab) too",
        );
    };
    let head = read_head(dz, dn, 512)?;
    let ver = match version_of_head(&head) {
        Ok(v) => v,
        Err(e) if e.starts_with('\0') => return Err(Fail::Error(e[1..].to_string())),
        Err(e) => return refused(format!("{dn}: cannot read the Unity version ({e})")),
    };
    let in_obb = matches!(data_in, DataIn::Obb(_));
    log(&format!(
        "Unity {} ({}{})",
        ver.text,
        dn.rsplit('/').next().unwrap(),
        if in_obb { " in an OBB" } else { "" }
    ));
    if !(uv::FIRST <= ver.line() && ver.line() <= uv::LAST || o.any_version) {
        return refused(format!(
            "Unity {}: the runtime runs players of Unity {}.{} - {}.{} (docs/compat/README.md); \
             --any-unity-version tries anyway",
            ver.text,
            uv::FIRST.0,
            uv::FIRST.1,
            uv::LAST.0,
            uv::LAST.1
        ));
    }
    if backend == "mono" && pick == "arm64-v8a" {
        return refused("a Mono build with arm64-v8a libraries: Unity never shipped one; the APK looks modified");
    }
    let ps = settings::read(dpath, dz, dn, log);
    let apis = ps.graphics_apis.clone().unwrap_or_default();
    if !apis.is_empty() && !apis.iter().any(|a| *a == 8 || *a == 11) {
        let names: Vec<String> = apis.iter().map(|&a| gfx_name(a)).collect();
        return refused(format!(
            "graphics APIs {}: the build has no OpenGL ES; the runtime draws with GLES 2/3 only",
            names.join(", ")
        ));
    }
    let mut notes: Vec<String> = Vec::new();
    if !apis.is_empty() {
        let names: Vec<String> = apis.iter().map(|&a| gfx_name(a)).collect();
        log(&format!(
            "graphics APIs {}{}",
            names.join(", "),
            if apis.contains(&21) {
                " (Vulkan is not offered: the player takes GLES)"
            } else {
                ""
            }
        ));
    }
    let swappy = ps.android_use_swappy.as_ref().is_some_and(settings::truthy);
    if swappy && ver.line() >= (2019, 3) {
        notes.push("Optimized Frame Pacing (Swappy) is on in this build".into());
    }
    let mut meta_ver: Option<u32> = None;
    if backend == "il2cpp" {
        let Some((_, mz, _)) = data_zips.iter().find(|(_, z, _)| z.get(METADATA).is_some()) else {
            return refused(
                "IL2CPP build without assets/bin/Data/Managed/Metadata/global-metadata.dat (the metadata \
                 libil2cpp loads at startup): moved or stripped by a protector, not runnable as is",
            );
        };
        let head = read_head(mz, METADATA, 8)?;
        let mut note = None;
        if head.len() == 8 && u32::from_le_bytes(head[..4].try_into().unwrap()) == IL2CPP_MAGIC {
            meta_ver = Some(u32::from_le_bytes(head[4..8].try_into().unwrap()));
        } else {
            note = Some("not in the usual format (encrypted or obfuscated: the game's own libil2cpp reads it)");
        }
        match meta_ver {
            Some(v) if v != 0 => log(&format!("IL2CPP metadata version {v}")),
            _ => log(&format!("IL2CPP metadata {}", note.unwrap_or("None"))),
        }
    }
    let plugins: Vec<String> = libnames
        .iter()
        .filter(|x| {
            ![
                "libmain.so",
                "libunity.so",
                "libil2cpp.so",
                "libmonobdwgc-2.0.so",
                "libmono.so",
                "libMonoPosixHelper.so",
                "libmono-native.so",
                "libmonosgen-2.0.so",
            ]
            .contains(&x.as_str())
        })
        .cloned()
        .collect();
    if !plugins.is_empty() {
        log(&format!("native plugins: {}", plugins.join(", ")));
    }
    let mut vr: Vec<String> = plugins
        .iter()
        .filter(|p| VR_LIBS.contains(&p.as_str()))
        .cloned()
        .collect::<BTreeSet<_>>()
        .into_iter()
        .collect();
    vr.extend(
        mf.categories
            .iter()
            .filter(|c| VR_CATEGORIES.contains(&c.as_str()))
            .cloned(),
    );
    vr.extend(
        mf.features
            .iter()
            .filter(|(f, req)| *req && f.starts_with("android.hardware.vr."))
            .map(|(f, _)| f.clone()),
    );
    if !vr.is_empty() {
        let msg = format!("a VR build (headset only: {})", vr.join(", "));
        if !o.force {
            return refused(format!("{msg}; --force imports it anyway"));
        }
        notes.push(msg);
    }
    if let Some(g) = mf.gles.filter(|&g| g != 0 && g > 0x30002) {
        notes.push(format!(
            "the manifest asks for OpenGL ES {}.{}",
            g >> 16,
            g & 0xffff
        ));
    }
    for n in &notes {
        log(&format!("note: {n}"));
    }
    let meta_json = Json::Obj(
        mf.meta
            .iter()
            .map(|(k, v)| (k.clone(), Json::opt_str(v.as_deref())))
            .collect(),
    );
    if o.dry_run {
        let abis: Vec<&str> = ABIS
            .iter()
            .copied()
            .filter(|a| have.iter().any(|h| h == a))
            .collect();
        let obbs_json = Json::Obj(
            obb_plan
                .iter()
                .map(|(k, _, n)| (k.to_string(), Json::str(n)))
                .collect(),
        );
        let mut sizes: BTreeSet<String> = inputs.iter().cloned().collect();
        for (_, p, _) in &obb_plan {
            if !inside(p, tmp) {
                sizes.insert(p.clone());
            }
        }
        let mut total = 0u64;
        for p in &sizes {
            total += getsize(p)?;
        }
        let size_mb = (total as f64 / 1048576.0 + 0.5) as i128;
        return Ok(Json::Obj(vec![
            ("package".into(), Json::str(&mf.package)),
            ("version_code".into(), Json::Int(code)),
            ("version_name".into(), Json::str(&mf.version_name)),
            ("min_sdk".into(), Json::opt_int(mf.min_sdk)),
            ("target_sdk".into(), Json::opt_int(mf.target_sdk)),
            ("abi".into(), Json::str(&pick)),
            ("abi_why".into(), Json::str(&why)),
            ("abis".into(), Json::strs(&abis)),
            ("all_abis".into(), Json::strs(&have)),
            ("backend".into(), Json::str(backend)),
            ("unity_version".into(), Json::str(&ver.text)),
            (
                "data_in".into(),
                Json::str(match data_in {
                    DataIn::Obb(_) => "obb",
                    DataIn::Pack(_) => "asset pack",
                    DataIn::Base => "apk",
                }),
            ),
            ("obbs".into(), obbs_json),
            (
                "graphics_apis".into(),
                Json::List(apis.iter().map(|&a| Json::str(&gfx_name(a))).collect()),
            ),
            ("swappy".into(), Json::Bool(swappy)),
            (
                "product_name".into(),
                ps.product_name.as_ref().map_or(Json::Null, value_json),
            ),
            (
                "company_name".into(),
                ps.company_name.as_ref().map_or(Json::Null, value_json),
            ),
            (
                "orientation".into(),
                orientation(ps.default_orientation.as_ref()),
            ),
            (
                "il2cpp_metadata".into(),
                Json::opt_int(meta_ver.map(i128::from)),
            ),
            ("plugins".into(), Json::strs(&plugins)),
            ("notes".into(), Json::strs(&notes)),
            ("meta_data".into(), meta_json),
            ("size_mb".into(), Json::Int(size_mb)),
        ]));
    }

    // write the layout
    let data_packs: Vec<&Apk> = packs
        .iter()
        .copied()
        .filter(|a| a.names.iter().any(|x| x.starts_with("assets/")))
        .collect();
    log(&format!("-> {out}"));
    let apk_out = py::join(out, "base.apk");
    if !data_packs.is_empty() {
        merge_apks(base, &data_packs, &apk_out, log)?;
        log(&format!(
            "  base.apk (merged with {} asset pack split(s), {:.1} MiB)",
            data_packs.len(),
            getsize(&apk_out)? as f64 / 1048576.0
        ));
    } else {
        place(&base.path, &apk_out, log, inside(&base.path, tmp))?;
    }
    let libdir = py::join(out, "lib");
    if Path::new(&libdir).is_dir() {
        for e in fs::read_dir(&libdir).map_err(|e| Fail::Error(format!("{e}: {libdir:?}")))? {
            let e = e?;
            let n = e.file_name().to_string_lossy().into_owned();
            if n.ends_with(".so") {
                let p = py::join(&libdir, &n);
                fs::remove_file(&p).map_err(|e| Fail::Error(format!("{e}: {p:?}")))?;
            }
        }
    }
    makedirs(&libdir)?;
    for (a, nm) in &lib_src {
        extract(&a.z, nm, &py::join(&libdir, py::basename(nm)))?;
    }
    log(&format!(
        "  lib/: {} libraries from lib/{pick}",
        libnames.len()
    ));
    for e in fs::read_dir(out).map_err(|e| Fail::Error(format!("{e}: {out:?}")))? {
        let e = e?;
        let n = e.file_name().to_string_lossy().into_owned();
        if is_placed_obb(&n) {
            let p = py::join(out, &n);
            fs::remove_file(&p).map_err(|e| Fail::Error(format!("{e}: {p:?}")))?;
        }
    }
    let mut res = Json::Obj(vec![
        ("package".into(), Json::str(&mf.package)),
        ("version_code".into(), Json::Int(code)),
        ("version_name".into(), Json::str(&mf.version_name)),
        ("apk".into(), Json::str("base.apk")),
    ]);
    for (kind, p, name) in &obb_plan {
        place(p, &py::join(out, name), log, inside(p, tmp))?;
        res.set(&format!("{kind}_obb"), Json::str(name));
    }
    if backend == "mono" {
        let etc: Vec<&String> = base
            .names
            .iter()
            .filter(|n| n.starts_with("assets/bin/Data/Managed/etc/") && !n.ends_with('/'))
            .collect();
        for nm in &etc {
            let dst = py::join(out, &nm.split('/').collect::<Vec<_>>().join("/"));
            makedirs(py::dirname(&dst))?;
            extract(&base.z, nm, &dst)?;
        }
        if !etc.is_empty() {
            log(&format!(
                "  assets/bin/Data/Managed/etc/: {} files (Mono config)",
                etc.len()
            ));
        }
    }
    if let Some(b) = &bid {
        res.set("build_id", Json::str(b));
    }
    res.set("meta_data", meta_json);
    res.set("min_sdk", Json::opt_int(mf.min_sdk));
    res.set("target_sdk", Json::opt_int(mf.target_sdk));
    res.set("abi", Json::str(&pick));
    res.set("backend", Json::str(backend));
    res.set("unity_version", Json::str(&ver.text));
    res.set("source", Json::str("apk"));
    if let Some(v) = meta_ver.filter(|&v| v != 0) {
        res.set("il2cpp_metadata", Json::Int(v as i128));
    }
    if !plugins.is_empty() {
        res.set("plugins", Json::strs(&plugins));
    }
    if let Some(pn) = ps.product_name.as_ref().filter(|v| settings::truthy(v)) {
        res.set("product_name", value_json(pn));
    }
    if !notes.is_empty() {
        res.set("notes", Json::strs(&notes));
    }
    let pj = py::join(out, "android_package.json");
    fs::write(&pj, json::dumps(&res)).map_err(|e| Fail::Error(format!("{e}: {pj:?}")))?;
    log(&format!(
        "  android_package.json; run: ua_host ({} build) {out}",
        if pick == "arm64-v8a" {
            "aarch64"
        } else {
            "armhf"
        }
    ));
    Ok(res)
}

/// ^(main|patch)\.\d+\..+\.obb$ (an OBB an earlier layout placed).
fn is_placed_obb(n: &str) -> bool {
    let m = py_end(n);
    let rest = match m.strip_prefix("main.").or_else(|| m.strip_prefix("patch.")) {
        Some(r) => r,
        None => return false,
    };
    let d = rest.bytes().take_while(u8::is_ascii_digit).count();
    if d == 0 || !rest[d..].starts_with('.') {
        return false;
    }
    let tail = &rest[d + 1..];
    tail.len() > 4 && tail.ends_with(".obb") && !tail[..tail.len() - 4].contains('\n')
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn obb_names() {
        assert_eq!(
            classify_obb("/x/main.12.com.a.obb", "com.a", 12).unwrap(),
            ("main", "main.12.com.a.obb".into(), Some(12))
        );
        assert_eq!(
            classify_obb("x.patch.obb", "com.a", 3).unwrap(),
            ("patch", "patch.3.com.a.obb".into(), None)
        );
        assert_eq!(
            classify_obb("Game-PATCH.obb", "com.a", 3).unwrap().0,
            "patch"
        );
        assert_eq!(classify_obb("patch.obb", "com.a", 3).unwrap().0, "main");
        assert_eq!(
            classify_obb("Game.main.obb", "com.a", 3).unwrap().1,
            "main.3.com.a.obb"
        );
        assert!(matches!(
            classify_obb("main.1.com.b.obb", "com.a", 1),
            Err(Fail::Refused(_))
        ));
        assert!(is_placed_obb("main.7.com.x.obb"));
        assert!(!is_placed_obb("main.x.com.obb"));
        assert!(!is_placed_obb("main.7..obb") || is_placed_obb("main.7..obb"));
    }

    #[test]
    fn build_ids() {
        assert_eq!(
            is_build_id_entry("assets/0123abcd-0000-1111-2222-333344445555"),
            Some("0123abcd-0000-1111-2222-333344445555")
        );
        assert_eq!(
            is_build_id_entry("assets/0123ABCD-0000-1111-2222-333344445555"),
            None
        );
        assert_eq!(is_build_id_entry("assets/bin/Data/x"), None);
    }

    #[test]
    fn heads() {
        assert_eq!(
            version_of_head(b"UnityFS\0\0\0\0\x085.x.x\x002021.3.4f1\0")
                .unwrap()
                .text,
            "2021.3.4f1"
        );
        assert_eq!(version_of_head(b"short").unwrap_err(), "short file");
        let mut h = vec![0u8; 20];
        h[11] = 30;
        h.extend([0u8; 40]);
        assert_eq!(
            version_of_head(&h).unwrap_err(),
            "unsupported SerializedFile format 30"
        );
    }

    #[test]
    fn stems() {
        assert_eq!(stem("Game.apk"), "Game");
        assert_eq!(stem(".apk"), ".apk");
        assert_eq!(stem("a.b.apk"), "a.b");
        assert_eq!(stem("noext"), "noext");
    }
}
