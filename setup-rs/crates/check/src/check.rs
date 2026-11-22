//! `pmu port check` (port.py's check and what it calls): what a game is, what it needs on the
//! handheld, the route candidates and the recommended one, the port config values `port new`
//! starts from, and the printed report. The report is the Python's dict (keys in its order): its
//! --json is the same document.
use crate::pyx::*;
use crate::survey::*;
use crate::{route, wwise};
use androiddata::uversion as UV;
use port::val::{Dict, Val};
use port::Repo;
use regex::Regex;
use std::process::Command;
use std::sync::OnceLock;

type Log<'a> = &'a mut dyn FnMut(&str);

fn re(cell: &'static OnceLock<Regex>, pat: &str) -> &'static Regex {
    cell.get_or_init(|| Regex::new(pat).unwrap())
}

fn range() -> String {
    format!("{}.{} - {}.{}", UV::FIRST.0, UV::FIRST.1, UV::LAST.0, UV::LAST.1)
}

// =========================================================================== asmpatch

/// The .NET SDK's dotnet: $DOTNET, else dotnet on PATH, else ~/.dotnet/dotnet, or None.
fn find_dotnet() -> Option<String> {
    let exec = |p: &str| isfile(p) && unsafe_access_x(p);
    let mut cands: Vec<String> = Vec::new();
    if let Ok(d) = std::env::var("DOTNET") {
        cands.push(d);
    }
    // (shutil.which: the first executable file named dotnet on PATH)
    if let Ok(path) = std::env::var("PATH") {
        if let Some(p) = path.split(':').map(|d| join(if d.is_empty() { "." } else { d }, "dotnet")).find(|p| exec(p)) {
            cands.push(p);
        }
    }
    if let Ok(h) = std::env::var("HOME") {
        cands.push(join(&h, ".dotnet/dotnet"));
    }
    cands.into_iter().find(|d| !d.is_empty() && exec(d))
}

fn unsafe_access_x(p: &str) -> bool {
    use std::os::unix::fs::PermissionsExt;
    std::fs::metadata(p).is_ok_and(|m| m.permissions().mode() & 0o111 != 0)
}

/// The asmpatch command for this machine: --asmpatch, $PMUNITY_ASMPATCH, a build in
/// tools/asmpatch/out (run with find_dotnet's dotnet), or None ("none": no survey).
fn find_asmpatch(repo: &Repo, explicit: Option<&str>) -> Option<String> {
    if let Some(e) = explicit.filter(|e| !e.is_empty()) {
        return Some(e.to_string());
    }
    if let Some(e) = std::env::var("PMUNITY_ASMPATCH").ok().filter(|e| !e.is_empty()) {
        return Some(e);
    }
    let out = port::ps(&repo.root.join("tools").join("asmpatch").join("out"));
    let native = joins(&[&out, "linux-arm64", "asmpatch"]);
    if cfg!(target_os = "linux") && cfg!(target_arch = "aarch64") && unsafe_access_x(&native) {
        return Some(shquote(&native));
    }
    let dll = joins(&[&out, "any", "asmpatch.dll"]);
    match find_dotnet() {
        Some(dotnet) if isfile(&dll) => Some(format!("{} {}", shquote(&dotnet), shquote(&dll))),
        _ => None,
    }
}

/// what the check misses without asmpatch's survey (the warning names it)
const ASMPATCH_MISSING: &str = "its input survey (the game's pad bindings: UA_DPAD_STICK and UA_DPAD_SWAP for handhelds without \
sticks, the [controls] draft; InControl's pad name; Mouse.current without a null check) and its \
file-access survey (StreamingAssets and dataPath reads)";

/// (asmpatch command or None, warning or None): find_asmpatch's, else tools/asmpatch/build.sh run
/// with the .NET 8 SDK when there is one, else a warning that says what is missing.
fn ensure_asmpatch(repo: &Repo, explicit: Option<&str>, log: Log) -> (Option<String>, Option<String>) {
    match find_asmpatch(repo, explicit) {
        Some(c) if c == "none" => return (None, None),
        Some(c) => return (Some(c), None),
        None => {}
    }
    let build = port::ps(&repo.root.join("tools").join("asmpatch").join("build.sh"));
    let Some(dotnet) = find_dotnet() else {
        return (None, Some(format!(
            "asmpatch is not built and no .NET 8 SDK was found ($DOTNET, dotnet on PATH, ~/.dotnet/dotnet): this check left \
             out {ASMPATCH_MISSING}. Install the .NET 8 SDK (docs/building.md), then run tools/asmpatch/build.sh (or set \
             DOTNET) and this check again; `port new` then says what to add to port.toml"
        )));
    };
    log(&format!("building asmpatch for its survey (tools/asmpatch/build.sh with {dotnet})"));
    let tail: Vec<String> = match Command::new(&build).env("DOTNET", &dotnet).output() {
        Ok(r) => {
            let text = format!("{}{}", String::from_utf8_lossy(&r.stdout), String::from_utf8_lossy(&r.stderr));
            let lines: Vec<&str> = text.trim().lines().collect();
            lines[lines.len().saturating_sub(3)..].iter().map(|x| x.to_string()).collect()
        }
        Err(e) => vec![e.to_string()],
    };
    if let Some(c) = find_asmpatch(repo, explicit) {
        return (Some(c), None);
    }
    let tail = if tail.is_empty() { "no output".to_string() } else { tail.join(" / ") };
    (None, Some(format!(
        "asmpatch did not build (tools/asmpatch/build.sh with {dotnet}: {tail}): this check left out {ASMPATCH_MISSING}. \
         Fix the build (docs/building.md) and run this check again"
    )))
}

/// (survey text, patches that would apply) from asmpatch, or None.
fn asmpatch_survey(cmd: &str, managed: &str) -> Option<(String, Vec<String>)> {
    static WOULD: OnceLock<Regex> = OnceLock::new();
    let words = shsplit(cmd).filter(|w| !w.is_empty())?;
    let run = |arg: &str| Command::new(&words[0]).args(&words[1..]).arg(arg).arg(managed).stdin(std::process::Stdio::null()).output().ok();
    let sv = run("--survey")?;
    let ls = run("--list")?;
    if !sv.status.success() {
        return None;
    }
    let ls = String::from_utf8_lossy(&ls.stdout).replace("\r\n", "\n");
    let would = re(&WOULD, r"(?m)^would\s+([\w-]+):").captures_iter(&ls).map(|c| c[1].to_string()).collect();
    Some((String::from_utf8_lossy(&sv.stdout).replace("\r\n", "\n"), would))
}

// =========================================================================== the check

pub fn route_text(r: &str) -> &'static str {
    port::text::route_text(r).unwrap_or("")
}

fn available(repo: &Repo, r: &str) -> Val {
    Val::Bool(repo.templates().join(r).join("route.json").is_file())
}

fn donor_need(version: &str, backend: &str, abi: &str) -> String {
    format!(
        "the donor {} (Unity's player, shipped in the release: `tools/donor/build.sh <Unity {version} editor> {} <DONORS dir>`)",
        port::routes::donor_name(version, backend, abi),
        port::routes::donor_build(backend, abi)
    )
}

fn route_entry(name: &str, ok: bool, avail: Option<Val>, why: &[String], needs: &[String]) -> Val {
    let mut d = dict(vec![("route", s(name)), ("ok", Val::Bool(ok))]);
    if let Some(a) = avail {
        d.set("available", a);
    }
    d.set("why", strs(why));
    d.set("needs", strs(needs));
    Val::Dict(d)
}

/// rep['route'] (and 'unsupported'): the first route that fits and has templates, else the first
/// that fits.
fn pick_route(rep: &mut Dict, none: &str) -> bool {
    let routes: Vec<&Dict> = rep.list("routes").iter().filter_map(Val::as_dict).collect();
    let best = routes.iter().find(|r| r.truthy("ok") && r.get("available").is_none_or(Val::truthy))
        .or_else(|| routes.iter().find(|r| r.truthy("ok"))).map(|r| r.str("route").to_string());
    let why: Vec<String> = routes.iter().filter_map(|r| r.list("why").first().map(Val::py_str)).collect();
    rep.set("route", opt_s(best.as_ref()));
    if best.is_none() {
        let w = why.join("; ");
        rep.set("unsupported", s(if w.is_empty() { none } else { &w }));
    }
    best.is_some()
}

/// Route candidates [{route, ok, why, needs}] and the recommended one.
fn decide_routes(repo: &Repo, rep: &mut Dict) -> port::Result<()> {
    let v = rep.str("unity_version").to_string();
    let supported = rep.truthy("unity_supported");
    let backend = rep.str("backend").to_string();
    let mono = backend == "mono";
    let gl = rep.get("shader_source").and_then(Val::as_str).map(String::from);
    let build = rep.str("build").to_string();
    let data = rep.str("data").to_string();
    let compressed = rep.truthy("compressed");
    let unsupported = format!("Unity {v}: the converter supports {}", range());
    let mut routes = Vec::new();
    // Android player, Mono
    let (mut why, mut needs): (Vec<String>, Vec<String>) = Default::default();
    if !mono {
        why.push(format!("scripting backend {backend}: the game's code is compiled for its desktop platform (no managed \
                          assemblies for the Mono player)"));
    }
    if !v.is_empty() && !supported {
        why.push(unsupported.clone());
    }
    match gl.as_deref() {
        None => why.push(format!("no shader programs the converter translates: {}",
                                 if rep.truthy("shader_why") { rep.str("shader_why") } else { "unknown graphics APIs" })),
        Some("d3d11") => needs.push("Direct3D 11 shaders: `shaders.from = [\"d3d11\"]`, translated on the handheld by pmhlslcc \
                                     (HLSLcc; the release ships it: `ARCH=arm64 tools/hlslcc/build.sh`)".into()),
        Some("vulkan") => needs.push("Vulkan shaders only: `shaders.from = [\"vulkan\"]`, translated on the handheld by \
                                      pmspirvcross (SPIRV-Cross; the release ships it: `ARCH=arm64 tools/spirvcross/build.sh`)".into()),
        Some("metal") => needs.push("Metal shaders only (a macOS build of Unity 2020.3+): `shaders.from = [\"metal\"]`, translated \
                                     by the converter's Metal reader (metal.py; checked on three macOS games, docs/shaders.md)".into()),
        _ => {}
    }
    // (every route on Unity's Android player draws with OpenGL ES: the two arm64 ones below too)
    let hdrp: Vec<String> = if rep.truthy("hdrp") {
        vec!["HDRP (Unity's High Definition pipeline) does not run on OpenGL ES: its shaders translate, the pipeline does not \
              render on the Android player's GLES3 device".into()]
    } else {
        Vec::new()
    };
    why.extend(hdrp.clone());
    if mono && build == "windows" && !data.is_empty() && androiddata::winbcl::is_win32_profile(&join(&data, "Managed")) {
        needs.push("Windows-only class libraries (unityjit-win32, Unity 2021.2+): the setup takes the Unix ones from the port's \
                    Mono donor (`pmunity winbcl --donor`): those its own build carries (mscorlib, System, System.Core, ...); the \
                    game's others stay the Windows ones (named in patchlog.txt)".into());
    }
    let vv = if v.is_empty() { None } else { UV::Version::parse(&v).ok() };
    let unity6 = vv.as_ref().is_some_and(|x| x.line() >= (6000, 0));
    if mono && unity6 && !data.is_empty() && !compressed && !isfile(&joins(&[&data, "Managed", "netstandard.dll"])) {
        needs.push("no netstandard.dll, which every Android engine module of Unity 6 references: the setup adds Unity's own \
                    facade of this version from the port's Mono donor (`pmunity winbcl --donor`)".into());
    }
    if compressed {
        needs.push("a compressed build: the setup unpacks it first (pmunity unpack)".into());
    }
    if !v.is_empty() {
        needs.push(donor_need(&v, "mono", "armeabi-v7a"));
    }
    routes.push(route_entry("android", why.is_empty(), None, &why, &needs));
    // IL2CPP to arm64 (64-bit only systems): the game's code compiled on the handheld in the setup
    let mut why: Vec<String> = if mono { Vec::new() } else { vec!["needs a Mono build (an IL2CPP desktop build is compiled for its own system)".into()] };
    let mix = vv.as_ref().and_then(il2cpp::mix::refusal);
    if vv.as_ref().is_some_and(|x| x.line() < (2017, 4)) {
        why.push(format!("Unity {v}: the arm64 IL2CPP players start with 2017.4"));
    } else if let Some(m) = mix {
        why.push(m);
    } else if !v.is_empty() && !supported {
        why.push(unsupported.clone());
    }
    if gl.is_none() {
        why.push("no shader programs the converter translates".into());
    }
    why.extend(hdrp.clone());
    let mut needs = Vec::new();
    if why.is_empty() {
        needs.push("the setup compiles the game's code with Unity's IL2CPP on the handheld (`pmunity il2cpp --on-device`): one to \
                    a few hours on a Cortex-A53, about 1.6 GB free, about 500 - 700 MB of memory, and Wi-Fi: Unity's IL2CPP tools \
                    and its arm64 IL2CPP player (with its static libraries), Debian's compilers and .NET, which no donor carries \
                    (the one route whose setup downloads)".to_string());
        needs.push("tested to the title in the arm64 rig with Unity 2017.4 - 2022.3".into());
        if vv.as_ref().is_some_and(il2cpp::mix::needed) {
            needs.push("Unity 2023.1+: its own il2cpp is a native program; Unity 2022.3's .NET il2cpp converts the game's code \
                        for this version's libil2cpp and player (docs/il2cpp.md, \"Unity 2023.1+\"; about 8 MB more downloads)".into());
        }
    }
    routes.push(route_entry("il2cpp-arm64", why.is_empty(), Some(available(repo, "il2cpp-arm64")), &why, &needs));
    // Mono on the arm64 player
    let mut why: Vec<String> = if mono { Vec::new() } else { vec!["needs a Mono build".into()] };
    let rt = rep.get("scripting_runtime").filter(|x| x.truthy()).map(Val::py_str).unwrap_or_default();
    let net35 = mono && rt.contains("3.5");
    let lines = port::config::MONO64_NET35_LINES;
    if net35 && !vv.as_ref().is_some_and(|x| lines.contains(&format!("{}.{}", x.major, x.minor).as_str())) {
        why.push(format!("a .NET 3.5 game: Unity's Mono of its desktop player (Mono 2.6) has no 64-bit ARM code generator (Unity \
                          {}: on its MonoBleedingEdge with Mono's 4.x class libraries)", lines.join(", ")));
    }
    if vv.as_ref().is_some_and(|x| x.line() < (2017, 4)) {
        why.push(format!("Unity {v}: the arm64 Android players start with 2017.4"));
    } else if !v.is_empty() && !supported {
        why.push(unsupported.clone());
    }
    if gl.is_none() {
        why.push("no shader programs the converter translates".into());
    }
    why.extend(hdrp);
    let mut needs = Vec::new();
    if why.is_empty() {
        needs.push("the release ships Unity's Mono for the game's Unity line, built for aarch64 Linux (`pmunity port package` \
                    runs tools/mono64/build-mono.sh, Docker; docs/mono-arm64.md)".to_string());
        needs.push("tested to the title in the arm64 rig with Unity 2019.4, 2020.3, 2021.3 and 6000.0".into());
        if net35 {
            needs.push("a .NET 3.5 game: its 2.0 class libraries replaced by Mono's 4.x ones from the same Mono tree \
                        (player.mono64_classlibs; tools/mono64/build-classlibs.sh; Cuphead, Unity 2017.4)".into());
        }
        if !v.is_empty() {
            needs.push(donor_need(&v, "il2cpp", "arm64-v8a"));
            needs.push(donor_need(&v, "mono", "armeabi-v7a"));
        }
    }
    routes.push(route_entry("mono-arm64", why.is_empty(), Some(available(repo, "mono-arm64")), &why, &needs));
    // box64: the game's own x86-64 Linux player
    let mut why = Vec::new();
    let arch = rep.get("arch").map_or("None".to_string(), Val::py_str);
    if build != "linux" {
        why.push(format!("needs the game's Linux build (this one is {})", rep.get("build").map_or("None".into(), Val::py_str)));
    } else if arch != "x86_64" && arch != "None" {
        why.push(format!("the Linux build is {arch} (box64 runs x86-64; box86 would be needed)"));
    }
    let needs: Vec<String> = if why.is_empty() { vec!["box64 and the player's x86-64 libraries on the handheld (tools/box64)".into()] } else { Vec::new() };
    routes.push(route_entry("box64", why.is_empty(), Some(available(repo, "box64")), &why, &needs));
    rep.set("routes", Val::List(routes));
    // (other modules refine or add routes: route.py's, plugins.py's, streaming.py's port_check)
    hook_route(rep);
    hook_plugins(rep);
    hook_streaming(rep);
    if pick_route(rep, "no route fits") && !v.is_empty() {
        // the donors the recommended route's release ships (Unity's player; tools/donor/build.sh makes them)
        let name = rep.str("route").to_string();
        if let Some(r) = port::routes::route(repo, &name)? {
            let pl = r.dict("defaults").dict("player");
            let get = |k: &str, d: &str| pl.get(k).map_or(d.to_string(), Val::py_str);
            let donors = port::routes::route_donors(&r, &[s(&v)], &get("backend", "mono"), &get("abi", "armeabi-v7a"));
            rep.set("donors", strs(&donors));
        }
    }
    Ok(())
}

/// route.py's port_check: adds what it knows to the box64 route candidate.
fn hook_route(rep: &mut Dict) {
    let data = rep.str("data").to_string();
    if data.is_empty() || !isdir(&data) {
        return;
    }
    let f = route::examine(&data);
    let apis = rep.get("graphics_apis").and_then(Val::as_list).map(|l| l.iter().map(Val::py_str).collect::<Vec<_>>());
    let Some(Val::List(routes)) = rep.get_mut("routes") else { return };
    let Some(r) = routes.iter_mut().filter_map(Val::as_dict_mut).find(|x| x.str("route") == "box64") else { return };
    if !r.truthy("ok") {
        return;
    }
    if let Some(apis) = apis {
        if !apis.iter().any(|a| a == "OpenGLCore" || a == "OpenGL2") {
            r.set("ok", Val::Bool(false));
            push(r, "why", s(format!(
                "the data has {} shaders, no OpenGL ones: the box64 port draws with OpenGL (gles, gl4es or Mesa); this player \
                 would need Vulkan on the handheld", if apis.is_empty() { "no".to_string() } else { apis.join(", ") })));
            return;
        }
    }
    let mode = route::gl_mode(&f);
    push(r, "needs", s(format!("GL mode {mode} ({})", if mode == "gles" { "GLES 3 shaders added by the setup" } else { "data as it is" })));
    for c in f.dict("box64").list("caveats") {
        let c = c.py_str();
        if c.contains("Mono JIT") || c.contains("plugin") {
            push(r, "needs", s(c));
        }
    }
}

/// plugins.py's port_check: Wwise's version and what the port does with it.
fn hook_plugins(rep: &mut Dict) {
    static AK: OnceLock<Regex> = OnceLock::new();
    let data = rep.str("data").to_string();
    if data.is_empty() || !isdir(&data) || !matches!(rep.get("backend"), Some(Val::Null) | None) && rep.str("backend") != "mono" {
        return;
    }
    let ak = re(&AK, r"(?i)^(lib)?aksoundengine$");
    if !rep.dict("code").dict("pinvoke").keys().any(|m| ak.is_match(&androiddata::plugins::norm(m))) {
        return;
    }
    if let Some(w) = wwise::wwise_info(&data) {
        let banks = wwise::wwise_player_banks(&data, &w);
        push(rep, "notes", s(wwise::wwise_summary(&w, banks.as_deref())));
        rep.set("wwise", Val::Dict(w));
        if let Some(b) = banks {
            rep.table("suggest").set("wwise", vdict(vec![("banks", s(b))]));
        }
    }
}

/// Without .NET (names only): the game's assemblies that reference both
/// Application.streamingAssetsPath and System.IO's file or directory APIs.
fn code_reads(managed: &str) -> Vec<String> {
    let mut out = Vec::new();
    if !isdir(managed) {
        return out;
    }
    for n in sorted_listdir(managed) {
        if !n.ends_with(".dll")
            || ["System", "Mono.", "mscorlib", "netstandard", "Microsoft."].iter().any(|p| n.starts_with(p))
            || n == "UnityEngine.dll"
            || n.starts_with("UnityEngine.") && n.ends_with("Module.dll")
        {
            continue;
        }
        let Ok(a) = crate::asm::names(&join(managed, &n)) else { continue };
        let sa = a.memberrefs.iter().any(|(ns, c, m)| ns == "UnityEngine" && c == "Application" && m == "get_streamingAssetsPath");
        let io = a.memberrefs.iter().any(|(ns, c, _)| ns == "System.IO" && ["File", "Directory", "FileInfo", "DirectoryInfo", "FileStream", "StreamReader"].contains(&c.as_str()));
        if sa && io {
            out.push(n[..n.len() - 4].to_string());
        }
    }
    out
}

/// streaming.py's port_check: suggests android_data.streaming_assets when the game's code reads
/// StreamingAssets with System.IO; notes reads of dataPath that reach System.IO.
fn hook_streaming(rep: &mut Dict) {
    if rep.str("backend") != "mono" || !rep.truthy("data") {
        return;
    }
    let would: Option<Vec<String>> = rep.get("asmpatch_would").and_then(Val::as_list).map(|l| l.iter().map(Val::py_str).collect());
    if would.as_ref().is_some_and(|w| w.iter().any(|x| x == "data-path")) {
        push(rep, "notes", s("reads of Application.dataPath reach System.IO (asmpatch --survey says where; on Android dataPath is \
                              the APK or OBB file): when the game needs files there or writes there, android_data.data_path = true \
                              gives those reads Game_Data/ beside the APK, with the data folder's own files"));
    }
    let data = rep.str("data").to_string();
    let sa = join(&data, "StreamingAssets");
    if !isdir(&sa) {
        return;
    }
    let mut size = 0u64;
    walk(&sa, &mut |r, _, fs| {
        for f in fs {
            size += std::fs::metadata(join(r, f)).map_or(0, |m| m.len());
        }
    });
    let (found, place) = match &would {
        Some(w) => (w.iter().any(|x| x == "streaming-assets"), "asmpatch: reads of Application.streamingAssetsPath reach System.IO".to_string()),
        None => {
            let names = code_reads(&join(&data, "Managed"));
            (!names.is_empty(), format!(
                "references to Application.streamingAssetsPath and System.IO in {}{}",
                names[..names.len().min(4)].join(", "),
                if names.is_empty() { "" } else { " (asmpatch, when built, says which reads reach System.IO)" }
            ))
        }
    };
    let mb = size as f64 / 1048576.0;
    rep.set("streaming_assets", vdict(vec![("mb", Val::Float(round1(mb))), ("io", Val::Bool(found))]));
    if !found {
        return;
    }
    let mode = if size < 256 << 20 { "both" } else { "loose" };
    rep.table("suggest").table("android_data").set("streaming_assets", s(mode));
    push(rep, "notes", s(format!(
        "StreamingAssets read as files ({place}): android_data.streaming_assets = \"{mode}\" puts them beside the APK \
         (Game_Data/StreamingAssets/, {mb:.1} MB{}) and asmpatch's {} patch gives those reads that folder",
        if mode == "both" { ", also in the OBB" } else { "" },
        if mode == "both" { "streaming-assets" } else { "streaming-assets-all" }
    )));
}

fn few(xs: &[String]) -> String {
    format!("{}{}", xs[..xs.len().min(4)].join(", "), if xs.len() > 4 { format!(" and {} more", xs.len() - 4) } else { String::new() })
}

fn pairs(l: &[Val]) -> Vec<(String, String)> {
    l.iter().filter_map(Val::as_list).filter(|p| p.len() == 2).map(|p| (p[0].py_str(), p[1].py_str())).collect()
}

/// The check's warnings and notes for what survey_streaming found.
fn streaming_warnings(rep: &mut Dict) {
    let st = rep.dict("streaming_survey").clone();
    if !st.truthy("bundles") && !st.truthy("odd") {
        return;
    }
    let (mut w, mut notes) = (Vec::new(), Vec::new());
    if st.truthy("odd") {
        let odd = pairs(st.list("odd"));
        w.push(format!(
            "{} file(s) under StreamingAssets look like Unity bundles the converter does not read ({}): their shaders and \
             textures stay as the desktop build has them (shaders without GLES programs draw magenta or not at all); the game's \
             own loader may need a look (docs/porting.md, port check)",
            odd.len(), few(&odd.iter().map(|(a, b)| format!("{a}: {b}")).collect::<Vec<_>>())));
    }
    if st.truthy("unread") {
        let un = pairs(st.list("unread"));
        w.push(format!("{} StreamingAssets bundle(s) UnityPy cannot open ({}): the converter cannot either, they stay as they are",
                       un.len(), few(&un.iter().map(|(a, _)| a.clone()).collect::<Vec<_>>())));
    }
    if st.truthy("no_serialized") {
        let ns = str_list(&st, "no_serialized");
        notes.push(format!("{} StreamingAssets bundle(s) without a serialized file ({}): nothing in them to convert, left as they are",
                           ns.len(), few(&ns)));
    }
    let src = rep.get("shader_source").and_then(Val::as_str).unwrap_or("").to_string();
    if st.truthy("shaders") && !src.is_empty() {
        let want = source_platform(&src);
        let missing: Vec<(&str, i64)> = st.dict("shaders").iter()
            .filter(|(k, _)| !k.split(", ").any(|x| x == want || x == GLES3_PLATFORM)).map(|(k, v)| (k, int_of(v))).collect();
        if !missing.is_empty() {
            let n: i64 = missing.iter().map(|x| x.1).sum();
            let mut bundles: Vec<String> = missing.iter().flat_map(|(k, _)| str_list(st.dict("shader_bundles"), k)).collect();
            bundles.sort();
            bundles.dedup();
            let mut keys: Vec<&str> = missing.iter().map(|x| x.0).collect();
            keys.sort();
            w.push(format!("{n} shader(s) in StreamingAssets bundles have no {want} programs (only {}) and so none the converter \
                            translates from (shaders from {src}): they draw magenta or not at all on the handheld ({})",
                           keys.join("; "), few(&bundles)));
        }
    }
    if st.truthy("scene_bundles") {
        let sb = str_list(&st, "scene_bundles");
        notes.push(format!("{} scene bundle(s) under StreamingAssets (several serialized files each: {}): pmu's convert converts \
                            each of their files (shaders, textures, JSON plugin edits); the Python converter, which a release \
                            ships only for a Python converter plugin of the port's, leaves them as they are (their shaders: \
                            desktop-only)", sb.len(), few(&sb)));
    }
    for x in w {
        push(rep, "warnings", s(x));
    }
    for x in notes {
        push(rep, "notes", s(x));
    }
}

/// The port check's report of the game at path.
pub fn check(repo: &Repo, path: &str, asmpatch: Option<&str>, deep: bool, log: Log) -> port::Result<Dict> {
    let mut rep = dict(vec![("path", s(abspath(path))), ("notes", Val::List(vec![])), ("warnings", Val::List(vec![])),
                            ("suggest", Val::Dict(Dict::new())), ("other_builds", Val::List(vec![]))]);
    let mut builds = find_builds(path, 4);
    if builds.is_empty() {
        let apks = android_packages(path, 4);
        if !apks.is_empty() {
            return crate::android::check_android(repo, rep, &apks, log);
        }
        return Err(port::exit(format!(
            "{path}: no Unity game here (no folder with globalgamemanagers, mainData or data.unity3d, no Android build: {})",
            ANDROID_PACKAGES.join(", "))));
    }
    // prefer a Mono build with OpenGL Core shaders (Linux, macOS), then any Mono build
    builds.sort_by_key(|b| (b.str("backend") != "mono", match b.str("build") {
        "linux" => 0,
        "macos" => 1,
        "windows" => 2,
        _ => 3,
    }));
    let b = builds.remove(0);
    rep.set("other_builds", Val::List(builds.iter().map(|x| {
        vdict(vec![("data", x.get("data").cloned().unwrap()), ("build", x.get("build").cloned().unwrap()), ("backend", x.get("backend").cloned().unwrap())])
    }).collect()));
    for (k, v) in b.iter() {
        rep.set(k, v.clone());
    }
    let (data, root, backend, build) = (b.str("data").to_string(), b.str("root").to_string(), b.str("backend").to_string(), b.str("build").to_string());
    let compressed = b.truthy("compressed");
    let mb = |d: &str| Val::Int((dir_size(d) as f64 / 1048576.0 + 0.5) as i64);
    rep.set("size_mb", mb(&root));
    rep.set("data_mb", mb(&data));
    let v = match UV::of_data(&data) {
        Ok(v) => {
            rep.set("unity_version", s(&v.text));
            rep.set("unity_line", s(format!("{}.{}", v.major, v.minor)));
            rep.set("unity_supported", Val::Bool(UV::FIRST <= v.line() && v.line() <= UV::LAST));
            Some(v)
        }
        Err(ex) => {
            rep.set("unity_version", Val::Null);
            rep.set("unity_supported", Val::Bool(false));
            push(&mut rep, "warnings", s(format!("Unity version not read: {}", ex.0)));
            None
        }
    };
    if let Some(v) = v.as_ref().filter(|_| backend == "mono") {
        rep.set("scripting_runtime", opt_s(androiddata::android_data::scripting_runtime(&data, v).ok().flatten()));
    }
    log("reading the game's settings and scenes");
    let mut d = survey_data(&data, deep && !compressed);
    if compressed {
        push(&mut d, "notes", s("a compressed build: its settings read from data.unity3d; scenes, textures and video are read \
                                 after `pmunity unpack`"));
    }
    for n in d.list("notes").to_vec() {
        push(&mut rep, "notes", n);
    }
    rep.set("data_survey", Val::Dict(d.clone()));
    if deep && !compressed {
        log("reading the StreamingAssets bundles");
        rep.set("streaming_survey", Val::Dict(survey_streaming(&data)));
    }
    rep.set("product", d.get("product").cloned().unwrap_or(Val::Null));
    // shaders: OpenGL Core programs (macOS, Linux) or Direct3D 11 (Windows)
    let apis: Option<Vec<i64>> = d.get("graphics_apis").and_then(Val::as_list).map(|l| l.iter().map(int_of).collect());
    let names: Option<Vec<String>> = apis.as_ref().map(|a| a.iter().map(|x| gfx_name(*x)).collect());
    rep.set("graphics_apis", names.as_ref().map_or(Val::Null, |n| strs(n)));
    match &apis {
        None => {
            let src = if build == "windows" { "d3d11" } else { "glcore" };
            rep.set("shader_source", s(src));
            if !(compressed || backend != "mono") {
                push(&mut rep, "warnings", s(format!("graphics APIs not read: assuming {src}")));
            }
        }
        Some(a) => {
            let src = [(17, "glcore"), (2, "d3d11"), (21, "vulkan"), (16, "metal")].iter().find(|(n, _)| a.contains(n)).map(|x| x.1);
            rep.set("shader_source", opt_s(src));
            if src.is_none() {
                let have = names.as_ref().unwrap().join(", ");
                rep.set("shader_why", s(format!(
                    "the build has {} shaders only; the converter translates OpenGL Core (macOS/Linux builds made with OpenGL \
                     Core), Direct3D 11, Vulkan and Metal", if have.is_empty() { "no" } else { &have })));
            }
        }
    }
    let managed = join(&data, "Managed");
    if rep.str("shader_source") == "vulkan" && backend == "mono" && isfile(&join(&managed, "Unity.RenderPipelines.HighDefinition.Runtime.dll")) {
        rep.set("hdrp", Val::Bool(true));
    }
    // the code
    let code = (backend == "mono").then(|| survey_assemblies(&managed));
    rep.set("code", code.clone().map_or(Val::Null, Val::Dict));
    if let Some(sv) = &code {
        rep.set("input_verdict", s(input_verdict(sv)));
        let (cmd, why) = ensure_asmpatch(repo, asmpatch, log);
        if let Some(w) = why {
            push(&mut rep, "warnings", s(w));
        }
        if let Some(cmd) = cmd {
            log("asmpatch --survey");
            match asmpatch_survey(&cmd, &managed).filter(|x| !x.0.is_empty()) {
                None => push(&mut rep, "warnings", s(format!("asmpatch --survey failed ({cmd}): this check left out {ASMPATCH_MISSING}"))),
                Some((text, would)) => {
                    static VERDICT: OnceLock<Regex> = OnceLock::new();
                    let verdict = re(&VERDICT, r"(?m)^verdict: (.*)$").captures(&text).map(|c| c[1].to_string());
                    rep.set("asmpatch_survey", s(&text));
                    rep.set("asmpatch_would", strs(&would));
                    if let Some(v) = verdict {
                        rep.set("input_verdict_text", s(v));
                    }
                }
            }
        }
    }
    streaming_warnings(&mut rep);
    // plugins
    let notes = plugin_notes(&plugin_files(&b));
    rep.set("plugins", Val::List(notes.iter().map(|(n, t)| strs(&[n, t])).collect()));
    decide_routes(repo, &mut rep)?;
    crate::suggest::suggest_config(&mut rep);
    Ok(rep)
}

// =========================================================================== the report

const INPUT_MODULES: [(&str, &str); 6] = [
    ("UnityEngine.EventSystems.StandaloneInputModule", "Unity UI, Input Manager (pad navigation and mouse)"),
    ("UnityEngine.InputSystem.UI.InputSystemUIInputModule", "Unity UI, Input System package"),
    ("Rewired.Integration.UnityUI.RewiredStandaloneInputModule", "Unity UI, Rewired"),
    ("Rewired.Integration.UnityUI.RewiredEventSystem", "Rewired event system"),
    ("InControl.InControlInputModule", "Unity UI, InControl"),
    ("UnityEngine.EventSystems.TouchInputModule", "Unity UI, touch"),
];

fn py(v: Option<&Val>) -> String {
    v.map_or("None".to_string(), Val::py_str)
}

fn or<'a>(d: &'a Dict, k: &str, other: &'a str) -> String {
    if d.truthy(k) { py(d.get(k)) } else { other.to_string() }
}

fn yes(d: &Dict, k: &str) -> &'static str {
    if d.truthy(k) { "yes" } else { "no" }
}

fn legacy_text(sv: &Dict) -> String {
    let lg: Vec<String> = sv.dict("legacy").iter().filter(|(_, v)| v.truthy()).map(|(k, v)| format!("{k} {}", v.py_str())).collect();
    if lg.is_empty() { "none".into() } else { lg.join(", ") }
}

fn top_textures(tex: &Dict) -> String {
    let mut top: Vec<(&str, i64, i64)> = tex.iter().filter_map(|(k, v)| v.as_list().map(|l| (k, int_of(&l[0]), int_of(&l[1])))).collect();
    top.sort_by_key(|x| -x.2);
    top.iter().take(5).map(|(k, c, b)| format!("{k} {c} ({:.0} MB)", *b as f64 / 1048576.0)).collect::<Vec<_>>().join(", ")
}

/// The report as `port check` prints it.
pub fn report_text(rep: &Dict) -> String {
    let mut out = String::new();
    let mut w = |line: String| {
        out.push_str(&line);
        out.push('\n');
    };
    let name = if rep.truthy("product") {
        py(rep.get("product"))
    } else {
        basename(if rep.truthy("data") { rep.str("data") } else { rep.str("path") }).to_string()
    };
    w(format!("game: {name}"));
    if rep.str("build") == "android" {
        let inputs = if rep.truthy("apk_inputs") { str_list(rep, "apk_inputs") } else { vec![py(rep.get("apk"))] };
        w(format!("  Android build: {} ({} MB)", inputs.join(", "), rep.get("size_mb").map_or("?".into(), Val::py_str)));
        if rep.truthy("apk_info") {
            let i = rep.dict("apk_info");
            w(format!("  package {}, versionCode {}, versionName {}, minSdk {}, targetSdk {}", py(i.get("package")), py(i.get("version_code")),
                      i.get("version_name").map_or("None".into(), Val::repr), py(i.get("min_sdk")), py(i.get("target_sdk"))));
            let (abis, all) = (str_list(i, "abis"), str_list(i, "all_abis"));
            let apis = str_list(i, "graphics_apis").join(", ");
            w(format!(
                "  Unity {}, {}; ABIs {}{}; graphics APIs {}{}{}",
                py(i.get("unity_version")), py(i.get("backend")), abis.join(", "),
                if all.len() > abis.len() { format!(" (also {})", all.iter().filter(|x| !abis.contains(x)).cloned().collect::<Vec<_>>().join(", ")) } else { String::new() },
                if apis.is_empty() { "not read" } else { &apis },
                if i.truthy("obbs") { format!("; OBBs: {}", i.dict("obbs").iter().map(|(_, v)| v.py_str()).collect::<Vec<_>>().join(", ")) } else { String::new() },
                if i.truthy("orientation") { format!("; screen {}", py(i.get("orientation"))) } else { String::new() }
            ));
        }
        if rep.truthy("code") {
            let sv = rep.dict("code");
            w(format!(
                "  input: Rewired {}; InControl {}; Input System {}; legacy Input: {}; UI pointer handlers {}; UI selection calls {}; verdict {}",
                or(sv, "rewired", "no"), yes(sv, "incontrol"), yes(sv, "input_system"), legacy_text(sv), py(sv.get("pointer_handlers")),
                py(sv.get("selects")), py(rep.get("input_verdict"))));
        }
    } else {
        w(format!("  data: {} ({} MB; the build: {} MB)", py(rep.get("data")), py(rep.get("data_mb")), py(rep.get("size_mb"))));
        w(format!("  build: {}{}{}", py(rep.get("build")),
                  if rep.truthy("exe") { format!(", {} ({})", py(rep.get("exe")), py(rep.get("arch"))) } else { String::new() },
                  if rep.truthy("compressed") { ", compressed (data.unity3d)" } else { "" }));
        w(format!("  Unity {}{}, {}{}", py(rep.get("unity_version")),
                  if rep.truthy("unity_supported") { "" } else { " (outside the converter's range)" }, py(rep.get("backend")),
                  if rep.truthy("scripting_runtime") { format!(", {}", py(rep.get("scripting_runtime"))) } else { String::new() }));
        if let Some(Val::List(apis)) = rep.get("graphics_apis") {
            let a = apis.iter().map(Val::py_str).collect::<Vec<_>>().join(", ");
            w(format!("  graphics APIs: {} -> shaders from {}", if a.is_empty() { "none listed" } else { &a }, py(rep.get("shader_source"))));
        }
        let ds = rep.dict("data_survey");
        if !matches!(ds.get("scenes"), None | Some(Val::Null)) {
            w(format!("  scenes: {}; screen {}; colour space {}{}", py(ds.get("scenes")), ds.get("screen").map_or("None".into(), Val::repr),
                      py(ds.get("color_space")),
                      if ds.truthy("active_input") { format!("; active input handling: {}", py(ds.get("active_input"))) } else { String::new() }));
        }
        if rep.truthy("code") {
            let sv = rep.dict("code");
            let asms = str_list(sv, "assemblies");
            w(format!("  code: {}{}", asms[..asms.len().min(8)].join(", "), if asms.len() > 8 { ", ..." } else { "" }));
            w(format!(
                "  input: Rewired {}; InControl {}; Input System {}; legacy Input: {}; OnMouse* {}; UI pointer handlers {}; UI selection calls {}",
                or(sv, "rewired", "no"), yes(sv, "incontrol"), yes(sv, "input_system"), legacy_text(sv), sv.list("mouse_messages").len(),
                py(sv.get("pointer_handlers")), py(sv.get("selects"))));
            let mods: Vec<&str> = ds.dict("scripts_used").keys().filter_map(|k| INPUT_MODULES.iter().find(|m| m.0 == k).map(|m| m.1)).collect();
            if !mods.is_empty() {
                w(format!("  UI input modules in scenes: {}", mods.join(", ")));
            }
            w(format!("  verdict: {}", if rep.truthy("input_verdict_text") { py(rep.get("input_verdict_text")) } else { py(rep.get("input_verdict")) }));
        }
        if ds.truthy("textures") {
            w(format!("  textures (main data): {}", top_textures(ds.dict("textures"))));
        }
        let st = rep.dict("streaming_survey");
        if st.truthy("bundles") || st.truthy("odd") {
            w(format!("  StreamingAssets: {} bundles ({:.0} MB){}{}", py(st.get("bundles")), float(st, "mb"),
                      if st.truthy("scene_bundles") { format!(", {} of them scene bundles", st.list("scene_bundles").len()) } else { String::new() },
                      if st.truthy("odd") { format!("; {} other Unity archives", st.list("odd").len()) } else { String::new() }));
            if st.truthy("shaders") {
                let mut sh: Vec<(&str, i64)> = st.dict("shaders").iter().map(|(k, v)| (k, int_of(v))).collect();
                sh.sort_by_key(|x| -x.1);
                w(format!("    shaders: {}", sh.iter().map(|(k, v)| format!("{v} with {}", if k.is_empty() { "no programs" } else { k })).collect::<Vec<_>>().join("; ")));
            }
            if st.truthy("textures") {
                w(format!("    textures: {}", top_textures(st.dict("textures"))));
            }
        }
    }
    for o in rep.list("other_builds").iter().filter_map(Val::as_dict) {
        w(format!("  also: {} build ({}) at {}", py(o.get("build")), py(o.get("backend")), py(o.get("data"))));
    }
    w(String::new());
    w("routes:".into());
    for r in rep.list("routes").iter().filter_map(Val::as_dict) {
        let mark = if r.truthy("ok") { "yes" } else { "no " };
        let avail = if r.get("available").is_none_or(Val::truthy) { "" } else { " (templates not there yet)" };
        w(format!("  [{mark}] {}{avail}: {}", r.str("route"), route_text(r.str("route"))));
        for x in r.list("why") {
            w(format!("        - {}", x.py_str()));
        }
        for x in r.list("needs") {
            w(format!("        + {}", x.py_str()));
        }
    }
    w(String::new());
    if rep.truthy("route") {
        w(format!("recommended route: {}", rep.str("route")));
        if rep.truthy("donors") {
            w(format!("donors the port ships (Unity's player; the handheld never downloads it): {}", str_list(rep, "donors").join(", ")));
            w("  build each with tools/donor/build.sh <the Unity editor of that version> MonoArmv7|Il2cppArm64 <DONORS dir>".into());
            w("  (tools/donor/get-editor.sh <version> <dir>: that editor with Android Build Support, unpacked on a Mac);".into());
            w("  `port package` takes them from --donors DIR or $DONORS (docs/porting.md, \"Donors\")".into());
        }
    } else {
        w(format!("unsupported: {}", py(rep.get("unsupported"))));
    }
    for x in rep.list("warnings") {
        w(format!("warning: {}", x.py_str()));
    }
    for x in rep.list("notes") {
        w(format!("- {}", x.py_str()));
    }
    if rep.truthy("suggest") {
        let sg = Dict(rep.dict("suggest").0.iter().filter(|(_, v)| v.truthy()).cloned().collect());
        if !sg.is_empty() {
            w(String::new());
            w(format!("port config (port new starts from these): {}", port::pyjson::dumps(&sort_keys(&Val::Dict(sg)), None, true)));
        }
    }
    if rep.truthy("asmpatch_survey") {
        w(String::new());
        w("asmpatch --survey:".into());
        for line in rep.str("asmpatch_survey").lines() {
            w(format!("  {line}"));
        }
    }
    out
}

/// The report as `port check --json` writes it (json.dump(rep, indent=1) and a newline).
pub fn report_json(rep: &Dict) -> String {
    format!("{}\n", port::pyjson::dumps(&Val::Dict(rep.clone()), Some(1), true))
}
