//! hotlist <ua_host.debug> <prof-N.txt>...: runtime/host/hot-arm64.txt, the functions of ua_host (its own objects and
//! the loader, shims and fake JNI archives linked into it) that UA_PROF recordings sampled on UnityMain, Unity's render
//! thread (UnityGfxDevice*) and ua_host's GL thread (ua_glthread), heaviest first, as many as cover 99.5 % of the
//! weight. Each recording's samples are scaled to the same total per thread, so every recording and thread counts
//! alike. The function names come from the ELF symbol table of the ua_host the recordings ran (its ua_host.debug).
//! runtime/host/build.sh (ARCH=arm64) compiles ua_host with one section a function and renames these .text.hot.<name>:
//! the linker places them together before the rest of the code (docs/porting.md, docs/arm64-routes.md).
use std::collections::HashMap;
use std::process::exit;

struct Sym {
    addr: u64,
    size: u64,
    name: String,
}

fn rd16(d: &[u8], o: usize) -> u16 { u16::from_le_bytes(d[o..o + 2].try_into().unwrap()) }
fn rd32(d: &[u8], o: usize) -> u32 { u32::from_le_bytes(d[o..o + 4].try_into().unwrap()) }
fn rd64(d: &[u8], o: usize) -> u64 { u64::from_le_bytes(d[o..o + 8].try_into().unwrap()) }

/// The functions (STT_FUNC with a size) of a 64-bit little-endian ELF file's .symtab, by address.
fn functions(path: &str) -> Result<Vec<Sym>, String> {
    let d = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
    if d.len() < 64 || &d[..4] != b"\x7fELF" || d[4] != 2 || d[5] != 1 {
        return Err(format!("{path}: not a 64-bit little-endian ELF file"));
    }
    let shoff = rd64(&d, 0x28) as usize;
    let (shentsize, shnum) = (rd16(&d, 0x3a) as usize, rd16(&d, 0x3c) as usize);
    let sec = |i: usize| shoff + i * shentsize;
    let mut out = Vec::new();
    for i in 0..shnum {
        let s = sec(i);
        if s + 64 > d.len() || rd32(&d, s + 4) != 2 {
            continue; // SHT_SYMTAB only
        }
        let (off, size, link, ent) = (rd64(&d, s + 0x18) as usize, rd64(&d, s + 0x20) as usize, rd32(&d, s + 0x28) as usize, rd64(&d, s + 0x38) as usize);
        let st = sec(link);
        let (stroff, strsize) = (rd64(&d, st + 0x18) as usize, rd64(&d, st + 0x20) as usize);
        let strs = &d[stroff..stroff + strsize];
        let ent = if ent == 0 { 24 } else { ent };
        for k in 0..size / ent {
            let e = off + k * ent;
            let (name, info, value, sz) = (rd32(&d, e) as usize, d[e + 4], rd64(&d, e + 8), rd64(&d, e + 16));
            if info & 0xf != 2 || sz == 0 || name >= strs.len() {
                continue;
            }
            let end = strs[name..].iter().position(|&c| c == 0).map_or(strs.len(), |p| name + p);
            out.push(Sym { addr: value, size: sz, name: String::from_utf8_lossy(&strs[name..end]).into_owned() });
        }
    }
    if out.is_empty() {
        return Err(format!("{path}: no symbol table (give the unstripped ua_host.debug)"));
    }
    out.sort_by_key(|s| s.addr);
    Ok(out)
}

fn hex(s: &str) -> Option<u64> { u64::from_str_radix(s, 16).ok() }

fn wanted(thread: &str) -> bool { thread == "UnityMain" || thread.starts_with("UnityGfxDevice") || thread == "ua_glthread" }

/// A thread's samples by function, in the order first seen
type Counts = Vec<(String, Vec<(String, u64)>)>;

/// One recording: for each wanted thread, the samples whose pc falls in a ua_host function, by name.
fn recording(path: &str, syms: &[Sym]) -> Result<Counts, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{path}: {e}"))?;
    let mut module: Option<(u64, u64, u64)> = None;
    let mut names: HashMap<String, String> = HashMap::new();
    for l in text.lines() {
        let q: Vec<&str> = l.split_whitespace().collect();
        match q.first() {
            Some(&"module") if q.len() >= 5 && q[4] == "ua_host" && module.is_none() => {
                if let (Some(a), Some(b), Some(c)) = (hex(q[1]), hex(q[2]), hex(q[3])) {
                    module = Some((a, b, c));
                }
            }
            Some(&"thread") if q.len() >= 4 => {
                names.insert(q[1].to_string(), q[3].to_string());
            }
            _ => {}
        }
    }
    let (lo, hi, bias) = module.ok_or_else(|| format!("{path}: no ua_host module"))?;
    let starts: Vec<u64> = syms.iter().map(|s| s.addr).collect();
    // per thread, in order of first sample: name -> count (in order of first sample)
    let mut per: Counts = Vec::new();
    for l in text.lines() {
        if !l.starts_with("stack ") {
            continue;
        }
        let q: Vec<&str> = l.split_whitespace().collect();
        if q.len() < 7 || q[4] != "R" {
            continue;
        }
        let t = names.get(q[3]).map(String::as_str).unwrap_or("");
        if !wanted(t) {
            continue;
        }
        let (Ok(w), Some(pc)) = (q[2].parse::<u64>(), hex(q[6])) else { continue };
        if pc < lo || pc >= hi {
            continue;
        }
        let v = pc - bias;
        let i = starts.partition_point(|&s| s <= v);
        if i == 0 || v >= syms[i - 1].addr + syms[i - 1].size {
            continue;
        }
        let n = &syms[i - 1].name;
        let ti = match per.iter().position(|(x, _)| x == t) {
            Some(k) => k,
            None => { per.push((t.to_string(), Vec::new())); per.len() - 1 }
        };
        let c = &mut per[ti].1;
        match c.iter_mut().find(|(x, _)| x == n) {
            Some(e) => e.1 += w,
            None => c.push((n.clone(), w)),
        }
    }
    Ok(per)
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() < 2 || args[0] == "-h" || args[0] == "--help" {
        eprintln!("usage: hotlist <ua_host.debug> <prof-N.txt>... > runtime/host/hot-arm64.txt");
        exit(2);
    }
    let syms = functions(&args[0]).unwrap_or_else(|e| { eprintln!("hotlist: {e}"); exit(1) });
    let mut weight: Vec<(String, f64)> = Vec::new();
    for p in &args[1..] {
        let per = recording(p, &syms).unwrap_or_else(|e| { eprintln!("hotlist: {e}"); exit(1) });
        for (_, counts) in per {
            let total: u64 = counts.iter().map(|c| c.1).sum();
            for (n, w) in counts {
                let x = w as f64 / total as f64;
                match weight.iter_mut().find(|(m, _)| *m == n) {
                    Some(e) => e.1 += x,
                    None => weight.push((n, x)),
                }
            }
        }
    }
    let total: f64 = weight.iter().map(|w| w.1).sum();
    weight.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap()); // stable: ties keep the order first seen
    let mut out = Vec::new();
    let mut acc = 0.0;
    for (n, w) in &weight {
        out.push(n.as_str());
        acc += w;
        if acc >= 0.995 * total {
            break;
        }
    }
    let size: HashMap<&str, u64> = syms.iter().map(|s| (s.name.as_str(), s.size)).collect();
    println!("# runtime/host/hot-arm64.txt: made by tools/hotlist from {} recordings; {} functions, {} bytes",
             args.len() - 1, out.len(), out.iter().map(|n| size.get(n).copied().unwrap_or(0)).sum::<u64>());
    for n in out {
        println!("{n}");
    }
}
