//! An IL2CPP build's type metadata as CLR assemblies made in memory (clrmeta's synthetic
//! assemblies), one per image, so what reads a Mono build's `Managed/` assemblies reads an IL2CPP
//! build's types the same way (convert's monotree: MonoBehaviour type trees for the quality step).
//! From the build's own files only: `il2cpp_data/Metadata/global-metadata.dat` (names, type
//! definitions, fields, nested types, generic parameters, custom attribute data) and the binary
//! that holds the type table the fields' types index (a macOS build's GameAssembly.dylib: its
//! `g_MetadataRegistration` symbol). Layouts as Il2CppDumper (MIT) reads them. Metadata version 31
//! (Unity 2022.3 - 6000.0); a Mach-O binary's pointers in either form a linker writes them
//! (classic dyld info, or the chained fixups of a build for macOS 12 or later, in the plain 64-bit
//! layouts): anything else is refused, with the reason.
//!
//! Each image becomes an assembly whose tables hold what a compiler would have written for the
//! parts that matter to serialization: TypeDef (flags, names, base type), Field (flags, signature),
//! NestedClass, GenericParam, TypeRef / AssemblyRef / TypeSpec for what it refers to, and the
//! fields' custom attributes (CustomAttribute rows naming each attribute's type through a
//! MemberRef `.ctor`). No methods, no IL.
use crate::clrmeta::Assembly;
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::os::unix::fs::FileExt;
use std::path::Path;

pub type R<T> = std::result::Result<T, String>;

const TYPEREF: u8 = 0x01;
const TYPEDEF: u8 = 0x02;
const FIELD: u8 = 0x04;
const MEMBERREF: u8 = 0x0A;
const CUSTOMATTRIBUTE: u8 = 0x0C;
const TYPESPEC: u8 = 0x1B;
const ASSEMBLYREF: u8 = 0x23;
const NESTEDCLASS: u8 = 0x29;
const GENERICPARAM: u8 = 0x2A;

// global-metadata.dat v31: the header's (offset, size) pairs, in order
const S_STRING: usize = 2;
const S_METHODS: usize = 5;
const S_FIELDS: usize = 11;
const S_GPARAMS: usize = 12;
const S_GCONTAINERS: usize = 14;
const S_NESTED: usize = 15;
const S_TYPEDEFS: usize = 19;
const S_IMAGES: usize = 20;
const S_ATTRDATA: usize = 24;
const S_ATTRRANGE: usize = 25;
const NSECTIONS: usize = 31;
const TYPEDEF_SIZE: usize = 88;
const FIELD_SIZE: usize = 12;
const IMAGE_SIZE: usize = 40;
const METHOD_SIZE: usize = 36;
const GPARAM_SIZE: usize = 16;
const GCONTAINER_SIZE: usize = 16;
const RANGE_SIZE: usize = 8;

// Il2CppTypeEnum (the same numbers as ECMA-335's element types)
const T_VALUETYPE: u8 = 0x11;
const T_CLASS: u8 = 0x12;
const T_VAR: u8 = 0x13;
const T_ARRAY: u8 = 0x14;
const T_GENERICINST: u8 = 0x15;
const T_SZARRAY: u8 = 0x1D;
const T_MVAR: u8 = 0x1E;

fn u32_at(b: &[u8], o: usize) -> R<u32> {
    b.get(o..o + 4)
        .map(|x| u32::from_le_bytes(x.try_into().unwrap()))
        .ok_or_else(|| format!("metadata: offset {o:#x} past the end"))
}

fn i32_at(b: &[u8], o: usize) -> R<i32> {
    u32_at(b, o).map(|v| v as i32)
}

fn u16_at(b: &[u8], o: usize) -> R<u16> {
    b.get(o..o + 2)
        .map(|x| u16::from_le_bytes([x[0], x[1]]))
        .ok_or_else(|| format!("metadata: offset {o:#x} past the end"))
}

/// libil2cpp's compressed unsigned integer (not ECMA-335's): (value, next offset).
pub fn compressed_u32(b: &[u8], o: usize) -> R<(u32, usize)> {
    let byte = |i: usize| {
        b.get(i)
            .copied()
            .ok_or_else(|| "attribute data: past the end".to_string())
    };
    let first = byte(o)?;
    Ok(if first & 0x80 == 0 {
        (first as u32, o + 1)
    } else if first & 0xC0 == 0x80 {
        (((first as u32 & 0x3F) << 8) | byte(o + 1)? as u32, o + 2)
    } else if first & 0xE0 == 0xC0 {
        let v = ((first as u32 & 0x1F) << 24)
            | (byte(o + 1)? as u32) << 16
            | (byte(o + 2)? as u32) << 8
            | byte(o + 3)? as u32;
        (v, o + 4)
    } else if first == 0xF0 {
        (u32_at(b, o + 1)?, o + 5)
    } else if first == 0xFE {
        (u32::MAX - 1, o + 1)
    } else if first == 0xFF {
        (u32::MAX, o + 1)
    } else {
        return Err(format!("attribute data: compressed integer {first:#x}"));
    })
}

/// ECMA-335's compressed unsigned integer, appended.
fn ecma_u32(out: &mut Vec<u8>, v: u32) {
    if v < 0x80 {
        out.push(v as u8);
    } else if v < 0x4000 {
        out.extend([0x80 | (v >> 8) as u8, v as u8]);
    } else {
        out.extend([
            0xC0 | (v >> 24) as u8,
            (v >> 16) as u8,
            (v >> 8) as u8,
            v as u8,
        ]);
    }
}

struct TypeDefRec {
    name: u32,
    ns: u32,
    declaring: i32,
    parent: i32,
    gcontainer: i32,
    flags: u32,
    field_start: i32,
    field_count: u16,
}

struct ImageRec {
    name: String,
    type_start: u32,
    type_count: u32,
    attr_start: u32,
    attr_count: u32,
}

/// global-metadata.dat (version 31).
/// One image's types: full name ("Ns.Outer/Inner") to (method names, field names).
pub type Members = HashMap<String, (HashSet<String>, HashSet<String>)>;

pub struct Metadata {
    b: Vec<u8>,
    sec: [(usize, usize); NSECTIONS],
    images: Vec<ImageRec>,
}

impl Metadata {
    pub fn open(path: &Path) -> R<Metadata> {
        let b = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
        Self::from_bytes(b)
    }

    pub fn from_bytes(b: Vec<u8>) -> R<Metadata> {
        if u32_at(&b, 0)? != 0xFAB1_1BAF {
            return Err("not IL2CPP metadata (no 0xFAB11BAF)".into());
        }
        let version = i32_at(&b, 4)?;
        if version != 31 {
            return Err(format!(
                "IL2CPP metadata version {version}: only 31 is read"
            ));
        }
        let mut sec = [(0usize, 0usize); NSECTIONS];
        for (i, s) in sec.iter_mut().enumerate() {
            let (o, n) = (
                u32_at(&b, 8 + 8 * i)? as usize,
                u32_at(&b, 12 + 8 * i)? as usize,
            );
            if o.checked_add(n).is_none_or(|e| e > b.len()) {
                return Err(format!("metadata section {i} ({o:#x}, {n}) past the end"));
            }
            *s = (o, n);
        }
        for (i, size) in [
            (S_TYPEDEFS, TYPEDEF_SIZE),
            (S_FIELDS, FIELD_SIZE),
            (S_IMAGES, IMAGE_SIZE),
            (S_METHODS, METHOD_SIZE),
            (S_GPARAMS, GPARAM_SIZE),
            (S_GCONTAINERS, GCONTAINER_SIZE),
            (S_ATTRRANGE, RANGE_SIZE),
        ] {
            if sec[i].1 % size != 0 {
                return Err(format!(
                    "metadata section {i}: {} bytes, not records of {size}",
                    sec[i].1
                ));
            }
        }
        let mut m = Metadata {
            b,
            sec,
            images: Vec::new(),
        };
        for i in 0..m.sec[S_IMAGES].1 / IMAGE_SIZE {
            let o = m.sec[S_IMAGES].0 + i * IMAGE_SIZE;
            let rec = ImageRec {
                name: m.string(u32_at(&m.b, o)?)?,
                type_start: u32_at(&m.b, o + 8)?,
                type_count: u32_at(&m.b, o + 12)?,
                attr_start: u32_at(&m.b, o + 32)?,
                attr_count: u32_at(&m.b, o + 36)?,
            };
            if rec.type_start as usize + rec.type_count as usize > m.typedef_count() {
                return Err(format!("image {}: types past the table", rec.name));
            }
            m.images.push(rec);
        }
        Ok(m)
    }

    /// The images' names ("Assembly-CSharp.dll"), in the metadata's order.
    pub fn image_names(&self) -> Vec<String> {
        self.images.iter().map(|i| i.name.clone()).collect()
    }

    pub fn string(&self, i: u32) -> R<String> {
        let (o, n) = self.sec[S_STRING];
        let s = o + i as usize;
        let end = self.b[s.min(o + n)..o + n]
            .iter()
            .position(|&c| c == 0)
            .ok_or_else(|| format!("metadata string {i}: no end"))?;
        Ok(String::from_utf8_lossy(&self.b[s..s + end]).into_owned())
    }

    pub fn typedef_count(&self) -> usize {
        self.sec[S_TYPEDEFS].1 / TYPEDEF_SIZE
    }

    fn typedef(&self, i: usize) -> R<TypeDefRec> {
        if i >= self.typedef_count() {
            return Err(format!("type definition {i} past the table"));
        }
        let o = self.sec[S_TYPEDEFS].0 + i * TYPEDEF_SIZE;
        let b = &self.b;
        Ok(TypeDefRec {
            name: u32_at(b, o)?,
            ns: u32_at(b, o + 4)?,
            declaring: i32_at(b, o + 12)?,
            parent: i32_at(b, o + 16)?,
            gcontainer: i32_at(b, o + 24)?,
            flags: u32_at(b, o + 28)?,
            field_start: i32_at(b, o + 32)?,
            field_count: u16_at(b, o + 68)?,
        })
    }

    /// (name, type index, token) of a field definition.
    fn field(&self, i: usize) -> R<(u32, i32, u32)> {
        if i >= self.sec[S_FIELDS].1 / FIELD_SIZE {
            return Err(format!("field {i} past the table"));
        }
        let o = self.sec[S_FIELDS].0 + i * FIELD_SIZE;
        Ok((
            u32_at(&self.b, o)?,
            i32_at(&self.b, o + 4)?,
            u32_at(&self.b, o + 8)?,
        ))
    }

    /// Every image's types with the names of their methods and fields, by image name.
    pub fn members(&self) -> R<HashMap<String, Members>> {
        let n = self.typedef_count();
        let (to, (no, nn)) = (self.sec[S_TYPEDEFS].0, self.sec[S_NESTED]);
        let (mo, mn) = self.sec[S_METHODS];
        let mut outer: Vec<Option<usize>> = vec![None; n];
        for i in 0..n {
            let o = to + i * TYPEDEF_SIZE;
            let (start, count) = (i32_at(&self.b, o + 48)?, u16_at(&self.b, o + 72)? as usize);
            for k in 0..count {
                let at = usize::try_from(start).map_err(|_| format!("type definition {i}: no nested types"))? + k;
                if (at + 1) * 4 > nn {
                    return Err(format!("type definition {i}: nested types past the table"));
                }
                let inner = u32_at(&self.b, no + at * 4)? as usize;
                if inner >= n || inner == i {
                    return Err(format!("type definition {i}: nested type {inner}"));
                }
                outer[inner] = Some(i);
            }
        }
        let mut out = HashMap::with_capacity(self.images.len());
        for im in &self.images {
            let mut types = Members::with_capacity(im.type_count as usize);
            for i in im.type_start as usize..(im.type_start + im.type_count) as usize {
                let td = self.typedef(i)?;
                let (mut full, mut top) = (self.string(td.name)?, i);
                for depth in 0.. {
                    let Some(p) = outer[top] else { break };
                    if depth == 64 {
                        return Err(format!("type definition {i}: nested without end"));
                    }
                    full = format!("{}/{full}", self.string(self.typedef(p)?.name)?);
                    top = p;
                }
                let ns = self.string(self.typedef(top)?.ns)?;
                if !ns.is_empty() {
                    full = format!("{ns}.{full}");
                }
                let o = to + i * TYPEDEF_SIZE;
                let (start, count) = (i32_at(&self.b, o + 36)?, u16_at(&self.b, o + 64)? as usize);
                let entry = types.entry(full).or_default();
                for k in 0..count {
                    let at = usize::try_from(start).map_err(|_| format!("type definition {i}: no methods"))? + k;
                    if (at + 1) * METHOD_SIZE > mn {
                        return Err(format!("type definition {i}: methods past the table"));
                    }
                    entry.0.insert(self.string(u32_at(&self.b, mo + at * METHOD_SIZE)?)?);
                }
                for k in 0..td.field_count as usize {
                    let at = usize::try_from(td.field_start).map_err(|_| format!("type definition {i}: no fields"))? + k;
                    entry.1.insert(self.string(self.field(at)?.0)?);
                }
            }
            out.insert(im.name.clone(), types);
        }
        Ok(out)
    }

    /// The type definition a method belongs to.
    fn method_type(&self, i: usize) -> R<usize> {
        if i >= self.sec[S_METHODS].1 / METHOD_SIZE {
            return Err(format!("method {i} past the table"));
        }
        let t = i32_at(&self.b, self.sec[S_METHODS].0 + i * METHOD_SIZE + 4)?;
        usize::try_from(t).map_err(|_| format!("method {i}: no type"))
    }

    /// (number, flags, name) of a generic parameter.
    fn gparam(&self, i: usize) -> R<(u16, u16, u32)> {
        if i >= self.sec[S_GPARAMS].1 / GPARAM_SIZE {
            return Err(format!("generic parameter {i} past the table"));
        }
        let o = self.sec[S_GPARAMS].0 + i * GPARAM_SIZE;
        Ok((
            u16_at(&self.b, o + 12)?,
            u16_at(&self.b, o + 14)?,
            u32_at(&self.b, o + 4)?,
        ))
    }

    /// (count, first parameter) of a generic container.
    fn gcontainer(&self, i: usize) -> R<(u32, u32)> {
        if i >= self.sec[S_GCONTAINERS].1 / GCONTAINER_SIZE {
            return Err(format!("generic container {i} past the table"));
        }
        let o = self.sec[S_GCONTAINERS].0 + i * GCONTAINER_SIZE;
        Ok((u32_at(&self.b, o + 4)?, u32_at(&self.b, o + 12)?))
    }

    /// {metadata token: (start, end) of its custom attribute data} of an image.
    fn attribute_ranges(&self, image: usize) -> R<HashMap<u32, (usize, usize)>> {
        let im = &self.images[image];
        let (ro, rn) = self.sec[S_ATTRRANGE];
        let (total, dn) = (rn / RANGE_SIZE, self.sec[S_ATTRDATA].1);
        let (first, n) = (im.attr_start as usize, im.attr_count as usize);
        if first + n > total {
            return Err(format!(
                "image {}: attribute ranges past the table",
                im.name
            ));
        }
        let mut out = HashMap::with_capacity(n);
        for k in first..first + n {
            let token = u32_at(&self.b, ro + k * RANGE_SIZE)?;
            let start = u32_at(&self.b, ro + k * RANGE_SIZE + 4)? as usize;
            let end = if k + 1 < total {
                u32_at(&self.b, ro + (k + 1) * RANGE_SIZE + 4)? as usize
            } else {
                dn
            };
            if start > end || end > dn {
                return Err(format!("attribute data of token {token:#x}: bad range"));
            }
            out.insert(token, (start, end));
        }
        Ok(out)
    }

    /// The custom attribute types (type definitions) of one range of attribute data.
    fn attribute_types(&self, (start, end): (usize, usize)) -> R<Vec<usize>> {
        let o0 = self.sec[S_ATTRDATA].0;
        let data = &self.b[o0 + start..o0 + end];
        let (count, mut o) = compressed_u32(data, 0)?;
        let mut out = Vec::new();
        for _ in 0..count {
            let m = u32_at(data, o)? as usize;
            o += 4;
            out.push(self.method_type(m)?);
        }
        Ok(out)
    }
}

/// The pointers of a Mach-O with chained fixups (LC_DYLD_CHAINED_FIXUPS): such a word holds, in
/// the file, where it points in a packed form and the distance to the next such word of its page,
/// so which words are pointers is known only by walking a page's chain from its first one. Pages
/// are walked when first asked for.
struct Chains {
    /// the address the image is linked at
    image: u64,
    /// per segment with chains: (address, page size, pointer layout, each page's first fixup)
    segs: Vec<(u64, u64, u16, Vec<u16>)>,
    /// page address -> (offset in the page, the address the word gets), sorted
    pages: RefCell<HashMap<u64, Vec<(u64, u64)>>>,
}

/// (no fixup in this page)
const NO_CHAIN: u16 = 0xFFFF;

impl Chains {
    /// The page a word is in, if its segment has chains and the page a first fixup:
    /// (the page's address, its size, its first fixup, the pointer layout).
    fn page_of(&self, va: u64) -> Option<(u64, u64, u16, u16)> {
        for (addr, size, layout, starts) in &self.segs {
            if va < *addr {
                continue;
            }
            let i = (va - addr) / size;
            if let Some(&start) = starts.get(usize::try_from(i).ok()?) {
                return (start != NO_CHAIN).then_some((addr + i * size, *size, start, *layout));
            }
        }
        None
    }

    /// The fixups of a page from its bytes (the page and, where the file has them, the four
    /// bytes after: a pointer may start in the page's last four): a rebase gets the address it
    /// points to, a bind (a name of another image, which no address here stands for) zero, as
    /// the file of the other form has it.
    fn walk(&self, page: &[u8], size: u64, start: u16, layout: u16) -> R<Vec<(u64, u64)>> {
        let (mut out, mut off) = (Vec::new(), start as u64);
        loop {
            let raw = usize::try_from(off)
                .ok()
                .filter(|_| off < size)
                .and_then(|o| page.get(o..o + 8))
                .map(|w| u64::from_le_bytes(w.try_into().unwrap()))
                .ok_or_else(|| format!("binary: a fixup chain leaves its page at {off:#x}"))?;
            let value = if raw >> 63 != 0 {
                0
            } else {
                let (target, high8) = (raw & 0xF_FFFF_FFFF, (raw >> 36) & 0xFF);
                // (layout 6: an offset from the image's start; layout 2: the linked address)
                let to = if layout == 6 { self.image.wrapping_add(target) } else { target };
                to | high8 << 56
            };
            out.push((off, value));
            let next = (raw >> 51) & 0xFFF;
            if next == 0 {
                return Ok(out);
            }
            off += 4 * next;
        }
    }
}

/// The binary's Il2CppType table, read in place: a 64-bit Mach-O (a universal file's arm64 slice,
/// else x86_64; with classic dyld info its pointers are the addresses they point to, with chained
/// fixups they are read through their chains) or a 64-bit ELF shared library (aarch64, x86_64;
/// its RELATIVE relocations: the pointers the loader writes).
pub struct Types {
    f: File,
    base: u64,
    /// (address, file offset, file size) of each loaded segment, and whether it holds data
    segs: Vec<(u64, u64, u64, bool)>,
    /// (address, value) of the pointers the loader writes, sorted (an ELF's RELATIVE relocations)
    relocs: Vec<(u64, u64)>,
    /// a Mach-O's chained fixups, if it has that form
    chains: Option<Chains>,
    types: u64,
    count: usize,
    cache: HashMap<u64, (u64, u32)>,
}

const MAX_COUNT: i64 = 50_000_000;

impl Types {
    fn read(&self, off: u64, n: usize) -> R<Vec<u8>> {
        let mut b = vec![0u8; n];
        self.f
            .read_exact_at(&mut b, self.base + off)
            .map_err(|e| format!("binary: {e} at {off:#x}"))?;
        Ok(b)
    }

    fn file_off(&self, va: u64) -> R<u64> {
        for &(vmaddr, fileoff, filesize, _) in &self.segs {
            if vmaddr <= va && va < vmaddr + filesize {
                return Ok(va - vmaddr + fileoff);
            }
        }
        Err(format!("binary: address {va:#x} outside its segments"))
    }

    /// A pointer-sized word at an address, as the loaded image has it.
    fn u64_va(&self, va: u64) -> R<u64> {
        if let Ok(i) = self.relocs.binary_search_by_key(&va, |r| r.0) {
            return Ok(self.relocs[i].1);
        }
        if let Some((page, size, start, layout)) =
            self.chains.as_ref().and_then(|c| c.page_of(va))
        {
            let c = self.chains.as_ref().unwrap();
            if !c.pages.borrow().contains_key(&page) {
                let have = self
                    .segs
                    .iter()
                    .find(|s| s.0 <= page && page < s.0 + s.2)
                    .map(|s| s.0 + s.2 - page)
                    .ok_or_else(|| format!("binary: fixups at {page:#x}, outside the file"))?;
                let bytes = self.read(self.file_off(page)?, (size + 4).min(have) as usize)?;
                let fixups = c.walk(&bytes, size, start, layout)?;
                c.pages.borrow_mut().insert(page, fixups);
            }
            let pages = c.pages.borrow();
            let fixups = &pages[&page];
            if let Ok(i) = fixups.binary_search_by_key(&(va - page), |f| f.0) {
                return Ok(fixups[i].1);
            }
        }
        let b = self.read(self.file_off(va)?, 8)?;
        Ok(u64::from_le_bytes(b.try_into().unwrap()))
    }

    /// (data, bits) of the Il2CppType at an address.
    fn at(&mut self, va: u64) -> R<(u64, u32)> {
        if let Some(&t) = self.cache.get(&va) {
            return Ok(t);
        }
        let data = self.u64_va(va)?;
        let b = self.read(self.file_off(va + 8)?, 4)?;
        let t = (data, u32::from_le_bytes(b.try_into().unwrap()));
        self.cache.insert(va, t);
        Ok(t)
    }

    /// The address of type `i` of the table.
    fn addr(&self, i: i32) -> R<u64> {
        let i = usize::try_from(i).map_err(|_| format!("type index {i}"))?;
        if i >= self.count {
            return Err(format!("type index {i} past the table ({})", self.count));
        }
        self.u64_va(self.types + 8 * i as u64)
    }

    /// Whether `reg` holds a metadata registration of `typedefs` types: its 16 words (counts and
    /// pointers in turn), field offsets and type sizes one per type definition, the type table in
    /// a data segment.
    fn registration(&self, reg: u64, typedefs: usize) -> Option<(usize, u64)> {
        let mut q = [0i64; 16];
        for (k, w) in q.iter_mut().enumerate() {
            *w = self.u64_va(reg + 8 * k as u64).ok()? as i64;
        }
        let counts_ok = [0, 2, 4, 6, 8, 10, 12]
            .iter()
            .all(|&k| (0..MAX_COUNT).contains(&q[k]));
        if !counts_ok || q[10] as usize != typedefs || q[12] as usize != typedefs || q[6] <= 0 {
            return None;
        }
        let types = q[7] as u64;
        let in_data = |va: u64| {
            self.segs
                .iter()
                .any(|&(a, _, n, d)| d && a <= va && va < a + n)
        };
        (in_data(types) && in_data(types + 8 * (q[6] as u64 - 1))).then_some((q[6] as usize, types))
    }

    /// The metadata registration found in the data segments: the word that counts the field
    /// offset tables and, two words on, the one that counts the type sizes, both the metadata's
    /// type count (Il2CppDumper's search), the registration ten words before.
    fn find_registration(&self, typedefs: usize) -> Option<(u64, usize, u64)> {
        let want = (typedefs as u64).to_le_bytes();
        for &(va, off, size, data) in &self.segs {
            if !data {
                continue;
            }
            const CHUNK: u64 = 1 << 20;
            let mut pos = 0u64;
            while pos < size {
                let n = CHUNK.min(size - pos);
                let Ok(b) = self.read(off + pos, (n + 16).min(size - pos) as usize) else {
                    break;
                };
                let mut k = 0usize;
                while k + 24 <= b.len() && (k as u64) < n {
                    if b[k..k + 8] == want && b[k + 16..k + 24] == want {
                        let at = va + pos + k as u64;
                        if let Some(reg) = at.checked_sub(80) {
                            if let Some((count, types)) = self.registration(reg, typedefs) {
                                return Some((reg, count, types));
                            }
                        }
                    }
                    k += 8;
                }
                pos += n;
            }
        }
        None
    }

    pub fn open(path: &Path, typedefs: usize) -> R<Types> {
        let f = File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let rd = |o: u64, n: usize| -> R<Vec<u8>> {
            let mut b = vec![0u8; n];
            f.read_exact_at(&mut b, o)
                .map_err(|e| format!("{}: {e}", path.display()))?;
            Ok(b)
        };
        let head = rd(0, 64)?;
        let (base, segs, relocs, chains, symbol) = if &head[..4] == b"\x7fELF" {
            let (segs, relocs) = elf(&rd, &head)?;
            (0, segs, relocs, None, None)
        } else if &head[..2] == b"MZ" {
            return Err("binary: a Windows DLL (PE is not read yet)".into());
        } else {
            let (base, segs, chains, symbol) = macho(&rd, &head)?;
            (base, segs, Vec::new(), chains, symbol)
        };
        let mut t = Types {
            f,
            base,
            segs,
            relocs,
            chains,
            types: 0,
            count: 0,
            cache: HashMap::new(),
        };
        let found = match symbol {
            Some(reg) => t.registration(reg, typedefs).map(|(c, ty)| (reg, c, ty)),
            None => t.find_registration(typedefs),
        };
        let (_, count, types) = found.ok_or_else(|| {
            format!(
                "binary: no metadata registration of {typedefs} types (another build's metadata?)"
            )
        })?;
        t.count = count;
        t.types = types;
        Ok(t)
    }
}

type Seg = (u64, u64, u64, bool);
/// (address, value) of the pointers a loader writes.
type Relocs = Vec<(u64, u64)>;

/// A Mach-O's chained fixups from the data of its LC_DYLD_CHAINED_FIXUPS command: which pages of
/// which segments have chains. The imports are not read: a bind is no address of this image.
fn chains(b: &[u8], segs: &[Seg]) -> R<Chains> {
    let bad = |what: &str| format!("binary: chained fixups: {what}");
    // (the segment the file starts with holds the header: the image's address)
    let image = segs
        .iter()
        .find(|s| s.1 == 0 && s.2 > 0)
        .map(|s| s.0)
        .ok_or_else(|| bad("no segment at the file's start"))?;
    let u32_at = |o: usize| u32_at(b, o).map_err(|_| bad("cut short"));
    let u16_at = |o: usize| u16_at(b, o).map_err(|_| bad("cut short"));
    if u32_at(0)? != 0 {
        return Err(bad(&format!("version {}", u32_at(0)?)));
    }
    let starts = u32_at(4)? as usize;
    let count = u32_at(starts)? as usize;
    let mut out = Vec::new();
    for i in 0..count {
        let o = u32_at(starts + 4 + 4 * i)? as usize;
        if o == 0 {
            continue;
        }
        let s = starts + o;
        let (size, layout) = (u16_at(s + 4)? as u64, u16_at(s + 6)?);
        if layout != 2 && layout != 6 {
            return Err(bad(&format!(
                "pointer format {layout} (the plain 64-bit ones, 2 and 6, are read; arm64e's signed pointers are not)"
            )));
        }
        if !size.is_power_of_two() || size < 0x1000 {
            return Err(bad(&format!("pages of {size:#x} bytes")));
        }
        let offset = u64::from(u32_at(s + 8)?) | u64::from(u32_at(s + 12)?) << 32;
        let pages = u16_at(s + 20)? as usize;
        let mut first = Vec::with_capacity(pages);
        for k in 0..pages {
            let start = u16_at(s + 22 + 2 * k)?;
            if start != NO_CHAIN && u64::from(start) >= size {
                return Err(bad(&format!("a page's first fixup at {start:#x}")));
            }
            first.push(start);
        }
        out.push((image.wrapping_add(offset), size, layout, first));
    }
    Ok(Chains {
        image,
        segs: out,
        pages: RefCell::new(HashMap::new()),
    })
}

/// A 64-bit Mach-O's slice offset, its segments, its chained fixups (if it has that form) and its
/// g_MetadataRegistration symbol (if kept).
fn macho(
    rd: &dyn Fn(u64, usize) -> R<Vec<u8>>,
    head: &[u8],
) -> R<(u64, Vec<Seg>, Option<Chains>, Option<u64>)> {
    let magic_be = u32::from_be_bytes(head[..4].try_into().unwrap());
    let mut base = 0u64;
    if magic_be == 0xCAFE_BABE {
        let n = u32::from_be_bytes(head[4..8].try_into().unwrap()) as u64;
        let mut pick = None;
        for i in 0..n.min(16) {
            let e = rd(8 + 20 * i, 20)?;
            let cpu = u32::from_be_bytes(e[..4].try_into().unwrap());
            let off = u32::from_be_bytes(e[8..12].try_into().unwrap()) as u64;
            // (arm64 first, else x86_64)
            if cpu == 0x0100_000C || (cpu == 0x0100_0007 && pick.is_none()) {
                pick = Some(off);
            }
        }
        base = pick.ok_or("binary: a universal file without a 64-bit slice")?;
    }
    let h = rd(base, 32)?;
    if u32::from_le_bytes(h[..4].try_into().unwrap()) != 0xFEED_FACF {
        return Err("binary: not a 64-bit Mach-O or ELF".into());
    }
    let ncmds = u32::from_le_bytes(h[16..20].try_into().unwrap());
    let sizeofcmds = u32::from_le_bytes(h[20..24].try_into().unwrap()) as usize;
    let cmds = rd(base + 32, sizeofcmds)?;
    let (mut segs, mut symtab, mut p) = (Vec::new(), None, 0usize);
    let (mut dyld_info, mut fixups) = (false, None);
    for _ in 0..ncmds {
        let cmd = u32_at(&cmds, p)?;
        let size = u32_at(&cmds, p + 4)? as usize;
        match cmd {
            0x19 => {
                let q = |k: usize| {
                    u64::from_le_bytes(cmds[p + 24 + 8 * k..p + 32 + 8 * k].try_into().unwrap())
                };
                let name = &cmds[p + 8..p + 24];
                let data = name.starts_with(b"__DATA");
                segs.push((q(0), q(2), q(3), data));
            }
            0x2 => {
                symtab = Some((
                    u32_at(&cmds, p + 8)?,
                    u32_at(&cmds, p + 12)?,
                    u32_at(&cmds, p + 16)?,
                ))
            }
            0x8000_0022 | 0x22 => dyld_info = true,
            0x8000_0034 => fixups = Some((u32_at(&cmds, p + 8)?, u32_at(&cmds, p + 12)?)),
            _ => {}
        }
        if size == 0 {
            break;
        }
        p += size;
    }
    // (the loader's rule, runtime/macho/macho.c: one form of fixups, not both, not neither)
    let chains = match (dyld_info, fixups) {
        (true, None) => None,
        (false, Some((off, size))) => {
            Some(chains(&rd(base + off as u64, size as usize)?, &segs)?)
        }
        (true, Some(_)) => return Err("binary: dyld info and chained fixups both".into()),
        (false, None) => return Err("binary: neither dyld info nor chained fixups".into()),
    };
    let mut reg = None;
    if let Some((symoff, nsyms, stroff)) = symtab {
        let syms = rd(base + symoff as u64, 16 * nsyms as usize)?;
        let want = b"_g_MetadataRegistration\0";
        for i in 0..nsyms as usize {
            let e = &syms[16 * i..16 * i + 16];
            let ty = e[4];
            // (a defined symbol in a section, not a debugger stab)
            if ty & 0xE0 != 0 || ty & 0x0E != 0x0E {
                continue;
            }
            let strx = u32::from_le_bytes(e[..4].try_into().unwrap()) as u64;
            if rd(base + stroff as u64 + strx, want.len()).is_ok_and(|n| n == want) {
                reg = Some(u64::from_le_bytes(e[8..16].try_into().unwrap()));
                break;
            }
        }
    }
    Ok((base, segs, chains, reg))
}

/// A 64-bit little-endian ELF's loaded segments and its RELATIVE relocations (address, value),
/// sorted. Packed relocations (Android's, RELR) are refused: their pointers would be read wrong.
fn elf(rd: &dyn Fn(u64, usize) -> R<Vec<u8>>, h: &[u8]) -> R<(Vec<Seg>, Relocs)> {
    if h[4] != 2 || h[5] != 1 {
        return Err("binary: a 32-bit or big-endian ELF (not read yet)".into());
    }
    let u16_ = |o: usize| u16::from_le_bytes([h[o], h[o + 1]]);
    let u64_ = |b: &[u8], o: usize| u64::from_le_bytes(b[o..o + 8].try_into().unwrap());
    let relative = match u16_(0x12) {
        0xB7 => 1027, // R_AARCH64_RELATIVE
        0x3E => 8,    // R_X86_64_RELATIVE
        m => return Err(format!("binary: an ELF for machine {m:#x} (not read yet)")),
    };
    let (phoff, phentsize, phnum) = (u64_(h, 0x20), u16_(0x36) as usize, u16_(0x38) as usize);
    let ph = rd(phoff, phentsize * phnum)?;
    let (mut segs, mut dynamic) = (Vec::new(), None);
    for i in 0..phnum {
        let e = &ph[i * phentsize..(i + 1) * phentsize];
        let ty = u32::from_le_bytes(e[..4].try_into().unwrap());
        let flags = u32::from_le_bytes(e[4..8].try_into().unwrap());
        let (off, va, filesz) = (u64_(e, 8), u64_(e, 16), u64_(e, 32));
        match ty {
            1 => segs.push((va, off, filesz, flags & 1 == 0)), // PT_LOAD; data: not executable
            2 => dynamic = Some((off, filesz)),                // PT_DYNAMIC
            _ => {}
        }
    }
    let (doff, dsize) = dynamic.ok_or("binary: an ELF without a dynamic section")?;
    let d = rd(doff, dsize as usize)?;
    let (mut rela, mut relasz, mut relaent) = (None, 0u64, 24u64);
    for k in 0..d.len() / 16 {
        let (tag, val) = (u64_(&d, 16 * k) as i64, u64_(&d, 16 * k + 8));
        match tag {
            0 => break,
            7 => rela = Some(val),
            8 => relasz = val,
            9 => relaent = val,
            0x24 | 0x6000_000F | 0x6000_0011 | 0x6FFF_E000 => {
                return Err("binary: packed relocations (not read yet)".into())
            }
            _ => {}
        }
    }
    let mut relocs = Vec::new();
    if let Some(va) = rela {
        let off = segs
            .iter()
            .find(|&&(a, _, n, _)| a <= va && va < a + n)
            .map(|&(a, o, _, _)| va - a + o)
            .ok_or("binary: the relocations outside the segments")?;
        let r = rd(off, relasz as usize)?;
        let ent = relaent.max(24) as usize;
        for k in 0..r.len() / ent {
            let e = &r[k * ent..k * ent + 24];
            if u64_(e, 8) & 0xFFFF_FFFF == relative {
                relocs.push((u64_(e, 0), u64_(e, 16)));
            }
        }
    }
    relocs.sort_unstable();
    Ok((segs, relocs))
}

/// An IL2CPP build's metadata and the binary with its type table, from its data folder (`<Game>_Data`,
/// a macOS app's `Contents/Resources/Data`): None when the build has no IL2CPP metadata.
pub fn files_of(data: &Path) -> Option<(std::path::PathBuf, std::path::PathBuf)> {
    let meta = data
        .join("il2cpp_data")
        .join("Metadata")
        .join("global-metadata.dat");
    if !meta.is_file() {
        return None;
    }
    let up = data.parent()?;
    let candidates = [
        up.parent()
            .map(|c| c.join("Frameworks").join("GameAssembly.dylib")), // (macOS: Contents/Frameworks)
        Some(up.join("GameAssembly.dll")),
        Some(up.join("GameAssembly.so")),
    ];
    let bin = candidates.into_iter().flatten().find(|p| p.is_file())?;
    Some((meta, bin))
}

/// An IL2CPP build's metadata and type table, its images made into assemblies on demand.
pub struct Il2Cpp {
    m: Metadata,
    t: Types,
    /// type definition -> image
    image_of: Vec<u32>,
}

/// One image's tables being built.
struct Builder {
    strings: Vec<u8>,
    string_at: HashMap<String, u32>,
    blob: Vec<u8>,
    blob_at: HashMap<Vec<u8>, u32>,
    tables: HashMap<u8, Vec<Vec<u32>>>,
    typeref_at: HashMap<usize, u32>,
    asmref_at: HashMap<u32, u32>,
    ctor_at: HashMap<u32, u32>,
}

impl Builder {
    fn string(&mut self, s: &str) -> u32 {
        if s.is_empty() {
            return 0;
        }
        if let Some(&i) = self.string_at.get(s) {
            return i;
        }
        let i = self.strings.len() as u32;
        self.strings.extend(s.as_bytes());
        self.strings.push(0);
        self.string_at.insert(s.to_string(), i);
        i
    }

    fn blob(&mut self, b: Vec<u8>) -> u32 {
        if let Some(&i) = self.blob_at.get(&b) {
            return i;
        }
        let i = self.blob.len() as u32;
        ecma_u32(&mut self.blob, b.len() as u32);
        self.blob.extend(&b);
        self.blob_at.insert(b, i);
        i
    }

    /// Appends a row; its 1-based number.
    fn row(&mut self, t: u8, r: Vec<u32>) -> u32 {
        let v = self.tables.entry(t).or_default();
        v.push(r);
        v.len() as u32
    }
}

impl Il2Cpp {
    /// The metadata and the binary of an IL2CPP build.
    pub fn open(metadata: &Path, binary: &Path) -> R<Il2Cpp> {
        let m = Metadata::open(metadata)?;
        let t = Types::open(binary, m.typedef_count())?;
        let mut image_of = vec![u32::MAX; m.typedef_count()];
        for (i, im) in m.images.iter().enumerate() {
            for k in im.type_start..im.type_start + im.type_count {
                image_of[k as usize] = i as u32;
            }
        }
        Ok(Il2Cpp { m, t, image_of })
    }

    /// The image names, without ".dll".
    pub fn names(&self) -> Vec<String> {
        self.m
            .images
            .iter()
            .map(|i| i.name.strip_suffix(".dll").unwrap_or(&i.name).to_string())
            .collect()
    }

    /// The image `name` (with or without ".dll") as an assembly, or None.
    pub fn assembly(&mut self, name: &str) -> R<Option<Assembly>> {
        let Some(img) = self
            .m
            .images
            .iter()
            .position(|i| i.name == name || i.name.strip_suffix(".dll") == Some(name))
        else {
            return Ok(None);
        };
        let im_start = self.m.images[img].type_start as usize;
        let im_count = self.m.images[img].type_count as usize;
        let mut b = Builder {
            strings: vec![0],
            string_at: HashMap::new(),
            blob: vec![0],
            blob_at: HashMap::new(),
            tables: HashMap::new(),
            typeref_at: HashMap::new(),
            asmref_at: HashMap::new(),
            ctor_at: HashMap::new(),
        };
        let modname = self.m.images[img].name.clone();
        let mn = b.string(&modname);
        b.row(0x00, vec![0, mn, 0, 0, 0]);
        // type definitions first (the rows other tables and signatures refer to)
        let mut field_rows: Vec<(usize, u32)> = Vec::new();
        let mut next_field = 1u32;
        let mut typedefs = Vec::with_capacity(im_count);
        for k in 0..im_count {
            let td = self.m.typedef(im_start + k)?;
            typedefs.push(td);
        }
        for td in &typedefs {
            let name = self.m.string(td.name)?;
            let ns = self.m.string(td.ns)?;
            let (n, s) = (b.string(&name), b.string(&ns));
            b.row(TYPEDEF, vec![td.flags, n, s, 0, next_field, 1]);
            for f in 0..td.field_count as usize {
                field_rows.push((td.field_start as usize + f, next_field));
                next_field += 1;
            }
        }
        for (k, td) in typedefs.iter().enumerate() {
            let row = k as u32 + 1;
            if td.parent >= 0 {
                let ext = self.type_def_or_ref_of(&mut b, img, td.parent)?;
                b.tables.get_mut(&TYPEDEF).unwrap()[k][3] = ext;
            }
            if td.declaring >= 0 {
                let (data, bits) = self.t.at(self.t.addr(td.declaring)?)?;
                let ty = (bits >> 16) as u8;
                if ty == T_CLASS || ty == T_VALUETYPE {
                    let outer = data as usize;
                    if self.image_of.get(outer) == Some(&(img as u32)) {
                        b.row(NESTEDCLASS, vec![row, (outer - im_start) as u32 + 1]);
                    }
                }
            }
            if td.gcontainer >= 0 {
                let (count, first) = self.m.gcontainer(td.gcontainer as usize)?;
                for p in 0..count {
                    let (num, flags, name) = self.m.gparam((first + p) as usize)?;
                    let n = self.m.string(name)?;
                    let n = b.string(&n);
                    // (owner: TypeOrMethodDef, a TypeDef)
                    b.row(GENERICPARAM, vec![num as u32, flags as u32, row << 1, n]);
                }
            }
        }
        for &(fi, _) in &field_rows {
            let (name, tidx, _) = self.m.field(fi)?;
            let (_, bits) = self.t.at(self.t.addr(tidx)?)?;
            let mut sig = vec![0x06];
            self.sig(&mut b, img, self.t.addr(tidx)?, &mut sig, 0)?;
            let n = self.m.string(name)?;
            let (n, s) = (b.string(&n), b.blob(sig));
            b.row(FIELD, vec![bits & 0xFFFF, n, s]);
        }
        // the fields' custom attributes
        let ranges = self.m.attribute_ranges(img)?;
        for &(fi, frow) in &field_rows {
            let (_, _, token) = self.m.field(fi)?;
            let Some(&range) = ranges.get(&token) else {
                continue;
            };
            for at in self.m.attribute_types(range)? {
                let ctor = self.ctor_of(&mut b, img, at)?;
                // (parent: HasCustomAttribute, a Field; type: CustomAttributeType, a MemberRef)
                b.row(CUSTOMATTRIBUTE, vec![(frow << 5) | 1, (ctor << 3) | 3, 0]);
            }
        }
        let mut tables: Vec<(u8, Vec<Vec<u32>>)> = b.tables.into_iter().collect();
        tables.sort_by_key(|(t, _)| *t);
        Ok(Some(Assembly::synthetic(
            &modname, tables, b.strings, b.blob,
        )))
    }

    /// A MemberRef `.ctor` of an attribute type (its row).
    fn ctor_of(&mut self, b: &mut Builder, img: usize, td: usize) -> R<u32> {
        if let Some(&r) = b.ctor_at.get(&(td as u32)) {
            return Ok(r);
        }
        let parent = match self.local_row(img, td) {
            Some(row) => row << 3,                   // MemberRefParent: TypeDef
            None => (self.typeref(b, td)? << 3) | 1, // TypeRef
        };
        let (n, s) = (b.string(".ctor"), b.blob(vec![0x20, 0, 0x01]));
        let r = b.row(MEMBERREF, vec![parent, n, s]);
        b.ctor_at.insert(td as u32, r);
        Ok(r)
    }

    fn local_row(&self, img: usize, td: usize) -> Option<u32> {
        (self.image_of.get(td) == Some(&(img as u32)))
            .then(|| (td - self.m.images[img].type_start as usize) as u32 + 1)
    }

    /// A TypeRef row for a type definition of another image (an enclosing TypeRef for a nested one).
    fn typeref(&mut self, b: &mut Builder, td: usize) -> R<u32> {
        if let Some(&r) = b.typeref_at.get(&td) {
            return Ok(r);
        }
        let d = self.m.typedef(td)?;
        let scope = if d.declaring >= 0 {
            let (data, bits) = self.t.at(self.t.addr(d.declaring)?)?;
            let ty = (bits >> 16) as u8;
            if ty != T_CLASS && ty != T_VALUETYPE {
                return Err(format!(
                    "type definition {td}: declaring type of kind {ty:#x}"
                ));
            }
            (self.typeref(b, data as usize)? << 2) | 3 // ResolutionScope: TypeRef
        } else {
            let other = *self
                .image_of
                .get(td)
                .ok_or("type definition past the table")?;
            let r = match b.asmref_at.get(&other) {
                Some(&r) => r,
                None => {
                    let name = self.names()[other as usize].clone();
                    let n = b.string(&name);
                    let r = b.row(ASSEMBLYREF, vec![0, 0, 0, 0, 0, 0, n, 0, 0]);
                    b.asmref_at.insert(other, r);
                    r
                }
            };
            (r << 2) | 2 // AssemblyRef
        };
        let name = self.m.string(d.name)?;
        let ns = self.m.string(d.ns)?;
        let (n, s) = (b.string(&name), b.string(&ns));
        let r = b.row(TYPEREF, vec![scope, n, s]);
        b.typeref_at.insert(td, r);
        Ok(r)
    }

    /// A TypeDefOrRef coded index for a type definition (as a table column, not a signature).
    fn type_def_or_ref(&mut self, b: &mut Builder, img: usize, td: usize) -> R<u32> {
        Ok(match self.local_row(img, td) {
            Some(r) => r << 2,
            None => (self.typeref(b, td)? << 2) | 1,
        })
    }

    /// A TypeDefOrRef coded index for a type of the table (a TypeSpec for a generic instance).
    fn type_def_or_ref_of(&mut self, b: &mut Builder, img: usize, tidx: i32) -> R<u32> {
        let va = self.t.addr(tidx)?;
        let (data, bits) = self.t.at(va)?;
        match (bits >> 16) as u8 {
            T_CLASS | T_VALUETYPE => self.type_def_or_ref(b, img, data as usize),
            _ => {
                let mut sig = Vec::new();
                self.sig(b, img, va, &mut sig, 0)?;
                let s = b.blob(sig);
                Ok((b.row(TYPESPEC, vec![s]) << 2) | 2)
            }
        }
    }

    /// The signature of the Il2CppType at `va`, appended.
    fn sig(
        &mut self,
        b: &mut Builder,
        img: usize,
        va: u64,
        out: &mut Vec<u8>,
        depth: usize,
    ) -> R<()> {
        if depth > 32 {
            return Err("type nested too deep".into());
        }
        let (data, bits) = self.t.at(va)?;
        let ty = (bits >> 16) as u8;
        match ty {
            T_CLASS | T_VALUETYPE => {
                out.push(ty);
                let c = self.type_def_or_ref(b, img, data as usize)?;
                ecma_u32(out, c);
            }
            T_SZARRAY => {
                out.push(ty);
                self.sig(b, img, data, out, depth + 1)?;
            }
            T_ARRAY => {
                // (Il2CppArrayType: the element type, then the rank)
                let et = self.t.u64_va(data)?;
                let rank = self.t.read(self.t.file_off(data + 8)?, 1)?[0];
                out.push(ty);
                self.sig(b, img, et, out, depth + 1)?;
                out.extend([rank, 0, 0]);
            }
            T_GENERICINST => {
                // (Il2CppGenericClass: the generic type, then its context's class instance)
                let gt = self.t.u64_va(data)?;
                let inst = self.t.u64_va(data + 8)?;
                let (gdata, gbits) = self.t.at(gt)?;
                let gty = (gbits >> 16) as u8;
                if gty != T_CLASS && gty != T_VALUETYPE {
                    return Err(format!("generic instance of a type of kind {gty:#x}"));
                }
                let argc = self.t.u64_va(inst)?;
                let argv = self.t.u64_va(inst + 8)?;
                if argc > 64 {
                    return Err(format!("generic instance with {argc} arguments"));
                }
                out.push(ty);
                out.push(gty);
                let c = self.type_def_or_ref(b, img, gdata as usize)?;
                ecma_u32(out, c);
                ecma_u32(out, argc as u32);
                for k in 0..argc {
                    let a = self.t.u64_va(argv + 8 * k)?;
                    self.sig(b, img, a, out, depth + 1)?;
                }
            }
            T_VAR | T_MVAR => {
                let (num, _, _) = self.m.gparam(data as usize)?;
                out.push(ty);
                ecma_u32(out, num as u32);
            }
            0x01..=0x0E | 0x16 | 0x18 | 0x19 | 0x1C => out.push(ty),
            // (pointers, function pointers and the like: never serialized; read as IntPtr)
            _ => out.push(0x18),
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compressed_integers() {
        assert_eq!(compressed_u32(&[0x05], 0), Ok((5, 1)));
        assert_eq!(compressed_u32(&[0x81, 0x02], 0), Ok((0x102, 2)));
        assert_eq!(
            compressed_u32(&[0xC1, 0x02, 0x03, 0x04], 0),
            Ok((0x0102_0304, 4))
        );
        assert_eq!(
            compressed_u32(&[0xF0, 1, 0, 0, 0x80], 0),
            Ok((0x8000_0001, 5))
        );
        assert_eq!(compressed_u32(&[0xFF], 0), Ok((u32::MAX, 1)));
        let mut v = Vec::new();
        for x in [0x7F, 0x80, 0x3FFF, 0x4000] {
            ecma_u32(&mut v, x);
        }
        assert_eq!(v, [0x7F, 0x80, 0x80, 0xBF, 0xFF, 0xC0, 0, 0x40, 0]);
    }

    /// A Mach-O as a linker lays one out, with a metadata registration of `TYPEDEFS` type
    /// definitions and a table of six types, in one of the forms its pointers can have: None for
    /// dyld info (the words hold the addresses), else chained fixups in a pointer layout. The
    /// table lies across a page's end, a page has no pointer at all, one page's chain ends at
    /// the page's last word, and one word is a bind.
    const IMAGE: u64 = 0x1_8000_0000;
    const DATA: u64 = IMAGE + 0x4000;
    const TYPEDEFS: usize = 0x4010; // (a count that, as an offset, would be an address of the image)
    const TABLE: u64 = DATA + 0x3FE8;
    const STRUCTS: u64 = DATA + 0x5000;
    const BIND: u64 = DATA + 0x4100;
    const PLAIN: u64 = DATA + 0x8010;

    fn image(form: Option<u16>, tweak: &dyn Fn(&mut Vec<u8>)) -> Vec<u8> {
        let mut data = vec![0u8; 0xC000];
        let mut pointers: Vec<(u64, u64)> = Vec::new();
        let put = |d: &mut Vec<u8>, va: u64, v: u64| {
            let o = (va - DATA) as usize;
            d[o..o + 8].copy_from_slice(&v.to_le_bytes());
        };
        // the registration: counts and pointers in turn
        let reg = DATA + 0x100;
        put(&mut data, reg + 8 * 6, 6);
        pointers.push((reg + 8 * 7, TABLE));
        put(&mut data, reg + 8 * 10, TYPEDEFS as u64);
        pointers.push((reg + 8 * 11, DATA + 0x200));
        put(&mut data, reg + 8 * 12, TYPEDEFS as u64);
        pointers.push((reg + 8 * 13, DATA + 0x300));
        // the table, and the types: the even ones name a type definition, the odd ones point
        // to the type before
        for i in 0..6u64 {
            pointers.push((TABLE + 8 * i, STRUCTS + 16 * i));
            if i % 2 == 0 {
                put(&mut data, STRUCTS + 16 * i, 0x1000 + i);
            } else {
                pointers.push((STRUCTS + 16 * i, STRUCTS + 16 * (i - 1)));
            }
            put(&mut data, STRUCTS + 16 * i + 8, 0x0012_0000 + i);
        }
        put(&mut data, PLAIN, TYPEDEFS as u64);
        pointers.sort_unstable();

        let mut link = Vec::new();
        if let Some(layout) = form {
            // each page's chain: target, the distance to the next in 4-byte steps
            let mut starts = [NO_CHAIN; 3];
            let mut words: Vec<(u64, u64)> = pointers
                .iter()
                .map(|&(at, to)| (at, if layout == 6 { to - IMAGE } else { to }))
                .collect();
            words.push((BIND, 1 << 63 | 5));
            words.sort_unstable();
            for (k, &(at, word)) in words.iter().enumerate() {
                let (page, off) = ((at - DATA) / 0x4000, (at - DATA) % 0x4000);
                if starts[page as usize] == NO_CHAIN {
                    starts[page as usize] = off as u16;
                }
                let next = words
                    .get(k + 1)
                    .filter(|n| (n.0 - DATA) / 0x4000 == page)
                    .map_or(0, |n| (n.0 - at) / 4);
                put(&mut data, at, word | next << 51);
            }
            // the header, the segments' offsets (only __DATA has chains), __DATA's starts
            for v in [0u32, 32, 0, 0, 1, 1, 0, 0, 3, 0, 16, 0] {
                link.extend(v.to_le_bytes());
            }
            link.extend(28u32.to_le_bytes());
            link.extend(0x4000u16.to_le_bytes());
            link.extend(layout.to_le_bytes());
            link.extend(0x4000u64.to_le_bytes());
            link.extend(0u32.to_le_bytes());
            link.extend(3u16.to_le_bytes());
            for s in starts {
                link.extend(s.to_le_bytes());
            }
        } else {
            for &(at, to) in &pointers {
                put(&mut data, at, to);
            }
            link.extend([0u8; 16]);
        }

        let mut cmds = Vec::new();
        let seg = |c: &mut Vec<u8>, name: &str, va: u64, off: u64, size: u64| {
            c.extend(0x19u32.to_le_bytes());
            c.extend(72u32.to_le_bytes());
            let mut n = [0u8; 16];
            n[..name.len()].copy_from_slice(name.as_bytes());
            c.extend(n);
            for v in [va, size, off, size] {
                c.extend(v.to_le_bytes());
            }
            c.extend([0u8; 16]);
        };
        seg(&mut cmds, "__TEXT", IMAGE, 0, 0x4000);
        seg(&mut cmds, "__DATA", DATA, 0x4000, 0xC000);
        seg(&mut cmds, "__LINKEDIT", IMAGE + 0x10000, 0x10000, link.len() as u64);
        let mut ncmds = 3u32;
        if form.is_some() {
            cmds.extend(0x8000_0034u32.to_le_bytes());
            cmds.extend(16u32.to_le_bytes());
            cmds.extend(0x10000u32.to_le_bytes());
            cmds.extend((link.len() as u32).to_le_bytes());
        } else {
            cmds.extend(0x8000_0022u32.to_le_bytes());
            cmds.extend(48u32.to_le_bytes());
            cmds.extend([0u8; 40]);
        }
        ncmds += 1;
        let mut b = Vec::new();
        for v in [0xFEED_FACFu32, 0x0100_000C, 0, 6, ncmds, cmds.len() as u32, 0, 0] {
            b.extend(v.to_le_bytes());
        }
        b.extend(&cmds);
        b.resize(0x4000, 0);
        b.extend(&data);
        b.extend(&link);
        tweak(&mut b);
        b
    }

    fn open(name: &str, b: &[u8]) -> R<Types> {
        let p = std::env::temp_dir().join(format!("pmu-il2cpp-{}-{name}", std::process::id()));
        std::fs::write(&p, b).unwrap();
        let t = Types::open(&p, TYPEDEFS);
        std::fs::remove_file(&p).unwrap();
        t
    }

    /// The type table is read the same whichever form the binary's pointers have.
    #[test]
    fn macho_pointer_forms() {
        for form in [None, Some(6), Some(2)] {
            let mut t = open(&format!("form-{form:?}"), &image(form, &|_| {}))
                .unwrap_or_else(|e| panic!("{form:?}: {e}"));
            assert_eq!((t.count, t.types), (6, TABLE), "{form:?}");
            for i in 0..6u64 {
                let at = t.addr(i as i32).unwrap();
                assert_eq!(at, STRUCTS + 16 * i, "{form:?}: type {i}");
                let data = if i % 2 == 0 {
                    0x1000 + i
                } else {
                    STRUCTS + 16 * (i - 1)
                };
                assert_eq!(t.at(at).unwrap(), (data, 0x0012_0000 + i as u32), "{form:?}: type {i}");
            }
            // a plain number in a page without fixups, and a word bound to another image
            assert_eq!(t.u64_va(PLAIN), Ok(TYPEDEFS as u64), "{form:?}");
            assert_eq!(t.u64_va(BIND), Ok(0), "{form:?}");
            assert!(t.addr(6).is_err());
        }
    }

    /// What is not read is refused, with the reason.
    #[test]
    fn macho_pointer_forms_refused() {
        let err = |name: &str, b: &[u8]| open(name, b).err().unwrap_or_else(|| panic!("{name}: read"));
        // arm64e's signed pointers
        let e = err("signed", &image(Some(1), &|_| {}));
        assert!(e.contains("pointer format 1"), "{e}");
        // both forms (the fixups command turned into a second dyld info), neither
        let both = image(Some(6), &|b| {
            let at = 32 + 3 * 72;
            b[at..at + 4].copy_from_slice(&0x8000_0034u32.to_le_bytes());
            b[16..20].copy_from_slice(&5u32.to_le_bytes());
            let n = u32::from_le_bytes(b[20..24].try_into().unwrap()) + 48;
            b[20..24].copy_from_slice(&n.to_le_bytes());
            let mut c = Vec::new();
            c.extend(0x8000_0022u32.to_le_bytes());
            c.extend(48u32.to_le_bytes());
            c.extend([0u8; 40]);
            b.splice(at + 16..at + 16, c);
            b.truncate(b.len() - 48);
        });
        let e = err("both", &both);
        assert!(e.contains("both"), "{e}");
        let neither = image(None, &|b| b[32 + 3 * 72..32 + 3 * 72 + 4].copy_from_slice(&0x1Bu32.to_le_bytes()));
        let e = err("neither", &neither);
        assert!(e.contains("neither"), "{e}");
        // a chain that walks out of its page (the last link of the table's second page made
        // the longest there is): reported when a word of that page is asked for
        let lost = image(Some(6), &|b| {
            let at = 0x4000 + (STRUCTS + 16 * 5 - DATA) as usize + 7;
            b[at] |= 0x7F;
            b[at - 1] |= 0xF8;
        });
        let e = open("lost", &lost).unwrap().addr(3).unwrap_err();
        assert!(e.contains("leaves its page"), "{e}");
        // fixups whose command gives them fewer bytes than their tables take
        let cut = image(Some(6), &|b| {
            b[32 + 3 * 72 + 12..32 + 3 * 72 + 16].copy_from_slice(&40u32.to_le_bytes())
        });
        let e = err("cut", &cut);
        assert!(e.contains("chained fixups: cut short"), "{e}");
    }

    /// The same on what a linker makes: runtime/macho/test/meta_reg.c as that folder's
    /// build-mac.sh links it on a Mac (opcode streams, chained fixups of both layouts, each
    /// with its symbols and stripped of them). Skipped where those files have not been built.
    #[test]
    fn macho_linker_output() {
        let out = concat!(env!("CARGO_MANIFEST_DIR"), "/../../../runtime/macho/test/out");
        for form in ["", "chained/", "chained11/"] {
            for name in ["meta_reg.dylib", "meta_reg_stripped.dylib"] {
                let p = format!("{out}/{form}{name}");
                if !Path::new(&p).is_file() {
                    eprintln!("macho_linker_output: no {p}, skipped");
                    continue;
                }
                let mut t = Types::open(Path::new(&p), 1234).unwrap_or_else(|e| panic!("{p}: {e}"));
                assert_eq!(t.chains.is_some(), !form.is_empty(), "{p}");
                assert_eq!(t.count, 4096, "{p}");
                let mut before = 0;
                for i in 0..4096u64 {
                    let at = t.addr(i as i32).unwrap();
                    let data = if i % 2 == 0 { 0x1000 + i } else { before };
                    assert_eq!(t.at(at).unwrap(), (data, 0x12_0000 + i as u32), "{p}: type {i}");
                    before = at;
                }
            }
        }
    }

    /// An opt-in check on a real build: PMU_IL2CPP_TEST=<global-metadata.dat>:<GameAssembly>[:<image>:<type>]
    #[test]
    fn real_build() {
        let Ok(spec) = std::env::var("PMU_IL2CPP_TEST") else {
            return;
        };
        let p: Vec<&str> = spec.split(':').collect();
        let mut il = Il2Cpp::open(Path::new(p[0]), Path::new(p[1])).unwrap();
        let (image, ty) = (
            p.get(2).copied().unwrap_or("Assembly-CSharp"),
            p.get(3).copied(),
        );
        let a = il.assembly(image).unwrap().expect("image");
        let types = a.types().unwrap();
        eprintln!("{image}: {} types, {} fields", types.len(), a.count(FIELD));
        if let Some(ty) = ty {
            let t = types.iter().find(|t| t.full == ty).expect("type");
            let rows = a.rows(FIELD).unwrap();
            for r in t.fields.0..t.fields.1 {
                let row = &rows[r as usize - 1];
                eprintln!(
                    "  {:#06x} {} {:02x?}",
                    row[0],
                    a.string(row[1]).unwrap(),
                    a.blob(row[2]).unwrap()
                );
            }
        }
    }
}
