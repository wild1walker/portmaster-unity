//! The player survey's code tools (were tools/re/jnitables.py, strrefs.py, jnibindings.py, dis.py and
//! survey_player.sh's inline string scans), through Capstone as the Python's were:
//!
//! `pmu-dev jni-tables LIB.so [LIB.sym.so]`: JNINativeMethod tables {name, signature, function} in an
//! ARM32 library's data sections.
//! `pmu-dev strrefs LIB.so LIB.sym.so [FILTER_REGEX]`: per function of an ARM32 (ARM or Thumb-2, PIC)
//! library the C strings it references (ldr literal + add pc, movw/movt + add pc, adr / add pc, #imm;
//! literal-pool strings in .text); only functions with a string matching the filter.
//! `pmu-dev jni-bindings STRREFS.txt OUT_bindings.tsv OUT_direct.txt`: strrefs' output as the Java
//! members libunity's generated bindings use, and the other functions using JNI names.
//! `pmu-dev dis LIB.so SYM.so NAME_REGEX [MAX]` (or `str:STRING_REGEX`: AArch64 functions whose ADRP+ADD
//! string references match): the functions disassembled, literals and calls named.
//! `pmu-dev strings FILE REGEX [--nul] [--sort]`: printable runs of 5 - 300 characters matching REGEX,
//! each once (--nul: whole NUL-bounded strings that match REGEX).
use std::collections::BTreeSet;

use capstone::prelude::*;
use regex::Regex;

use crate::elf::{u32le, Elf};
use crate::util::bisect_right;

fn rx(s: &str) -> Result<Regex, String> {
    Regex::new(s).map_err(|e| format!("{s}: {e}"))
}

/// Python's int(s, 0) of capstone's immediates ("0x1f", "-0x8", "12", "-4")
fn int0(s: &str) -> Option<i64> {
    let (neg, t) = s.strip_prefix('-').map_or((false, s), |t| (true, t));
    let v = match t.strip_prefix("0x") {
        Some(h) => i64::from_str_radix(h, 16).ok()?,
        None => t.parse::<i64>().ok()?,
    };
    Some(if neg { -v } else { v })
}

fn sec_bytes<'a>(e: &'a Elf, name: &str) -> Result<(u64, &'a [u8]), String> {
    let s = e.section(name).ok_or(format!("no {name} section"))?;
    Ok((s.addr, e.sec_data(&s)))
}

fn cs_arm(thumb: bool) -> Result<Capstone, String> {
    Capstone::new()
        .arm()
        .mode(if thumb { arch::arm::ArchMode::Thumb } else { arch::arm::ArchMode::Arm })
        .build()
        .map_err(|e| e.to_string())
}

fn cs_a64() -> Result<Capstone, String> {
    Capstone::new().arm64().mode(arch::arm64::ArchMode::Arm).build().map_err(|e| e.to_string())
}

/// (address, size, mnemonic, operands) of code at addr, up to the first undecodable instruction
fn disasm(cs: &Capstone, code: &[u8], addr: u64) -> Vec<(u64, usize, String, String)> {
    match cs.disasm_all(code, addr) {
        Ok(is) => is.iter().map(|i| (i.address(), i.bytes().len(), i.mnemonic().unwrap_or("").to_string(), i.op_str().unwrap_or("").to_string())).collect(),
        Err(_) => vec![],
    }
}

// ---- jni-tables

pub fn jnitables_run(args: &[String]) -> Result<u8, String> {
    let lib = args.first().ok_or("usage: pmu-dev jni-tables LIB.so [LIB.sym.so]")?;
    let e = Elf::open(lib)?;
    let (ro_lo, rodata) = sec_bytes(&e, ".rodata")?;
    let ro_hi = ro_lo + rodata.len() as u64;
    let tx = e.section(".text").ok_or("no .text section")?;
    let (tx_lo, tx_hi) = (tx.addr, tx.addr + tx.size);
    let cstr = |addr: u64| -> Option<String> {
        if !(ro_lo <= addr && addr < ro_hi) {
            return None;
        }
        let o = (addr - ro_lo) as usize;
        let end = rodata[o..].iter().position(|&c| c == 0).map_or(rodata.len().saturating_sub(1).max(o), |p| o + p);
        let b = &rodata[o..end];
        (b.is_ascii()).then(|| String::from_utf8_lossy(b).into_owned())
    };
    let mut names: Vec<(u64, String)> = Vec::new();
    if let Some(sf) = args.get(1) {
        for s in Elf::open(sf)?.symbols() {
            if s.info & 15 == 2 && s.value != 0 {
                names.push((s.value & !1, s.name));
            }
        }
        names.sort();
    }
    let addrs: Vec<u64> = names.iter().map(|n| n.0).collect();
    let fname = |a: u64| -> String {
        let i = bisect_right(&addrs, &(a & !1)) as i64 - 1;
        if i >= 0 && addrs[i as usize] == (a & !1) {
            names[i as usize].1.clone()
        } else {
            "?".into()
        }
    };
    let sigre = rx(r"^\(([ZBCSIJFDV]|L[\w/$]+;|\[)*\)([ZBCSIJFDV]|L[\w/$]+;|\[+([ZBCSIJFD]|L[\w/$]+;))$")?;
    let ok = |n: u32, g: u32, f: u32| -> Option<(String, String)> {
        let (nm, sg) = (cstr(n as u64)?, cstr(g as u64)?);
        (!nm.is_empty() && !sg.is_empty() && sigre.is_match(&sg) && tx_lo <= (f & !1) as u64 && ((f & !1) as u64) < tx_hi).then_some((nm, sg))
    };
    for sname in [".data", ".data.rel.ro", ".data.rel.ro.local"] {
        let Some(s) = e.section(sname) else { continue };
        let d = e.sec_data(&s);
        let base = s.addr;
        let mut i = 0;
        while i + 12 <= d.len() {
            let (n, g, f) = (u32le(d, i), u32le(d, i + 4), u32le(d, i + 8));
            if ok(n, g, f).is_some() {
                let start = i;
                let mut rows = vec![];
                while i + 12 <= d.len() {
                    let (n, g, f) = (u32le(d, i), u32le(d, i + 4), u32le(d, i + 8));
                    let Some((nm, sg)) = ok(n, g, f) else { break };
                    rows.push((nm, sg, f));
                    i += 12;
                }
                println!("TABLE @{:#x} ({sname}) entries={}", base + start as u64, rows.len());
                for (nm, sg, f) in rows {
                    println!("  {nm:<36} {sg:<60} 0x{f:07x} {}", fname(f as u64));
                }
                continue;
            }
            i += 4;
        }
    }
    Ok(0)
}

// ---- strrefs

/// Python's repr() of a printable ASCII string
pub fn pyrepr(s: &str) -> String {
    if s.contains('\'') && !s.contains('"') {
        format!("\"{}\"", s.replace('\\', "\\\\"))
    } else {
        format!("'{}'", s.replace('\\', "\\\\").replace('\'', "\\'"))
    }
}

/// ast.literal_eval of pyrepr's output (None: anything else)
pub fn unrepr(s: &str) -> Option<String> {
    let q = s.chars().next()?;
    if (q != '\'' && q != '"') || s.len() < 2 || !s.ends_with(q) {
        return None;
    }
    let body = &s[1..s.len() - 1];
    let mut out = String::new();
    let mut it = body.chars();
    while let Some(c) = it.next() {
        if c == '\\' {
            match it.next()? {
                '\\' => out.push('\\'),
                '\'' => out.push('\''),
                '"' => out.push('"'),
                'n' => out.push('\n'),
                't' => out.push('\t'),
                _ => return None,
            }
        } else if c == q {
            return None;
        } else {
            out.push(c);
        }
    }
    Some(out)
}

pub fn strrefs_run(args: &[String]) -> Result<u8, String> {
    let (Some(lib), Some(symf)) = (args.first(), args.get(1)) else { return Err("usage: pmu-dev strrefs LIB.so LIB.sym.so [FILTER_REGEX]".into()) };
    let filt = match args.get(2) {
        Some(f) if !f.starts_with("--") => Some(rx(f)?),
        _ => None,
    };
    let e = Elf::open(lib)?;
    let (tbase, tdata) = sec_bytes(&e, ".text")?;
    let (ro_lo, rodata) = sec_bytes(&e, ".rodata")?;
    let ro_hi = ro_lo + rodata.len() as u64;
    let tend = tbase + tdata.len() as u64;
    let word = |a: i64| -> Option<i64> { (tbase as i64 <= a && a < tend as i64 - 3).then(|| u32le(tdata, (a - tbase as i64) as usize) as i64) };
    let textstr = regex::bytes::Regex::new(r"^[A-Za-z0-9_$<>/;()\[.]{2,}$").unwrap();
    let cstr = |addr: i64| -> Option<String> {
        if addr < 0 {
            return None;
        }
        let addr = addr as u64;
        let (data, o, in_text) = if ro_lo <= addr && addr < ro_hi {
            (rodata, (addr - ro_lo) as usize, false)
        } else if tbase <= addr && addr < tend {
            (tdata, (addr - tbase) as usize, true)
        } else {
            return None;
        };
        let end = data[o..(o + 400).min(data.len())].iter().position(|&c| c == 0)? + o;
        let s = &data[o..end];
        if s.is_empty() || s.iter().any(|&c| !(0x20..=0x7e).contains(&c)) {
            return None;
        }
        if in_text && !textstr.is_match(s) {
            return None;
        }
        Some(String::from_utf8_lossy(s).into_owned())
    };
    let mut set = BTreeSet::new();
    for s in Elf::open(symf)?.symbols() {
        if s.info & 15 == 2 && s.size > 0 {
            set.insert((s.value & !1, s.size, s.name, s.value & 1));
        }
    }
    let funcs: Vec<(u64, u64, String, u64)> = set.into_iter().collect();
    let dem = crate::syms::demangle(&funcs.iter().map(|f| f.2.clone()).collect::<Vec<_>>());
    let (arm, thumb) = (cs_arm(false)?, cs_arm(true)?);
    let r = r"(r\d+|ip|lr|sb|sl|fp)";
    let ldr_re = rx(&format!(r"^{r}, \[pc, #(-?0x[0-9a-f]+|-?\d+)\]$"))?;
    let addpc_re = rx(&format!(r"^{r}, pc, {r}$"))?;
    let addpc2_re = rx(&format!(r"^{r}, {r}, pc$"))?;
    let addpc1_re = rx(&format!(r"^{r}, pc$"))?;
    let add3_re = rx(r"^(\w+), (\w+), (\w+)$")?;
    let mov_re = rx(&format!(r"^{r}, #(0x[0-9a-f]+|\d+)$"))?;
    let addimm_re = rx(&format!(r"^{r}, pc, #(-?0x[0-9a-f]+|-?\d+)$"))?;
    let adr_re = rx(&format!(r"^{r}, #(-?0x[0-9a-f]+|-?\d+)$"))?;
    let mask = 0xffffffffi64;
    for (idx, (addr, size, name, th)) in funcs.iter().enumerate() {
        if !(tbase <= *addr && *addr < tend) {
            continue;
        }
        let is_thumb = *th != 0;
        let md = if is_thumb { &thumb } else { &arm };
        let o = (addr - tbase) as usize;
        let code = &tdata[o..(o + *size as usize).min(tdata.len())];
        let mut regs: std::collections::HashMap<String, i64> = Default::default();
        let mut absr: std::collections::HashMap<String, i64> = Default::default();
        let mut refs: Vec<String> = vec![];
        let step = if is_thumb { 2 } else { 4 };
        let mut off = 0usize;
        let mut ins = Vec::new();
        while off < code.len() {
            let got = disasm(md, &code[off..], addr + off as u64);
            let n: usize = got.iter().map(|i| i.1).sum();
            ins.extend(got);
            off += n;
            if off < code.len() {
                off += step; // an undecodable word (literal pool data): skipped
            }
        }
        for (a, _, mn, ops) in ins {
            let a = a as i64;
            if mn == "ldr" || mn == "ldr.w" {
                if let Some(m) = ldr_re.captures(&ops) {
                    let pcv = (a + if is_thumb { 4 } else { 8 }) & !3;
                    if let Some(w) = word(pcv + int0(&m[2]).unwrap_or(0)) {
                        regs.insert(m[1].to_string(), w);
                    }
                    continue;
                }
            }
            if mn == "movw" || mn == "movw.w" {
                if let Some(m) = mov_re.captures(&ops) {
                    regs.insert(m[1].to_string(), int0(&m[2]).unwrap_or(0));
                    continue;
                }
            }
            if mn == "movt" || mn == "movt.w" {
                if let Some(m) = mov_re.captures(&ops) {
                    if let Some(v) = regs.get(&m[1]).copied() {
                        regs.insert(m[1].to_string(), (v & 0xffff) | (int0(&m[2]).unwrap_or(0) << 16));
                        continue;
                    }
                }
            }
            if mn == "adr" || mn == "adr.w" || (matches!(mn.as_str(), "add" | "addw" | "add.w") && addimm_re.is_match(&ops)) {
                let m = if mn.starts_with("adr") { adr_re.captures(&ops) } else { addimm_re.captures(&ops) };
                if let Some(m) = m {
                    let pcv = if is_thumb { (a + 4) & !3 } else { a + 8 };
                    let v = (pcv + int0(&m[2]).unwrap_or(0)) & mask;
                    absr.insert(m[1].to_string(), v);
                    if let Some(st) = cstr(v) {
                        refs.push(st);
                    }
                    continue;
                }
            }
            if mn == "add" {
                let pcv = a + if is_thumb { 4 } else { 8 };
                let m = addpc_re.captures(&ops).or_else(|| addpc2_re.captures(&ops));
                let m1 = if m.is_none() { addpc1_re.captures(&ops) } else { None };
                if m.is_some() || m1.is_some() {
                    let (dst, src) = match (&m, &m1) {
                        (Some(m), _) => (m[1].to_string(), m[2].to_string()),
                        (_, Some(m1)) => (m1[1].to_string(), m1[1].to_string()),
                        _ => unreachable!(),
                    };
                    if let Some(rv) = regs.get(&src).copied() {
                        let v = (rv + pcv) & mask;
                        absr.insert(dst, v);
                        if let Some(st) = cstr(v) {
                            refs.push(st);
                        }
                    }
                    continue;
                }
                if let Some(m) = add3_re.captures(&ops) {
                    let (x, y) = (&m[2], &m[3]);
                    let v = match (absr.get(x), regs.get(y), absr.get(y), regs.get(x)) {
                        (Some(ax), Some(ry), _, _) => Some(ax + ry),
                        (_, _, Some(ay), Some(rx_)) => Some(ay + rx_),
                        _ => None,
                    };
                    if let Some(v) = v {
                        if let Some(st) = cstr(v & mask) {
                            refs.push(st);
                        }
                    }
                    continue;
                }
            }
        }
        if refs.is_empty() {
            continue;
        }
        if let Some(f) = &filt {
            if !refs.iter().any(|r| f.is_match(r)) {
                continue;
            }
        }
        println!("FUNC 0x{addr:07x} {size:5} {}", dem.get(idx).unwrap_or(name));
        let mut seen: Vec<&String> = vec![];
        for r in &refs {
            if !seen.contains(&r) {
                seen.push(r);
            }
        }
        for r in seen {
            println!("    {}", pyrepr(r));
        }
    }
    Ok(0)
}

// ---- jni-bindings

pub fn jnibindings_run(args: &[String]) -> Result<u8, String> {
    let [src, out_b, out_d] = args else { return Err("usage: pmu-dev jni-bindings STRREFS.txt OUT_bindings.tsv OUT_direct.txt".into()) };
    let text = std::fs::read_to_string(src).map_err(|e| format!("{src}: {e}"))?;
    let mut blocks: Vec<(String, String, Vec<String>)> = vec![];
    for line in text.lines() {
        if line.starts_with("FUNC ") {
            // (Python's split(None, 3): runs of blanks are one separator, the 4th field the rest)
            let mut r = line;
            let mut fields = vec![];
            for _ in 0..3 {
                r = r.trim_start();
                let i = r.find(char::is_whitespace).unwrap_or(r.len());
                fields.push(&r[..i]);
                r = &r[i..];
            }
            blocks.push((fields[1].to_string(), r.trim_start().to_string(), vec![]));
        } else if line.starts_with("    ") {
            if let (Some(b), Some(v)) = (blocks.last_mut(), unrepr(line.trim())) {
                b.2.push(v);
            }
        }
    }
    let sig = rx(r"^(\((\[*([ZBCSIJFD]|L[\w/$]+;))*\)(V|\[*([ZBCSIJFD]|L[\w/$]+;))|\[*([ZBCSIJFD]|L[\w/$]+;))$")?;
    let cls = rx(r"^(android|java|javax|dalvik|com|org|bitter)/[\w/$]+$")?;
    let ns = rx(r"^(?:\w+ )*((android|java|javax|dalvik|com|org|bitter)::[\w:]+)::(\w+)\(")?;
    let upper = rx(r"^[A-Z0-9_]+$")?;
    let member_rx = rx(r"^[a-z<][\w<>]*$")?;
    let mut rows: Vec<(String, String, String, String, String, String)> = vec![];
    let mut direct: Vec<(String, String, Vec<String>)> = vec![];
    for (addr, name, strs) in &blocks {
        if let Some(m) = ns.captures(name) {
            let (cpp_cls, member) = (m[1].to_string(), m[3].to_string());
            let jcls = cpp_cls.replace("::", "/");
            let mut parts: Vec<String> = jcls.split('/').map(String::from).collect();
            let last = parts.pop().unwrap_or_default();
            let last = if last.contains('_') && last.chars().next().is_some_and(|c| c.is_uppercase()) { last.replace('_', "$") } else { last };
            parts.push(last);
            let jcls = parts.join("/");
            let sigs: Vec<&String> = strs.iter().filter(|s| sig.is_match(s)).collect();
            let names: Vec<&String> = strs.iter().filter(|s| !sig.is_match(s) && !cls.is_match(s)).collect();
            if sigs.is_empty() {
                continue;
            }
            let mut kind = "method";
            let mc: Vec<char> = member.chars().collect();
            if member.starts_with('f') && mc.len() > 1 && (mc[1].is_uppercase() || mc[1] == '_') && !sigs[0].starts_with('(') {
                kind = if upper.is_match(&member[1..]) { "static-field" } else { "field" };
            }
            if member == "__Constructor" {
                kind = "ctor";
            }
            for (i, sg) in sigs.iter().enumerate() {
                let mut nm = names.get(i).map(|s| s.to_string()).unwrap_or_else(|| if kind == "ctor" { "<init>".into() } else { "?".into() });
                if kind == "ctor" {
                    nm = "<init>".into();
                }
                if !sg.starts_with('(') && kind == "method" {
                    kind = "field";
                }
                rows.push((jcls.clone(), nm, sg.to_string(), kind.to_string(), format!("{cpp_cls}::{member}"), addr.clone()));
            }
            continue;
        }
        if strs.iter().any(|s| sig.is_match(s) && s.starts_with('(')) || strs.iter().any(|s| cls.is_match(s)) {
            let j: Vec<String> = strs.iter().filter(|s| sig.is_match(s) || cls.is_match(s) || member_rx.is_match(s)).cloned().collect();
            direct.push((addr.clone(), name.clone(), j));
        }
    }
    let classes: BTreeSet<&String> = rows.iter().map(|r| &r.0).collect();
    let nclasses = classes.len();
    let uniq: BTreeSet<_> = rows.iter().cloned().collect();
    let mut o = String::from("# java_class\tmember\tsignature\tkind\tcpp_binding\taddr\n");
    for r in &uniq {
        o += &format!("{}\t{}\t{}\t{}\t{}\t{}\n", r.0, r.1, r.2, r.3, r.4, r.5);
    }
    std::fs::write(out_b, o).map_err(|e| e.to_string())?;
    let mut d = String::new();
    for (a, n, s) in &direct {
        d += &format!("{a} {n}\n    {}\n", s.join(" | "));
    }
    std::fs::write(out_d, d).map_err(|e| e.to_string())?;
    println!("{} binding members; {nclasses} classes; {} direct users", uniq.len(), direct.len());
    Ok(0)
}

// ---- dis

pub fn dis_run(args: &[String]) -> Result<u8, String> {
    let (Some(lib), Some(symf), Some(pat)) = (args.first(), args.get(1), args.get(2)) else {
        return Err("usage: pmu-dev dis LIB.so SYM.so NAME_REGEX|str:STRING_REGEX [MAX_FUNCS]".into());
    };
    let by_str = pat.starts_with("str:");
    let re = rx(if by_str { &pat[4..] } else { pat })?;
    let maxf: usize = args.get(3).and_then(|m| m.parse().ok()).unwrap_or(5);
    let e = Elf::open(lib)?;
    let a64 = e.machine == 183;
    let (tbase, tdata) = sec_bytes(&e, ".text")?;
    let tend = tbase + tdata.len() as u64;
    let loads: Vec<_> = e.phdrs.iter().filter(|p| p.typ == 1).copied().collect();
    let cstr = |addr: u64| -> Option<String> {
        for p in &loads {
            if p.va <= addr && addr < p.va + p.filesz {
                let o = (p.off + addr - p.va) as usize;
                let end = e.d[o..(o + 200).min(e.d.len())].iter().position(|&c| c == 0)? + o;
                let s = &e.d[o..end];
                return (end > o && s.iter().all(|&c| (0x20..0x7f).contains(&c))).then(|| String::from_utf8_lossy(s).into_owned());
            }
        }
        None
    };
    let funcs: Vec<(u64, u64, String)> = Elf::open(symf)?.symbols().into_iter().filter(|s| s.info & 15 == 2 && s.size > 0).map(|s| (s.value, s.size, s.name)).collect();
    let dem = crate::syms::demangle(&funcs.iter().map(|f| f.2.clone()).collect::<Vec<_>>());
    let mut fsorted: Vec<(u64, String)> = funcs.iter().zip(&dem).map(|(f, d)| (f.0 & !1, d.clone())).collect();
    fsorted.sort();
    let faddrs: Vec<u64> = fsorted.iter().map(|f| f.0).collect();
    let fname = |t: u64| -> Option<String> {
        let i = bisect_right(&faddrs, &t) as i64 - 1;
        (i >= 0 && faddrs[i as usize] == t).then(|| fsorted[i as usize].1.chars().take(100).collect())
    };
    let ldr = rx(r"^(\w+), \[pc, #(-?0x[0-9a-f]+|-?\d+)\]")?;
    let adrp_re = rx(r"^(\w+), #(0x[0-9a-f]+)")?;
    let add_re = rx(r"^(\w+), (\w+), #(0x[0-9a-f]+|\d+)")?;
    let (arm, thumb, aa64) = (cs_arm(false)?, cs_arm(true)?, cs_a64()?);
    let mut n = 0;
    for ((addr, size, name), d) in funcs.iter().zip(&dem) {
        if !by_str && !(re.is_match(d) || re.is_match(name)) {
            continue;
        }
        let th = !a64 && addr & 1 != 0;
        let addr = addr & !1;
        if !(tbase <= addr && addr < tend) {
            continue;
        }
        let md = if a64 { &aa64 } else if th { &thumb } else { &arm };
        let mut lines = vec![format!("FUNC {addr:#x} {size} {d}{}", if th { " [thumb]" } else { "" })];
        let o = (addr - tbase) as usize;
        let code = &tdata[o..(o + *size as usize).min(tdata.len())];
        let mut adrp: std::collections::HashMap<String, i64> = Default::default();
        let mut hit = !by_str;
        for (ia, _, mn, ops) in disasm(md, code, addr) {
            let mut note = String::new();
            if !a64 && mn.starts_with("ldr") {
                if let Some(m) = ldr.captures(&ops) {
                    let pc = ((ia as i64 + if th { 4 } else { 8 }) & !3) + int0(&m[2]).unwrap_or(0);
                    if tbase as i64 <= pc && pc < tend as i64 - 3 {
                        note = format!(" ; ={:#x}", u32le(tdata, (pc - tbase as i64) as usize));
                    }
                }
            }
            if a64 && mn == "adrp" {
                if let Some(m) = adrp_re.captures(&ops) {
                    adrp.insert(m[1].to_string(), int0(&m[2]).unwrap_or(0));
                }
            }
            if a64 && mn == "add" {
                if let Some(m) = add_re.captures(&ops) {
                    if let Some(b) = adrp.get(&m[2]) {
                        let v = b + int0(&m[3]).unwrap_or(0);
                        let st = if v >= 0 { cstr(v as u64) } else { None };
                        note = format!(" ; {v:#x}{}", st.as_ref().map_or(String::new(), |s| format!(" \"{s}\"")));
                        if by_str && st.as_ref().is_some_and(|s| re.is_match(s)) {
                            hit = true;
                        }
                    }
                }
            }
            if matches!(mn.as_str(), "bl" | "blx" | "b" | "b.w" | "bl.w") && ops.starts_with('#') {
                let t = ops[1..].trim_start_matches("0x");
                if let Some(f) = u64::from_str_radix(t, 16).ok().and_then(fname) {
                    note = format!(" ; {f}");
                }
            }
            lines.push(format!("  {ia:x}: {mn:<8} {ops}{note}"));
        }
        if !hit {
            continue;
        }
        println!("{}", lines.join("\n"));
        n += 1;
        if n >= maxf {
            break;
        }
    }
    Ok(0)
}

// ---- strings

pub fn strings_run(args: &[String]) -> Result<u8, String> {
    let (mut nul, mut sort) = (false, false);
    let mut pos = vec![];
    for a in args {
        match a.as_str() {
            "--nul" => nul = true,
            "--sort" => sort = true,
            _ => pos.push(a),
        }
    }
    let [file, pat] = &pos[..] else { return Err("usage: pmu-dev strings FILE REGEX [--nul] [--sort]".into()) };
    let data = std::fs::read(file).map_err(|e| format!("{file}: {e}"))?;
    let keep = rx(pat)?;
    let mut found: Vec<String> = vec![];
    if nul {
        let parts: Vec<&[u8]> = data.split(|&c| c == 0).collect();
        // (bounded by a NUL on both sides: not the file's first or last piece)
        for p in parts.iter().skip(1).take(parts.len().saturating_sub(2)) {
            if let Ok(s) = std::str::from_utf8(p) {
                if !s.is_empty() && keep.find(s).is_some_and(|m| m.start() == 0 && m.end() == s.len()) {
                    found.push(s.to_string());
                }
            }
        }
    } else {
        let mut i = 0;
        while i < data.len() {
            if !(0x20..=0x7e).contains(&data[i]) {
                i += 1;
                continue;
            }
            let j = i + data[i..].iter().position(|c| !(0x20..=0x7e).contains(c)).unwrap_or(data.len() - i);
            let mut k = i;
            while j - k >= 5 {
                let e = (k + 300).min(j);
                let s = String::from_utf8_lossy(&data[k..e]).into_owned();
                if keep.is_match(&s) {
                    found.push(s);
                }
                k = e;
            }
            i = j;
        }
    }
    let mut out: Vec<String> = vec![];
    let mut seen = std::collections::HashSet::new();
    for s in found {
        if seen.insert(s.clone()) {
            out.push(s);
        }
    }
    if sort {
        out.sort();
    }
    for s in out {
        println!("{s}");
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reprs() {
        for s in ["abc", "a'b", "a\"b", "a'\"b", "back\\slash", "(Ljava/lang/String;)V"] {
            assert_eq!(unrepr(&pyrepr(s)).as_deref(), Some(s));
        }
        assert_eq!(pyrepr("a'b"), "\"a'b\"");
        assert_eq!(pyrepr("a'\"b"), "'a\\'\"b'");
        assert_eq!(int0("-0x10"), Some(-16));
        assert_eq!(int0("12"), Some(12));
    }

    #[test]
    fn thumb_and_a64_decode() {
        // Thumb: ldr r0, [pc, #8]; add r0, pc ; AArch64: adrp x0, #0x1000 at 0
        let t = disasm(&cs_arm(true).unwrap(), &[0x02, 0x48, 0x78, 0x44], 0x1000);
        assert_eq!(t[0].2, "ldr");
        assert!(t[0].3.starts_with("r0, [pc, #"), "{:?}", t);
        assert_eq!((t[1].2.as_str(), t[1].3.as_str()), ("add", "r0, pc"));
        let a = disasm(&cs_a64().unwrap(), &0x90000000u32.to_le_bytes(), 0);
        assert_eq!(a[0].2, "adrp");
    }
}
