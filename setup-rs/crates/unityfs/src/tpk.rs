//! The class database UnityPy reads objects of files without type trees with (a desktop build's
//! main data): a TPK file (AssetRipper's type tree package, UnityPy/resources/lzma.tpk, parsed as
//! tpk_ar 0.2.4 does). It is Unity-derived data, so it is not in this repository: it is read at
//! run time from a path (the Python's copy), or embedded at build time from the file named by
//! PMU_TPK (see build.rs).
use crate::bin::Reader;
use crate::compress;
use crate::error::{bad, Error, Result};
use crate::typetree::{CommonStrings, Kind, Node, TypeTree};
use crate::version::UnityVersion;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

/// tpk_ar's TpkCommonStringV1: (version, count) entries and string indices.
type V1Strings = (Vec<(UnityVersion, u8)>, Vec<u16>);

/// One class in one release range: its name and base (string indices) and root nodes.
#[derive(Clone, Debug)]
pub struct TpkClass {
    pub name: u16,
    pub base: u16,
    pub flags: u8,
    pub editor_root: Option<u16>,
    pub release_root: Option<u16>,
}

#[derive(Clone, Debug)]
struct TpkNode {
    type_name: u16,
    name: u16,
    byte_size: i32,
    version: i16,
    meta_flag: u32,
    sub: Vec<u16>,
}

/// The class database.
pub struct ClassDb {
    /// class id -> [(first version, class or None when absent from then on)], in the file's order.
    classes: HashMap<i32, Vec<(UnityVersion, Option<TpkClass>)>>,
    nodes: Vec<TpkNode>,
    strings: Vec<Arc<str>>,
    common: CommonStrings,
    cache: Mutex<HashMap<(i32, u16), Arc<TypeTree>>>,
}

impl std::fmt::Debug for ClassDb {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "ClassDb({} classes)", self.classes.len())
    }
}

fn varint_str(r: &mut Reader) -> Result<Arc<str>> {
    let mut shift = 0;
    let mut len: usize = 0;
    loop {
        let b = r.u8()?;
        len |= ((b & 0x7F) as usize) << shift;
        shift += 7;
        if b & 0x80 == 0 {
            break;
        }
        if shift > 35 {
            return bad("tpk: bad string length");
        }
    }
    let b = r.take(len)?;
    std::str::from_utf8(b)
        .map(Arc::from)
        .map_err(|_| Error::Format("tpk: string not UTF-8".into()))
}

impl ClassDb {
    /// Reads a TPK file (type tree information; uncompressed, LZ4 or LZMA).
    pub fn open(path: impl AsRef<std::path::Path>) -> Result<Self> {
        Self::parse(&std::fs::read(path)?)
    }

    /// The database embedded at build time (PMU_TPK), if there is one.
    pub fn embedded() -> Option<Result<Self>> {
        let b: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/classdb.tpk"));
        if b.is_empty() {
            None
        } else {
            Some(Self::parse(b))
        }
    }

    /// The database from $PMU_TPK at run time, else the embedded one.
    pub fn default_db() -> Result<Self> {
        if let Some(p) = std::env::var_os("PMU_TPK") {
            return Self::open(p);
        }
        Self::embedded().unwrap_or_else(|| {
            Err(Error::Unsupported(
                "no class database (set PMU_TPK to UnityPy's lzma.tpk)".into(),
            ))
        })
    }

    pub fn parse(data: &[u8]) -> Result<Self> {
        let mut r = Reader::new(data, false);
        if r.u32()? != 0x2A4B_5054 {
            return bad("not a TPK file");
        }
        let tpk_version = r.u8()?;
        if !(1..=2).contains(&tpk_version) {
            return bad(format!("TPK version {tpk_version}"));
        }
        let comp = r.u8()?;
        let dtype = r.u8()?;
        r.u8()?;
        r.u32()?;
        let csize = r.u32()? as usize;
        let usize = r.u32()? as usize;
        let cdata = r.take(csize)?;
        let blob = match comp {
            0 => cdata.to_vec(),
            1 => compress::lz4_decompress(cdata, usize)?,
            2 => compress::lzma_decompress(cdata, usize)?,
            c => return Err(Error::Unsupported(format!("TPK compression {c}"))),
        };
        if blob.len() != usize {
            return bad("TPK: decompressed size differs");
        }
        if dtype != 0 {
            return Err(Error::Unsupported(format!(
                "TPK data type {dtype} (not type trees)"
            )));
        }
        let mut r = Reader::new(&blob, false);
        r.i64()?; // creation time
        let nv = r.i32()?;
        for _ in 0..nv {
            r.u64()?;
        }
        let nc = r.i32()?;
        let mut classes = HashMap::new();
        for _ in 0..nc {
            let id = r.i32()?;
            let n = r.i32()?;
            let mut entries = Vec::new();
            for _ in 0..n {
                let v = UnityVersion(r.u64()?);
                let c = if r.u8()? != 0 {
                    let name = r.u16()?;
                    let base = r.u16()?;
                    let flags = r.u8()?;
                    let editor_root = if flags & 64 != 0 {
                        Some(r.u16()?)
                    } else {
                        None
                    };
                    let release_root = if flags & 128 != 0 {
                        Some(r.u16()?)
                    } else {
                        None
                    };
                    Some(TpkClass {
                        name,
                        base,
                        flags,
                        editor_root,
                        release_root,
                    })
                } else {
                    None
                };
                entries.push((v, c));
            }
            classes.insert(id, entries);
        }
        // common strings: (version, [(offset, string index)])
        let mut common_versions: Vec<(UnityVersion, Vec<(u16, u16)>)> = Vec::new();
        let mut v1: Option<V1Strings> = None;
        if tpk_version == 2 {
            let n = r.i32()?;
            for _ in 0..n {
                let v = UnityVersion(r.u64()?);
                let m = r.i32()?;
                let mut e = Vec::with_capacity(m.max(0) as usize);
                for _ in 0..m {
                    e.push((r.u16()?, r.u16()?));
                }
                common_versions.push((v, e));
            }
        } else {
            let n = r.i32()?;
            let mut vi = Vec::new();
            for _ in 0..n {
                vi.push((UnityVersion(r.u64()?), r.u8()?));
            }
            let m = r.i32()?;
            let mut idx = Vec::new();
            for _ in 0..m {
                idx.push(r.u16()?);
            }
            v1 = Some((vi, idx));
        }
        let nn = r.i32()?;
        let mut nodes = Vec::with_capacity(nn.max(0) as usize);
        for _ in 0..nn {
            let type_name = r.u16()?;
            let name = r.u16()?;
            let byte_size = r.i32()?;
            let version = r.i16()?;
            let _type_flags = r.u8()?;
            let meta_flag = r.u32()?;
            let k = r.u16()?;
            let mut sub = Vec::with_capacity(k as usize);
            for _ in 0..k {
                sub.push(r.u16()?);
            }
            nodes.push(TpkNode {
                type_name,
                name,
                byte_size,
                version,
                meta_flag,
                sub,
            });
        }
        let ns = r.i32()?;
        let mut strings = Vec::with_capacity(ns.max(0) as usize);
        for _ in 0..ns {
            strings.push(varint_str(&mut r)?);
        }
        let s = |i: u16| {
            strings
                .get(i as usize)
                .cloned()
                .ok_or_else(|| Error::Format("tpk: string index".into()))
        };
        // get_common_strings(): the latest table (v2: the entry list of the highest version
        // that get_item_for_version picks for it; v1: every string, offsets by length)
        let mut entries = Vec::new();
        if let Some((_, idx)) = v1 {
            let mut off = 0u32;
            for i in idx {
                let st = s(i)?;
                let l = st.len() as u32;
                entries.push((off, st));
                off += l + 1;
            }
        } else if let Some(max) = common_versions.iter().map(|(v, _)| *v).max() {
            let mut pick = None;
            for (v, e) in &common_versions {
                if max >= *v {
                    pick = Some(e);
                } else {
                    break;
                }
            }
            for &(o, i) in pick.ok_or_else(|| Error::Format("tpk: no common strings".into()))? {
                entries.push((o as u32, s(i)?));
            }
        }
        let common = CommonStrings::new(entries);
        Ok(ClassDb {
            classes,
            nodes,
            strings,
            common,
            cache: Mutex::new(HashMap::new()),
        })
    }

    /// The common strings type tree blobs are read and written with.
    pub fn common_strings(&self) -> &CommonStrings {
        &self.common
    }

    /// The class's entry for a version (tpk_ar's get_item_for_version: the last entry not newer
    /// than it), or None (no such class, a version before its first entry, or absent then).
    pub fn class(&self, class_id: i32, version: UnityVersion) -> Option<&TpkClass> {
        let entries = self.classes.get(&class_id)?;
        let mut ret = None;
        for (v, c) in entries {
            if version >= *v {
                ret = c.as_ref();
            } else {
                break;
            }
        }
        ret
    }

    /// Every entry of a class: (first version, class or None).
    pub fn class_entries(&self, class_id: i32) -> &[(UnityVersion, Option<TpkClass>)] {
        self.classes.get(&class_id).map(|v| &v[..]).unwrap_or(&[])
    }

    /// The class name (e.g. "Texture2D") for a version's entry.
    pub fn class_name(&self, class_id: i32, version: UnityVersion) -> Option<&str> {
        self.class(class_id, version)
            .and_then(|c| self.strings.get(c.name as usize))
            .map(|s| &**s)
    }

    /// UnityPy's get_typetree_node(class_id, version): the release tree of the class's entry for
    /// that version.
    pub fn typetree(&self, class_id: i32, version: UnityVersion) -> Result<Arc<TypeTree>> {
        let c = self.class(class_id, version).ok_or_else(|| {
            Error::Unsupported(format!("Could not find class info for class id {class_id}"))
        })?;
        self.tree_of(class_id, c)
    }

    /// The release tree of a class entry (UnityPy's generate_node), cached.
    pub fn tree_of(&self, class_id: i32, c: &TpkClass) -> Result<Arc<TypeTree>> {
        let root = c.release_root.ok_or_else(|| {
            Error::Unsupported(format!("class {class_id} has no release root node"))
        })?;
        if let Some(t) = self.cache.lock().unwrap().get(&(class_id, root)) {
            return Ok(t.clone());
        }
        let t = Arc::new(self.generate(root)?);
        self.cache
            .lock()
            .unwrap()
            .insert((class_id, root), t.clone());
        Ok(t)
    }

    fn generate(&self, root: u16) -> Result<TypeTree> {
        let mut list = Vec::new();
        let mut stack: Vec<(u16, i32)> = vec![(root, 0)];
        while let Some((id, level)) = stack.pop() {
            let n = self
                .nodes
                .get(id as usize)
                .ok_or_else(|| Error::Format("tpk: node index".into()))?;
            let t = self
                .strings
                .get(n.type_name as usize)
                .cloned()
                .ok_or_else(|| Error::Format("tpk: string".into()))?;
            let nm = self
                .strings
                .get(n.name as usize)
                .cloned()
                .ok_or_else(|| Error::Format("tpk: string".into()))?;
            list.push(Node {
                level,
                kind: Kind::of(&t),
                type_name: t,
                name: nm,
                byte_size: n.byte_size,
                version: n.version as i32,
                type_flags: None,
                variable_count: None,
                index: Some(list.len() as i32),
                meta_flag: Some(n.meta_flag as i32),
                ref_type_hash: None,
                children: Vec::new(),
            });
            for &s in n.sub.iter().rev() {
                stack.push((s, level + 1));
            }
        }
        TypeTree::from_list(list)
    }
}
