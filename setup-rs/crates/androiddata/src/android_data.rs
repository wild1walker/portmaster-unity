//! `pmu android-data` (android_data.py main): the data tree Unity's Android player reads, from a
//! game's converted desktop data, and (--pack) the APK and OBB(s) it is packed into. The same
//! files, state, report and exit statuses as the Python.
use crate::args::Args;
use crate::ggm;
use crate::paths::{self, abspath, listdir, realpath};
use crate::plugins::{self, Survey};
use crate::pyjson::{self, Json};
use crate::retarget;
use crate::streaming as ST;
use crate::uversion::{self as UV, Version};
use crate::video;
use pkgzip::py;
use std::collections::BTreeSet;
use std::fs::{self, File};
use std::io::Write;
use std::os::unix::fs::MetadataExt;
use std::path::Path;
use std::sync::Arc;
use unityfs::ClassDb;

pub const ANDROID: u32 = 13;
pub const NO_SPACE: u8 = 4;
const STATE_VERSION: i128 = 10;
const DATA_JSON: &[&str] = &["ScriptingAssemblies.json", "RuntimeInitializeOnLoads.json"];
const GLES31_SDK: i64 = 22;
pub const OBB_GUID_FROM: (u32, u32) = (2021, 1);
const DEFAULT_RESOURCES_IN_RESOURCES_FROM: (u32, u32) = (2023, 2);

/// How the command stops, as the Python does.
#[derive(Debug)]
pub enum Stop {
    /// SystemExit(message): the message on stderr, exit 1.
    Exit(String),
    /// SystemExit(status): that status, nothing said (NO_SPACE).
    Status(u8),
    /// An exception: its message on stderr, exit 1.
    Fail(String),
    /// What this port does not do (yet): exit 2, the setup runs the Python.
    NotPorted(String),
}

impl Stop {
    pub fn status(&self) -> u8 {
        match self {
            Stop::Exit(_) | Stop::Fail(_) => 1,
            Stop::Status(s) => *s,
            Stop::NotPorted(_) => 2,
        }
    }
}

impl From<unityfs::Error> for Stop {
    fn from(e: unityfs::Error) -> Self {
        Stop::Fail(e.to_string())
    }
}

impl From<std::io::Error> for Stop {
    fn from(e: std::io::Error) -> Self {
        Stop::Fail(e.to_string())
    }
}

impl From<pkgzip::Error> for Stop {
    fn from(e: pkgzip::Error) -> Self {
        match e {
            pkgzip::Error::Exit(m) => Stop::Exit(m),
            e => Stop::Fail(e.to_string()),
        }
    }
}

fn io_at(p: impl AsRef<Path>) -> impl FnOnce(std::io::Error) -> Stop {
    move |e| {
        Stop::Fail(format!(
            "{e}: {}",
            pkgzip::py::str_repr(&p.as_ref().to_string_lossy())
        ))
    }
}

/// A planned file: where it goes (relative to --out), where from, what is done to it.
#[derive(Clone, Debug)]
pub struct Item {
    pub dst: String,
    pub src: String,
    pub act: String,
}

fn item(dst: String, src: String, act: &str) -> Item {
    Item {
        dst,
        src,
        act: act.to_string(),
    }
}

/// The run's state (the Python's args namespace and what check_source and plan add to it).
pub struct Ctx {
    pub args: Args,
    pub unity: Version,
    pub player_unity: Option<Version>,
    pub retarget: Option<String>,
    pub warnings: Vec<String>,
    pub runtime: Option<String>,
    pub plugins: Option<Survey>,
    pub plugin_entries: Json,
    pub loose_assets: Vec<String>,
    pub beside: Vec<String>,
    pub name: String,
    pub package: String,
    pub skipped: Vec<String>,
    pub dropped_modules: Vec<String>,
    pub inlined_ress: Vec<String>,
    /// The .resS split instead of inlined (ress_split.rs).
    pub split_ress: Vec<String>,
    /// Each archive's limit (pack.rs: 2 GiB - 1; tests: $PMU_ZIP_LIMIT) and the split's rule.
    pub zip_limit: u64,
    pub split: crate::ress_split::SplitRule,
    pub managed_names: BTreeSet<String>,
    pub gi_neon: usize,
    pub compute_kernels: Option<bool>,
    /// Hidden/VideoDecodeAndroid's path id in unity_builtin_extra (video.rs), and its conversion.
    pub video_shader_pid: Option<i64>,
    pub video_bt709: bool,
    pub db: Arc<ClassDb>,
    logf: Option<File>,
}

impl Ctx {
    pub fn opts(&self) -> unityfs::Options {
        unityfs::Options {
            db: Some(self.db.clone()),
            ..Default::default()
        }
    }

    /// print(msg), and the line in make_android_data.log with the time.
    pub fn log(&mut self, msg: &str) {
        println!("{msg}");
        if let Some(f) = &mut self.logf {
            let _ = writeln!(f, "{} {msg}", crate::clock::hms());
            let _ = f.flush();
        }
    }

    /// _retarget: a serialized file about to be saved, for the player of another release of the
    /// line (objects of changed layouts converted, the player's version). Returns notes.
    pub fn retarget_sf(&self, sf: &mut unityfs::SerializedFile) -> Result<Vec<String>, Stop> {
        let Some(rt) = self.retarget.clone() else {
            return Ok(Vec::new());
        };
        if sf.enable_type_tree {
            return Ok(Vec::new());
        }
        let src = String::from_utf8_lossy(&sf.unity_version).into_owned();
        let mut notes = retarget::migrate(sf, &src, &rt)?;
        notes.push(format!("version {src} -> {rt}"));
        sf.unity_version = rt.into_bytes();
        Ok(notes)
    }
}

fn is_file(p: &str) -> bool {
    Path::new(p).is_file()
}

fn is_dir(p: &str) -> bool {
    Path::new(p).is_dir()
}

fn exists(p: &str) -> bool {
    Path::new(p).exists()
}

fn lexists(p: &str) -> bool {
    fs::symlink_metadata(p).is_ok()
}

fn makedirs(d: &str) -> Result<(), Stop> {
    if d.is_empty() {
        return Ok(());
    }
    fs::create_dir_all(d).map_err(io_at(d))
}

// ------------------------------------------------------------------- file placement

/// dst = a hard link of src (allow_hardlink) or a copy (no clones on Linux).
pub fn clone(src: &str, dst: &str, allow_hardlink: bool) -> Result<&'static str, Stop> {
    makedirs(py::dirname(dst))?;
    if lexists(dst) {
        fs::remove_file(dst).map_err(io_at(dst))?;
    }
    if allow_hardlink && fs::hard_link(src, dst).is_ok() {
        return Ok("hardlink");
    }
    copyfile(src, dst)?;
    if let Ok(m) = fs::metadata(src) {
        let _ = fs::set_permissions(dst, m.permissions());
    }
    Ok("copy")
}

/// shutil.copyfile.
pub fn copyfile(src: &str, dst: &str) -> Result<(), Stop> {
    let mut i = File::open(src).map_err(io_at(src))?;
    let mut o = File::create(dst).map_err(io_at(dst))?;
    std::io::copy(&mut i, &mut o).map_err(io_at(dst))?;
    Ok(())
}

pub(crate) fn mv(src: &str, dst: &str) -> Result<&'static str, Stop> {
    makedirs(py::dirname(dst))?;
    fs::rename(src, dst).map_err(io_at(src))?;
    Ok("moved")
}

/// dst written whole through dst.tmp.
pub fn write_bytes(dst: &str, data: &[u8]) -> Result<(), Stop> {
    makedirs(py::dirname(dst))?;
    let tmp = format!("{dst}.tmp");
    fs::write(&tmp, data).map_err(io_at(&tmp))?;
    fs::rename(&tmp, dst).map_err(io_at(dst))?;
    Ok(())
}

fn consumable(ctx: &Ctx, src: &str, act: &str) -> bool {
    if !ctx.args.consume_src || !matches!(act, "link" | "sfpatch" | "bundle" | "sfinline") {
        return false;
    }
    let rel = paths::relpath(&realpath(src), &realpath(&ctx.args.src));
    !(rel.starts_with("..") || rel.starts_with("Managed/") || rel.starts_with("Mono/"))
}

fn save_state(path: &str, state: &Json) -> Result<(), Stop> {
    let tmp = format!("{path}.tmp");
    fs::write(&tmp, pyjson::dumps(state, Some(0))).map_err(io_at(&tmp))?;
    fs::rename(&tmp, path).map_err(io_at(path))?;
    Ok(())
}

/// dict(state, **newstate)
fn merged(state: &Json, newstate: &Json) -> Json {
    let mut m = state.clone();
    for (k, v) in newstate.as_obj().unwrap() {
        m.set(k, v.clone());
    }
    m
}

// ------------------------------------------------------------------- Mono config

/// str.splitlines(keepends=True).
fn splitlines_keep(t: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut start = 0;
    let mut it = t.char_indices().peekable();
    while let Some((i, c)) = it.next() {
        let end = match c {
            '\r' => {
                if let Some(&(_, '\n')) = it.peek() {
                    it.next();
                    i + 2
                } else {
                    i + 1
                }
            }
            '\n' | '\x0b' | '\x0c' | '\x1c' | '\x1d' | '\x1e' | '\u{85}' | '\u{2028}'
            | '\u{2029}' => i + c.len_utf8(),
            _ => continue,
        };
        out.push(&t[start..end]);
        start = end;
    }
    if start < t.len() {
        out.push(&t[start..]);
    }
    out
}

/// Mono's etc/mono/config for the Android player: Mac library names -> Linux ones, plus the
/// extra dllmaps.
pub fn mono_config_text(txt: &str, extra: &[(String, String)]) -> String {
    use regex::Regex;
    use std::sync::OnceLock;
    static RX: OnceLock<Vec<(Regex, &'static str)>> = OnceLock::new();
    let rx = RX.get_or_init(|| {
        [
            (r"libc\.dylib", "libc.so.6"),
            (r"libintl\.dylib", "libc.so.6"),
            (r"lib([\w.+-]+?)\.(\d+)\.dylib", "lib${1}.so.${2}"),
            (r"lib([\w.+-]+?)\.dylib", "lib${1}.so"),
        ]
        .into_iter()
        .map(|(r, to)| (Regex::new(&format!("target=\"{r}\"")).unwrap(), to))
        .collect()
    });
    let mut out = String::new();
    for l in splitlines_keep(txt) {
        if l.contains("os=\"osx\"") {
            out.push_str(l);
            continue;
        }
        let mut l = l.to_string();
        for (r, to) in rx {
            l = r
                .replace_all(&l, |c: &regex::Captures| {
                    let mut s = String::new();
                    c.expand(to, &mut s);
                    format!("target=\"{s}\"")
                })
                .into_owned();
        }
        out.push_str(&l);
    }
    let mut txt = out;
    let more = [("libmono.so.0".to_string(), "libmono.so".to_string())];
    for (dll, target) in extra.iter().chain(more.iter()) {
        let line = format!("\t<dllmap dll=\"{dll}\" target=\"{target}\" os=\"!windows\"/>\n");
        if !txt.contains(&format!("dll=\"{dll}\"")) {
            txt = txt.replace("</configuration>", &format!("{line}</configuration>"));
        }
    }
    txt
}

/// Text as Python's open(path).read() gives it: UTF-8, universal newlines.
fn read_text(path: &str) -> Result<String, Stop> {
    let b = fs::read(path).map_err(io_at(path))?;
    let t = String::from_utf8(b).map_err(|e| Stop::Fail(format!("UnicodeDecodeError: {e}")))?;
    Ok(t.replace("\r\n", "\n").replace('\r', "\n"))
}

fn mono_etc_dir(src: &str) -> String {
    let up = py::dirname(&abspath(src)).to_string();
    for d in [src.to_string(), up.clone(), py::dirname(&up).to_string()] {
        for m in ["Mono", "MonoBleedingEdge"] {
            let p = py::join(&py::join(&d, m), "etc");
            if is_dir(&p) {
                return p;
            }
        }
    }
    py::join(&py::join(src, "Mono"), "etc")
}

// ------------------------------------------------------------------- assemblies

/// {engine module: [type names]} the game's assemblies need from a module the Android player's
/// copy lacks but the desktop one has.
fn desktop_modules_needed(gmanaged: &str, amanaged: &str) -> Vec<(String, Vec<String>)> {
    use crate::clrmeta::{Assembly, TYPEREF};
    let mut out = Vec::new();
    if !(is_dir(gmanaged) && is_dir(amanaged)) {
        return out;
    }
    let mut wanted: Vec<(String, BTreeSet<String>)> = Vec::new();
    for n in listdir(gmanaged).unwrap_or_default() {
        if !n.ends_with(".dll") || ggm::is_engine_module(&n) {
            continue;
        }
        let Ok(a) = Assembly::open(py::join(gmanaged, &n)) else {
            continue;
        };
        for r in 1..=a.count(TYPEREF) {
            let Ok(scope) = a.typeref_scope(r) else { break };
            if let Some(s) =
                scope.filter(|s| s.starts_with("UnityEngine.") && s.ends_with("Module"))
            {
                let Ok(tn) = a.typeref_name(r) else { break };
                let k = format!("{s}.dll");
                match wanted.iter_mut().find(|(m, _)| *m == k) {
                    Some(e) => {
                        e.1.insert(tn);
                    }
                    None => wanted.push((k, BTreeSet::from([tn]))),
                }
            }
        }
    }
    let types_of = |p: &str| -> Option<BTreeSet<String>> {
        let a = Assembly::open(p).ok()?;
        Some(a.types().ok()?.iter().map(|t| t.full.clone()).collect())
    };
    for (m, names) in wanted {
        let (ap, gp) = (py::join(amanaged, &m), py::join(gmanaged, &m));
        if !(is_file(&ap) && is_file(&gp)) {
            continue;
        }
        let (Some(have), Some(desk)) = (types_of(&ap), types_of(&gp)) else {
            continue;
        };
        let missing: Vec<String> = names
            .into_iter()
            .filter(|t| !have.contains(t) && desk.contains(t))
            .collect();
        if !missing.is_empty() {
            out.push((m, missing));
        }
    }
    out
}

/// {assembly: [engine modules]}: assemblies the player's engine modules reference that are not
/// shipped.
pub fn missing_references(amanaged: &str, names: &BTreeSet<String>) -> Vec<(String, Vec<String>)> {
    use crate::clrmeta::Assembly;
    let mut have: BTreeSet<String> = names
        .iter()
        .filter(|n| n.to_lowercase().ends_with(".dll"))
        .map(|n| n[..n.len() - 4].to_lowercase())
        .collect();
    have.insert("mscorlib".into());
    let mut out: Vec<(String, Vec<String>)> = Vec::new();
    for n in listdir(amanaged).unwrap_or_default() {
        if !n.ends_with(".dll") {
            continue;
        }
        let Ok(refs) = Assembly::open(py::join(amanaged, &n)).and_then(|a| a.assembly_refs())
        else {
            continue;
        };
        for r in refs {
            if !have.contains(&r.to_lowercase()) {
                let user = n[..n.len() - 4].to_string();
                match out.iter_mut().find(|(x, _)| *x == r) {
                    Some(e) => e.1.push(user),
                    None => out.push((r, vec![user])),
                }
            }
        }
    }
    out
}

fn scripting_runtime(src: &str, unity: &Version) -> Result<Option<String>, Stop> {
    if unity.line() >= (2019, 2) {
        return Ok(Some(".NET 4.x (libmonobdwgc-2.0)".into()));
    }
    let level = dotnet_level(src);
    if let Some(l) = level {
        return Ok(Some(
            if l == 2 {
                ".NET 3.5 (libmono)"
            } else {
                ".NET 4.x (libmonobdwgc-2.0)"
            }
            .into(),
        ));
    }
    let etc = mono_etc_dir(src);
    let parent = py::basename(py::dirname(&etc)).to_string();
    Ok(match parent.as_str() {
        "MonoBleedingEdge" => Some(".NET 4.x (libmonobdwgc-2.0)".into()),
        "Mono" => Some(".NET 3.5 (libmono)".into()),
        _ => None,
    })
}

/// il2cpp.dotnet_level: 2 or 4 from the mscorlib the game ships, else from what Assembly-CSharp
/// references; None where the Python raises Unsupported.
fn dotnet_level(data: &str) -> Option<u8> {
    use crate::clrmeta::{Assembly, ASSEMBLYREF};
    let gm = py::join(data, "Managed");
    if let Ok(a) = Assembly::open(py::join(&gm, "mscorlib.dll")) {
        if let Ok(rows) = a.rows(0x20) {
            if let Some(r) = rows.first() {
                return Some(if r[1] == 2 { 2 } else { 4 });
            }
        }
    }
    if let Ok(b) = Assembly::open(py::join(&gm, "Assembly-CSharp.dll")) {
        if let Ok(rows) = b.rows(ASSEMBLYREF) {
            let mut refs: Vec<(String, u32)> = Vec::new();
            for r in rows.iter() {
                let Ok(s) = b.string(r[6]) else { return None };
                match refs.iter_mut().find(|(k, _)| *k == s) {
                    Some(e) => e.1 = r[0],
                    None => refs.push((s, r[0])),
                }
            }
            if refs.iter().any(|(k, _)| k == "netstandard") {
                return Some(4);
            }
            if let Some((_, v)) = refs.iter().find(|(k, _)| k == "mscorlib") {
                return Some(if *v == 2 { 2 } else { 4 });
            }
        }
    }
    None
}

// ------------------------------------------------------------------- the player package

fn version_in(text: &str) -> Option<String> {
    use regex::Regex;
    use std::sync::OnceLock;
    static R: OnceLock<Regex> = OnceLock::new();
    let r = R.get_or_init(|| Regex::new(r"\b((?:20\d\d|6\d\d\d|5)\.\d+\.\d+[abfp]\d+)\b").unwrap());
    r.captures(text).map(|c| c[1].to_string())
}

/// The Unity version of an Android player package.
pub fn player_version(pkg: &str) -> Option<Version> {
    let pj = py::join(pkg, "player.json");
    if is_file(&pj) {
        if let Ok(j) = fs::read(&pj)
            .map_err(|e| e.to_string())
            .and_then(|b| pyjson::loads_bytes(&b))
        {
            if let Some(v) = j.get("version").and_then(Json::as_str) {
                if let Ok(v) = Version::parse(v) {
                    return Some(v);
                }
            }
        }
    }
    let ivy = py::join(pkg, "ivy.xml");
    if is_file(&ivy) {
        let t = String::from_utf8_lossy(&fs::read(&ivy).unwrap_or_default()).into_owned();
        if let Some(i) = t.find("unityVersion=\"") {
            let rest = &t[i + 14..];
            if let Some(e) = rest.find('"') {
                if e > 0 {
                    if let Ok(v) = Version::parse(&rest[..e]) {
                        return Some(v);
                    }
                }
            }
        }
    }
    for name in ["modules.asset", "version.txt"] {
        let p = py::join(pkg, name);
        if is_file(&p) {
            let t = String::from_utf8_lossy(&fs::read(&p).unwrap_or_default()).into_owned();
            if let Some(v) = version_in(&t).and_then(|v| Version::parse(&v).ok()) {
                return Some(v);
            }
        }
    }
    for (abi, backend) in [
        ("armeabi-v7a", "mono"),
        ("arm64-v8a", "mono"),
        ("arm64-v8a", "il2cpp"),
        ("armeabi-v7a", "il2cpp"),
    ] {
        let lib = format!("{pkg}/Variations/{backend}/Release/Libs/{abi}/libunity.so");
        if is_file(&lib) {
            let d = fs::read(&lib).unwrap_or_default();
            let found = versions_nul(&d);
            if !found.is_empty() {
                // max(set(found), key=found.count): the most common, the first in the set's order on ties
                let mut uniq: Vec<&String> =
                    found.iter().collect::<BTreeSet<_>>().into_iter().collect();
                uniq.sort_by_key(|v| std::cmp::Reverse(found.iter().filter(|x| *x == *v).count()));
                if let Ok(v) = Version::parse(uniq[0]) {
                    return Some(v);
                }
            }
        }
    }
    let udr = pkg_default_resources(pkg);
    if is_file(&udr) {
        if let Ok(mut v) = UV::of_file(Path::new(&udr)) {
            v.exact = false;
            return Some(v);
        }
    }
    None
}

/// re.findall(VERSION_RE + b'\x00') over a binary.
fn versions_nul(d: &[u8]) -> Vec<String> {
    use regex::bytes::Regex;
    use std::sync::OnceLock;
    static R: OnceLock<Regex> = OnceLock::new();
    let r = R.get_or_init(|| {
        Regex::new(r"(?-u)\b((?:20\d\d|6\d\d\d|5)\.\d+\.\d+[abfp]\d+)\x00").unwrap()
    });
    r.captures_iter(d)
        .map(|c| String::from_utf8_lossy(&c[1]).into_owned())
        .collect()
}

fn pkg_default_resources(pkg: &str) -> String {
    for rel in [
        "Data/unity default resources",
        "Data/Resources/unity default resources",
    ] {
        let p = py::join(pkg, rel);
        if is_file(&p) {
            return p;
        }
    }
    py::join(pkg, "Data/unity default resources")
}

fn default_resources_path(unity: &Version) -> &'static str {
    if unity.line() >= DEFAULT_RESOURCES_IN_RESOURCES_FROM {
        "Resources/unity default resources"
    } else {
        "unity default resources"
    }
}

/// The Memory Advice files the player reads (Unity 2023.1), or none.
fn memory_advice(pkg: &str, il2cpp: bool) -> Vec<(String, String)> {
    let ma = py::join(pkg, "Apk/MemoryAdvice");
    if !is_dir(&ma) {
        return Vec::new();
    }
    let libs = format!(
        "{pkg}/Variations/{}/Release/Libs",
        if il2cpp { "il2cpp" } else { "mono" }
    );
    let mut uses = false;
    for abi in listdir(&libs).unwrap_or_default() {
        let p = format!("{libs}/{abi}/libunity.so");
        if is_file(&p) {
            uses = py::find(&fs::read(&p).unwrap_or_default(), b"realtime_features.json").is_some();
            break;
        }
    }
    if !uses {
        return Vec::new();
    }
    listdir(&ma)
        .unwrap_or_default()
        .into_iter()
        .map(|n| (n.clone(), py::join(&ma, &n)))
        .collect()
}

// ------------------------------------------------------------------- check_source

fn check_source(ctx: &mut Ctx) -> Result<(), Stop> {
    let src = ctx.args.src.clone();
    let uns = |m: String| Stop::Exit(m);
    if !is_file(&py::join(&src, "globalgamemanagers")) && is_file(&py::join(&src, "data.unity3d")) {
        return Err(uns(format!(
            "{src}: a compressed build (data.unity3d): unpack it first with `python -m pmunity unpack {src}`"
        )));
    }
    let v = UV::of_data(&src).map_err(|e| uns(e.0))?;
    ctx.unity = UV::check(v, ctx.args.any_unity_version).map_err(|e| uns(e.0))?;
    let gm = py::join(&src, "Managed");
    if !is_dir(&gm)
        || !listdir(&gm)
            .unwrap_or_default()
            .iter()
            .any(|n| n.ends_with(".dll"))
    {
        let il2cpp: Vec<&str> = [
            "il2cpp_data",
            "Il2CppData",
            "../GameAssembly.so",
            "../GameAssembly.dll",
            "../GameAssembly.dylib",
        ]
        .into_iter()
        .filter(|n| exists(&py::join(&src, n)))
        .collect();
        let why = if il2cpp.is_empty() {
            String::new()
        } else {
            format!(
                ": an IL2CPP build ({}), which the Mono Android player cannot run",
                il2cpp.join(", ")
            )
        };
        return Err(uns(format!(
            "{src}: no managed assemblies (Managed/*.dll){why}"
        )));
    }
    let pkg = ctx.args.pkg.clone().unwrap();
    ctx.player_unity = if is_dir(&pkg) {
        player_version(&pkg)
    } else {
        None
    };
    if let Some(p) = ctx.player_unity.clone() {
        let u = ctx.unity.clone();
        if p.line() != u.line() && !ctx.args.any_unity_version {
            return Err(uns(format!(
                "the data is Unity {u}, the Android player Unity {p}: use the Android player of Unity {}.{} (serialized data \
                 has no type trees: the player must read the same layouts)",
                u.major, u.minor
            )));
        }
        if p.exact && (p.patch, p.build) != (u.patch, u.build) {
            if ctx.args.data_version == "player" && p.line() == u.line() {
                let rt = p.to_string();
                let mut ch: Vec<String> = retarget::changed_classes(&ctx.db, &u.to_string(), &rt)
                    .into_iter()
                    .filter(|(c, _)| !(1000..100000).contains(c))
                    .map(|(_, n)| n)
                    .collect();
                ch.sort();
                let list = if ch.is_empty() {
                    "none".to_string()
                } else {
                    ch.join(", ")
                };
                ctx.warnings.push(format!(
                    "data of Unity {u}, Android player {p}: the data's serialized files are retargeted to {rt} (classes whose \
                     layout differs: {list})"
                ));
                ctx.retarget = Some(rt);
            } else {
                ctx.warnings.push(format!(
                    "data of Unity {u}, Android player {p}: same line, other patch (the player refuses serialized files of \
                     another release: --data-version player)"
                ));
            }
        }
    }
    ctx.runtime = scripting_runtime(&src, &ctx.unity)?;
    if ctx.args.il2cpp.is_some() {
        return Err(Stop::NotPorted("--il2cpp".into()));
    }
    if ctx.args.mono64 {
        if !is_dir(&py::join(&pkg, "Variations/il2cpp/Managed")) {
            return Err(uns(format!(
                "--mono64: {pkg} is no IL2CPP player package with its engine modules (Variations/il2cpp/Managed: `pmunity \
                 player fetch <version> --backend il2cpp --abi arm64-v8a`)"
            )));
        }
        ctx.runtime = Some(format!(
            "{} on the IL2CPP player (arm64-v8a, Mono for aarch64 Linux)",
            ctx.runtime
                .as_deref()
                .map_or("None".to_string(), str::to_string)
        ));
    }
    let survey = plugins::survey(&src).map_err(Stop::Fail)?;
    let port_map = match &ctx.args.plugin_map {
        Some(pm) => {
            let b = fs::read(pm).map_err(|e| uns(format!("plugins: {e}")))?;
            pyjson::loads_bytes(&b).map_err(|e| uns(format!("plugins: {e}")))?
        }
        None => Json::obj(),
    };
    ctx.plugin_entries =
        plugins::merge_map(&survey, &port_map).map_err(|e| uns(format!("plugins: {e}")))?;
    for line in plugins::summary(&survey) {
        ctx.warnings.push(format!("plugin {line}"));
    }
    ctx.loose_assets = match &ctx.args.loose_assets {
        Some(l) => l.clone(),
        None if survey.fmod.is_some() => vec!["*.bank".into()],
        None => Vec::new(),
    };
    ctx.plugins = Some(survey);
    Ok(())
}

// ------------------------------------------------------------------- plan

fn cmd_key(cmd: &str) -> Result<String, Stop> {
    let mut parts = vec![cmd.to_string()];
    for w in paths::shlex_split(cmd).map_err(|e| Stop::Fail(format!("ValueError: {e}")))? {
        if is_file(&w) {
            let m = fs::metadata(&w)?;
            parts.push((m.mtime() as i128 * 1_000_000_000 + m.mtime_nsec() as i128).to_string());
        }
    }
    Ok(parts.join("|"))
}

fn is_unityfs(p: &str) -> Result<bool, Stop> {
    use std::io::Read;
    let mut b = Vec::new();
    File::open(p)
        .map_err(io_at(p))?
        .take(8)
        .read_to_end(&mut b)?;
    Ok(b == b"UnityFS\0")
}

fn level_or_shared(n: &str) -> bool {
    let num = |s: &str| !s.is_empty() && s.bytes().all(|c| c.is_ascii_digit());
    n.strip_prefix("level").is_some_and(num)
        || n.strip_prefix("sharedassets")
            .and_then(|r| r.strip_suffix(".assets"))
            .is_some_and(num)
}

fn plan(ctx: &mut Ctx) -> Result<Vec<Item>, Stop> {
    let src = ctx.args.src.clone();
    let pkg = ctx.args.pkg.clone().unwrap();
    let name = ctx.name.clone();
    let data = format!("{name}/assets/bin/Data");
    let mut items = Vec::new();
    for n in listdir(&src).map_err(io_at(&src))? {
        let p = py::join(&src, &n);
        if !is_file(&p) || n.starts_with('.') {
            continue;
        }
        if n == "globalgamemanagers" {
            items.push(item(py::join(&data, &n), p, "ggm"));
        } else if n == "globalgamemanagers.assets" || n == "resources.assets" || level_or_shared(&n)
        {
            let inline = ctx.args.inline_textures && exists(&format!("{p}.resS"));
            items.push(item(
                py::join(&data, &n),
                p,
                if inline { "sfinline" } else { "sfpatch" },
            ));
        } else if n.ends_with(".resS") && ctx.args.inline_textures && exists(&p[..p.len() - 5]) {
            ctx.inlined_ress.push(n);
        } else if n.ends_with(".resS")
            || n.ends_with(".resource")
            || n == "boot.config"
            || DATA_JSON.contains(&n.as_str())
        {
            items.push(item(py::join(&data, &n), p, "link"));
        } else if ctx.args.root_file.contains(&n) {
            items.push(item(py::join(&name, &n), p, "link"));
        } else if ctx.args.data_path {
            items.push(item(format!("{name}/{}/{n}", ST::FOLDER), p, "link"));
        } else {
            ctx.skipped.push(n);
        }
    }
    let gi = py::join(&src, "GI");
    ctx.gi_neon = 0;
    for (r, _, mut fs) in paths::walk(&gi, true) {
        fs.sort();
        for f in &fs {
            if f.starts_with('.') {
                continue;
            }
            let s = py::join(&r, f);
            let dst = py::join(&py::join(&data, "GI"), &paths::relpath(&s, &gi));
            items.push(item(dst.clone(), s.clone(), "link"));
            if ctx.args.realtime_gi == "neon"
                && f.ends_with(".sse")
                && !fs.contains(&format!("{}.neo", &f[..f.len() - 4]))
            {
                items.push(item(format!("{}.neo", &dst[..dst.len() - 4]), s, "link"));
                ctx.gi_neon += 1;
            }
        }
    }
    if ctx.args.data_path {
        for (d, s, a) in ST::data_folder_items(&src, &format!("{name}/{}", ST::FOLDER))? {
            items.push(item(d, s, &a));
        }
    }
    for (n, path) in memory_advice(&pkg, ctx.args.il2cpp.is_some() || ctx.args.mono64) {
        items.push(item(format!("{name}/assets/{n}"), path, "link"));
        ctx.loose_assets.push(n);
    }
    let udr_dst = py::join(&data, default_resources_path(&ctx.unity));
    items.push(item(udr_dst, pkg_default_resources(&pkg), "link"));
    let bex = py::join(&src, "Resources/unity_builtin_extra");
    // the Android VideoPlayer's shader, made from the desktop one (video.rs)
    ctx.video_shader_pid = if ctx.args.video_shader && video::has_template(&bex) {
        video::path_id_for(&bex, ctx)?
    } else {
        None
    };
    ctx.video_bt709 = ctx.video_shader_pid.is_some() && video::wants_bt709(&src, ctx)?;
    let act = match (ctx.video_shader_pid, ctx.video_bt709) {
        (Some(_), true) => "sfvideo709",
        (Some(_), false) => "sfvideo",
        _ => "sfpatch",
    };
    items.push(item(
        py::join(&data, "Resources/unity_builtin_extra"),
        bex,
        act,
    ));
    let backend = if ctx.args.il2cpp.is_some() || ctx.args.mono64 {
        "il2cpp"
    } else {
        "mono"
    };
    let amanaged = format!("{pkg}/Variations/{backend}/Managed");
    let mut names: BTreeSet<String> = BTreeSet::new();
    // {basename: path}, the last of a name winning (a dict)
    let mut overrides: Vec<(String, String)> = Vec::new();
    let set_override = |o: &mut Vec<(String, String)>, k: String, v: String| match o
        .iter_mut()
        .find(|(x, _)| *x == k)
    {
        Some(e) => e.1 = v,
        None => o.push((k, v)),
    };
    for f in &ctx.args.managed_file {
        set_override(&mut overrides, py::basename(f).to_string(), abspath(f));
    }
    for (n, why) in desktop_modules_needed(&py::join(&src, "Managed"), &amanaged) {
        if !overrides.iter().any(|(x, _)| *x == n) {
            set_override(&mut overrides, n.clone(), format!("{src}/Managed/{n}"));
            let more = if why.len() > 4 { "..." } else { "" };
            ctx.warnings.push(format!(
                "{n}: the game's own (desktop) copy is shipped: its assemblies use types the Android one lacks ({}{more})",
                why.iter().take(4).cloned().collect::<Vec<_>>().join(", ")
            ));
        }
    }
    for n in listdir(&amanaged).map_err(io_at(&amanaged))? {
        if n.ends_with(".dll") {
            let from = match overrides.iter().position(|(x, _)| *x == n) {
                Some(i) => overrides.remove(i).1,
                None => py::join(&amanaged, &n),
            };
            items.push(item(format!("{data}/Managed/{n}"), from, "link"));
            names.insert(n);
        }
    }
    let gmanaged = py::join(&src, "Managed");
    let mut all: BTreeSet<String> = listdir(&gmanaged)
        .map_err(io_at(&gmanaged))?
        .into_iter()
        .collect();
    all.extend(overrides.iter().map(|(k, _)| k.clone()));
    for n in all {
        if !n.ends_with(".dll") {
            continue;
        }
        if ggm::is_engine_module(&n) {
            if !names.contains(&n) {
                ctx.dropped_modules.push(n);
            }
            continue;
        }
        let dst = format!("{data}/Managed/{n}");
        if let Some((_, o)) = overrides.iter().find(|(x, _)| *x == n) {
            items.push(item(dst, o.clone(), "link"));
        } else if let Some((_, cmd)) = ctx.args.assembly_patch.iter().rev().find(|(x, _)| *x == n) {
            let orig = py::join(ctx.args.game_managed.as_deref().unwrap_or(""), &n);
            let act = format!("patch:{n}:{}", cmd_key(cmd)?);
            items.push(item(dst, orig, &act));
        } else {
            items.push(item(dst, py::join(&gmanaged, &n), "link"));
        }
        names.insert(n);
    }
    ctx.managed_names = names.clone();
    for (r, users) in missing_references(&amanaged, &names) {
        let more = if users.len() > 3 { "..." } else { "" };
        ctx.warnings.push(format!(
            "{r}.dll: the Android engine modules reference it ({}{more}) and the game's Managed folder has none: their types \
             fail to load (\"Could not load file or assembly '{r}'\"); ship Unity's copy of the game's version (--managed-file)",
            users.iter().take(3).cloned().collect::<Vec<_>>().join(", ")
        ));
    }
    let metc = ctx
        .args
        .mono_etc
        .clone()
        .unwrap_or_else(|| mono_etc_dir(&src));
    for (r, _, fs) in paths::walk(&metc, false) {
        for n in fs {
            let s = py::join(&r, &n);
            let rel = paths::relpath(&s, &metc);
            let act = if rel == "mono/config" {
                "monoconfig"
            } else {
                "link"
            };
            items.push(item(format!("{data}/Managed/etc/{rel}"), s, act));
        }
    }
    let sa = py::join(&src, "StreamingAssets");
    for (r, _, mut fs) in paths::walk(&sa, true) {
        fs.sort();
        for n in fs {
            if n.starts_with('.') {
                continue;
            }
            let s = py::join(&r, &n);
            let rel = paths::relpath(&s, &sa);
            let act = if is_unityfs(&s)? {
                "bundle"
            } else if ST::is_catalog(&rel) && ST::catalog_needs_fix(&s) {
                "aacatalog"
            } else {
                "link"
            };
            items.push(item(format!("{name}/assets/{rel}"), s, act));
        }
    }
    let gd = py::dirname(&src).to_string();
    for n in listdir(&gd).map_err(io_at(&gd))? {
        if ctx.beside.iter().any(|g| paths::fnmatch(&n, g)) {
            items.push(item(n.clone(), py::join(&gd, &n), "link"));
        }
    }
    Ok(items)
}

// ------------------------------------------------------------------- the run

fn mtime_ns(m: &fs::Metadata) -> i128 {
    m.mtime() as i128 * 1_000_000_000 + m.mtime_nsec() as i128
}

fn opts_key(ctx: &Ctx) -> Json {
    let a = &ctx.args;
    let mut k: Vec<Json> = vec![
        a.platform_patch.as_str().into(),
        a.graphics_apis.as_str().into(),
        a.mt_rendering.as_str().into(),
        a.blit.as_str().into(),
        a.rewired_joysticks.into(),
        ctx.name.as_str().into(),
        a.async_upload_buffer.into(),
        a.inline_textures.into(),
    ];
    if !a.video_shader {
        k.push("no-video-shader".into());
    }
    if a.mono64 {
        k.push("mono64".into());
    }
    if a.swappy != "keep" && ctx.unity.line() >= ggm::SWAPPY_FROM {
        k.push(format!("swappy={}", a.swappy).into());
    }
    if let Some(r) = &ctx.retarget {
        k.push(format!("retarget={r}").into());
    }
    // (tests only: the knobs that change what is split, when set)
    for v in ["PMU_ZIP_LIMIT", "PMU_RESS_SPLIT"] {
        if let Ok(x) = std::env::var(v) {
            k.push(format!("{v}={x}").into());
        }
    }
    Json::List(k)
}

fn tuple3(a: &str, b: u32, c: &str) -> Json {
    Json::List(vec![a.into(), b.into(), c.into()])
}

/// pmu android-data's main: Ok, or how it stops.
pub fn main(argv: &[String]) -> Result<(), Stop> {
    let args = match crate::args::parse(argv) {
        Ok(Some(a)) => a,
        Ok(None) => {
            println!("{}", crate::args::USAGE);
            return Ok(());
        }
        Err(m) => {
            eprintln!("{m}");
            return Err(Stop::Status(2));
        }
    };
    run(args)
}

pub fn run(mut args: Args) -> Result<(), Stop> {
    if args.pkg.as_deref().is_none_or(str::is_empty) {
        return Err(Stop::Exit(
            "--pkg: the unpacked Unity Android player package is needed (or set PMUNITY_ANDROID_PLAYER)".into(),
        ));
    }
    if args.il2cpp.is_some() {
        let mut given = Vec::new();
        if args.asmpatch.is_some() {
            given.push("--asmpatch");
        }
        if !args.managed_file.is_empty() {
            given.push("--managed-file");
        }
        if !args.assembly_patch.is_empty() {
            given.push("--assembly-patch");
        }
        if !given.is_empty() {
            return Err(Stop::Exit(format!(
                "{} with --il2cpp: the assemblies are compiled into libil2cpp.so, so these go to `pmunity il2cpp` (its options \
                 of the same names), not to android-data",
                given.join(", ")
            )));
        }
    }
    let name = match &args.name {
        Some(n) => n.clone(),
        None => {
            let base = py::basename(&py::normpath(&args.src)).to_string();
            match base.strip_suffix("_Data") {
                Some(b) => b.to_string(),
                None => {
                    let a = abspath(&args.src);
                    py::basename(py::dirname(py::dirname(py::dirname(&a)))).replace(".app", "")
                }
            }
        }
    };
    let package = args.package.clone().unwrap_or_else(|| {
        format!(
            "com.pmunity.{}",
            name.to_lowercase()
                .chars()
                .filter(|c| c.is_ascii_lowercase() || c.is_ascii_digit())
                .collect::<String>()
        )
    });
    let beside = args
        .beside
        .clone()
        .unwrap_or_else(|| vec!["goggame-*.info".into()]);
    if !args.assembly_patch.is_empty() && args.game_managed.is_none() {
        return Err(Stop::Exit("--assembly-patch needs --game-managed".into()));
    }
    let lname = name.to_lowercase();
    if [".apk", ".obb", ".zip", ".jar"]
        .iter()
        .any(|e| lname.ends_with(e))
    {
        return Err(Stop::Exit(
            "--name must not end in .apk/.obb/.zip/.jar: the player would try to open paths under it as a zip".into(),
        ));
    }
    args.src = abspath(&args.src);
    args.pkg = Some(abspath(args.pkg.as_deref().unwrap()));
    args.out = abspath(&args.out);
    let real_out = realpath(&args.out);
    for ro in std::iter::once(args.src.clone()).chain(args.game_managed.clone()) {
        if real_out.starts_with(&realpath(&ro)) {
            return Err(Stop::Exit(format!("refusing to write inside {ro}")));
        }
    }
    if args.streaming_assets != "obb" {
        if let Some(c) = &args.asmpatch {
            args.asmpatch = Some(ST::asmpatch_command(c, &args.streaming_assets));
        }
    }
    if args.data_path {
        if let Some(c) = &mut args.asmpatch {
            c.push_str(" --with=data-path");
        }
    }
    let db = Arc::new(ClassDb::default_db().map_err(|e| Stop::Fail(e.to_string()))?);
    // (tests only: a lower archive limit, the split's part size)
    let zip_limit = crate::pack::limit_from(std::env::var("PMU_ZIP_LIMIT").ok().as_deref())
        .map_err(Stop::Exit)?;
    let split = crate::ress_split::SplitRule::new(
        zip_limit,
        std::env::var("PMU_RESS_SPLIT").ok().as_deref(),
    )
    .map_err(Stop::Exit)?;
    let mut ctx = Ctx {
        args,
        unity: Version::parse("0.0.0").unwrap(),
        player_unity: None,
        retarget: None,
        warnings: Vec::new(),
        runtime: None,
        plugins: None,
        plugin_entries: Json::obj(),
        loose_assets: Vec::new(),
        beside,
        name,
        package,
        skipped: Vec::new(),
        dropped_modules: Vec::new(),
        inlined_ress: Vec::new(),
        split_ress: Vec::new(),
        zip_limit,
        split,
        managed_names: BTreeSet::new(),
        gi_neon: 0,
        compute_kernels: None,
        video_shader_pid: None,
        video_bt709: false,
        db,
        logf: None,
    };
    check_source(&mut ctx)?;
    let out = ctx.args.out.clone();
    makedirs(&out)?;
    let t0 = std::time::Instant::now();
    let logp = py::join(&out, "make_android_data.log");
    ctx.logf = Some(
        File::options()
            .append(true)
            .create(true)
            .open(&logp)
            .map_err(io_at(&logp))?,
    );
    let mut line = format!("data: Unity {}", ctx.unity);
    if let Some(p) = &ctx.player_unity {
        line.push_str(&format!("; Android player: Unity {p}"));
    }
    if let Some(r) = &ctx.runtime {
        line.push_str(&format!("; scripting runtime: {r}"));
    }
    ctx.log(&line);
    for w in ctx.warnings.clone() {
        ctx.log(&format!("WARNING: {w}"));
    }
    let statep = py::join(&out, ".android_data_state.json");
    let mut state = Json::obj();
    let mut placed_before: BTreeSet<String> = BTreeSet::new();
    if exists(&statep) {
        let loaded = fs::read(&statep)
            .ok()
            .and_then(|b| pyjson::loads_bytes(&b).ok());
        if let Some(s @ Json::Obj(_)) = loaded {
            placed_before = s
                .as_obj()
                .unwrap()
                .iter()
                .filter(|(k, _)| !k.starts_with('_'))
                .map(|(k, _)| k.clone())
                .collect();
            let fresh = ctx.args.force
                || s.get("_version") != Some(&Json::Int(STATE_VERSION))
                || s.get("_opts") != Some(&opts_key(&ctx));
            state = if fresh { Json::obj() } else { s };
        }
    }
    let asmpatch_notes = match ctx.args.asmpatch.clone() {
        Some(c) => run_asmpatch(&mut ctx, &c)?,
        None => Vec::new(),
    };
    if ctx.args.data_path && ctx.args.asmpatch.is_none() {
        ctx.log(
            "WARNING: --data-path without --asmpatch: Game_Data/ is beside the APK, but the game's code still reads \
             Application.dataPath (asmpatch data-path)",
        );
    }
    if ctx.args.streaming_assets != "obb" && ctx.args.asmpatch.is_none() {
        let m = format!(
            "WARNING: --streaming-assets {} without --asmpatch: the files are beside the APK, but the game's code still reads the \
             jar path (asmpatch {})",
            ctx.args.streaming_assets,
            ST::patch_of(&ctx.args.streaming_assets).unwrap_or("")
        );
        ctx.log(&m);
    }
    let items = plan(&mut ctx)?;
    let crc = items.iter().any(|i| i.act == "aacatalog");
    // source CRC -> patched CRC of every bundle placed (a dict: insertion order, last value)
    let mut crcs: Vec<(u32, u32)> = Vec::new();
    let put_crc =
        |crcs: &mut Vec<(u32, u32)>, k: u32, v: u32| match crcs.iter_mut().find(|(x, _)| *x == k) {
            Some(e) => e.1 = v,
            None => crcs.push((k, v)),
        };
    if let Some(Json::Obj(cs)) = state.get("_crc") {
        for (_, v) in cs {
            if let Some(l) = v.as_list() {
                if let (Some(a), Some(b)) = (
                    l.first().and_then(Json::as_int),
                    l.get(1).and_then(Json::as_int),
                ) {
                    put_crc(&mut crcs, a as u32, b as u32);
                }
            }
        }
    }
    ctx.log(&format!(
        "{} files planned from {}",
        items.len(),
        ctx.args.src
    ));
    let mut stats: Vec<(String, i64)> = Vec::new();
    let bump =
        |stats: &mut Vec<(String, i64)>, k: &str| match stats.iter_mut().find(|(x, _)| x == k) {
            Some(e) => e.1 += 1,
            None => stats.push((k.to_string(), 1)),
        };
    let mut report = Json::obj();
    report.set("bundles", Json::obj());
    report.set("files", Json::obj());
    report.set(
        "notes",
        Json::List(
            asmpatch_notes
                .iter()
                .map(|n| Json::Str(format!("asmpatch: {n}")))
                .collect(),
        ),
    );
    let mut newstate = Json::obj();
    newstate.set("_version", Json::Int(STATE_VERSION));
    newstate.set("_opts", opts_key(&ctx));
    let mut kept: BTreeSet<String> = BTreeSet::new();
    let consume = ctx.args.consume_src;
    for (k, it) in items.iter().enumerate() {
        let (dst_rel, src, act) = (&it.dst, &it.src, it.act.as_str());
        let dst = py::join(&out, dst_rel);
        if !exists(src) && exists(&dst) && (state.get(dst_rel).is_some() || consume) {
            newstate.set(
                dst_rel,
                state
                    .get(dst_rel)
                    .cloned()
                    .unwrap_or_else(|| "consumed".into()),
            );
            bump(&mut stats, "unchanged");
            continue;
        }
        let st = fs::metadata(src).map_err(io_at(src))?;
        let key = Json::List(vec![st.len().into(), Json::Int(mtime_ns(&st)), act.into()]);
        if act == "aacatalog" {
            newstate.set(dst_rel, key);
            continue;
        }
        if state.get(dst_rel) == Some(&key) && exists(&dst) {
            newstate.set(dst_rel, key.clone());
            let prev: Vec<String> = state
                .get("_kept")
                .and_then(|k| k.get(dst_rel))
                .and_then(Json::as_list)
                .map(|l| l.iter().map(Json::py_str).collect())
                .unwrap_or_default();
            for kr in prev {
                push_kept(&mut newstate, dst_rel, &kr);
                newstate.set(&kr, state.get(&kr).cloned().unwrap_or_else(|| key.clone()));
                kept.insert(kr);
            }
            bump(&mut stats, "unchanged");
            continue;
        }
        let mv_ = consumable(&ctx, src, act);
        let crc0 = if crc && act == "bundle" {
            Some(ST::bundle_crc(Path::new(src))?)
        } else {
            None
        };
        let how: String;
        if act == "link"
            || (matches!(act, "sfpatch" | "bundle") && ctx.args.platform_patch == "none")
        {
            how = if mv_ {
                mv(src, &dst)?
            } else {
                clone(src, &dst, true)?
            }
            .into();
        } else if act == "sfpatch" && ctx.retarget.is_some() {
            let p = if mv_ { src.clone() } else { dst.clone() };
            let mut h = String::new();
            if !mv_ {
                h = clone(src, &dst, false)?.into();
            }
            let note = retarget::retarget_file(
                Path::new(&p),
                &ctx.unity.to_string(),
                ctx.retarget.as_deref().unwrap(),
                ANDROID,
                &ctx.db,
            )?;
            report.get_mut("files").unwrap().set(dst_rel, note.into());
            if mv_ {
                h = mv(src, &dst)?.into();
            }
            how = h;
        } else if act == "sfpatch" {
            let old;
            if mv_ {
                old = unityfs::platform::patch_sf_platform(Path::new(src), ANDROID)?;
                how = mv(src, &dst)?.into();
            } else {
                how = clone(src, &dst, false)?.into();
                old = unityfs::platform::patch_sf_platform(Path::new(&dst), ANDROID)?;
            }
            report
                .get_mut("files")
                .unwrap()
                .set(dst_rel, format!("platform {old} -> {ANDROID}").into());
        } else if let Some(rest) = act.strip_prefix("patch:") {
            let pname = rest.split(':').next().unwrap_or("").to_string();
            let notes = assembly_patch(&ctx, &pname, &dst)?;
            if let Some(Json::List(l)) = report.get_mut("notes") {
                l.push(Json::Str(format!("{pname}: {notes}")));
            }
            how = "patched".into();
        } else if act == "sfinline" {
            match crate::inline::inline_serialized(&ctx, src, &dst, mv_)? {
                crate::inline::Placed::Inlined(n, still) => {
                    let mut note = format!("platform -> {ANDROID}, {n} textures inlined");
                    how = "inlined".into();
                    let mut still_sorted: Vec<&String> = still.iter().collect();
                    still_sorted.sort();
                    for name in still_sorted {
                        let sd = py::dirname(src);
                        let (s_res, d_res) =
                            (py::join(sd, name), py::join(py::dirname(&dst), name));
                        if mv_ {
                            mv(&s_res, &d_res)?;
                        } else {
                            clone(&s_res, &d_res, true)?;
                        }
                        let kr = py::join(py::dirname(dst_rel), name);
                        kept.insert(kr.clone());
                        newstate.set(&kr, key.clone());
                        push_kept(&mut newstate, dst_rel, &kr);
                        note.push_str(&format!("; {name} kept (still read by meshes)"));
                    }
                    report.get_mut("files").unwrap().set(dst_rel, note.into());
                    if mv_ {
                        fs::remove_file(src).map_err(io_at(src))?;
                        let res = format!("{src}.resS");
                        if exists(&res) && !still.contains(&format!("{}.resS", py::basename(src))) {
                            fs::remove_file(&res).map_err(io_at(&res))?;
                        }
                    }
                }
                crate::inline::Placed::Split(inlined, sp) => {
                    how = "split".into();
                    let file = py::basename(src).to_string();
                    let sd = py::dirname(dst_rel).to_string();
                    for (name, _) in &sp.parts {
                        let kr = py::join(&sd, name);
                        kept.insert(kr.clone());
                        newstate.set(&kr, key.clone());
                        push_kept(&mut newstate, dst_rel, &kr);
                    }
                    let sz = |n: u64| {
                        if n >= 1 << 30 {
                            format!("{:.2} GiB", n as f64 / (1u64 << 30) as f64)
                        } else {
                            format!("{:.1} MiB", n as f64 / (1u64 << 20) as f64)
                        }
                    };
                    let parts = sp
                        .parts
                        .iter()
                        .map(|(n, z)| format!("{n} ({})", sz(*z)))
                        .collect::<Vec<_>>()
                        .join(", ");
                    let note = format!(
                        "platform -> {ANDROID}, textures not inlined ({} so: no archive holds it whole): {file}.resS kept, split at stream \
                         boundaries into {} parts of at most {}: {parts}; {} textures and {} meshes stream from them ({} moved to a later part)",
                        sz(inlined),
                        sp.parts.len(),
                        sz(ctx.split.part),
                        sp.textures,
                        sp.meshes,
                        sp.moved
                    );
                    report
                        .get_mut("files")
                        .unwrap()
                        .set(dst_rel, note.clone().into());
                    let mut m = format!("  {dst_rel}: {note}");
                    if sp.unread > 0 {
                        m.push_str(&format!(
                            " ({} bytes of the .resS read by none of them, carried along)",
                            sp.unread
                        ));
                    }
                    ctx.log(&m);
                    if ctx.unity.line() < crate::ress_split::STREAMS_FROM {
                        let w = format!(
                            "{dst_rel}: its textures stream from {file}.resS (too big inlined); Unity {}.{}'s Android player is not known to \
                             load streamed textures (2017.4: an asynchronous load of one never finishes)",
                            ctx.unity.line().0,
                            ctx.unity.line().1
                        );
                        ctx.log(&format!("WARNING: {w}"));
                        ctx.warnings.push(w);
                    }
                    ctx.split_ress.push(format!("{file}.resS"));
                }
            }
        } else if act == "bundle" && ctx.args.inline_textures && crate::inline::has_ress_node(src)?
        {
            let (n, dropped) = crate::inline::inline_bundle(&ctx, src, &dst)?;
            let list = format!(
                "[{}]",
                dropped
                    .iter()
                    .map(|d| py::str_repr(d))
                    .collect::<Vec<_>>()
                    .join(", ")
            );
            report.get_mut("bundles").unwrap().set(
                py::basename(&dst),
                format!("{n} textures inlined, dropped {list}").into(),
            );
            how = "inlined".into();
            if mv_ {
                fs::remove_file(src).map_err(io_at(src))?;
            }
        } else if act == "bundle" {
            let p = if mv_ { src.clone() } else { dst.clone() };
            let mut h = String::new();
            if !mv_ {
                h = clone(src, &dst, false)?.into();
            }
            let res: Vec<Json> =
                match unityfs::platform::patch_bundle_platform(Path::new(&p), ANDROID) {
                    Ok(r) => r.into_iter().map(|(n, o, m)| tuple3(&n, o, m)).collect(),
                    Err(
                        e @ (unityfs::Error::Format(_)
                        | unityfs::Error::Unsupported(_)
                        | unityfs::Error::Value(_)),
                    ) => ST::repack_bundle(
                        Path::new(&p),
                        ANDROID,
                        &e.to_string(),
                        Some(ctx.db.clone()),
                    )?
                    .into_iter()
                    .map(|(n, o, m)| tuple3(&n, o, &m))
                    .collect(),
                    Err(e) => return Err(e.into()),
                };
            if mv_ {
                h = mv(src, &dst)?.into();
            }
            how = h;
            let rewrite = res.iter().any(|r| {
                r.as_list().and_then(|l| l.get(2)).and_then(Json::as_str) == Some("rewrite")
            });
            report
                .get_mut("bundles")
                .unwrap()
                .set(py::basename(&dst), Json::List(res));
            if rewrite {
                ctx.log(&format!(
                    "  {dst_rel}: bundle rewritten (no same-size encoding of the patched block)"
                ));
            }
        } else if act == "sfvideo" || act == "sfvideo709" {
            let mut f = unityfs::DataFile::open(src, &ctx.opts())?;
            let pid = video::add_to_builtin_extra(&mut f.sf, &mut ctx, act == "sfvideo709")?;
            if pid != ctx.video_shader_pid {
                let s = |p: Option<i64>| p.map_or("None".to_string(), |p| p.to_string());
                return Err(Stop::Exit(format!(
                    "video: shader got path id {}, globalgamemanagers maps {}",
                    s(pid),
                    s(ctx.video_shader_pid)
                )));
            }
            f.sf.target_platform = ANDROID as i32;
            let rt = ctx.retarget_sf(&mut f.sf)?;
            write_bytes(&dst, &f.sf.save()?)?;
            let mut note = format!(
                "platform -> {ANDROID}, {} added (path id {})",
                video::NAME,
                pid.unwrap()
            );
            for n in rt {
                note.push_str(&format!("; {n}"));
            }
            report.get_mut("files").unwrap().set(dst_rel, note.into());
            how = "video".into();
        } else if act == "ggm" {
            let (data, notes) = ggm::edit(Path::new(src), &mut ctx)?;
            write_bytes(&dst, &data)?;
            if let Some(Json::List(l)) = report.get_mut("notes") {
                l.extend(notes.into_iter().map(Json::Str));
            }
            how = "unitypy".into();
        } else if act == "monoconfig" {
            let extra: Vec<(String, String)> = ctx
                .args
                .dllmap
                .iter()
                .map(|d| match d.split_once('=') {
                    Some((a, b)) => Ok((a.to_string(), b.to_string())),
                    None => Err(Stop::Fail(
                        "ValueError: not enough values to unpack (expected 2, got 1)".into(),
                    )),
                })
                .collect::<Result<_, _>>()?;
            let txt = mono_config_text(&read_text(src)?, &extra);
            write_bytes(&dst, txt.as_bytes())?;
            how = "written".into();
        } else {
            return Err(Stop::Fail(format!("ValueError: {act}")));
        }
        bump(&mut stats, &how);
        newstate.set(dst_rel, key);
        if mv_ || (k + 1) % 50 == 0 {
            save_state(&statep, &merged(&state, &newstate))?;
        }
        if let Some(c0) = crc0 {
            let c1 = ST::bundle_crc(Path::new(&dst))?;
            put_crc(&mut crcs, c0, c1);
            set_crc(&mut newstate, dst_rel, c0, c1);
        }
        if (k + 1) % 100 == 0 {
            let m = format!(
                "  {}/{} ({:.0} s)",
                k + 1,
                items.len(),
                t0.elapsed().as_secs_f64()
            );
            ctx.log(&m);
        }
    }
    for it in &items {
        if it.act == "aacatalog" {
            let data = fs::read(&it.src).map_err(io_at(&it.src))?;
            let (data, n, n_paths) = ST::fix_catalog(&it.src, &data, &crcs).map_err(Stop::Fail)?;
            write_bytes(&py::join(&out, &it.dst), &data)?;
            report.get_mut("files").unwrap().set(
                &it.dst,
                format!("Addressables catalog: {n} bundle CRCs updated, {n_paths} Windows paths")
                    .into(),
            );
            let more = if n_paths > 0 {
                format!("; {n_paths} bundle paths with backslashes (a Windows build) now with /")
            } else {
                String::new()
            };
            ctx.log(&format!(
                "  {}: {n} bundle CRCs follow the patched bundles{more}",
                it.dst
            ));
        }
    }
    let mut planned: BTreeSet<String> = items.iter().map(|i| i.dst.clone()).collect();
    planned.extend(kept.iter().cloned());
    for d in &placed_before {
        if !planned.contains(d) {
            let p = py::join(&out, d);
            if consume && state.get(d).is_some() && lexists(&p) {
                newstate.set(d, state.get(d).unwrap().clone());
                continue;
            }
            if lexists(&p) {
                fs::remove_file(&p).map_err(io_at(&p))?;
                ctx.log(&format!("removed stale {d}"));
            }
        }
    }
    if let Some(Json::Obj(old)) = state.get("_crc") {
        let newcrc: Vec<String> = newstate
            .get("_crc")
            .and_then(Json::as_obj)
            .map(|o| o.iter().map(|(k, _)| k.clone()).collect())
            .unwrap_or_default();
        let keep: Vec<(String, Json)> = old
            .iter()
            .filter(|(d, _)| newstate.get(d).is_some() && !newcrc.contains(d))
            .cloned()
            .collect();
        if !keep.is_empty() {
            if newstate.get("_crc").is_none() {
                newstate.set("_crc", Json::obj());
            }
            let c = newstate.get_mut("_crc").unwrap();
            for (d, v) in keep {
                c.set(&d, v);
            }
        }
    }
    save_state(&statep, &newstate)?;
    let stats_repr = format!(
        "{{{}}}",
        stats
            .iter()
            .map(|(k, v)| format!("{}: {v}", py::str_repr(k)))
            .collect::<Vec<_>>()
            .join(", ")
    );
    let mut line = format!(
        "placed: {stats_repr}; skipped source files: {}; engine modules not shipped: {}; .resS moved into their serialized files: {}",
        list_repr(&ctx.skipped),
        list_repr(&ctx.dropped_modules),
        ctx.inlined_ress.len() - ctx.split_ress.len()
    );
    if !ctx.split_ress.is_empty() {
        line.push_str(&format!(
            "; .resS kept and split (their files too big for one archive inlined): {}",
            list_repr(&ctx.split_ress)
        ));
    }
    ctx.log(&line);
    if ctx.gi_neon > 0 {
        let m = format!(
            "realtime GI: {} SSE files also under their NEON names (.neo)",
            ctx.gi_neon
        );
        ctx.log(&m);
        if let Some(Json::List(l)) = report.get_mut("notes") {
            l.push(m.into());
        }
    }
    report.set(
        "stats",
        Json::Obj(
            stats
                .iter()
                .map(|(k, v)| (k.clone(), Json::Int(*v as i128)))
                .collect(),
        ),
    );
    let mut unity = Json::obj();
    unity.set("data", ctx.unity.to_string().into());
    unity.set(
        "player",
        ctx.player_unity.as_ref().map(|p| p.to_string()).into(),
    );
    unity.set("scripting_runtime", ctx.runtime.clone().into());
    unity.set("warnings", ctx.warnings.clone().into());
    report.set("unity", unity);
    report.set("skipped", ctx.skipped.clone().into());
    report.set("dropped_modules", ctx.dropped_modules.clone().into());
    let survey = ctx.plugins.take().unwrap();
    report.set("plugins", plugins::report(&survey, &ctx.plugin_entries));
    write_plugin_map(&mut ctx, &survey, &out)?;
    if !ctx.args.no_verify {
        let v = crate::verify::verify(&mut ctx, &items)?;
        report.set("verify", v);
    }
    if ctx.args.pack != "none" {
        let p = crate::pack::pack(&mut ctx, &survey)?;
        report.set("pack", p);
    }
    let rp = py::join(&out, "android_data_report.json");
    fs::write(&rp, pyjson::dumps(&report, Some(1))).map_err(io_at(&rp))?;
    let m = format!(
        "done in {:.0} s -> {}",
        t0.elapsed().as_secs_f64(),
        py::join(&out, &ctx.name)
    );
    ctx.log(&m);
    Ok(())
}

fn push_kept(newstate: &mut Json, dst_rel: &str, kr: &str) {
    if newstate.get("_kept").is_none() {
        newstate.set("_kept", Json::obj());
    }
    let k = newstate.get_mut("_kept").unwrap();
    if k.get(dst_rel).is_none() {
        k.set(dst_rel, Json::List(Vec::new()));
    }
    if let Some(Json::List(l)) = k.get_mut(dst_rel) {
        l.push(kr.into());
    }
}

fn set_crc(newstate: &mut Json, dst_rel: &str, a: u32, b: u32) {
    if newstate.get("_crc").is_none() {
        newstate.set("_crc", Json::obj());
    }
    newstate
        .get_mut("_crc")
        .unwrap()
        .set(dst_rel, Json::List(vec![a.into(), b.into()]));
}

pub fn list_repr(v: &[String]) -> String {
    format!(
        "[{}]",
        v.iter()
            .map(|s| py::str_repr(s))
            .collect::<Vec<_>>()
            .join(", ")
    )
}

/// The --assembly-patch command for one assembly, run with {managed} and {out}: the assembly it
/// writes placed at dst. Returns the note ("ok" lines of its output).
fn assembly_patch(ctx: &Ctx, name: &str, dst: &str) -> Result<String, Stop> {
    let cmd = ctx
        .args
        .assembly_patch
        .iter()
        .rev()
        .find(|(x, _)| x == name)
        .unwrap()
        .1
        .clone();
    let td = crate::paths::temp_dir()?;
    let res = (|| -> Result<String, Stop> {
        let gm = ctx.args.game_managed.clone().unwrap_or_default();
        let words: Vec<String> = paths::shlex_split(&cmd)
            .map_err(|e| Stop::Fail(format!("ValueError: {e}")))?
            .into_iter()
            .map(|w| w.replace("{managed}", &gm).replace("{out}", &td))
            .collect();
        let r = std::process::Command::new(&words[0])
            .args(&words[1..])
            .stdin(std::process::Stdio::inherit())
            .output()
            .map_err(|e| Stop::Fail(format!("{e}: {}", py::str_repr(&words[0]))))?;
        let stdout = String::from_utf8_lossy(&r.stdout).into_owned();
        let stderr = String::from_utf8_lossy(&r.stderr).into_owned();
        if !r.status.success() {
            return Err(Stop::Exit(format!(
                "assembly patch for {name} failed:\n{stdout}{stderr}"
            )));
        }
        let p = py::join(&td, name);
        let data = fs::read(&p).map_err(io_at(&p))?;
        write_bytes(dst, &data)?;
        Ok(crate::pack::splitlines(&stdout)
            .into_iter()
            .filter(|l| l.starts_with("ok"))
            .map(|l| l.chars().skip(5).collect::<String>())
            .collect::<Vec<_>>()
            .join(", "))
    })();
    let _ = fs::remove_dir_all(&td);
    res
}

/// --asmpatch: tools/asmpatch on the game's Managed folder; the assemblies it changes are
/// shipped like --managed-file ones. Returns its ok/done/FAIL lines.
fn run_asmpatch(ctx: &mut Ctx, cmd: &str) -> Result<Vec<String>, Stop> {
    let src = ctx
        .args
        .game_managed
        .clone()
        .unwrap_or_else(|| py::join(&ctx.args.src, "Managed"));
    let work = py::join(&ctx.args.out, ".asmpatch");
    let _ = fs::remove_dir_all(&work);
    let mut words = paths::shlex_split(cmd).map_err(|e| Stop::Fail(format!("ValueError: {e}")))?;
    words.push(src);
    words.push(work.clone());
    let r = std::process::Command::new(&words[0])
        .args(&words[1..])
        .output()
        .map_err(|e| Stop::Fail(format!("{e}: {}", py::str_repr(&words[0]))))?;
    let stdout = String::from_utf8_lossy(&r.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&r.stderr).into_owned();
    let lines = crate::pack::splitlines(&stdout);
    for l in &lines {
        ctx.log(&format!("asmpatch: {l}"));
    }
    if !r.status.success() {
        let code = r
            .status
            .code()
            .map_or_else(|| "-1".to_string(), |c| c.to_string());
        return Err(Stop::Exit(format!(
            "asmpatch failed (exit {code}):\n{stdout}{stderr}"
        )));
    }
    let written: Vec<String> = if is_dir(&work) {
        listdir(&work)?
            .into_iter()
            .filter(|n| n.ends_with(".dll"))
            .collect()
    } else {
        Vec::new()
    };
    let mf: BTreeSet<&str> = ctx
        .args
        .managed_file
        .iter()
        .map(|f| py::basename(f))
        .collect();
    let clash: Vec<String> = written
        .iter()
        .filter(|n| ctx.args.assembly_patch.iter().any(|(x, _)| x == *n) || mf.contains(n.as_str()))
        .cloned()
        .collect();
    if !clash.is_empty() {
        return Err(Stop::Exit(format!(
            "asmpatch changed {}, which --assembly-patch/--managed-file also provide: run asmpatch on that assembly inside \
             the patch command instead",
            clash.join(", ")
        )));
    }
    for n in written {
        ctx.args.managed_file.push(py::join(&work, &n));
    }
    Ok(lines
        .into_iter()
        .filter(|l| l.starts_with("ok") || l.starts_with("done") || l.starts_with("FAIL"))
        .map(str::to_string)
        .collect())
}

/// plugins.map beside the APK (and in the out folder).
pub fn write_plugin_map(ctx: &mut Ctx, survey: &Survey, where_: &str) -> Result<(), Stop> {
    let txt = plugins::map_text(survey, &ctx.plugin_entries)
        .map_err(|e| Stop::Fail(format!("ValueError: {e}")))?;
    let p = py::join(where_, "plugins.map");
    fs::write(&p, &txt).map_err(io_at(&p))?;
    let n = txt.lines().filter(|l| l.starts_with("map ")).count();
    let s = txt.lines().filter(|l| l.starts_with("sig ")).count();
    ctx.log(&format!(
        "plugins.map: {n} libraries, {s} float signatures -> {where_}"
    ));
    Ok(())
}

/// Whether the game's serialized files hold compute shaders with GLES3 kernels.
pub fn has_compute_kernels(ctx: &Ctx, src: &str) -> Result<bool, Stop> {
    let mut names: Vec<String> = listdir(src)
        .map_err(io_at(src))?
        .into_iter()
        .filter(|n| {
            n.ends_with(".assets")
                || n.strip_prefix("level")
                    .is_some_and(|r| !r.is_empty() && r.bytes().all(|c| c.is_ascii_digit()))
        })
        .collect();
    names.push("Resources/unity_builtin_extra".into());
    for n in names {
        let p = py::join(src, &n);
        if !is_file(&p) || is_unityfs(&p).unwrap_or(false) {
            continue;
        }
        let Ok(f) = unityfs::DataFile::open(&p, &ctx.opts()) else {
            continue;
        };
        for o in &f.sf.objects {
            if f.sf.class_name(o) != "ComputeShader" {
                continue;
            }
            let t = f.sf.read_typetree(o.path_id)?;
            let found = t
                .get("variants")
                .and_then(unityfs::Value::as_array)
                .is_some_and(|vs| {
                    vs.iter().any(|v| {
                        v.get("targetRenderer").and_then(unityfs::Value::as_int) == Some(11)
                            && v.get("kernels")
                                .and_then(unityfs::Value::as_array)
                                .is_some_and(|k| !k.is_empty())
                    })
                });
            if found {
                return Ok(true);
            }
        }
    }
    Ok(false)
}

/// android_package.json's min_sdk (--min-sdk).
pub fn min_sdk(ctx: &mut Ctx) -> Result<i64, Stop> {
    if ctx.args.min_sdk == "auto" {
        if ctx.compute_kernels.is_none() {
            let src = ctx.args.src.clone();
            ctx.compute_kernels = Some(has_compute_kernels(ctx, &src)?);
        }
        return Ok(if ctx.compute_kernels == Some(true) {
            GLES31_SDK
        } else {
            0
        });
    }
    pkgzip::py::int(&ctx.args.min_sdk, 10)
        .map(|v| v as i64)
        .ok_or_else(|| {
            Stop::Fail(format!(
                "ValueError: invalid literal for int() with base 10: {}",
                py::str_repr(&ctx.args.min_sdk)
            ))
        })
}

/// The quality step's caps recorded in the conversion's journal ('' if none).
pub fn quality_caps(src: &str) -> String {
    let Ok(b) = fs::read(py::join(src, ".pmunity.json")) else {
        return String::new();
    };
    let Ok(j) = pyjson::loads_bytes(&b) else {
        return String::new();
    };
    let Some(caps) = j.get("quality_caps").filter(|c| c.truthy()) else {
        return String::new();
    };
    let Some(caps) = caps.as_str() else {
        return String::new();
    };
    let ok = caps.split(',').all(|p| {
        p.split_once('=').is_some_and(|(k, v)| {
            !k.is_empty()
                && k.bytes().all(|c| c.is_ascii_lowercase() || c == b'_')
                && !v.is_empty()
                && v.bytes().all(|c| c.is_ascii_digit())
        })
    });
    if ok {
        caps.to_string()
    } else {
        String::new()
    }
}

/// boot.config's gc-max-time-slice (ms), else 0.
pub fn gc_time_slice(src: &str) -> i64 {
    let Ok(b) = fs::read(py::join(src, "boot.config")) else {
        return 0;
    };
    let t = String::from_utf8_lossy(&b);
    for line in t.lines() {
        let line = py::strip(line);
        let (k, v) = line.split_once('=').unwrap_or((line, ""));
        let v = py::strip(v);
        if k == "gc-max-time-slice" && !v.is_empty() && v.chars().all(|c| c.is_ascii_digit()) {
            return v.parse().unwrap_or(0);
        }
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mono_config() {
        let t = "<configuration>\n\t<dllmap dll=\"i:cygwin1.dll\" target=\"libc.dylib\" os=\"!windows\" />\n\
                 \t<dllmap dll=\"libX11\" target=\"libX11.6.dylib\" os=\"!windows\" />\n\
                 \t<dllmap dll=\"gdiplus\" target=\"libgdiplus.dylib\" os=\"osx\"/>\n\
                 \t<dllmap dll=\"sqlite\" target=\"libsqlite3.0.dylib\" />\r\n</configuration>\n";
        let out = mono_config_text(t, &[("SDL2".into(), "libSDL2-2.0.so.0".into())]);
        assert_eq!(
            out,
            "<configuration>\n\t<dllmap dll=\"i:cygwin1.dll\" target=\"libc.so.6\" os=\"!windows\" />\n\
             \t<dllmap dll=\"libX11\" target=\"libX11.so.6\" os=\"!windows\" />\n\
             \t<dllmap dll=\"gdiplus\" target=\"libgdiplus.dylib\" os=\"osx\"/>\n\
             \t<dllmap dll=\"sqlite\" target=\"libsqlite3.so.0\" />\r\n\
             \t<dllmap dll=\"SDL2\" target=\"libSDL2-2.0.so.0\" os=\"!windows\"/>\n\
             \t<dllmap dll=\"libmono.so.0\" target=\"libmono.so\" os=\"!windows\"/>\n</configuration>\n"
        );
    }

    #[test]
    fn small_things() {
        assert!(level_or_shared("level12"));
        assert!(level_or_shared("sharedassets3.assets"));
        assert!(!level_or_shared("level"));
        assert!(!level_or_shared("sharedassets.assets"));
        assert_eq!(
            versions_nul(b"\x002017.4.9f1\0 5.6.7f1\0x2017.4.1f1\0"),
            ["2017.4.9f1", "5.6.7f1"]
        );
        assert_eq!(splitlines_keep("a\r\nb\rc\n"), ["a\r\n", "b\r", "c\n"]);
    }
}
