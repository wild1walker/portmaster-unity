//! A small ELF reader (32 and 64-bit, little-endian): segments, notes, sections, symbols.

pub fn u16le(d: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([d[o], d[o + 1]])
}
pub fn u32le(d: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(d[o..o + 4].try_into().unwrap())
}
pub fn u64le(d: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(d[o..o + 8].try_into().unwrap())
}

pub struct Elf {
    pub d: Vec<u8>,
    pub is64: bool,
    pub machine: u16,
    pub phdrs: Vec<Phdr>,
    segs: Vec<(u64, u64, u64, u32)>,
    notes: Vec<(u64, u64)>,
    /// name, type, flags, addr, off, size, link, info, align, entsize
    secs: Vec<[u64; 10]>,
    shstrndx: usize,
}

/// A program header.
#[derive(Clone, Copy, Debug)]
pub struct Phdr {
    pub typ: u32,
    pub flags: u32,
    pub off: u64,
    pub va: u64,
    pub filesz: u64,
    pub memsz: u64,
    pub align: u64,
}

/// A section header, with its name.
#[derive(Clone, Debug)]
pub struct Sec {
    pub name: String,
    pub typ: u32,
    pub addr: u64,
    pub off: u64,
    pub size: u64,
    pub link: u32,
    pub info: u32,
    pub entsize: u64,
}

/// A symbol table entry: its table's name (.symtab, .dynsym), name, value, size, st_info, st_shndx.
pub struct Sym {
    pub table: String,
    pub name: String,
    pub value: u64,
    pub size: u64,
    pub info: u8,
    pub shndx: u16,
}

impl Elf {
    pub fn open(path: &str) -> Result<Elf, String> {
        let d = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
        Elf::new(d).ok_or_else(|| format!("{path}: not an ELF file"))
    }

    pub fn new(d: Vec<u8>) -> Option<Elf> {
        if d.len() < 0x34 || &d[..4] != b"\x7fELF" {
            return None;
        }
        let is64 = d[4] == 2;
        let (phoff, shoff, at) = if is64 {
            (u64le(&d, 0x20), u64le(&d, 0x28), 0x36)
        } else {
            (u32le(&d, 0x1c) as u64, u32le(&d, 0x20) as u64, 0x2a)
        };
        let [phentsize, phnum, shentsize, shnum, shstrndx] = [0, 1, 2, 3, 4].map(|k| u16le(&d, at + 2 * k) as u64);
        let (mut segs, mut notes, mut secs, mut phdrs) = (Vec::new(), Vec::new(), Vec::new(), Vec::new());
        let machine = u16le(&d, 0x12);
        for i in 0..phnum {
            let o = (phoff + i * phentsize) as usize;
            if o + if is64 { 56 } else { 32 } > d.len() {
                return None;
            }
            let (t, fl, off, va, fs, ms, al) = if is64 {
                (u32le(&d, o), u32le(&d, o + 4), u64le(&d, o + 8), u64le(&d, o + 16), u64le(&d, o + 32), u64le(&d, o + 40), u64le(&d, o + 48))
            } else {
                let w = |k: usize| u32le(&d, o + k) as u64;
                (u32le(&d, o), u32le(&d, o + 24), w(4), w(8), w(16), w(20), w(28))
            };
            phdrs.push(Phdr { typ: t, flags: fl, off, va, filesz: fs, memsz: ms, align: al });
            if t == 1 {
                segs.push((va, off, fs, fl));
            } else if t == 4 {
                notes.push((off, fs));
            }
        }
        for i in 0..shnum {
            let o = (shoff + i * shentsize) as usize;
            if o + if is64 { 64 } else { 40 } > d.len() {
                return None;
            }
            secs.push(if is64 {
                [
                    u32le(&d, o) as u64, u32le(&d, o + 4) as u64, u64le(&d, o + 8), u64le(&d, o + 16), u64le(&d, o + 24),
                    u64le(&d, o + 32), u32le(&d, o + 40) as u64, u32le(&d, o + 44) as u64, u64le(&d, o + 48), u64le(&d, o + 56),
                ]
            } else {
                [0, 1, 2, 3, 4, 5, 6, 7, 8, 9].map(|k| u32le(&d, o + 4 * k) as u64)
            });
        }
        Some(Elf { d, is64, machine, phdrs, segs, notes, secs, shstrndx: shstrndx as usize })
    }

    pub fn build_id(&self) -> Option<String> {
        let d = &self.d;
        let srcs: Vec<(u64, u64)> = self.notes.iter().copied().chain(self.secs.iter().filter(|s| s[1] == 7).map(|s| (s[4], s[5]))).collect();
        for (off, size) in srcs {
            let (off, size) = (off as usize, size as usize);
            let mut p = off;
            while p + 12 <= off + size && p + 12 <= d.len() {
                let (nsz, dsz, typ) = (u32le(d, p) as usize, u32le(d, p + 4) as usize, u32le(d, p + 8));
                let name = d.get(p + 12..(p + 12 + nsz).min(d.len())).unwrap_or(&[]);
                let desc_at = p + 12 + ((nsz + 3) & !3);
                if typ == 3 && name.starts_with(b"GNU") {
                    return Some(d.get(desc_at..desc_at + dsz)?.iter().map(|b| format!("{b:02x}")).collect());
                }
                p = desc_at + ((dsz + 3) & !3);
            }
        }
        None
    }

    /// (file offset, segment flags) of n bytes at va, inside one loaded segment.
    pub fn file_off(&self, va: u64, n: u64) -> Option<(usize, u32)> {
        self.segs.iter().find(|(v, _, fs, _)| *v <= va && va + n <= v + fs).map(|(v, o, _, fl)| ((o + va - v) as usize, *fl))
    }

    fn cstr(&self, off: usize) -> String {
        let d = &self.d;
        if off >= d.len() {
            return String::new();
        }
        let e = d[off..].iter().position(|&c| c == 0).map_or(d.len(), |e| off + e);
        String::from_utf8_lossy(&d[off..e]).into_owned()
    }

    fn sec_name(&self, s: &[u64; 10]) -> String {
        match self.secs.get(self.shstrndx) {
            Some(st) => self.cstr(st[4] as usize + s[0] as usize),
            None => String::new(),
        }
    }

    /// Every entry of the symbol tables (SHT_SYMTAB and SHT_DYNSYM), in order.
    pub fn symbols(&self) -> Vec<Sym> {
        let d = &self.d;
        let mut out = Vec::new();
        for s in &self.secs {
            if s[1] != 2 && s[1] != 11 {
                continue;
            }
            let table = self.sec_name(s);
            let Some(strtab) = self.secs.get(s[6] as usize) else { continue };
            let stroff = strtab[4] as usize;
            let ent = s[9];
            if ent == 0 {
                continue;
            }
            for i in 0..s[5] / ent {
                let o = (s[4] + i * ent) as usize;
                if o + ent as usize > d.len() {
                    break;
                }
                let (st_name, info, shndx, value, size) = if self.is64 {
                    (u32le(d, o) as usize, d[o + 4], u16le(d, o + 6), u64le(d, o + 8), u64le(d, o + 16))
                } else {
                    (u32le(d, o) as usize, d[o + 12], u16le(d, o + 14), u32le(d, o + 4) as u64, u32le(d, o + 8) as u64)
                };
                out.push(Sym { table: table.clone(), name: self.cstr(stroff + st_name), value, size, info, shndx });
            }
        }
        out
    }

    /// (name, value, size) of each function symbol in the symbol tables.
    pub fn funcs(&self) -> Vec<(String, u64, u64)> {
        let d = &self.d;
        let mut out = Vec::new();
        for s in &self.secs {
            if s[1] != 2 {
                continue; // SHT_SYMTAB
            }
            let stroff = self.secs[s[6] as usize][4] as usize;
            let ent = s[9];
            if ent == 0 {
                continue;
            }
            for i in 0..s[5] / ent {
                let o = (s[4] + i * ent) as usize;
                let (st_name, st_info, st_shndx, st_value, st_size) = if self.is64 {
                    (u32le(d, o) as usize, d[o + 4], u16le(d, o + 6), u64le(d, o + 8), u64le(d, o + 16))
                } else {
                    (u32le(d, o) as usize, d[o + 12], u16le(d, o + 14), u32le(d, o + 4) as u64, u32le(d, o + 8) as u64)
                };
                if st_info & 15 != 2 || st_value == 0 || st_shndx == 0 {
                    continue;
                }
                let s0 = stroff + st_name;
                let e = d[s0..].iter().position(|&c| c == 0).map_or(d.len(), |e| s0 + e);
                out.push((String::from_utf8_lossy(&d[s0..e]).into_owned(), st_value, st_size));
            }
        }
        out
    }
}


impl Elf {
    /// Every section header (index 0, the null one, too), with its name.
    pub fn sections(&self) -> Vec<Sec> {
        self.secs
            .iter()
            .map(|s| Sec { name: self.sec_name(s), typ: s[1] as u32, addr: s[3], off: s[4], size: s[5], link: s[6] as u32, info: s[7] as u32, entsize: s[9] })
            .collect()
    }

    /// The last section of that name (as a dict by name keeps it).
    pub fn section(&self, name: &str) -> Option<Sec> {
        self.sections().into_iter().filter(|s| s.name == name).last()
    }

    /// A section's bytes (none for SHT_NOBITS).
    pub fn sec_data(&self, s: &Sec) -> &[u8] {
        if s.typ == 8 {
            return &[];
        }
        let (a, b) = (s.off as usize, (s.off + s.size) as usize);
        self.d.get(a..b.min(self.d.len())).unwrap_or(&[])
    }

    /// The C string at a file offset (lossy).
    pub fn str_at(&self, off: usize) -> String {
        self.cstr(off)
    }
}
