//! What a game's files say (port.py's surveys, read only): the desktop builds under a path, the
//! game's own code (names the assemblies define and reference), its native plugins, the
//! serialized files (player settings, scripts scenes use, video, texture formats) and the asset
//! bundles under StreamingAssets.
use crate::asm;
use crate::pyx::*;
use port::val::{Dict, Val};
use regex::Regex;
use std::sync::{Arc, OnceLock};
use unityfs::{ClassDb, FileSource, SerializedFile, SharedSource, Value};

// =========================================================================== finding the build

pub fn is_data_dir(d: &str) -> bool {
    ["globalgamemanagers", "mainData", "data.unity3d"].iter().any(|n| isfile(&join(d, n)))
}

/// ('linux'|'windows'|'macos', arch) of an executable's header, or None.
fn exe_kind(path: &str) -> Option<(&'static str, String)> {
    let h = head(path, 4096);
    let u16l = |o: usize| h.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]]));
    if h.len() > 20 && &h[..4] == b"\x7fELF" {
        if h[16] != 2 && h[16] != 3 {
            return None;
        }
        let m = u16l(18)?;
        let arch = match m {
            0x3E => "x86_64".to_string(),
            0x03 => "x86".to_string(),
            0xB7 => "aarch64".to_string(),
            0x28 => "armhf".to_string(),
            _ => format!("{m:#x}"),
        };
        return Some(("linux", arch));
    }
    if h.len() > 0x40 && &h[..2] == b"MZ" {
        let pe = u32::from_le_bytes(h[0x3C..0x40].try_into().unwrap()) as usize;
        if pe + 6 <= h.len() && &h[pe..pe + 4] == b"PE\0\0" {
            let m = u16l(pe + 4)?;
            let arch = match m {
                0x8664 => "x86_64".to_string(),
                0x14C => "x86".to_string(),
                0xAA64 => "aarch64".to_string(),
                _ => format!("{m:#x}"),
            };
            return Some(("windows", arch));
        }
    }
    match h.get(..4) {
        Some(b"\xca\xfe\xba\xbe") => Some(("macos", "universal".into())),
        Some(b"\xcf\xfa\xed\xfe") | Some(b"\xce\xfa\xed\xfe") => Some(("macos", "x86_64/arm64".into())),
        _ => None,
    }
}

/// What kind of desktop build a data folder belongs to: platform, executable, architecture,
/// scripting backend.
pub fn describe_build(data: &str) -> Dict {
    let data = abspath(data);
    let mut b = dict(vec![("data", s(&data)), ("build", s("unknown")), ("exe", Val::Null), ("arch", Val::Null)]);
    let parent = dirname(&data).to_string();
    let il2cpp;
    if data.ends_with("Contents/Resources/Data") {
        let app = dirname(dirname(&parent)).to_string();
        b.set("build", s("macos"));
        b.set("root", s(&app));
        let macos = joins(&[&app, "Contents", "MacOS"]);
        if isdir(&macos) {
            for n in sorted_listdir(&macos) {
                if let Some(k) = exe_kind(&join(&macos, &n)) {
                    b.set("exe", s(&n));
                    b.set("arch", s(&k.1));
                    break;
                }
            }
        }
        il2cpp = isfile(&joins(&[&app, "Contents", "Frameworks", "GameAssembly.dylib"]));
    } else {
        b.set("root", s(&parent));
        let base = basename(&data).strip_suffix("_Data").map(String::from);
        let mut cands: Vec<String> = Vec::new();
        if let Some(base) = base.as_deref().filter(|b| !b.is_empty()) {
            cands = [".x86_64", ".x86", ".x64", ".exe", ""].iter().map(|e| format!("{base}{e}")).collect();
        }
        let more: Vec<String> = sorted_listdir(&parent).into_iter().filter(|n| !cands.contains(n)).collect();
        cands.extend(more);
        for n in &cands {
            let p = join(&parent, n);
            if isfile(&p) && ![".so", ".dll", ".debug", ".dylib", ".txt"].iter().any(|e| n.ends_with(e)) {
                if let Some(k) = exe_kind(&p) {
                    b.set("build", s(k.0));
                    b.set("exe", s(n));
                    b.set("arch", s(&k.1));
                    break;
                }
            }
        }
        if b.str("build") == "unknown" {
            if isfile(&join(&parent, "UnityPlayer.dll")) {
                b.set("build", s("windows"));
            } else if isfile(&join(&parent, "UnityPlayer.so")) {
                b.set("build", s("linux"));
            }
        }
        il2cpp = ["GameAssembly.so", "GameAssembly.dll"].iter().any(|n| isfile(&join(&parent, n)));
    }
    let managed = join(&data, "Managed");
    let has_managed = isdir(&managed) && listdir(&managed).iter().any(|n| n.ends_with(".dll"));
    let il2cpp = il2cpp || isdir(&join(&data, "il2cpp_data"));
    b.set("backend", s(if has_managed && !il2cpp { "mono" } else if il2cpp { "il2cpp" } else { "unknown" }));
    b.set("compressed", Val::Bool(!isfile(&join(&data, "globalgamemanagers")) && isfile(&join(&data, "data.unity3d"))));
    b
}

fn keep_dirs(ds: &mut Vec<String>) {
    ds.retain(|x| !x.starts_with('.') && x != "__MACOSX");
    ds.sort();
}

/// Desktop builds (describe_build) under path: a data folder, a .app, a game folder.
pub fn find_builds(path: &str, depth: usize) -> Vec<Dict> {
    let path = abspath(path);
    if isfile(&path) {
        return Vec::new();
    }
    if is_data_dir(&path) {
        return vec![describe_build(&path)];
    }
    let app_data = joins(&[&path, "Contents", "Resources", "Data"]);
    if path.ends_with(".app") && is_data_dir(&app_data) {
        return vec![describe_build(&app_data)];
    }
    let mut out = Vec::new();
    let base = path.matches('/').count();
    walk(&path, &mut |r, ds, _| {
        keep_dirs(ds);
        if is_data_dir(r) {
            out.push(describe_build(r));
            ds.clear();
            return;
        }
        if r.matches('/').count() - base >= depth {
            ds.clear();
        }
    });
    out
}

pub const ANDROID_PACKAGES: [&str; 4] = [".apk", ".xapk", ".apks", ".aab"];

/// An Android build at path, for the apk route: the file itself (.apk, .xapk, .apks, .aab), or a
/// folder's packages and OBBs.
pub fn android_packages(path: &str, depth: usize) -> Vec<String> {
    let is_pkg = |n: &str| ANDROID_PACKAGES.iter().any(|e| n.to_lowercase().ends_with(e));
    if isfile(path) {
        return if is_pkg(path) { vec![path.to_string()] } else { Vec::new() };
    }
    let mut out = Vec::new();
    let base = abspath(path).matches('/').count() as i64;
    let pass = |want: &dyn Fn(&str) -> bool, out: &mut Vec<String>| {
        walk(path, &mut |r, ds, fs| {
            keep_dirs(ds);
            let mut names: Vec<&String> = fs.iter().collect();
            names.sort();
            out.extend(names.into_iter().filter(|n| want(n)).map(|n| join(r, n)));
            if r.matches('/').count() as i64 - base >= depth as i64 {
                ds.clear();
            }
        });
    };
    pass(&is_pkg, &mut out);
    if !out.is_empty() {
        pass(&|n: &str| n.to_lowercase().ends_with(".obb"), &mut out);
    }
    out
}

pub fn dir_size(d: &str) -> u64 {
    let mut total = 0;
    walk(d, &mut |r, _, fs| {
        for n in fs {
            if let Ok(m) = std::fs::symlink_metadata(join(r, n)) {
                total += m.len();
            }
        }
    });
    total
}

// =========================================================================== what the code uses

const TEXT_TYPES: [((&str, &str), &str); 4] = [
    (("UnityEngine.UI", "InputField"), "UI InputField"),
    (("TMPro", "TMP_InputField"), "TextMeshPro InputField"),
    (("UnityEngine", "TouchScreenKeyboard"), "TouchScreenKeyboard"),
    (("UnityEngine.UIElements", "TextField"), "UI Toolkit TextField"),
];
const LEGACY: [(&str, &[&str]); 4] = [
    ("axes/buttons", &["GetAxis", "GetAxisRaw", "GetButton", "GetButtonDown", "GetButtonUp"]),
    ("keys", &["GetKey", "GetKeyDown", "GetKeyUp", "get_anyKey", "get_anyKeyDown", "get_inputString"]),
    ("mouse", &["GetMouseButton", "GetMouseButtonDown", "GetMouseButtonUp", "get_mousePosition", "get_mouseScrollDelta"]),
    ("touch", &["get_touchCount", "GetTouch", "get_touches"]),
];
const MOUSE_MSGS: [&str; 7] = ["OnMouseDown", "OnMouseUp", "OnMouseUpAsButton", "OnMouseEnter", "OnMouseExit", "OnMouseOver", "OnMouseDrag"];
const POINTER_IFACES: [&str; 8] = ["IPointerClickHandler", "IPointerDownHandler", "IPointerUpHandler", "IPointerEnterHandler",
                                   "IPointerExitHandler", "IDragHandler", "IBeginDragHandler", "IScrollHandler"];
const NAV_IFACES: [&str; 4] = ["ISubmitHandler", "ISelectHandler", "IMoveHandler", "ICancelHandler"];

fn services_of(ns: &str, n: &str, out: &mut Vec<&'static str>) {
    let st = |ps: &[&str]| ps.iter().any(|p| ns.starts_with(p));
    let hits = [
        ("Steamworks", ns == "Steamworks" && ["SteamAPI", "SteamClient", "SteamUser"].contains(&n) || ns.starts_with("Facepunch.Steamworks")),
        ("GOG Galaxy", ns.starts_with("Galaxy.Api")),
        ("Discord", st(&["DiscordRPC", "Discord"]) && ["DiscordRpcClient", "Discord", "DiscordRpc"].contains(&n)),
        ("Epic Online Services", ns.starts_with("Epic.OnlineServices")),
        ("FMOD Studio", ns == "FMODUnity" || ns == "FMOD.Studio" || (ns.is_empty() && n == "FMODStudioSystem")),
        ("Wwise", n == "AkSoundEngine" || n == "AkInitializer"),
        ("CRIWARE", ns.starts_with("CriWare") || n.starts_with("CriAtom")),
        ("Unity Ads / IAP / Analytics", st(&["UnityEngine.Advertisements", "UnityEngine.Purchasing"])),
        ("Unity Netcode / Mirror / Photon networking", st(&["Mirror", "Photon", "Unity.Netcode"])),
    ];
    for (label, hit) in hits {
        if hit && !out.contains(&label) {
            out.push(label);
        }
    }
}

/// P/Invoke modules every desktop build has (Unity's own, the C library) and those the runtime answers.
pub fn pinvoke_benign(m: &str) -> bool {
    static R: OnceLock<Regex> = OnceLock::new();
    R.get_or_init(|| {
        Regex::new(concat!(
            r"(?i)^(__Internal|libc|libc\.so\.6|libdl|kernel32|user32|gdi32|shell32|ole32|advapi32|dwmapi|",
            r"msvcrt|psapi|winmm|ntdll|libSystem|libobjc|/usr/lib/libobjc|MonoPosixHelper|libMonoPosixHelper|",
            r"System\.Native|libSystem\.Native|System\.Globalization\.Native|libmono.*|mono.*|",
            r"SDL2|libSDL2.*|X11|libX11.*|libudev.*|udev)(\.dll|\.so.*|\.dylib)?$"
        ))
        .unwrap()
    })
    .is_match(m)
}

/// The game's own code (not Unity's or the class libraries): input stacks, text input, video,
/// P/Invoke modules, store/audio/network services. From names only.
pub fn survey_assemblies(managed: &str) -> Dict {
    let mut legacy = [0i64; 4];
    let (mut assemblies, mut unreadable, mut text_input, mut video, mut cursor, mut screen): (Vec<String>, Vec<String>, Vec<String>, Vec<String>, Vec<String>, Vec<String>) = Default::default();
    let (mut rewired, mut playmaker): (Option<String>, Option<String>) = (None, None);
    let (mut incontrol, mut pointer_handlers, mut navigation, mut selects) = (false, 0i64, 0i64, 0i64);
    let mut mouse_messages: Vec<String> = Vec::new();
    let mut pinvoke: Vec<(String, Vec<String>)> = Vec::new();
    let mut services: Vec<&'static str> = Vec::new();
    let mut names: Vec<String> = Vec::new();
    if isdir(managed) {
        names = listdir(managed).into_iter().filter(|n| n.ends_with(".dll")).collect();
        names.sort();
    }
    let stem = |n: &str| n[..n.len() - 4].to_string();
    let libraries: Vec<String> = names.iter().map(|n| stem(n)).filter(|n| asm::is_platform_assembly(n)).collect();
    let input_system = names.iter().any(|n| n == "Unity.InputSystem.dll");
    let add = |l: &mut Vec<String>, x: &str| {
        if !l.iter().any(|y| y == x) {
            l.push(x.to_string());
        }
    };
    for n in &names {
        if asm::is_platform_assembly(&stem(n)) {
            continue;
        }
        let a = match asm::names(&join(managed, n)) {
            Ok(a) => a,
            Err(ex) => {
                unreadable.push(format!("{n}: {ex}"));
                continue;
            }
        };
        assemblies.push(stem(n));
        let mut types: Vec<&(String, String)> = a.typedefs.iter().chain(a.typerefs.iter()).collect();
        types.sort();
        types.dedup();
        for (ns, tn) in &a.typedefs {
            if ns == "Rewired" && (tn == "InputManager" || tn == "InputManager_Base") && rewired.is_none() {
                let ed = if n.starts_with("Assembly-CSharp") { "source edition" } else { "DLL edition" };
                rewired = Some(format!("{n} ({ed})"));
            }
            if ns == "HutongGames.PlayMaker" && tn == "Fsm" {
                playmaker = Some(n.clone());
            }
            if ns.starts_with("InControl") && (tn == "InputManager" || tn == "InControlManager") {
                incontrol = true;
            }
        }
        if a.typerefs.iter().any(|(ns, _)| ns.starts_with("InControl")) {
            incontrol = true;
        }
        for ((ns, tn), label) in TEXT_TYPES {
            if types.iter().any(|t| t.0 == ns && t.1 == tn) {
                add(&mut text_input, label);
            }
        }
        for (ns, tn) in types.iter().map(|t| (t.0.as_str(), t.1.as_str())) {
            if ns == "UnityEngine.Video" && tn == "VideoPlayer" {
                add(&mut video, "VideoPlayer");
            }
            if tn == "MovieTexture" {
                add(&mut video, "MovieTexture");
            }
            if ns == "UnityEngine.EventSystems" && POINTER_IFACES.contains(&tn) {
                pointer_handlers += 1;
            }
            if ns == "UnityEngine.EventSystems" && NAV_IFACES.contains(&tn) {
                navigation += 1;
            }
            services_of(ns, tn, &mut services);
        }
        for (cns, cn, m) in a.memberrefs.iter().map(|x| (x.0.as_str(), x.1.as_str(), x.2.as_str())) {
            if (cns, cn) == ("UnityEngine", "Input") {
                for (i, (_, ms)) in LEGACY.iter().enumerate() {
                    if ms.contains(&m) {
                        legacy[i] += 1;
                    }
                }
                if m == "get_inputString" {
                    add(&mut text_input, "Input.inputString");
                }
            }
            if (m == "SetSelectedGameObject" || m == "set_firstSelectedGameObject") && cn == "EventSystem"
                || m == "Select" && cns == "UnityEngine.UI"
            {
                selects += 1;
            }
            if (cns, cn) == ("UnityEngine", "Cursor") {
                add(&mut cursor, m);
            }
            if (cns, cn) == ("UnityEngine", "Screen") && ["SetResolution", "set_fullScreen", "set_fullScreenMode"].contains(&m) {
                add(&mut screen, m);
            }
            // (the Python looks for 'Handheld' and appends the longer name: one for every reference)
            if (cns, cn) == ("UnityEngine", "Handheld") && m == "PlayFullScreenMovie" && !video.iter().any(|v| v == "Handheld") {
                video.push("Handheld.PlayFullScreenMovie".to_string());
            }
        }
        for m in &a.methods {
            if MOUSE_MSGS.contains(&m.as_str()) {
                add(&mut mouse_messages, m);
            }
        }
        for (module, _) in &a.pinvokes {
            match pinvoke.iter_mut().find(|(m, _)| m == module) {
                Some((_, v)) => add(v, &stem(n)),
                None => pinvoke.push((module.clone(), vec![stem(n)])),
            }
        }
    }
    mouse_messages.sort();
    pinvoke.sort();
    services.sort();
    if names.iter().any(|n| n == "Rewired_Core.dll") && rewired.is_none() {
        rewired = Some("Rewired_Core.dll (no InputManager found)".into());
    }
    dict(vec![
        ("assemblies", strs(&assemblies)),
        ("libraries", strs(&libraries)),
        ("unreadable", strs(&unreadable)),
        ("rewired", opt_s(rewired)),
        ("incontrol", Val::Bool(incontrol)),
        ("playmaker", opt_s(playmaker)),
        ("input_system", Val::Bool(input_system)),
        ("legacy", Val::Dict(Dict(LEGACY.iter().enumerate().map(|(i, (k, _))| (k.to_string(), Val::Int(legacy[i]))).collect()))),
        ("mouse_messages", strs(&mouse_messages)),
        ("pointer_handlers", Val::Int(pointer_handlers)),
        ("navigation", Val::Int(navigation)),
        ("selects", Val::Int(selects)),
        ("text_input", strs(&text_input)),
        ("video", strs(&video)),
        ("pinvoke", Val::Dict(Dict(pinvoke.into_iter().map(|(m, mut v)| {
            v.sort();
            (m, strs(&v))
        }).collect()))),
        ("services", strs(&services)),
        ("cursor", strs(&cursor)),
        ("screen", strs(&screen)),
    ])
}

/// pointer | mixed | pad | none, as asmpatch --survey's verdict line.
pub fn input_verdict(sv: &Dict) -> &'static str {
    let lg = sv.dict("legacy");
    let pointer = int(lg, "mouse") > 0 || sv.truthy("mouse_messages") || int(sv, "pointer_handlers") > 0;
    let nav = int(sv, "selects") > 0 || int(sv, "navigation") > 0;
    let pad = sv.truthy("rewired") || sv.truthy("incontrol") || int(lg, "axes/buttons") > 0 || nav;
    if pointer && !nav {
        "pointer"
    } else if pointer {
        "mixed"
    } else if pad {
        "pad"
    } else {
        "none"
    }
}

// =========================================================================== what the data holds

pub fn gfx_name(a: i64) -> String {
    match a {
        0 => "OpenGL2",
        1 => "Direct3D9",
        2 => "Direct3D11",
        4 => "Null",
        8 => "OpenGLES2",
        11 => "OpenGLES3",
        16 => "Metal",
        17 => "OpenGLCore",
        18 => "Direct3D12",
        21 => "Vulkan",
        _ => return a.to_string(),
    }
    .to_string()
}

const KNOWN_PLUGINS: [(&str, &str); 9] = [
    (r"(?i)steam_api", "Steamworks: the runtime has a stub (Steam not running: SteamAPI_Init fails, most games go on)"),
    (r"(?i)fmod", "FMOD: needs an ARM (armhf) build of FMOD; without it the game is silent or stops"),
    (r"(?i)_burst_generated", "Burst: falls back to the managed code (slower, same results)"),
    (r"(?i)discord", "Discord: not on the handheld (calls fail)"),
    (r"(?i)galaxy", "GOG Galaxy: not on the handheld (the game must go on without it)"),
    (r"(?i)AkSoundEngine|wwise", "Wwise: ua_host's built-in Wwise; its own player plays the banks the setup converts (bank \
                                  version 128, pmu wwise), else silent; an ARM build of the game's Wwise version also gives sound"),
    (r"(?i)^(lib)?(?-i:Ak[A-Z])\w+\.(so|dll|bundle)$", "a Wwise plugin (effects): loaded by Wwise's engine, not by the game's code"),
    (r"(?i)cri_ware|criware", "CRIWARE: needs an ARM build"),
    (r"(?i)EOSSDK", "Epic Online Services: not on the handheld"),
];

/// Native libraries a build ships (Plugins/, the macOS bundle's PlugIns/).
pub fn plugin_files(build: &Dict) -> Vec<String> {
    let data = build.str("data");
    let mut dirs = vec![join(data, "Plugins")];
    if build.str("build") == "macos" {
        dirs.push(joins(&[build.str("root"), "Contents", "PlugIns"]));
    }
    let mut out = Vec::new();
    for d in dirs {
        walk(&d, &mut |r, ds, fs| {
            for n in fs.iter().chain(ds.iter().filter(|x| x.ends_with(".bundle"))) {
                if [".so", ".dll", ".dylib", ".bundle"].iter().any(|e| n.ends_with(e)) || n.contains(".so.") {
                    out.push(relpath(&join(r, n), data));
                }
            }
            ds.retain(|x| !x.ends_with(".bundle"));
        });
    }
    out.sort();
    out
}

/// One line per library (Plugins/x86 and x86_64 alike): [(file, note)].
pub fn plugin_notes(names: &[String]) -> Vec<(String, String)> {
    static KNOWN: OnceLock<Vec<Regex>> = OnceLock::new();
    let known = KNOWN.get_or_init(|| KNOWN_PLUGINS.iter().map(|k| Regex::new(k.0).unwrap()).collect());
    let mut seen: Vec<&str> = Vec::new();
    let mut out = Vec::new();
    for n in names {
        let b = basename(n);
        if seen.contains(&b) {
            continue;
        }
        seen.push(b);
        if b.starts_with("ScreenSelector.") {
            continue;
        }
        let note = known.iter().position(|r| r.is_match(b)).map_or("not shipped: a game that calls it needs an ARM build", |i| KNOWN_PLUGINS[i].1);
        out.push((n.clone(), note.to_string()));
    }
    out
}

/// The class database for files without type trees ($PMU_TPK, else the one built in), or None.
pub fn class_db() -> Option<Arc<ClassDb>> {
    static DB: OnceLock<Option<Arc<ClassDb>>> = OnceLock::new();
    DB.get_or_init(|| ClassDb::default_db().ok().map(Arc::new)).clone()
}

fn open_sf(path: &str) -> Result<SerializedFile, String> {
    let src: SharedSource = Arc::new(FileSource::open(path).map_err(|e| e.to_string())?);
    SerializedFile::open(basename(path), src, class_db(), None).map_err(|e| e.to_string())
}

fn text(v: Option<&Value>) -> Option<String> {
    v.and_then(Value::as_bytes).map(|b| String::from_utf8_lossy(b).into_owned())
}

fn num(v: Option<&Value>) -> Option<i64> {
    v.and_then(Value::as_i64)
}

/// UnityPy's TextureFormat names.
fn texture_format(f: i64) -> String {
    const NAMES: [(i64, &str); 78] = [
        (1, "Alpha8"), (2, "ARGB4444"), (3, "RGB24"), (4, "RGBA32"), (5, "ARGB32"), (6, "ARGBFloat"), (7, "RGB565"), (8, "BGR24"),
        (9, "R16"), (10, "DXT1"), (11, "DXT3"), (12, "DXT5"), (13, "RGBA4444"), (14, "BGRA32"), (15, "RHalf"), (16, "RGHalf"),
        (17, "RGBAHalf"), (18, "RFloat"), (19, "RGFloat"), (20, "RGBAFloat"), (21, "YUY2"), (22, "RGB9e5Float"), (23, "RGBFloat"),
        (24, "BC6H"), (25, "BC7"), (26, "BC4"), (27, "BC5"), (28, "DXT1Crunched"), (29, "DXT5Crunched"), (30, "PVRTC_RGB2"),
        (31, "PVRTC_RGBA2"), (32, "PVRTC_RGB4"), (33, "PVRTC_RGBA4"), (34, "ETC_RGB4"), (35, "ATC_RGB4"), (36, "ATC_RGBA8"),
        (41, "EAC_R"), (42, "EAC_R_SIGNED"), (43, "EAC_RG"), (44, "EAC_RG_SIGNED"), (45, "ETC2_RGB"), (46, "ETC2_RGBA1"),
        (47, "ETC2_RGBA8"), (48, "ASTC_RGB_4x4"), (49, "ASTC_RGB_5x5"), (50, "ASTC_RGB_6x6"), (51, "ASTC_RGB_8x8"),
        (52, "ASTC_RGB_10x10"), (53, "ASTC_RGB_12x12"), (54, "ASTC_RGBA_4x4"), (55, "ASTC_RGBA_5x5"), (56, "ASTC_RGBA_6x6"),
        (57, "ASTC_RGBA_8x8"), (58, "ASTC_RGBA_10x10"), (59, "ASTC_RGBA_12x12"), (60, "ETC_RGB4_3DS"), (61, "ETC_RGBA8_3DS"),
        (62, "RG16"), (63, "R8"), (64, "ETC_RGB4Crunched"), (65, "ETC2_RGBA8Crunched"), (66, "ASTC_HDR_4x4"), (67, "ASTC_HDR_5x5"),
        (68, "ASTC_HDR_6x6"), (69, "ASTC_HDR_8x8"), (70, "ASTC_HDR_10x10"), (71, "ASTC_HDR_12x12"), (72, "RG32"), (73, "RGB48"),
        (74, "RGBA64"), (75, "R8_SIGNED"), (76, "RG16_SIGNED"), (77, "RGB24_SIGNED"), (78, "RGBA32_SIGNED"), (79, "R16_SIGNED"),
        (80, "RG32_SIGNED"), (81, "RGB48_SIGNED"), (82, "RGBA64_SIGNED"),
    ];
    NAMES.iter().find(|(k, _)| *k == f).map_or_else(|| f.to_string(), |(_, n)| n.to_string())
}

/// A Texture2D's place in a survey's counts: textures {format: [count, bytes]} and the megabytes
/// BC/DXT ones decode to (formats a GLES handheld GPU cannot sample: Unity decodes them to RGBA32).
fn count_texture(t: &Value, textures: &mut Dict, decoded_mb: &mut f64) {
    let Some(f) = num(t.get("m_TextureFormat")) else { return };
    let f = texture_format(f);
    let size = num(t.get("m_CompleteImageSize")).unwrap_or(0);
    let (c, b) = match textures.get(&f) {
        Some(Val::List(l)) => (int_of(&l[0]), int_of(&l[1])),
        _ => (0, 0),
    };
    textures.set(&f, Val::List(vec![Val::Int(c + 1), Val::Int(b + size)]));
    if f.starts_with("DXT") || f.starts_with("BC") {
        let (w, h) = (num(t.get("m_Width")).unwrap_or(0), num(t.get("m_Height")).unwrap_or(0));
        let mips = num(t.get("m_MipCount")).filter(|m| *m != 0).unwrap_or(1);
        *decoded_mb += (w * h * 4) as f64 * (if mips > 1 { 4.0 / 3.0 } else { 1.0 }) / 1048576.0;
    }
}

pub fn int_of(v: &Val) -> i64 {
    match v {
        Val::Int(i) => *i,
        Val::Float(f) => *f as i64,
        Val::Bool(b) => *b as i64,
        _ => 0,
    }
}

const PLAYER_SETTINGS: i32 = 129;
const BUILD_SETTINGS: i32 = 141;
const MONO_BEHAVIOUR: i32 = 114;
const MONO_SCRIPT: i32 = 115;
const TEXTURE2D: i32 = 28;
const SHADER: i32 = 48;
const VIDEO_PLAYER: i32 = 328;
const MOVIE_TEXTURE: i32 = 152;

fn settings_of(sf: &SerializedFile, r: &mut Dict) -> Result<(), String> {
    for o in &sf.objects {
        if o.class_id == PLAYER_SETTINGS {
            let (t, _) = androiddata::typetrees::read(sf, o.path_id, &[]).map_err(|e| e.to_string())?;
            r.set("product", opt_s(text(t.get("productName"))));
            r.set("company", opt_s(text(t.get("companyName"))));
            let dim = |k: &str| num(t.get(k)).map_or(Val::Null, Val::Int);
            r.set("screen", Val::List(vec![dim("defaultScreenWidth"), dim("defaultScreenHeight")]));
            r.set("color_space", match num(t.get("m_ActiveColorSpace")) {
                Some(0) => s("gamma"),
                Some(1) => s("linear"),
                _ => Val::Null,
            });
            if let Some(h) = t.get("activeInputHandler") {
                r.set("active_input", match h.as_i64() {
                    Some(0) => s("Input Manager"),
                    Some(1) => s("Input System package"),
                    Some(2) => s("both"),
                    _ => Val::Null,
                });
            }
        } else if o.class_id == BUILD_SETTINGS {
            let (t, _) = androiddata::typetrees::read(sf, o.path_id, &[]).map_err(|e| e.to_string())?;
            let apis: Vec<Val> = t.get("m_GraphicsAPIs").and_then(Value::as_array)
                .map(|a| a.iter().filter_map(Value::as_i64).map(Val::Int).collect()).unwrap_or_default();
            r.set("graphics_apis", Val::List(apis));
            let n = ["scenes", "levels", "m_Scenes"].iter()
                .find_map(|k| t.get(k).and_then(Value::as_array).filter(|a| !a.is_empty()).map(Vec::len)).unwrap_or(0);
            r.set("scenes", Val::Int(n as i64));
        }
    }
    Ok(())
}

/// A MonoBehaviour's m_Script (file id, path id), read with its base tree.
fn mb_script(sf: &SerializedFile, pid: i64) -> Option<(i64, i64)> {
    let tree = sf.typetree_of(pid).ok()?;
    let data = sf.original_bytes(pid).ok()?;
    let (v, _) = unityfs::value::read(&tree, &data, sf.header.big, None).ok()?;
    let sc = v.get("m_Script")?;
    Some((num(sc.get("m_FileID"))?, num(sc.get("m_PathID"))?))
}

/// From the serialized files (read only): player settings, the scripts scenes use (input modules,
/// text fields), video, texture formats, scenes.
pub fn survey_data(data: &str, deep: bool) -> Dict {
    let mut r = dict(vec![
        ("product", Val::Null), ("company", Val::Null), ("graphics_apis", Val::Null), ("color_space", Val::Null),
        ("active_input", Val::Null), ("screen", Val::Null), ("scenes", Val::Null), ("scripts_used", Val::Dict(Dict::new())),
        ("video_players", Val::Int(0)), ("video_clips", Val::Dict(Dict::new())), ("movie_textures", Val::Int(0)),
        ("textures", Val::Dict(Dict::new())), ("decoded_mb", Val::Float(0.0)), ("notes", Val::List(Vec::new())),
    ]);
    let ggm = join(data, "globalgamemanagers");
    let u3d = join(data, "data.unity3d");
    if isfile(&ggm) || isfile(&u3d) {
        let read = |r: &mut Dict| -> Result<(), String> {
            if isfile(&ggm) {
                return settings_of(&open_sf(&ggm)?, r);
            }
            // a compressed build: its globalgamemanagers out of the archive, only the blocks it spans
            let b = unityfs::bundle::Bundle::open(&u3d).map_err(|e| e.to_string())?;
            if let Some(i) = b.node_index("globalgamemanagers") {
                let src = b.node(i);
                if src.len() > 0 {
                    let first = src.read_clipped(0, 48).map_err(|e| e.to_string())?;
                    if !unityfs::serialized::looks_serialized(&first, src.len()) {
                        return Err("ValueError: not a serialized file".into());
                    }
                    let sf = SerializedFile::open("globalgamemanagers", src, class_db(), Some(&b.header.engine)).map_err(|e| e.to_string())?;
                    settings_of(&sf, r)?;
                }
            }
            if deep {
                push(r, "notes", s("a compressed build: its scenes and textures are surveyed after `pmu unpack <data>` (or pmu unpack)"));
            }
            Ok(())
        };
        if let Err(ex) = read(&mut r) {
            push(&mut r, "notes", s(format!("globalgamemanagers: not read ({ex})")));
        }
    }
    if !deep {
        return r;
    }
    static FILES: OnceLock<Regex> = OnceLock::new();
    let files_rx = FILES.get_or_init(|| Regex::new(r"^(level\d+|sharedassets\d+\.assets|resources\.assets|globalgamemanagers\.assets)$").unwrap());
    let files: Vec<String> = sorted_listdir(data).into_iter().filter(|n| files_rx.is_match(n)).collect();
    let mut loaded: Vec<(String, SerializedFile)> = Vec::new();
    for n in &files {
        match open_sf(&join(data, n)) {
            Ok(sf) => loaded.push((n.clone(), sf)),
            Err(_) => push(&mut r, "notes", s(format!("{n}: not read"))),
        }
    }
    // scripts: MonoScripts, and which of them scenes and assets use
    let mut scripts: std::collections::HashMap<(String, i64), (String, String, String)> = Default::default();
    for (n, sf) in &loaded {
        for o in sf.objects.iter().filter(|o| o.class_id == MONO_SCRIPT) {
            if let Ok(t) = sf.read_typetree(o.path_id) {
                let g = |k: &str| text(t.get(k)).unwrap_or_default();
                scripts.insert((n.clone(), o.path_id), (g("m_Namespace"), g("m_ClassName"), g("m_AssemblyName")));
            }
        }
    }
    let mut used: std::collections::BTreeMap<(String, String, String), i64> = Default::default();
    let (mut video_players, mut movie_textures, mut decoded) = (0i64, 0i64, 0.0f64);
    let mut textures = Dict::new();
    for (n, sf) in &loaded {
        for o in &sf.objects {
            match o.class_id {
                MONO_BEHAVIOUR => {
                    let Some((fid, pid)) = mb_script(sf, o.path_id) else { continue };
                    let src = if fid == 0 {
                        n.clone()
                    } else {
                        match sf.externals.get((fid - 1) as usize).filter(|_| fid > 0) {
                            Some(e) => basename(&String::from_utf8_lossy(&e.path)).to_string(),
                            None => continue,
                        }
                    };
                    if let Some(k) = scripts.get(&(src, pid)) {
                        *used.entry(k.clone()).or_insert(0) += 1;
                    }
                }
                VIDEO_PLAYER => video_players += 1,
                MOVIE_TEXTURE => movie_textures += 1,
                TEXTURE2D => {
                    if let Ok(t) = sf.read_typetree(o.path_id) {
                        count_texture(&t, &mut textures, &mut decoded);
                    }
                }
                _ => {}
            }
        }
    }
    let mut su = Dict::new();
    for ((ns, cn, _), c) in &used {
        su.set(&if ns.is_empty() { cn.clone() } else { format!("{ns}.{cn}") }, Val::Int(*c));
    }
    r.set("scripts_used", Val::Dict(su));
    r.set("video_players", Val::Int(video_players));
    r.set("movie_textures", Val::Int(movie_textures));
    r.set("textures", Val::Dict(textures));
    r.set("decoded_mb", Val::Float(decoded));
    let opts = unityfs::Options { db: class_db(), ..Default::default() };
    match androiddata::video::clip_codecs_with(data, &opts) {
        Ok(c) => r.set("video_clips", Val::Dict(Dict(c.into_iter().map(|(k, n)| (k, Val::Int(n as i64))).collect()))),
        Err(_) => push(&mut r, "notes", s("video clips: not read")),
    }
    r
}

fn shader_platform(x: i64) -> String {
    match x {
        4 => "Direct3D 11",
        5 => "GLES 2",
        9 => "GLES 3",
        14 => "Metal",
        15 => "OpenGL Core",
        18 => "Vulkan",
        _ => return x.to_string(),
    }
    .to_string()
}

/// A shader source's name among a Shader's compiler platforms.
pub fn source_platform(src: &str) -> String {
    shader_platform(match src {
        "glcore" => 15,
        "d3d11" => 4,
        "vulkan" => 18,
        _ => 14,
    })
}

pub const GLES3_PLATFORM: &str = "GLES 3";

/// What a file looks like when it is a Unity bundle the converter does not read; None for a
/// UnityFS bundle and for anything else.
pub fn odd_bundle(path: &str) -> Option<&'static str> {
    let h = head(path, 4096);
    if h.is_empty() && !isfile(path) {
        return None;
    }
    if h.starts_with(b"UnityFS\0") {
        return None;
    }
    for (sig, what) in [
        (&b"UnityWeb\0"[..], "a UnityWeb archive (Unity 5.x web bundles)"),
        (b"UnityRaw\0", "a UnityRaw archive (Unity 5.x uncompressed bundles)"),
        (b"UnityArchive\0", "a UnityArchive"),
    ] {
        if h.starts_with(sig) {
            return Some(what);
        }
    }
    if find(&h, b"UnityFS\0").is_some() {
        return Some("a UnityFS bundle behind other bytes (the game's own offset or encryption)");
    }
    None
}

/// The asset bundles under StreamingAssets (read only, a bundle at a time): how many, those with
/// several serialized files (scene bundles), their shaders by the programs they have, texture
/// formats and sizes, and the files the converter does not read.
pub fn survey_streaming(data: &str) -> Dict {
    let (mut bundles, mut mb, mut decoded) = (0i64, 0.0f64, 0.0f64);
    let (mut scene_bundles, mut no_serialized): (Vec<String>, Vec<String>) = Default::default();
    let (mut shaders, mut shader_bundles, mut textures) = (Dict::new(), Dict::new(), Dict::new());
    let (mut odd, mut unread): (Vec<Val>, Vec<Val>) = Default::default();
    let sa = join(data, "StreamingAssets");
    if isdir(&sa) {
        walk(&sa, &mut |d, dirs, names| {
            dirs.sort();
            let mut names: Vec<&String> = names.iter().collect();
            names.sort();
            for n in names {
                let p = join(d, n);
                let rel = relpath(&p, data);
                if let Some(what) = odd_bundle(&p) {
                    odd.push(strs(&[rel.as_str(), what]));
                    continue;
                }
                if head(&p, 8) != b"UnityFS\0" {
                    continue;
                }
                bundles += 1;
                mb += std::fs::metadata(&p).map_or(0, |m| m.len()) as f64 / 1048576.0;
                let open = || -> Result<Vec<SerializedFile>, String> {
                    let b = unityfs::bundle::Bundle::open(&p).map_err(|e| e.to_string())?;
                    let mut sfs = Vec::new();
                    for i in unityfs::data::unitypy_serialized(&b).map_err(|e| e.to_string())? {
                        let name = b.header.nodes[i].name.clone();
                        sfs.push(SerializedFile::open(&name, b.node(i), class_db(), Some(&b.header.engine)).map_err(|e| e.to_string())?);
                    }
                    Ok(sfs)
                };
                let sfs = match open() {
                    Ok(x) => x,
                    Err(ex) => {
                        unread.push(strs(&[rel.as_str(), ex.as_str()]));
                        continue;
                    }
                };
                if sfs.is_empty() {
                    no_serialized.push(rel);
                    continue;
                }
                if sfs.len() > 1 {
                    scene_bundles.push(rel.clone());
                }
                for sf in &sfs {
                    for o in &sf.objects {
                        if o.class_id == SHADER {
                            let Ok(t) = sf.read_typetree(o.path_id) else { continue };
                            let mut pl: Vec<i64> = t.get("platforms").and_then(Value::as_array)
                                .map(|a| a.iter().filter_map(Value::as_i64).collect()).unwrap_or_default();
                            pl.sort();
                            pl.dedup();
                            let key = pl.iter().map(|x| shader_platform(*x)).collect::<Vec<_>>().join(", ");
                            shaders.set(&key, Val::Int(int(&shaders, &key) + 1));
                            if !shader_bundles.contains(&key) {
                                shader_bundles.set(&key, Val::List(Vec::new()));
                            }
                            if !shader_bundles.list(&key).iter().any(|x| x.as_str() == Some(&rel)) {
                                push(&mut shader_bundles, &key, s(&rel));
                            }
                        } else if o.class_id == TEXTURE2D {
                            if let Ok(t) = sf.read_typetree(o.path_id) {
                                count_texture(&t, &mut textures, &mut decoded);
                            }
                        }
                    }
                }
            }
        });
    }
    dict(vec![
        ("bundles", Val::Int(bundles)),
        ("mb", Val::Float(if isdir(&sa) { round1(mb) } else { 0.0 })),
        ("scene_bundles", strs(&scene_bundles)),
        ("shaders", Val::Dict(shaders)),
        ("shader_bundles", Val::Dict(shader_bundles)),
        ("textures", Val::Dict(textures)),
        ("decoded_mb", Val::Float(if isdir(&sa) { round1(decoded) } else { 0.0 })),
        ("odd", Val::List(odd)),
        ("unread", Val::List(unread)),
        ("no_serialized", strs(&no_serialized)),
    ])
}
