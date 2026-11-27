//! Function names for module+offset addresses (was tools/analysis/elfsyms.py and unitysyms.py), from
//! ELF files with symbols: Unity's symbol files (Variations/<backend>/Release/Symbols/<abi>/
//! libunity.sym.so, libmonobdwgc-2.0.sym.so, libmain.sym.so, as `pmu player fetch --symbols` downloads
//! them), ua_host (runtime/host/out/ua_host: stripped, its names from ua_host.debug beside it), or any
//! library copied from a handheld (its exported names only, when it is stripped).
//!
//! A folder's <name>.sym.so stands for module <name>.so, any other ELF file for its own name. Tables
//! are cached as syms_<module>_<key>.txt in tools/analysis (gitignored: derived from Unity's or the
//! device's binaries; $PMU_DEV_CACHE another folder). libunity.so also comes from unitysyms' sources
//! ($UA_SYMS, syms_libunity.txt, $UA_LIBUNITY_SYM) when no folder or file names it. Names are
//! demangled with c++filt (binutils, or macOS's) when there is one.
use std::collections::HashMap;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use crate::elf::Elf;

/// Where the symbol tables are cached.
pub fn cache_dir() -> PathBuf {
    if let Some(d) = std::env::var_os("PMU_DEV_CACHE").filter(|d| !d.is_empty()) {
        return PathBuf::from(d);
    }
    let here = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tools/analysis");
    if here.is_dir() {
        here
    } else {
        std::env::temp_dir()
    }
}

pub(crate) fn demangle(names: &[String]) -> Vec<String> {
    for args in [&["-n"][..], &[][..]] {
        let Ok(mut c) = Command::new("c++filt").args(args).stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::null()).spawn() else {
            continue;
        };
        let input = names.join("\n");
        let mut stdin = c.stdin.take().unwrap();
        let w = std::thread::spawn(move || {
            let _ = stdin.write_all(input.as_bytes());
        });
        let Ok(o) = c.wait_with_output() else { continue };
        let _ = w.join();
        let text = String::from_utf8_lossy(&o.stdout).into_owned();
        let out: Vec<String> = text.split('\n').map(String::from).collect();
        let mangled = names.iter().any(|n| n.starts_with("_Z"));
        let changed = out.iter().zip(names).any(|(o, n)| n.starts_with("_Z") && !o.starts_with("_Z") && !o.starts_with("__Z"));
        if out.len() >= names.len() && (!mangled || changed) {
            return out[..names.len()].to_vec();
        }
    }
    names.to_vec()
}

fn fnv(s: &str) -> u64 {
    s.bytes().fold(0xcbf29ce484222325u64, |h, b| (h ^ b as u64).wrapping_mul(0x100000001b3))
}

/// [(address, size, name)] of an ELF file's functions, sorted (cached). A stripped file with the file
/// as linked beside it (<path>.debug: runtime/host/build.sh's ua_host.debug) is read from that.
pub fn load(path: &str) -> Result<Vec<(u64, u64, String)>, String> {
    let mut path = path.to_string();
    if Path::new(&format!("{path}.debug")).is_file() {
        path += ".debug";
    }
    let md = std::fs::metadata(&path).map_err(|e| format!("{path}: {e}"))?;
    let abs = std::fs::canonicalize(&path).unwrap_or_else(|_| PathBuf::from(&path));
    let mtime = md.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map_or(0, |d| d.as_secs());
    let key = format!("{:016x}", fnv(&format!("{}:{}:{mtime}", abs.display(), md.len())));
    let base = Path::new(&path).file_name().map(|b| b.to_string_lossy().into_owned()).unwrap_or_default();
    let cache = cache_dir().join(format!("syms_{base}_{}.txt", &key[..10]));
    if !cache.exists() {
        let elf = Elf::open(&path)?;
        let mut rows: HashMap<u64, (String, u64)> = HashMap::new();
        for s in elf.symbols() {
            let t = s.info & 15;
            if (t == 2 || t == 10) && s.value != 0 && s.shndx != 0 {
                let a = s.value & !1; // Thumb functions have bit 0 set
                if !rows.contains_key(&a) || (s.table == ".symtab" && rows[&a].1 == 0) {
                    rows.insert(a, (s.name, s.size));
                }
            }
        }
        let mut addrs: Vec<u64> = rows.keys().copied().collect();
        addrs.sort();
        let names = demangle(&addrs.iter().map(|a| rows[a].0.clone()).collect::<Vec<_>>());
        let mut text = String::new();
        for (a, n) in addrs.iter().zip(names) {
            text += &format!("{a:x} {:x} {n}\n", rows[a].1);
        }
        let tmp = cache.with_extension("txt.tmp");
        std::fs::write(&tmp, text).map_err(|e| format!("{}: {e}", tmp.display()))?;
        std::fs::rename(&tmp, &cache).map_err(|e| format!("{}: {e}", cache.display()))?;
    }
    let text = std::fs::read(&cache).map_err(|e| format!("{}: {e}", cache.display()))?;
    let mut rows = Vec::new();
    for line in String::from_utf8_lossy(&text).lines() {
        let p: Vec<&str> = line.splitn(3, ' ').collect();
        if let [a, s, n] = p[..] {
            if let (Ok(a), Ok(s)) = (u64::from_str_radix(a, 16), u64::from_str_radix(s, 16)) {
                rows.push((a, s, n.to_string()));
            }
        }
    }
    Ok(rows)
}

pub fn module_name(path: &str) -> String {
    let b = Path::new(path).file_name().map(|b| b.to_string_lossy().into_owned()).unwrap_or_default();
    match b.strip_suffix(".sym.so") {
        Some(s) => format!("{s}.so"),
        None => b,
    }
}

/// The name up to its argument list, at most `n` characters.
pub fn short(name: &str, n: usize) -> String {
    let s = name.split('(').next().unwrap_or("");
    s.chars().take(n).collect()
}

type Table = (Vec<(u64, u64, String)>, Vec<u64>);

pub struct ElfNames {
    short: bool,
    sources: HashMap<String, String>,
    tables: HashMap<String, Table>,
    /// (tests) tables given as they are
    pub given: HashMap<String, Vec<(u64, u64, String)>>,
}

impl ElfNames {
    pub fn new(dirs: &[String], files: &[String], short: bool) -> ElfNames {
        let mut sources = HashMap::new();
        for d in dirs {
            let mut names: Vec<String> = std::fs::read_dir(d)
                .map(|r| r.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect())
                .unwrap_or_default();
            names.sort();
            for f in names {
                let p = Path::new(d).join(&f);
                if p.is_file() && (f.ends_with(".so") || f.contains(".so.")) {
                    let p = p.to_string_lossy().into_owned();
                    sources.entry(module_name(&p)).or_insert(p);
                }
            }
        }
        for p in files {
            sources.insert(module_name(p), p.clone());
        }
        ElfNames { short, sources, tables: HashMap::new(), given: HashMap::new() }
    }

    fn table(&mut self, m: &str) -> &Table {
        if !self.tables.contains_key(m) {
            let rows = if let Some(r) = self.given.get(m) {
                r.clone()
            } else if let Some(src) = self.sources.get(m) {
                match load(src) {
                    Ok(r) => r,
                    Err(e) => {
                        println!("(symbols of {m}: {e})");
                        vec![]
                    }
                }
            } else if m == "libunity.so" {
                unity_load().0.into_iter().map(|(a, n)| (a, 0, n)).collect()
            } else {
                vec![]
            };
            let addrs = rows.iter().map(|r| r.0).collect();
            self.tables.insert(m.to_string(), (rows, addrs));
        }
        &self.tables[m]
    }

    pub fn has(&mut self, m: &str) -> bool {
        !self.table(m).0.is_empty()
    }

    pub fn name(&mut self, m: &str, off: u64) -> Option<String> {
        let short_names = self.short;
        let (rows, addrs) = self.table(m);
        let i = addrs.partition_point(|a| *a <= off & !1);
        if i == 0 {
            return None;
        }
        let (a, size, n) = &rows[i - 1];
        if *size != 0 && off.wrapping_sub(*a) >= size + 4 {
            return None; // past its end: not this function
        }
        Some(if short_names { short(n, 100) } else { n.clone() })
    }
}

/// libunity.so's symbols (was unitysyms.py): $UA_SYMS (a text file of 'address type name' lines, as
/// `nm -C` prints them), syms_libunity.txt in the cache folder, else built from $UA_LIBUNITY_SYM (the
/// unstripped libunity.sym.so of Unity's Android player) and saved there. ([(address, name)] sorted,
/// the addresses)
pub fn unity_load() -> (Vec<(u64, String)>, Vec<u64>) {
    let path = std::env::var("UA_SYMS").ok().filter(|p| !p.is_empty()).map(PathBuf::from).unwrap_or_else(|| cache_dir().join("syms_libunity.txt"));
    if !path.exists() {
        let Some(so) = std::env::var("UA_LIBUNITY_SYM").ok().filter(|p| !p.is_empty()) else {
            return (vec![], vec![]);
        };
        let Ok(elf) = Elf::open(&so) else { return (vec![], vec![]) };
        let mut rows: Vec<(u64, String)> =
            elf.symbols().into_iter().filter(|s| s.info & 15 == 2 && s.value != 0).map(|s| (s.value & !1, s.name)).collect();
        let names = demangle(&rows.iter().map(|r| r.1.clone()).collect::<Vec<_>>());
        if names.len() >= rows.len() && names.iter().take(2000).any(|n| !n.starts_with("_Z") && !n.starts_with("__Z") && n.contains("::")) {
            for (r, n) in rows.iter_mut().zip(names) {
                r.1 = n;
            }
        }
        rows.sort();
        let text: String = rows.iter().map(|(a, n)| format!("{a:08x} T {n}\n")).collect();
        if std::fs::write(&path, text).is_err() {
            return (vec![], vec![]);
        }
    }
    let Ok(text) = std::fs::read(&path) else { return (vec![], vec![]) };
    let mut syms = Vec::new();
    for line in String::from_utf8_lossy(&text).lines() {
        let p: Vec<&str> = line.splitn(3, ' ').collect();
        if let [a, t, n] = p[..] {
            if !t.is_empty() && "tTwW".contains(t) {
                if let Ok(a) = u64::from_str_radix(a, 16) {
                    syms.push((a, n.to_string()));
                }
            }
        }
    }
    syms.sort();
    let addrs = syms.iter().map(|s| s.0).collect();
    (syms, addrs)
}

/// unitysyms.namer: "libunity.so+0x<off>" tokens named from libunity.so's symbols.
pub struct UnityNamer {
    syms: Vec<(u64, String)>,
    addrs: Vec<u64>,
    short: bool,
}

impl UnityNamer {
    pub fn new(short: bool) -> UnityNamer {
        let (syms, addrs) = unity_load();
        UnityNamer { syms, addrs, short }
    }

    pub fn name(&self, tok: &str) -> String {
        let Some((m, off)) = tok.rsplit_once("+0x") else { return tok.to_string() };
        if m.is_empty() || m.contains(char::is_whitespace) || off.is_empty() || !off.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c)) {
            return tok.to_string();
        }
        let Ok(off) = u64::from_str_radix(off, 16) else { return tok.to_string() };
        let off = off & !1;
        if m == "libunity.so" && !self.addrs.is_empty() {
            let i = self.addrs.partition_point(|a| *a <= off);
            if i > 0 {
                let (a, n) = &self.syms[i - 1];
                return if self.short { short(n, 90) } else { format!("{} +{:#x}", n.chars().take(150).collect::<String>(), off - a) };
            }
        }
        format!("{m}+0x{off:x}")
    }
}

/// `pmu-dev sym <hex offset>...` (was tools/analysis/sym.py): names for libunity.so offsets.
pub fn sym_run(args: &[String]) -> Result<u8, String> {
    let n = UnityNamer::new(false);
    for x in args {
        println!("{x}: {}", n.name(&format!("libunity.so+0x{}", x.to_lowercase().replace("0x", ""))));
    }
    Ok(0)
}
