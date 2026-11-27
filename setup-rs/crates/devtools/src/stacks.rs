//! `pmu-dev stacks <pid> [name-filter] [words]` (was tools/device/stacks.py; runs on the handheld:
//! the aarch64 musl build): what each thread of a running ua_host is blocked in. For every thread (or
//! the ones whose name contains the filter) prints the current syscall and the return-address-looking
//! words (32-bit) on its stack that fall in a mapped .so, as <module>+<offset> (feed the libunity
//! offsets to `pmu-dev sym` on the computer).
use std::collections::HashMap;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use regex::Regex;

pub fn run(args: &[String]) -> Result<u8, String> {
    let usage = "usage: pmu-dev stacks <pid> [name-filter] [words]";
    let pid: u32 = args.first().and_then(|p| p.parse().ok()).ok_or(usage)?;
    let filt = args.get(1).cloned().unwrap_or_default();
    let nwords: usize = args.get(2).and_then(|w| w.parse().ok()).unwrap_or(1024);
    let rx = Regex::new(r"^([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \d+\s*(.*)").unwrap();
    let maps_text = std::fs::read_to_string(format!("/proc/{pid}/maps")).map_err(|e| format!("/proc/{pid}/maps: {e}"))?;
    let mut maps: Vec<(u64, u64, u64, String)> = Vec::new();
    // module base = start of its first mapping (file offset 0)
    let mut bases: HashMap<String, u64> = HashMap::new();
    for l in maps_text.lines() {
        let Some(m) = rx.captures(l) else { continue };
        let h = |i: usize| u64::from_str_radix(&m[i], 16).unwrap_or(0);
        let path = m[5].to_string();
        if path.is_empty() {
            continue;
        }
        if m[3].contains('x') {
            maps.push((h(1), h(2), h(4), path.clone()));
        }
        if h(4) == 0 {
            bases.entry(path).or_insert(h(1));
        }
    }
    let where_ = |v: u64| -> Option<String> {
        let (s, _, off, path) = maps.iter().find(|(s, e, _, _)| *s <= v && v < *e)?;
        let base = bases.get(path).copied().unwrap_or(s.wrapping_sub(*off));
        let b = Path::new(path).file_name().map_or(path.clone(), |b| b.to_string_lossy().into_owned());
        Some(format!("{b}+0x{:x}", v.wrapping_sub(base)))
    };
    let mut mem = std::fs::File::open(format!("/proc/{pid}/mem")).map_err(|e| format!("/proc/{pid}/mem: {e}"))?;
    let mut tids: Vec<u64> = std::fs::read_dir(format!("/proc/{pid}/task"))
        .map_err(|e| e.to_string())?
        .filter_map(|e| e.ok()?.file_name().to_str()?.parse().ok())
        .collect();
    tids.sort();
    for t in tids {
        let name = std::fs::read_to_string(format!("/proc/{pid}/task/{t}/comm")).unwrap_or_default().trim().to_string();
        if !filt.is_empty() && !name.contains(&filt) {
            continue;
        }
        let sc_text = std::fs::read_to_string(format!("/proc/{pid}/task/{t}/syscall")).unwrap_or_default();
        let sc: Vec<&str> = sc_text.split_whitespace().collect();
        println!("== {t} {name}: syscall {}", sc[..sc.len().min(4)].join(" "));
        if sc.len() < 3 || sc[0] == "running" {
            continue;
        }
        let h = |s: &str| u64::from_str_radix(s.trim_start_matches("0x"), 16).unwrap_or(0);
        let (sp, pc) = (h(sc[sc.len() - 2]), h(sc[sc.len() - 1]));
        println!("   pc {}", where_(pc).unwrap_or_else(|| format!("{pc:#x}")));
        let mut data = vec![0u8; nwords * 4];
        let n = match mem.seek(SeekFrom::Start(sp)).and_then(|_| mem.read(&mut data)) {
            Ok(n) => n,
            Err(e) => {
                println!("   stack: {e}");
                continue;
            }
        };
        let mut shown = 0;
        for i in 0..n / 4 {
            let v = u32::from_le_bytes(data[i * 4..i * 4 + 4].try_into().unwrap()) as u64;
            if v <= 0x10000 {
                continue;
            }
            if let Some(w) = where_(v & !1).filter(|w| !w.ends_with("+0x0")) {
                println!("   sp+{:#06x} {w}", i * 4);
                shown += 1;
                if shown >= 24 {
                    break;
                }
            }
        }
    }
    Ok(0)
}
