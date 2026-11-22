//! What Wwise a game uses (plugins.py's wwise_info and the lines port check writes from it): the
//! version of its desktop sound engine library, the soundbank format of its banks, and whether
//! ua_host's own Wwise player reads them.
use crate::pyx::*;
use port::val::{Dict, Val};
use regex::bytes::Regex as BRegex;
use std::sync::OnceLock;

fn bank_line(v: i64) -> Option<&'static str> {
    Some(match v {
        88 => "2013",
        112 => "2014.1",
        113 => "2015.1",
        118 => "2016.1",
        120 => "2016.2",
        125 => "2017.1",
        128 => "2017.2",
        132 => "2018.1",
        134 => "2019.1",
        135 => "2019.2",
        140 => "2021.1",
        _ => return None,
    })
}

/// soundbank versions the setup's `pmu wwise` reads into ua_host's own Wwise player's data
pub const WWISE_PLAYER_VERSIONS: [i64; 1] = [128];

fn rd(d: &[u8], o: usize, n: usize, le: bool) -> Option<u64> {
    let b = d.get(o..o.checked_add(n)?)?;
    let mut v = 0u64;
    for i in 0..n {
        v = v << 8 | b[if le { n - 1 - i } else { i }] as u64;
    }
    Some(v)
}

/// {name: file offset} of an ELF's dynamic symbols that are functions, via the section headers
/// (what was found before a read past the file's end stays).
fn elf_dynsyms(d: &[u8]) -> Vec<(String, usize)> {
    let mut out: Vec<(String, usize)> = Vec::new();
    let mut go = || -> Option<()> {
        let (is64, le) = (*d.get(4)? == 2, *d.get(5)? == 1);
        let w = if is64 { 8 } else { 4 };
        let shoff = rd(d, if is64 { 0x28 } else { 0x20 }, w, le)? as usize;
        let shsz = rd(d, if is64 { 0x3A } else { 0x2E }, 2, le)? as usize;
        let shn = rd(d, if is64 { 0x3C } else { 0x30 }, 2, le)? as usize;
        let mut secs = Vec::new();
        for i in 0..shn {
            let o = shoff + i * shsz;
            // (name, type, flags, addr, offset, size, link, info, addralign, entsize)
            let typ = rd(d, o + 4, 4, le)?;
            let (addr, off, size, link, entsz) = if is64 {
                (rd(d, o + 16, 8, le)?, rd(d, o + 24, 8, le)?, rd(d, o + 32, 8, le)?, rd(d, o + 40, 4, le)?, rd(d, o + 56, 8, le)?)
            } else {
                (rd(d, o + 12, 4, le)?, rd(d, o + 16, 4, le)?, rd(d, o + 20, 4, le)?, rd(d, o + 24, 4, le)?, rd(d, o + 36, 4, le)?)
            };
            secs.push((typ, addr, off as usize, size, link as usize, entsz as usize));
        }
        for &(typ, _, off, size, link, entsz) in &secs {
            if typ != 11 || entsz == 0 {
                continue;
            }
            let stroff = secs.get(link)?.2;
            let mut o = off;
            while o < off + size as usize {
                let name = rd(d, o, 4, le)? as usize;
                let (info, shndx, value) = if is64 {
                    (rd(d, o + 4, 1, le)?, rd(d, o + 6, 2, le)?, rd(d, o + 8, 8, le)?)
                } else {
                    (rd(d, o + 12, 1, le)?, rd(d, o + 14, 2, le)?, rd(d, o + 4, 4, le)?)
                };
                if is64 {
                    rd(d, o + 16, 8, le)?;
                }
                if shndx != 0 && info & 0xf == 2 {
                    let st = stroff + name;
                    let e = st + find(d.get(st..)?, b"\0")?;
                    let n: String = d[st..e].iter().map(|&c| c as char).collect();
                    for &(t, a, so, sz, _, _) in &secs {
                        if t == 1 && a <= value && value < a + sz {
                            let at = so + (value - a) as usize;
                            match out.iter_mut().find(|(k, _)| *k == n) {
                                Some(x) => x.1 = at,
                                None => out.push((n.clone(), at)),
                            }
                        }
                    }
                }
                o += entsz;
            }
        }
        Some(())
    };
    let _ = go();
    out
}

/// The int an x86 function returns as a constant: mov eax, imm32; ret, after following a jmp, or
/// the first call.
fn x86_constant(d: &[u8], off: Option<usize>, depth: u32) -> Option<u32> {
    let off = off?;
    if depth > 2 || off + 8 > d.len() {
        return None;
    }
    if d[off] == 0xb8 && d[off + 5] == 0xc3 {
        return Some(u32::from_le_bytes(d[off + 1..off + 5].try_into().unwrap()));
    }
    let end = (off + 24).min(d.len().saturating_sub(5));
    for i in off..end {
        if d[i] == 0xe9 || d[i] == 0xe8 {
            let rel = i32::from_le_bytes(d[i + 1..i + 5].try_into().unwrap()) as i64;
            let target = i as i64 + 5 + rel;
            // (a negative target indexes from the end in the Python; such code is no library's)
            let v = if target < 0 { None } else { x86_constant(d, Some(target as usize), depth + 1) };
            if v.is_some() || d[i] == 0xe9 {
                return v;
            }
        }
    }
    None
}

/// The most of Wwise's packed Vorbis codebooks the library at path holds back to back (64 at
/// least), as pmu wwise finds them; 0 without.
fn codebooks(path: &str) -> i64 {
    let Ok(d) = std::fs::read(path) else { return 0 };
    ::wwise::codebooks::find(&d, path).iter().map(|l| l.count as i64).max().unwrap_or(0)
}

/// None without Wwise; else {libraries, version, [version_from], banks, bank_versions,
/// bank_folders, bank_lines, codebooks}.
pub fn wwise_info(data: &str) -> Option<Dict> {
    static LIB: OnceLock<regex::Regex> = OnceLock::new();
    static VER: OnceLock<BRegex> = OnceLock::new();
    static SBV: OnceLock<BRegex> = OnceLock::new();
    let lib_rx = LIB.get_or_init(|| regex::Regex::new(r"(?i)^(lib)?aksoundengine\b").unwrap());
    let ver_rx = VER.get_or_init(|| {
        BRegex::new(r"(?-u)(?:Wwise|Ak)[^\x00]{0,16}?v?(20[1-3][0-9]\.[0-9]+\.[0-9]+)(?:[^\x00]{0,8}?[Bb]uild[ :]*([0-9]+))?").unwrap()
    });
    let sbv_rx = SBV.get_or_init(|| BRegex::new(r#"(?-u)SoundbankVersion="([0-9]+)""#).unwrap());
    let libs: Vec<(String, String)> = androiddata::plugins::plugin_files(data).into_iter().filter(|(rel, _)| lib_rx.is_match(basename(rel))).collect();
    let mut banks: Vec<String> = Vec::new();
    walk(&join(data, "StreamingAssets"), &mut |r, ds, fs| {
        ds.sort();
        let mut names: Vec<&String> = fs.iter().filter(|n| n.to_lowercase().ends_with(".bnk")).collect();
        names.sort();
        banks.extend(names.into_iter().map(|n| join(r, n)));
    });
    if libs.is_empty() && banks.is_empty() {
        return None;
    }
    let mut folders: Vec<String> = banks.iter().map(|b| relpath(dirname(b), data)).collect();
    folders.sort();
    folders.dedup();
    let mut info = dict(vec![
        ("libraries", Val::List(libs.iter().map(|(rel, _)| s(rel)).collect())),
        ("version", Val::Null),
        ("banks", Val::Int(banks.len() as i64)),
        ("bank_versions", Val::List(Vec::new())),
        ("bank_folders", strs(&folders)),
    ]);
    for (rel, p) in &libs {
        if isdir(p) || info.truthy("version") {
            continue;
        }
        let Ok(d) = std::fs::read(p) else { continue };
        if let Some(m) = ver_rx.captures(&d) {
            let g = |i: usize| m.get(i).map(|x| String::from_utf8_lossy(x.as_bytes()).into_owned());
            info.set("version", s(format!("{}{}", g(1).unwrap(), g(2).map_or(String::new(), |b| format!(" build {b}")))));
            info.set("version_from", s(format!("{rel} (a string)")));
            continue;
        }
        if d.len() < 19 || &d[..4] != b"\x7fELF" || ![3, 62].contains(&d[18]) {
            continue;
        }
        let syms = elf_dynsyms(&d);
        let sym = |n: &str| syms.iter().find(|(k, _)| k == n).map(|x| x.1);
        let mm = x86_constant(&d, sym("CSharp_GetMajorMinorVersion"), 0);
        let sb = x86_constant(&d, sym("CSharp_GetSubminorBuildVersion"), 0);
        if let Some(mm) = mm.filter(|mm| (2010..=2040).contains(&(mm >> 16)) && (mm & 0xffff) < 20) {
            info.set("version", s(format!("{}.{}{}", mm >> 16, mm & 0xffff,
                                          sb.map_or(String::new(), |sb| format!(".{} build {}", sb >> 16, sb & 0xffff)))));
            info.set("version_from", s(format!("{rel} (its code)")));
        }
    }
    let mut vers: Vec<i64> = Vec::new();
    for b in banks.iter().take(64) {
        let h = head(b, 12);
        if h.len() == 12 && &h[..4] == b"BKHD" {
            vers.push(u32::from_le_bytes(h[8..12].try_into().unwrap()) as i64);
        }
    }
    for folder in &folders {
        let x = joins(&[data, folder, "SoundbanksInfo.xml"]);
        if let Some(m) = sbv_rx.captures(&head(&x, 4096)) {
            if let Ok(v) = String::from_utf8_lossy(&m[1]).parse::<i64>() {
                vers.push(v);
            }
        }
    }
    vers.sort();
    vers.dedup();
    info.set("bank_versions", Val::List(vers.iter().map(|v| Val::Int(*v)).collect()));
    let lines: Vec<&str> = vers.iter().map(|v| bank_line(*v).unwrap_or(if *v > 140 { "2021.1 or later" } else { "unknown" })).collect();
    info.set("bank_lines", strs(&lines));
    // Wwise Vorbis's codebooks: pmu wwise reads them from this library at setup
    info.set("codebooks", Val::Null);
    for (rel, p) in &libs {
        let mut p = p.clone();
        if isdir(&p) {
            let b = basename(&p).to_string();
            let stem = b.rsplit_once('.').map_or(b.as_str(), |x| if x.0.is_empty() { b.as_str() } else { x.0 }).to_string();
            p = joins(&[&p, "Contents", "MacOS", &stem]);
        }
        if isfile(&p) {
            let n = codebooks(&p);
            info.set("codebooks", Val::List(vec![s(rel), Val::Int(n)]));
            if n != 0 {
                break;
            }
        }
    }
    Some(info)
}

/// The folder of soundbanks (relative to the data folder) the port's setup converts for ua_host's
/// Wwise player, or None.
pub fn wwise_player_banks(data: &str, w: &Dict) -> Option<String> {
    let vers = w.list("bank_versions");
    if vers.is_empty() || !vers.iter().all(|v| WWISE_PLAYER_VERSIONS.contains(&crate::survey::int_of(v))) {
        return None;
    }
    let all = str_list(w, "bank_folders");
    let mut folders: Vec<String> = all.iter().filter(|f| isfile(&joins(&[data, f, "Init.bnk"]))).cloned().collect();
    if folders.is_empty() {
        folders = all;
    }
    for plat in ["linux", "mac", "windows"] {
        for f in &folders {
            if basename(f).to_lowercase().starts_with(plat) {
                return Some(f.clone());
            }
        }
    }
    folders.first().cloned()
}

/// One line on a game's Wwise and what the port does with it.
pub fn wwise_summary(w: &Dict, player: Option<&str>) -> String {
    let ver = w.str("version");
    let vers: Vec<String> = w.list("bank_versions").iter().map(Val::py_str).collect();
    let lines = str_list(w, "bank_lines");
    let banks = vers.iter().zip(&lines).map(|(v, ln)| format!("version {v} (Wwise {ln})")).collect::<Vec<_>>().join(", ");
    let what = format!("Wwise {}", if !ver.is_empty() { ver } else if let Some(l) = lines.first() { l } else { "(version not read)" });
    let mut src = Vec::new();
    if !ver.is_empty() {
        src.push(w.str("version_from").to_string());
    }
    if w.truthy("banks") {
        src.push(format!("{} soundbanks in {}{}", int(w, "banks"), str_list(w, "bank_folders").join(", "),
                         if banks.is_empty() { String::new() } else { format!(": {banks}") }));
    }
    let base = format!("{what} ({}): the port gives the game ua_host's built-in Wwise, made of the integration's own entry points \
                        (every call succeeds, banks load, the bank-loaded and end-of-event callbacks reach the game)", src.join("; "));
    let cb = w.list("codebooks");
    let vorbis = if cb.len() == 2 && cb[1].truthy() {
        format!("; its Vorbis sounds need the {} codebooks in {}, which pmu wwise reads at setup", cb[1].py_str(), cb[0].py_str())
    } else if cb.len() == 2 {
        format!("; no Vorbis codebooks found in {}: its Vorbis sounds would stay silent (ADPCM and PCM ones play)", cb[0].py_str())
    } else {
        "; no Wwise library found: its Vorbis sounds would stay silent (ADPCM and PCM ones play)".to_string()
    };
    if let Some(player) = player {
        return format!("{base}. Sound: the setup converts the banks in {player} once (pmu wwise: wwise.banks in port.toml) into the \
                        port's wwise/ folder, which ua_host's own Wwise player plays (UA_WWISE_DATA, docs/wwise-player.md){vorbis}");
    }
    let vs = WWISE_PLAYER_VERSIONS.iter().map(i64::to_string).collect::<Vec<_>>().join(", ");
    format!("{base}; silent: ua_host's Wwise player reads soundbank version {vs} only{}. Sound needs Wwise's sound engine for ARM \
             of {} in the port's lib/ (Wwise's Android SDK: libAkSoundEngine.so, an Audiokinetic account) or a sound backend \
             (UA_WWISE_BACKEND, docs/plugins.md)",
            if vers.is_empty() { String::new() } else { format!(" (these are {})", vers.join(", ")) },
            if ver.is_empty() { "the same version" } else { ver })
}
