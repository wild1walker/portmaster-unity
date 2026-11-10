//! --macho: a macOS IL2CPP build whose own arm64 code, its GameAssembly.dylib, runs on Unity's
//! arm64-v8a IL2CPP Android player of the same version (docs/macho-route.md: the runtime's bridge
//! is that player's libil2cpp.so). What android-data needs of such a build: where its code and
//! metadata are, the code's arm64 part written out for the port's lib folder, and the files the
//! player keeps under Managed/ (Metadata, Resources, etc), which a macOS build has in
//! il2cpp_data/. Nothing of the code is changed.
use crate::il2cppmeta::{self, Members};
use crate::paths;
use crate::pyjson::Json;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

/// Where the output keeps the game's code (as `pmunity il2cpp` keeps libil2cpp.so in lib/<abi>).
pub const LIB: &str = "lib/arm64-v8a/GameAssembly.dylib";
pub const ABI: &str = "arm64-v8a";

const MH_MAGIC_64: u32 = 0xFEED_FACF;
const FAT_MAGIC: u32 = 0xCAFE_BABE;
const FAT_MAGIC_64: u32 = 0xCAFE_BABF;
const CPU_ARM64: u32 = 0x0100_000C;
const CPU_X86_64: u32 = 0x0100_0007;
const SUBTYPE_MASK: u32 = 0x00FF_FFFF;
const SUBTYPE_ARM64E: u32 = 2;

/// A macOS IL2CPP build, from --macho's value.
#[derive(Debug)]
pub struct Build {
    /// its data folder (<Game>.app/Contents/Resources/Data)
    pub data: String,
    pub metadata: String,
    pub binary: String,
    /// the arm64 code in the binary: (offset, size); the whole file where it is not universal
    pub slice: (u64, u64),
    pub universal: bool,
    pub metadata_version: i32,
}

fn be32(b: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_be_bytes(b.get(o..o + 4)?.try_into().ok()?))
}

fn be64(b: &[u8], o: usize) -> Option<u64> {
    Some(u64::from_be_bytes(b.get(o..o + 8)?.try_into().ok()?))
}

fn cpu_name(cpu: u32, sub: u32) -> String {
    match (cpu, sub & SUBTYPE_MASK) {
        (CPU_ARM64, SUBTYPE_ARM64E) => "arm64e".into(),
        (CPU_ARM64, _) => "arm64".into(),
        (CPU_X86_64, _) => "x86_64".into(),
        _ => format!("cpu type {cpu:#x}"),
    }
}

/// The arm64 code of a Mach-O file: (offset, size, universal), or why it has none this route runs.
pub fn arm64_slice(path: &str) -> Result<(u64, u64, bool), String> {
    let mut f = File::open(path).map_err(|e| format!("{path}: {e}"))?;
    let len = f.metadata().map_err(|e| format!("{path}: {e}"))?.len();
    let mut head = Vec::new();
    Read::by_ref(&mut f)
        .take(4096)
        .read_to_end(&mut head)
        .map_err(|e| format!("{path}: {e}"))?;
    let no = |what: String| {
        format!(
            "{path}: {what}: the Mach-O route runs a macOS build's arm64 code as it is (an Intel-only build would need \
             its code translated, which this route does not do)"
        )
    };
    let magic = be32(&head, 0).ok_or_else(|| format!("{path}: no Mach-O file (too short)"))?;
    if magic.swap_bytes() == MH_MAGIC_64 {
        let (cpu, sub) = (
            be32(&head, 4).unwrap_or(0).swap_bytes(),
            be32(&head, 8).unwrap_or(0).swap_bytes(),
        );
        if cpu != CPU_ARM64 || sub & SUBTYPE_MASK == SUBTYPE_ARM64E {
            return Err(no(format!("{} code only", cpu_name(cpu, sub))));
        }
        return Ok((0, len, false));
    }
    if magic != FAT_MAGIC && magic != FAT_MAGIC_64 {
        return Err(format!("{path}: no Mach-O file (magic {magic:#010x})"));
    }
    let wide = magic == FAT_MAGIC_64;
    let n = be32(&head, 4).unwrap_or(0) as usize;
    let rec = if wide { 32 } else { 20 };
    let mut have = Vec::new();
    for i in 0..n {
        let o = 8 + i * rec;
        let bad = || format!("{path}: universal header cut short ({n} parts)");
        let cpu = be32(&head, o).ok_or_else(bad)?;
        let sub = be32(&head, o + 4).ok_or_else(bad)?;
        let (off, size) = if wide {
            (
                be64(&head, o + 8).ok_or_else(bad)?,
                be64(&head, o + 16).ok_or_else(bad)?,
            )
        } else {
            (
                be32(&head, o + 8).ok_or_else(bad)? as u64,
                be32(&head, o + 12).ok_or_else(bad)? as u64,
            )
        };
        if cpu == CPU_ARM64 && sub & SUBTYPE_MASK != SUBTYPE_ARM64E {
            if off.checked_add(size).is_none_or(|e| e > len) || size < 32 {
                return Err(format!(
                    "{path}: its arm64 part ({off:#x}, {size} bytes) is past the file's end ({len} bytes): a cut-off copy"
                ));
            }
            return Ok((off, size, true));
        }
        have.push(cpu_name(cpu, sub));
    }
    Err(no(format!("a universal file of {}", have.join(", "))))
}

/// --macho's value as the build's data folder: the app (<Game>.app), its Contents folder, or
/// that data folder itself.
pub fn data_of(app: &str) -> String {
    let app = app.trim_end_matches('/');
    for rel in ["Contents/Resources/Data", "Resources/Data"] {
        let d = format!("{app}/{rel}");
        if Path::new(&d).is_dir() {
            return d;
        }
    }
    app.to_string()
}

/// The game's name from its app's (<Game>.app/Contents/Resources/Data: "<Game>").
pub fn app_name(data: &str) -> Option<String> {
    let app = Path::new(data).parent()?.parent()?.parent()?;
    let n = app.file_name()?.to_str()?.strip_suffix(".app")?;
    (!n.is_empty()).then(|| n.to_string())
}

/// The build's code and metadata, or why --macho cannot take it.
pub fn find(app: &str) -> Result<Build, String> {
    let data = data_of(app);
    let src = data.as_str();
    let (metadata, binary) = match il2cppmeta::files_of(Path::new(src)) {
        Some((m, b)) => (
            m.to_string_lossy().into_owned(),
            b.to_string_lossy().into_owned(),
        ),
        None => {
            return Err(format!(
                "--macho: {app} is no macOS IL2CPP build (<Game>.app with Contents/Resources/Data/il2cpp_data/Metadata/\
                 global-metadata.dat and Contents/Frameworks/GameAssembly.dylib)"
            ))
        }
    };
    if !binary.ends_with(".dylib") {
        return Err(format!(
            "--macho: {binary}: not a macOS build's code (the route runs GameAssembly.dylib; a Windows or Linux IL2CPP \
             build's is x86-64)"
        ));
    }
    let (off, size, universal) = arm64_slice(&binary).map_err(|e| format!("--macho: {e}"))?;
    let mut head = [0u8; 8];
    File::open(&metadata)
        .and_then(|mut f| f.read_exact(&mut head))
        .map_err(|e| format!("--macho: {metadata}: {e}"))?;
    if u32::from_le_bytes(head[..4].try_into().unwrap()) != 0xFAB1_1BAF {
        return Err(format!(
            "--macho: {metadata}: not IL2CPP metadata (no 0xFAB11BAF: an encrypted or packed build, which this route \
             leaves alone)"
        ));
    }
    Ok(Build {
        data: data.clone(),
        metadata,
        binary,
        slice: (off, size),
        universal,
        metadata_version: i32::from_le_bytes(head[4..].try_into().unwrap()),
    })
}

/// The files the Android player keeps under Data/Managed/ for an IL2CPP build, which a macOS
/// build has in il2cpp_data/: (path under Data, source).
pub fn managed_files(build: &Build) -> Vec<(String, String)> {
    let base = format!("{}/il2cpp_data", build.data);
    let mut out = Vec::new();
    for (r, _, mut fs) in paths::walk(&base, true) {
        fs.sort();
        for n in fs {
            if n.starts_with('.') {
                continue;
            }
            let s = format!("{r}/{n}");
            out.push((format!("Managed/{}", paths::relpath(&s, &base)), s));
        }
    }
    out
}

/// What `pmunity il2cpp` states of its build in il2cpp.json, for this one: android-data's
/// --il2cpp paths (the package file's abi, the verify step's assemblies) read it.
/// The Input System package's image in a build.
pub const INPUT_SYSTEM: &str = "Unity.InputSystem.dll";

/// How the player's pads (Android game controllers to the package) get a layout in the build.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Pads {
    /// the package's own Android support is in the build
    Own,
    /// the bridge registers layouts with the package's InputSystem.RegisterLayout
    Bridge,
    /// the build has no InputSystem.RegisterLayout: nothing gives the pads a layout
    None,
}

impl Pads {
    pub fn key(self) -> &'static str {
        match self {
            Pads::Own => "own",
            Pads::Bridge => "bridge",
            Pads::None => "none",
        }
    }
}

/// From the package's types in the build's metadata. (By name: whether the overload that takes
/// a layout's text is among the RegisterLayout methods, the bridge sees and says at run time.)
pub fn pads(types: &Members) -> Pads {
    if types.keys().any(|t| t.starts_with("UnityEngine.InputSystem.Android.")) {
        Pads::Own
    } else if types.get("UnityEngine.InputSystem.InputSystem").is_some_and(|(methods, _)| methods.contains("RegisterLayout")) {
        Pads::Bridge
    } else {
        Pads::None
    }
}

pub fn info(
    build: &Build,
    unity: &str,
    assemblies: Vec<String>,
    files: &[(String, String)],
    streaming_assets: &str,
) -> Json {
    let mut j = Json::obj();
    j.set("unity", unity.into());
    j.set("abi", ABI.into());
    j.set("metadata_version", (build.metadata_version as i64).into());
    j.set(
        "files",
        Json::List(files.iter().map(|(d, _)| format!("Data/{d}").as_str().into()).collect()),
    );
    j.set(
        "assemblies",
        Json::List(assemblies.iter().map(|a| a.as_str().into()).collect()),
    );
    j.set("streaming_assets", streaming_assets.into());
    j.set("data_path", false.into());
    j
}

/// The binary's arm64 code as a file of its own (a universal file's other half is not shipped).
pub fn write_slice(binary: &str, dst: &str) -> Result<u64, String> {
    let (off, size, _) = arm64_slice(binary)?;
    let mut f = File::open(binary).map_err(|e| format!("{binary}: {e}"))?;
    f.seek(SeekFrom::Start(off)).map_err(|e| format!("{binary}: {e}"))?;
    if let Some(d) = Path::new(dst).parent() {
        std::fs::create_dir_all(d).map_err(|e| format!("{}: {e}", d.display()))?;
    }
    let tmp = format!("{dst}.part");
    let mut o = File::create(&tmp).map_err(|e| format!("{tmp}: {e}"))?;
    let n = std::io::copy(&mut f.take(size), &mut o).map_err(|e| format!("{tmp}: {e}"))?;
    if n != size {
        return Err(format!("{binary}: {n} of {size} bytes read"));
    }
    o.flush().map_err(|e| format!("{tmp}: {e}"))?;
    drop(o);
    std::fs::rename(&tmp, dst).map_err(|e| format!("{dst}: {e}"))?;
    Ok(size)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str, b: &[u8]) -> String {
        let d = std::env::temp_dir().join(format!("pmu-macho-{}-{name}", std::process::id()));
        std::fs::write(&d, b).unwrap();
        d.to_string_lossy().into_owned()
    }

    fn thin(cpu: u32, sub: u32, fill: u8) -> Vec<u8> {
        let mut b = Vec::new();
        b.extend(MH_MAGIC_64.to_le_bytes());
        b.extend(cpu.to_le_bytes());
        b.extend(sub.to_le_bytes());
        b.resize(64, fill);
        b
    }

    fn fat(parts: &[(u32, u32, Vec<u8>)]) -> Vec<u8> {
        let mut b = Vec::new();
        b.extend(FAT_MAGIC.to_be_bytes());
        b.extend((parts.len() as u32).to_be_bytes());
        let mut off = 4096u32;
        let mut body = Vec::new();
        for (cpu, sub, d) in parts {
            b.extend(cpu.to_be_bytes());
            b.extend(sub.to_be_bytes());
            b.extend(off.to_be_bytes());
            b.extend((d.len() as u32).to_be_bytes());
            b.extend(12u32.to_be_bytes());
            body.push((off, d.clone()));
            off += 4096;
        }
        for (o, d) in body {
            b.resize(o as usize, 0);
            b.extend(d);
        }
        b
    }

    /// --macho's value: the app, its Contents folder or its data folder; the game's name is the app's
    #[test]
    fn input_system_pads() {
        let types = |names: &[(&str, &[&str])]| -> Members {
            names
                .iter()
                .map(|(t, m)| (t.to_string(), (m.iter().map(|s| s.to_string()).collect(), Default::default())))
                .collect()
        };
        let system = "UnityEngine.InputSystem.InputSystem";
        assert_eq!(pads(&types(&[(system, &["RegisterLayout", "Update"])])), Pads::Bridge);
        assert_eq!(pads(&types(&[(system, &["Update"])])), Pads::None);
        assert_eq!(pads(&types(&[])), Pads::None);
        assert_eq!(
            pads(&types(&[(system, &["Update"]), ("UnityEngine.InputSystem.Android.AndroidGamepad", &[])])),
            Pads::Own
        );
    }

    #[test]
    fn app_paths() {
        let t = std::env::temp_dir().join(format!("pmu-macho-{}-app", std::process::id()));
        let data = t.join("Some Game.app/Contents/Resources/Data");
        std::fs::create_dir_all(&data).unwrap();
        let (t, data) = (t.to_string_lossy().into_owned(), data.to_string_lossy().into_owned());
        for given in ["Some Game.app", "Some Game.app/", "Some Game.app/Contents", "Some Game.app/Contents/Resources/Data"] {
            assert_eq!(data_of(&format!("{t}/{given}")), data, "{given}");
        }
        assert_eq!(app_name(&data).as_deref(), Some("Some Game"));
        assert_eq!(app_name(&format!("{t}/Data")), None);
        assert_eq!(data_of(&format!("{t}/none")), format!("{t}/none"));
        assert!(find(&format!("{t}/Some Game.app")).unwrap_err().contains("is no macOS IL2CPP build"));
        std::fs::remove_dir_all(&t).unwrap();
    }

    #[test]
    fn slices() {
        let a = tmp("thin", &thin(CPU_ARM64, 0, 1));
        assert_eq!(arm64_slice(&a).unwrap(), (0, 64, false));
        let x = tmp("intel", &thin(CPU_X86_64, 3, 1));
        assert!(arm64_slice(&x).unwrap_err().contains("x86_64 code only"));
        let e = tmp("arm64e", &thin(CPU_ARM64, 0x8000_0002, 1));
        assert!(arm64_slice(&e).unwrap_err().contains("arm64e code only"));
        let u = tmp(
            "fat",
            &fat(&[(CPU_X86_64, 3, thin(CPU_X86_64, 3, 7)), (CPU_ARM64, 0, thin(CPU_ARM64, 0, 9))]),
        );
        assert_eq!(arm64_slice(&u).unwrap(), (8192, 64, true));
        let out = format!("{u}.out");
        assert_eq!(write_slice(&u, &out).unwrap(), 64);
        assert_eq!(std::fs::read(&out).unwrap(), thin(CPU_ARM64, 0, 9));
        let i = tmp("fat-intel", &fat(&[(CPU_X86_64, 3, thin(CPU_X86_64, 3, 7))]));
        assert!(arm64_slice(&i).unwrap_err().contains("a universal file of x86_64"));
        let mut cut = fat(&[(CPU_ARM64, 0, thin(CPU_ARM64, 0, 9))]);
        cut.truncate(4100);
        let c = tmp("cut", &cut);
        assert!(arm64_slice(&c).unwrap_err().contains("cut-off copy"));
        let n = tmp("none", b"#!/bin/sh\n# nothing");
        assert!(arm64_slice(&n).unwrap_err().contains("no Mach-O file"));
        for p in [a, x, e, u, out, i, c, n] {
            std::fs::remove_file(p).unwrap();
        }
    }
}
