//! il2cpp.py's toolchain: Unity's Editor/Data parts (il2cpp, its sources, a class library folder)
//! from the Windows editor installer, the Android NDK's target files from Google's zip (its x86-64
//! programs replaced by ndk-clang.sh), the IL2CPP Android player; the Unity version the build uses.
use crate::fetch::{self, RemoteZip};
use crate::util::{self, at, join, joins};
use crate::{bee, exit, log, mix, Error, Result};
use androiddata::pyjson::{self, Json};
use androiddata::uversion::Version;
use regex::Regex;
use std::sync::OnceLock;

/// tools/il2cpp/ndk-clang.sh (a copy: crates/il2cpp/src/ndk-clang.sh, checked by a test), the
/// stand-in for the NDK's x86-64 compilers and binutils.
pub const NDK_CLANG: &str = include_str!("ndk-clang.sh");

/// (il2cpp's architecture, the NDK's triple, the API level) of an ABI.
pub fn abi_info(abi: &str) -> (&'static str, &'static str, u32) {
    match abi {
        "armeabi-v7a" => ("ARMv7", "arm-linux-androideabi", 19),
        _ => ("ARM64", "aarch64-linux-android", 21),
    }
}

pub const ABIS: [&str; 2] = ["arm64-v8a", "armeabi-v7a"];
pub const EDITOR_API: &str = "https://services.api.unity.com/unity/editor/release/v1/releases";
pub const NDK_BASE: &str = "https://dl.google.com/android/repository/";
/// (first Unity line, name, Pkg.Revision, zip)
pub const NDKS: [((u32, u32), &str, &str, &str); 5] = [
    (
        (2017, 1),
        "r13b",
        "13.1.3345770",
        "android-ndk-r13b-linux-x86_64.zip",
    ),
    (
        (2018, 3),
        "r16b",
        "16.1.4479499",
        "android-ndk-r16b-linux-x86_64.zip",
    ),
    (
        (2019, 3),
        "r19",
        "19.0.5232133",
        "android-ndk-r19-linux-x86_64.zip",
    ),
    (
        (2021, 1),
        "r21d",
        "21.3.6528147",
        "android-ndk-r21d-linux-x86_64.zip",
    ),
    (
        (2022, 2),
        "r23b",
        "23.1.7779620",
        "android-ndk-r23b-linux.zip",
    ),
];
pub const GNU_NDKS: [&str; 2] = ["r13b", "r16b"];
pub const BEE_NDKS: [&str; 2] = ["r21d", "r23b"];
pub const METADATA_MAGIC: u32 = 0xFAB11BAF;
/// the first 2017.4 release with a 64-bit ARM IL2CPP player, and the line's last
pub const ARM64_2017: (u32, &str) = (16, "2017.4.40f1");
pub const BCL_DIR: &str = "MonoBleedingEdge/lib/mono/unityaot";
pub const MAP_FILE_UNTIL: (u32, u32) = (2018, 4);
pub const EDITOR_PARTS: [&str; 2] = ["il2cpp/il2cpp_root", "MonoBleedingEdge/etc/mono/config"];

fn editor_cdn(rev: &str, ver: &str) -> String {
    format!("https://download.unity3d.com/download_unity/{rev}/Windows64EditorInstaller/UnitySetup64-{ver}.exe")
}

/// api_level: 21 on arm64; on armv7 16 with NDK r13b / r16b, else 19.
pub fn api_level(rev: &str, abi: &str) -> u32 {
    if abi == "armeabi-v7a" && GNU_NDKS.contains(&rev) {
        16
    } else {
        abi_info(abi).2
    }
}

/// ndk_apis: the API levels whose libraries are fetched (22 too for Bee's NDKs).
pub fn ndk_apis(rev: &str, abi: &str) -> Vec<u32> {
    let mut v = vec![api_level(rev, abi)];
    if BEE_NDKS.contains(&rev) && !v.contains(&22) {
        v.push(22);
    }
    v.sort();
    v
}

/// ndk_for: (name, Pkg.Revision, zip) of the NDK a Unity line's IL2CPP wants.
pub fn ndk_for(version: &Version) -> Result<(&'static str, &'static str, &'static str)> {
    let mut best = None;
    for (first, name, rev, z) in NDKS {
        if version.line() >= first {
            best = Some((name, rev, z));
        }
    }
    best.ok_or_else(|| {
        Error::Unsupported(format!(
            "Unity {version}: IL2CPP Android builds of this tool start at 2017.4"
        ))
    })
}

/// editor_installer: the Windows editor installer's URL (Unity's release API).
pub fn editor_installer(version: &str) -> Result<String> {
    let q = format!("version={}&limit=1", pkgzip::http::quote_plus(version));
    let body = pkgzip::http::fetch(&format!("{EDITOR_API}?{q}"))?;
    let d = pyjson::loads_bytes(&body)
        .map_err(|e| Error::Fail(format!("json.decoder.JSONDecodeError: {e}")))?;
    let empty = Vec::new();
    for rec in d.get("results").and_then(Json::as_list).unwrap_or(&empty) {
        if rec.get("version").and_then(Json::as_str) != Some(version) {
            continue;
        }
        for dl in rec
            .get("downloads")
            .and_then(Json::as_list)
            .unwrap_or(&empty)
        {
            let url = dl.get("url").map(Json::py_str).unwrap_or_default();
            if dl.get("platform").and_then(Json::as_str) == Some("WINDOWS") && url.ends_with(".exe")
            {
                return Ok(url);
            }
        }
        if let Some(r) = rec.get("shortRevision").filter(|r| r.truthy()) {
            return Ok(editor_cdn(&r.py_str(), version));
        }
    }
    exit(format!(
        "Unity does not list version {version} (check `python -m pmunity version <data>`)"
    ))
}

/// editor_rel: an editor installer path -> its path under Editor/Data, else None.
pub fn editor_rel(p: &str) -> Option<String> {
    if let Some(r) = p.strip_prefix("$_OUTDIR/") {
        return Some(r.to_string());
    }
    p.find("/Editor/Data/")
        .map(|i| p[i + "/Editor/Data/".len()..].to_string())
}

/// bcl_wanted: a class library folder's assemblies and its Mono's etc folder.
pub fn bcl_wanted(rel: &str, bcl: &str) -> bool {
    (rel.starts_with(&format!("{bcl}/")) && rel.ends_with(".dll"))
        || rel.starts_with(&format!("{}/etc/", bcl.split('/').next().unwrap_or("")))
}

/// editor_wanted: il2cpp/, MonoBleedingEdge/etc and a class library folder.
pub fn editor_wanted(rel: &str, bcl: &str) -> bool {
    if rel.starts_with("MonoBleedingEdge/etc/") || bcl_wanted(rel, bcl) {
        return true;
    }
    if !rel.starts_with("il2cpp/") {
        return false;
    }
    !(rel.starts_with("il2cpp/MapFileParser/")
        || rel.starts_with("il2cpp/external/mono/mono/tests/")
        || rel.ends_with(".pdb")
        || rel.ends_with(".mdb"))
}

/// bcl_complete
pub fn bcl_complete(dest: &str, bcl: &str) -> bool {
    if util::isfile(&joins(&[dest, bcl, ".complete"])) {
        return true;
    }
    bcl == BCL_DIR
        && util::isfile(&join(dest, ".complete"))
        && util::isfile(&joins(&[dest, bcl, "mscorlib.dll"]))
}

/// runtime_pack_files: the files a self-contained build's deps.json takes from .NET's runtime
/// pack (lower case names).
pub fn runtime_pack_files(deps: &Json) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let empty = Vec::new();
    let libs = deps
        .get("libraries")
        .and_then(Json::as_obj)
        .unwrap_or(&empty);
    let targets = deps.get("targets").and_then(Json::as_obj).unwrap_or(&empty);
    for (name, lib) in libs {
        if lib.get("type").and_then(Json::as_str) != Some("runtimepack") {
            continue;
        }
        for (_, tgt) in targets {
            for asset in ["runtime", "native"] {
                if let Some(Json::Obj(files)) = tgt.get(name).and_then(|t| t.get(asset)) {
                    for (f, _) in files {
                        let n = f.rsplit('/').next().unwrap_or(f).to_lowercase();
                        if !out.contains(&n) {
                            out.push(n);
                        }
                    }
                }
            }
        }
    }
    out
}

fn read_json(p: &str) -> Result<Json> {
    let b = util::read(p)?;
    let b = b.strip_prefix(b"\xef\xbb\xbf").unwrap_or(&b);
    pyjson::loads_bytes(b).map_err(|e| Error::Fail(format!("json.decoder.JSONDecodeError: {e}")))
}

/// fetch_editor: il2cpp/, MonoBleedingEdge/etc and one class library folder of the Windows editor
/// installer (a home with the first two gets just another class library folder).
pub fn fetch_editor(
    version: &Version,
    dest: &str,
    installer: Option<&str>,
    bcl: &str,
) -> Result<String> {
    let base = util::isfile(&join(dest, ".complete"))
        && EDITOR_PARTS.iter().all(|p| util::exists(&join(dest, p)));
    if base && bcl_complete(dest, bcl) {
        return Ok(dest.to_string());
    }
    if base {
        let installer = match installer {
            Some(i) => i.to_string(),
            None => match read_json(&join(dest, ".complete"))?
                .get("installer")
                .filter(|v| v.truthy())
            {
                Some(v) => v.py_str(),
                None => editor_installer(&version.text)?,
            },
        };
        log(&format!(
            "editor {version}: the class libraries of {bcl} from {installer}"
        ));
        let mut ns = fetch::open_nsis(&installer)?;
        let b = bcl.to_string();
        let got = fetch::nsis_fetch(
            &mut ns,
            &|p| editor_rel(p).filter(|r| bcl_wanted(r, &b)),
            dest,
        )?;
        if !got.contains(&format!("{bcl}/mscorlib.dll")) {
            return exit(format!(
                "{installer}: no {bcl}/mscorlib.dll in this installer"
            ));
        }
        util::write_text(&joins(&[dest, bcl, ".complete"]), "")?;
        return Ok(dest.to_string());
    }
    let installer = match installer {
        Some(i) => i.to_string(),
        None => editor_installer(&version.text)?,
    };
    log(&format!(
        "editor {version}: il2cpp and the class libraries of {bcl} from {installer}"
    ));
    let mut ns = fetch::open_nsis(&installer)?;
    // dir -> the runtime pack's files (lower case) its deps.json names
    let mut deps: Vec<(String, Vec<String>)> = Vec::new();
    let rels = fetch::nsis_fetch(
        &mut ns,
        &|p| editor_rel(p).filter(|r| r.starts_with("il2cpp/build/") && r.ends_with(".deps.json")),
        dest,
    )?;
    for rel in rels {
        let files = runtime_pack_files(&read_json(&join(dest, &rel))?);
        let d = util::dirname(&rel).to_string();
        let i = match deps.iter().position(|x| x.0 == d) {
            Some(i) => i,
            None => {
                deps.push((d, Vec::new()));
                deps.len() - 1
            }
        };
        for f in files {
            if !deps[i].1.contains(&f) {
                deps[i].1.push(f);
            }
        }
    }
    let b = bcl.to_string();
    let deps_ref = &deps;
    let select = |p: &str| -> Option<String> {
        let r = editor_rel(p)?;
        if !editor_wanted(&r, &b) || r.ends_with(".deps.json") {
            return None;
        }
        let name = r.rsplit('/').next().unwrap_or(&r).to_lowercase();
        let d = util::dirname(&r);
        if deps_ref.iter().any(|(k, v)| k == d && v.contains(&name)) {
            return None;
        }
        Some(r)
    };
    let got = fetch::nsis_fetch(&mut ns, &select, dest)?;
    if !got.iter().any(|g| g.starts_with("il2cpp/libil2cpp/")) {
        return exit(format!(
            "{installer}: no Editor/Data/il2cpp in this installer"
        ));
    }
    if !got.contains(&format!("{bcl}/mscorlib.dll")) {
        return exit(format!(
            "{installer}: no {bcl}/mscorlib.dll in this installer"
        ));
    }
    util::write_text(&joins(&[dest, bcl, ".complete"]), "")?;
    // framework-dependent from here on: the deps files would name the runtime pack's files
    for (d, _) in &deps {
        for n in util::listdir(&join(dest, d))? {
            if n.ends_with(".deps.json") {
                let p = joins(&[dest, d, &n]);
                util::replace(&p, &format!("{p}.orig"))?;
            }
        }
    }
    let root = joins(&[dest, "il2cpp", "il2cpp_root"]);
    std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&root)
        .map_err(at(&root))?;
    let mut left: Vec<String> = deps.iter().flat_map(|(_, v)| v.clone()).collect();
    left.sort();
    left.dedup();
    let info = Json::Obj(vec![
        ("version".into(), Json::str(&version.text)),
        ("installer".into(), Json::str(&installer)),
        ("files".into(), Json::Int(got.len() as i128)),
        (
            "runtime_pack_files_left_out".into(),
            Json::List(left.into_iter().map(Json::Str).collect()),
        ),
    ]);
    util::write_text(&join(dest, ".complete"), &pyjson::dumps(&info, Some(0)))?;
    Ok(dest.to_string())
}

fn re(p: &str) -> Regex {
    Regex::new(p).unwrap()
}

/// ndk_wanted: an NDK zip member -> its path without the top folder when il2cpp's Android build
/// needs it (`apis`: the per-API library folders, default ndk_apis).
pub fn ndk_wanted(name: &str, rev: &str, abi: &str, apis: Option<&[u32]>) -> Option<String> {
    let triple = abi_info(abi).1;
    let api = api_level(rev, abi);
    let default = ndk_apis(rev, abi);
    let apis = apis.unwrap_or(&default);
    let (_, rel) = name.split_once('/')?;
    if rel == "source.properties" {
        return Some(rel.into());
    }
    let llvm = "toolchains/llvm/prebuilt/linux-x86_64/";
    if GNU_NDKS.contains(&rev) {
        let arch = if abi == "arm64-v8a" { "arm64" } else { "arm" };
        let gcc = format!("toolchains/{triple}-4.9/prebuilt/linux-x86_64/");
        let stl = "sources/cxx-stl/gnu-libstdc++/4.9/";
        if rel.starts_with("sysroot/usr/include/")
            || rel.starts_with(&format!("platforms/android-{api}/arch-{arch}/"))
        {
            return Some(rel.into());
        }
        if rel.starts_with(&format!("{stl}include/"))
            || rel.starts_with(&format!("{stl}libs/{abi}/"))
        {
            return (!rel.ends_with(".so")).then(|| rel.into());
        }
        if rel.starts_with(&format!("{gcc}lib/gcc/{triple}/"))
            || rel.starts_with(&format!("{gcc}{triple}/lib"))
        {
            return (!rel.ends_with('/')).then(|| rel.into());
        }
        return None;
    }
    let r = rel.strip_prefix(llvm)?;
    if r.starts_with("sysroot/usr/include/") || r.starts_with("sysroot/usr/local/include/") {
        return Some(rel.into());
    }
    let libdir = format!("sysroot/usr/lib/{triple}/");
    if let Some(rest) = r.strip_prefix(&libdir) {
        let ok = !rest.contains('/')
            || apis
                .iter()
                .any(|a| a.to_string() == rest.split('/').next().unwrap_or(""));
        return ok.then(|| rel.into());
    }
    if r.starts_with(&format!("lib/gcc/{triple}/")) || r.starts_with(&format!("{triple}/lib")) {
        return (!r.contains("ldscripts")).then(|| rel.into());
    }
    static CLANG_RT: OnceLock<Regex> = OnceLock::new();
    let rx = CLANG_RT.get_or_init(|| re(r"^lib64/clang/[^/]+/lib/linux/"));
    let arch0 = triple.split('-').next().unwrap_or("");
    if rx.is_match(r)
        && (r.contains("android") || r.contains(&format!("/{arch0}/")) || r.contains("/arm/"))
    {
        return Some(rel.into());
    }
    None
}

/// ndk_api_dir: the per-API library folder of the unified sysroot (r19+).
pub fn ndk_api_dir(ndk: &str, rev: &str, abi: &str, api: u32) -> Option<String> {
    if GNU_NDKS.contains(&rev) {
        return None;
    }
    Some(joins(&[
        ndk,
        "toolchains",
        "llvm",
        "prebuilt",
        "linux-x86_64",
        "sysroot",
        "usr",
        "lib",
        abi_info(abi).1,
        &api.to_string(),
    ]))
}

fn ints(v: &[u32]) -> Json {
    Json::List(v.iter().map(|&a| Json::Int(a as i128)).collect())
}

/// fetch_ndk: the NDK's target files (`extra_apis`: more API levels' libraries).
pub fn fetch_ndk(
    rev_name: &str,
    zipname: &str,
    abi: &str,
    dest: &str,
    extra_apis: &[u32],
) -> Result<String> {
    let mark = join(dest, &format!(".complete-{abi}"));
    let url = format!("{NDK_BASE}{zipname}");
    let mut apis = ndk_apis(rev_name, abi);
    for a in extra_apis {
        if !apis.contains(a) {
            apis.push(*a);
        }
    }
    apis.sort();
    if util::isfile(&mark) {
        let more: Vec<u32> = apis
            .iter()
            .copied()
            .filter(|&a| {
                ndk_api_dir(dest, rev_name, abi, a)
                    .is_some_and(|d| !util::isfile(&join(&d, "libc.so")))
            })
            .collect();
        if more.is_empty() {
            return Ok(dest.to_string());
        }
        log(&format!(
            "NDK {rev_name} ({abi}): the API {} libraries from {url}",
            more.iter()
                .map(u32::to_string)
                .collect::<Vec<_>>()
                .join(", ")
        ));
        let triple = abi_info(abi).1;
        let dirs: Vec<String> = more.iter().map(|a| format!("/{triple}/{a}/")).collect();
        let z = RemoteZip::open(&url)?;
        let got = z.extract(
            &|n| {
                ndk_wanted(n, rev_name, abi, Some(&more))
                    .filter(|r| dirs.iter().any(|x| r.contains(x.as_str())))
            },
            dest,
        )?;
        let mut info = read_json(&mark)?;
        info.set("apis", ints(&apis));
        let files = info.get("files").and_then(Json::as_int).unwrap_or(0) + got.len() as i128;
        info.set("files", Json::Int(files));
        util::write_text(&mark, &pyjson::dumps(&info, None))?;
        return Ok(dest.to_string());
    }
    log(&format!(
        "NDK {rev_name} ({abi}): sysroot, C++ runtime and libgcc from {url}"
    ));
    let z = RemoteZip::open(&url)?;
    let got = z.extract(&|n| ndk_wanted(n, rev_name, abi, None), dest)?;
    if !got.iter().any(|g| g == "source.properties") {
        return exit(format!("{url}: not an NDK archive (no source.properties)"));
    }
    install_ndk_shims(dest, abi)?;
    let info = Json::Obj(vec![
        ("ndk".into(), Json::str(rev_name)),
        ("zip".into(), Json::str(&url)),
        ("files".into(), Json::Int(got.len() as i128)),
        ("apis".into(), ints(&apis)),
    ]);
    util::write_text(&mark, &pyjson::dumps(&info, None))?;
    Ok(dest.to_string())
}

/// android_note: (API level, NDK revision) of an NDK-built library's .note.android.ident.
pub fn android_note(path: &str) -> (Option<u32>, Option<String>) {
    let Ok(b) = std::fs::read(path) else {
        return (None, None);
    };
    let pat = b"\x08\x00\x00\x00\x84\x00\x00\x00\x01\x00\x00\x00Android\x00";
    let Some(i) = pkgzip::py::find(&b, pat) else {
        return (None, None);
    };
    let Some(api) = b.get(i + 20..i + 24) else {
        return (None, None);
    };
    let api = u32::from_le_bytes(api.try_into().unwrap());
    let s = &b[(i + 24).min(b.len())..(i + 88).min(b.len())];
    let s = s.split(|&c| c == 0).next().unwrap_or(&[]);
    let rev = String::from_utf8_lossy(s).into_owned();
    (Some(api), (!rev.is_empty()).then_some(rev))
}

/// install_ndk_shims: ndk-clang.sh where il2cpp's Android toolchain runs the NDK's programs (a
/// file already the same and executable is left as it is: its time stays).
pub fn install_ndk_shims(ndk: &str, abi: &str) -> Result<()> {
    let triple = abi_info(abi).1;
    let llvm = "toolchains/llvm/prebuilt/linux-x86_64/";
    let mut paths = vec![format!("{llvm}bin/clang"), format!("{llvm}bin/clang++")];
    for tool in ["objcopy", "strip", "ar", "nm"] {
        paths.push(format!(
            "toolchains/{triple}-4.9/prebuilt/linux-x86_64/bin/{triple}-{tool}"
        ));
        paths.push(format!("{llvm}bin/{triple}-{tool}"));
        paths.push(format!("{llvm}{triple}/bin/{tool}"));
        paths.push(format!("{llvm}bin/llvm-{tool}"));
    }
    for p in paths {
        let dst = join(ndk, &p);
        if std::fs::read(&dst).is_ok_and(|d| d == NDK_CLANG.as_bytes()) && util::executable(&dst) {
            continue;
        }
        util::write_file(&dst, NDK_CLANG.as_bytes(), Some(0o755))?;
    }
    Ok(())
}

pub fn player_wanted(abi: &str) -> Vec<String> {
    vec![
        "Variations/il2cpp/Managed/*.dll".into(),
        format!("Variations/il2cpp/Release/StaticLibs/{abi}/baselib.a"),
    ]
}

/// fetch_player: the IL2CPP player package (`player fetch --backend il2cpp`) and what the build
/// reads of it (the IL2CPP UnityEngine assemblies, baselib). Returns its player.json.
pub fn fetch_player(version: &Version, pkg: &str, abi: &str) -> Result<Json> {
    let pj = join(pkg, "player.json");
    let mut info = if util::isfile(&pj) {
        Some(read_json(&pj)?)
    } else {
        None
    };
    if let Some(i) = &info {
        let b = i.get("backend").map(Json::py_str).unwrap_or("None".into());
        let a = i.get("abi").map(Json::py_str).unwrap_or("None".into());
        if b != "il2cpp" || a != abi {
            return exit(format!(
                "{pkg} holds the {b} / {a} player; use another --pkg for the IL2CPP {abi} one"
            ));
        }
    }
    if info.is_none() {
        pkgzip::player::fetch(
            &version.text,
            pkg,
            "il2cpp",
            abi,
            None,
            false,
            false,
            false,
            &mut |m| log(m),
        )?;
        info = Some(read_json(&pj)?);
    }
    let info = info.unwrap();
    let man = joins(&[pkg, "Variations", "il2cpp", "Managed"]);
    let has_dll = util::isdir(&man)
        && util::listdir(&man)
            .unwrap_or_default()
            .iter()
            .any(|n| n.ends_with(".dll"));
    if !has_dll {
        log(&format!(
            "player {version}: the IL2CPP UnityEngine assemblies and static libraries"
        ));
        let globs = player_wanted(abi);
        let mut src = info.get("installer").map(Json::py_str).unwrap_or_default();
        if !(src.starts_with("http://") || src.starts_with("https://") || util::isfile(&src)) {
            let rec = pkgzip::player::release(&version.text)?;
            let rec = rec.ok_or_else(|| {
                Error::Fail("AttributeError: 'NoneType' object has no attribute 'get'".into())
            })?;
            let u = pkgzip::player::installer_url(&rec)?;
            log(&format!("player {version}: {src} is gone: from {u}"));
            src = u;
        }
        let mut ns = fetch::open_nsis(&src)?;
        fetch::nsis_fetch(
            &mut ns,
            &|p| {
                pkgzip::player::package_path(p)
                    .filter(|r| globs.iter().any(|g| pkgzip::player::fnmatch(r, g)))
            },
            pkg,
        )?;
    }
    Ok(info)
}

/// SOURCE_FIXES: libil2cpp's armv7 64-bit atomics' clobber lists as in Unity's later files.
pub const SOURCE_FIXES: [(&str, &str, &str); 1] = [(
    "il2cpp/libil2cpp/os/Unity/ExtendedAtomicOps-arm.h",
    r#"(?m)^([ \t]*: "cc"(?:, "memory")?)(?:, "r\d+")+[ \t]*(\r?)$"#,
    "${1}${2}",
)];

/// fix_editor_sources: SOURCE_FIXES in the fetched Editor/Data; the files changed.
pub fn fix_editor_sources(editor: &str) -> Result<Vec<String>> {
    let mut changed = Vec::new();
    for (rel, pat, rep) in SOURCE_FIXES {
        let p = join(editor, rel);
        if !util::isfile(&p) {
            continue;
        }
        let text = String::from_utf8(util::read(&p)?)
            .map_err(|e| Error::Fail(format!("UnicodeDecodeError: {p}: {e}")))?;
        let new = re(pat).replace_all(&text, rep).into_owned();
        if new != text {
            util::write_file(&p, new.as_bytes(), None)?;
            changed.push(rel.to_string());
        }
    }
    Ok(changed)
}

/// What the build uses (il2cpp.toolchain's dict).
#[derive(Clone, Debug)]
pub struct Toolchain {
    pub editor: String,
    pub ndk: String,
    pub ndk_name: String,
    pub ndk_revision: String,
    pub pkg: String,
    pub player: Json,
    pub bcl: String,
    pub il2cpp_editor: String,
    pub converter: Option<String>,
    pub android_api: Option<u32>,
}

impl Toolchain {
    /// The dict as `il2cpp fetch` prints it (without the player, and il2cpp_editor only when
    /// it is not the editor), and `device_toolchain`.
    pub fn json(&self, device_toolchain: Option<&str>) -> Json {
        let mut o = vec![
            ("editor".to_string(), Json::str(&self.editor)),
            ("ndk".into(), Json::str(&self.ndk)),
            ("ndk_name".into(), Json::str(&self.ndk_name)),
            ("ndk_revision".into(), Json::str(&self.ndk_revision)),
            ("pkg".into(), Json::str(&self.pkg)),
            ("bcl".into(), Json::str(&self.bcl)),
        ];
        if self.il2cpp_editor != self.editor {
            o.push(("il2cpp_editor".into(), Json::str(&self.il2cpp_editor)));
        }
        if let Some(c) = &self.converter {
            o.push(("converter".into(), Json::str(c)));
        }
        if let Some(a) = self.android_api {
            o.push(("android_api".into(), Json::Int(a as i128)));
        }
        if let Some(d) = device_toolchain {
            o.push(("device_toolchain".into(), Json::str(d)));
        }
        Json::Obj(o)
    }
}

/// toolchain: fetches what is missing; returns the paths.
pub fn toolchain(
    version: &Version,
    abi: &str,
    home: &str,
    pkg: Option<&str>,
    bcl: &str,
) -> Result<Toolchain> {
    let vdir = join(home, &version.text);
    let editor = fetch_editor(version, &join(&vdir, "editor"), None, bcl)?;
    for rel in fix_editor_sources(&editor)? {
        log(&format!(
            "{rel}: armv7 asm clobber lists as in later Unity releases (SOURCE_FIXES)"
        ));
    }
    bee::install(&editor)?;
    let (name, rev, zipname) = ndk_for(version)?;
    let ndk = fetch_ndk(name, zipname, abi, &join(home, &format!("ndk-{name}")), &[])?;
    install_ndk_shims(&ndk, abi)?;
    let pkg = pkg
        .map(str::to_string)
        .unwrap_or_else(|| join(&vdir, &format!("player-{abi}")));
    let info = fetch_player(version, &pkg, abi)?;
    let mut tc = Toolchain {
        editor: editor.clone(),
        ndk: ndk.clone(),
        ndk_name: name.into(),
        ndk_revision: rev.into(),
        pkg: pkg.clone(),
        player: info,
        bcl: bcl.into(),
        il2cpp_editor: editor.clone(),
        converter: None,
        android_api: None,
    };
    if mix::needed(version) {
        let (ed, conv) = mix::prepare(version, &editor, home, None)?;
        tc.il2cpp_editor = ed;
        tc.converter = Some(conv);
        let (api, _) = android_note(&joins(&[
            &pkg,
            "Variations",
            "il2cpp",
            "Release",
            "Libs",
            abi,
            "libunity.so",
        ]));
        if let Some(api) = api {
            if api > *ndk_apis(name, abi).iter().max().unwrap() {
                fetch_ndk(name, zipname, abi, &ndk, &[api])?;
                tc.android_api = Some(api);
            }
        }
    }
    Ok(tc)
}

/// tool_version: the Unity version whose IL2CPP and player the build uses.
pub fn tool_version(data_version: &Version, abi: &str, unity: Option<&str>) -> Result<Version> {
    if let Some(u) = unity {
        let v = Version::parse(u).map_err(|e| Error::Fail(format!("ValueError: {e}")))?;
        if v.line() != data_version.line() {
            return Err(Error::Unsupported(format!(
                "--unity {v}: the data is Unity {data_version}; the IL2CPP player must be of the same line ({}.{})",
                data_version.major, data_version.minor
            )));
        }
        return Ok(v);
    }
    if abi == "arm64-v8a" && data_version.line() == (2017, 4) && data_version.patch < ARM64_2017.0 {
        log(&format!(
            "Unity {data_version} has no arm64-v8a IL2CPP player (2017.4.{}f1 and later): building with {}'s IL2CPP for its player (--unity chooses another)",
            ARM64_2017.0, ARM64_2017.1
        ));
        return Ok(Version::parse(ARM64_2017.1).unwrap());
    }
    Ok(data_version.clone())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ndk_clang_is_the_repository_s() {
        // the copy compiled in is tools/il2cpp/ndk-clang.sh (when the repository is there)
        let p = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../tools/il2cpp/ndk-clang.sh"
        );
        if let Ok(t) = std::fs::read_to_string(p) {
            assert_eq!(
                t, NDK_CLANG,
                "crates/il2cpp/src/ndk-clang.sh differs from tools/il2cpp/ndk-clang.sh"
            );
        }
    }

    #[test]
    fn wanted() {
        assert_eq!(editor_rel("$_OUTDIR/il2cpp/x").as_deref(), Some("il2cpp/x"));
        assert_eq!(
            editor_rel("$INSTDIR/Editor/Data/il2cpp/y").as_deref(),
            Some("il2cpp/y")
        );
        assert_eq!(editor_rel("$INSTDIR/x"), None);
        assert!(editor_wanted("il2cpp/build/il2cpp.exe", BCL_DIR));
        assert!(!editor_wanted("il2cpp/build/il2cpp.pdb", BCL_DIR));
        assert!(!editor_wanted("il2cpp/MapFileParser/x", BCL_DIR));
        assert!(editor_wanted(
            "MonoBleedingEdge/lib/mono/unityaot/mscorlib.dll",
            BCL_DIR
        ));
        assert!(!editor_wanted(
            "MonoBleedingEdge/lib/mono/unityaot/x.xml",
            BCL_DIR
        ));
        assert!(editor_wanted("Mono/etc/mono/config", "Mono/lib/mono/2.0"));
        assert_eq!(ndk_apis("r23b", "arm64-v8a"), vec![21, 22]);
        assert_eq!(ndk_apis("r16b", "armeabi-v7a"), vec![16]);
        let t = "aarch64-linux-android";
        let l = "toolchains/llvm/prebuilt/linux-x86_64/";
        assert!(ndk_wanted(
            &format!("ndk/{l}sysroot/usr/lib/{t}/21/libc.so"),
            "r23b",
            "arm64-v8a",
            None
        )
        .is_some());
        assert!(ndk_wanted(
            &format!("ndk/{l}sysroot/usr/lib/{t}/24/libc.so"),
            "r23b",
            "arm64-v8a",
            None
        )
        .is_none());
        assert!(ndk_wanted(
            &format!("ndk/{l}sysroot/usr/lib/{t}/libc++_static.a"),
            "r23b",
            "arm64-v8a",
            None
        )
        .is_some());
        assert!(ndk_wanted(
            &format!("ndk/{l}lib64/clang/12.0.8/lib/linux/aarch64/libunwind.a"),
            "r23b",
            "arm64-v8a",
            None
        )
        .is_some());
        assert!(ndk_wanted(
            &format!("ndk/{l}lib64/clang/12.0.8/lib/linux/x86_64/libunwind.a"),
            "r23b",
            "arm64-v8a",
            None
        )
        .is_none());
        assert_eq!(
            ndk_wanted("ndk/source.properties", "r19", "arm64-v8a", None).as_deref(),
            Some("source.properties")
        );
        assert!(ndk_wanted(
            "ndk/platforms/android-21/arch-arm64/usr/lib/libc.so",
            "r16b",
            "arm64-v8a",
            None
        )
        .is_some());
        assert!(ndk_wanted(
            "ndk/sources/cxx-stl/gnu-libstdc++/4.9/libs/arm64-v8a/libgnustl_shared.so",
            "r16b",
            "arm64-v8a",
            None
        )
        .is_none());
    }

    #[test]
    fn source_fix() {
        let r = re(SOURCE_FIXES[0].1);
        let t = "  asm(\n        : \"cc\", \"memory\", \"r0\", \"r1\"\r\n        : \"cc\"\n";
        assert_eq!(
            r.replace_all(t, SOURCE_FIXES[0].2),
            "  asm(\n        : \"cc\", \"memory\"\r\n        : \"cc\"\n"
        );
    }

    #[test]
    fn versions() {
        let v = Version::parse("2017.4.9f1").unwrap();
        assert_eq!(
            tool_version(&v, "arm64-v8a", None).unwrap().text,
            "2017.4.40f1"
        );
        assert_eq!(
            tool_version(&v, "armeabi-v7a", None).unwrap().text,
            "2017.4.9f1"
        );
        assert!(tool_version(&v, "arm64-v8a", Some("2018.4.1f1")).is_err());
        assert_eq!(
            ndk_for(&Version::parse("2021.3.16f1").unwrap()).unwrap().0,
            "r21d"
        );
        assert_eq!(
            ndk_for(&Version::parse("2022.3.36f1").unwrap()).unwrap().0,
            "r23b"
        );
        assert!(ndk_for(&Version::parse("5.6.7f1").unwrap()).is_err());
    }
}
