//! A small reader of .NET assembly metadata (ECMA-335, partition II), as clrmeta.py: type and
//! method names, P/Invoke declarations (ImplMap -> ModuleRef), method signatures and field
//! constants. A read past the file's end or a malformed value is an Err (the Python's ValueError,
//! IndexError or struct.error), which the callers treat as the Python does.
use std::collections::{BTreeMap, HashMap};
use std::path::Path;
use std::sync::OnceLock;

pub const MODULE: u8 = 0;
pub const TYPEREF: u8 = 1;
pub const TYPEDEF: u8 = 2;
pub const FIELD: u8 = 4;
pub const METHODDEF: u8 = 6;
pub const PARAM: u8 = 8;
pub const CONSTANT: u8 = 0x0B;
pub const NESTEDCLASS: u8 = 0x29;
pub const CLASSLAYOUT: u8 = 0x0F;
pub const MODULEREF: u8 = 0x1A;
pub const IMPLMAP: u8 = 0x1C;
pub const ASSEMBLYREF: u8 = 0x23;

pub const E_VOID: u8 = 1;
pub const E_BOOL: u8 = 2;
pub const E_CHAR: u8 = 3;
pub const E_I1: u8 = 4;
pub const E_U1: u8 = 5;
pub const E_I2: u8 = 6;
pub const E_U2: u8 = 7;
pub const E_I4: u8 = 8;
pub const E_U4: u8 = 9;
pub const E_I8: u8 = 0x0A;
pub const E_U8: u8 = 0x0B;
pub const E_R4: u8 = 0x0C;
pub const E_R8: u8 = 0x0D;
pub const E_STRING: u8 = 0x0E;
pub const E_PTR: u8 = 0x0F;
pub const E_BYREF: u8 = 0x10;
pub const E_VALUETYPE: u8 = 0x11;
pub const E_CLASS: u8 = 0x12;
pub const E_VAR: u8 = 0x13;
pub const E_ARRAY: u8 = 0x14;
pub const E_GENERICINST: u8 = 0x15;
pub const E_TYPEDBYREF: u8 = 0x16;
pub const E_I: u8 = 0x18;
pub const E_U: u8 = 0x19;
pub const E_FNPTR: u8 = 0x1B;
pub const E_OBJECT: u8 = 0x1C;
pub const E_SZARRAY: u8 = 0x1D;
pub const E_MVAR: u8 = 0x1E;
pub const E_CMOD_REQD: u8 = 0x1F;
pub const E_CMOD_OPT: u8 = 0x20;
pub const E_SENTINEL: u8 = 0x41;
pub const E_PINNED: u8 = 0x45;

/// The element types clrmeta.PRIM names (the primitive types).
pub fn is_prim(e: u8) -> bool {
    matches!(
        e,
        E_BOOL..=E_STRING | E_I | E_U | E_OBJECT | E_VOID | E_TYPEDBYREF
    )
}

/// clrmeta.PRIM_SIZE (4 for the others).
pub fn prim_size(e: u8) -> Option<u32> {
    Some(match e {
        E_BOOL | E_I1 | E_U1 => 1,
        E_CHAR | E_I2 | E_U2 => 2,
        E_I4 | E_U4 | E_R4 | E_I | E_U => 4,
        E_I8 | E_U8 | E_R8 => 8,
        _ => return None,
    })
}

/// What reading an assembly raises in the Python: NotAssembly, ValueError (and IndexError: the
/// callers catch both alike), struct.error, KeyError (a missing stream), OSError.
#[derive(Debug, Clone)]
pub enum Error {
    NotAssembly(String),
    Value(String),
    Struct(String),
    Key(String),
    Io(String),
}

impl Error {
    /// Caught where the Python catches (NotAssembly, ValueError, IndexError, struct.error).
    pub fn is_bad_assembly(&self) -> bool {
        matches!(
            self,
            Error::NotAssembly(_) | Error::Value(_) | Error::Struct(_)
        )
    }

    /// Caught where the Python catches (ValueError, IndexError, struct.error).
    pub fn is_value(&self) -> bool {
        matches!(self, Error::Value(_) | Error::Struct(_))
    }
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::NotAssembly(m)
            | Error::Value(m)
            | Error::Struct(m)
            | Error::Key(m)
            | Error::Io(m) => f.write_str(m),
        }
    }
}

pub type Result<T> = std::result::Result<T, Error>;

fn bad<T>(m: impl Into<String>) -> Result<T> {
    Err(Error::Value(m.into()))
}

/// A column: a fixed size, a heap index, a table index or a coded index.
#[derive(Clone, Copy)]
enum Col {
    Fixed(usize),
    Str,
    Guid,
    Blob,
    Table(u8),
    Cx(Coded),
}

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub enum Coded {
    TypeDefOrRef,
    HasConstant,
    HasCustomAttribute,
    HasFieldMarshal,
    HasDeclSecurity,
    MemberRefParent,
    HasSemantics,
    MethodDefOrRef,
    MemberForwarded,
    Implementation,
    CustomAttributeType,
    ResolutionScope,
    TypeOrMethodDef,
}

fn coded_info(c: Coded) -> (u32, &'static [Option<u8>]) {
    use Coded::*;
    match c {
        TypeDefOrRef => (2, &[Some(TYPEDEF), Some(TYPEREF), Some(0x1B)]),
        HasConstant => (2, &[Some(FIELD), Some(PARAM), Some(0x17)]),
        HasCustomAttribute => (
            5,
            &[
                Some(METHODDEF),
                Some(FIELD),
                Some(TYPEREF),
                Some(TYPEDEF),
                Some(PARAM),
                Some(0x09),
                Some(0x0A),
                Some(MODULE),
                Some(0x0E),
                Some(0x17),
                Some(0x14),
                Some(0x11),
                Some(MODULEREF),
                Some(0x1B),
                Some(0x20),
                Some(ASSEMBLYREF),
                Some(0x26),
                Some(0x27),
                Some(0x28),
                Some(0x2A),
                Some(0x2C),
                Some(0x2B),
            ],
        ),
        HasFieldMarshal => (1, &[Some(FIELD), Some(PARAM)]),
        HasDeclSecurity => (2, &[Some(TYPEDEF), Some(METHODDEF), Some(0x20)]),
        MemberRefParent => (
            3,
            &[
                Some(TYPEDEF),
                Some(TYPEREF),
                Some(MODULEREF),
                Some(METHODDEF),
                Some(0x1B),
            ],
        ),
        HasSemantics => (1, &[Some(0x14), Some(0x17)]),
        MethodDefOrRef => (1, &[Some(METHODDEF), Some(0x0A)]),
        MemberForwarded => (1, &[Some(FIELD), Some(METHODDEF)]),
        Implementation => (2, &[Some(0x26), Some(ASSEMBLYREF), Some(0x27)]),
        CustomAttributeType => (3, &[None, None, Some(METHODDEF), Some(0x0A), None]),
        ResolutionScope => (
            2,
            &[
                Some(MODULE),
                Some(MODULEREF),
                Some(ASSEMBLYREF),
                Some(TYPEREF),
            ],
        ),
        TypeOrMethodDef => (1, &[Some(TYPEDEF), Some(METHODDEF)]),
    }
}

fn schema(t: u8) -> Option<Vec<Col>> {
    use Coded::*;
    use Col::*;
    let f = Fixed;
    Some(match t {
        0x00 => vec![f(2), Str, Guid, Guid, Guid],
        0x01 => vec![Cx(ResolutionScope), Str, Str],
        0x02 => vec![
            f(4),
            Str,
            Str,
            Cx(TypeDefOrRef),
            Table(FIELD),
            Table(METHODDEF),
        ],
        0x03 => vec![Table(FIELD)],
        0x04 => vec![f(2), Str, Blob],
        0x05 => vec![Table(METHODDEF)],
        0x06 => vec![f(4), f(2), f(2), Str, Blob, Table(PARAM)],
        0x07 => vec![Table(PARAM)],
        0x08 => vec![f(2), f(2), Str],
        0x09 => vec![Table(TYPEDEF), Cx(TypeDefOrRef)],
        0x0A => vec![Cx(MemberRefParent), Str, Blob],
        0x0B => vec![f(1), f(1), Cx(HasConstant), Blob],
        0x0C => vec![Cx(HasCustomAttribute), Cx(CustomAttributeType), Blob],
        0x0D => vec![Cx(HasFieldMarshal), Blob],
        0x0E => vec![f(2), Cx(HasDeclSecurity), Blob],
        0x0F => vec![f(2), f(4), Table(TYPEDEF)],
        0x10 => vec![f(4), Table(FIELD)],
        0x11 => vec![Blob],
        0x12 => vec![Table(TYPEDEF), Table(0x14)],
        0x13 => vec![Table(0x14)],
        0x14 => vec![f(2), Str, Cx(TypeDefOrRef)],
        0x15 => vec![Table(TYPEDEF), Table(0x17)],
        0x16 => vec![Table(0x17)],
        0x17 => vec![f(2), Str, Blob],
        0x18 => vec![f(2), Table(METHODDEF), Cx(HasSemantics)],
        0x19 => vec![Table(TYPEDEF), Cx(MethodDefOrRef), Cx(MethodDefOrRef)],
        0x1A => vec![Str],
        0x1B => vec![Blob],
        0x1C => vec![f(2), Cx(MemberForwarded), Str, Table(MODULEREF)],
        0x1D => vec![f(4), Table(FIELD)],
        0x1E => vec![f(4), f(4)],
        0x1F => vec![f(4)],
        0x20 => vec![f(4), f(2), f(2), f(2), f(2), f(4), Blob, Str, Str],
        0x21 => vec![f(4)],
        0x22 => vec![f(4), f(4), f(4)],
        0x23 => vec![f(2), f(2), f(2), f(2), f(4), Blob, Str, Str, Blob],
        0x24 => vec![f(4), Table(ASSEMBLYREF)],
        0x25 => vec![f(4), f(4), f(4), Table(ASSEMBLYREF)],
        0x26 => vec![f(4), Str, Blob],
        0x27 => vec![f(4), f(4), Str, Str, Cx(Implementation)],
        0x28 => vec![f(4), f(4), Str, Cx(Implementation)],
        0x29 => vec![Table(TYPEDEF), Table(TYPEDEF)],
        0x2A => vec![f(2), f(2), Cx(TypeOrMethodDef), Str],
        0x2B => vec![Cx(MethodDefOrRef), Blob],
        0x2C => vec![Table(0x2A), Cx(TypeDefOrRef)],
        _ => return None,
    })
}

/// A signature's type: ('prim', E_x) | ('ptr', t) | ('byref', t) | ('valuetype'|'class', name,
/// (table, row)) | ('class', name) | ('array', t) | ('fnptr',).
#[derive(Clone, Debug, PartialEq)]
pub enum SigType {
    Prim(u8),
    Ptr(Box<SigType>),
    ByRef(Box<SigType>),
    ValueType(String, Option<(u8, u32)>),
    Class(String, Option<(u8, u32)>),
    Array(Box<SigType>),
    FnPtr,
}

/// (return type, parameter types)
pub type MethodSig = (SigType, Vec<SigType>);

/// One TypeDef with its full name and member ranges.
#[derive(Clone, Debug)]
pub struct TypeInfo {
    pub ns: String,
    pub name: String,
    pub flags: u32,
    pub extends: (Option<u8>, u32),
    pub fields: (u32, u32),
    pub methods: (u32, u32),
    pub full: String,
}

/// A P/Invoke declaration.
#[derive(Clone, Debug)]
pub struct PInvoke {
    pub module: String,
    pub entry: String,
    pub method: String,
    pub typ: String,
    pub sig: Option<MethodSig>,
}

/// A literal field's value (field_constants).
#[derive(Clone, Debug, PartialEq)]
pub enum Const {
    Int(i64),
    UInt(u64),
    Str(String),
    None,
}

/// The metadata of one PE file with a CLI header.
pub struct Assembly {
    pub path: String,
    d: Vec<u8>,
    secs: Vec<(u32, u32, u32, u32)>,
    pub streams: HashMap<String, (usize, usize)>,
    nrows: BTreeMap<u8, u32>,
    tables: HashMap<u8, (usize, usize, Vec<usize>, u32)>,
    rows_cache: std::sync::Mutex<HashMap<u8, std::sync::Arc<Vec<Vec<u32>>>>>,
    types_cache: OnceLock<Result<Vec<TypeInfo>>>,
}

fn u16_at(d: &[u8], o: usize) -> Result<u16> {
    d.get(o..o + 2)
        .map(|b| u16::from_le_bytes([b[0], b[1]]))
        .ok_or_else(|| Error::Struct("unpack_from requires a buffer of at least 2 bytes".into()))
}

fn u32_at(d: &[u8], o: usize) -> Result<u32> {
    d.get(o..o + 4)
        .map(|b| u32::from_le_bytes(b.try_into().unwrap()))
        .ok_or_else(|| Error::Struct("unpack_from requires a buffer of at least 4 bytes".into()))
}

fn u64_at(d: &[u8], o: usize) -> Result<u64> {
    d.get(o..o + 8)
        .map(|b| u64::from_le_bytes(b.try_into().unwrap()))
        .ok_or_else(|| Error::Struct("unpack_from requires a buffer of at least 8 bytes".into()))
}

fn byte_at(d: &[u8], o: usize) -> Result<u8> {
    d.get(o)
        .copied()
        .ok_or_else(|| Error::Value("index out of range".into()))
}

fn index_of_nul(d: &[u8], o: usize) -> Result<usize> {
    d.get(o..)
        .and_then(|r| r.iter().position(|&c| c == 0))
        .map(|i| o + i)
        .ok_or_else(|| Error::Value("subsection not found".into()))
}

/// Columns that are table indices although their table number is 1, 2 or 4.
fn index_col(t: u8, k: usize) -> Option<u8> {
    Some(match (t, k) {
        (0x02, 4) | (0x03, 0) | (0x10, 1) | (0x1D, 1) => FIELD,
        (0x09, 0) | (0x0F, 2) | (0x12, 0) | (0x15, 0) | (0x19, 0) | (0x29, 0) | (0x29, 1) => {
            TYPEDEF
        }
        _ => return None,
    })
}

impl Assembly {
    pub fn open(path: impl AsRef<Path>) -> Result<Assembly> {
        let p = path.as_ref();
        let d = std::fs::read(p).map_err(|e| Error::Io(format!("{e}: {}", p.display())))?;
        Self::from_bytes(&p.to_string_lossy(), d)
    }

    pub fn from_bytes(path: &str, d: Vec<u8>) -> Result<Assembly> {
        let na = || Error::NotAssembly(path.to_string());
        if d.len() < 0x40 || &d[..2] != b"MZ" {
            return Err(na());
        }
        let pe = u32_at(&d, 0x3c)? as usize;
        if d.get(pe..pe + 4) != Some(b"PE\0\0") {
            return Err(na());
        }
        let nsec = u16_at(&d, pe + 6)? as usize;
        let optsz = u16_at(&d, pe + 20)? as usize;
        let opt = pe + 24;
        let magic = u16_at(&d, opt)?;
        let dirs = opt + if magic == 0x10b { 96 } else { 112 };
        let mut secs = Vec::new();
        for i in 0..nsec {
            let o = opt + optsz + 40 * i + 8;
            secs.push((
                u32_at(&d, o)?,
                u32_at(&d, o + 4)?,
                u32_at(&d, o + 8)?,
                u32_at(&d, o + 12)?,
            ));
        }
        let mut a = Assembly {
            path: path.to_string(),
            d,
            secs,
            streams: HashMap::new(),
            nrows: BTreeMap::new(),
            tables: HashMap::new(),
            rows_cache: Default::default(),
            types_cache: OnceLock::new(),
        };
        let cli_rva = u32_at(&a.d, dirs + 14 * 8)?;
        if cli_rva == 0 {
            return Err(na());
        }
        let cli = a.off(cli_rva)?;
        let md_rva = u32_at(&a.d, cli + 8)?;
        let md = a.off(md_rva)?;
        if a.d.get(md..md + 4) != Some(b"BSJB") {
            return Err(na());
        }
        let vlen = u32_at(&a.d, md + 12)? as usize;
        let mut p = md + 16 + vlen;
        let nstreams = u16_at(&a.d, p + 2)?;
        p += 4;
        for _ in 0..nstreams {
            let so = u32_at(&a.d, p)? as usize;
            let ss = u32_at(&a.d, p + 4)? as usize;
            let e = index_of_nul(&a.d, p + 8)?;
            let name = std::str::from_utf8(&a.d[p + 8..e])
                .map_err(|_| Error::Value("'utf-8' codec can't decode a stream name".into()))?
                .to_string();
            a.streams.insert(name, (md + so, ss));
            p = (e + 4) & !3;
        }
        a.read_tables()?;
        Ok(a)
    }

    pub fn off(&self, rva: u32) -> Result<usize> {
        for &(vsize, va, rsize, raw) in &self.secs {
            if va <= rva && (rva as u64) < va as u64 + vsize.max(rsize) as u64 {
                return Ok((rva - va + raw) as usize);
            }
        }
        Err(Error::NotAssembly(format!(
            "{}: rva {rva:#x} outside the sections",
            self.path
        )))
    }

    fn stream(&self, name: &str) -> Result<usize> {
        self.streams
            .get(name)
            .map(|s| s.0)
            .ok_or_else(|| Error::Key(format!("KeyError: '{name}'")))
    }

    /// A #Strings entry (UTF-8, bad bytes replaced).
    pub fn string(&self, i: u32) -> Result<String> {
        let o = self.stream("#Strings")? + i as usize;
        let e = index_of_nul(&self.d, o)?;
        Ok(String::from_utf8_lossy(&self.d[o..e]).into_owned())
    }

    /// A #Blob entry (sliced as Python slices: clipped at the file's end).
    pub fn blob(&self, i: u32) -> Result<Vec<u8>> {
        let o = self.stream("#Blob")? + i as usize;
        let (n, o) = cuint(&self.d, o)?;
        let end = (o + n as usize).min(self.d.len());
        Ok(self.d.get(o..end).map(<[u8]>::to_vec).unwrap_or_default())
    }

    fn read_tables(&mut self) -> Result<()> {
        let name = if self.streams.contains_key("#~") {
            "#~"
        } else {
            "#-"
        };
        let mut o = self.stream(name)?;
        let heaps = byte_at(&self.d, o + 6)?;
        let valid = u64_at(&self.d, o + 8)?;
        o += 24;
        for t in 0..64u8 {
            if valid >> t & 1 != 0 {
                self.nrows.insert(t, u32_at(&self.d, o)?);
                o += 4;
            }
        }
        if heaps & 0x40 != 0 {
            o += 4;
        }
        let sizes = |c: Col| match c {
            Col::Str => Some(if heaps & 1 != 0 { 4 } else { 2 }),
            Col::Guid => Some(if heaps & 2 != 0 { 4 } else { 2 }),
            Col::Blob => Some(if heaps & 4 != 0 { 4 } else { 2 }),
            _ => None,
        };
        let ts: Vec<u8> = self.nrows.keys().copied().collect();
        for t in ts {
            let Some(sch) = schema(t) else {
                return Err(Error::NotAssembly(format!(
                    "{}: unknown metadata table {t:#x}",
                    self.path
                )));
            };
            let mut css = Vec::new();
            for (k, c) in sch.iter().enumerate() {
                let s = if let Some(it) = index_col(t, k) {
                    if self.count(it) < 65536 {
                        2
                    } else {
                        4
                    }
                } else if let Col::Fixed(n) = c {
                    *n
                } else if let Some(s) = sizes(*c) {
                    s
                } else if let Col::Cx(k) = c {
                    let (bits, tabs) = coded_info(*k);
                    let big = tabs
                        .iter()
                        .flatten()
                        .map(|&x| self.count(x))
                        .max()
                        .unwrap_or(0);
                    if (big as u64) < (1u64 << (16 - bits)) {
                        2
                    } else {
                        4
                    }
                } else if let Col::Table(x) = c {
                    if self.count(*x) < 65536 {
                        2
                    } else {
                        4
                    }
                } else {
                    unreachable!()
                };
                css.push(s);
            }
            let rowsize: usize = css.iter().sum();
            let n = self.nrows[&t];
            self.tables.insert(t, (o, rowsize, css, n));
            o += rowsize * n as usize;
        }
        Ok(())
    }

    pub fn count(&self, t: u8) -> u32 {
        self.nrows.get(&t).copied().unwrap_or(0)
    }

    /// A table's rows (each column's value).
    pub fn rows(&self, t: u8) -> Result<std::sync::Arc<Vec<Vec<u32>>>> {
        if let Some(r) = self.rows_cache.lock().unwrap().get(&t) {
            return Ok(r.clone());
        }
        let r = std::sync::Arc::new(self.rows_of(t)?);
        self.rows_cache.lock().unwrap().insert(t, r.clone());
        Ok(r)
    }

    fn rows_of(&self, t: u8) -> Result<Vec<Vec<u32>>> {
        let Some((o, rs, css, n)) = self.tables.get(&t) else {
            return Ok(Vec::new());
        };
        let mut out = Vec::with_capacity(*n as usize);
        for i in 0..*n as usize {
            let mut p = o + i * rs;
            let mut row = Vec::with_capacity(css.len());
            for &s in css {
                row.push(match s {
                    1 => byte_at(&self.d, p)? as u32,
                    2 => u16_at(&self.d, p)? as u32,
                    _ => u32_at(&self.d, p)?,
                });
                p += s;
            }
            out.push(row);
        }
        Ok(out)
    }

    /// (table, 1-based row) of a coded index.
    pub fn coded(kind: Coded, v: u32) -> Result<(Option<u8>, u32)> {
        let (bits, tabs) = coded_info(kind);
        let tag = (v & ((1 << bits) - 1)) as usize;
        let t = tabs
            .get(tag)
            .ok_or_else(|| Error::Value("tuple index out of range".into()))?;
        Ok((*t, v >> bits))
    }

    /// The TypeDefs with full names (Outer/Inner for nested types) and member ranges.
    pub fn types(&self) -> Result<&Vec<TypeInfo>> {
        self.types_cache
            .get_or_init(|| self.types_of())
            .as_ref()
            .map_err(Clone::clone)
    }

    fn types_of(&self) -> Result<Vec<TypeInfo>> {
        let td = self.rows(TYPEDEF)?;
        let mut nested: HashMap<u32, u32> = HashMap::new();
        for r in self.rows(NESTEDCLASS)?.iter() {
            nested.insert(r[0], r[1]);
        }
        let nf = self.count(FIELD) + 1;
        let nm = self.count(METHODDEF) + 1;
        let mut out = Vec::with_capacity(td.len());
        for (i, r) in td.iter().enumerate() {
            let fe = td.get(i + 1).map_or(nf, |n| n[4]);
            let me = td.get(i + 1).map_or(nm, |n| n[5]);
            out.push(TypeInfo {
                ns: self.string(r[2])?,
                name: self.string(r[1])?,
                flags: r[0],
                extends: Self::coded(Coded::TypeDefOrRef, r[3])?,
                fields: (r[4], fe),
                methods: (r[5], me),
                full: String::new(),
            });
        }
        for i in 0..out.len() {
            let mut full = out[i].name.clone();
            let mut k = i as u32 + 1;
            let mut seen = std::collections::HashSet::new();
            while let Some(&outer) = nested.get(&k) {
                if !seen.insert(k) {
                    break;
                }
                k = outer;
                // (out[k - 1]: Python's negative index for k = 0 is the last type)
                let idx = (k as i64 - 1).rem_euclid(out.len() as i64) as usize;
                if k as usize > out.len() {
                    return bad("list index out of range");
                }
                full = format!("{}/{full}", out[idx].name);
                let ns = out[idx].ns.clone();
                out[i].ns = ns;
            }
            out[i].full = if out[i].ns.is_empty() {
                full
            } else {
                format!("{}.{full}", out[i].ns)
            };
        }
        Ok(out)
    }

    /// The type whose member list (methods or fields) holds row.
    fn owner(&self, methods: bool, row: u32) -> Result<Option<&TypeInfo>> {
        let ts = self.types()?;
        let range = |t: &TypeInfo| if methods { t.methods } else { t.fields };
        // bisect_right on the starts, then back over types with empty lists
        let starts: Vec<u32> = ts.iter().map(|t| range(t).0).collect();
        let mut i = bisect_right(&starts, row) as i64 - 1;
        while i >= 0 {
            let r = range(&ts[i as usize]);
            if r.0 <= row && row < r.1 {
                break;
            }
            i -= 1;
        }
        Ok(if i >= 0 { Some(&ts[i as usize]) } else { None })
    }

    pub fn typeref_name(&self, row: u32) -> Result<String> {
        let rows = self.rows(TYPEREF)?;
        let r = row_at(&rows, row)?;
        let (n, s) = (self.string(r[1])?, self.string(r[2])?);
        Ok(if s.is_empty() { n } else { format!("{s}.{n}") })
    }

    pub fn type_name(&self, tab: Option<u8>, row: u32) -> Result<String> {
        match tab {
            Some(TYPEDEF) => {
                let ts = self.types()?;
                Ok(py_index(ts, row as i64 - 1)?.full.clone())
            }
            Some(TYPEREF) => self.typeref_name(row),
            _ => Ok("?".into()),
        }
    }

    /// Every P/Invoke declaration.
    pub fn pinvokes(&self) -> Result<Vec<PInvoke>> {
        let mods: Vec<String> = self
            .rows(MODULEREF)?
            .iter()
            .map(|r| self.string(r[0]))
            .collect::<Result<_>>()?;
        let methods = self.rows(METHODDEF)?;
        let mut out = Vec::new();
        for r in self.rows(IMPLMAP)?.iter() {
            let (_flags, fwd, entry, scope) = (r[0], r[1], r[2], r[3]);
            let (tab, row) = Self::coded(Coded::MemberForwarded, fwd)?;
            if tab != Some(METHODDEF)
                || !(1..=methods.len() as u32).contains(&row)
                || !(1..=mods.len() as u32).contains(&scope)
            {
                continue;
            }
            let m = &methods[row as usize - 1];
            let t = self.owner(true, row)?;
            let name = self.string(m[3])?;
            let e = self.string(entry)?;
            out.push(PInvoke {
                module: mods[scope as usize - 1].clone(),
                entry: if e.is_empty() { name.clone() } else { e },
                method: name,
                typ: t.map_or("?".into(), |t| t.full.clone()),
                sig: self.method_sig(&self.blob(m[4])?)?,
            });
        }
        Ok(out)
    }

    /// {'Namespace.Type.field': value} of the literal fields, in the Constant table's order.
    pub fn field_constants(&self) -> Result<Vec<(String, Const)>> {
        let fields = self.rows(FIELD)?;
        let mut out: Vec<(String, Const)> = Vec::new();
        for r in self.rows(CONSTANT)?.iter() {
            let (typ, parent, val) = (r[0] as u8, r[2], r[3]);
            let (tab, row) = Self::coded(Coded::HasConstant, parent)?;
            if tab != Some(FIELD) || !(1..=fields.len() as u32).contains(&row) {
                continue;
            }
            let t = self.owner(false, row)?;
            let b = self.blob(val)?;
            let v = match typ {
                E_I4 if b.len() == 4 => {
                    Const::Int(i32::from_le_bytes(b[..4].try_into().unwrap()) as i64)
                }
                E_U4 if b.len() == 4 => {
                    Const::UInt(u32::from_le_bytes(b[..4].try_into().unwrap()) as u64)
                }
                E_I8 if b.len() == 8 => Const::Int(i64::from_le_bytes(b[..8].try_into().unwrap())),
                E_U8 if b.len() == 8 => Const::UInt(u64::from_le_bytes(b[..8].try_into().unwrap())),
                E_I2 if b.len() == 2 => Const::Int(i16::from_le_bytes([b[0], b[1]]) as i64),
                E_U2 if b.len() == 2 => Const::UInt(u16::from_le_bytes([b[0], b[1]]) as u64),
                E_I1 | E_U1 | E_BOOL if b.len() == 1 => Const::UInt(b[0] as u64),
                E_STRING => Const::Str(utf16le_replace(&b)),
                _ => Const::None,
            };
            if let Some(t) = t {
                let key = format!("{}.{}", t.full, self.string(fields[row as usize - 1][1])?);
                match out.iter_mut().find(|(k, _)| *k == key) {
                    Some(e) => e.1 = v,
                    None => out.push((key, v)),
                }
            }
        }
        Ok(out)
    }

    fn sig_type(&self, b: &[u8], mut o: usize) -> Result<(SigType, usize)> {
        let mut e = byte_at(b, o)?;
        o += 1;
        while e == E_CMOD_REQD || e == E_CMOD_OPT {
            o = cuint(b, o)?.1;
            e = byte_at(b, o)?;
            o += 1;
        }
        if e == E_PINNED {
            return self.sig_type(b, o);
        }
        if is_prim(e) {
            return Ok((SigType::Prim(e), o));
        }
        match e {
            E_PTR | E_BYREF => {
                let (t, o) = self.sig_type(b, o)?;
                Ok((
                    if e == E_PTR {
                        SigType::Ptr(Box::new(t))
                    } else {
                        SigType::ByRef(Box::new(t))
                    },
                    o,
                ))
            }
            E_VALUETYPE | E_CLASS => {
                let (v, o) = cuint(b, o)?;
                let (tab, row) = Self::coded(Coded::TypeDefOrRef, v)?;
                let name = self.type_name(tab, row)?;
                let r = tab.map(|t| (t, row));
                Ok((
                    if e == E_VALUETYPE {
                        SigType::ValueType(name, r)
                    } else {
                        SigType::Class(name, r)
                    },
                    o,
                ))
            }
            E_SZARRAY => {
                let (t, o) = self.sig_type(b, o)?;
                Ok((SigType::Array(Box::new(t)), o))
            }
            E_ARRAY => {
                let (t, mut o) = self.sig_type(b, o)?;
                o = cuint(b, o)?.1;
                let (n, mut o) = cuint(b, o)?;
                for _ in 0..n {
                    o = cuint(b, o)?.1;
                }
                let (n, mut o) = cuint(b, o)?;
                for _ in 0..n {
                    o = cuint(b, o)?.1;
                }
                Ok((SigType::Array(Box::new(t)), o))
            }
            E_GENERICINST => {
                let k = byte_at(b, o)?;
                let o = cuint(b, o + 1)?.1;
                let (n, mut o) = cuint(b, o)?;
                for _ in 0..n {
                    o = self.sig_type(b, o)?.1;
                }
                Ok((
                    if k == E_VALUETYPE {
                        SigType::ValueType("generic".into(), None)
                    } else {
                        SigType::Class("generic".into(), None)
                    },
                    o,
                ))
            }
            E_VAR | E_MVAR => {
                let o = cuint(b, o)?.1;
                Ok((SigType::Class("generic parameter".into(), None), o))
            }
            E_FNPTR => {
                let o = self.sig(b, o)?.1;
                Ok((SigType::FnPtr, o))
            }
            _ => bad(format!("element type {e:#x}")),
        }
    }

    fn sig(&self, b: &[u8], mut o: usize) -> Result<(MethodSig, usize)> {
        let cc = byte_at(b, o)?;
        o += 1;
        if cc & 0x10 != 0 {
            o = cuint(b, o)?.1;
        }
        let (n, o2) = cuint(b, o)?;
        let (ret, mut o) = self.sig_type(b, o2)?;
        let mut ps = Vec::new();
        for _ in 0..n {
            if byte_at(b, o)? == E_SENTINEL {
                o += 1;
            }
            let (t, o3) = self.sig_type(b, o)?;
            o = o3;
            ps.push(t);
        }
        Ok(((ret, ps), o))
    }

    /// A method signature, None when it does not parse (ValueError, IndexError); other errors
    /// come through.
    pub fn method_sig(&self, b: &[u8]) -> Result<Option<MethodSig>> {
        match self.sig(b, 0) {
            Ok((s, _)) => Ok(Some(s)),
            Err(Error::Value(_)) => Ok(None),
            Err(e) => Err(e),
        }
    }

    pub fn field_sig(&self, b: &[u8]) -> Result<Option<SigType>> {
        if b.first() != Some(&0x06) {
            return Ok(None);
        }
        match self.sig_type(b, 1) {
            Ok((t, _)) => Ok(Some(t)),
            Err(Error::Value(_)) => Ok(None),
            Err(e) => Err(e),
        }
    }

    /// (base type name, [(field name, type)] of the instance fields, explicit size, flags).
    #[allow(clippy::type_complexity)]
    pub fn typedef_layout(
        &self,
        row: u32,
    ) -> Result<(
        Option<String>,
        Vec<(String, Option<SigType>)>,
        Option<u32>,
        u32,
    )> {
        let ts = self.types()?;
        let t = py_index(ts, row as i64 - 1)?;
        let (tab, r) = t.extends;
        let base = if r != 0 {
            Some(self.type_name(tab, r)?)
        } else {
            None
        };
        let fields = self.rows(FIELD)?;
        let mut out = Vec::new();
        for i in t.fields.0..t.fields.1 {
            let f = py_index(&fields, i as i64 - 1)?;
            if f[0] & 0x10 != 0 {
                continue;
            }
            out.push((self.string(f[1])?, self.field_sig(&self.blob(f[2])?)?));
        }
        let mut size = None;
        for l in self.rows(CLASSLAYOUT)?.iter() {
            if l[2] == row && l[1] != 0 {
                size = Some(l[1]);
            }
        }
        Ok((base, out, size, t.flags))
    }

    pub fn assembly_refs(&self) -> Result<Vec<String>> {
        self.rows(ASSEMBLYREF)?
            .iter()
            .map(|r| self.string(r[6]))
            .collect()
    }

    /// The assembly name a TypeRef points into (None for nested or module-local scopes).
    pub fn typeref_scope(&self, row: u32) -> Result<Option<String>> {
        let rows = self.rows(TYPEREF)?;
        let scope = row_at(&rows, row)?[0];
        let (tab, r) = Self::coded(Coded::ResolutionScope, scope)?;
        if tab == Some(ASSEMBLYREF) {
            let ar = self.rows(ASSEMBLYREF)?;
            return Ok(Some(self.string(py_index(&ar, r as i64 - 1)?[6])?));
        }
        Ok(None)
    }
}

/// bisect.bisect_right (the same steps, for lists that are not sorted too).
fn bisect_right(a: &[u32], x: u32) -> usize {
    let (mut lo, mut hi) = (0, a.len());
    while lo < hi {
        let mid = (lo + hi) / 2;
        if x < a[mid] {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    lo
}

/// rows[row - 1] as Python indexes (row 0 is the last one).
fn row_at(rows: &[Vec<u32>], row: u32) -> Result<&Vec<u32>> {
    py_index(rows, row as i64 - 1)
}

fn py_index<T>(v: &[T], i: i64) -> Result<&T> {
    let n = v.len() as i64;
    let j = if i < 0 { i + n } else { i };
    if j < 0 || j >= n {
        return bad("list index out of range");
    }
    Ok(&v[j as usize])
}

/// A compressed unsigned integer at o: (value, next offset).
pub fn cuint(d: &[u8], o: usize) -> Result<(u32, usize)> {
    let b = byte_at(d, o)? as u32;
    if b < 0x80 {
        return Ok((b, o + 1));
    }
    if b < 0xC0 {
        return Ok((((b & 0x3f) << 8) | byte_at(d, o + 1)? as u32, o + 2));
    }
    Ok((
        ((b & 0x1f) << 24)
            | (byte_at(d, o + 1)? as u32) << 16
            | (byte_at(d, o + 2)? as u32) << 8
            | byte_at(d, o + 3)? as u32,
        o + 4,
    ))
}

/// bytes.decode('utf-16-le', 'replace').
pub fn utf16le_replace(b: &[u8]) -> String {
    let units: Vec<u16> = b
        .chunks_exact(2)
        .map(|c| u16::from_le_bytes([c[0], c[1]]))
        .collect();
    let mut s = String::from_utf16_lossy(&units);
    if b.len() % 2 == 1 {
        s.push('\u{fffd}');
    }
    s
}

#[cfg(test)]
pub mod tests {
    use super::*;

    /// A minimal assembly: one section, a CLI header, metadata with #~, #Strings, #Blob and the
    /// given tables ([(table, rows of column values)], columns 2 bytes wide but 1/4-byte fixed ones).
    pub fn build(tables: &[(u8, Vec<Vec<u32>>)], strings: &[u8], blob: &[u8]) -> Vec<u8> {
        let mut d = vec![0u8; 0x200];
        d[..2].copy_from_slice(b"MZ");
        d[0x3c..0x40].copy_from_slice(&0x80u32.to_le_bytes());
        let pe = 0x80;
        d[pe..pe + 4].copy_from_slice(b"PE\0\0");
        d[pe + 6..pe + 8].copy_from_slice(&1u16.to_le_bytes()); // one section
        let optsz = 224u16;
        d[pe + 20..pe + 22].copy_from_slice(&optsz.to_le_bytes());
        let opt = pe + 24;
        d[opt..opt + 2].copy_from_slice(&0x10bu16.to_le_bytes());
        let dirs = opt + 96;
        let sec = opt + optsz as usize;
        // section: vsize, va 0x2000, rsize, raw 0x200
        let body_va = 0x2000u32;
        // CLI header at va 0x2000, metadata at 0x2048
        let mut body = vec![0u8; 0x48];
        let md_rva = body_va + 0x48;
        body[8..12].copy_from_slice(&md_rva.to_le_bytes());
        // metadata root
        let mut md = Vec::new();
        md.extend(b"BSJB");
        md.extend([1, 0, 1, 0]);
        md.extend(0u32.to_le_bytes());
        md.extend(4u32.to_le_bytes());
        md.extend(b"v4\0\0");
        md.extend(0u16.to_le_bytes());
        md.extend(3u16.to_le_bytes());
        // tables stream
        let mut tb = vec![0u8; 24];
        let mut valid = 0u64;
        for (t, _) in tables {
            valid |= 1 << t;
        }
        tb[8..16].copy_from_slice(&valid.to_le_bytes());
        let mut sorted: Vec<&(u8, Vec<Vec<u32>>)> = tables.iter().collect();
        sorted.sort_by_key(|x| x.0);
        for (_, rows) in &sorted {
            tb.extend((rows.len() as u32).to_le_bytes());
        }
        for (t, rows) in &sorted {
            let sch = schema(*t).unwrap();
            for r in rows {
                for (k, c) in sch.iter().enumerate() {
                    let fixed = index_col(*t, k).is_none();
                    match c {
                        Col::Fixed(1) if fixed => tb.push(r[k] as u8),
                        Col::Fixed(4) if fixed => tb.extend(r[k].to_le_bytes()),
                        _ => tb.extend((r[k] as u16).to_le_bytes()),
                    }
                }
            }
        }
        while tb.len() % 4 != 0 {
            tb.push(0);
        }
        let mut st = strings.to_vec();
        while st.len() % 4 != 0 {
            st.push(0);
        }
        let mut bl = blob.to_vec();
        while bl.len() % 4 != 0 {
            bl.push(0);
        }
        let hdr_len = md.len() + (8 + 4) + (8 + 12) + (8 + 8);
        let mut off = hdr_len;
        for (name, len) in [
            (&b"#~\0\0"[..], tb.len()),
            (b"#Strings\0\0\0\0", st.len()),
            (b"#Blob\0\0\0", bl.len()),
        ] {
            md.extend((off as u32).to_le_bytes());
            md.extend((len as u32).to_le_bytes());
            md.extend(name);
            off += len;
        }
        assert_eq!(md.len(), hdr_len);
        md.extend(&tb);
        md.extend(&st);
        md.extend(&bl);
        body.extend(&md);
        let size = body.len() as u32;
        d[sec + 8..sec + 12].copy_from_slice(&size.to_le_bytes());
        d[sec + 12..sec + 16].copy_from_slice(&body_va.to_le_bytes());
        d[sec + 16..sec + 20].copy_from_slice(&size.to_le_bytes());
        d[sec + 20..sec + 24].copy_from_slice(&0x200u32.to_le_bytes());
        d[dirs + 14 * 8..dirs + 14 * 8 + 4].copy_from_slice(&body_va.to_le_bytes());
        d[dirs + 14 * 8 + 4..dirs + 14 * 8 + 8].copy_from_slice(&0x48u32.to_le_bytes());
        d.extend(body);
        d
    }

    /// #Strings with the given names: (heap, offset of each).
    pub fn strings(names: &[&str]) -> (Vec<u8>, Vec<u32>) {
        let mut h = vec![0u8];
        let mut offs = Vec::new();
        for n in names {
            offs.push(h.len() as u32);
            h.extend(n.as_bytes());
            h.push(0);
        }
        (h, offs)
    }

    #[test]
    fn pinvokes_types_and_constants() {
        // strings: 1 Native, 2 Ns, 3 Glue, 4 Inner, 5 DoIt, 6 do_it, 7 libfoo, 8 Version, 9 mscorlib, 10 Object, 11 System
        let (s, o) = strings(&[
            "<Module>", "Ns", "Glue", "Inner", "DoIt", "do_it", "libfoo", "Version", "mscorlib",
            "Object", "System",
        ]);
        // blobs: 1 method sig (default, 2 params, void(float, int)); 7 const int 7; 12 field sig int
        let blob = vec![
            0, 5, 0x00, 2, E_VOID, E_R4, E_I4, 4, 0x07, 0, 0, 0, 2, 0x06, E_I4,
        ];
        let tables = vec![
            (MODULE, vec![vec![0, o[0], 0, 0, 0]]),
            (TYPEREF, vec![vec![(1 << 2) | 2, o[9], o[10]]]),
            (
                TYPEDEF,
                vec![
                    vec![0, o[0], 0, 0, 1, 1],
                    vec![0, o[2], o[1], (1 << 2) | 1, 1, 1],
                    vec![0, o[3], 0, 0, 2, 2],
                ],
            ),
            (FIELD, vec![vec![0x10 | 0x40, o[7], 12]]),
            (METHODDEF, vec![vec![0, 0, 0x2000, o[4], 1, 1]]),
            (CONSTANT, vec![vec![E_I4 as u32, 0, 4, 7]]),
            (MODULEREF, vec![vec![o[6]]]),
            (IMPLMAP, vec![vec![0, (1 << 1) | 1, o[5], 1]]),
            (ASSEMBLYREF, vec![vec![4, 0, 0, 0, 0, 0, o[8], 0, 0]]),
            (NESTEDCLASS, vec![vec![3, 2]]),
        ];
        let d = build(&tables, &s, &blob);
        let a = Assembly::from_bytes("t.dll", d).unwrap();
        let ts = a.types().unwrap();
        assert_eq!(ts[1].full, "Ns.Glue");
        assert_eq!(ts[2].full, "Ns.Glue/Inner");
        let p = a.pinvokes().unwrap();
        assert_eq!(p.len(), 1);
        assert_eq!(p[0].module, "libfoo");
        assert_eq!(p[0].entry, "do_it");
        assert_eq!(p[0].method, "DoIt");
        assert_eq!(p[0].typ, "Ns.Glue");
        assert_eq!(
            p[0].sig,
            Some((
                SigType::Prim(E_VOID),
                vec![SigType::Prim(E_R4), SigType::Prim(E_I4)]
            ))
        );
        assert_eq!(
            a.field_constants().unwrap(),
            vec![("Ns.Glue.Version".to_string(), Const::Int(7))]
        );
        assert_eq!(a.typeref_name(1).unwrap(), "System.Object");
        assert_eq!(a.typeref_scope(1).unwrap().as_deref(), Some("mscorlib"));
        assert_eq!(a.assembly_refs().unwrap(), ["mscorlib"]);
        assert!(matches!(
            Assembly::from_bytes("x", b"not a pe".to_vec()),
            Err(Error::NotAssembly(_))
        ));
    }
}
