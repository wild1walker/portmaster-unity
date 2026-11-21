//! `pmu arch-check`: which ARM architectures a port release carries, and whether an aarch64-only
//! release (port.toml `port.aarch64_only`, docs/arm64.md "Shipping aarch64 only") still has every
//! piece its 64-bit route needs. Every ELF file is found by its header, in the release's folders and
//! inside its zips (the donor APKs, an OBB): a 32-bit ARM file (`EM_ARM`) anywhere fails an
//! aarch64-only release, a 32-bit player in a donor APK too. Read only.
use std::fs::{self, File};
use std::io::Read;
use std::path::{Path, PathBuf};

use crate::zip::Archive;
use crate::{io_at, Error, Result};

pub const EM_ARM: u16 = 40;
pub const EM_AARCH64: u16 = 183;

/// The archives looked into (by name; a zip by its first bytes too).
const ARCHIVES: [&str; 6] = [".apk", ".zip", ".obb", ".aab", ".xapk", ".apks"];

/// An ELF file's class (1: 32-bit, 2: 64-bit) and machine, from its first 20 bytes.
pub fn elf_machine(head: &[u8]) -> Option<(u8, u16)> {
    if head.len() < 20 || &head[..4] != b"\x7fELF" {
        return None;
    }
    let m = if head[5] == 2 {
        u16::from_be_bytes([head[18], head[19]])
    } else {
        u16::from_le_bytes([head[18], head[19]])
    };
    Some((head[4], m))
}

/// The machine's name, as `file` and PortMaster say it.
pub fn machine_name(m: u16) -> String {
    match m {
        EM_ARM => "armhf".into(),
        EM_AARCH64 => "aarch64".into(),
        3 => "x86".into(),
        62 => "x86_64".into(),
        _ => format!("machine {m}"),
    }
}

/// One ELF file found: its path (an archive's entry as `<archive>!<entry>`), class and machine.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Elf {
    pub path: String,
    pub class: u8,
    pub machine: u16,
}

fn head_of(r: &mut dyn Read) -> std::io::Result<Vec<u8>> {
    let mut h = vec![0u8; 20];
    let mut n = 0;
    while n < h.len() {
        let k = r.read(&mut h[n..])?;
        if k == 0 {
            break;
        }
        n += k;
    }
    h.truncate(n);
    Ok(h)
}

fn is_archive_name(n: &str) -> bool {
    let l = n.to_ascii_lowercase();
    ARCHIVES.iter().any(|x| l.ends_with(x))
}

/// The ELF files of an archive's entries (nested archives are not opened: a release's donors and
/// OBBs hold none).
pub fn scan_archive(path: &str, label: &str) -> Result<Vec<Elf>> {
    let z = Archive::open(path)?;
    let mut out = Vec::new();
    for e in &z.entries {
        if e.is_dir() || e.file_size < 20 {
            continue;
        }
        let mut r = z.reader(e)?;
        let h = head_of(&mut r)?;
        if let Some((class, machine)) = elf_machine(&h) {
            out.push(Elf {
                path: format!("{label}!{}", e.name),
                class,
                machine,
            });
        }
    }
    Ok(out)
}

/// Every ELF file under root, and inside its archives, sorted by path (relative to root).
pub fn scan(root: &Path) -> Result<Vec<Elf>> {
    let mut out = Vec::new();
    let mut todo: Vec<PathBuf> = vec![root.to_path_buf()];
    while let Some(d) = todo.pop() {
        for ent in fs::read_dir(&d).map_err(io_at(&d))? {
            let p = ent.map_err(io_at(&d))?.path();
            let ft = fs::symlink_metadata(&p).map_err(io_at(&p))?.file_type();
            if ft.is_dir() {
                todo.push(p);
                continue;
            }
            if !ft.is_file() {
                continue;
            }
            let rel = p
                .strip_prefix(root)
                .unwrap_or(&p)
                .to_string_lossy()
                .replace('\\', "/");
            let h = head_of(&mut File::open(&p).map_err(io_at(&p))?).map_err(io_at(&p))?;
            if let Some((class, machine)) = elf_machine(&h) {
                out.push(Elf {
                    path: rel,
                    class,
                    machine,
                });
            } else if h.starts_with(b"PK\x03\x04") || is_archive_name(&rel) {
                let ps = p.to_string_lossy().into_owned();
                match scan_archive(&ps, &rel) {
                    Ok(v) => out.extend(v),
                    // (an archive that does not open is reported: a release must not carry one)
                    Err(e) => {
                        return Err(Error::Exit(format!("{rel}: not a readable archive ({e})")))
                    }
                }
            }
        }
    }
    out.sort_by(|a, b| a.path.cmp(&b.path));
    Ok(out)
}

/// A shell assignment's value as `shlex.quote` writes it ('...' with '"'"' inside, or bare).
fn unquote(v: &str) -> String {
    let (mut out, mut q) = (String::new(), None::<char>);
    for c in v.chars() {
        match (q, c) {
            (None, '\'' | '"') => q = Some(c),
            (Some(o), c) if c == o => q = None,
            _ => out.push(c),
        }
    }
    out
}

/// setup/setup.env's simple assignments (`KEY=value`), as the setup sources them.
pub fn read_env(path: &Path) -> Result<Vec<(String, String)>> {
    let text = fs::read_to_string(path).map_err(io_at(path))?;
    Ok(text
        .lines()
        .filter(|l| !l.starts_with('#'))
        .filter_map(|l| l.split_once('='))
        .filter(|(k, _)| !k.is_empty() && k.chars().all(|c| c.is_ascii_alphanumeric() || c == '_'))
        .map(|(k, v)| (k.to_string(), unquote(v)))
        .collect())
}

/// What an aarch64-only release check found.
#[derive(Debug, Default)]
pub struct Report {
    /// The port folder (ports/<name>) the release installs.
    pub port: String,
    pub route: String,
    pub elves: Vec<Elf>,
    /// The 32-bit ARM files (EM_ARM), archives' entries included.
    pub armhf: Vec<String>,
    /// What the 64-bit route needs and the release lacks (or has for another machine).
    pub missing: Vec<String>,
}

impl Report {
    pub fn ok(&self) -> bool {
        self.armhf.is_empty() && self.missing.is_empty()
    }
}

/// The routes whose setup and player are 64-bit only (port_template/<route>).
pub const AARCH64_ROUTES: [&str; 2] = ["mono-arm64", "il2cpp-arm64"];

/// A Unity version's line ("6000.0.61f1" -> "6000.0").
fn line_of(v: &str) -> String {
    v.split('.').take(2).collect::<Vec<_>>().join(".")
}

/// The release folder's port: the folder with port.json and setup/setup.env.
fn find_port(root: &Path) -> Result<String> {
    let mut found = Vec::new();
    for ent in fs::read_dir(root).map_err(io_at(root))? {
        let p = ent.map_err(io_at(root))?.path();
        if p.join("port.json").is_file() && p.join("setup").join("setup.env").is_file() {
            found.push(
                p.file_name()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .into_owned(),
            );
        }
    }
    found.sort();
    match found.len() {
        1 => Ok(found.remove(0)),
        0 => Err(Error::Exit(format!(
            "{}: no port folder (one with port.json and setup/setup.env): not a release",
            root.display()
        ))),
        _ => Err(Error::Exit(format!(
            "{}: more than one port folder ({})",
            root.display(),
            found.join(", ")
        ))),
    }
}

/// The aarch64-only check of an unpacked release (the zip's top folder: the launcher and
/// ports/<name>): no EM_ARM file anywhere, and the 64-bit route's pieces there, each for aarch64:
/// the 64-bit setup (setup.env's PORT_ROUTE one of AARCH64_ROUTES, its patchscript), ua_host,
/// the setup's tools (pmu, or the setup's Python), and for mono-arm64, for each Unity version: the
/// IL2CPP arm64 donor with its player (libmain, libunity, libil2cpp), the engine's assemblies from
/// the Mono donor and Unity's Mono for aarch64 of the version's line (setup/mono64/<line>).
pub fn check_release(root: &Path) -> Result<Report> {
    let port = find_port(root)?;
    let pd = root.join(&port);
    let env = read_env(&pd.join("setup").join("setup.env"))?;
    let get = |k: &str| {
        env.iter()
            .rev()
            .find(|(kk, _)| kk == k)
            .map(|(_, v)| v.clone())
            .unwrap_or_default()
    };
    let mut r = Report {
        route: get("PORT_ROUTE"),
        elves: scan(root)?,
        port: port.clone(),
        ..Default::default()
    };
    r.armhf = r
        .elves
        .iter()
        .filter(|e| e.machine == EM_ARM)
        .map(|e| e.path.clone())
        .collect();
    let machine_of = |rel: &str| {
        r.elves
            .iter()
            .find(|e| e.path == rel)
            .map(|e| (e.class, e.machine))
    };
    let mut missing = Vec::new();
    let need_a64 = |rel: String, what: &str, missing: &mut Vec<String>| match machine_of(&rel) {
        Some((2, EM_AARCH64)) => {}
        Some((c, m)) => missing.push(format!(
            "{rel} ({what}) is {} ({}-bit), not aarch64",
            machine_name(m),
            if c == 2 { 64 } else { 32 }
        )),
        None => missing.push(format!("{rel} ({what})")),
    };
    if !AARCH64_ROUTES.contains(&r.route.as_str()) {
        missing.push(format!(
            "a 64-bit setup: {port}/setup/setup.env says PORT_ROUTE={} (an aarch64-only release is made \
             on the {} route: `port new` again with port.aarch64_only = true)",
            if r.route.is_empty() { "nothing" } else { &r.route },
            AARCH64_ROUTES.join(" or ")
        ));
    }
    if !pd.join("setup").join("patchscript").is_file() {
        missing.push(format!("{port}/setup/patchscript (the setup)"));
    }
    let launchers = fs::read_dir(root)
        .map_err(io_at(root))?
        .filter_map(|e| e.ok())
        .filter(|e| e.path().is_file() && e.file_name().to_string_lossy().ends_with(".sh"))
        .count();
    if launchers == 0 {
        missing.push("the launcher (<title>.sh beside the port folder)".into());
    }
    need_a64(format!("{port}/ua_host"), "the runtime", &mut missing);
    if pd.join("setup").join("pmu").exists() || !pd.join("setup").join("python.tar.gz").is_file() {
        need_a64(
            format!("{port}/setup/pmu"),
            "the setup's tools",
            &mut missing,
        );
    }
    let versions: Vec<String> = get("UNITY_VERSIONS")
        .split_whitespace()
        .map(str::to_string)
        .collect();
    if versions.is_empty() {
        missing.push(format!(
            "{port}/setup/setup.env: UNITY_VERSIONS names no Unity version"
        ));
    }
    if (get("PLAYER_ABI"), get("PLAYER_BACKEND")) != ("arm64-v8a".into(), "il2cpp".into()) {
        missing.push(format!(
            "the player: setup.env says {}/{}, the 64-bit route runs Unity's il2cpp/arm64-v8a player",
            get("PLAYER_BACKEND"),
            get("PLAYER_ABI")
        ));
    }
    let mut lines: Vec<String> = Vec::new();
    // (the il2cpp-arm64 route fetches Unity's IL2CPP tools and player on the handheld: no donor)
    for v in versions.iter().filter(|_| r.route == "mono-arm64") {
        let donor = format!("{port}/donor/donor-{v}-il2cpp-arm64.apk");
        if !root.join(&donor).is_file() {
            missing.push(format!("{donor} (Unity {v}'s arm64 player)"));
        } else {
            for lib in ["libmain.so", "libunity.so", "libil2cpp.so"] {
                need_a64(
                    format!("{donor}!lib/arm64-v8a/{lib}"),
                    "Unity's arm64 player",
                    &mut missing,
                );
            }
        }
        {
            let mono = format!("{port}/donor/donor-{v}-mono-armv7.apk");
            let managed = "assets/bin/Data/Managed/UnityEngine.dll";
            match Archive::open(&root.join(&mono).to_string_lossy()) {
                Ok(z) if z.get(managed).is_some() => {}
                Ok(_) => missing.push(format!("{mono}!{managed} (the engine's assemblies)")),
                Err(_) => missing.push(format!("{mono} (the engine's assemblies, Unity {v})")),
            }
            let l = line_of(v);
            if !lines.contains(&l) {
                lines.push(l);
            }
        }
    }
    for l in &lines {
        for lib in ["libil2cpp.so", "libmonobdwgc-2.0.so"] {
            need_a64(
                format!("{port}/setup/mono64/{l}/{lib}"),
                "Unity's Mono for aarch64 and its bridge",
                &mut missing,
            );
        }
    }
    r.missing = missing;
    Ok(r)
}

/// check_release on a release zip: unpacked into a new folder under `tmp` first (removed after).
pub fn check_release_zip(zip: &str, tmp: &Path) -> Result<Report> {
    let d = tmp.join(format!("arch-check-{}", std::process::id()));
    let _ = fs::remove_dir_all(&d);
    fs::create_dir_all(&d).map_err(io_at(&d))?;
    let r = crate::zip::extract_all(zip, &d.to_string_lossy()).and_then(|_| check_release(&d));
    let _ = fs::remove_dir_all(&d);
    r
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn headers() {
        let mut h = b"\x7fELF\x01\x01\x01".to_vec();
        h.resize(20, 0);
        h[18..20].copy_from_slice(&EM_ARM.to_le_bytes());
        assert_eq!(elf_machine(&h), Some((1, EM_ARM)));
        h[4] = 2;
        h[18..20].copy_from_slice(&EM_AARCH64.to_le_bytes());
        assert_eq!(elf_machine(&h), Some((2, EM_AARCH64)));
        assert_eq!(elf_machine(&h[..19]), None);
        assert_eq!(elf_machine(b"#!/bin/bash\nexit 0\n\n\n"), None);
    }

    #[test]
    fn shell_values() {
        assert_eq!(unquote("'a b'"), "a b");
        assert_eq!(unquote("mono-arm64"), "mono-arm64");
        assert_eq!(unquote("'it'\"'\"'s'"), "it's");
    }
}
