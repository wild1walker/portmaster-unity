//! Serialized files (.assets, levels, globalgamemanagers, a bundle's CAB-...): read and written as
//! UnityPy 1.25.4's SerializedFile does. The metadata (types, object table, scripts, externals,
//! reference types) is held in memory; objects' bytes are read from the source where needed.
//!
//! Saving (UnityPy's save, which every Python write path uses, a bundle's serialized file
//! included): the metadata written again (type trees as dump_blob writes them), the objects by
//! path id, each one's data 8-aligned after the one before, the data 16-aligned after the
//! metadata. `save_to` streams the objects into a file (pmunity's save_serialized_to): the same
//! bytes, one object in memory at a time.
use crate::bin::{Reader, Writer};
use crate::error::{bad, Error, Result};
use crate::source::SharedSource;
use crate::tpk::ClassDb;
use crate::typetree::{CommonStrings, TypeTree};
use crate::value::{self, RefType, RefTypes, Value};
use crate::version::UnityVersion;
use std::collections::HashMap;
use std::io::{Seek, SeekFrom, Write};
use std::sync::Arc;

/// The file header (the first 16 bytes big-endian; format 9+: the endian byte and 3 reserved;
/// format 22+: 64-bit sizes after them).
#[derive(Clone, Debug)]
pub struct Header {
    pub metadata_size: u64,
    pub file_size: u64,
    pub version: u32,
    pub data_offset: u64,
    /// The metadata's and objects' byte order.
    pub big: bool,
    pub reserved: [u8; 3],
    /// Format 22+: the 8 bytes after the data offset.
    pub unknown: i64,
}

/// A type: class id, hashes, and its type tree when the file has them.
#[derive(Clone, Debug)]
pub struct SerializedType {
    pub class_id: i32,
    pub is_stripped_type: Option<bool>,
    pub script_type_index: i16,
    pub script_id: Option<[u8; 16]>,
    pub old_type_hash: Option<[u8; 16]>,
    pub node: Option<Arc<TypeTree>>,
    /// Reference types (format 21+): class, namespace, assembly.
    pub class_name: Option<Vec<u8>>,
    pub namespace: Option<Vec<u8>>,
    pub assembly_name: Option<Vec<u8>>,
    pub type_dependencies: Option<Vec<i32>>,
    pub type_tree_content_hash: Option<[u8; 16]>,
    pub type_tree_serialized_size: Option<i32>,
}

/// An object table entry, and the object's new bytes once it is changed.
#[derive(Clone, Debug)]
pub struct ObjectInfo {
    pub path_id: i64,
    /// Offset of the object's bytes in the file (the data offset added).
    pub byte_start: u64,
    pub byte_size: u32,
    pub type_id: i32,
    pub class_id: i32,
    /// The type entry (format 16+: types[type_id]; before: the first type of this class id).
    pub type_index: Option<usize>,
    pub is_destroyed: Option<u16>,
    pub is_stripped: Option<u8>,
    /// The object's bytes as last saved (save_typetree / set_raw): written instead of the file's.
    pub data: Option<Vec<u8>>,
}

/// A script type entry (LocalSerializedObjectIdentifier).
#[derive(Clone, Debug)]
pub struct ScriptType {
    pub file_index: i32,
    pub identifier: i64,
}

/// An external file reference (FileIdentifier).
#[derive(Clone, Debug)]
pub struct External {
    pub temp_empty: Option<Vec<u8>>,
    pub guid: Option<[u8; 16]>,
    pub typ: Option<i32>,
    pub path: Vec<u8>,
}

impl External {
    /// The path's last part (ntpath.basename).
    pub fn name(&self) -> String {
        let p = String::from_utf8_lossy(&self.path);
        p.rsplit(['/', '\\']).next().unwrap_or("").to_string()
    }
}

/// A serialized file.
pub struct SerializedFile {
    pub name: String,
    pub header: Header,
    pub unity_version: Vec<u8>,
    pub version: UnityVersion,
    pub target_platform: i32,
    pub enable_type_tree: bool,
    pub types: Vec<SerializedType>,
    pub big_id_enabled: i32,
    /// The objects in the table's order (a path id listed twice: one entry, the later one, in
    /// the first one's place, as UnityPy's dict).
    pub objects: Vec<ObjectInfo>,
    index: HashMap<i64, usize>,
    pub script_types: Vec<ScriptType>,
    pub externals: Vec<External>,
    pub ref_types: Option<Vec<SerializedType>>,
    pub user_information: Option<Vec<u8>>,
    src: SharedSource,
    db: Option<Arc<ClassDb>>,
    common: Arc<CommonStrings>,
    refs: Option<Vec<RefType>>,
}

fn hash16(r: &mut Reader) -> Result<[u8; 16]> {
    Ok(r.take(16)?.try_into().unwrap())
}

impl SerializedType {
    fn parse(
        r: &mut Reader,
        version: u32,
        type_trees: bool,
        is_ref: bool,
        common: &CommonStrings,
    ) -> Result<Self> {
        let class_id = r.i32()?;
        let is_stripped_type = if version >= 16 { Some(r.bool()?) } else { None };
        let script_type_index = if version >= 17 { r.i16()? } else { -1 };
        let mut script_id = None;
        let mut old_type_hash = None;
        if version >= 13 {
            if (is_ref && script_type_index >= 0)
                || (version < 16 && class_id < 0)
                || (version >= 16 && class_id == 114)
            {
                script_id = Some(hash16(r)?);
            }
            old_type_hash = Some(hash16(r)?);
        }
        let mut t = SerializedType {
            class_id,
            is_stripped_type,
            script_type_index,
            script_id,
            old_type_hash,
            node: None,
            class_name: None,
            namespace: None,
            assembly_name: None,
            type_dependencies: None,
            type_tree_content_hash: None,
            type_tree_serialized_size: None,
        };
        if type_trees {
            if version >= 23 {
                t.type_tree_content_hash = Some(hash16(r)?);
                t.type_tree_serialized_size = Some(r.i32()?);
            }
            if version >= 12 || version == 10 {
                if version < 23 || t.type_tree_serialized_size.unwrap_or(0) != 0 {
                    t.node = Some(Arc::new(TypeTree::parse_blob(r, version, common)?));
                }
            } else {
                t.node = Some(Arc::new(TypeTree::parse_old(r, version)?));
            }
            if version >= 21 {
                if is_ref {
                    t.class_name = Some(r.cstr()?.to_vec());
                    t.namespace = Some(r.cstr()?.to_vec());
                    t.assembly_name = Some(r.cstr()?.to_vec());
                } else {
                    let n = r.i32()?;
                    let mut d = Vec::new();
                    for _ in 0..n.max(0) {
                        d.push(r.i32()?);
                    }
                    t.type_dependencies = Some(d);
                }
            }
        }
        Ok(t)
    }

    fn write(
        &self,
        w: &mut Writer,
        version: u32,
        type_trees: bool,
        is_ref: bool,
        common: &CommonStrings,
    ) -> Result<()> {
        w.i32(self.class_id);
        if version >= 16 {
            w.u8(self
                .is_stripped_type
                .ok_or_else(|| Error::Value("type without is_stripped_type".into()))?
                as u8);
        }
        if version >= 17 {
            w.i16(self.script_type_index);
        }
        if version >= 13 {
            if (is_ref && self.script_type_index >= 0)
                || (version < 16 && self.class_id < 0)
                || (version >= 16 && self.class_id == 114)
            {
                w.bytes(
                    &self
                        .script_id
                        .ok_or_else(|| Error::Value("type without script id".into()))?,
                );
            }
            w.bytes(
                &self
                    .old_type_hash
                    .ok_or_else(|| Error::Value("type without type hash".into()))?,
            );
        }
        if type_trees {
            let node = self
                .node
                .as_ref()
                .ok_or_else(|| Error::Value("type without a type tree".into()))?;
            if version >= 23 {
                w.bytes(
                    &self
                        .type_tree_content_hash
                        .ok_or_else(|| Error::Value("type without a content hash".into()))?,
                );
            }
            if version >= 12 || version == 10 {
                if version >= 23 {
                    let mut b = Writer::new(w.big);
                    node.dump_blob(&mut b, version, common)?;
                    w.i32(b.len() as i32);
                    w.bytes(&b.buf);
                } else {
                    node.dump_blob(w, version, common)?;
                }
            } else {
                // (UnityPy calls a method SerializedFile does not have)
                return Err(Error::Unsupported(format!(
                    "writing type trees of format {version}"
                )));
            }
            if version >= 21 {
                if is_ref {
                    for s in [&self.class_name, &self.namespace, &self.assembly_name] {
                        w.cstr(
                            s.as_ref().ok_or_else(|| {
                                Error::Value("reference type without names".into())
                            })?,
                        );
                    }
                } else {
                    let d = self
                        .type_dependencies
                        .as_ref()
                        .ok_or_else(|| Error::Value("type without dependencies".into()))?;
                    w.i32(d.len() as i32);
                    for &x in d {
                        w.i32(x);
                    }
                }
            }
        }
        Ok(())
    }
}

/// Bytes the metadata needs read for a header: (header length, the end of the metadata) — the
/// prefix of the file `parse` reads.
pub fn head_len(first: &[u8]) -> Result<u64> {
    let mut r = Reader::new(first, true);
    let ms = r.u32()? as u64;
    let _fs = r.u32()?;
    let ver = r.u32()?;
    let doff = r.u32()? as u64;
    if ver < 9 {
        return Err(Error::Unsupported(format!(
            "serialized file format {ver} (metadata at the end)"
        )));
    }
    if ver >= 22 {
        r.take(4)?;
        let ms = r.u32()? as u64;
        let _fs = r.u64()?;
        let doff = r.u64()?;
        return Ok((48 + ms).max(doff));
    }
    Ok((20 + ms).max(doff))
}

/// Whether bytes look like a serialized file (UnityPy's check_file_type: AssetsFile), given the
/// first 48 bytes and the length.
pub fn looks_serialized(first: &[u8], len: u64) -> bool {
    if len < 20 {
        return false;
    }
    let sig_end = first
        .iter()
        .take(20)
        .position(|&c| c == 0)
        .unwrap_or(first.len().min(20));
    let sig = &first[..sig_end];
    if [
        &b"UnityWeb"[..],
        b"UnityRaw",
        b"\xfa\xfa\xfa\xfa\xfa\xfa\xfa\xfa",
        b"UnityFS",
    ]
    .contains(&sig)
        || sig.starts_with(b"UnityWebData")
        || sig.starts_with(b"TuanjieWebData")
        || sig == b"PK\x03\x04"
    {
        return false;
    }
    if len < 128 || first.len() < 48 {
        return false;
    }
    if first[..2] == [0x1f, 0x8b] || &first[0x20..0x26] == b"brotli" {
        return false;
    }
    let be = |o: usize| u32::from_be_bytes(first[o..o + 4].try_into().unwrap()) as i64;
    let (mut ms, mut fs, ver, mut doff) = (be(0), be(4), be(8), be(12));
    if ver >= 22 {
        ms = be(20);
        fs = i64::from_be_bytes(first[24..32].try_into().unwrap());
        doff = i64::from_be_bytes(first[32..40].try_into().unwrap());
    }
    let l = len as i64;
    !(!(0..=100).contains(&ver)
        || [fs, ms, ver, doff].iter().any(|&x| x < 0 || x > l)
        || fs < ms
        || fs < doff)
}

impl SerializedFile {
    /// Parses the header and metadata (read from the source's start) of a serialized file.
    /// `db`: the class database, for objects without a type tree in the file. `engine`: the
    /// bundle's engine version, for a file whose own version is empty.
    pub fn open(
        name: &str,
        src: SharedSource,
        db: Option<Arc<ClassDb>>,
        engine: Option<&str>,
    ) -> Result<Self> {
        let n = src.len();
        let first = src.read_clipped(0, 64)?;
        let hl = head_len(&first)?.min(n);
        let head = src.read_vec(0, hl as usize)?;
        let common = Arc::new(
            db.as_ref()
                .map(|d| d.common_strings().clone())
                .unwrap_or_default(),
        );
        Self::parse(name, &head, src, db, common, engine)
    }

    fn parse(
        name: &str,
        head: &[u8],
        src: SharedSource,
        db: Option<Arc<ClassDb>>,
        common: Arc<CommonStrings>,
        engine: Option<&str>,
    ) -> Result<Self> {
        let mut r = Reader::new(head, true);
        let mut h = Header {
            metadata_size: r.u32()? as u64,
            file_size: r.u32()? as u64,
            version: r.u32()?,
            data_offset: r.u32()? as u64,
            big: false,
            reserved: [0; 3],
            unknown: 0,
        };
        let v = h.version;
        if v < 9 {
            return Err(Error::Unsupported(format!("serialized file format {v}")));
        }
        h.big = r.bool()?;
        h.reserved = r.take(3)?.try_into().unwrap();
        if v >= 22 {
            h.metadata_size = r.u32()? as u64;
            h.file_size = r.u64()?;
            h.data_offset = r.u64()?;
            h.unknown = r.i64()?;
        }
        r.big = h.big;
        let unity_version = if v >= 7 {
            r.cstr()?.to_vec()
        } else {
            Vec::new()
        };
        let vs = String::from_utf8_lossy(&unity_version).to_string();
        let mut version = UnityVersion::parse(&vs).filter(|_| !vs.is_empty() && vs != "0.0.0");
        if version.is_none() || version.map(|v| v.major()) == Some(0) {
            version = engine.and_then(UnityVersion::parse);
        }
        let version = version.unwrap_or_default();
        let target_platform = if v >= 8 { r.i32()? } else { 0 };
        let enable_type_tree = if v >= 13 { r.bool()? } else { true };
        let nt = r.i32()?;
        let mut types = Vec::new();
        for _ in 0..nt.max(0) {
            types.push(SerializedType::parse(
                &mut r,
                v,
                enable_type_tree,
                false,
                &common,
            )?);
        }
        let big_id_enabled = if (7..14).contains(&v) { r.i32()? } else { 0 };
        let no = r.i32()?;
        let mut objects: Vec<ObjectInfo> = Vec::new();
        let mut index = HashMap::new();
        for _ in 0..no.max(0) {
            let path_id = if big_id_enabled != 0 {
                r.i64()?
            } else if v < 14 {
                r.i32()? as i64
            } else {
                r.align(4);
                r.i64()?
            };
            let start = if v >= 22 {
                r.i64()? as u64
            } else {
                r.u32()? as u64
            };
            let byte_start = start.wrapping_add(h.data_offset);
            let byte_size = r.u32()?;
            let type_id = r.i32()?;
            let (class_id, type_index) = if v < 16 {
                let c = r.u16()? as i32;
                (c, types.iter().position(|t| t.class_id == type_id))
            } else {
                let t = types.get(type_id as usize).ok_or_else(|| {
                    Error::Format(format!("{name}: object type {type_id} out of range"))
                })?;
                (t.class_id, Some(type_id as usize))
            };
            let is_destroyed = if v < 11 { Some(r.u16()?) } else { None };
            if (11..17).contains(&v) {
                let sti = r.i16()?;
                if let Some(ti) = type_index {
                    types[ti].script_type_index = sti;
                }
            }
            let is_stripped = if v == 15 || v == 16 {
                Some(r.u8()?)
            } else {
                None
            };
            let o = ObjectInfo {
                path_id,
                byte_start,
                byte_size,
                type_id,
                class_id,
                type_index,
                is_destroyed,
                is_stripped,
                data: None,
            };
            match index.get(&path_id) {
                Some(&i) => objects[i] = o,
                None => {
                    index.insert(path_id, objects.len());
                    objects.push(o);
                }
            }
        }
        let mut script_types = Vec::new();
        if v >= 11 {
            let n = r.i32()?;
            for _ in 0..n.max(0) {
                let file_index = r.i32()?;
                let identifier = if v < 14 {
                    r.i32()? as i64
                } else {
                    r.align(4);
                    r.i64()?
                };
                script_types.push(ScriptType {
                    file_index,
                    identifier,
                });
            }
        }
        let n = r.i32()?;
        let mut externals = Vec::new();
        for _ in 0..n.max(0) {
            let temp_empty = if v >= 6 {
                Some(r.cstr()?.to_vec())
            } else {
                None
            };
            let (guid, typ) = if v >= 5 {
                (Some(hash16(&mut r)?), Some(r.i32()?))
            } else {
                (None, None)
            };
            let path = r.cstr()?.to_vec();
            externals.push(External {
                temp_empty,
                guid,
                typ,
                path,
            });
        }
        let ref_types = if v >= 20 {
            let n = r.i32()?;
            let mut t = Vec::new();
            for _ in 0..n.max(0) {
                t.push(SerializedType::parse(
                    &mut r,
                    v,
                    enable_type_tree,
                    true,
                    &common,
                )?);
            }
            Some(t)
        } else {
            None
        };
        let user_information = if v >= 5 {
            Some(r.cstr()?.to_vec())
        } else {
            None
        };
        let refs = ref_types.as_ref().map(|rt| {
            rt.iter()
                .map(|t| {
                    (
                        t.class_name.clone().unwrap_or_default(),
                        t.namespace.clone().unwrap_or_default(),
                        t.assembly_name.clone().unwrap_or_default(),
                        t.node.clone(),
                    )
                })
                .collect()
        });
        Ok(SerializedFile {
            name: name.to_string(),
            header: h,
            unity_version,
            version,
            target_platform,
            enable_type_tree,
            types,
            big_id_enabled,
            objects,
            index,
            script_types,
            externals,
            ref_types,
            user_information,
            src,
            db,
            common,
            refs,
        })
    }

    /// The source the file was read from.
    pub fn source(&self) -> &SharedSource {
        &self.src
    }

    pub fn db(&self) -> Option<&Arc<ClassDb>> {
        self.db.as_ref()
    }

    pub fn object(&self, path_id: i64) -> Option<&ObjectInfo> {
        self.index.get(&path_id).map(|&i| &self.objects[i])
    }

    pub fn object_mut(&mut self, path_id: i64) -> Option<&mut ObjectInfo> {
        self.index.get(&path_id).map(|&i| &mut self.objects[i])
    }

    fn obj(&self, path_id: i64) -> Result<&ObjectInfo> {
        self.object(path_id)
            .ok_or_else(|| Error::Value(format!("{}: no object {path_id}", self.name)))
    }

    /// The class name UnityPy gives an object (`obj.type.name`: its ClassIDType names).
    pub fn class_name(&self, o: &ObjectInfo) -> String {
        crate::classes::class_name(o.class_id)
    }

    /// The objects of a class id, in the table's order.
    pub fn objects_of(&self, class_id: i32) -> impl Iterator<Item = &ObjectInfo> {
        self.objects.iter().filter(move |o| o.class_id == class_id)
    }

    /// The bytes the file has for an object (UnityPy's get_raw_data; what read_typetree reads,
    /// even after a save).
    pub fn original_bytes(&self, path_id: i64) -> Result<Vec<u8>> {
        let o = self.obj(path_id)?;
        Ok(self.src.read_vec(o.byte_start, o.byte_size as usize)?)
    }

    /// An object's current bytes: as last saved, else the file's.
    pub fn bytes(&self, path_id: i64) -> Result<Vec<u8>> {
        let o = self.obj(path_id)?;
        match &o.data {
            Some(d) => Ok(d.clone()),
            None => self.original_bytes(path_id),
        }
    }

    /// The type tree an object is read with (UnityPy's _get_typetree_node(None)): its type's
    /// own, else the class database's for its class id and the file's version (a
    /// MonoBehaviour: the base class's fields, as UnityPy without a type tree generator).
    pub fn typetree(&self, o: &ObjectInfo) -> Result<Arc<TypeTree>> {
        if let Some(n) = o.type_index.and_then(|i| self.types[i].node.clone()) {
            return Ok(n);
        }
        let db = self.db.as_ref().ok_or_else(|| {
            Error::Unsupported("no class database for an object without a type tree".into())
        })?;
        db.typetree(o.class_id, self.version)
    }

    pub fn typetree_of(&self, path_id: i64) -> Result<Arc<TypeTree>> {
        self.typetree(self.obj(path_id)?)
    }

    fn ref_types_view(&self) -> Option<&RefTypes> {
        self.refs.as_deref()
    }

    /// UnityPy's obj.read_typetree(): the file's bytes of the object read with its type tree,
    /// every byte (else Error::ReadSize).
    pub fn read_typetree(&self, path_id: i64) -> Result<Value> {
        let o = self.obj(path_id)?;
        let t = self.typetree(o)?;
        let data = self.original_bytes(path_id)?;
        value::read_object(&t, &data, self.header.big, self.ref_types_view())
    }

    /// obj.read_typetree(nodes=tree): with another type tree.
    pub fn read_typetree_with(&self, path_id: i64, tree: &TypeTree) -> Result<Value> {
        let data = self.original_bytes(path_id)?;
        value::read_object(tree, &data, self.header.big, self.ref_types_view())
    }

    /// pmunity's current_typetree: the object as last saved, else as the file has it.
    pub fn current_typetree(&self, path_id: i64) -> Result<Value> {
        let o = self.obj(path_id)?;
        let t = self.typetree(o)?;
        let data = self.bytes(path_id)?;
        value::read_object(&t, &data, self.header.big, self.ref_types_view())
    }

    /// Reads bytes with a type tree in this file's byte order and reference types.
    pub fn read_value(&self, tree: &TypeTree, data: &[u8]) -> Result<Value> {
        value::read_object(tree, data, self.header.big, self.ref_types_view())
    }

    /// A value written with an object's type tree (the bytes save_typetree sets).
    pub fn write_value(&self, tree: &TypeTree, v: &Value) -> Result<Vec<u8>> {
        value::write(tree, v, self.header.big, self.ref_types_view())
    }

    /// UnityPy's obj.save_typetree(tree): the object's new bytes from a value.
    pub fn save_typetree(&mut self, path_id: i64, v: &Value) -> Result<()> {
        let t = self.typetree_of(path_id)?;
        self.save_typetree_with(path_id, v, &t)
    }

    /// obj.save_typetree(tree, nodes=tree).
    pub fn save_typetree_with(&mut self, path_id: i64, v: &Value, tree: &TypeTree) -> Result<()> {
        let data = self.write_value(tree, v)?;
        self.set_raw(path_id, data)
    }

    /// obj.set_raw_data: the object's new bytes.
    pub fn set_raw(&mut self, path_id: i64, data: Vec<u8>) -> Result<()> {
        let name = self.name.clone();
        let o = self
            .object_mut(path_id)
            .ok_or_else(|| Error::Value(format!("{name}: no object {path_id}")))?;
        o.data = Some(data);
        Ok(())
    }

    /// Drops an object's saved bytes (it is written as the file has it again).
    pub fn clear_raw(&mut self, path_id: i64) {
        if let Some(o) = self.object_mut(path_id) {
            o.data = None;
        }
    }

    /// The metadata as the file holds it, each object where it is, in the table's order
    /// (retarget.metadata_bytes: the version string and platform as now set).
    pub fn metadata_in_place(&self) -> Result<Vec<u8>> {
        let order: Vec<usize> = (0..self.objects.len()).collect();
        let placed: Vec<(u64, u32)> = self
            .objects
            .iter()
            .map(|o| {
                (
                    o.byte_start.wrapping_sub(self.header.data_offset),
                    o.byte_size,
                )
            })
            .collect();
        self.metadata(&placed, &order)
    }

    /// The metadata as UnityPy's save writes it: the objects in `order` (indices into
    /// `objects`), each at its (offset in the data, size) in `placed`.
    pub fn metadata(&self, placed: &[(u64, u32)], order: &[usize]) -> Result<Vec<u8>> {
        let h = &self.header;
        let v = h.version;
        let mut w = Writer::new(h.big);
        if v >= 7 {
            w.cstr(&self.unity_version);
        }
        if v >= 8 {
            w.i32(self.target_platform);
        }
        if v >= 13 {
            w.u8(self.enable_type_tree as u8);
        }
        w.i32(self.types.len() as i32);
        for t in &self.types {
            t.write(&mut w, v, self.enable_type_tree, false, &self.common)?;
        }
        if (7..14).contains(&v) {
            w.i32(self.big_id_enabled);
        }
        w.i32(self.objects.len() as i32);
        for (k, &i) in order.iter().enumerate() {
            let o = &self.objects[i];
            if self.big_id_enabled != 0 {
                w.i64(o.path_id);
            } else if v < 14 {
                w.i32(o.path_id as i32);
            } else {
                w.align(4);
                w.i64(o.path_id);
            }
            let (off, size) = placed[k];
            if v >= 22 {
                w.i64(off as i64);
            } else {
                w.u32(off as u32);
            }
            w.u32(size);
            w.i32(o.type_id);
            if v < 16 {
                w.u16(o.class_id as u16);
            }
            if v < 11 {
                w.u16(
                    o.is_destroyed
                        .ok_or_else(|| Error::Value("object without is_destroyed".into()))?,
                );
            }
            if (11..17).contains(&v) {
                let ti = o
                    .type_index
                    .ok_or_else(|| Error::Value(format!("object {} without a type", o.path_id)))?;
                w.i16(self.types[ti].script_type_index);
            }
            if v == 15 || v == 16 {
                w.u8(o
                    .is_stripped
                    .ok_or_else(|| Error::Value("object without is_stripped".into()))?);
            }
        }
        if v >= 11 {
            w.i32(self.script_types.len() as i32);
            for s in &self.script_types {
                w.i32(s.file_index);
                if v < 14 {
                    w.i32(s.identifier as i32);
                } else {
                    w.align(4);
                    w.i64(s.identifier);
                }
            }
        }
        w.i32(self.externals.len() as i32);
        for e in &self.externals {
            if v >= 6 {
                w.cstr(
                    e.temp_empty
                        .as_ref()
                        .ok_or_else(|| Error::Value("external without temp_empty".into()))?,
                );
            }
            if v >= 5 {
                w.bytes(
                    &e.guid
                        .ok_or_else(|| Error::Value("external without guid".into()))?,
                );
                w.i32(
                    e.typ
                        .ok_or_else(|| Error::Value("external without type".into()))?,
                );
            }
            w.cstr(&e.path);
        }
        if v >= 20 {
            let rt = self
                .ref_types
                .as_ref()
                .ok_or_else(|| Error::Value("no ref types".into()))?;
            w.i32(rt.len() as i32);
            for t in rt {
                t.write(&mut w, v, self.enable_type_tree, true, &self.common)?;
            }
        }
        if v >= 5 {
            w.cstr(
                self.user_information
                    .as_ref()
                    .ok_or_else(|| Error::Value("no user information".into()))?,
            );
        }
        Ok(w.buf)
    }

    /// The header for a metadata length and data length: (header bytes, data offset).
    fn header_bytes(&self, metadata_size: usize, data_size: u64) -> (Vec<u8>, u64) {
        let h = &self.header;
        let header_size: u64 = if h.version < 22 { 20 } else { 48 };
        let mut data_offset = header_size + metadata_size as u64;
        data_offset += (16 - data_offset % 16) % 16;
        let file_size = data_offset + data_size;
        let mut w = Writer::new(true);
        if h.version < 22 {
            w.u32(metadata_size as u32);
            w.u32(file_size as u32);
            w.u32(h.version);
            w.u32(data_offset as u32);
            w.u8(h.big as u8);
            w.bytes(&h.reserved);
        } else {
            w.u32(0);
            w.u32(0);
            w.u32(h.version);
            w.u32(0);
            w.u8(h.big as u8);
            w.bytes(&h.reserved);
            w.u32(metadata_size as u32);
            w.u64(file_size);
            w.u64(data_offset);
            w.i64(h.unknown);
        }
        (w.buf, data_offset)
    }

    /// The object table's order when saved: by path id.
    fn save_order(&self) -> Vec<usize> {
        let mut order: Vec<usize> = (0..self.objects.len()).collect();
        order.sort_by_key(|&i| self.objects[i].path_id);
        order
    }

    /// UnityPy's sf.save(): the whole file in memory.
    pub fn save(&self) -> Result<Vec<u8>> {
        let mut out = std::io::Cursor::new(Vec::new());
        self.save_to(&mut out, |_, _| Ok(None))?;
        Ok(out.into_inner())
    }

    /// The file written to `out` (from its current position) as save() gives it, the objects'
    /// data streamed: in memory the metadata and one object at a time. `prepare(file, object)`
    /// is called before each object is written and may return bytes to write for it this time
    /// only (pmunity's prepare: a texture given its pixels for this write).
    pub fn save_to<W: Write + Seek>(
        &self,
        out: &mut W,
        mut prepare: impl FnMut(&SerializedFile, &ObjectInfo) -> Result<Option<Vec<u8>>>,
    ) -> Result<()> {
        let order = self.save_order();
        // the metadata's length does not depend on where the objects are (fixed-size fields)
        let dummy = vec![(0u64, 0u32); order.len()];
        let mlen = self.metadata(&dummy, &order)?.len();
        let (_, data_offset) = self.header_bytes(mlen, 0);
        let base = out.stream_position()?;
        out.seek(SeekFrom::Start(base + data_offset))?;
        let mut pos: u64 = 0;
        let mut placed = Vec::with_capacity(order.len());
        for &i in &order {
            let o = &self.objects[i];
            let once = prepare(self, o)?;
            let size = match (&once, &o.data) {
                (Some(d), _) | (None, Some(d)) => {
                    out.write_all(d)?;
                    d.len() as u64
                }
                (None, None) => {
                    self.src.copy_to(o.byte_start, o.byte_size as u64, out)?;
                    o.byte_size as u64
                }
            };
            if size > u32::MAX as u64 {
                return Err(Error::Unsupported(format!(
                    "object {} of {size} bytes",
                    o.path_id
                )));
            }
            placed.push((pos, size as u32));
            pos += size;
            let pad = (8 - pos % 8) % 8;
            out.write_all(&[0u8; 8][..pad as usize])?;
            pos += pad;
        }
        let meta = self.metadata(&placed, &order)?;
        if meta.len() != mlen {
            return bad("metadata length changed between passes");
        }
        let (head, _) = self.header_bytes(mlen, pos);
        let end = base + data_offset + pos;
        out.seek(SeekFrom::Start(base))?;
        out.write_all(&head)?;
        out.write_all(&meta)?;
        let gap = data_offset - (head.len() + meta.len()) as u64;
        out.write_all(&vec![0u8; gap as usize])?;
        out.seek(SeekFrom::Start(end))?;
        Ok(())
    }
}

/// The target platform field's place in a serialized file's first bytes (android_data's
/// sf_platform_offset over unity_version.sf_header; formats 9 - 23): (offset, big-endian,
/// current value, unity version).
pub fn platform_offset(head: &[u8]) -> Result<(usize, bool, u32, String)> {
    if head.len() < 40 {
        return bad("short file");
    }
    let fmt = u32::from_be_bytes(head[8..12].try_into().unwrap());
    if !(9..=23).contains(&fmt) {
        return bad(format!("unsupported SerializedFile format {fmt}"));
    }
    let p = if fmt >= 22 { 48 } else { 20 };
    let big = head[16] != 0;
    let end = head
        .get(p..)
        .and_then(|r| r.iter().position(|&c| c == 0))
        .map(|e| p + e);
    let end = end.ok_or_else(|| Error::Format("no unity version string".into()))?;
    let uv = &head[p..end];
    if !uv.is_ascii() {
        return bad("unity version string is not ASCII");
    }
    let uv = String::from_utf8(uv.to_vec()).unwrap();
    // ^\d+\.\d+\.\d+
    let mut it = uv.splitn(3, '.');
    let num = |s: Option<&str>, whole: bool| {
        s.is_some_and(|s| {
            let d = s.bytes().take_while(|c| c.is_ascii_digit()).count();
            d > 0 && (!whole || d == s.len())
        })
    };
    if !(num(it.next(), true) && num(it.next(), true) && num(it.next(), false)) {
        return bad(format!("bad unity version string {uv:?}"));
    }
    let off = end + 1;
    let b: [u8; 4] = head
        .get(off..off + 4)
        .ok_or_else(|| Error::Format("short file".into()))?
        .try_into()
        .unwrap();
    let cur = if big {
        u32::from_be_bytes(b)
    } else {
        u32::from_le_bytes(b)
    };
    Ok((off, big, cur, uv))
}
