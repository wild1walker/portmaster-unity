//! The native plugins a game's managed code P/Invokes and what the port gives each (plugins.py):
//! the survey of a data folder (its report, the plugin map's defaults and the float signatures
//! ua_host's thunks need), a port's map over the defaults, and plugins.map.
use crate::clrmeta::{self as C, Assembly, Const, SigType};
use crate::pyjson::Json;
use regex::Regex;
use std::collections::{BTreeSet, HashMap};
use std::path::Path;
use std::sync::{Arc, OnceLock};

/// (id, DllImport name pattern, title, default use, stub id, note)
type Sdk = (
    &'static str,
    &'static str,
    &'static str,
    &'static str,
    Option<&'static str>,
    &'static str,
);

const SDKS: &[Sdk] = &[
    ("fmod", r"fmod(studio)?l?(64)?", "FMOD Engine / FMOD Studio", "lib", None,
     "audio middleware: supply FMOD's ARM libraries of the same version (see \"fmod\")"),
    ("steamworks", r"steam_api(64)?", "Steamworks", "stub", Some("steam"),
     "built-in stub: SteamAPI_Init fails as without a running Steam client"),
    ("steam-encryptedappticket", r"sdkencryptedappticket(64)?", "Steamworks encrypted app tickets", "none", None,
     "only reached after a successful SteamAPI_Init"),
    ("csteamworks", r"csteamworks", "Steamworks.NET (CSteamworks glue, before 9.0)", "stub", Some("csteamworks"),
     "built-in stub: Init fails as without a running Steam client"),
    ("discord-game-sdk", r"discord_game_sdk", "Discord Game SDK", "stub", Some("discord_game_sdk"),
     "built-in stub: DiscordCreate answers NotRunning, as without a Discord client"),
    ("discord-rpc", r"discord-rpc", "Discord RPC", "stub", Some("discord-rpc"),
     "built-in stub: never connects, as without a Discord client"),
    ("discord-partner-sdk", r"discord_partner_sdk", "Discord Social SDK", "none", None,
     "no stub yet: DllNotFoundException at the first call"),
    ("galaxy", r"galaxy(csharpglue|peer)?(64)?", "GOG Galaxy SDK", "none", None,
     "no stub: the SWIG glue throws DllNotFoundException/TypeInitializationException at its first call; \
GOG builds usually catch it (offline mode)"),
    ("eos", r"eossdk-.*", "Epic Online Services", "stub", Some("eos"),
     "built-in stub: EOS_Initialize succeeds, EOS_Platform_Create gives no platform (no network service)"),
    ("wwise", r"aksoundengine", "Audiokinetic Wwise", "lib", None,
     "audio middleware: supply Wwise's Android (or Linux ARM) AkSoundEngine of the same version; \
without it the integration logs an error and the game is silent"),
    ("sqlite", r"(e_)?sqlite3?", "SQLite", "host", None,
     "the system's libsqlite3.so.0 (32-bit for the armv7 player), or an ARM build in lib/"),
    ("standalonefilebrowser", r"standalonefilebrowser", "StandaloneFileBrowser (native file dialogs)", "none", None,
     "no file dialogs on a handheld: DllNotFoundException when the game opens one"),
    ("xinput", r"xinputinterface(32|64)?", "XInputDotNet", "none", None, "Windows-only"),
    ("rewired", r"rewired_.*", "Rewired native helper", "none", None, "Rewired uses its Unity-input path on Android"),
    ("steamaudio", r"phonon", "Steam Audio", "lib", None, "supply an ARM build of phonon"),
    ("sdl2", r"sdl2(-2\.0)?", "SDL2", "keep", None,
     "left to Mono's dllmap and the loader (a port maps it with use host libSDL2-2.0.so.0 or --dllmap)"),
    ("openal", r"(soft_oal|openal(32)?)", "OpenAL", "keep", None, "a port can map the system's libopenal.so.1 (use host)"),
    ("udev", r"udev", "libudev (joystick enumeration, e.g. Rewired on Linux)", "keep", None,
     "Rewired's Linux path: on the Android player see tools/asmpatch rewired-android"),
];

fn host_default(sdk: &str) -> Option<&'static str> {
    (sdk == "sqlite").then_some("libsqlite3.so.0")
}

struct Res {
    sdks: Vec<Regex>,
    system: Regex,
    windows: Regex,
    macos: Regex,
    framework: Regex,
    file_notes: Vec<(Regex, &'static str)>,
    norm: Regex,
    lib_file: Regex,
    fmod_file: Regex,
    soname_num: Regex,
    plist: Regex,
    space: Regex,
}

fn re() -> &'static Res {
    static R: OnceLock<Res> = OnceLock::new();
    R.get_or_init(|| Res {
        sdks: SDKS.iter().map(|s| Regex::new(&format!("(?i)^(?:{})$", s.1)).unwrap()).collect(),
        system: Regex::new(
            r"(?i)^(__internal|libc|c|libc\.so(\.6)?|libdl(\.so(\.2)?)?|dl|libm|m|libpthread|pthread|libz|z|zlib1?|monoposixhelper|system\.native|system\.net\.security\.native|system\.globalization\.native|system\.io\.compression\.native|libmono-btls-shared|mono-native)$",
        )
        .unwrap(),
        windows: Regex::new(
            r"(?i)(^(kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|ws2_32|winmm|comdlg32|crypt32|iphlpapi|winspool|ntdll|dwmapi|shcore|xinput[0-9_]*|dinput8|psapi|version|imm32|setupapi|hid|dbghelp|d3d\w*|dxgi|opengl32|msvcrt|ucrtbase)(\.dll|\.drv)?$)|\.dll$",
        )
        .unwrap(),
        macos: Regex::new(r"(?i)^/System/|^/usr/lib/|\.dylib$|\.framework|\.bundle$").unwrap(),
        framework: Regex::new(
            r"(?i)^(mscorlib|netstandard|System|System\..*|Mono\.(Security|Posix|Cecil.*|CompilerServices.*|Simd|Tasklets|WebBrowser)|UnityEngine(\..*)?|Microsoft\..*|Boo\..*|UnityScript.*|mcs|nunit\.framework|ICSharpCode\..*)\.dll$",
        )
        .unwrap(),
        file_notes: vec![
            (Regex::new(r"(?i)lib_burst_generated").unwrap(),
             "Burst-compiled code: absent on ARM, Burst runs the managed versions of those jobs"),
            (Regex::new(r"(?i)screenselector").unwrap(), "the Linux player's own launcher dialog (not used by the game's code)"),
            (Regex::new(r"(?i)resonanceaudio").unwrap(),
             "an FMOD DSP plugin (Google Resonance Audio): needed only if the FMOD settings load it; supply its ARM build with FMOD's"),
            (Regex::new(r"(?i)(fmod|ak).*plugin|libfmod_.*|.*_fmod").unwrap(),
             "an FMOD/Wwise plugin: supply its ARM build if the game loads it"),
        ],
        norm: Regex::new(r"(?i)(\.so(\.\d+)*|\.dll|\.dylib|\.bundle)$").unwrap(),
        lib_file: Regex::new(r"(?i)(\.so(\.\d+)*|\.dll|\.dylib)$").unwrap(),
        fmod_file: Regex::new(r"(?i)fmod").unwrap(),
        soname_num: Regex::new(r"\.so\.(\d+)").unwrap(),
        plist: Regex::new(r"<key>CFBundle(?:ShortVersionString|Version)</key>\s*<string>([^<]+)</string>").unwrap(),
        space: Regex::new(r"\s").unwrap(),
    })
}

/// A DllImport name without directory, 'lib' prefix and library extension.
pub fn norm(name: &str) -> String {
    let n = name.replace('\\', "/");
    let n = n.rsplit('/').next().unwrap_or("").to_string();
    let n = re().norm.replace(&n, "").into_owned();
    if n.to_lowercase().starts_with("lib") && n.chars().count() > 3 {
        return n.chars().skip(3).collect();
    }
    n
}

fn sdk_of(name: &str) -> Option<&'static Sdk> {
    let n = norm(name).to_lowercase();
    SDKS.iter()
        .zip(&re().sdks)
        .find(|(_, r)| r.is_match(&n))
        .map(|(s, _)| s)
}

// ------------------------------------------------------------------- argument classes

/// (kind, size) of a scalar leaf: 'f' float, 'd' double, 'i' integer or pointer.
type Leaf = (char, u32);

fn known_valuetype(name: &str) -> Option<&'static str> {
    Some(match name {
        "System.IntPtr" | "System.UIntPtr" | "System.Boolean" | "System.Char" | "System.SByte"
        | "System.Byte" | "System.Int16" | "System.UInt16" | "System.Int32" | "System.UInt32" => {
            "i"
        }
        "System.Int64" | "System.UInt64" => "l",
        "System.Single" => "f",
        "System.Double" => "d",
        "System.Guid" => "s4",
        "UnityEngine.Vector2" => "F2",
        "UnityEngine.Vector3" => "F3",
        "UnityEngine.Vector4"
        | "UnityEngine.Quaternion"
        | "UnityEngine.Color"
        | "UnityEngine.Rect" => "F4",
        "UnityEngine.Color32" => "s1",
        "UnityEngine.Vector2Int" => "s2",
        "UnityEngine.Vector3Int" => "s3",
        _ => return None,
    })
}

/// Opens the assemblies of a Managed folder on demand to resolve by-value types.
pub struct Resolver {
    managed: String,
    cache: HashMap<String, Option<Arc<Assembly>>>,
}

impl Resolver {
    pub fn new(managed: &str) -> Self {
        Resolver {
            managed: managed.to_string(),
            cache: HashMap::new(),
        }
    }

    pub fn get(&mut self, name: &str) -> C::Result<Option<Arc<Assembly>>> {
        if let Some(a) = self.cache.get(name) {
            return Ok(a.clone());
        }
        let p = pkgzip::py::join(&self.managed, &format!("{name}.dll"));
        let a = if Path::new(&p).is_file() {
            match Assembly::open(&p) {
                Ok(a) => Some(Arc::new(a)),
                Err(e) if e.is_bad_assembly() => None,
                Err(e) => return Err(e),
            }
        } else {
            None
        };
        self.cache.insert(name.to_string(), a.clone());
        Ok(a)
    }

    /// (assembly, typedef row) of a TypeDef/TypeRef of asm, following TypeRefs into other
    /// assemblies; else (None, the TypeRef's full name if any).
    #[allow(clippy::type_complexity)]
    fn find(
        &mut self,
        asm: &Arc<Assembly>,
        tab: u8,
        row: u32,
    ) -> C::Result<(Option<(Arc<Assembly>, u32)>, Option<String>)> {
        if tab == C::TYPEDEF {
            return Ok((Some((asm.clone(), row)), None));
        }
        if tab != C::TYPEREF {
            return Ok((None, None));
        }
        let full = asm.typeref_name(row)?;
        let scope = asm.typeref_scope(row)?;
        let other = match scope.filter(|s| !s.is_empty()) {
            Some(s) => self.get(&s)?,
            None => None,
        };
        let Some(other) = other else {
            return Ok((None, Some(full)));
        };
        let i = other.types()?.iter().position(|t| t.full == full);
        Ok(match i {
            Some(i) => (Some((other.clone(), i as u32 + 1)), None),
            None => (None, Some(full)),
        })
    }
}

fn leaf_classes(
    res: &mut Resolver,
    asm: &Arc<Assembly>,
    t: &SigType,
    depth: u32,
) -> C::Result<Option<Vec<Leaf>>> {
    if depth > 8 {
        return Ok(None);
    }
    Ok(match t {
        SigType::Prim(e) => Some(vec![match *e {
            C::E_R4 => ('f', 4),
            C::E_R8 => ('d', 8),
            C::E_I8 | C::E_U8 => ('i', 8),
            C::E_STRING | C::E_OBJECT | C::E_I | C::E_U => ('i', 4),
            e => ('i', C::prim_size(e).unwrap_or(4)),
        }]),
        SigType::Ptr(_)
        | SigType::ByRef(_)
        | SigType::Class(..)
        | SigType::Array(_)
        | SigType::FnPtr => Some(vec![('i', 4)]),
        SigType::ValueType(name, r) => {
            let Some((tab, row)) = *r else {
                return Ok(None);
            };
            let (found, full) = res.find(asm, tab, row)?;
            let Some((a, row)) = found else {
                let key = full.as_deref().unwrap_or(name);
                let Some(v) = known_valuetype(key) else {
                    return Ok(None);
                };
                return Ok(Some(match v {
                    "i" => vec![('i', 4)],
                    "l" => vec![('i', 8)],
                    "f" => vec![('f', 4)],
                    "d" => vec![('d', 8)],
                    _ => {
                        let n: usize = v[1..].parse().unwrap();
                        if v.starts_with('F') {
                            vec![('f', 4); n]
                        } else {
                            vec![('i', 4); n]
                        }
                    }
                }));
            };
            let (base, fields, size, _flags) = a.typedef_layout(row)?;
            if base.as_deref() == Some("System.Enum") {
                for (fname, ft) in &fields {
                    if fname == "value__" {
                        if let Some(ft) = ft {
                            return leaf_classes(res, &a, ft, depth + 1);
                        }
                    }
                }
                return Ok(Some(vec![('i', 4)]));
            }
            let mut out = Vec::new();
            for (_, ft) in &fields {
                let Some(ft) = ft else {
                    return Ok(None);
                };
                let Some(sub) = leaf_classes(res, &a, ft, depth + 1)? else {
                    return Ok(None);
                };
                out.extend(sub);
            }
            let total: u32 = out.iter().map(|l| l.1).sum();
            if let Some(size) = size {
                if total < size {
                    out.push(('i', size - total));
                }
            }
            if out.is_empty() {
                Some(vec![('i', 4)])
            } else {
                Some(out)
            }
        }
    })
}

/// The argument class of one P/Invoke argument or result, as ua_host's thunks read it.
fn abi_token(res: &mut Resolver, asm: &Arc<Assembly>, t: &SigType) -> C::Result<String> {
    if *t == SigType::Prim(C::E_VOID) {
        return Ok("v".into());
    }
    let Some(leaves) = leaf_classes(res, asm, t, 0)? else {
        return Ok("?".into());
    };
    let is_vt = matches!(t, SigType::ValueType(..));
    if matches!(t, SigType::Prim(_)) || (leaves.len() == 1 && !is_vt) {
        let (kind, size) = leaves[0];
        return Ok(match kind {
            'f' => "f".into(),
            'd' => "d".into(),
            _ if size == 8 => "l".into(),
            _ => "i".into(),
        });
    }
    let kinds: BTreeSet<char> = leaves.iter().map(|l| l.0).collect();
    let only = |c: char| kinds.len() == 1 && kinds.contains(&c);
    if is_vt && leaves.len() == 1 && !only('f') && !only('d') {
        return Ok(if leaves[0].1 == 8 { "l" } else { "i" }.into());
    }
    if only('f') && leaves.len() <= 4 {
        return Ok(format!("F{}", leaves.len()));
    }
    if only('d') && leaves.len() <= 4 {
        return Ok(format!("D{}", leaves.len()));
    }
    let (mut size, mut align) = (0u32, 4u32);
    for &(_, s) in &leaves {
        let a = if s == 8 { 8 } else { s.min(4) };
        align = align.max(a);
        size = size.div_ceil(a) * a + s;
    }
    let words = size.div_ceil(4);
    Ok(if align == 8 {
        format!("S{}", words.div_ceil(2) * 2)
    } else {
        format!("s{words}")
    })
}

/// (result, [arguments]) classes.
pub type Abi = (String, Vec<String>);

fn first_in(tok: &str, set: &str) -> bool {
    tok.chars().next().is_some_and(|c| set.contains(c))
}

fn needs_thunk(sig: &Abi) -> bool {
    std::iter::once(&sig.0)
        .chain(&sig.1)
        .any(|x| first_in(x, "fdFD"))
}

// ------------------------------------------------------------------- files

/// describe_file's result: {format, arch, soname, needed, android, version} as the Python's dicts.
#[derive(Default, Clone)]
pub struct FileInfo {
    pub format: String,
    pub arch: Option<String>,
    pub soname: Option<String>,
    pub needed: Vec<String>,
    pub android: bool,
    pub version: Option<String>,
}

fn elf_machine(m: u16) -> String {
    match m {
        3 => "x86".into(),
        62 => "x86-64".into(),
        40 => "arm".into(),
        183 => "arm64".into(),
        m => m.to_string(),
    }
}

fn latin1(b: &[u8]) -> String {
    b.iter().map(|&c| c as char).collect()
}

/// {'format': 'elf', ...} of an ELF file, or None.
pub fn elf_info(path: &str) -> Option<FileInfo> {
    let d = std::fs::read(path).ok()?;
    if d.len() < 4 || &d[..4] != b"\x7fELF" {
        return None;
    }
    let is64 = d.get(4) == Some(&2);
    let little = d.get(5) == Some(&1);
    let u16_ = |o: usize| {
        d.get(o..o + 2).map(|b| {
            if little {
                u16::from_le_bytes([b[0], b[1]])
            } else {
                u16::from_be_bytes([b[0], b[1]])
            }
        })
    };
    let u32_ = |o: usize| {
        d.get(o..o + 4).map(|b| {
            let a: [u8; 4] = b.try_into().unwrap();
            if little {
                u32::from_le_bytes(a)
            } else {
                u32::from_be_bytes(a)
            }
        })
    };
    let u64_ = |o: usize| {
        d.get(o..o + 8).map(|b| {
            let a: [u8; 8] = b.try_into().unwrap();
            if little {
                u64::from_le_bytes(a)
            } else {
                u64::from_be_bytes(a)
            }
        })
    };
    let mach = u16_(18)?;
    let mut info = FileInfo {
        format: "elf".into(),
        arch: Some(elf_machine(mach)),
        ..Default::default()
    };
    // (struct.error, ValueError, IndexError: what was found so far stays)
    let _ = (|| -> Option<()> {
        let (phoff, phsz, phn) = if is64 {
            (u64_(32)?, u16_(54)? as u64, u16_(56)?)
        } else {
            (u32_(28)? as u64, u16_(42)? as u64, u16_(44)?)
        };
        let mut loads: Vec<(u64, u64, u64)> = Vec::new();
        let mut dynamic = None;
        for i in 0..phn as u64 {
            let o = (phoff + i * phsz) as usize;
            let (pt, off, va, fsz) = if is64 {
                (u32_(o)?, u64_(o + 8)?, u64_(o + 16)?, u64_(o + 32)?)
            } else {
                (
                    u32_(o)?,
                    u32_(o + 4)? as u64,
                    u32_(o + 8)? as u64,
                    u32_(o + 16)? as u64,
                )
            };
            match pt {
                1 => loads.push((va, off, fsz)),
                2 => dynamic = Some((off, fsz)),
                4 => {
                    let s = (off as usize).min(d.len());
                    let e = (off.saturating_add(fsz) as usize).min(d.len()).max(s);
                    if pkgzip::py::find(&d[s..e], b"Android").is_some() {
                        info.android = true;
                    }
                }
                _ => {}
            }
        }
        if let Some((doff, dsz)) = dynamic {
            let mut ents = Vec::new();
            let mut strtab = None;
            let w = if is64 { 16 } else { 8 };
            let mut o = doff;
            while o < doff + dsz {
                let (tag, val) = if is64 {
                    (u64_(o as usize)? as i64, u64_(o as usize + 8)?)
                } else {
                    (
                        u32_(o as usize)? as i32 as i64,
                        u32_(o as usize + 4)? as u64,
                    )
                };
                if tag == 0 {
                    break;
                }
                ents.push((tag, val));
                if tag == 5 {
                    strtab = Some(val);
                }
                o += w;
            }
            if let Some(st) = strtab {
                let so = loads
                    .iter()
                    .find(|&&(va, _, fsz)| va <= st && st < va + fsz)
                    .map(|&(va, off, _)| (off + st).wrapping_sub(va));
                if let Some(so) = so {
                    let s = |i: u64| -> Option<String> {
                        let a = so.checked_add(i)? as usize;
                        let e = a + d.get(a..)?.iter().position(|&c| c == 0)?;
                        Some(latin1(&d[a..e]))
                    };
                    let mut needed = Vec::new();
                    for &(t, v) in &ents {
                        if t == 1 {
                            needed.push(s(v)?);
                        }
                    }
                    info.needed = needed;
                    let mut soname = None;
                    if let Some(&(_, v)) = ents.iter().find(|e| e.0 == 14) {
                        soname = Some(s(v)?);
                    }
                    info.soname = soname;
                }
            }
        }
        if info
            .needed
            .iter()
            .any(|n| n == "libc.so" || n == "liblog.so")
        {
            info.android = true;
        }
        Some(())
    })();
    Some(info)
}

/// FileVersion / ProductVersion of a Windows DLL, or None.
pub fn pe_version(path: &str) -> Option<String> {
    let d = std::fs::read(path).ok()?;
    if d.len() < 2 || &d[..2] != b"MZ" {
        return None;
    }
    for key in ["ProductVersion", "FileVersion"] {
        let k: Vec<u8> = format!("{key}\0")
            .encode_utf16()
            .flat_map(u16::to_le_bytes)
            .collect();
        if let Some(i) = pkgzip::py::find(&d, &k) {
            let mut j = i + k.len();
            j += (4 - j % 4) % 4;
            let mut e = j;
            while e + 1 < d.len() && d[e..e + 2] != [0, 0] {
                e += 2;
            }
            let v = C::utf16le_replace(d.get(j..e).unwrap_or(&[]));
            let v = pkgzip::py::strip(&v).to_string();
            if !v.is_empty() {
                return Some(v);
            }
        }
    }
    None
}

fn plist_version(bundle: &str) -> Option<String> {
    let p = format!("{bundle}/Contents/Info.plist");
    let b = std::fs::read(p).ok()?;
    let txt = String::from_utf8_lossy(&b);
    let m = re().plist.captures(&txt)?;
    Some(pkgzip::py::strip(&m[1]).to_string())
}

fn walk_sorted(root: &str, mut f: impl FnMut(&str, &mut Vec<String>, &[String])) {
    // os.walk, top-down, the callback sorting and pruning the subfolders
    let mut stack = vec![root.to_string()];
    while let Some(r) = stack.pop() {
        let Ok(rd) = std::fs::read_dir(&r) else {
            continue;
        };
        let (mut ds, mut fs) = (Vec::new(), Vec::new());
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            let p = Path::new(&r).join(&n);
            if p.is_dir() {
                ds.push(n);
            } else {
                fs.push(n);
            }
        }
        f(&r, &mut ds, &fs);
        for d in ds.iter().rev() {
            let p = format!("{r}/{d}");
            if !Path::new(&p).is_symlink() {
                stack.push(p);
            }
        }
    }
}

fn relpath(p: &str, base: &str) -> String {
    crate::paths::relpath(p, base)
}

/// The native libraries the build ships: (path relative to the data's parent, path).
pub fn plugin_files(data: &str) -> Vec<(String, String)> {
    let mut out = Vec::new();
    let mut roots = vec![pkgzip::py::join(data, "Plugins")];
    let abs = crate::paths::abspath(data);
    let contents = pkgzip::py::dirname(pkgzip::py::dirname(&abs)).to_string();
    if pkgzip::py::basename(&contents) == "Contents" {
        roots.push(pkgzip::py::join(&contents, "Plugins"));
        roots.push(pkgzip::py::join(&contents, "Frameworks"));
    }
    for root in roots {
        if !Path::new(&root).is_dir() {
            continue;
        }
        let parent = pkgzip::py::dirname(&root).to_string();
        walk_sorted(&root, |r, ds, fs| {
            ds.sort();
            for n in ds.clone() {
                if n.ends_with(".bundle") || n.ends_with(".framework") {
                    let p = pkgzip::py::join(r, &n);
                    out.push((relpath(&p, &parent), p));
                    ds.retain(|x| *x != n);
                }
            }
            let mut fs = fs.to_vec();
            fs.sort();
            for n in fs {
                if n.starts_with('.') || !re().lib_file.is_match(&n) {
                    continue;
                }
                let p = pkgzip::py::join(r, &n);
                out.push((relpath(&p, &parent), p));
            }
        });
    }
    out
}

pub fn describe_file(path: &str) -> FileInfo {
    if Path::new(path).is_dir() {
        return FileInfo {
            format: "macos-bundle".into(),
            version: plist_version(path),
            ..Default::default()
        };
    }
    if let Some(e) = elf_info(path) {
        return e;
    }
    if path.to_lowercase().ends_with(".dll") {
        let head = std::fs::read(path).unwrap_or_default();
        if head.starts_with(b"MZ") {
            if Assembly::open(path).is_ok() {
                return FileInfo {
                    format: "managed".into(),
                    ..Default::default()
                };
            }
            return FileInfo {
                format: "pe".into(),
                version: pe_version(path),
                ..Default::default()
            };
        }
    }
    if path.ends_with(".dylib") {
        return FileInfo {
            format: "mach-o".into(),
            ..Default::default()
        };
    }
    FileInfo {
        format: "unknown".into(),
        ..Default::default()
    }
}

// ------------------------------------------------------------------- FMOD

fn fmod_version_str(n: u64) -> String {
    format!(
        "{:x}.{:02x}.{:02x}",
        n >> 16 & 0xffff,
        n >> 8 & 0xff,
        n & 0xff
    )
}

fn fmod_soname(n: &str) -> Option<&'static str> {
    Some(match n {
        "10" => "1.10",
        "11" => "2.00",
        "12" => "2.01",
        "13" => "2.02",
        "14" => "2.03",
        _ => return None,
    })
}

fn const_json(c: &Const) -> Json {
    match c {
        Const::Int(i) => Json::Int(*i as i128),
        Const::UInt(u) => Json::Int(*u as i128),
        Const::Str(s) => Json::Str(s.clone()),
        Const::None => Json::Null,
    }
}

fn fmod_info(
    constants: &[(String, Const)],
    pins: &[Pin],
    files: &[(String, String)],
) -> Option<Json> {
    let num = constants.iter().find_map(|(k, v)| {
        if !k.ends_with("FMOD.VERSION.number") {
            return None;
        }
        match v {
            Const::Int(i) => Some(*i as i128),
            Const::UInt(u) => Some(*u as i128),
            _ => None,
        }
    });
    let names: Vec<String> = pins
        .iter()
        .filter(|p| sdk_of(&p.module).is_some_and(|s| s.0 == "fmod"))
        .map(|p| p.module.clone())
        .collect::<BTreeSet<_>>()
        .into_iter()
        .collect();
    if num.is_none() && names.is_empty() {
        return None;
    }
    // (FMOD's version numbers are positive and small: a 32-bit unsigned value)
    let numu = num.map(|n| n as u64);
    let ver = numu.map(fmod_version_str);
    let mut info = Json::obj();
    info.set("header_version", ver.clone().into());
    info.set(
        "header_version_hex",
        numu.map(|n| format!("0x{n:08x}")).into(),
    );
    info.set("dllimport_names", names.clone().into());
    if let Some((_, suffix)) = constants
        .iter()
        .find(|(k, _)| k.ends_with("FMOD.VERSION.dllSuffix"))
    {
        let j = const_json(suffix);
        if j.truthy() {
            info.set("dll_suffix", j);
        }
    }
    let mut native = Vec::new();
    let mut lines = BTreeSet::new();
    for (rel, p) in files {
        if !re().fmod_file.is_match(pkgzip::py::basename(rel)) {
            continue;
        }
        let d = describe_file(p);
        let mut e = Json::obj();
        e.set("file", rel.as_str().into());
        e.set("format", d.format.as_str().into());
        e.set("arch", d.arch.clone().into());
        if let Some(so) = &d.soname {
            if !so.is_empty() {
                e.set("soname", so.as_str().into());
                if let Some(m) = re().soname_num.captures(so) {
                    if let Some(l) = fmod_soname(&m[1]) {
                        e.set("line", l.into());
                        lines.insert(l.to_string());
                    }
                }
            }
        }
        if let Some(v) = &d.version {
            if !v.is_empty() {
                e.set("version", v.as_str().into());
            }
        }
        if let Some(n) = numu {
            if !Path::new(p).is_dir() {
                let data = std::fs::read(p).unwrap_or_default();
                let le = (n as u32).to_le_bytes();
                e.set(
                    "contains_header_version",
                    pkgzip::py::find(&data, &le).is_some().into(),
                );
            }
        }
        native.push(e);
    }
    info.set("desktop_libraries", Json::List(native));
    let line = ver
        .as_ref()
        .map(|v| v.split('.').take(2).collect::<Vec<_>>().join("."));
    if let Some(line) = &line {
        if !lines.is_empty() && !lines.contains(line) {
            info.set(
                "warning",
                format!(
                    "the managed wrapper says FMOD {}, the desktop library is of line {}",
                    ver.as_deref().unwrap_or(""),
                    lines.iter().cloned().collect::<Vec<_>>().join(", ")
                )
                .into(),
            );
        }
    }
    let core_in_studio = names == ["fmodstudio"];
    info.set(
        "core_dllimport",
        if core_in_studio {
            Json::from("fmodstudio")
        } else if names.iter().any(|n| n == "fmod") {
            Json::from("fmod")
        } else {
            Json::Null
        },
    );
    let v = ver.clone().unwrap_or_else(|| "<version>".into());
    let vv = v.replace('.', "");
    let files_android: Vec<&str> = if names.iter().any(|n| n == "fmodstudio") || core_in_studio {
        vec!["libfmod.so", "libfmodstudio.so"]
    } else {
        vec!["libfmod.so"]
    };
    let mut android = Json::obj();
    android.set(
        "files",
        files_android
            .iter()
            .map(|s| Json::from(*s))
            .collect::<Vec<_>>()
            .into(),
    );
    android.set(
        "abi",
        "armeabi-v7a for the armv7 Mono player, arm64-v8a for an arm64 player".into(),
    );
    android.set(
        "from",
        vec![
            format!(
                "FMOD Engine {v} for Android (fmodstudioapi{vv}android.tar.gz): api/core/lib/<abi>/libfmod.so, api/studio/lib/<abi>/libfmodstudio.so"
            ),
            format!(
                "or FMOD for Unity {v} (fmodstudio{vv}.unitypackage): the Android libraries under Assets/Plugins/FMOD/.../android/.../<abi>/"
            ),
        ]
        .into(),
    );
    android.set("to", "lib/ (beside libunity.so)".into());
    let mut linux = Json::obj();
    linux.set(
        "files",
        files_android
            .iter()
            .map(|s| Json::from(*s))
            .collect::<Vec<_>>()
            .into(),
    );
    linux.set(
        "from",
        format!(
            "FMOD Engine {v} for Linux (fmodstudioapi{vv}linux.tar.gz): api/core/lib/arm/ (armhf) or arm64/, api/studio/lib/arm/ or arm64/"
        )
        .into(),
    );
    linux.set(
        "to",
        "lib/ under those names (libfmod.so.<n>.<m> -> libfmod.so; ua_host loads a library's DT_NEEDED siblings from lib/ first)"
            .into(),
    );
    linux.set(
        "note",
        "alternative: output through ALSA/PulseAudio (not the runtime's audio path); on the armv7 player calls with float arguments go through ua_host's softfp thunks (sig lines)"
            .into(),
    );
    let mut supply = Json::obj();
    supply.set("version", v.as_str().into());
    supply.set("android", android);
    supply.set("linux", linux);
    supply.set(
        "plugins",
        "FMOD DSP plugins the game's FMOD settings load (e.g. libresonanceaudio.so) from the same release".into(),
    );
    supply.set(
        "license",
        "FMOD's licence applies (free for small developers; downloads need an FMOD account); nothing of FMOD is redistributed with the port tools"
            .into(),
    );
    info.set("supply", supply);
    Some(info)
}

// ------------------------------------------------------------------- survey

struct Pin {
    module: String,
    entry: String,
    typ: String,
    assembly: String,
    abi: Option<Abi>,
}

/// The plugin report of a desktop data folder.
pub struct Survey {
    /// {name: entry} in the report's order
    pub modules: Json,
    pub files: Json,
    pub fmod: Option<Json>,
    pub sigs: Vec<(String, String, String, Vec<String>)>,
    pub unreadable: Vec<String>,
    /// the default plugin map {name: {use, ...}}
    pub map: Json,
}

/// An error the Python does not catch (a KeyError, an OSError): the survey stops with it.
pub type SurveyResult<T> = Result<T, String>;

fn sorted_lower(names: &[String]) -> Vec<String> {
    let mut v = names.to_vec();
    v.sort_by_key(|n| n.to_lowercase());
    v
}

pub fn survey(data: &str) -> SurveyResult<Survey> {
    let managed = pkgzip::py::join(data, "Managed");
    let mut res = Resolver::new(&managed);
    let mut pins: Vec<Pin> = Vec::new();
    let mut consts: Vec<(String, Const)> = Vec::new();
    let mut bad: Vec<String> = Vec::new();
    if Path::new(&managed).is_dir() {
        let mut names: Vec<String> = std::fs::read_dir(&managed)
            .map_err(|e| e.to_string())?
            .flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .collect();
        names.sort();
        for n in names {
            if !n.ends_with(".dll") || re().framework.is_match(&n) {
                continue;
            }
            let a = res.get(&n[..n.len() - 4]).map_err(|e| e.to_string())?;
            let Some(a) = a else {
                bad.push(n);
                continue;
            };
            let r = (|| -> C::Result<()> {
                for p in a.pinvokes()? {
                    let abi = match &p.sig {
                        Some((ret, ps)) => {
                            let r = abi_token(&mut res, &a, ret)?;
                            let mut v = Vec::new();
                            for x in ps {
                                v.push(abi_token(&mut res, &a, x)?);
                            }
                            Some((r, v))
                        }
                        None => None,
                    };
                    pins.push(Pin {
                        module: p.module,
                        entry: p.entry,
                        typ: p.typ,
                        assembly: n.clone(),
                        abi,
                    });
                }
                for (k, v) in a.field_constants()? {
                    match consts.iter_mut().find(|(x, _)| *x == k) {
                        Some(e) => e.1 = v,
                        None => consts.push((k, v)),
                    }
                }
                Ok(())
            })();
            match r {
                Ok(()) => {}
                Err(e) if e.is_value() => bad.push(format!("{n}: {e}")),
                Err(e) => return Err(e.to_string()),
            }
        }
    }
    let files = plugin_files(data);
    // modules in the order first seen; entries {entry: abi} likewise
    struct Mod {
        entries: Vec<(String, Option<Abi>)>,
        assemblies: BTreeSet<String>,
        types: BTreeSet<String>,
    }
    let mut mods: Vec<(String, Mod)> = Vec::new();
    for p in &pins {
        let i = match mods.iter().position(|(n, _)| *n == p.module) {
            Some(i) => i,
            None => {
                mods.push((
                    p.module.clone(),
                    Mod {
                        entries: Vec::new(),
                        assemblies: BTreeSet::new(),
                        types: BTreeSet::new(),
                    },
                ));
                mods.len() - 1
            }
        };
        let m = &mut mods[i].1;
        match m.entries.iter_mut().find(|(e, _)| *e == p.entry) {
            Some(e) => e.1 = p.abi.clone(),
            None => m.entries.push((p.entry.clone(), p.abi.clone())),
        }
        m.assemblies.insert(p.assembly.clone());
        m.types.insert(p.typ.clone());
    }
    let mut out = Json::obj();
    let mut used_files: BTreeSet<String> = BTreeSet::new();
    let names: Vec<String> = mods.iter().map(|(n, _)| n.clone()).collect();
    for name in sorted_lower(&names) {
        let m = &mods.iter().find(|(n, _)| *n == name).unwrap().1;
        let key = norm(&name).to_lowercase();
        let matched: Vec<String> = files
            .iter()
            .filter(|(rel, _)| norm(rel).to_lowercase() == key)
            .map(|(rel, _)| rel.clone())
            .collect();
        used_files.extend(matched.iter().cloned());
        let sdk = sdk_of(&name);
        let mut e = Json::obj();
        e.set("entries", m.entries.len().into());
        e.set(
            "assemblies",
            m.assemblies.iter().cloned().collect::<Vec<_>>().into(),
        );
        e.set(
            "types",
            m.types.iter().take(8).cloned().collect::<Vec<_>>().into(),
        );
        e.set("desktop_files", matched.clone().into());
        let mut floats: Vec<String> = m
            .entries
            .iter()
            .filter(|(_, v)| v.as_ref().is_some_and(needs_thunk))
            .map(|(k, _)| k.clone())
            .collect();
        floats.sort();
        let mut unknown: Vec<String> = m
            .entries
            .iter()
            .filter(|(_, v)| match v {
                None => true,
                Some((r, ps)) => r == "?" || ps.iter().any(|x| x == "?"),
            })
            .map(|(k, _)| k.clone())
            .collect();
        unknown.sort();
        if !floats.is_empty() {
            e.set("float_args", floats.len().into());
        }
        if !unknown.is_empty() {
            e.set(
                "unknown_args",
                unknown.into_iter().take(10).collect::<Vec<_>>().into(),
            );
        }
        if let Some(s) = sdk {
            e.set("sdk", s.0.into());
            e.set("title", s.2.into());
            e.set("use", s.3.into());
            e.set("note", s.5.into());
            if let Some(st) = s.4 {
                e.set("stub", st.into());
            }
            if s.3 == "host" {
                e.set("lib", host_default(s.0).into());
            }
        } else if re().system.is_match(&name) {
            e.set("use", "system".into());
            e.set("note", "C library / Mono runtime library: provided".into());
        } else if re().windows.is_match(&name) {
            e.set("use", "system".into());
            e.set("note", "Windows-only: calls fail as on a Linux PC".into());
        } else if re().macos.is_match(&name) {
            e.set("use", "system".into());
            e.set("note", "macOS-only: calls fail as on a Linux PC".into());
        } else if !matched.is_empty() {
            e.set("use", "none".into());
            e.set(
                "note",
                "shipped as a desktop library only: DllNotFoundException at the first call unless an ARM build \
                 (Android, or Linux glibc) is put in lib/"
                    .into(),
            );
        } else {
            e.set("use", "none".into());
            e.set(
                "note",
                "not shipped with the game (a system library on the desktop?): DllNotFoundException at the first \
                 call unless the handheld has it"
                    .into(),
            );
        }
        out.set(&name, e);
    }
    let mut filerep = Vec::new();
    for (rel, path) in &files {
        let d = describe_file(path);
        let mut f = Json::obj();
        f.set("file", rel.as_str().into());
        f.set("format", d.format.as_str().into());
        for (k, v) in [
            ("arch", &d.arch),
            ("soname", &d.soname),
            ("version", &d.version),
        ] {
            if let Some(v) = v.as_ref().filter(|v| !v.is_empty()) {
                f.set(k, v.as_str().into());
            }
        }
        if used_files.contains(rel) {
            let mut by: Vec<String> = out
                .as_obj()
                .unwrap()
                .iter()
                .filter(|(_, e)| {
                    e.get("desktop_files")
                        .and_then(Json::as_list)
                        .is_some_and(|l| l.iter().any(|x| x.as_str() == Some(rel)))
                })
                .map(|(n, _)| n.clone())
                .collect();
            by.sort();
            f.set("used_by", by.into());
        } else if d.format != "managed" {
            let base = pkgzip::py::basename(rel);
            let note = re()
                .file_notes
                .iter()
                .find(|(rx, _)| rx.is_match(base))
                .map_or(
                "no P/Invoke of the managed code names it (loaded by another plugin, or unused)",
                |x| x.1,
            );
            f.set("note", note.into());
        }
        filerep.push(f);
    }
    let mut sigs = Vec::new();
    let mut by_name: Vec<&(String, Mod)> = mods.iter().collect();
    by_name.sort_by(|a, b| a.0.cmp(&b.0));
    for (name, m) in by_name {
        let mut ents: Vec<&(String, Option<Abi>)> = m.entries.iter().collect();
        ents.sort_by(|a, b| a.0.cmp(&b.0));
        for (entry, abi) in ents {
            if let Some(abi) = abi {
                if needs_thunk(abi) && abi.0 != "?" && !abi.1.iter().any(|x| x == "?") {
                    sigs.push((name.clone(), entry.clone(), abi.0.clone(), abi.1.clone()));
                }
            }
        }
    }
    let fmod = fmod_info(&consts, &pins, &files);
    let map = default_map(&out);
    Ok(Survey {
        modules: out,
        files: Json::List(filerep),
        fmod,
        sigs,
        unreadable: bad,
        map,
    })
}

fn default_map(modules: &Json) -> Json {
    let mut m = Json::obj();
    for (name, e) in modules.as_obj().unwrap() {
        let u = e.get("use").and_then(Json::as_str).unwrap_or("");
        if u == "system" || u == "keep" {
            continue;
        }
        let mut ent = Json::obj();
        ent.set("use", u.into());
        match u {
            "stub" => ent.set("stub", e.get("stub").cloned().unwrap_or(Json::Null)),
            "host" => {
                let lib = e.get("lib").cloned().unwrap_or(Json::Null);
                if !lib.truthy() {
                    continue;
                }
                ent.set("lib", lib);
            }
            "lib" => ent.set("file", format!("lib{}.so", norm(name)).into()),
            _ => {}
        }
        m.set(name, ent);
    }
    m
}

/// The port's plugin map (a JSON object, or a port config with a "plugins" object) over the
/// defaults; Err (the Python's ValueError) for an entry without a valid "use".
pub fn merge_map(rep: &Survey, port_map: &Json) -> Result<Json, String> {
    let mut m = rep.map.clone();
    let pm = match port_map {
        Json::Obj(_) => port_map.get("plugins").unwrap_or(port_map).clone(),
        _ => Json::obj(),
    };
    // (a "plugins" value that is no object: its .items() fails as the Python's does)
    let items = match &pm {
        Json::Obj(o) => o.clone(),
        other => {
            return Err(format!(
                "'{}' object has no attribute 'items'",
                py_type(other)
            ))
        }
    };
    for (name, ent) in items {
        let ent = match ent {
            Json::Str(s) => {
                let mut o = Json::obj();
                o.set("use", Json::Str(s));
                o
            }
            e => e,
        };
        let ok = matches!(&ent, Json::Obj(_))
            && matches!(
                ent.get("use").and_then(Json::as_str),
                Some("lib" | "stub" | "host" | "none")
            );
        if !ok {
            return Err(format!(
                "plugin map: {name}: \"use\" must be lib, stub, host or none"
            ));
        }
        m.set(&name, ent);
    }
    Ok(m)
}

fn py_type(j: &Json) -> &'static str {
    match j {
        Json::Null => "NoneType",
        Json::Bool(_) => "bool",
        Json::Int(_) => "int",
        Json::Float(_) => "float",
        Json::Str(_) => "str",
        Json::List(_) => "list",
        Json::Obj(_) => "dict",
    }
}

const STUBS: &[&str] = &[
    "steam",
    "csteamworks",
    "discord_game_sdk",
    "discord-rpc",
    "eos",
];

/// plugins.map for ua_host (runtime/host/plugins.c); Err (ValueError) for an unknown stub.
pub fn map_text(rep: &Survey, pmap: &Json) -> Result<String, String> {
    let mut lines: Vec<String> = vec![
        "# plugins.map (pmunity android-data; docs/plugins.md): what the runtime gives each DllImport name".into(),
        "#   map <name> lib <file in lib/> | stub <id> | host <system library> | none".into(),
        "#   sig <name> <entry> <result> <args>: argument classes for the softfp thunks of glibc libraries".into(),
    ];
    let items = pmap.as_obj().cloned().unwrap_or_default();
    let mut sorted: Vec<&(String, Json)> = items.iter().collect();
    sorted.sort_by_key(|(n, _)| n.to_lowercase());
    for (name, e) in sorted {
        let u = e.get("use").map(Json::py_str).unwrap_or_default();
        if re().space.is_match(name) {
            continue;
        }
        if u == "lib" {
            let file = e
                .get("file")
                .filter(|f| f.truthy())
                .map(Json::py_str)
                .unwrap_or_else(|| format!("lib{}.so", norm(name)));
            lines.push(format!("map {name} lib {file}"));
        } else if u == "stub" {
            let st = e
                .get("stub")
                .filter(|s| s.truthy())
                .cloned()
                .unwrap_or_else(|| {
                    sdk_of(name)
                        .and_then(|s| s.4)
                        .map_or(Json::Null, Json::from)
                });
            let ok = st.as_str().is_some_and(|s| STUBS.contains(&s));
            if !ok {
                return Err(format!(
                    "plugin map: {name}: no stub {} (stubs: {})",
                    st.py_repr(),
                    STUBS.join(", ")
                ));
            }
            lines.push(format!("map {name} stub {}", st.py_str()));
        } else if u == "host" {
            let lib = e.get("lib").ok_or_else(|| "KeyError: 'lib'".to_string())?;
            lines.push(format!("map {name} host {}", lib.py_str()));
        } else {
            lines.push(format!("map {name} none"));
        }
    }
    let libs: BTreeSet<&str> = items
        .iter()
        .filter(|(_, e)| matches!(e.get("use").and_then(Json::as_str), Some("lib" | "host")))
        .map(|(n, _)| n.as_str())
        .collect();
    for (name, entry, ret, params) in &rep.sigs {
        if re().space.is_match(&format!("{name}{entry}")) || !libs.contains(name.as_str()) {
            continue;
        }
        let ps = if params.is_empty() {
            "-".to_string()
        } else {
            params.join(",")
        };
        lines.push(format!("sig {name} {entry} {ret} {ps}"));
    }
    Ok(lines.join("\n") + "\n")
}

/// Short lines for the converter's log and warnings.
pub fn summary(rep: &Survey) -> Vec<String> {
    let mut out = Vec::new();
    for (name, e) in rep.modules.as_obj().unwrap() {
        let u = e.get("use").and_then(Json::as_str).unwrap_or("");
        if u == "system" || u == "keep" {
            continue;
        }
        let what = e
            .get("title")
            .filter(|t| t.truthy())
            .map(Json::py_str)
            .unwrap_or_else(|| "unknown library".into());
        let stub = e
            .get("stub")
            .filter(|s| s.truthy())
            .map(|s| format!(" {}", s.py_str()))
            .unwrap_or_default();
        out.push(format!(
            "{name} ({what}, {} entry points): {u}{stub} - {}",
            e.get("entries").map(Json::py_str).unwrap_or_default(),
            e.get("note").map(Json::py_str).unwrap_or_default()
        ));
    }
    if let Some(f) = &rep.fmod {
        let s = f.get("supply").unwrap();
        let a = s.get("android").unwrap();
        let files: Vec<String> = a
            .get("files")
            .unwrap()
            .as_list()
            .unwrap()
            .iter()
            .map(Json::py_str)
            .collect();
        let mut line = format!(
            "FMOD {}: put {} of FMOD Engine {} for Android ({}) in {}",
            f.get("header_version").unwrap().py_str(),
            files.join(", "),
            s.get("version").unwrap().py_str(),
            a.get("abi").unwrap().py_str(),
            a.get("to").unwrap().py_str()
        );
        if let Some(w) = f.get("warning").filter(|w| w.truthy()) {
            line.push_str(&format!("; {}", w.py_str()));
        }
        out.push(line.replace(
            " in lib/ (beside libunity.so)",
            " in the port's lib/, beside libunity.so",
        ));
    }
    out
}

/// The report's "plugins": the survey without its signatures and map, then the map in use and
/// the number of float signatures.
pub fn report(rep: &Survey, map: &Json) -> Json {
    let mut j = Json::obj();
    j.set("modules", rep.modules.clone());
    j.set("files", rep.files.clone());
    j.set("fmod", rep.fmod.clone().unwrap_or(Json::Null));
    if !rep.unreadable.is_empty() {
        j.set("unreadable_assemblies", rep.unreadable.clone().into());
    }
    j.set("map", map.clone());
    j.set("float_signatures", rep.sigs.len().into());
    j
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names() {
        assert_eq!(norm("libfmodstudio.so.13"), "fmodstudio");
        assert_eq!(norm("C:\\x\\steam_api64.dll"), "steam_api64");
        assert_eq!(norm("lib"), "lib");
        assert_eq!(sdk_of("libfmodstudioL.so").unwrap().0, "fmod");
        assert_eq!(sdk_of("GalaxyCSharpGlue").unwrap().0, "galaxy");
        assert!(sdk_of("foo").is_none());
        assert!(re().system.is_match("__Internal"));
        assert!(re().windows.is_match("user32.dll"));
        assert!(re().windows.is_match("anything.DLL"));
        assert!(re().framework.is_match("UnityEngine.CoreModule.dll"));
        assert!(!re().framework.is_match("Assembly-CSharp.dll"));
    }

    #[test]
    fn map_and_text() {
        let mut modules = Json::obj();
        let mut e = Json::obj();
        e.set("use", "stub".into());
        e.set("stub", "steam".into());
        modules.set("steam_api", e);
        let mut e = Json::obj();
        e.set("use", "system".into());
        modules.set("libc", e);
        let rep = Survey {
            map: default_map(&modules),
            modules,
            files: Json::List(vec![]),
            fmod: None,
            sigs: vec![(
                "SDL2".into(),
                "SDL_SetGamma".into(),
                "i".into(),
                vec!["f".into(), "f".into()],
            )],
            unreadable: vec![],
        };
        let pm = crate::pyjson::loads(
            r#"{"plugins": {"SDL2": {"use": "host", "lib": "libSDL2-2.0.so.0"}, "x y": "none"}}"#,
        )
        .unwrap();
        let m = merge_map(&rep, &pm).unwrap();
        let t = map_text(&rep, &m).unwrap();
        assert!(t.ends_with(
            "map SDL2 host libSDL2-2.0.so.0\nmap steam_api stub steam\nsig SDL2 SDL_SetGamma i f,f\n"
        ));
        assert!(merge_map(
            &rep,
            &crate::pyjson::loads(r#"{"a": {"use": "x"}}"#).unwrap()
        )
        .is_err());
    }
}
