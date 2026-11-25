//! `pmu route` (route.py): which way a Unity game fits a PortMaster handheld, and why: the facts of
//! each build under a path (examine), the report, --json, --env (the box64 port's game.env) and
//! --launcher. Everything is read from file headers, the data's serialized-file headers and the
//! player binary's strings; nothing of the game runs.
use crate::pyx::*;
use androiddata::uversion as UV;
use port::val::{Dict, Val};
use regex::bytes::Regex as BRegex;
use regex::Regex;
use std::sync::OnceLock;

const DATA_MARKERS: [&str; 3] = ["globalgamemanagers", "mainData", "data.unity3d"];

fn platform_name(t: i32) -> Option<&'static str> {
    Some(match t {
        2 | 4 | 27 => "macos",
        5 | 19 => "windows",
        17 | 24 | 25 => "linux",
        13 => "android",
        9 => "ios",
        20 => "webgl",
        _ => return None,
    })
}

fn elf_machine(m: u16) -> Option<&'static str> {
    Some(match m {
        3 => "x86",
        62 => "x86_64",
        40 => "armv7",
        183 => "arm64",
        _ => return None,
    })
}

fn macho_cpu(c: i32) -> &'static str {
    match c {
        7 => "x86",
        0x0100_0007 => "x86_64",
        0x0100_000c => "arm64",
        12 => "armv7",
        _ => "other",
    }
}

/// (regex on the file name, what it is, effect on the Android route, severity there, effect on
/// the box64 route)
const KNOWN_PLUGINS: [(&str, &str, &str, &str, &str); 11] = [
    (r"(?i)^(lib)?_?burst_generated", "Burst-compiled jobs",
     "not loaded: Burst jobs fall back to their managed code (slower; fine unless the game insists on Burst)", "minor",
     "emulated"),
    (r"(?i)steam_api|CSteamworks|steamworks", "Steamworks",
     "replaced by the runtime's libsteam_api stub (SteamAPI_Init fails, as without a Steam client)", "ok",
     "emulated; without a Steam client SteamAPI_Init fails (games usually go on; a few quit)"),
    (r"(?i)fmod", "FMOD Studio integration",
     "needs FMOD's ARM Android libraries, which a desktop build does not carry: the game is silent without them", "major",
     "emulated (audio through the device's ALSA / PulseAudio)"),
    (r"(?i)AkSoundEngine|wwise", "Wwise sound engine",
     "needs Wwise's ARM Android libraries: no sound (or no start) without them", "major", "emulated"),
    (r"(?i)resonanceaudio", "Resonance Audio spatializer", "missing: spatialized sources lose their effect", "minor",
     "emulated"),
    (r"(?i)ScreenSelector", "Unity's start-up resolution dialog (GTK)", "not used", "ok",
     "shown only when the build enables the dialog (needs GTK 2 on the device; pass -screen-* arguments)"),
    (r"(?i)discord", "Discord SDK", "missing: Discord features off", "minor", "emulated; fails without a Discord client"),
    (r"(?i)galaxy", "GOG Galaxy SDK", "missing: Galaxy features off", "minor", "emulated; fails without the Galaxy client"),
    (r"(?i)EOSSDK", "Epic Online Services", "missing: online features off", "minor", "emulated (no network on most handhelds)"),
    (r"(?i)AVPro", "AVPro Video", "missing: the game's videos do not play", "major", "emulated (software decoding)"),
    (r"(?i)^lib(SDL2|sdl2)", "SDL2 (the game's own copy)", "not used", "ok", "box64 uses the device's SDL2 instead"),
];

fn rx(cell: &'static OnceLock<Regex>, pat: &str) -> &'static Regex {
    cell.get_or_init(|| Regex::new(pat).unwrap())
}

fn wrapped(n: &str) -> bool {
    static R: OnceLock<Regex> = OnceLock::new();
    rx(&R, concat!(
        r"^lib(c|m|dl|pthread|rt|util|bsd|uuid|resolv|crypt)\.so|^ld-linux|",
        r"^lib(GL|EGL|GLESv2|GLX|OpenGL|GLU)[._]|^lib(X[A-Za-z0-9-]*|xcb[-a-z0-9]*|xkb[a-z0-9-]*|xshmfence)\.so|",
        r"^lib(SDL2|sdl2)[-_.]|^lib(asound|pulse|pulse-simple|pulse-mainloop-glib|openal)\.so|",
        r"^lib(gtk-x11-2\.0|gdk-x11-2\.0|gtk-3|gdk-3|gdk_pixbuf-2\.0|gio-2\.0|gobject-2\.0|glib-2\.0|dbus-1|dbus-glib-1)\.so|",
        r"^lib(z|udev|vulkan|gbm|drm|freetype|fontconfig|png12|png16|bz2|lzma|expat|curl|curl-gnutls|ssl|crypto|xml2|xslt)\.so|",
        r"^libwayland-(client|cursor|egl)\.so|^libicu(uc|i18n)\.so"
    ))
    .is_match(n)
}

fn x64lib(n: &str) -> bool {
    static R: OnceLock<Regex> = OnceLock::new();
    rx(&R, r"^lib(stdc\+\+\.so\.[56]|gcc_s\.so\.1|unwind\.so\.8)$").is_match(n)
}

fn glibc_own(n: &str) -> bool {
    static R: OnceLock<Regex> = OnceLock::new();
    rx(&R, r"^(ld-linux.*|lib(c|m|dl|pthread|rt|util|resolv|crypt|anl|nsl)\.so\.\d+)$").is_match(n)
}

fn runtime_lib(n: &str) -> bool {
    static R: OnceLock<Regex> = OnceLock::new();
    rx(&R, concat!(
        r"^lib(GL|EGL|GLESv2|X11|Xext|Xcursor|Xrandr|Xi|Xinerama|Xss|Xxf86vm|xkbcommon|",
        r"wayland-(client|cursor|egl)|asound|pulse-simple|udev|vulkan|dbus-1|gtk-x11-2\.0)\.so\.\d+$"
    ))
    .is_match(n)
}

// ---------------------------------------------------------------------------------------------
// Binaries (headers only)

fn u16e(h: &[u8], o: usize, le: bool) -> Option<u16> {
    let b: [u8; 2] = h.get(o..o + 2)?.try_into().ok()?;
    Some(if le { u16::from_le_bytes(b) } else { u16::from_be_bytes(b) })
}

fn u32e(h: &[u8], o: usize, le: bool) -> Option<u32> {
    let b: [u8; 4] = h.get(o..o + 4)?.try_into().ok()?;
    Some(if le { u32::from_le_bytes(b) } else { u32::from_be_bytes(b) })
}

fn u64e(h: &[u8], o: usize, le: bool) -> Option<u64> {
    let b: [u8; 8] = h.get(o..o + 8)?.try_into().ok()?;
    Some(if le { u64::from_le_bytes(b) } else { u64::from_be_bytes(b) })
}

/// ('elf'|'pe'|'macho', arch) of an executable or library, or None.
pub fn binary_kind(path: &str) -> Option<(&'static str, String)> {
    let h = head(path, 4096);
    if h.len() >= 20 && &h[..4] == b"\x7fELF" {
        let m = u16e(&h, 18, h[5] == 1)?;
        return Some(("elf", elf_machine(m).unwrap_or("other").to_string()));
    }
    if h.len() >= 0x40 && &h[..2] == b"MZ" {
        let pe = u32e(&h, 0x3c, true)? as usize;
        let h = head(path, pe + 6);
        if h.get(pe..pe + 4) == Some(b"PE\0\0") {
            let m = u16e(&h, pe + 4, true)?;
            let arch = match m {
                0x14c => "x86",
                0x8664 => "x86_64",
                0xaa64 => "arm64",
                0x1c4 => "armv7",
                _ => "other",
            };
            return Some(("pe", arch.to_string()));
        }
        return None;
    }
    let magic = h.get(..4)?;
    if magic == b"\xcf\xfa\xed\xfe" || magic == b"\xce\xfa\xed\xfe" {
        return Some(("macho", macho_cpu(u32e(&h, 4, true)? as i32).to_string()));
    }
    if magic == b"\xca\xfe\xba\xbe" && h.len() >= 8 {
        let n = u32e(&h, 4, false)? as usize;
        if n < 16 {
            let cpus: Vec<&str> = (0..n).map(|i| u32e(&h, 8 + 20 * i, false).map_or("other", |c| macho_cpu(c as i32))).collect();
            return Some(("macho", cpus.join("+")));
        }
    }
    None
}

#[derive(Default, Clone)]
pub struct ElfInfo {
    pub interp: Option<String>,
    pub needed: Vec<String>,
    pub glibc: Option<String>,
    pub glibcxx: Option<String>,
    pub ok: bool,
}

fn latin1(b: &[u8]) -> String {
    b.iter().map(|&c| c as char).collect()
}

/// ELF facts: interpreter, DT_NEEDED and the highest GLIBC_ / GLIBCXX_ symbol versions asked for
/// (section headers; nothing for a non-ELF file or one cut short).
pub fn elf_info(path: &str) -> ElfInfo {
    let Ok(m) = std::fs::read(path) else { return ElfInfo::default() };
    if m.is_empty() {
        return ElfInfo::default();
    }
    elf_parse(&m).unwrap_or_default()
}

fn elf_parse(m: &[u8]) -> Option<ElfInfo> {
    if m.get(..4)? != b"\x7fELF" {
        return Some(ElfInfo::default());
    }
    let (is64, le) = (*m.get(4)? == 2, *m.get(5)? == 1);
    let (phoff, shoff, phentsize, phnum, shentsize, shnum) = if is64 {
        (u64e(m, 32, le)? as usize, u64e(m, 40, le)? as usize, u16e(m, 54, le)? as usize, u16e(m, 56, le)? as usize,
         u16e(m, 58, le)? as usize, u16e(m, 60, le)? as usize)
    } else {
        (u32e(m, 28, le)? as usize, u32e(m, 32, le)? as usize, u16e(m, 42, le)? as usize, u16e(m, 44, le)? as usize,
         u16e(m, 46, le)? as usize, u16e(m, 48, le)? as usize)
    };
    let mut info = ElfInfo { ok: true, ..Default::default() };
    for i in 0..phnum {
        let o = phoff + i * phentsize;
        if u32e(m, o, le)? == 3 {
            let (off, size) = if is64 {
                (u64e(m, o + 8, le)? as usize, u64e(m, o + 32, le)? as usize)
            } else {
                (u32e(m, o + 4, le)? as usize, u32e(m, o + 16, le)? as usize)
            };
            let end = off.saturating_add(size).min(m.len());
            let b = m.get(off.min(m.len())..end)?;
            info.interp = Some(latin1(b.split(|&c| c == 0).next().unwrap_or(b"")));
        }
    }
    let mut secs = Vec::new();
    for i in 0..shnum {
        let o = shoff + i * shentsize;
        let sh_type = u32e(m, o + 4, le)?;
        let (off, size, link) = if is64 {
            (u64e(m, o + 24, le)? as usize, u64e(m, o + 32, le)? as usize, u32e(m, o + 40, le)? as usize)
        } else {
            (u32e(m, o + 16, le)? as usize, u32e(m, o + 20, le)? as usize, u32e(m, o + 24, le)? as usize)
        };
        secs.push((sh_type, off, size, link));
    }
    let cstr = |sec: usize, off: usize| -> Option<String> {
        let base = secs.get(sec)?.1 + off;
        // (m.find gives -1 without a NUL, and the slice then stops a byte before the end)
        let end = find(m.get(base..)?, b"\0").map_or(m.len().saturating_sub(1), |e| base + e);
        Some(latin1(m.get(base..end.max(base)).unwrap_or(b"")))
    };
    let mut versions: Vec<String> = Vec::new();
    for &(sh_type, off, size, link) in &secs {
        if sh_type == 6 {
            let ent = if is64 { 16 } else { 8 };
            for j in 0..size / ent {
                let o = off + j * ent;
                let (tag, val) = if is64 {
                    (u64e(m, o, le)? as i64, u64e(m, o + 8, le)? as usize)
                } else {
                    (u32e(m, o, le)? as i32 as i64, u32e(m, o + 4, le)? as usize)
                };
                if tag == 0 {
                    break;
                }
                if tag == 1 {
                    info.needed.push(cstr(link, val)?);
                }
            }
        } else if sh_type == 0x6fff_fffe {
            let mut p = off;
            loop {
                let cnt = u16e(m, p + 2, le)?;
                let aux = u32e(m, p + 8, le)? as usize;
                let next = u32e(m, p + 12, le)? as usize;
                let mut a = p + aux;
                for _ in 0..cnt {
                    let name = u32e(m, a + 8, le)? as usize;
                    let vnext = u32e(m, a + 12, le)? as usize;
                    let v = cstr(link, name)?;
                    if !versions.contains(&v) {
                        versions.push(v);
                    }
                    if vnext == 0 {
                        break;
                    }
                    a += vnext;
                }
                if next == 0 {
                    break;
                }
                p += next;
            }
        }
    }
    let highest = |prefix: &str| -> Option<String> {
        let mut best: Option<(Vec<u64>, String)> = None;
        for v in &versions {
            let Some(rest) = v.strip_prefix(prefix) else { continue };
            if rest.is_empty() || !rest.chars().all(|c| c.is_ascii_digit() || c == '.') {
                continue;
            }
            // (int('') raises in the Python: such a version cannot be compared; none is written)
            let key: Option<Vec<u64>> = rest.split('.').map(|x| x.parse().ok()).collect();
            let key = key?;
            if best.as_ref().is_none_or(|(k, _)| key > *k) {
                best = Some((key, v.clone()));
            }
        }
        best.map(|b| b.1)
    };
    info.glibc = highest("GLIBC_");
    info.glibcxx = highest("GLIBCXX_");
    Some(info)
}

pub struct PlayerStrings {
    pub flags: Vec<String>,
    pub libs: Vec<String>,
    pub egl: bool,
    pub glx_es: bool,
}

/// What the player binary can do, from its strings: the -force-* renderer switches and the shared
/// libraries it names - read, never run.
pub fn player_strings(path: &str) -> PlayerStrings {
    static FLAGS: OnceLock<BRegex> = OnceLock::new();
    static DLOPEN: OnceLock<BRegex> = OnceLock::new();
    let mut out = PlayerStrings { flags: Vec::new(), libs: Vec::new(), egl: false, glx_es: false };
    let Ok(m) = std::fs::read(path) else { return out };
    if m.is_empty() {
        return out;
    }
    let flags = FLAGS.get_or_init(|| BRegex::new(r"(?-u)force-(glcore[0-9]*|vulkan|opengl|gles[0-9a-z]*|wayland|gfx-direct)\x00").unwrap());
    let dlopen = DLOPEN.get_or_init(|| BRegex::new(r"(?-u)(lib[A-Za-z0-9_+-]+\.so(?:\.[0-9]+)*)\x00").unwrap());
    let set = |r: &BRegex| {
        let mut v: Vec<String> = r.captures_iter(&m).map(|c| latin1(&c[1])).collect();
        v.sort();
        v.dedup();
        v
    };
    out.flags = set(flags);
    out.libs = set(dlopen);
    out.egl = find(&m, b"eglCreateWindowSurface\0").is_some();
    out.glx_es = find(&m, b"GLX_EXT_create_context_es2_profile\0").is_some();
    out
}

// ---------------------------------------------------------------------------------------------
// The build's layout

fn is_data(d: &str) -> bool {
    DATA_MARKERS.iter().any(|n| isfile(&join(d, n)))
}

/// Data folders (<Game>_Data, <Game>.app/Contents/Resources/Data) at or below path.
pub fn find_data_dirs(path: &str, depth: usize) -> Vec<String> {
    let mut path = abspath(path);
    if isfile(&path) {
        path = dirname(&path).to_string();
    }
    if is_data(&path) {
        return vec![path];
    }
    let app = joins(&[&path, "Contents", "Resources", "Data"]);
    if is_data(&app) {
        return vec![app];
    }
    let mut found = Vec::new();
    let plen = path.len();
    walk(&path, &mut |root, dirs, _| {
        if root[plen..].matches('/').count() >= depth {
            dirs.clear();
            return;
        }
        // (by name, and the walk goes down in that order too: the order found is not the file system's)
        dirs.sort();
        for d in dirs.iter() {
            let p = join(root, d);
            if d.ends_with("_Data") && is_data(&p) {
                found.push(p);
            } else if d.ends_with(".app") && is_data(&joins(&[&p, "Contents", "Resources", "Data"])) {
                found.push(joins(&[&p, "Contents", "Resources", "Data"]));
            }
        }
        dirs.retain(|d| !(d.ends_with("_Data") || d.ends_with(".app")));
    });
    found
}

fn sf_head(data: &str) -> Vec<u8> {
    for n in ["globalgamemanagers", "mainData"] {
        let p = join(data, n);
        if isfile(&p) {
            return head(&p, 512);
        }
    }
    Vec::new()
}

/// The Unity version of data in a SerializedFile format newer than unity_version knows: the
/// version string still follows the 48-byte header of format 22.
fn lenient_version(data: &str) -> Option<UV::Version> {
    let h = sf_head(data);
    if h.len() < 64 {
        return None;
    }
    if u32e(&h, 8, false)? < 22 {
        return None;
    }
    let end = 48 + find(&h[48..], b"\0")?;
    if end <= 48 {
        return None;
    }
    let s = std::str::from_utf8(&h[48..end]).ok().filter(|s| s.is_ascii())?;
    UV::Version::parse(s).ok()
}

/// The build target the data was made for (from its first serialized file's header).
fn data_platform(data: &str) -> Option<String> {
    let h = sf_head(data);
    if h.is_empty() {
        return None;
    }
    let (off, big) = match UV::sf_header(&h) {
        Ok((_, _, off, big)) => (off, big),
        Err(_) => {
            let fmt = u32e(&h, 8, false)?;
            let end = 48 + find(h.get(48..)?, b"\0")?;
            if fmt < 22 {
                return None;
            }
            (end + 1, h[16] != 0)
        }
    };
    let t = u32e(&h, off, !big)? as i32;
    Some(platform_name(t).map_or_else(|| format!("target {t}"), String::from))
}

fn first(paths: &[String]) -> Option<String> {
    paths.iter().find(|p| exists(p)).cloned()
}

struct Layout {
    kind: &'static str,
    game_dir: String,
    name: String,
    exes: Vec<String>,
    player_lib: Option<String>,
    game_assembly: Option<String>,
    mono: Option<String>,
    plugins_dir: Option<String>,
}

fn layout(data: &str) -> Layout {
    let data = abspath(data);
    if data.ends_with("Contents/Resources/Data") {
        let contents = dirname(dirname(&data)).to_string();
        let app = dirname(&contents).to_string();
        let name = basename(&app);
        // (name[:-len('.app')])
        let cut = name.chars().count().saturating_sub(4);
        let name: String = name.chars().take(cut).collect();
        let macos = join(&contents, "MacOS");
        let exes = if isdir(&macos) {
            sorted_listdir(&macos).iter().map(|n| join(&macos, n)).filter(|p| binary_kind(p).is_some()).collect()
        } else {
            Vec::new()
        };
        let fw = join(&contents, "Frameworks");
        return Layout {
            kind: "macos-app",
            game_dir: app,
            name,
            exes,
            player_lib: first(&[join(&fw, "UnityPlayer.dylib")]),
            game_assembly: first(&[join(&fw, "GameAssembly.dylib")]),
            mono: first(&[join(&contents, "MonoBleedingEdge"), join(&contents, "Mono"), join(&fw, "MonoEmbedRuntime"),
                          join(&fw, "MonoBleedingEdge")]),
            plugins_dir: first(&[join(&contents, "PlugIns"), join(&contents, "Plugins")]),
        };
    }
    let gd = dirname(&data).to_string();
    let base = basename(&data);
    let name = base.strip_suffix("_Data").unwrap_or(base).to_string();
    let mut exes = Vec::new();
    for n in sorted_listdir(&gd) {
        let p = join(&gd, &n);
        if !isfile(&p) || !(n == name || n.starts_with(&format!("{name}."))) || n.ends_with(".debug") {
            continue;
        }
        match binary_kind(&p) {
            Some(("elf", _)) if elf_info(&p).interp.is_some_and(|i| !i.is_empty()) => exes.push(p),
            Some(("pe", _)) if n.to_lowercase().ends_with(".exe") => exes.push(p),
            _ => {}
        }
    }
    Layout {
        kind: "folder",
        game_dir: gd.clone(),
        name,
        exes,
        player_lib: first(&[join(&gd, "UnityPlayer.so"), join(&gd, "UnityPlayer.dll")]),
        game_assembly: first(&[join(&gd, "GameAssembly.so"), join(&gd, "GameAssembly.dll")]),
        mono: first(&[join(&data, "MonoBleedingEdge"), join(&data, "Mono"), join(&gd, "MonoBleedingEdge"), join(&gd, "Mono")]),
        plugins_dir: first(&[join(&data, "Plugins")]),
    }
}

/// Native plugins under the build's plugin folder: [{file, format, arch, what, android, severity, box64}].
fn plugins(plugins_dir: Option<&str>) -> Vec<Val> {
    static KNOWN: OnceLock<Vec<Regex>> = OnceLock::new();
    let known = KNOWN.get_or_init(|| KNOWN_PLUGINS.iter().map(|k| Regex::new(k.0).unwrap()).collect());
    let mut out = Vec::new();
    let Some(pd) = plugins_dir else { return out };
    walk(pd, &mut |root, dirs, files| {
        let mut names: Vec<String> = files.to_vec();
        names.extend(dirs.iter().filter(|d| d.ends_with(".bundle")).cloned());
        names.sort();
        for n in &names {
            let p = join(root, n);
            let k = if n.ends_with(".bundle") {
                let macos = joins(&[&p, "Contents", "MacOS"]);
                if isdir(&macos) {
                    sorted_listdir(&macos).iter().find_map(|x| binary_kind(&join(&macos, x)))
                } else {
                    None
                }
            } else if n.ends_with(".so") || n.ends_with(".dll") || n.ends_with(".dylib") || n.contains(".so.") {
                binary_kind(&p)
            } else {
                continue;
            };
            let Some(k) = k else { continue };
            let mut e = dict(vec![("file", s(relpath(&p, pd))), ("format", s(k.0)), ("arch", s(&k.1))]);
            match known.iter().position(|r| r.is_match(n)) {
                Some(i) => {
                    let (_, what, android, sev, box64) = KNOWN_PLUGINS[i];
                    e.set("what", s(what));
                    e.set("android", s(android));
                    e.set("severity", s(sev));
                    e.set("box64", s(box64));
                }
                None => {
                    e.set("what", Val::Null);
                    e.set("android", s("an x86-only native plugin: the Android player needs an ARM build of it (or a game that runs without it)"));
                    e.set("severity", s("major"));
                    e.set("box64", s("emulated"));
                }
            }
            out.push(Val::Dict(e));
        }
        dirs.retain(|d| !d.ends_with(".bundle"));
    });
    out
}

// ---------------------------------------------------------------------------------------------
// The verdict

fn android_range() -> String {
    format!("{}.{} - {}.{}", UV::FIRST.0, UV::FIRST.1, UV::LAST.0, UV::LAST.1)
}

fn in_range(v: &UV::Version) -> bool {
    UV::FIRST <= v.line() && v.line() <= UV::LAST
}

/// All facts about one build (data folder) and the verdict for both routes.
pub fn examine(data: &str) -> Dict {
    let l = layout(data);
    let mut f = dict(vec![("data", s(data)), ("name", s(&l.name)), ("layout_kind", s(l.kind))]);
    let v = match UV::of_data(data) {
        Ok(v) => Some(v),
        Err(ex) => {
            let v = lenient_version(data);
            if v.is_none() {
                f.set("unity_error", s(&ex.0));
            }
            v
        }
    };
    f.set("unity", opt_s(v.as_ref().map(|v| &v.text)));
    f.set("compressed", Val::Bool(isfile(&join(data, "data.unity3d")) && !isfile(&join(data, "globalgamemanagers"))));
    let platform = data_platform(data).or_else(|| (l.kind == "macos-app").then(|| "macos".to_string()));
    f.set("platform", opt_s(platform.as_ref()));
    let il2cpp = l.game_assembly.is_some() || isdir(&join(data, "il2cpp_data"));
    let mono = isdir(&join(data, "Managed")) && !il2cpp;
    let backend = if il2cpp { Some("il2cpp") } else if mono { Some("mono") } else { None };
    f.set("backend", opt_s(backend));
    f.set("mono_runtime", match &l.mono {
        Some(m) if mono => s(if m.contains("BleedingEdge") || m.contains("EmbedRuntime") {
            "bdwgc (MonoBleedingEdge, .NET 4.x)"
        } else {
            "legacy Mono (.NET 2.0/3.5)"
        }),
        _ => Val::Null,
    });
    let mut exes = Vec::new();
    for p in &l.exes {
        let k = binary_kind(p).unwrap_or(("?", "?".into()));
        let mut e = dict(vec![("file", s(relpath(p, &l.game_dir))), ("format", s(k.0)), ("arch", s(&k.1))]);
        if k.0 == "elf" {
            let ei = elf_info(p);
            e.set("needed", strs(&ei.needed));
            e.set("glibc", opt_s(ei.glibc));
            e.set("glibcxx", opt_s(ei.glibcxx));
        }
        exes.push(e);
    }
    f.set("executables", Val::List(exes.iter().cloned().map(Val::Dict).collect()));
    let mut platform = platform;
    if platform.is_none() {
        let mut kinds: Vec<&str> = exes.iter().map(|e| e.str("format")).collect();
        kinds.sort();
        kinds.dedup();
        platform = match kinds.as_slice() {
            ["pe"] => Some("windows".into()),
            ["elf"] => Some("linux".into()),
            _ => None,
        };
        f.set("platform", opt_s(platform.as_ref()));
    }
    let mut elf: Vec<&Dict> = exes.iter().filter(|e| e.str("format") == "elf").collect();
    elf.sort_by_key(|e| e.str("arch") != "x86_64");
    let player = match &l.player_lib {
        Some(p) if p.ends_with(".so") => Some(p.clone()),
        _ => elf.first().map(|e| join(&l.game_dir, e.str("file"))),
    };
    f.set("player_binary", opt_s(player.as_ref().map(|p| relpath(p, &l.game_dir))));
    if let Some(player) = &player {
        let ps = player_strings(player);
        let pi = elf_info(player);
        f.set("player", vdict(vec![
            ("renderers", strs(&renderers(&ps, v.as_ref()))),
            ("flags", strs(&ps.flags)),
            ("egl", Val::Bool(ps.egl)),
            ("needed", strs(&pi.needed)),
            ("dlopen", strs(&ps.libs)),
            ("glibc", opt_s(pi.glibc)),
            ("glibcxx", opt_s(pi.glibcxx)),
        ]));
    }
    if let Some(ga) = &l.game_assembly {
        let gi = elf_info(ga);
        f.set("game_assembly", vdict(vec![
            ("file", s(relpath(ga, &l.game_dir))),
            ("arch", s(binary_kind(ga).map_or("?".to_string(), |k| k.1))),
            ("needed", strs(&gi.needed)),
            ("glibc", opt_s(gi.glibc)),
        ]));
    }
    f.set("plugins", Val::List(plugins(l.plugins_dir.as_deref())));
    let libs = libraries(&f);
    f.set("libraries", Val::Dict(libs));
    let a = android_verdict(&f, v.as_ref());
    let fits_a = a.truthy("fits");
    f.set("android", Val::Dict(a));
    let b = box64_verdict(&f, v.as_ref());
    let fits_b = b.truthy("fits");
    f.set("box64", Val::Dict(b));
    f.set("route", if fits_a { s("android") } else if fits_b { s("box64") } else { Val::Null });
    f
}

fn renderers(ps: &PlayerStrings, v: Option<&UV::Version>) -> Vec<&'static str> {
    let mut r = Vec::new();
    if ps.flags.iter().any(|f| f.starts_with("glcore")) {
        r.push("glcore");
    }
    if ps.flags.iter().any(|f| f == "vulkan") {
        r.push("vulkan");
    }
    if ps.flags.iter().any(|f| f == "opengl") && v.is_none_or(|v| v.line() < (5, 5)) {
        r.push("opengl2");
    }
    if r.contains(&"glcore") && ps.glx_es && v.is_none_or(|v| v.line() >= (2017, 1)) {
        r.push("gles");
    }
    r
}

/// Shared libraries the Linux build needs, split as box64 loads them.
fn libraries(f: &Dict) -> Dict {
    let mut needed: Vec<String> = Vec::new();
    for e in f.list("executables") {
        if let Some(e) = e.as_dict() {
            needed.extend(str_list(e, "needed"));
        }
    }
    let p = f.dict("player");
    needed.extend(str_list(p, "needed"));
    let mut runtime: Vec<String> = str_list(p, "dlopen").into_iter().filter(|n| runtime_lib(n)).collect();
    needed.extend(str_list(f.dict("game_assembly"), "needed"));
    needed.sort();
    needed.dedup();
    runtime.sort();
    runtime.dedup();
    let mut own: Vec<String> = f.list("plugins").iter().filter_map(Val::as_dict).map(|x| basename(x.str("file")).to_string()).collect();
    own.extend(["UnityPlayer.so".to_string(), "GameAssembly.so".to_string()]);
    let (mut native, mut x64, mut game, mut emulated) = (Vec::new(), Vec::new(), Vec::new(), Vec::new());
    let mut all: Vec<&String> = needed.iter().chain(runtime.iter()).collect();
    all.sort();
    all.dedup();
    for n in all {
        if glibc_own(n) {
            continue;
        }
        if own.contains(n) {
            game.push(n.clone());
        } else if x64lib(n) {
            x64.push(n.clone());
        } else if wrapped(n) {
            native.push(n.clone());
        } else {
            emulated.push(n.clone());
        }
    }
    let rt: Vec<&String> = runtime.iter().filter(|n| !needed.contains(n)).collect();
    dict(vec![("native", strs(&native)), ("x64lib", strs(&x64)), ("game", strs(&game)), ("emulated", strs(&emulated)),
              ("runtime", strs(&rt))])
}

fn android_verdict(f: &Dict, v: Option<&UV::Version>) -> Dict {
    let (mut why, mut block, mut caveats) = (Vec::new(), Vec::new(), Vec::new());
    match v {
        None => block.push("the Unity version cannot be read from the data".to_string()),
        Some(v) if !in_range(v) => block.push(format!("Unity {v}: the converter and runtime cover {}", android_range())),
        Some(v) => why.push(format!("Unity {v} is in the covered range {}", android_range())),
    }
    match f.str("backend") {
        "il2cpp" => block.push("IL2CPP build: the game's code is compiled x86 machine code, there is no IL to run on the Android \
                                player's Mono (unless the game's own Android build is owned)".to_string()),
        "mono" => why.push("Mono build: its assemblies run on the Android player's Mono".to_string()),
        _ => block.push("scripting backend not found (no Managed/ or il2cpp_data/)".to_string()),
    }
    let plat = f.str("platform");
    if plat == "linux" || plat == "macos" {
        why.push(format!("{plat} build: OpenGL Core shaders, converted to GLES 3"));
    } else if plat == "windows" {
        caveats.push("Windows build: Direct3D 11 shaders only, translated with `shaders.from = [\"d3d11\"]` (tools/hlslcc; \
                      pmhlslcc for the handheld)".to_string());
        if v.is_some_and(|v| v.line() >= (2021, 2)) {
            caveats.push("Windows build of 2021.2+: its class libraries are the Windows-only unityjit-win32 ones; \
                          `python -m pmunity.winbcl` fetches the Unix ones for `android-data --managed-file`".to_string());
        }
    } else if !plat.is_empty() {
        block.push(format!("{plat} build: not a desktop build"));
    }
    if f.truthy("compressed") {
        caveats.push("compressed build (data.unity3d): `pmunity unpack` first".to_string());
    }
    for p in f.list("plugins").iter().filter_map(Val::as_dict) {
        if matches!(p.str("arch"), "arm64" | "armv7") {
            continue;
        }
        let what = p.str("what");
        let msg = format!("plugin {}{}: {}", p.str("file"), if what.is_empty() { String::new() } else { format!(" ({what})") },
                          p.str("android"));
        match p.str("severity") {
            "major" if what.is_empty() => block.push(msg),
            "major" => caveats.push(msg + " [major]"),
            "minor" => caveats.push(msg),
            _ => {}
        }
    }
    dict(vec![("fits", Val::Bool(block.is_empty())), ("why", strs(&why)), ("blockers", strs(&block)), ("caveats", strs(&caveats))])
}

fn box64_verdict(f: &Dict, v: Option<&UV::Version>) -> Dict {
    let (mut why, mut block, mut caveats, mut needs) = (Vec::new(), Vec::new(), Vec::new(), Vec::new());
    let linux: Vec<&Dict> = f.list("executables").iter().filter_map(Val::as_dict).filter(|e| e.str("format") == "elf").collect();
    let x64 = linux.iter().find(|e| e.str("arch") == "x86_64");
    let x86 = linux.iter().find(|e| e.str("arch") == "x86");
    let (backend, platform) = (f.str("backend"), f.str("platform"));
    if let Some(e) = x64 {
        why.push(format!("Linux x86_64 build ({}): runs unmodified under box64", e.str("file")));
    } else if let Some(e) = x86 {
        why.push(format!("Linux x86 (32-bit) build ({})", e.str("file")));
        caveats.push("32-bit x86: needs box86 on a 32-bit ARM userland (armhf libraries, GL and SDL2), or box64 built with BOX32 \
                      (experimental); see docs/box64-route.md".to_string());
    } else if backend == "mono" && platform == "macos" && v.is_some() {
        why.push(format!("macOS Mono build: its data runs on Unity's own Linux player of {} (Unity's Linux Mono support \
                          installer), under box64", v.unwrap()));
        caveats.push("the Linux player comes from Unity's installer (download on the device, not shipped)".to_string());
    } else if backend == "il2cpp" {
        block.push(format!("IL2CPP {} build without a Linux one: its code is compiled for {}; get the game's Linux build",
                           platform, if platform.is_empty() { "another system" } else { platform }));
    } else if backend == "mono" && platform == "windows" {
        block.push("Windows-only Mono build: Unity's Linux player needs OpenGL Core shaders and this data has Direct3D ones; \
                    get the macOS or Linux build".to_string());
    } else {
        block.push("no Linux build found".to_string());
    }
    let r = str_list(f.dict("player"), "renderers");
    let has = |x: &str| r.iter().any(|y| y == x);
    if has("opengl2") {
        needs.push("OpenGL 2.1 (legacy renderer): gl4es on GLES 2 works (-force-opengl)".to_string());
    }
    if has("glcore") {
        needs.push("desktop OpenGL 3.2+ core for the OpenGL Core renderer: Mesa (Panfrost / Turnip / llvmpipe); gl4es (GL 2.1) \
                    is not enough".to_string());
    }
    if has("gles") {
        needs.push("or, on GLES-only drivers, the player's OpenGL ES mode: -force-gles with GLES 3 shaders added (pmunity \
                    convert --config tools/box64/gles.json) and glxes (GLX for ES contexts on EGL)".to_string());
    }
    needs.push("an X11 display: Xwayland or PortMaster's Westonpack on firmwares without X".to_string());
    let libs = f.dict("libraries");
    if libs.truthy("native") {
        needs.push(format!("native (aarch64) libraries box64 maps the x86_64 ones to: {}", str_list(libs, "native").join(", ")));
    }
    if libs.truthy("x64lib") {
        needs.push(format!("x86_64 libraries from box64's x64lib, shipped with the port: {}", str_list(libs, "x64lib").join(", ")));
    }
    if libs.truthy("emulated") {
        caveats.push(format!("x86_64 libraries box64 does not wrap; the port must ship x86_64 copies: {}",
                             str_list(libs, "emulated").join(", ")));
    }
    if backend == "mono" {
        caveats.push("Mono JIT under emulation: expect about half the Android route's frame rate (Mono AOT of the assemblies \
                      helps first-use hitches)".to_string());
    }
    for pl in f.list("plugins").iter().filter_map(Val::as_dict) {
        if matches!(pl.str("arch"), "x86_64" | "x86") && pl.truthy("what") && pl.str("box64").contains("fails without") {
            caveats.push(format!("plugin {} ({}): {}", pl.str("file"), pl.str("what"), pl.str("box64")));
        }
    }
    dict(vec![("fits", Val::Bool(block.is_empty())), ("why", strs(&why)), ("blockers", strs(&block)), ("caveats", strs(&caveats)),
              ("needs", strs(&needs))])
}

// ---------------------------------------------------------------------------------------------

/// Human-readable lines for one build.
pub fn report(f: &Dict) -> Vec<String> {
    let or = |k: &str, d: &str| if f.truthy(k) { f.str(k).to_string() } else { d.to_string() };
    let mut out = vec![format!("{}: {}", or("name", "?"), f.str("data"))];
    out.push(format!(
        "  Unity {}, {} build, {}{}",
        or("unity", &format!("? ({})", f.str("unity_error"))),
        or("platform", "?"),
        or("backend", "? backend"),
        if f.truthy("mono_runtime") { format!(", {}", f.str("mono_runtime")) } else { String::new() }
    ));
    for e in f.list("executables").iter().filter_map(Val::as_dict) {
        out.push(format!("  executable {}: {} {}{}", e.str("file"), e.str("format"), e.str("arch"),
                         if e.truthy("glibc") { format!(", needs {}", e.str("glibc")) } else { String::new() }));
    }
    if f.truthy("player") {
        let p = f.dict("player");
        let r = str_list(p, "renderers").join(", ");
        out.push(format!("  player {}: renderers {}{}", f.get("player_binary").map_or("None".into(), Val::py_str),
                         if r.is_empty() { "?" } else { &r },
                         if p.truthy("glibc") { format!("; x86_64 side asks for {}", p.str("glibc")) } else { String::new() }));
    }
    if f.truthy("game_assembly") {
        let g = f.dict("game_assembly");
        out.push(format!("  IL2CPP code {} ({})", g.str("file"), g.str("arch")));
    }
    for pl in f.list("plugins").iter().filter_map(Val::as_dict) {
        out.push(format!("  plugin {}: {} {}{}", pl.str("file"), pl.str("format"), pl.str("arch"),
                         if pl.truthy("what") { format!(" ({})", pl.str("what")) } else { String::new() }));
    }
    let route = f.str("route");
    out.push(format!("route: {}{}", if route.is_empty() { "none" } else { route }, match route {
        "android" => " (Unity's Android player, data converted)",
        "box64" => " (the game's Linux build under box64)",
        _ => "",
    }));
    for name in ["android", "box64"] {
        let r = f.dict(name);
        out.push(format!("  {name}: {}", if r.truthy("fits") { "fits" } else { "does not fit" }));
        for k in ["why", "blockers", "caveats", "needs"] {
            for line in r.list(k) {
                out.push(format!("    {}: {}", k.strip_suffix('s').unwrap_or(k), line.py_str()));
            }
        }
    }
    out
}

/// How the game draws on a handheld: gles (the player's OpenGL ES mode) when the player has it and
/// the converter can add GLES 3 shaders to the data, else gl4es for the legacy OpenGL 2 renderer,
/// else mesa (OpenGL Core).
pub fn gl_mode(f: &Dict) -> &'static str {
    let r = str_list(f.dict("player"), "renderers");
    let v = if f.truthy("unity") { UV::Version::parse(f.str("unity")).ok() } else { None };
    if r.iter().any(|x| x == "gles") && v.is_some_and(|v| in_range(&v)) {
        return "gles";
    }
    if r.iter().any(|x| x == "opengl2") {
        return "gl4es";
    }
    "mesa"
}

fn has_x64(f: &Dict) -> bool {
    f.list("executables").iter().filter_map(Val::as_dict).any(|e| e.str("format") == "elf" && e.str("arch") == "x86_64")
}

/// game.env for the box64 launcher (shell assignments): the game's folder relative to base, its
/// executable and data folder, the Unity version, the backend, the GL mode.
pub fn game_env(f: &Dict, base: &str) -> String {
    let x64 = f.list("executables").iter().filter_map(Val::as_dict)
        .find(|e| e.str("format") == "elf" && e.str("arch") == "x86_64").map_or("", |e| e.str("file"));
    let data = f.str("data");
    let vals = [
        ("GAME_DIR", relpath(dirname(data), &abspath(base))),
        ("EXE", basename(x64).to_string()),
        ("DATA", basename(data).to_string()),
        ("UNITY", f.str("unity").to_string()),
        ("BACKEND", f.str("backend").to_string()),
        ("GL_MODE", gl_mode(f).to_string()),
    ];
    vals.iter().map(|(k, v)| format!("{k}={}\n", shquote(v))).collect()
}

/// The box64 route's launcher (templates/box64/launcher.sh) with its @@NAME@@ values filled, as
/// `port new` fills them.
pub fn launcher(f: &Dict, template: &str, port: Option<&str>, title: Option<&str>) -> Result<String, String> {
    let text = port::routes::read_text(std::path::Path::new(template)).map_err(|e| e.to_string())?;
    let name = f.str("name");
    let port = match port.filter(|p| !p.is_empty()) {
        Some(p) => p.to_string(),
        None => {
            let low = if name.is_empty() { "game".to_string() } else { name.to_lowercase() };
            let p: String = low.chars().filter(|c| c.is_ascii_lowercase() || c.is_ascii_digit()).collect();
            if p.is_empty() { "game".into() } else { p }
        }
    };
    let title = title.filter(|t| !t.is_empty()).map(String::from).unwrap_or_else(|| if name.is_empty() { port.clone() } else { name.to_string() });
    let vals: Vec<(&str, String)> = vec![
        ("READY", ".setup_done".into()),
        ("SETUP_TIME", "a few minutes".into()),
        ("ROUTE", "box64".into()),
        ("TITLE", title.clone()),
        ("PORTDIR", port.clone()),
        ("ZIP", format!("{port}.zip")),
        ("LAUNCHER", format!("{title}.sh")),
        ("GAME_WHAT", format!("the Linux build of {title}")),
    ];
    static TOKEN: OnceLock<Regex> = OnceLock::new();
    let token = rx(&TOKEN, r"@@([A-Z0-9_]+)@@");
    let mut missing: Vec<&str> = token.captures_iter(&text).map(|c| c.get(1).unwrap().as_str())
        .filter(|k| !vals.iter().any(|(n, _)| n == k)).collect();
    missing.sort();
    missing.dedup();
    if !missing.is_empty() {
        return Err(format!("ValueError: template values missing: {}", missing.join(", ")));
    }
    Ok(token.replace_all(&text, |c: &regex::Captures| vals.iter().find(|(n, _)| *n == &c[1]).unwrap().1.clone()).into_owned())
}

pub struct Args {
    pub path: String,
    pub json: bool,
    pub env: bool,
    pub launcher: Option<String>,
    pub port: Option<String>,
    pub title: Option<String>,
    pub template: Option<String>,
}

/// `pmu route`: prints what route.py's main prints; the exit status.
pub fn main(repo: Option<&port::Repo>, a: &Args) -> u8 {
    let dirs = find_data_dirs(&a.path, 4);
    if dirs.is_empty() {
        eprintln!("{}: no Unity data folder (<Game>_Data with globalgamemanagers, mainData or data.unity3d)", a.path);
        return 1;
    }
    let all: Vec<Dict> = dirs.iter().map(|d| examine(d)).collect();
    if a.env {
        let Some(f) = all.iter().find(|x| x.dict("box64").truthy("fits") && has_x64(x)) else {
            eprintln!("{}: no Linux x86-64 build here", a.path);
            return 1;
        };
        let base = if isdir(&a.path) { a.path.clone() } else { dirname(&a.path).to_string() };
        print!("{}", game_env(f, &base));
        return 0;
    }
    if a.json {
        let v = if all.len() > 1 { Val::List(all.iter().cloned().map(Val::Dict).collect()) } else { Val::Dict(all[0].clone()) };
        println!("{}", port::pyjson::dumps(&v, Some(1), true));
    } else {
        for (i, f) in all.iter().enumerate() {
            if i > 0 {
                println!();
            }
            println!("{}", report(f).join("\n"));
        }
    }
    if let Some(out) = &a.launcher {
        let Some(f) = all.iter().find(|x| x.dict("box64").truthy("fits")) else {
            eprintln!("no build here fits the box64 route: no launcher written");
            return 1;
        };
        let template = match (&a.template, repo) {
            (Some(t), _) => t.clone(),
            (None, Some(r)) => port::ps(&r.templates().join("box64").join("launcher.sh")),
            (None, None) => {
                eprintln!("pmu route: no portmaster-unity checkout (setup-rs/templates): set PMUNITY_REPO, or give --template");
                return 2;
            }
        };
        match launcher(f, &template, a.port.as_deref(), a.title.as_deref()) {
            Ok(text) => {
                if let Err(e) = std::fs::write(out, text) {
                    eprintln!("{out}: {e}");
                    return 1;
                }
                eprintln!("launcher: {out}");
            }
            Err(e) => {
                eprintln!("{e}");
                return 1;
            }
        }
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    // Folders made in an order that is neither by name nor its reverse: the data folders are found
    // by name, whatever order the file system lists them in.
    #[test]
    fn data_dirs_are_found_in_name_order() {
        let top = std::env::temp_dir().join(format!("check-data-dirs-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&top);
        for n in ["c", "a", "e", "b", "f", "d"] {
            let d = top.join(n).join("Game_Data");
            std::fs::create_dir_all(&d).unwrap();
            std::fs::write(d.join("globalgamemanagers"), b"").unwrap();
        }
        let top = top.to_string_lossy().into_owned();
        let got = find_data_dirs(&top, 4);
        let _ = std::fs::remove_dir_all(&top);
        let want: Vec<String> = "abcdef"
            .chars()
            .map(|n| format!("{top}/{n}/Game_Data"))
            .collect();
        assert_eq!(got, want);
    }
}
