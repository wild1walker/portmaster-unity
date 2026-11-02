//! `pmu route <path> --env` (route.py with --env): the game.env the box64 port's setup writes for
//! its launcher, from the first build under <path> with a Linux x86-64 executable: the game's
//! folder (relative to <path>), the executable, the data folder, the Unity version, the scripting
//! backend and the GL mode. Only what --env prints is computed (route.py's examine() has more: the
//! report, --json and --launcher stop with status 2, for the Python). Everything is read from file
//! headers and the player binary's strings; nothing of the game runs.
use std::fs::File;
use std::io::Read;
use std::path::{Component, Path, PathBuf};
use std::process::ExitCode;

const DATA_MARKERS: [&str; 3] = ["globalgamemanagers", "mainData", "data.unity3d"];

pub fn run(args: &[String]) -> ExitCode {
    let mut path: Option<&str> = None;
    let mut env = false;
    let mut other = false;
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        match a {
            "--env" => env = true,
            "--json" => other = true,
            "--launcher" | "--port" | "--title" | "--template" => {
                other = true;
                i += 1;
            }
            _ if a.starts_with("--launcher=")
                || a.starts_with("--port=")
                || a.starts_with("--title=")
                || a.starts_with("--template=") =>
            {
                other = true
            }
            _ if a.starts_with('-') && a.len() > 1 => {
                eprintln!("usage: pmunity route [-h] [--json] [--env] [--launcher OUT] [--port PORT] [--title TITLE] [--template TEMPLATE] path");
                eprintln!("pmunity route: error: unrecognized arguments: {a}");
                return ExitCode::from(2);
            }
            _ if path.is_none() => path = Some(a),
            _ => {
                eprintln!("usage: pmunity route [-h] [--json] [--env] [--launcher OUT] [--port PORT] [--title TITLE] [--template TEMPLATE] path");
                eprintln!("pmunity route: error: unrecognized arguments: {a}");
                return ExitCode::from(2);
            }
        }
        i += 1;
    }
    let Some(path) = path else {
        eprintln!("usage: pmunity route [-h] [--json] [--env] [--launcher OUT] [--port PORT] [--title TITLE] [--template TEMPLATE] path");
        eprintln!("pmunity route: error: the following arguments are required: path");
        return ExitCode::from(2);
    };
    // (with --env the Python prints game.env and nothing else)
    if !env {
        let _ = other;
        eprintln!("pmu: route: only --env is ported (run `python -m pmunity route`)");
        return ExitCode::from(2);
    }
    let dirs = find_data_dirs(path);
    if dirs.is_empty() {
        eprintln!("{path}: no Unity data folder (<Game>_Data with globalgamemanagers, mainData or data.unity3d)");
        return ExitCode::from(1);
    }
    let base = if Path::new(path).is_dir() {
        abspath(path)
    } else {
        abspath(path)
            .parent()
            .map_or_else(|| PathBuf::from("/"), Path::to_path_buf)
    };
    for d in &dirs {
        let f = examine(d);
        if let Some(text) = game_env(&f, &base) {
            print!("{text}");
            return ExitCode::SUCCESS;
        }
    }
    eprintln!("{path}: no Linux x86-64 build here");
    ExitCode::from(1)
}

/// os.path.abspath: absolute and normalized (no `.`, `..` or doubled separators), symlinks kept.
fn abspath(p: &str) -> PathBuf {
    let joined = if Path::new(p).is_absolute() {
        PathBuf::from(p)
    } else {
        std::env::current_dir().unwrap_or_default().join(p)
    };
    let mut out = PathBuf::from("/");
    for c in joined.components() {
        match c {
            Component::Normal(x) => out.push(x),
            Component::ParentDir => {
                out.pop();
            }
            _ => {}
        }
    }
    out
}

/// os.path.relpath of two absolute, normalized paths.
fn relpath(p: &Path, base: &Path) -> String {
    let a: Vec<_> = p.components().collect();
    let b: Vec<_> = base.components().collect();
    let n = a.iter().zip(&b).take_while(|(x, y)| x == y).count();
    let mut parts: Vec<String> = vec!["..".into(); b.len() - n];
    parts.extend(
        a[n..]
            .iter()
            .map(|c| c.as_os_str().to_string_lossy().into_owned()),
    );
    if parts.is_empty() {
        ".".into()
    } else {
        parts.join("/")
    }
}

fn is_data(d: &Path) -> bool {
    DATA_MARKERS.iter().any(|n| d.join(n).is_file())
}

/// route.find_data_dirs: data folders (<Game>_Data, <Game>.app/Contents/Resources/Data) at or
/// below path (os.walk's order: each folder's entries as the file system lists them, the found
/// ones sorted within a folder, four levels down).
fn find_data_dirs(path: &str) -> Vec<PathBuf> {
    let mut path = abspath(path);
    if path.is_file() {
        path = path
            .parent()
            .map_or_else(|| PathBuf::from("/"), Path::to_path_buf);
    }
    if is_data(&path) {
        return vec![path];
    }
    let app = path.join("Contents").join("Resources").join("Data");
    if is_data(&app) {
        return vec![app];
    }
    let mut found = Vec::new();
    walk(&path, 0, &mut found);
    found
}

fn walk(root: &Path, depth: usize, found: &mut Vec<PathBuf>) {
    if depth >= 4 {
        return;
    }
    // (os.walk: the folder's subfolders, symbolic links to folders included, in listing order)
    let Ok(rd) = std::fs::read_dir(root) else {
        return;
    };
    let mut dirs: Vec<(String, bool)> = Vec::new();
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            let link = e.file_type().map(|t| t.is_symlink()).unwrap_or(false);
            dirs.push((e.file_name().to_string_lossy().into_owned(), link));
        }
    }
    let mut sorted: Vec<&String> = dirs.iter().map(|(n, _)| n).collect();
    sorted.sort();
    for d in sorted {
        let p = root.join(d);
        if d.ends_with("_Data") && is_data(&p) {
            found.push(p);
        } else if d.ends_with(".app") {
            let q = p.join("Contents").join("Resources").join("Data");
            if is_data(&q) {
                found.push(q);
            }
        }
    }
    for (d, link) in &dirs {
        if d.ends_with("_Data") || d.ends_with(".app") || *link {
            continue;
        }
        walk(&root.join(d), depth + 1, found);
    }
}

fn read_head(p: &Path, n: usize) -> Vec<u8> {
    let mut out = Vec::new();
    if let Ok(f) = File::open(p) {
        let _ = f.take(n as u64).read_to_end(&mut out);
    }
    out
}

fn h16(h: &[u8], o: usize, le: bool) -> Option<u16> {
    let b: [u8; 2] = h.get(o..o + 2)?.try_into().ok()?;
    Some(if le {
        u16::from_le_bytes(b)
    } else {
        u16::from_be_bytes(b)
    })
}

fn h32(h: &[u8], o: usize, le: bool) -> Option<u32> {
    let b: [u8; 4] = h.get(o..o + 4)?.try_into().ok()?;
    Some(if le {
        u32::from_le_bytes(b)
    } else {
        u32::from_be_bytes(b)
    })
}

/// route.binary_kind: (format, arch) of an executable or library.
fn binary_kind(p: &Path) -> Option<(&'static str, String)> {
    let h = read_head(p, 4096);
    if h.starts_with(b"\x7fELF") && h.len() >= 20 {
        let m = h16(&h, 18, h[5] == 1)?;
        let arch = match m {
            3 => "x86",
            62 => "x86_64",
            40 => "armv7",
            183 => "arm64",
            _ => "other",
        };
        return Some(("elf", arch.into()));
    }
    if h.starts_with(b"MZ") && h.len() >= 0x40 {
        let pe = h32(&h, 0x3c, true)? as usize;
        let h = read_head(p, pe + 6);
        if h.get(pe..pe + 4) == Some(b"PE\0\0") {
            let arch = match h16(&h, pe + 4, true)? {
                0x14c => "x86",
                0x8664 => "x86_64",
                0xaa64 => "arm64",
                0x1c4 => "armv7",
                _ => "other",
            };
            return Some(("pe", arch.into()));
        }
        return None;
    }
    let cpu = |v: i32| match v {
        7 => "x86",
        0x0100_0007 => "x86_64",
        0x0100_000c => "arm64",
        12 => "armv7",
        _ => "other",
    };
    let magic = h.get(..4)?;
    if magic == b"\xcf\xfa\xed\xfe" || magic == b"\xce\xfa\xed\xfe" {
        return Some(("macho", cpu(h32(&h, 4, true)? as i32).into()));
    }
    if magic == b"\xca\xfe\xba\xbe" && h.len() >= 8 {
        let n = h32(&h, 4, false)? as usize;
        if n < 16 {
            let mut cpus = Vec::new();
            for i in 0..n {
                cpus.push(cpu(h32(&h, 8 + 20 * i, false)? as i32));
            }
            return Some(("macho", cpus.join("+")));
        }
    }
    None
}

/// Whether an ELF file names an interpreter (route.elf_info()['interp']: PT_INTERP, its string
/// up to the first NUL; nothing when the file does not parse, as elf_info's {}).
fn elf_interp(p: &Path) -> bool {
    let Ok(f) = File::open(p) else {
        return false;
    };
    let len = f.metadata().map(|m| m.len() as usize).unwrap_or(0);
    elf_parse(&Elf { f, len }).unwrap_or(false)
}

/// An ELF file read where it is needed (the Python maps it; a player binary is tens of MB).
struct Elf {
    f: File,
    len: usize,
}

impl Elf {
    fn get(&self, o: usize, n: usize) -> Option<Vec<u8>> {
        use std::os::unix::fs::FileExt;
        if o.checked_add(n)? > self.len {
            return None;
        }
        let mut b = vec![0u8; n];
        self.f.read_exact_at(&mut b, o as u64).ok()?;
        Some(b)
    }

    /// m[a:b] clipped to the file, as a slice of the mapping is.
    fn slice(&self, a: usize, b: usize) -> Vec<u8> {
        let (a, b) = (a.min(self.len), b.min(self.len));
        if a >= b {
            return Vec::new();
        }
        self.get(a, b - a).unwrap_or_default()
    }
}

fn u16_at(m: &Elf, o: usize, le: bool) -> Option<u16> {
    let b: [u8; 2] = m.get(o, 2)?.try_into().ok()?;
    Some(if le {
        u16::from_le_bytes(b)
    } else {
        u16::from_be_bytes(b)
    })
}

fn u32_at(m: &Elf, o: usize, le: bool) -> Option<u32> {
    let b: [u8; 4] = m.get(o, 4)?.try_into().ok()?;
    Some(if le {
        u32::from_le_bytes(b)
    } else {
        u32::from_be_bytes(b)
    })
}

fn u64_at(m: &Elf, o: usize, le: bool) -> Option<u64> {
    let b: [u8; 8] = m.get(o, 8)?.try_into().ok()?;
    Some(if le {
        u64::from_le_bytes(b)
    } else {
        u64::from_be_bytes(b)
    })
}

fn elf_parse(m: &Elf) -> Option<bool> {
    let id = m.slice(0, 6);
    if !id.starts_with(b"\x7fELF") {
        return Some(false);
    }
    let is64 = *id.get(4)? == 2;
    let le = *id.get(5)? == 1;
    let (phoff, shoff, base) = if is64 {
        (u64_at(m, 32, le)? as usize, u64_at(m, 40, le)? as usize, 54)
    } else {
        (u32_at(m, 28, le)? as usize, u32_at(m, 32, le)? as usize, 42)
    };
    u16_at(m, 16, le)?;
    u16_at(m, 18, le)?;
    let phentsize = u16_at(m, base, le)? as usize;
    let phnum = u16_at(m, base + 2, le)? as usize;
    let shentsize = u16_at(m, base + 4, le)? as usize;
    let shnum = u16_at(m, base + 6, le)? as usize;
    let mut interp = false;
    for i in 0..phnum {
        let o = phoff + i * phentsize;
        if u32_at(m, o, le)? == 3 {
            let (off, size) = if is64 {
                (
                    u64_at(m, o + 8, le)? as usize,
                    u64_at(m, o + 32, le)? as usize,
                )
            } else {
                (
                    u32_at(m, o + 4, le)? as usize,
                    u32_at(m, o + 16, le)? as usize,
                )
            };
            let s = m.slice(off, off.saturating_add(size));
            interp = s.split(|&c| c == 0).next().is_some_and(|x| !x.is_empty());
        }
    }
    // (the section headers, the dynamic section and the version needs parse, as elf_info's do, or
    // the file gives nothing)
    let mut secs = Vec::new();
    for i in 0..shnum {
        let o = shoff + i * shentsize;
        if is64 {
            secs.push((
                u32_at(m, o + 4, le)?,
                u64_at(m, o + 24, le)? as usize,
                u64_at(m, o + 32, le)? as usize,
                u32_at(m, o + 40, le)? as usize,
            ));
        } else {
            secs.push((
                u32_at(m, o + 4, le)?,
                u32_at(m, o + 16, le)? as usize,
                u32_at(m, o + 20, le)? as usize,
                u32_at(m, o + 24, le)? as usize,
            ));
        }
    }
    for &(ty, off, size, link) in &secs {
        if ty == 6 {
            let ent = if is64 { 16 } else { 8 };
            for j in 0..size / ent {
                let o = off + j * ent;
                let tag = if is64 {
                    u64_at(m, o, le)? as i64
                } else {
                    u32_at(m, o, le)? as i32 as i64
                };
                if is64 {
                    u64_at(m, o + 8, le)?;
                } else {
                    u32_at(m, o + 4, le)?;
                }
                if tag == 0 {
                    break;
                }
                if tag == 1 || tag == 14 {
                    secs.get(link)?;
                }
            }
        } else if ty == 0x6fff_fffe {
            let mut p = off;
            let mut guard = 0;
            loop {
                let cnt = u16_at(m, p + 2, le)? as usize;
                u32_at(m, p + 4, le)?;
                let aux = u32_at(m, p + 8, le)? as usize;
                let next = u32_at(m, p + 12, le)? as usize;
                let mut a = p + aux;
                for _ in 0..cnt {
                    u32_at(m, a + 8, le)?;
                    secs.get(link)?;
                    let na = u32_at(m, a + 12, le)? as usize;
                    if na == 0 {
                        break;
                    }
                    a += na;
                }
                guard += 1;
                if next == 0 || guard > 1 << 20 {
                    break;
                }
                p += next;
            }
        }
    }
    Some(interp)
}

/// route.player_strings' -force-* switches and its GLX ES string: (flags, glx_es), read in
/// pieces (the Python maps the file).
fn player_strings(p: &Path) -> (Vec<String>, bool) {
    const K: usize = 256;
    let mut flags = std::collections::BTreeSet::new();
    let mut glx_es = false;
    let Ok(mut f) = File::open(p) else {
        return (Vec::new(), false);
    };
    let mut buf: Vec<u8> = Vec::new();
    let mut chunk = vec![0u8; 1 << 20];
    loop {
        let n = match f.read(&mut chunk) {
            Ok(n) => n,
            Err(_) => return (Vec::new(), false),
        };
        let eof = n == 0;
        buf.extend_from_slice(&chunk[..n]);
        // positions before `upto` are looked at now; the rest (an unfinished string) waits
        let upto = if eof {
            buf.len()
        } else {
            buf.len().saturating_sub(K)
        };
        let mut i = 0;
        while let Some(j) = find(&buf[i..], b"force-") {
            let at = i + j;
            if at >= upto {
                break;
            }
            let rest = &buf[at + 6..];
            if let Some(z) = rest.iter().position(|&c| c == 0) {
                if let Some(name) = flag_name(&rest[..z]) {
                    flags.insert(name);
                }
            }
            i = at + 1;
        }
        if !glx_es {
            let g = b"GLX_EXT_create_context_es2_profile\0";
            glx_es = find(&buf, g).is_some();
        }
        if eof {
            break;
        }
        buf.drain(..upto);
    }
    (flags.into_iter().collect(), glx_es)
}

fn find(h: &[u8], n: &[u8]) -> Option<usize> {
    h.windows(n.len()).position(|w| w == n)
}

/// `force-(glcore[0-9]*|vulkan|opengl|gles[0-9a-z]*|wayland|gfx-direct)\0`: the name, when the
/// bytes between the prefix and the NUL are one.
fn flag_name(b: &[u8]) -> Option<String> {
    let s = std::str::from_utf8(b).ok()?;
    let ok = match s {
        "vulkan" | "opengl" | "wayland" | "gfx-direct" => true,
        _ => {
            s.strip_prefix("glcore")
                .is_some_and(|r| r.bytes().all(|c| c.is_ascii_digit()))
                || s.strip_prefix("gles").is_some_and(|r| {
                    r.bytes()
                        .all(|c| c.is_ascii_digit() || c.is_ascii_lowercase())
                })
        }
    };
    ok.then(|| s.to_string())
}

/// What --env needs of route.examine(): the data folder, its executables (file relative to the
/// game's folder, format, arch), the Unity version, the backend, the player's renderers.
struct Facts {
    data: PathBuf,
    exes: Vec<(String, &'static str, String)>,
    unity: Option<String>,
    backend: Option<&'static str>,
    renderers: Vec<&'static str>,
}

fn first(paths: &[PathBuf]) -> Option<PathBuf> {
    paths.iter().find(|p| p.exists()).cloned()
}

fn lenient_version(data: &Path) -> Option<convert::version::Version> {
    let head = ["globalgamemanagers", "mainData"]
        .iter()
        .map(|n| data.join(n))
        .find(|p| p.is_file())
        .map(|p| read_head(&p, 512))
        .unwrap_or_default();
    if head.len() < 64 {
        return None;
    }
    let fmt = u32::from_be_bytes(head[8..12].try_into().unwrap());
    if fmt < 22 {
        return None;
    }
    let end = head[48..].iter().position(|&c| c == 0).map(|e| 48 + e)?;
    if end <= 48 {
        return None;
    }
    let s = std::str::from_utf8(&head[48..end])
        .ok()
        .filter(|s| s.is_ascii())?;
    convert::version::Version::parse(s).ok()
}

fn examine(data: &Path) -> Facts {
    // (layout)
    let mac = data.to_string_lossy().ends_with("Contents/Resources/Data");
    let (game_dir, exes, player_lib, game_assembly) = if mac {
        let contents = data.parent().unwrap().parent().unwrap();
        let game_dir = contents.parent().unwrap_or(contents).to_path_buf();
        let macos = contents.join("MacOS");
        let mut exes = Vec::new();
        if macos.is_dir() {
            let mut names: Vec<String> = std::fs::read_dir(&macos)
                .map(|rd| {
                    rd.flatten()
                        .map(|e| e.file_name().to_string_lossy().into_owned())
                        .collect()
                })
                .unwrap_or_default();
            names.sort();
            for n in names {
                let p = macos.join(&n);
                if binary_kind(&p).is_some() {
                    exes.push(p);
                }
            }
        }
        let fw = contents.join("Frameworks");
        (
            game_dir,
            exes,
            first(&[fw.join("UnityPlayer.dylib")]),
            first(&[fw.join("GameAssembly.dylib")]),
        )
    } else {
        let gd = data.parent().unwrap_or(data).to_path_buf();
        let base = data
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        let name = base.strip_suffix("_Data").unwrap_or(&base).to_string();
        let mut names: Vec<String> = std::fs::read_dir(&gd)
            .map(|rd| {
                rd.flatten()
                    .map(|e| e.file_name().to_string_lossy().into_owned())
                    .collect()
            })
            .unwrap_or_default();
        names.sort();
        let mut exes = Vec::new();
        for n in names {
            let p = gd.join(&n);
            if !p.is_file()
                || !(n == name || n.starts_with(&format!("{name}.")))
                || n.ends_with(".debug")
            {
                continue;
            }
            match binary_kind(&p) {
                Some(("elf", _)) if elf_interp(&p) => exes.push(p),
                Some(("pe", _)) if n.to_lowercase().ends_with(".exe") => exes.push(p),
                _ => {}
            }
        }
        (
            gd.clone(),
            exes,
            first(&[gd.join("UnityPlayer.so"), gd.join("UnityPlayer.dll")]),
            first(&[gd.join("GameAssembly.so"), gd.join("GameAssembly.dll")]),
        )
    };
    let v = convert::version::of_data(data)
        .ok()
        .or_else(|| lenient_version(data));
    let il2cpp = game_assembly.is_some() || data.join("il2cpp_data").is_dir();
    let mono = data.join("Managed").is_dir() && !il2cpp;
    let backend = if il2cpp {
        Some("il2cpp")
    } else if mono {
        Some("mono")
    } else {
        None
    };
    let mut ex = Vec::new();
    for p in &exes {
        let (fmt, arch) = binary_kind(p).unwrap_or(("?", "?".into()));
        let rel = relpath(p, &game_dir);
        ex.push((rel, fmt, arch));
    }
    // the player: UnityPlayer.so, else the first ELF executable (x86_64 first)
    let mut elf: Vec<&(String, &'static str, String)> =
        ex.iter().filter(|e| e.1 == "elf").collect();
    elf.sort_by_key(|e| e.2 != "x86_64");
    let player = match &player_lib {
        Some(p) if p.to_string_lossy().ends_with(".so") => Some(p.clone()),
        _ => elf.first().map(|e| game_dir.join(&e.0)),
    };
    let line = v.as_ref().map(|v| v.line);
    let mut renderers = Vec::new();
    if let Some(p) = player {
        let (flags, glx_es) = player_strings(&p);
        if flags.iter().any(|f| f.starts_with("glcore")) {
            renderers.push("glcore");
        }
        if flags.iter().any(|f| f == "vulkan") {
            renderers.push("vulkan");
        }
        if flags.iter().any(|f| f == "opengl") && line.is_none_or(|l| l < (5, 5)) {
            renderers.push("opengl2");
        }
        if renderers.contains(&"glcore") && glx_es && line.is_none_or(|l| l >= (2017, 1)) {
            renderers.push("gles");
        }
    }
    Facts {
        data: data.to_path_buf(),
        exes: ex,
        unity: v.map(|v| v.text),
        backend,
        renderers,
    }
}

/// route.gl_mode.
fn gl_mode(f: &Facts) -> &'static str {
    let (lo, hi) = (convert::version::FIRST, convert::version::LAST);
    let line = f
        .unity
        .as_deref()
        .and_then(|t| convert::version::Version::parse(t).ok())
        .map(|v| v.line);
    if f.renderers.contains(&"gles") && line.is_some_and(|l| lo <= l && l <= hi) {
        "gles"
    } else if f.renderers.contains(&"opengl2") {
        "gl4es"
    } else {
        "mesa"
    }
}

/// shlex.quote.
fn quote(s: &str) -> String {
    if s.is_empty() {
        return "''".into();
    }
    let safe = s
        .chars()
        .all(|c| c.is_ascii_alphanumeric() || "_@%+=:,./-".contains(c));
    if safe {
        s.to_string()
    } else {
        format!("'{}'", s.replace('\'', "'\"'\"'"))
    }
}

/// route.game_env, or None when the build has no Linux x86-64 executable.
fn game_env(f: &Facts, base: &Path) -> Option<String> {
    let x64 = f.exes.iter().find(|e| e.1 == "elf" && e.2 == "x86_64")?;
    let game_dir = relpath(f.data.parent().unwrap_or(&f.data), base);
    let exe = Path::new(&x64.0)
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let data = f
        .data
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let vals = [
        ("GAME_DIR", game_dir),
        ("EXE", exe),
        ("DATA", data),
        ("UNITY", f.unity.clone().unwrap_or_default()),
        ("BACKEND", f.backend.unwrap_or("").to_string()),
        ("GL_MODE", gl_mode(f).to_string()),
    ];
    Some(
        vals.iter()
            .map(|(k, v)| format!("{k}={}\n", quote(v)))
            .collect(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A minimal x86-64 ELF executable: PT_INTERP (or none), then `strings`.
    fn elf(interp: bool, strings: &[u8]) -> Vec<u8> {
        let body: &[u8] = if interp {
            b"/lib64/ld-linux-x86-64.so.2\0"
        } else {
            b""
        };
        let nph = interp as u16;
        let data_off = 64 + 56 * nph as u64;
        let mut h = b"\x7fELF\x02\x01\x01".to_vec();
        h.resize(16, 0);
        h.extend(2u16.to_le_bytes());
        h.extend(62u16.to_le_bytes());
        h.extend(1u32.to_le_bytes());
        h.extend(0u64.to_le_bytes());
        h.extend((if interp { 64u64 } else { 0 }).to_le_bytes());
        h.extend(0u64.to_le_bytes());
        h.extend(0u32.to_le_bytes());
        for v in [64u16, 56, nph, 64, 0, 0] {
            h.extend(v.to_le_bytes());
        }
        if interp {
            h.extend(3u32.to_le_bytes());
            h.extend(4u32.to_le_bytes());
            for v in [data_off, 0, 0, body.len() as u64, body.len() as u64, 1] {
                h.extend(v.to_le_bytes());
            }
        }
        h.extend(body);
        h.extend(strings);
        h
    }

    fn ggm(version: &str) -> Vec<u8> {
        let mut h = vec![0u8; 64];
        h[8..12].copy_from_slice(&17u32.to_be_bytes());
        h[20..20 + version.len()].copy_from_slice(version.as_bytes());
        h.extend(24i32.to_le_bytes());
        h
    }

    #[test]
    fn env_of_made_up_builds() {
        let t = std::env::temp_dir().join(format!("pmu-route-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        let case = |name: &str, version: &str, exes: &[(&str, Vec<u8>)]| {
            let gd = t.join(name);
            std::fs::create_dir_all(gd.join("G_Data/Managed")).unwrap();
            std::fs::write(gd.join("G_Data/globalgamemanagers"), ggm(version)).unwrap();
            for (n, b) in exes {
                std::fs::write(gd.join(n), b).unwrap();
            }
            let f = examine(&gd.join("G_Data"));
            game_env(&f, &abspath(gd.to_str().unwrap()))
        };
        let gles = elf(
            true,
            b"force-glcore45\0GLX_EXT_create_context_es2_profile\0",
        );
        // (the outputs are the Python's for the same files)
        assert_eq!(
            case("gles", "2021.3.1f1", &[("G.x86_64", gles)]).unwrap(),
            "GAME_DIR=.\nEXE=G.x86_64\nDATA=G_Data\nUNITY=2021.3.1f1\nBACKEND=mono\nGL_MODE=gles\n"
        );
        let old = elf(true, b"xx-force-opengl\0yy-force-glcore\0");
        assert!(case("gl4es", "5.4.1f1", &[("G.x86_64", old)])
            .unwrap()
            .ends_with("GL_MODE=gl4es\n"));
        let core = elf(true, b"force-glcore\0force-vulkan\0");
        assert!(case("mesa", "2019.4.1f1", &[("G.x86_64", core)])
            .unwrap()
            .ends_with("GL_MODE=mesa\n"));
        // no PT_INTERP: not an executable
        assert_eq!(
            case("lib", "2021.3.1f1", &[("G.x86_64", elf(false, b""))]),
            None
        );
        let _ = std::fs::remove_dir_all(&t);
    }

    #[test]
    fn helpers() {
        assert_eq!(quote(""), "''");
        assert_eq!(quote("a b"), "'a b'");
        assert_eq!(quote("it's"), "'it'\"'\"'s'");
        assert_eq!(quote("6000.0.61f1"), "6000.0.61f1");
        assert_eq!(relpath(Path::new("/a/b/c"), Path::new("/a")), "b/c");
        assert_eq!(relpath(Path::new("/a"), Path::new("/a/b")), "..");
        assert_eq!(relpath(Path::new("/a"), Path::new("/a")), ".");
        assert_eq!(flag_name(b"gles30"), Some("gles30".into()));
        assert_eq!(flag_name(b"glcore4x"), None);
        assert_eq!(flag_name(b"opengles"), None);
    }
}
