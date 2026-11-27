//! The player survey's ELF tools (were tools/re/elfimage.py, elfimports.py; tools/re/survey_player.sh
//! runs them): Android shared objects of both ABIs (armeabi-v7a, arm64-v8a; x86 and x86_64 read too).
//!
//! `pmu-dev elf-facts LIB.so...`: per library its ELF facts as JSON (machine, relocation formats and
//! counts, notes, TLS, IFUNC exports, ...), as elfimage.py printed them.
//! `pmu-dev elf-imports OUTDIR LIB... [--glibc DIR]`: per library OUTDIR/imports_<name>.txt (each
//! undefined dynamic symbol: provider, name, type, bind, version, relocations, ABI notes) and
//! exports_<name>.txt, and a JSON summary on stdout. The providers are judged against armhf glibc's
//! exports (DIR, default /usr/arm-linux-gnueabihf/lib: Debian's libc6-armhf-cross).
pub mod arm;

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

use port::val::{Dict, Val};

use crate::elf::{u32le, u64le, Elf, Sec, Sym};

const DT_TABLE: &[(&str, u64)] = &[
    ("DT_NULL", 0), ("DT_NEEDED", 1), ("DT_PLTRELSZ", 2), ("DT_PLTGOT", 3), ("DT_HASH", 4), ("DT_STRTAB", 5),
    ("DT_SYMTAB", 6), ("DT_RELA", 7), ("DT_RELASZ", 8), ("DT_RELAENT", 9), ("DT_STRSZ", 10), ("DT_SYMENT", 11),
    ("DT_INIT", 12), ("DT_FINI", 13), ("DT_SONAME", 14), ("DT_RPATH", 15), ("DT_SYMBOLIC", 16), ("DT_REL", 17),
    ("DT_RELSZ", 18), ("DT_RELENT", 19), ("DT_PLTREL", 20), ("DT_DEBUG", 21), ("DT_TEXTREL", 22), ("DT_JMPREL", 23),
    ("DT_BIND_NOW", 24), ("DT_INIT_ARRAY", 25), ("DT_FINI_ARRAY", 26), ("DT_INIT_ARRAYSZ", 27),
    ("DT_FINI_ARRAYSZ", 28), ("DT_RUNPATH", 29), ("DT_FLAGS", 30), ("DT_PREINIT_ARRAY", 32),
    ("DT_PREINIT_ARRAYSZ", 33), ("DT_SYMTAB_SHNDX", 34), ("DT_RELRSZ", 35), ("DT_RELR", 36), ("DT_RELRENT", 37),
    ("DT_ANDROID_REL", 0x6000000f), ("DT_ANDROID_RELSZ", 0x60000010), ("DT_ANDROID_RELA", 0x60000011),
    ("DT_ANDROID_RELASZ", 0x60000012), ("DT_GNU_PRELINKED", 0x6ffffdf5), ("DT_GNU_CONFLICTSZ", 0x6ffffdf6),
    ("DT_GNU_LIBLISTSZ", 0x6ffffdf7), ("DT_CHECKSUM", 0x6ffffdf8), ("DT_PLTPADSZ", 0x6ffffdf9),
    ("DT_MOVEENT", 0x6ffffdfa), ("DT_MOVESZ", 0x6ffffdfb), ("DT_FEATURE_1", 0x6ffffdfc), ("DT_POSFLAG_1", 0x6ffffdfd),
    ("DT_SYMINSZ", 0x6ffffdfe), ("DT_SYMINENT", 0x6ffffdff), ("DT_ANDROID_RELR", 0x6fffe000),
    ("DT_ANDROID_RELRSZ", 0x6fffe001), ("DT_GNU_HASH", 0x6ffffef5), ("DT_TLSDESC_PLT", 0x6ffffef6),
    ("DT_TLSDESC_GOT", 0x6ffffef7), ("DT_GNU_CONFLICT", 0x6ffffef8), ("DT_GNU_LIBLIST", 0x6ffffef9),
    ("DT_CONFIG", 0x6ffffefa), ("DT_DEPAUDIT", 0x6ffffefb), ("DT_AUDIT", 0x6ffffefc), ("DT_PLTPAD", 0x6ffffefd),
    ("DT_MOVETAB", 0x6ffffefe), ("DT_SYMINFO", 0x6ffffeff), ("DT_VERSYM", 0x6ffffff0), ("DT_RELACOUNT", 0x6ffffff9),
    ("DT_RELCOUNT", 0x6ffffffa), ("DT_FLAGS_1", 0x6ffffffb), ("DT_VERDEF", 0x6ffffffc), ("DT_VERDEFNUM", 0x6ffffffd),
    ("DT_VERNEED", 0x6ffffffe), ("DT_VERNEEDNUM", 0x6fffffff), ("DT_AUXILIARY", 0x7ffffffd), ("DT_FILTER", 0x7fffffff),
];

fn dt(name: &str) -> u64 {
    DT_TABLE.iter().find(|t| t.0 == name).map(|t| t.1).unwrap()
}
fn dt_name(v: u64) -> Option<&'static str> {
    DT_TABLE.iter().find(|t| t.1 == v).map(|t| t.0)
}

pub fn machine_name(m: u16) -> String {
    match m {
        3 => "EM_386".into(),
        40 => "EM_ARM".into(),
        62 => "EM_X86_64".into(),
        183 => "EM_AARCH64".into(),
        x => format!("EM_{x}"),
    }
}

fn reloc_names(mach: u16) -> &'static [(u32, &'static str)] {
    match mach {
        40 => &[(0, "NONE"), (2, "ABS32"), (3, "REL32"), (17, "TLS_DTPMOD32"), (18, "TLS_DTPOFF32"), (19, "TLS_TPOFF32"), (20, "COPY"), (21, "GLOB_DAT"), (22, "JUMP_SLOT"), (23, "RELATIVE"), (160, "IRELATIVE")],
        183 => &[(0, "NONE"), (257, "ABS64"), (1024, "COPY"), (1025, "GLOB_DAT"), (1026, "JUMP_SLOT"), (1027, "RELATIVE"), (1028, "TLS_DTPMOD64"), (1029, "TLS_DTPREL64"), (1030, "TLS_TPREL64"), (1031, "TLSDESC"), (1032, "IRELATIVE")],
        3 => &[(1, "32"), (2, "PC32"), (6, "GLOB_DAT"), (7, "JMP_SLOT"), (8, "RELATIVE"), (14, "TLS_TPOFF"), (35, "TLS_DTPMOD32"), (36, "TLS_DTPOFF32"), (42, "IRELATIVE")],
        62 => &[(1, "64"), (6, "GLOB_DAT"), (7, "JUMP_SLOT"), (8, "RELATIVE"), (16, "DTPMOD64"), (17, "DTPOFF64"), (18, "TPOFF64"), (37, "IRELATIVE")],
        _ => &[],
    }
}

fn relative(mach: u16) -> u32 {
    match mach {
        40 => 23,
        183 => 1027,
        _ => 8,
    }
}

/// p_type as pyelftools names it (None: a number it does not know)
fn pt_name(t: u32, mach: u16) -> Option<&'static str> {
    Some(match t {
        0 => "PT_NULL",
        1 => "PT_LOAD",
        2 => "PT_DYNAMIC",
        3 => "PT_INTERP",
        4 => "PT_NOTE",
        5 => "PT_SHLIB",
        6 => "PT_PHDR",
        7 => "PT_TLS",
        0x6474e550 => "PT_GNU_EH_FRAME",
        0x6474e551 => "PT_GNU_STACK",
        0x6474e552 => "PT_GNU_RELRO",
        0x6474e553 => "PT_GNU_PROPERTY",
        0x70000001 if mach == 40 => "PT_ARM_EXIDX",
        0x70000000 if mach == 183 => "PT_AARCH64_ARCHEXT",
        0x70000001 if mach == 183 => "PT_AARCH64_UNWIND",
        _ => return None,
    })
}

fn pt_val(t: u32, mach: u16) -> Val {
    pt_name(t, mach).map_or(Val::Int(t as i64), |n| Val::Str(n.into()))
}

pub fn hex(v: u64) -> String {
    format!("{v:#x}")
}

fn s(x: &str) -> Val {
    Val::Str(x.into())
}
fn strs<I: IntoIterator<Item = String>>(v: I) -> Val {
    Val::List(v.into_iter().map(Val::Str).collect())
}

pub(crate) fn sleb(d: &[u8], mut p: usize) -> Result<(i128, usize), String> {
    let (mut r, mut sh) = (0i128, 0u32);
    loop {
        let b = *d.get(p).ok_or("packed relocations: truncated")? as i128;
        p += 1;
        if sh < 120 {
            r |= (b & 0x7f) << sh;
        }
        sh += 7;
        if b & 0x80 == 0 {
            if b & 0x40 != 0 && sh < 127 {
                r -= 1i128 << sh;
            }
            return Ok((r, p));
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Reloc {
    pub off: u64,
    pub typ: u32,
    pub sym: u64,
    pub add: Option<i64>,
    pub table: &'static str,
}

/// An ELF file as the Android linker sees it: its loaded segments, dynamic tags and every
/// relocation table (REL/RELA, PLT, packed APS2, RELR).
pub struct Image {
    pub path: String,
    pub e: Elf,
    pub mach: u16,
    pub ps: u64,
    loads: Vec<crate::elf::Phdr>,
    /// (tag, value) of the dynamic section, the first of each tag, DT_NEEDED left out
    pub tags: Vec<(u64, u64)>,
    pub needed: Vec<String>,
    rel: Option<Vec<Reloc>>,
    relmap: Option<HashMap<u64, (u32, Option<i64>)>>,
}

impl Image {
    pub fn open(path: &str) -> Result<Image, String> {
        let e = Elf::open(path)?;
        let mach = e.machine;
        let ps = if e.is64 { 8 } else { 4 };
        let loads: Vec<_> = e.phdrs.iter().copied().filter(|p| p.typ == 1).collect();
        let mut im = Image { path: path.into(), mach, ps, loads, tags: vec![], needed: vec![], rel: None, relmap: None, e };
        let (raw, strtab) = match im.e.section(".dynamic") {
            Some(sec) => {
                let st = im.e.sections().get(sec.link as usize).cloned();
                (im.e.sec_data(&sec).to_vec(), st.map(|t| t.off))
            }
            None => {
                let p = im.e.phdrs.iter().find(|p| p.typ == 2).copied().ok_or(format!("{path}: no dynamic section"))?;
                (im.e.d.get(p.off as usize..(p.off + p.filesz) as usize).unwrap_or(&[]).to_vec(), None)
            }
        };
        let ent = if im.e.is64 { 16 } else { 8 };
        let mut needed_off = Vec::new();
        for c in raw.chunks_exact(ent) {
            let (t, v) = if im.e.is64 { (u64le(c, 0), u64le(c, 8)) } else { (u32le(c, 0) as u64, u32le(c, 4) as u64) };
            if t == 1 {
                needed_off.push(v);
            } else if !im.tags.iter().any(|x| x.0 == t) {
                im.tags.push((t, v));
            }
            if t == 0 {
                break;
            }
        }
        let stroff = strtab.or_else(|| im.tag(5).and_then(|a| im.file_off(a)));
        if let Some(so) = stroff {
            im.needed = needed_off.iter().map(|o| im.e.str_at((so + o) as usize)).collect();
        }
        Ok(im)
    }

    pub fn tag(&self, t: u64) -> Option<u64> {
        self.tags.iter().find(|x| x.0 == t).map(|x| x.1)
    }

    fn file_off(&self, va: u64) -> Option<u64> {
        self.loads.iter().find(|s| s.va <= va && va < s.va + s.filesz).map(|s| s.off + va - s.va)
    }

    /// n bytes at a virtual address inside one loaded segment's file bytes
    pub fn read(&self, addr: u64, n: u64) -> Option<&[u8]> {
        for s in &self.loads {
            if s.va <= addr && addr + n <= s.va + s.filesz {
                let o = (s.off + addr - s.va) as usize;
                return self.e.d.get(o..o + n as usize);
            }
        }
        None
    }

    fn table(&self, at: u64, sz: u64, rela: bool, name: &'static str, out: &mut Vec<Reloc>) {
        let (Some(a), Some(n)) = (self.tag(at), self.tag(sz)) else { return };
        if n == 0 {
            return;
        }
        let data = self.read(a, n).unwrap_or(&[]);
        let ent = match (self.e.is64, rela) {
            (true, true) => 24,
            (true, false) => 16,
            (false, true) => 12,
            (false, false) => 8,
        };
        for c in data.chunks_exact(ent) {
            if self.e.is64 {
                let (off, info) = (u64le(c, 0), u64le(c, 8));
                let add = rela.then(|| u64le(c, 16) as i64);
                out.push(Reloc { off, typ: (info & 0xffffffff) as u32, sym: info >> 32, add, table: name });
            } else {
                let (off, info) = (u32le(c, 0) as u64, u32le(c, 4) as u64);
                let add = rela.then(|| u32le(c, 8) as i32 as i64);
                out.push(Reloc { off, typ: (info & 0xff) as u32, sym: info >> 8, add, table: name });
            }
        }
    }

    fn aps2(&self, at: u64, sz: u64, rela: bool, name: &'static str, out: &mut Vec<Reloc>) -> Result<(), String> {
        let Some(a) = self.tag(at) else { return Ok(()) };
        let data = self.read(a, self.tag(sz).unwrap_or(0)).unwrap_or(&[]);
        if data.get(..4) != Some(b"APS2") {
            return Err(format!("{}: packed relocations without APS2", self.path));
        }
        let mut p = 4;
        let (count, q) = sleb(data, p)?;
        let (mut off, q) = sleb(data, q)?;
        p = q;
        let (mut info, mut add) = (0i128, 0i128);
        let mut n = 0i128;
        while n < count {
            let (gsize, q) = sleb(data, p)?;
            let (flags, q) = sleb(data, q)?;
            p = q;
            let mut step = 0;
            if flags & 2 != 0 {
                (step, p) = sleb(data, p)?;
            }
            if flags & 1 != 0 {
                (info, p) = sleb(data, p)?;
            }
            if flags & 12 == 12 {
                let (d, q) = sleb(data, p)?;
                p = q;
                add += d;
            } else if flags & 8 == 0 {
                add = 0;
            }
            for _ in 0..gsize {
                if flags & 2 != 0 {
                    off += step;
                } else {
                    let (d, q) = sleb(data, p)?;
                    p = q;
                    off += d;
                }
                if flags & 1 == 0 {
                    (info, p) = sleb(data, p)?;
                }
                if flags & 12 == 8 {
                    let (d, q) = sleb(data, p)?;
                    p = q;
                    add += d;
                }
                let addv = rela.then_some(add as i64);
                if self.e.is64 {
                    out.push(Reloc { off: (off & 0xffff_ffff_ffff_ffff) as u64, typ: (info & 0xffffffff) as u32, sym: (info >> 32) as u64, add: addv, table: name });
                } else {
                    out.push(Reloc { off: (off & 0xffffffff) as u64, typ: (info & 0xff) as u32, sym: (info >> 8) as u64, add: addv, table: name });
                }
                n += 1;
            }
        }
        Ok(())
    }

    fn relr(&self, at: u64, sz: u64, name: &'static str, out: &mut Vec<Reloc>) {
        let Some(a) = self.tag(at) else { return };
        let data = self.read(a, self.tag(sz).unwrap_or(0)).unwrap_or(&[]);
        let (ps, bits) = (self.ps, self.ps * 8 - 1);
        let rel = relative(self.mach);
        let mut wh = 0u64;
        for c in data.chunks_exact(ps as usize) {
            let w = if self.e.is64 { u64le(c, 0) } else { u32le(c, 0) as u64 };
            if w & 1 == 0 {
                wh = w;
                out.push(Reloc { off: wh, typ: rel, sym: 0, add: None, table: name });
                wh = wh.wrapping_add(ps);
            } else {
                let b = w >> 1;
                for k in 0..bits {
                    if b & (1 << k) != 0 {
                        out.push(Reloc { off: wh.wrapping_add(k * ps), typ: rel, sym: 0, add: None, table: name });
                    }
                }
                wh = wh.wrapping_add(bits * ps);
            }
        }
    }

    pub fn relocs(&mut self) -> Result<&[Reloc], String> {
        if self.rel.is_none() {
            let mut out = Vec::new();
            self.aps2(dt("DT_ANDROID_REL"), dt("DT_ANDROID_RELSZ"), false, "android_rel", &mut out)?;
            self.aps2(dt("DT_ANDROID_RELA"), dt("DT_ANDROID_RELASZ"), true, "android_rela", &mut out)?;
            self.relr(dt("DT_RELR"), dt("DT_RELRSZ"), "relr", &mut out);
            self.relr(dt("DT_ANDROID_RELR"), dt("DT_ANDROID_RELRSZ"), "android_relr", &mut out);
            self.table(dt("DT_REL"), dt("DT_RELSZ"), false, "rel", &mut out);
            self.table(dt("DT_RELA"), dt("DT_RELASZ"), true, "rela", &mut out);
            let rela_plt = self.tag(dt("DT_PLTREL")) == Some(7);
            self.table(dt("DT_JMPREL"), dt("DT_PLTRELSZ"), rela_plt, "plt", &mut out);
            self.rel = Some(out);
        }
        Ok(self.rel.as_deref().unwrap())
    }

    /// The pointer at addr after relocation (load base 0); a symbol's relocation gives None.
    pub fn ptr(&mut self, addr: u64) -> Result<Option<u64>, String> {
        if self.relmap.is_none() {
            let m: HashMap<u64, (u32, Option<i64>)> = self.relocs()?.iter().map(|r| (r.off, (r.typ, r.add))).collect();
            self.relmap = Some(m);
        }
        let Some(b) = self.read(addr, self.ps) else { return Ok(None) };
        let v = if self.e.is64 { u64le(b, 0) } else { u32le(b, 0) as u64 };
        Ok(match self.relmap.as_ref().unwrap().get(&addr) {
            None => Some(v),
            Some((t, a)) if *t == relative(self.mach) => Some(a.map_or(v, |a| a as u64)),
            Some(_) => None,
        })
    }

    pub fn reloc_name(&self, t: u32) -> String {
        reloc_names(self.mach).iter().find(|x| x.0 == t).map_or(t.to_string(), |x| x.1.to_string())
    }

    fn dynsym(&self) -> Vec<Sym> {
        self.e.symbols().into_iter().filter(|s| s.table == ".dynsym").collect()
    }

    pub fn facts(&mut self) -> Result<Val, String> {
        let mut r = Dict::new();
        r.set("machine", s(&machine_name(self.mach)));
        r.set("class", Val::Int(if self.e.is64 { 64 } else { 32 }));
        r.set("needed", strs(self.needed.clone()));
        let mut names: Vec<String> = self.tags.iter().filter_map(|t| dt_name(t.0)).map(String::from).collect();
        names.sort();
        r.set("dyn_tags", strs(names));
        let rel: Vec<(String, String)> = self.relocs()?.iter().map(|x| (x.table.to_string(), x.typ)).collect::<Vec<_>>().into_iter().map(|(t, ty)| (t, self.reloc_name(ty))).collect();
        let mut c: BTreeMap<(String, String), i64> = BTreeMap::new();
        for k in &rel {
            *c.entry(k.clone()).or_default() += 1;
        }
        let mut rd = Dict::new();
        for ((t, n), v) in &c {
            rd.set(&format!("{t} {n}"), Val::Int(*v));
        }
        r.set("relocs", Val::Dict(rd));
        let formats: BTreeSet<String> = rel.iter().map(|x| x.0.clone()).collect();
        r.set("reloc_formats", strs(formats));
        r.set("phdrs", Val::List(self.e.phdrs.iter().map(|p| pt_val(p.typ, self.mach)).collect()));
        let al: BTreeSet<String> = self.loads.iter().map(|p| hex(p.align)).collect();
        r.set("load_align", strs(al));
        let mut fl = Dict::new();
        for n in ["DT_FLAGS", "DT_FLAGS_1", "DT_TEXTREL"] {
            if let Some(v) = self.tag(dt(n)) {
                fl.set(n, s(&hex(v)));
            }
        }
        r.set("flags", Val::Dict(fl));
        let h: Vec<String> = [("gnu", "DT_GNU_HASH"), ("sysv", "DT_HASH")].iter().filter(|x| self.tag(dt(x.1)).is_some()).map(|x| x.0.to_string()).collect();
        r.set("hash", strs(h));
        let mut tls = Val::Null;
        for p in &self.e.phdrs {
            if p.typ == 7 {
                let mut t = Dict::new();
                t.set("memsz", Val::Int(p.memsz as i64));
                t.set("align", Val::Int(p.align as i64));
                tls = Val::Dict(t);
            }
        }
        r.set("pt_tls", tls);
        let (mut ifunc, mut tlss, mut emu) = (vec![], vec![], vec![]);
        for sym in self.dynsym() {
            let t = sym.info & 15;
            if t == 10 && sym.shndx != 0 {
                ifunc.push(sym.name.clone());
            }
            if t == 6 {
                tlss.push(sym.name.clone());
            }
            if sym.name.starts_with("__emutls") {
                emu.push(format!("{}{}", sym.name, if sym.shndx == 0 { "(undef)" } else { "" }));
            }
        }
        r.set("ifunc_exports", strs(ifunc));
        r.set("tls_symbols", strs(tlss));
        r.set("emutls_symbols", strs(emu.into_iter().take(20)));
        for sec in self.e.sections() {
            if sec.typ != 7 {
                continue;
            }
            let d = self.e.sec_data(&sec);
            let mut p = 0;
            while p + 12 <= d.len() {
                let (nsz, dsz, typ) = (u32le(d, p) as usize, u32le(d, p + 4) as usize, u32le(d, p + 8));
                let name = &d[(p + 12).min(d.len())..(p + 12 + nsz).min(d.len())];
                let name: &[u8] = &name[..name.iter().rposition(|&c| c != 0).map_or(0, |i| i + 1)];
                let q = p + 12 + ((nsz + 3) & !3);
                let desc = &d[q.min(d.len())..(q + dsz).min(d.len())];
                p = q + ((dsz + 3) & !3);
                if name == b"Android" && typ == 1 && desc.len() >= 4 {
                    let field = |a: usize, b: usize| {
                        let f = &desc[a.min(desc.len())..b.min(desc.len())];
                        ascii_replace(f.split(|&c| c == 0).next().unwrap_or(&[]))
                    };
                    let mut a = Dict::new();
                    a.set("api", Val::Int(u32le(desc, 0) as i64));
                    a.set("ndk", s(&field(4, 68)));
                    a.set("ndk_build", s(&field(68, 132)));
                    r.set("android_ident", Val::Dict(a));
                } else if name == b"GNU" && typ == 5 {
                    r.set("gnu_property", s(&desc.iter().map(|b| format!("{b:02x}")).collect::<String>()));
                } else if name == b"GNU" && typ == 3 {
                    r.set("build_id", Val::Bool(true));
                }
            }
        }
        if let Some(cm) = self.e.section(".comment") {
            let set: BTreeSet<String> = self.e.sec_data(&cm).split(|&c| c == 0).filter(|x| !x.is_empty()).map(ascii_replace).collect();
            r.set("comment", strs(set.into_iter().take(6)));
        }
        r.set("sections", Val::Int(self.e.sections().len() as i64));
        Ok(Val::Dict(r))
    }
}

/// bytes.decode('ascii', 'replace')
pub fn ascii_replace(b: &[u8]) -> String {
    b.iter().map(|&c| if c < 0x80 { c as char } else { '\u{fffd}' }).collect()
}

fn dump1(k: &str, v: Val) -> String {
    let mut d = Dict::new();
    d.set(k, v);
    port::pyjson::dumps(&Val::Dict(d), Some(1), true)
}

pub fn facts_run(args: &[String]) -> Result<u8, String> {
    for p in args {
        let mut im = Image::open(p)?;
        println!("{}", dump1(p, im.facts()?));
    }
    Ok(0)
}

// ---- elf-imports

const SOFTFP_FLOAT: &str = "acos acosf asin asinf atan atan2 atan2f atanf ceil ceilf cos cosf cosh exp expf fabs fabsf
floor floorf fmax fmaxf fmin fminf fmod fmodf frexp ldexp log log10 log10f logf modf modff pow powf remainder
rint round roundf sin sinf sinh sqrt sqrtf tan tanf tanh trunc truncf strtod strtof atof difftime __isfinite
ASensor_getResolution";

const ABI_NOTE: &[(&str, &str)] = &[
    ("__sF", "bionic FILE[3] array (84 bytes each); map &__sF[0..2] to glibc stdin/stdout/stderr"),
    ("__errno", "bionic name of __errno_location"),
    ("_ctype_", "bionic/OpenBSD ctype table char[257] (index c+1)"), ("_tolower_tab_", "short[257] table"),
    ("_toupper_tab_", "short[257] table"), ("__page_size", "unsigned int variable = 4096"),
    ("__system_property_get", "Android properties; return fake values"), ("__assert2", "(file,line,func,msg)"),
    ("__get_h_errno", "glibc __h_errno_location"), ("strlcpy", "not in glibc<2.38"), ("tkill", "syscall(SYS_tkill)"),
    ("__pthread_cleanup_push", "bionic cleanup list"), ("__pthread_cleanup_pop", "bionic cleanup list"),
    ("sigsetjmp", "bionic sigjmp_buf is 256 bytes; glibc writes 388 -> own implementation"),
    ("setjmp", "bionic jmp_buf is 256 bytes (and setjmp saves sigmask); glibc writes 388 -> own implementation"),
    ("longjmp", "pair with own setjmp"), ("siglongjmp", "pair with own sigsetjmp"),
    ("__data_start", "WEAK; Boehm GC data-root start: give libmono RW segment start or 0, never host value"),
    ("data_start", "WEAK; same as __data_start"),
    ("__google_potentially_blocking_region_begin", "WEAK; must resolve to 0"),
    ("__google_potentially_blocking_region_end", "WEAK; must resolve to 0"),
    ("__gnu_Unwind_Find_exidx", "must search OUR modules PT_ARM_EXIDX (glibc version uses dl_iterate_phdr and will not see them)"),
    ("pthread_cond_timedwait_relative_np", "bionic-only; relative timeout"),
    ("sysconf", "bionic _SC_* numbering differs (0x28 PAGE_SIZE, 0x61 NPROCESSORS_ONLN, 0x62 PHYS_PAGES, 6 CLK_TCK)"),
    ("dlopen", "bionic RTLD_* values (LAZY=1 NOW=0 GLOBAL=2 NOLOAD=4); route through loader"),
    ("dlsym", "bionic RTLD_DEFAULT=0xffffffff RTLD_NEXT=0xfffffffe"), ("dlclose", "loader"), ("dlerror", "loader"),
    ("stat", "bionic struct stat == kernel stat64 (104 B): call stat64"), ("fstat", "call fstat64"),
    ("lstat", "call lstat64"), ("statfs", "bionic struct statfs == statfs64: call statfs64"),
    ("readdir", "bionic struct dirent == dirent64: call readdir64"),
    ("pthread_mutex_init", "bionic pthread_mutex_t is 4 bytes (glibc 24); static init 0 / 0x4000 recursive"),
    ("pthread_mutex_lock", "mutex 4 bytes"), ("pthread_mutex_unlock", "mutex 4 bytes"), ("pthread_mutex_trylock", "mutex 4 bytes"),
    ("pthread_mutex_destroy", "mutex 4 bytes"),
    ("pthread_cond_init", "bionic pthread_cond_t is 4 bytes (glibc 48); static init 0"),
    ("pthread_cond_wait", "cond 4 bytes"), ("pthread_cond_timedwait", "cond 4 bytes"), ("pthread_cond_signal", "cond 4 bytes"),
    ("pthread_cond_broadcast", "cond 4 bytes"), ("pthread_cond_destroy", "cond 4 bytes"),
    ("pthread_attr_init", "bionic pthread_attr_t is 24 bytes (glibc 36)"), ("pthread_attr_destroy", "attr 24 bytes"),
    ("pthread_attr_setdetachstate", "attr 24 bytes"), ("pthread_attr_getdetachstate", "attr 24 bytes"),
    ("pthread_attr_setstacksize", "attr 24 bytes"), ("pthread_attr_setschedparam", "attr 24 bytes"),
    ("pthread_attr_getstack", "attr 24 bytes"), ("pthread_getattr_np", "fills bionic attr (24 bytes)"),
    ("pthread_create", "takes bionic attr"), ("pthread_exit", "run bionic cleanup handlers"),
    ("sem_init", "bionic sem_t is 4 bytes (glibc 16)"), ("sem_destroy", "sem 4 bytes"), ("sem_post", "sem 4 bytes"),
    ("sem_wait", "sem 4 bytes"), ("sem_trywait", "sem 4 bytes"), ("sem_timedwait", "sem 4 bytes"), ("sem_getvalue", "sem 4 bytes"),
    ("sigaction", "bionic struct sigaction 16 B (sigset 4 B) vs glibc 140 B"),
    ("sigprocmask", "bionic sigset_t 4 bytes (glibc 128)"), ("pthread_sigmask", "sigset_t 4 bytes"),
    ("sigsuspend", "sigset_t 4 bytes"), ("bsd_signal", "signal()"),
    ("getpwuid", "bionic LP32 struct passwd has no pw_gecos"), ("getpwnam", "bionic LP32 struct passwd has no pw_gecos"),
    ("getaddrinfo", "struct addrinfo: ai_canonname/ai_addr swapped; AI_* values differ"),
    ("freeaddrinfo", "pair with getaddrinfo wrapper"), ("getnameinfo", "NI_* flag values differ"),
    ("mbrtowc", "bionic mbstate_t 4 bytes vs glibc 8"), ("wcrtomb", "mbstate_t 4 vs 8 bytes"),
    ("setlocale", "bionic only C locale; return \"C\""),
    ("clearerr", "FILE*"), ("fclose", "FILE*"), ("fflush", "FILE* (NULL ok)"), ("fgets", "FILE*"), ("fprintf", "FILE*"),
    ("fputc", "FILE*"), ("fputs", "FILE*"), ("fread", "FILE*"), ("fscanf", "FILE*"), ("fseek", "FILE*; long offset (archives <2GiB)"),
    ("ftell", "FILE*"), ("fwrite", "FILE*"), ("getc", "FILE*"), ("putc", "FILE*"), ("ungetc", "FILE*"), ("getwc", "FILE*"),
    ("putwc", "FILE*"), ("ungetwc", "FILE*"), ("setvbuf", "FILE*"), ("vfprintf", "FILE*"), ("fdopen", "returns FILE*"),
    ("fopen", "returns FILE*"), ("popen", "returns FILE*"), ("pclose", "FILE*"),
    ("environ", "glibc environ"), ("__stack_chk_guard", "provide variable"),
    ("__aeabi_atexit", "(obj, dtor, dso) -> __cxa_atexit(dtor, obj, dso)"),
];

struct Glibc {
    libs: HashMap<String, HashSet<String>>,
    c: HashSet<String>,
}

fn exports_of(path: &str) -> Result<HashSet<String>, String> {
    let e = Elf::open(path)?;
    Ok(e.symbols().into_iter().filter(|s| s.table == ".dynsym" && s.shndx != 0 && matches!(s.info >> 4, 1 | 2)).map(|s| s.name).collect())
}

impl Glibc {
    fn load(dir: &str) -> Result<Glibc, String> {
        let mut libs = HashMap::new();
        for lib in ["libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1", "libgcc_s.so.1", "libstdc++.so.6", "ld-linux-armhf.so.3"] {
            let p = format!("{dir}/{lib}");
            if std::path::Path::new(&p).exists() {
                libs.insert(lib.to_string(), exports_of(&p)?);
            }
        }
        if !libs.contains_key("libc.so.6") || !libs.contains_key("libm.so.6") {
            return Err(format!("no armhf glibc in {dir} (libc.so.6, libm.so.6: Debian's libc6-armhf-cross; --glibc DIR)"));
        }
        let mut c = HashSet::new();
        for l in ["libc.so.6", "libpthread.so.0", "librt.so.1", "libdl.so.2", "ld-linux-armhf.so.3"] {
            c.extend(libs.get(l).cloned().unwrap_or_default());
        }
        Ok(Glibc { libs, c })
    }
    fn has(&self, lib: &str, n: &str) -> bool {
        self.libs.get(lib).is_some_and(|s| s.contains(n))
    }
}

fn provider(g: &Glibc, libmain: &HashSet<String>, name: &str, needed: &[String], ver: &str) -> &'static str {
    if needed.iter().any(|n| n == "libmain.so") && libmain.contains(name) {
        return "libmain.so";
    }
    if ver.ends_with("@libm.so") {
        return "libm";
    }
    if ver.ends_with("@libdl.so") {
        return "libdl";
    }
    if name.starts_with("__android_log") || name == "android_set_abort_message" {
        return if name.starts_with("__android_log") { "liblog" } else { "libc(bionic)" };
    }
    if name.starts_with("egl") {
        return "libEGL";
    }
    if name.starts_with("gl") && name.len() > 2 && name.as_bytes()[2].is_ascii_uppercase() {
        return "libGLESv2";
    }
    if ["slCreateEngine", "slQuery", "SL_IID_"].iter().any(|p| name.starts_with(p)) {
        return "libOpenSLES";
    }
    const ANDROID: [&str; 15] = ["ANativeWindow", "AAsset", "ALooper", "AConfiguration", "AInput", "AKeyEvent", "AMotionEvent", "ASensor", "AStorageManager", "ANativeActivity", "AObbInfo", "AChoreographer", "ATrace", "AHardwareBuffer", "ASharedMemory"];
    if ANDROID.iter().any(|p| name.starts_with(p)) {
        return "libandroid";
    }
    const ZLIB: [&str; 10] = ["inflate", "deflate", "compress", "uncompress", "gz", "zlib", "zError", "crc32", "adler32", "get_crc_table"];
    if ZLIB.iter().any(|p| name.starts_with(p)) {
        return "libz";
    }
    const DL: [&str; 10] = ["dlopen", "dlsym", "dlclose", "dlerror", "dladdr", "dl_iterate_phdr", "dl_unwind_find_exidx", "android_dlopen_ext", "android_get_LD_LIBRARY_PATH", "android_update_LD_LIBRARY_PATH"];
    if DL.contains(&name) {
        return "libdl";
    }
    if name.starts_with("_Z") || name.starts_with("__cxa_") || name.starts_with("__gxx_") {
        if name == "__cxa_atexit" || name == "__cxa_finalize" {
            return "libc";
        }
        return "libstdc++";
    }
    if g.has("libm.so.6", name) && !g.has("libc.so.6", name) {
        return "libm";
    }
    if name.starts_with("__aeabi_") || name.starts_with("__gnu_") || name.starts_with("_Unwind_") || (g.has("libgcc_s.so.1", name) && !g.c.contains(name)) {
        return "libgcc(compiler-rt; exported by bionic libc on LP32)";
    }
    "libc"
}

fn stt(t: u8) -> String {
    match t {
        0 => "NOTYPE",
        1 => "OBJECT",
        2 => "FUNC",
        3 => "SECTION",
        4 => "FILE",
        5 => "COMMON",
        6 => "TLS",
        10 => "LOOS",
        12 => "HIOS",
        13 => "LOPROC",
        15 => "HIPROC",
        x => return x.to_string(),
    }
    .into()
}
fn stb(b: u8) -> String {
    match b {
        0 => "LOCAL",
        1 => "GLOBAL",
        2 => "WEAK",
        10 => "LOOS",
        12 => "HIOS",
        13 => "LOPROC",
        15 => "HIPROC",
        x => return x.to_string(),
    }
    .into()
}

struct Undef {
    name: String,
    typ: String,
    bind: String,
    ver: String,
    rel: Vec<String>,
    prov: &'static str,
    flags: Vec<String>,
}

/// {version index: "aux@file" (needed) or "name(def)"} from .gnu.version_r and .gnu.version_d
fn version_names(e: &Elf) -> HashMap<u16, String> {
    let mut out = HashMap::new();
    let secs = e.sections();
    let strs_of = |s: &Sec| secs.get(s.link as usize).map(|t| t.off as usize).unwrap_or(0);
    let d = &e.d;
    if let Some(vn) = secs.iter().filter(|s| s.typ == 0x6ffffffe).last() {
        let so = strs_of(vn);
        let mut p = vn.off as usize;
        for _ in 0..vn.info.max(1) {
            if p + 16 > d.len() {
                break;
            }
            let (cnt, file, aux, next) = (crate::elf::u16le(d, p + 2), u32le(d, p + 4), u32le(d, p + 8), u32le(d, p + 12));
            let fname = e.str_at(so + file as usize);
            let mut a = p + aux as usize;
            for _ in 0..cnt {
                if a + 16 > d.len() {
                    break;
                }
                let (other, name, anext) = (crate::elf::u16le(d, a + 6), u32le(d, a + 8), u32le(d, a + 12));
                out.insert(other, format!("{}@{fname}", e.str_at(so + name as usize)));
                if anext == 0 {
                    break;
                }
                a += anext as usize;
            }
            if next == 0 {
                break;
            }
            p += next as usize;
        }
    }
    if let Some(vd) = secs.iter().filter(|s| s.typ == 0x6ffffffd).last() {
        let so = strs_of(vd);
        let mut p = vd.off as usize;
        for _ in 0..vd.info.max(1) {
            if p + 20 > d.len() {
                break;
            }
            let (ndx, cnt, aux, next) = (crate::elf::u16le(d, p + 4), crate::elf::u16le(d, p + 6), u32le(d, p + 12), u32le(d, p + 16));
            if cnt > 0 && p + aux as usize + 8 <= d.len() {
                let name = u32le(d, p + aux as usize);
                out.insert(ndx, format!("{}(def)", e.str_at(so + name as usize)));
            }
            if next == 0 {
                break;
            }
            p += next as usize;
        }
    }
    out
}

struct Analysis {
    out: Dict,
    undefined: Vec<Undef>,
    exports: Vec<String>,
}

fn analyse(path: &str, g: &Glibc, libmain: &HashSet<String>) -> Result<Analysis, String> {
    let mut im = Image::open(path)?;
    let mut out = Dict::new();
    out.set("path", s(path));
    out.set("phdrs", Val::List(im.e.phdrs.iter().map(|p| pt_val(p.typ, im.mach)).collect()));
    out.set("needed", strs(im.needed.clone()));
    let mut fl = Dict::new();
    for (t, v) in im.tags.clone() {
        if let Some(n @ ("DT_FLAGS" | "DT_FLAGS_1" | "DT_SYMBOLIC" | "DT_TEXTREL")) = dt_name(t) {
            fl.set(n, s(&hex(v)));
        }
    }
    out.set("flags", Val::Dict(fl));
    let syms = im.dynsym();
    let vername = version_names(&im.e);
    let vset: BTreeSet<String> = vername.values().cloned().collect();
    out.set("verneed", strs(vset));
    let vers: Vec<u16> = match im.e.sections().into_iter().filter(|s| s.typ == 0x6fffffff).last() {
        Some(vs) => im.e.sec_data(&vs).chunks_exact(2).map(|c| crate::elf::u16le(c, 0)).collect(),
        None => vec![],
    };
    let prefix = match im.mach {
        40 => "ARM_",
        183 => "AARCH64_",
        _ => "",
    };
    let rels: Vec<Reloc> = im.relocs()?.to_vec();
    let mut relcount: BTreeMap<(String, String), i64> = BTreeMap::new();
    let mut symrel: HashMap<u64, BTreeSet<String>> = HashMap::new();
    for r in &rels {
        let t = format!("R_{prefix}{}", im.reloc_name(r.typ));
        *relcount.entry((r.table.to_string(), t.clone())).or_default() += 1;
        if r.sym != 0 {
            symrel.entry(r.sym).or_default().insert(t);
        }
    }
    let mut rd = Dict::new();
    for ((a, b), v) in &relcount {
        rd.set(&format!("{a} {b}"), Val::Int(*v));
    }
    out.set("relocs", Val::Dict(rd));
    out.set("tls_symbols", strs(syms.iter().filter(|s| s.info & 15 == 6).map(|s| s.name.clone())));
    let soft: HashSet<&str> = SOFTFP_FLOAT.split_whitespace().collect();
    let mut und = Vec::new();
    for (i, sy) in syms.iter().enumerate() {
        if i == 0 || sy.shndx != 0 {
            continue;
        }
        let v = vers.get(i).copied().unwrap_or(0);
        let vn = if matches!(v, 0 | 1 | 0xff00 | 0xff01) { String::new() } else { vername.get(&v).cloned().unwrap_or_default() };
        let prov = provider(g, libmain, &sy.name, &im.needed, &vn);
        let mut flags = vec![];
        if soft.contains(sy.name.as_str()) {
            flags.push("SOFTFP_FLOAT_ARGS".to_string());
        }
        if let Some((_, n)) = ABI_NOTE.iter().find(|x| x.0 == sy.name) {
            flags.push(format!("ABI:{n}"));
        }
        if !g.c.contains(&sy.name) && !g.has("libm.so.6", &sy.name) && matches!(prov, "libc" | "libm" | "libdl" | "libc(bionic)") {
            flags.push("NOT_IN_GLIBC".into());
        }
        und.push(Undef {
            name: sy.name.clone(),
            typ: stt(sy.info & 15),
            bind: stb(sy.info >> 4),
            ver: vn,
            rel: symrel.get(&(i as u64)).map_or(vec![], |s| s.iter().cloned().collect()),
            prov,
            flags,
        });
    }
    out.set("n_defined_exports", Val::Int(syms.iter().filter(|s| s.shndx != 0 && !s.name.is_empty()).count() as i64));
    let mut exports: Vec<String> = syms.iter().filter(|s| s.shndx != 0 && !s.name.is_empty() && matches!(s.info >> 4, 1 | 2)).map(|s| s.name.clone()).collect();
    exports.sort();
    let mut arrays = Vec::new();
    for (a, sz, key) in [("DT_INIT_ARRAY", "DT_INIT_ARRAYSZ", "init_array"), ("DT_FINI_ARRAY", "DT_FINI_ARRAYSZ", "fini_array")] {
        let (addr, size) = (im.tag(dt(a)), im.tag(dt(sz)).unwrap_or(0));
        let mut entries = vec![];
        if let Some(addr) = addr {
            let mut i = 0;
            while i < size {
                entries.push(hex(im.ptr(addr + i)?.unwrap_or(0)));
                i += im.ps;
            }
        }
        let mut d = Dict::new();
        d.set("addr", s(&hex(addr.unwrap_or(0))));
        d.set("count", Val::Int((size / im.ps) as i64));
        d.set("entries", strs(entries));
        arrays.push((key, d));
    }
    for (k, d) in arrays {
        out.set(k, Val::Dict(d));
    }
    for (n, k) in [("DT_INIT", "DT_INIT"), ("DT_FINI", "DT_FINI")] {
        out.set(k, im.tag(dt(n)).map_or(Val::Null, |v| s(&hex(v))));
    }
    let text = im.e.section(".text").ok_or(format!("{path}: no .text"))?;
    let data = im.e.sec_data(&text);
    let base = text.addr;
    if im.mach == 183 {
        let hits: Vec<u64> = (0..data.len().saturating_sub(3)).step_by(4).filter(|&o| u32le(data, o) & 0xFFFFFFE0 == 0xD53BD040).map(|o| base + o as u64).collect();
        let mut t = Dict::new();
        t.set("count", Val::Int(hits.len() as i64));
        t.set("first", strs(hits.iter().take(20).map(|h| hex(*h))));
        out.set("tpidr_el0_reads", Val::Dict(t));
    } else {
        let (mut arm, mut thumb) = (vec![], vec![]);
        let mut off = 0;
        while off + 3 < data.len() {
            let w = u32le(data, off);
            if off % 4 == 0 && (w & 0xFFFF0FFF) == 0xEE1D0F70 {
                arm.push(hex(base + off as u64));
            }
            let (h1, h2) = (crate::elf::u16le(data, off), crate::elf::u16le(data, off + 2));
            if h1 == 0xEE1D && (h2 & 0x0FFF) == 0x0F70 {
                thumb.push(hex(base + off as u64));
            }
            off += 2;
        }
        let mut t = Dict::new();
        t.set("arm", strs(arm.iter().take(50).cloned()));
        t.set("arm_count", Val::Int(arm.len() as i64));
        t.set("thumb", strs(thumb.iter().take(50).cloned()));
        t.set("thumb_count", Val::Int(thumb.len() as i64));
        out.set("tpidruro_reads", Val::Dict(t));
    }
    Ok(Analysis { out, undefined: und, exports })
}

pub fn imports_run(args: &[String]) -> Result<u8, String> {
    let mut glibc = "/usr/arm-linux-gnueabihf/lib".to_string();
    let mut pos = vec![];
    let mut it = args.iter();
    while let Some(a) = it.next() {
        if a == "--glibc" {
            glibc = it.next().ok_or("--glibc DIR")?.clone();
        } else {
            pos.push(a.clone());
        }
    }
    let [outdir, libs @ ..] = &pos[..] else { return Err("usage: pmu-dev elf-imports OUTDIR LIB... [--glibc DIR]".into()) };
    let g = Glibc::load(&glibc)?;
    let mut libmain = HashSet::new();
    for p in libs {
        if p.rsplit('/').next() == Some("libmain.so") {
            libmain.extend(Elf::open(p)?.symbols().into_iter().filter(|s| s.table == ".dynsym" && s.shndx != 0 && !s.name.is_empty()).map(|s| s.name));
        }
    }
    let mut res = Vec::new();
    for p in libs {
        let base = p.rsplit('/').next().unwrap_or(p);
        let name = base.split(".so").next().unwrap_or(base).to_string();
        let a = analyse(p, &g, &libmain)?;
        let mut txt = format!("# Undefined dynamic symbols of {base}\n# DT_NEEDED: {}\n# columns: provider\tname\ttype\tbind\tversion\trelocs\tflags\n", match a.out.get("needed") {
            Some(Val::List(l)) => l.iter().map(|v| if let Val::Str(s) = v { s.clone() } else { String::new() }).collect::<Vec<_>>().join(" "),
            _ => String::new(),
        });
        let mut und: Vec<&Undef> = a.undefined.iter().collect();
        und.sort_by(|x, y| (x.prov, &x.name).cmp(&(y.prov, &y.name)));
        let dash = |s: String| if s.is_empty() { "-".to_string() } else { s };
        for u in und {
            txt += &format!("{}\t{}\t{}\t{}\t{}\t{}\t{}\n", u.prov, u.name, u.typ, u.bind, dash(u.ver.clone()), dash(u.rel.join(",")), dash(u.flags.join(",")));
        }
        std::fs::write(format!("{outdir}/imports_{name}.txt"), txt).map_err(|e| e.to_string())?;
        std::fs::write(format!("{outdir}/exports_{name}.txt"), a.exports.join("\n") + "\n").map_err(|e| e.to_string())?;
        res.push((name, a));
    }
    // (a dict by name: a later library of the same name replaces the earlier one in its place)
    let mut order: Vec<String> = Vec::new();
    for (n, _) in &res {
        if !order.contains(n) {
            order.push(n.clone());
        }
    }
    for n in order {
        let (_, a) = res.iter().rev().find(|(k, _)| *k == n).unwrap();
        let mut r = a.out.clone();
        r.set("undefined_count", Val::Int(a.undefined.len() as i64));
        let mut by = Dict::new();
        for u in &a.undefined {
            let c = match by.get(u.prov) {
                Some(Val::Int(c)) => *c,
                _ => 0,
            };
            by.set(u.prov, Val::Int(c + 1));
        }
        r.set("by_provider", Val::Dict(by));
        r.set("not_in_glibc", strs(a.undefined.iter().filter(|u| u.flags.iter().any(|f| f == "NOT_IN_GLIBC")).map(|u| u.name.clone())));
        r.set("object_imports", strs(a.undefined.iter().filter(|u| u.typ == "OBJECT").map(|u| u.name.clone())));
        r.set("weak_imports", strs(a.undefined.iter().filter(|u| u.bind == "WEAK").map(|u| u.name.clone())));
        println!("{}", dump1(&n, Val::Dict(r)));
    }
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sleb128() {
        assert_eq!(sleb(&[0x02], 0).unwrap(), (2, 1));
        assert_eq!(sleb(&[0x7e], 0).unwrap(), (-2, 1));
        assert_eq!(sleb(&[0x80, 0x01], 0).unwrap(), (128, 2));
        assert_eq!(sleb(&[0xff, 0x7e], 0).unwrap(), (-129, 2));
        assert_eq!(ascii_replace(b"a\xffb"), "a\u{fffd}b");
        assert_eq!(hex(255), "0xff");
    }
}
