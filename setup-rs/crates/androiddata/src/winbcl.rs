//! `pmu winbcl <Game>_Data <out dir> [--version V] [--installer FILE|URL] [--player PKG]
//! [--no-facades] [--json FILE]` (winbcl.py): Unity's own class libraries a desktop build needs
//! for the Android player, from Unity's Windows editor installer (read with range requests,
//! pkgzip's nsis.rs): a Windows build of 2021.2+ gets the unityjit-linux twins of its
//! unityjit-win32 class libraries; with --player, the assemblies the player's engine modules
//! reference that the build lacks (a facade such as netstandard.dll) when they are Unity's own.
//! Nothing is read when neither applies. Prints the `--managed-file` options for android-data.
use crate::clrmeta::Assembly;
use crate::paths::listdir;
use crate::pyjson::{self, Json};
use crate::uversion::{self as UV, Version};
use pkgzip::nsis::Nsis;
use pkgzip::py;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::Path;

pub const API: &str = "https://services.api.unity.com/unity/editor/release/v1/releases";
pub const WIN: &str = "MonoBleedingEdge/lib/mono/unityjit-win32/";
pub const LINUX: &str = "MonoBleedingEdge/lib/mono/unityjit-linux/";
/// 2018.1 - 2021.1: one profile for every platform
pub const UNITYJIT: &str = "MonoBleedingEdge/lib/mono/unityjit/";
const MARK: &[u8] = b"SetThreadErrorMode"; // in the unityjit-win32 mscorlib (Interop+Kernel32)

/// How the command stops: SystemExit's message (1), an exception (1), argument errors (2).
#[derive(Debug)]
pub enum Fail {
    Exit(String),
    Error(String),
}

impl From<pkgzip::Error> for Fail {
    fn from(e: pkgzip::Error) -> Self {
        match e {
            pkgzip::Error::Exit(m) => Fail::Exit(m),
            e => Fail::Error(e.to_string()),
        }
    }
}

impl From<std::io::Error> for Fail {
    fn from(e: std::io::Error) -> Self {
        Fail::Error(e.to_string())
    }
}

/// True if a Managed folder's mscorlib.dll is the unityjit-win32 one (a 2021.2+ Windows build).
pub fn is_win32_profile(managed: &str) -> bool {
    fs::read(py::join(managed, "mscorlib.dll")).is_ok_and(|d| py::find(&d, MARK).is_some())
}

/// The Windows editor installer of a version, from Unity's release API.
pub fn editor_installer(version: &str) -> Result<String, Fail> {
    let q = format!("version={}&limit=1", pkgzip::http::quote_plus(version));
    let body = pkgzip::http::fetch(&format!("{API}?{q}"))?;
    let d: serde_like::Value = serde_like::parse(&body)?;
    for rec in d.get("results").map(|r| r.list()).unwrap_or_default() {
        if rec.get("version").map(|v| v.text()) != Some(version.to_string()) {
            continue;
        }
        for dl in rec.get("downloads").map(|r| r.list()).unwrap_or_default() {
            let url = dl.get("url").map(|u| u.text()).unwrap_or_default();
            if dl.get("platform").map(|p| p.text()).as_deref() == Some("WINDOWS")
                && url.ends_with(".exe")
            {
                return Ok(url);
            }
        }
    }
    Err(Fail::Exit(format!(
        "Unity's release API has no Windows editor installer for {version}"
    )))
}

/// The JSON the release API answers, read with the crate's pyjson.
mod serde_like {
    use super::Fail;
    use crate::pyjson::{self, Json};

    pub struct Value(pub Json);

    pub fn parse(b: &[u8]) -> Result<Value, Fail> {
        pyjson::loads_bytes(b)
            .map(Value)
            .map_err(|e| Fail::Error(format!("json.decoder.JSONDecodeError: {e}")))
    }

    impl Value {
        pub fn get(&self, k: &str) -> Option<Value> {
            self.0.get(k).cloned().map(Value)
        }

        pub fn list(&self) -> Vec<Value> {
            self.0
                .as_list()
                .map(|l| l.iter().cloned().map(Value).collect())
                .unwrap_or_default()
        }

        /// str(value)
        pub fn text(&self) -> String {
            self.0.py_str()
        }
    }
}

/// An installer path relative to the editor folder ('$_OUTDIR/MonoBleedingEdge/...').
fn rel(path: &str) -> String {
    match path.split_once('/') {
        Some((first, rest)) if first.starts_with('$') => rest.to_string(),
        None if path.starts_with('$') => String::new(),
        _ => path.to_string(),
    }
}

/// (major, minor, build, revision) of a .NET assembly, or None.
pub fn assembly_version(path: &str) -> Option<Vec<u32>> {
    let a = Assembly::open(path).ok()?;
    let r = a.rows(0x20).ok()?;
    r.first().and_then(|r| r.get(1..5)).map(<[u32]>::to_vec)
}

fn dotted(v: &[u32]) -> String {
    v.iter().map(u32::to_string).collect::<Vec<_>>().join(".")
}

/// The editor installer's class library folder for a Mono build of this version on the Android
/// player (unityjit-linux from 2021.2, unityjit 2018.1 - 2021.1), or None (older, or .NET 3.5).
pub fn facade_profile(v: &Version, managed: Option<&str>) -> Option<&'static str> {
    if v.line() >= (2021, 2) {
        return Some(LINUX);
    }
    if v.line() < (2018, 1) {
        return None;
    }
    if let Some(m) = managed.filter(|m| !m.is_empty()) {
        let major = assembly_version(&py::join(m, "mscorlib.dll")).map_or(4, |v| v[0]);
        if major < 4 {
            return None;
        }
    }
    Some(UNITYJIT)
}

/// The engine modules of a `player fetch` folder (the Mono player's Managed/; the IL2CPP
/// player's, fetched with --managed, for the Mono route on it).
pub fn player_managed(player: &str) -> String {
    let mono = py::join(player, "Variations/mono/Managed");
    let il2cpp = py::join(player, "Variations/il2cpp/Managed");
    if !Path::new(&mono).is_dir() && Path::new(&il2cpp).is_dir() {
        il2cpp
    } else {
        mono
    }
}

/// {assembly: [engine modules]}: what the player's engine modules reference that neither they
/// nor the game's Managed folder have (android_data.missing_references).
pub fn missing(managed: &str, player: &str) -> Vec<(String, Vec<String>)> {
    let pm = player_managed(player);
    let mut names = BTreeSet::new();
    for d in [managed, pm.as_str()] {
        if Path::new(d).is_dir() {
            for n in listdir(d).unwrap_or_default() {
                if n.to_lowercase().ends_with(".dll") {
                    names.insert(n);
                }
            }
        }
    }
    crate::android_data::missing_references(&pm, &names)
}

/// {installer path relative to the editor folder: block offset} (the last of a path winning),
/// in the installer's order.
fn index(ns: &Nsis) -> Result<Vec<(String, u64)>, Fail> {
    let mut out: Vec<(String, u64)> = Vec::new();
    let mut at: BTreeMap<String, usize> = BTreeMap::new();
    for (path, off, _) in ns.files()? {
        let r = rel(&path);
        match at.get(&r) {
            Some(&i) => out[i].1 = off,
            None => {
                at.insert(r.clone(), out.len());
                out.push((r, off));
            }
        }
    }
    Ok(out)
}

fn lookup(files: &[(String, u64)], k: &str) -> Option<u64> {
    files.iter().find(|(p, _)| p == k).map(|x| x.1)
}

fn write_part(dst: &str, data: &[u8]) -> Result<(), Fail> {
    let part = format!("{dst}.part");
    fs::write(&part, data)?;
    Ok(())
}

/// The unityjit-linux twins of the game's unityjit-win32 class libraries, written to out (a DLL
/// of another assembly version than Unity's is the game's own and is kept).
fn replacements(
    managed: &str,
    ns: &mut Nsis,
    out: &str,
    log: &mut dyn FnMut(&str),
    files: &[(String, u64)],
) -> Result<Vec<String>, Fail> {
    let mut names: Vec<String> = listdir(managed)?
        .into_iter()
        .filter(|n| n.to_lowercase().ends_with(".dll"))
        .collect();
    names.sort();
    fs::create_dir_all(out)?;
    let mut got = Vec::new();
    for n in names {
        let facade = format!("Facades/{n}");
        let Some(sub) = [n.as_str(), facade.as_str()].into_iter().find(|s| {
            lookup(files, &format!("{WIN}{s}")).is_some()
                && lookup(files, &format!("{LINUX}{s}")).is_some()
        }) else {
            continue;
        };
        let dst = py::join(out, &n);
        let data = ns.extract(lookup(files, &format!("{LINUX}{sub}")).unwrap())?;
        write_part(&dst, &data)?;
        let part = format!("{dst}.part");
        let theirs = assembly_version(&py::join(managed, &n));
        let ours = assembly_version(&part);
        if let (Some(t), Some(o)) = (&theirs, &ours) {
            if t != o {
                fs::remove_file(&part)?;
                log(&format!(
                    "winbcl: {n}: kept the game's own (version {}; Unity's is {})",
                    dotted(t),
                    dotted(o)
                ));
                continue;
            }
        }
        fs::rename(&part, &dst)?;
        got.push(n);
    }
    log(&format!(
        "winbcl: {} class libraries as unityjit-linux ({:.1} MB read): {}",
        got.len(),
        ns.s.fetched as f64 / 1e6,
        got.join(", ")
    ));
    Ok(got)
}

/// Unity's own copies (profile, or its Facades/) of the assemblies named, written to out:
/// (names written, names not in the profile).
fn facades(
    refs: &[String],
    profile: &str,
    ns: &mut Nsis,
    out: &str,
    log: &mut dyn FnMut(&str),
    files: &[(String, u64)],
) -> Result<(Vec<String>, Vec<String>), Fail> {
    let mut low: BTreeMap<String, String> = BTreeMap::new();
    for (k, _) in files {
        if k.starts_with(profile) {
            low.insert(k.to_lowercase(), k.clone());
        }
    }
    fs::create_dir_all(out)?;
    let (mut got, mut other) = (Vec::new(), Vec::new());
    let mut sorted: Vec<&String> = refs.iter().collect();
    sorted.sort();
    for r in sorted {
        let n = format!("{r}.dll");
        let key = [format!("{profile}{n}"), format!("{profile}Facades/{n}")]
            .into_iter()
            .find_map(|k| low.get(&k.to_lowercase()).cloned());
        let Some(key) = key else {
            other.push(r.clone());
            continue;
        };
        let dst = py::join(out, &n);
        let data = ns.extract(lookup(files, &key).unwrap())?;
        write_part(&dst, &data)?;
        fs::rename(format!("{dst}.part"), &dst)?;
        let v = assembly_version(&dst);
        log(&format!(
            "winbcl: {n}: Unity's {}{}",
            &key[profile.len()..],
            v.map_or(String::new(), |v| format!(" (version {})", dotted(&v)))
        ));
        got.push(n);
    }
    Ok((got, other))
}

fn not_unity_json(m: &[(String, Vec<String>)]) -> Json {
    Json::Obj(
        m.iter()
            .map(|(k, v)| {
                (
                    k.clone(),
                    Json::List(v.iter().map(|x| Json::str(x.clone())).collect()),
                )
            })
            .collect(),
    )
}

/// What the build needs of Unity's class libraries for the Android player, written to out: the
/// report (winbcl.supply's dict) and the names written.
pub fn supply(
    data: &str,
    out: &str,
    version: Option<&str>,
    installer: Option<&str>,
    player: Option<&str>,
    with_facades: bool,
    log: &mut dyn FnMut(&str),
) -> Result<(Json, Vec<String>), Fail> {
    let managed = py::join(data, "Managed");
    let v = match version.filter(|v| !v.is_empty()) {
        Some(t) => Version::parse(t).map_err(|e| Fail::Error(format!("ValueError: {e}")))?,
        None => UV::of_data(data).map_err(|e| Fail::Error(e.0))?,
    };
    let text = v.to_string();
    let mut rep = Json::obj();
    rep.set("version", Json::str(text.clone()));
    rep.set("class_libraries", Json::List(Vec::new()));
    rep.set("facades", Json::List(Vec::new()));
    rep.set("not_unity", Json::obj());
    rep.set("read_mb", Json::Float(0.0));
    rep.set("installer", Json::Null);
    let win = v.line() >= (2021, 2) && is_win32_profile(&managed);
    let mut want = match player.filter(|p| with_facades && !p.is_empty()) {
        Some(p) => missing(&managed, p),
        None => Vec::new(),
    };
    let profile = if want.is_empty() {
        None
    } else {
        facade_profile(&v, Some(&managed))
    };
    let mut not_unity = Vec::new();
    if !want.is_empty() && profile.is_none() {
        not_unity = want.clone();
        rep.set("not_unity", not_unity_json(&not_unity));
        let mut names: Vec<&str> = want.iter().map(|x| x.0.as_str()).collect();
        names.sort();
        log(&format!(
            "winbcl: the engine modules reference {}, not in the build; Unity {text} ({}) has no class library profile to take them from",
            names.join(", "),
            if v.line() >= (2018, 1) { ".NET 3.5" } else { "before 2018.1" }
        ));
        want.clear();
    }
    if !win && want.is_empty() {
        if not_unity.is_empty() {
            log(&format!(
                "winbcl: nothing to do (the class libraries are not the unityjit-win32 ones{}",
                if player.is_some_and(|p| !p.is_empty()) {
                    ", and the build has every assembly the engine modules reference)"
                } else {
                    ")"
                }
            ));
        }
        return Ok((rep, Vec::new()));
    }
    // (the installer's URL, then the installer: a failure of either leaves the facades out)
    let mut said = None;
    let opened = (|| -> Result<Nsis, Fail> {
        let src = match installer.filter(|i| !i.is_empty()) {
            Some(i) => i.to_string(),
            None => editor_installer(&text)?,
        };
        said = Some(src.clone());
        log(&format!(
            "winbcl: Unity {text}'s Windows editor installer: {src}"
        ));
        Ok(Nsis::open_logged(&src)?)
    })();
    if let Some(src) = said {
        rep.set("installer", Json::str(src));
    }
    let mut ns = match opened {
        Ok(ns) => ns,
        Err(e) => {
            if win {
                return Err(e);
            }
            let m = match &e {
                Fail::Exit(m) | Fail::Error(m) => m.clone(),
            };
            rep.set("error", Json::str(m.clone()));
            let mut names: Vec<&str> = want.iter().map(|x| x.0.as_str()).collect();
            names.sort();
            log(&format!(
                "winbcl: could not read the installer ({m}): {} left out",
                names.join(", ")
            ));
            return Ok((rep, Vec::new()));
        }
    };
    let files = index(&ns)?;
    let mut written = Vec::new();
    if win {
        let cl = replacements(&managed, &mut ns, out, log, &files)?;
        rep.set(
            "class_libraries",
            Json::List(cl.iter().map(|n| Json::str(n.clone())).collect()),
        );
        written.extend(cl);
    }
    if !want.is_empty() {
        let refs: Vec<String> = want.iter().map(|x| x.0.clone()).collect();
        let (got, other) = facades(&refs, profile.unwrap(), &mut ns, out, log, &files)?;
        rep.set(
            "facades",
            Json::List(got.iter().map(|n| Json::str(n.clone())).collect()),
        );
        let nu: Vec<(String, Vec<String>)> = other
            .iter()
            .map(|r| {
                (
                    r.clone(),
                    want.iter().find(|x| &x.0 == r).unwrap().1.clone(),
                )
            })
            .collect();
        rep.set("not_unity", not_unity_json(&nu));
        if !other.is_empty() {
            log(&format!(
                "winbcl: not Unity's class libraries (a package's or the developer's assemblies the build lacks; their types fail to load): {}",
                nu.iter()
                    .map(|(r, m)| format!(
                        "{r} (referenced by {})",
                        m.iter().take(3).cloned().collect::<Vec<_>>().join(", ")
                    ))
                    .collect::<Vec<_>>()
                    .join(", ")
            ));
        }
        written.extend(got);
    }
    // round(fetched / 1e6, 2)
    let mb = py_round2(ns.s.fetched as f64 / 1e6);
    rep.set("read_mb", Json::Float(mb));
    log(&format!("winbcl: {mb:.1} MB read from the installer"));
    Ok((rep, written))
}

/// Python's round(x, 2) (correctly rounded, half to even on the exact value).
fn py_round2(x: f64) -> f64 {
    let s = format!("{x:.2}");
    s.parse().unwrap_or(x)
}

trait OpenLogged {
    fn open_logged(src: &str) -> pkgzip::Result<Nsis>;
}

impl OpenLogged for Nsis {
    fn open_logged(src: &str) -> pkgzip::Result<Nsis> {
        Nsis::open(src, Box::new(|m| eprintln!("{m}")))
    }
}

/// The command (winbcl.main after its arguments): the report, the options it prints.
#[allow(clippy::too_many_arguments)]
pub fn main(
    data: &str,
    out: &str,
    version: Option<&str>,
    installer: Option<&str>,
    player: Option<&str>,
    no_facades: bool,
    json: Option<&str>,
) -> Result<(), Fail> {
    let (rep, written) = supply(
        data,
        out,
        version,
        installer,
        player,
        !no_facades,
        &mut |m| eprintln!("{m}"),
    )?;
    if let Some(j) = json {
        fs::write(j, pyjson::dumps(&rep, Some(1)))?;
    }
    let opts: Vec<String> = written
        .iter()
        .map(|n| format!("--managed-file {}", py::join(out, n)))
        .collect();
    println!("{}", opts.join(" "));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paths_and_profiles() {
        assert_eq!(
            rel("$_OUTDIR/MonoBleedingEdge/lib/x.dll"),
            "MonoBleedingEdge/lib/x.dll"
        );
        assert_eq!(rel("Editor/x.dll"), "Editor/x.dll");
        let v = |t: &str| Version::parse(t).unwrap();
        assert_eq!(facade_profile(&v("6000.0.61f1"), None), Some(LINUX));
        assert_eq!(facade_profile(&v("2019.4.40f1"), None), Some(UNITYJIT));
        assert_eq!(facade_profile(&v("2017.4.9f1"), None), None);
        assert_eq!(py_round2(6.125), 6.12);
        assert_eq!(py_round2(13.3456), 13.35);
    }

    /// A build with nothing to do: no installer read, an empty line of options.
    #[test]
    fn nothing_to_do() {
        let d = std::env::temp_dir().join(format!("pmu-winbcl-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(d.join("Data/Managed")).unwrap();
        fs::write(
            d.join("Data/Managed/mscorlib.dll"),
            b"not the win32 profile",
        )
        .unwrap();
        let mut said = Vec::new();
        let (rep, written) = supply(
            &d.join("Data").to_string_lossy(),
            &d.join("bcl").to_string_lossy(),
            Some("6000.0.61f1"),
            None,
            Some(&d.join("player").to_string_lossy()),
            true,
            &mut |m| said.push(m.to_string()),
        )
        .unwrap();
        assert!(written.is_empty());
        assert_eq!(
            said,
            ["winbcl: nothing to do (the class libraries are not the unityjit-win32 ones, and the build has every assembly the engine modules reference)"]
        );
        assert_eq!(
            pyjson::dumps(&rep, None),
            r#"{"version": "6000.0.61f1", "class_libraries": [], "facades": [], "not_unity": {}, "read_mb": 0.0, "installer": null}"#
        );
        assert!(!d.join("bcl").exists());
        let _ = fs::remove_dir_all(&d);
    }
}
