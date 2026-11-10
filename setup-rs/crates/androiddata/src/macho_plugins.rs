//! --macho: the native plugins of a macOS build (<Game>.app/Contents/PlugIns, in Unity 6 also its
//! ARM64/ and x86_64/ folders): which of them the runtime's loader can run (docs/macho-route.md,
//! "Plugins the game opens"), written as their arm64 code into lib/arm64-v8a of the output under
//! the plugin's own name, where the game's dlopen finds them; which are an SDK the runtime
//! answers itself (the plugin map: Steam, Epic Online Services, ...); and which are left out,
//! with the reason. A plugin is read the way the loader reads it: its load commands and the
//! names its bind streams ask for, each against the libraries beside it and the list of names
//! the runtime answers (macho_shims.txt, the runtime's tables: a test keeps the two in step).
//! Nothing of a plugin is changed.
use crate::macho::{self, ABI};
use crate::plugins;
use crate::pyjson::Json;
use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;
use std::sync::OnceLock;

const SHIMS: &str = include_str!("macho_shims.txt");

const MH_MAGIC_64: u32 = 0xFEED_FACF;
const MH_DYLIB: u32 = 6;
const MH_BUNDLE: u32 = 8;
const LC_LOAD_DYLIB: u32 = 0xC;
const LC_ID_DYLIB: u32 = 0xD;
const LC_LOAD_WEAK_DYLIB: u32 = 0x8000_0018;
const LC_REEXPORT_DYLIB: u32 = 0x8000_001F;
const LC_LOAD_UPWARD_DYLIB: u32 = 0x8000_0023;
const LC_DYLD_INFO: u32 = 0x22;
const LC_DYLD_INFO_ONLY: u32 = 0x8000_0022;
const LC_DYLD_EXPORTS_TRIE: u32 = 0x8000_0033;
const LC_DYLD_CHAINED_FIXUPS: u32 = 0x8000_0034;
const LC_ENCRYPTION_INFO_64: u32 = 0x2C;
/// (the load commands and one bind stream of a plugin: far above any real one)
const MAX_COMMANDS: u64 = 4 << 20;
const MAX_STREAM: u64 = 64 << 20;

/// The files the route itself keeps in the lib folder: a plugin of such a name is not written over them.
const OWN_FILES: &[&str] = &[
    "GameAssembly.dylib",
    "libc++.1.dylib",
    "libc++abi.1.dylib",
    "libil2cpp.so",
    "libmain.so",
    "libunity.so",
];

/// Whether the runtime answers a libSystem or CoreFoundation name ("_open").
pub fn answered(name: &str) -> bool {
    static S: OnceLock<HashSet<&'static str>> = OnceLock::new();
    S.get_or_init(|| SHIMS.lines().filter(|l| !l.is_empty()).collect())
        .contains(name)
}

/// A name a plugin asks another library for.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct Import {
    pub name: String,
    /// the library by its number among the plugin's dylibs (1..); 0: the plugin itself; below 0:
    /// looked up among all that is loaded
    pub lib: i64,
    /// the plugin runs without it
    pub weak: bool,
}

/// What the loader reads of a plugin before it runs it.
#[derive(Debug, Default)]
pub struct Facts {
    pub filetype: u32,
    pub install_name: Option<String>,
    /// the dylibs it names, in the order its imports number them
    pub dylibs: Vec<String>,
    pub imports: Vec<Import>,
    pub chained: bool,
    pub encrypted: bool,
}

fn le32(b: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(o..o + 4)?.try_into().ok()?))
}

fn cstr(b: &[u8]) -> Option<&str> {
    let n = b.iter().position(|&c| c == 0)?;
    std::str::from_utf8(&b[..n]).ok()
}

fn leaf(p: &str) -> &str {
    p.rsplit('/').next().unwrap_or(p)
}

fn uleb(b: &[u8], p: &mut usize) -> Option<u64> {
    let (mut v, mut shift) = (0u64, 0u32);
    loop {
        let c = *b.get(*p)?;
        *p += 1;
        if shift < 64 {
            v |= u64::from(c & 0x7f) << shift;
        }
        shift += 7;
        if c & 0x80 == 0 {
            return Some(v);
        }
    }
}

/// The names one bind stream asks for (the loader's walk, macho.c: every opcode it reads, and
/// no other). lazy: an entry ends with a zero and the next follows.
fn binds(b: &[u8], lazy: bool, out: &mut BTreeSet<Import>) -> Result<(), String> {
    let cut = || "a bind stream is cut".to_string();
    let (mut p, mut lib, mut weak) = (0usize, 0i64, false);
    let mut name: Option<&str> = None;
    while p < b.len() {
        let (op, imm) = (b[p] & 0xf0, b[p] & 0x0f);
        p += 1;
        let mut bound = false;
        match op {
            0x00 => {
                if !lazy {
                    return Ok(());
                }
            }
            0x10 => lib = i64::from(imm),
            0x20 => lib = uleb(b, &mut p).ok_or_else(cut)? as i64,
            0x30 => {
                lib = if imm != 0 {
                    i64::from((0xf0 | imm) as i8)
                } else {
                    0
                }
            }
            0x40 => {
                let s = cstr(&b[p..]).ok_or_else(cut)?;
                p += s.len() + 1;
                name = Some(s);
                weak = imm & 1 != 0;
            }
            0x50 => {}
            0x60 | 0x70 | 0x80 => {
                uleb(b, &mut p).ok_or_else(cut)?;
            }
            0x90 | 0xb0 => bound = true,
            0xa0 => {
                uleb(b, &mut p).ok_or_else(cut)?;
                bound = true;
            }
            0xc0 => {
                let n = uleb(b, &mut p).ok_or_else(cut)?;
                uleb(b, &mut p).ok_or_else(cut)?;
                bound = n > 0;
            }
            _ => return Err(format!("bind opcode {op:#04x}")),
        }
        if bound {
            let name = name.ok_or("a bind without a name")?.to_string();
            out.insert(Import { name, lib, weak });
        }
    }
    Ok(())
}

/// A plugin's facts from its arm64 code, or why it has none this route runs (macho::arm64_slice's words).
pub fn facts(path: &str) -> Result<Facts, String> {
    let (off, size, _) = macho::arm64_slice(path)?;
    let mut f = File::open(path).map_err(|e| format!("{path}: {e}"))?;
    let mut part = |at: u64, n: u64, what: &str| -> Result<Vec<u8>, String> {
        if at.checked_add(n).is_none_or(|e| e > size) {
            return Err(format!("{path}: {what} past the end of its arm64 code"));
        }
        let mut b = vec![0u8; n as usize];
        f.seek(SeekFrom::Start(off + at))
            .and_then(|_| f.read_exact(&mut b))
            .map_err(|e| format!("{path}: {e}"))?;
        Ok(b)
    };
    let head = part(0, 32, "its header")?;
    if le32(&head, 0) != Some(MH_MAGIC_64) {
        return Err(format!("{path}: no 64-bit Mach-O code"));
    }
    let mut facts = Facts {
        filetype: le32(&head, 12).unwrap_or(0),
        ..Default::default()
    };
    let (ncmds, len) = (
        le32(&head, 16).unwrap_or(0),
        u64::from(le32(&head, 20).unwrap_or(0)),
    );
    if len > MAX_COMMANDS {
        return Err(format!("{path}: {len} bytes of load commands"));
    }
    let cmds = part(32, len, "its load commands")?;
    let mut streams: Vec<(u64, u64, bool)> = Vec::new();
    let mut c = 0usize;
    for i in 0..ncmds {
        let bad = || format!("{path}: load command {i} cut");
        let (cmd, n) = (
            le32(&cmds, c).ok_or_else(bad)?,
            le32(&cmds, c + 4).ok_or_else(bad)? as usize,
        );
        let body = cmds.get(c..c + n).filter(|_| n >= 8).ok_or_else(bad)?;
        let dylib = || -> Result<String, String> {
            let o = le32(body, 8).ok_or_else(bad)? as usize;
            Ok(cstr(body.get(o..).ok_or_else(bad)?)
                .ok_or_else(bad)?
                .to_string())
        };
        match cmd {
            LC_LOAD_DYLIB | LC_LOAD_WEAK_DYLIB | LC_REEXPORT_DYLIB | LC_LOAD_UPWARD_DYLIB => {
                facts.dylibs.push(dylib()?)
            }
            LC_ID_DYLIB => facts.install_name = Some(dylib()?),
            LC_DYLD_INFO | LC_DYLD_INFO_ONLY => {
                let at = |o: usize| le32(body, o).map(u64::from).ok_or_else(bad);
                streams.push((at(16)?, at(20)?, false));
                streams.push((at(32)?, at(36)?, true));
            }
            LC_DYLD_CHAINED_FIXUPS | LC_DYLD_EXPORTS_TRIE => facts.chained = true,
            LC_ENCRYPTION_INFO_64 => facts.encrypted |= le32(body, 16).ok_or_else(bad)? != 0,
            _ => {}
        }
        c += n;
    }
    let mut names = BTreeSet::new();
    for (at, n, lazy) in streams {
        if n == 0 {
            continue;
        }
        if n > MAX_STREAM {
            return Err(format!("{path}: a bind stream of {n} bytes"));
        }
        binds(&part(at, n, "a bind stream")?, lazy, &mut names)
            .map_err(|e| format!("{path}: {e}"))?;
    }
    facts.imports = names.into_iter().collect();
    Ok(facts)
}

/// What becomes of one plugin.
#[derive(Debug, Clone, PartialEq)]
pub enum Verdict {
    /// its arm64 code goes into the output's lib folder: the runtime's loader runs it
    Copy,
    /// the runtime answers the library itself: the plugin map's entry for its name, and why
    Map(Json, String),
    /// not in the port, and why (the game gets DllNotFoundException at its first call of it)
    Out(String),
}

#[derive(Debug)]
pub struct Plugin {
    /// its name in the lib folder: the file's, or the bundle folder's
    pub name: String,
    /// where it is, under the app's Contents
    pub rel: String,
    /// its Mach-O file ("" where none was found)
    pub binary: String,
    /// the name the game's code imports it by, as the plugin map spells it
    pub import: String,
    pub sdk: Option<&'static str>,
    pub dylibs: Vec<String>,
    pub imports: usize,
    pub verdict: Verdict,
}

impl Plugin {
    /// Where its arm64 code goes in the output.
    pub fn dst(&self) -> String {
        format!("lib/{ABI}/{}", self.name)
    }
}

fn is_macho(path: &str) -> bool {
    let mut b = [0u8; 4];
    if File::open(path)
        .and_then(|mut f| f.read_exact(&mut b))
        .is_err()
    {
        return false;
    }
    u32::from_le_bytes(b) == MH_MAGIC_64
        || matches!(u32::from_be_bytes(b), 0xCAFE_BABE | 0xCAFE_BABF)
}

fn entries(dir: &str) -> (Vec<String>, Vec<String>) {
    let (mut ds, mut fs) = (Vec::new(), Vec::new());
    if let Ok(rd) = std::fs::read_dir(dir) {
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if Path::new(dir).join(&n).is_dir() {
                ds.push(n);
            } else {
                fs.push(n);
            }
        }
    }
    ds.sort();
    fs.sort();
    (ds, fs)
}

/// A bundle folder's code: Contents/MacOS/<CFBundleExecutable>, else the one Mach-O file there.
fn bundle_binary(dir: &str) -> Result<String, String> {
    let macos = format!("{dir}/Contents/MacOS");
    let plist = std::fs::read(format!("{dir}/Contents/Info.plist")).unwrap_or_default();
    let plist = String::from_utf8_lossy(&plist);
    if let Some(at) = plist.find("<key>CFBundleExecutable</key>") {
        let rest = &plist[at..];
        if let (Some(a), Some(b)) = (rest.find("<string>"), rest.find("</string>")) {
            if a + 8 <= b {
                let p = format!("{macos}/{}", rest[a + 8..b].trim());
                if is_macho(&p) {
                    return Ok(p);
                }
            }
        }
    }
    let (_, fs) = entries(&macos);
    let code: Vec<String> = fs
        .iter()
        .map(|n| format!("{macos}/{n}"))
        .filter(|p| is_macho(p))
        .collect();
    match code.len() {
        1 => Ok(code[0].clone()),
        0 => Err("a bundle folder without code (Contents/MacOS)".into()),
        n => Err(format!(
            "a bundle folder with {n} code files and no CFBundleExecutable that names one"
        )),
    }
}

/// (name in the lib folder, path under Contents, its Mach-O file or why none)
type Found = (String, String, Result<String, String>);

fn walk(dir: &str, rel: &str, out: &mut Vec<Found>) {
    let (ds, fs) = entries(dir);
    for n in fs {
        let p = format!("{dir}/{n}");
        if !n.starts_with('.') && is_macho(&p) {
            out.push((n.clone(), format!("{rel}/{n}"), Ok(p)));
        }
    }
    for n in ds {
        let (p, r, low) = (format!("{dir}/{n}"), format!("{rel}/{n}"), n.to_lowercase());
        if low.ends_with(".bundle") {
            out.push((n, r, bundle_binary(&p)));
        } else if low.ends_with(".framework") {
            out.push((
                n,
                r,
                Err("a framework: the route loads library and bundle plugins only".into()),
            ));
        } else if !low.ends_with(".dsym") && !Path::new(&p).is_symlink() {
            walk(&p, &r, out);
        }
    }
}

/// The plugin folders of the app whose data folder this is (<Game>.app/Contents/Resources/Data:
/// Contents/PlugIns, however the build spells it).
fn plugin_dirs(data: &str) -> Vec<(String, String)> {
    let Some(resources) = Path::new(data).parent() else {
        return Vec::new();
    };
    if resources.file_name().and_then(|n| n.to_str()) != Some("Resources") {
        return Vec::new();
    }
    let Some(contents) = resources.parent() else {
        return Vec::new();
    };
    let contents = contents.to_string_lossy().into_owned();
    let (ds, _) = entries(&contents);
    ds.into_iter()
        .filter(|n| n.eq_ignore_ascii_case("PlugIns"))
        .map(|n| (format!("{contents}/{n}"), n))
        .collect()
}

fn import_name(name: &str) -> String {
    let low = name.to_lowercase();
    for ext in [".dylib", ".bundle", ".so"] {
        if low.ends_with(ext) {
            return name[..name.len() - ext.len()].to_string();
        }
    }
    name.to_string()
}

fn is_cxx(lib: &str) -> bool {
    matches!(
        leaf(lib),
        "libc++.1.dylib" | "libc++abi.dylib" | "libc++abi.1.dylib"
    )
}

/// The names of a plugin no one answers, with the library each is asked of, when `there` are
/// the plugins in the lib folder: as the loader binds (dyld.c's resolve), a name asked of a
/// library that is loaded is that library's matter, the C++ library's are the runtime's own
/// libc++, and every other one needs an answer among the runtime's. (A name the plugin has
/// itself, or one looked up among all that is loaded, is not judged here: what a library
/// exports is not read, and the loader says at the load which name it found no answer for.)
pub fn unanswered(f: &Facts, there: &BTreeSet<String>) -> Vec<(String, String)> {
    let mut out = Vec::new();
    for im in &f.imports {
        if im.weak || im.lib < 1 || answered(&im.name) {
            continue;
        }
        let lib = f.dylibs.get(im.lib as usize - 1).map_or("", |l| leaf(l));
        if is_cxx(lib) || there.contains(lib) {
            continue;
        }
        out.push((im.name.clone(), lib.to_string()));
    }
    out
}

fn why_unanswered(missing: &[(String, String)]) -> String {
    let libs: BTreeSet<&str> = missing.iter().map(|m| m.1.as_str()).collect();
    let shown: Vec<&str> = missing.iter().take(6).map(|m| m.0.as_str()).collect();
    format!(
        "it asks {} for {} the runtime has no answer for ({}{})",
        libs.into_iter().collect::<Vec<_>>().join(", "),
        if missing.len() == 1 {
            "a name".to_string()
        } else {
            format!("{} names", missing.len())
        },
        shown.join(", "),
        if missing.len() > shown.len() {
            ", ..."
        } else {
            ""
        }
    )
}

/// The plugin map's entry for an SDK's library (plugins.rs's table), where it has one.
fn map_entry(sdk: &plugins::Sdk, import: &str) -> Option<Json> {
    let mut e = Json::obj();
    e.set("use", sdk.3.into());
    match sdk.3 {
        "stub" => e.set("stub", sdk.4?.into()),
        "none" => {}
        "lib" => e.set(
            "file",
            format!("lib{}.so", plugins::norm(import)).as_str().into(),
        ),
        "host" => e.set("lib", plugins::host_default(sdk.0)?.into()),
        _ => return None,
    }
    Some(e)
}

/// The plugins of the macOS build whose data folder this is, each with what becomes of it.
pub fn survey(data: &str) -> Vec<Plugin> {
    let mut found = Vec::new();
    for (dir, rel) in plugin_dirs(data) {
        walk(&dir, &rel, &mut found);
    }
    // One plugin per name: the copy with arm64 code where a build keeps one per CPU.
    let mut by_name: BTreeMap<String, Vec<Found>> = BTreeMap::new();
    for f in found {
        by_name.entry(f.0.clone()).or_default().push(f);
    }
    let mut plugins: Vec<Plugin> = Vec::new();
    let mut read: Vec<Option<Facts>> = Vec::new();
    for (name, copies) in by_name {
        let tried: Vec<(&Found, Result<Facts, String>)> = copies
            .iter()
            .map(|c| (c, c.2.clone().and_then(|p| facts(&p))))
            .collect();
        let (c, f) = tried
            .into_iter()
            .reduce(|a, b| if a.1.is_ok() || b.1.is_err() { a } else { b })
            .unwrap();
        let import = import_name(&name);
        let sdk = plugins::sdk_of(&import);
        let binary = c.2.clone().unwrap_or_default();
        let (dylibs, imports) = f
            .as_ref()
            .map_or((Vec::new(), 0), |f| (f.dylibs.clone(), f.imports.len()));
        // (a client library answers nothing without its client, on any system: the runtime's own answer)
        let own = sdk
            .filter(|s| matches!(s.3, "stub" | "none"))
            .and_then(|s| Some((s, map_entry(s, &import)?)));
        let verdict = match (own, &f) {
            (Some((s, e)), _) => Verdict::Map(e, format!("{}: {}", s.2, s.5)),
            (_, Err(e)) => Verdict::Out(e.strip_prefix(&format!("{binary}: ")).unwrap_or(e).to_string()),
            (_, Ok(f)) if f.filetype != MH_DYLIB && f.filetype != MH_BUNDLE => {
                Verdict::Out(format!("no library (Mach-O file type {})", f.filetype))
            }
            (_, Ok(f)) if f.encrypted => Verdict::Out("encrypted code: left alone".into()),
            (_, Ok(f)) if f.chained => Verdict::Out(
                "built for macOS 12 or later only (chained fixups), which the runtime's loader does not read".into(),
            ),
            _ if OWN_FILES.contains(&name.as_str()) => {
                Verdict::Out("the lib folder has the route's own file of that name".into())
            }
            _ => Verdict::Copy,
        };
        plugins.push(Plugin {
            name,
            rel: c.1.clone(),
            binary,
            import,
            sdk: sdk.map(|s| s.0),
            dylibs,
            imports,
            verdict,
        });
        read.push(f.ok());
    }
    // A plugin that needs one that is left out is left out too: until nothing changes.
    loop {
        let there: BTreeSet<String> = plugins
            .iter()
            .zip(&read)
            .filter(|(p, _)| p.verdict == Verdict::Copy)
            .flat_map(|(p, f)| {
                let id = f
                    .as_ref()
                    .and_then(|f| f.install_name.as_deref())
                    .map(|n| leaf(n).to_string());
                std::iter::once(p.name.clone()).chain(id)
            })
            .collect();
        let mut changed = false;
        for (p, f) in plugins.iter_mut().zip(&read) {
            let Some(f) = f.as_ref().filter(|_| p.verdict == Verdict::Copy) else {
                continue;
            };
            let missing = unanswered(f, &there);
            if missing.is_empty() {
                continue;
            }
            let why = why_unanswered(&missing);
            let sdk = p.sdk.and_then(|_| plugins::sdk_of(&p.import));
            p.verdict = match sdk.and_then(|s| map_entry(s, &p.import).map(|e| (s, e))) {
                Some((s, e)) => Verdict::Map(
                    e,
                    format!(
                        "{}: {}; the build's own library is not used: {why}",
                        s.2, s.5
                    ),
                ),
                None => Verdict::Out(why),
            };
            changed = true;
        }
        if !changed {
            break;
        }
    }
    plugins
}

/// The plugin map's entries the survey gives: (the name the game imports, its entry).
pub fn map_entries(found: &[Plugin]) -> Vec<(String, Json)> {
    found
        .iter()
        .filter_map(|p| match &p.verdict {
            Verdict::Map(e, _) => Some((p.import.clone(), e.clone())),
            _ => None,
        })
        .collect()
}

/// One line per plugin for the log: (whether it is left out, the line).
pub fn lines(found: &[Plugin]) -> Vec<(bool, String)> {
    found
        .iter()
        .map(|p| {
            let line = match &p.verdict {
            Verdict::Copy => format!(
                "{}: a plugin the runtime's loader runs ({}): its arm64 code goes to {}",
                p.rel,
                if p.imports == 0 {
                    "it asks no other library for a name".to_string()
                } else {
                    format!(
                        "{} names asked of {}",
                        p.imports,
                        p.dylibs.iter().map(|d| leaf(d)).collect::<Vec<_>>().join(", ")
                    )
                },
                p.dst()
            ),
            Verdict::Map(e, why) => format!(
                "{}: not copied, the runtime answers \"{}\" itself ({}): {why}",
                p.rel,
                p.import,
                match e.get("use").and_then(Json::as_str) {
                    Some("stub") => format!("built-in stub {}", e.get("stub").map(Json::py_str).unwrap_or_default()),
                    Some("lib") => format!("an ARM build, lib/{}", e.get("file").map(Json::py_str).unwrap_or_default()),
                    Some("host") => format!("the system's {}", e.get("lib").map(Json::py_str).unwrap_or_default()),
                    _ => "not available".to_string(),
                }
            ),
            Verdict::Out(why) => format!(
                "{}: left out ({why}): the game gets DllNotFoundException at its first call of \"{}\"",
                p.rel, p.import
            ),
            };
            (matches!(p.verdict, Verdict::Out(_)), line)
        })
        .collect()
}

/// The survey for the conversion's report.
pub fn report(found: &[Plugin]) -> Json {
    Json::List(
        found
            .iter()
            .map(|p| {
                let mut j = Json::obj();
                j.set("file", p.rel.as_str().into());
                j.set("import", p.import.as_str().into());
                if let Some(s) = p.sdk {
                    j.set("sdk", s.into());
                }
                j.set("dylibs", p.dylibs.clone().into());
                j.set("imports", p.imports.into());
                match &p.verdict {
                    Verdict::Copy => {
                        j.set("use", "copied".into());
                        j.set("lib", p.dst().as_str().into());
                    }
                    Verdict::Map(e, why) => {
                        j.set("use", "map".into());
                        j.set("map", e.clone());
                        j.set("note", why.as_str().into());
                    }
                    Verdict::Out(why) => {
                        j.set("use", "left out".into());
                        j.set("note", why.as_str().into());
                    }
                }
                j
            })
            .collect(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A Mach-O file as the loader reads one: the dylibs it names, the names it binds
    /// ((name, dylib number, weak): the first half in the bind stream, the rest in the lazy one).
    struct Image<'a> {
        filetype: u32,
        id: Option<&'a str>,
        dylibs: &'a [&'a str],
        imports: &'a [(&'a str, i64, bool)],
        chained: bool,
        cryptid: u32,
    }

    impl Default for Image<'_> {
        fn default() -> Self {
            Image {
                filetype: MH_DYLIB,
                id: None,
                dylibs: &[],
                imports: &[],
                chained: false,
                cryptid: 0,
            }
        }
    }

    fn uleb_bytes(mut v: u64, out: &mut Vec<u8>) {
        loop {
            let c = (v & 0x7f) as u8;
            v >>= 7;
            out.push(if v != 0 { c | 0x80 } else { c });
            if v == 0 {
                break;
            }
        }
    }

    fn stream(imports: &[(&str, i64, bool)], lazy: bool) -> Vec<u8> {
        let mut b = Vec::new();
        for (i, (name, lib, weak)) in imports.iter().enumerate() {
            match *lib {
                l if l < 0 => b.push(0x30 | (l as u8 & 0x0f)),
                l if l < 16 => b.push(0x10 | l as u8),
                l => {
                    b.push(0x20);
                    uleb_bytes(l as u64, &mut b);
                }
            }
            b.push(0x40 | u8::from(*weak));
            b.extend(name.as_bytes());
            b.push(0);
            b.push(0x51);
            b.push(0x72);
            uleb_bytes(8 * i as u64 + 300, &mut b);
            // each of the loader's bind forms in turn
            match if lazy { 0 } else { i % 4 } {
                0 => b.push(0x90),
                1 => {
                    b.push(0xa0);
                    uleb_bytes(200, &mut b);
                }
                2 => b.push(0xb3),
                _ => {
                    b.push(0xc0);
                    uleb_bytes(3, &mut b);
                    uleb_bytes(8, &mut b);
                }
            }
            if lazy {
                b.push(0);
            }
        }
        if !lazy {
            b.push(0);
        }
        b
    }

    fn image(im: &Image) -> Vec<u8> {
        let mut cmds: Vec<Vec<u8>> = Vec::new();
        let dylib = |cmd: u32, name: &str| {
            let mut c = Vec::new();
            let n = (24 + name.len() + 1).div_ceil(8) * 8;
            c.extend(cmd.to_le_bytes());
            c.extend((n as u32).to_le_bytes());
            c.extend(24u32.to_le_bytes());
            c.extend([0u8; 12]);
            c.extend(name.as_bytes());
            c.resize(n, 0);
            c
        };
        if let Some(id) = im.id {
            cmds.push(dylib(LC_ID_DYLIB, id));
        }
        for (i, d) in im.dylibs.iter().enumerate() {
            cmds.push(dylib(
                if i % 2 == 1 {
                    LC_LOAD_WEAK_DYLIB
                } else {
                    LC_LOAD_DYLIB
                },
                d,
            ));
        }
        let half = im.imports.len() / 2;
        let (bind, lazy) = (
            stream(&im.imports[..half], false),
            stream(&im.imports[half..], true),
        );
        let info_at = cmds.len();
        cmds.push(vec![0u8; 48]);
        if im.chained {
            let mut c = Vec::new();
            c.extend(LC_DYLD_CHAINED_FIXUPS.to_le_bytes());
            c.extend(16u32.to_le_bytes());
            c.extend([0u8; 8]);
            cmds.push(c);
        }
        if im.cryptid != 0 {
            let mut c = Vec::new();
            c.extend(LC_ENCRYPTION_INFO_64.to_le_bytes());
            c.extend(24u32.to_le_bytes());
            c.extend([0u8; 8]);
            c.extend(im.cryptid.to_le_bytes());
            c.extend([0u8; 4]);
            cmds.push(c);
        }
        let len: usize = cmds.iter().map(Vec::len).sum();
        let (bind_at, lazy_at) = (32 + len, 32 + len + bind.len());
        let mut info = Vec::new();
        info.extend(LC_DYLD_INFO_ONLY.to_le_bytes());
        info.extend(48u32.to_le_bytes());
        info.extend([0u8; 8]);
        info.extend((bind_at as u32).to_le_bytes());
        info.extend((bind.len() as u32).to_le_bytes());
        info.extend([0u8; 8]);
        info.extend((lazy_at as u32).to_le_bytes());
        info.extend((lazy.len() as u32).to_le_bytes());
        info.extend([0u8; 8]);
        cmds[info_at] = info;
        let mut b = Vec::new();
        b.extend(MH_MAGIC_64.to_le_bytes());
        b.extend(0x0100_000Cu32.to_le_bytes());
        b.extend(0u32.to_le_bytes());
        b.extend(im.filetype.to_le_bytes());
        b.extend((cmds.len() as u32).to_le_bytes());
        b.extend((len as u32).to_le_bytes());
        b.extend([0u8; 8]);
        for c in cmds {
            b.extend(c);
        }
        b.extend(bind);
        b.extend(lazy);
        b
    }

    fn intel() -> Vec<u8> {
        let mut b = Vec::new();
        b.extend(MH_MAGIC_64.to_le_bytes());
        b.extend(0x0100_0007u32.to_le_bytes());
        b.extend(3u32.to_le_bytes());
        b.resize(64, 0);
        b
    }

    fn universal(parts: &[(u32, Vec<u8>)]) -> Vec<u8> {
        let mut b = Vec::new();
        b.extend(0xCAFE_BABEu32.to_be_bytes());
        b.extend((parts.len() as u32).to_be_bytes());
        let mut at = 4096u32;
        let mut places = Vec::new();
        for (cpu, d) in parts {
            b.extend(cpu.to_be_bytes());
            b.extend(0u32.to_be_bytes());
            b.extend(at.to_be_bytes());
            b.extend((d.len() as u32).to_be_bytes());
            b.extend(12u32.to_be_bytes());
            places.push((at, d));
            at += (d.len() as u32).div_ceil(4096) * 4096;
        }
        for (at, d) in places {
            b.resize(at as usize, 0);
            b.extend(d);
        }
        b
    }

    struct App(String);

    impl App {
        fn new(name: &str) -> App {
            let t = std::env::temp_dir()
                .join(format!("pmu-machoplugins-{}-{name}", std::process::id()));
            let _ = std::fs::remove_dir_all(&t);
            std::fs::create_dir_all(t.join("Game.app/Contents/Resources/Data")).unwrap();
            App(t.to_string_lossy().into_owned())
        }
        fn put(&self, rel: &str, b: &[u8]) {
            let p = format!("{}/Game.app/Contents/{rel}", self.0);
            std::fs::create_dir_all(Path::new(&p).parent().unwrap()).unwrap();
            std::fs::write(p, b).unwrap();
        }
        fn data(&self) -> String {
            format!("{}/Game.app/Contents/Resources/Data", self.0)
        }
    }

    impl Drop for App {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn of<'a>(found: &'a [Plugin], name: &str) -> &'a Plugin {
        found
            .iter()
            .find(|p| p.name == name)
            .unwrap_or_else(|| panic!("no {name}"))
    }

    const SYSTEM: &str = "/usr/lib/libSystem.B.dylib";
    const CXX: &str = "/usr/lib/libc++.1.dylib";

    /// the loader's walk of both streams: every bind form, the library numbers, the weak flag
    #[test]
    fn reads_what_the_loader_reads() {
        let app = App::new("facts");
        let imports = [
            ("_malloc", 1, false),
            ("__ZNSt3__16vectorIiE", 2, false),
            ("_maybe", 1, true),
            ("_own", 0, false),
            ("_anywhere", -2, false),
            ("_far", 20, false),
            ("_free", 1, false),
            ("_strlen", 1, false),
            ("dyld_stub_binder", 1, false),
            ("_pthread_create", 1, false),
        ];
        let dylibs: Vec<String> = [SYSTEM, CXX]
            .iter()
            .map(|s| s.to_string())
            .chain((3..=20).map(|i| format!("@rpath/lib{i}.dylib")))
            .collect();
        let dylibs: Vec<&str> = dylibs.iter().map(String::as_str).collect();
        app.put(
            "PlugIns/libx.dylib",
            &image(&Image {
                id: Some("@rpath/libx.1.dylib"),
                dylibs: &dylibs,
                imports: &imports,
                ..Default::default()
            }),
        );
        let f = facts(&format!("{}/Game.app/Contents/PlugIns/libx.dylib", app.0)).unwrap();
        assert_eq!(f.filetype, MH_DYLIB);
        assert_eq!(f.install_name.as_deref(), Some("@rpath/libx.1.dylib"));
        assert_eq!(f.dylibs, dylibs);
        let mut want: Vec<Import> = imports
            .iter()
            .map(|(n, l, w)| Import {
                name: n.to_string(),
                lib: *l,
                weak: *w,
            })
            .collect();
        want.sort();
        assert_eq!(f.imports, want);
        assert!(!f.chained && !f.encrypted);
        // a cut stream is said, not half-read
        let mut b = BTreeSet::new();
        assert!(binds(&[0x11, 0x40, b'_', b'a'], false, &mut b)
            .unwrap_err()
            .contains("cut"));
        assert!(binds(&[0xd0], false, &mut b).unwrap_err().contains("0xd0"));
    }

    #[test]
    fn verdicts() {
        let app = App::new("verdicts");
        let sys = &[SYSTEM, CXX][..];
        // loadable: libSystem names the runtime answers, C++ names, a weak one it has none for
        app.put(
            "PlugIns/lib_mesh.dylib",
            &image(&Image {
                id: Some("@rpath/lib_mesh.dylib"),
                dylibs: sys,
                imports: &[
                    ("_malloc", 1, false),
                    ("__ZNSt3__1lsERNS_13basic_ostream", 2, false),
                    ("_os_log_create", 1, true),
                    ("_free", 1, false),
                ],
                ..Default::default()
            }),
        );
        // a bundle folder, universal, its code named by the plist; it needs lib_mesh
        app.put(
            "PlugIns/tool.bundle/Contents/Info.plist",
            b"<plist><dict><key>CFBundleExecutable</key>\n<string>Tool Code</string></dict></plist>",
        );
        app.put("PlugIns/tool.bundle/Contents/MacOS/other", &intel());
        app.put(
            "PlugIns/tool.bundle/Contents/MacOS/Tool Code",
            &universal(&[
                (0x0100_0007, intel()),
                (
                    0x0100_000C,
                    image(&Image {
                        filetype: MH_BUNDLE,
                        dylibs: &[SYSTEM, "@rpath/lib_mesh.dylib"],
                        imports: &[("_mesh_run", 2, false), ("_strlen", 1, false)],
                        ..Default::default()
                    }),
                ),
            ]),
        );
        // SDKs: answered by the runtime whatever the file is
        app.put(
            "PlugIns/steam_api.bundle/Contents/MacOS/libsteam_api.dylib",
            &intel(),
        );
        app.put(
            "PlugIns/libEOSSDK-Mac-Shipping.dylib",
            &image(&Image {
                dylibs: sys,
                imports: &[("_objc_msgSend", 1, false)],
                ..Default::default()
            }),
        );
        // Cocoa: names the runtime has no answer for
        app.put(
            "PlugIns/MicrophoneUtility_macos.dylib",
            &image(&Image {
                dylibs: &[
                    SYSTEM,
                    "/System/Library/Frameworks/AVFoundation.framework/Versions/A/AVFoundation",
                    "/usr/lib/libobjc.A.dylib",
                ],
                imports: &[
                    ("_objc_msgSend", 3, false),
                    ("_AVCaptureDeviceTypeBuiltInMicrophone", 2, false),
                    ("_malloc", 1, false),
                ],
                ..Default::default()
            }),
        );
        // needs the one above: left out with it; and one that needs that one
        app.put(
            "PlugIns/libvoice.dylib",
            &image(&Image {
                dylibs: &[SYSTEM, "@loader_path/MicrophoneUtility_macos.dylib"],
                imports: &[("_mic_open", 2, false), ("_free", 1, false)],
                ..Default::default()
            }),
        );
        app.put(
            "PlugIns/libchat.dylib",
            &image(&Image {
                dylibs: &["@rpath/libvoice.dylib"],
                imports: &[("_voice_start", 1, false)],
                ..Default::default()
            }),
        );
        // one copy per CPU (Unity 6): the arm64 one is the plugin
        app.put("PlugIns/x86_64/lib_checks.dylib", &intel());
        app.put(
            "PlugIns/ARM64/lib_checks.dylib",
            &image(&Image {
                dylibs: sys,
                imports: &[("_strlen", 1, false)],
                ..Default::default()
            }),
        );
        // Intel only, a newer build, an encrypted one, an executable, the route's own name, a framework
        app.put("PlugIns/libold.dylib", &intel());
        app.put(
            "PlugIns/libnew.dylib",
            &image(&Image {
                chained: true,
                ..Default::default()
            }),
        );
        app.put(
            "PlugIns/libshut.dylib",
            &image(&Image {
                cryptid: 1,
                ..Default::default()
            }),
        );
        app.put(
            "PlugIns/helper",
            &image(&Image {
                filetype: 2,
                ..Default::default()
            }),
        );
        app.put("PlugIns/libunity.so", &image(&Image::default()));
        app.put("PlugIns/Thing.framework/Thing", &image(&Image::default()));
        // sqlite: the build's own library, loadable, is the answer; FMOD's is not loadable: the map's
        app.put(
            "PlugIns/libsqlite3.dylib",
            &image(&Image {
                dylibs: sys,
                imports: &[("_malloc", 1, false)],
                ..Default::default()
            }),
        );
        app.put(
            "PlugIns/libfmodstudio.dylib",
            &image(&Image {
                dylibs: &[
                    SYSTEM,
                    "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox",
                ],
                imports: &[("_AudioUnitRender", 2, false)],
                ..Default::default()
            }),
        );
        // not plugins
        app.put("PlugIns/readme.txt", b"text");
        app.put("PlugIns/.hidden.dylib", &image(&Image::default()));
        app.put(
            "PlugIns/lib_mesh.dylib.dSYM/Contents/Resources/DWARF/lib_mesh.dylib",
            &image(&Image::default()),
        );

        let found = survey(&app.data());
        let names: Vec<&str> = found.iter().map(|p| p.name.as_str()).collect();
        assert_eq!(
            names,
            [
                "MicrophoneUtility_macos.dylib",
                "Thing.framework",
                "helper",
                "libEOSSDK-Mac-Shipping.dylib",
                "lib_checks.dylib",
                "lib_mesh.dylib",
                "libchat.dylib",
                "libfmodstudio.dylib",
                "libnew.dylib",
                "libold.dylib",
                "libshut.dylib",
                "libsqlite3.dylib",
                "libunity.so",
                "libvoice.dylib",
                "steam_api.bundle",
                "tool.bundle",
            ]
        );
        let copied: Vec<&str> = found
            .iter()
            .filter(|p| p.verdict == Verdict::Copy)
            .map(|p| p.name.as_str())
            .collect();
        assert_eq!(
            copied,
            [
                "lib_checks.dylib",
                "lib_mesh.dylib",
                "libsqlite3.dylib",
                "tool.bundle"
            ]
        );
        assert_eq!(
            of(&found, "lib_checks.dylib").rel,
            "PlugIns/ARM64/lib_checks.dylib"
        );
        assert_eq!(
            of(&found, "lib_checks.dylib").dst(),
            "lib/arm64-v8a/lib_checks.dylib"
        );
        let tool = of(&found, "tool.bundle");
        assert!(
            tool.binary
                .ends_with("tool.bundle/Contents/MacOS/Tool Code"),
            "{}",
            tool.binary
        );
        assert_eq!((tool.import.as_str(), tool.imports), ("tool", 2));
        let out = |n: &str| match &of(&found, n).verdict {
            Verdict::Out(w) => w.clone(),
            v => panic!("{n}: {v:?}"),
        };
        assert_eq!(
            out("MicrophoneUtility_macos.dylib"),
            "it asks AVFoundation, libobjc.A.dylib for 2 names the runtime has no answer for (_AVCaptureDeviceTypeBuiltInMicrophone, _objc_msgSend)"
        );
        assert_eq!(out("libvoice.dylib"), "it asks MicrophoneUtility_macos.dylib for a name the runtime has no answer for (_mic_open)");
        assert_eq!(
            out("libchat.dylib"),
            "it asks libvoice.dylib for a name the runtime has no answer for (_voice_start)"
        );
        assert!(
            out("libold.dylib").contains("x86_64 code only"),
            "{}",
            out("libold.dylib")
        );
        assert!(out("libnew.dylib").contains("chained fixups"));
        assert!(out("libshut.dylib").contains("encrypted"));
        assert!(out("helper").contains("no library (Mach-O file type 2)"));
        assert!(out("libunity.so").contains("the route's own file"));
        assert!(out("Thing.framework").contains("a framework"));
        let map: Vec<(String, String)> = map_entries(&found)
            .into_iter()
            .map(|(n, e)| (n, crate::pyjson::dumps(&e, None)))
            .collect();
        assert_eq!(
            map,
            [
                (
                    "libEOSSDK-Mac-Shipping".to_string(),
                    r#"{"use": "stub", "stub": "eos"}"#.to_string()
                ),
                (
                    "libfmodstudio".to_string(),
                    r#"{"use": "lib", "file": "libfmodstudio.so"}"#.to_string()
                ),
                (
                    "steam_api".to_string(),
                    r#"{"use": "stub", "stub": "steam"}"#.to_string()
                ),
            ]
        );
        match &of(&found, "libfmodstudio.dylib").verdict {
            Verdict::Map(_, why) => assert!(
                why.contains(
                    "the build's own library is not used: it asks AudioToolbox for a name"
                ),
                "{why}"
            ),
            v => panic!("{v:?}"),
        }
        let text = lines(&found);
        assert_eq!(text.len(), found.len());
        assert!(text.iter().any(|(out, l)| !out && l
            == "PlugIns/lib_mesh.dylib: a plugin the runtime's loader runs (4 names asked of libSystem.B.dylib, libc++.1.dylib): its arm64 code goes to lib/arm64-v8a/lib_mesh.dylib"));
        assert!(text.iter().any(|(out, l)| !out && l.starts_with("PlugIns/steam_api.bundle: not copied, the runtime answers \"steam_api\" itself (built-in stub steam): Steamworks: ")));
        assert!(text.iter().any(|(out, l)| *out
            && l.ends_with(
                "the game gets DllNotFoundException at its first call of \"libvoice\""
            )));
        let rep = report(&found);
        assert_eq!(rep.as_list().unwrap().len(), found.len());
        // an app without plugins, a data folder that is no app's
        let bare = App::new("bare");
        assert!(survey(&bare.data()).is_empty());
        assert!(survey(&format!("{}/none/Data", bare.0)).is_empty());
    }

    /// The list of names here is the runtime's tables (runtime/macho/*.c: darwin_syms_*), when
    /// the runtime's sources are beside this crate.
    #[test]
    fn names_as_the_runtime_answers() {
        let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/../../../runtime/macho");
        let Ok(rd) = std::fs::read_dir(dir) else {
            return;
        };
        let mut names = BTreeSet::new();
        for e in rd.flatten() {
            let p = e.path();
            if p.extension().and_then(|x| x.to_str()) != Some("c") {
                continue;
            }
            let text = std::fs::read_to_string(&p).unwrap();
            let mut rest = text.as_str();
            while let Some(at) = rest.find("const struct darwin_sym darwin_syms_") {
                rest = &rest[at + 1..];
                let Some(open) = rest.find("[] = {") else {
                    continue;
                };
                if rest[..open].contains(';') {
                    continue; // (a declaration)
                }
                let table = &rest[open..open + rest[open..].find("\n};").expect("the table's end")];
                for line in table.lines() {
                    let line = line.split("//").next().unwrap();
                    let mut l = line;
                    // NAMED("name", f); every other row macro is X(name): "_name"
                    while let Some(at) = l.find('(') {
                        let word = l[..at]
                            .trim_end_matches(|c: char| {
                                c.is_ascii_uppercase() || c.is_ascii_digit()
                            })
                            .len();
                        let mac = &l[word..at];
                        l = &l[at + 1..];
                        if mac.is_empty() {
                            continue;
                        }
                        if mac == "NAMED" {
                            let s = l.strip_prefix('"').expect("NAMED's name");
                            names.insert(s[..s.find('"').unwrap()].to_string());
                        } else {
                            let n: String = l
                                .chars()
                                .take_while(|c| c.is_ascii_alphanumeric() || *c == '_')
                                .collect();
                            names.insert(format!("_{n}"));
                        }
                    }
                }
            }
        }
        let here: BTreeSet<String> = SHIMS
            .lines()
            .filter(|l| !l.is_empty())
            .map(str::to_string)
            .collect();
        let missing: Vec<&String> = names.difference(&here).collect();
        let stale: Vec<&String> = here.difference(&names).collect();
        assert!(
            missing.is_empty() && stale.is_empty(),
            "macho_shims.txt is not the runtime's tables: add {missing:?}, drop {stale:?}"
        );
        assert!(answered("_malloc") && answered("dyld_stub_binder") && !answered("_objc_msgSend"));
    }
}
