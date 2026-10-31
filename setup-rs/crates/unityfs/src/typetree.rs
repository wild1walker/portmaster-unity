//! Type trees: the node lists that say how an object's bytes are laid out, from a serialized
//! file (its blob, or the old nested format) or the class database, and the blob written back as
//! UnityPy's TypeTreeNode.dump_blob writes it (common strings by the database's latest table).
use crate::bin::{Reader, Writer};
use crate::error::{bad, Error, Result};
use std::collections::HashMap;
use std::sync::Arc;

/// A node as typetrees.py compares trees: (level, type, name, meta flag).
pub type Shape = (i32, Arc<str>, Arc<str>, Option<i32>);

/// kAlignBytesFlag: the reader moves to a multiple of 4 after this field.
pub const ALIGN: i32 = 0x4000;

/// What a node's type reads as (UnityPyBoost's NodeDataType).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    U8,
    U16,
    U32,
    U64,
    I8,
    I16,
    I32,
    I64,
    F32,
    F64,
    Bool,
    Str,
    Bytes,
    Pair,
    Array,
    PPtr,
    RefObject,
    RefObjectData,
    RefRegistry,
    Other,
}

impl Kind {
    /// UnityPyBoost's get_node_data_type.
    pub fn of(t: &str) -> Kind {
        if t.starts_with("PPtr<") {
            return Kind::PPtr;
        }
        match t {
            "SInt8" => Kind::I8,
            "UInt8" | "char" => Kind::U8,
            "short" | "SInt16" => Kind::I16,
            "unsigned short" | "UInt16" => Kind::U16,
            "int" | "SInt32" => Kind::I32,
            "unsigned int" | "UInt32" | "Type*" => Kind::U32,
            "long long" | "SInt64" => Kind::I64,
            "unsigned long long" | "UInt64" | "FileSize" => Kind::U64,
            "float" => Kind::F32,
            "double" => Kind::F64,
            "bool" => Kind::Bool,
            "string" => Kind::Str,
            "TypelessData" => Kind::Bytes,
            "pair" => Kind::Pair,
            "Array" => Kind::Array,
            "ReferencedObject" => Kind::RefObject,
            "ReferencedObjectData" => Kind::RefObjectData,
            "ManagedReferencesRegistry" => Kind::RefRegistry,
            _ => Kind::Other,
        }
    }

    /// A plain number or bool (UnityPy writes these with FUNCTION_WRITE_MAP).
    pub fn is_number(self) -> bool {
        matches!(
            self,
            Kind::U8
                | Kind::U16
                | Kind::U32
                | Kind::U64
                | Kind::I8
                | Kind::I16
                | Kind::I32
                | Kind::I64
                | Kind::F32
                | Kind::F64
                | Kind::Bool
        )
    }

    /// Bytes a number takes.
    pub fn size(self) -> usize {
        match self {
            Kind::U8 | Kind::I8 | Kind::Bool => 1,
            Kind::U16 | Kind::I16 => 2,
            Kind::U32 | Kind::I32 | Kind::F32 => 4,
            Kind::U64 | Kind::I64 | Kind::F64 => 8,
            _ => 0,
        }
    }
}

/// One node. The fields a source does not have are None (UnityPy's None): the class database's
/// nodes have no type flags, the old format no ref type hash, and so on.
#[derive(Clone, Debug)]
pub struct Node {
    pub level: i32,
    pub type_name: Arc<str>,
    pub name: Arc<str>,
    pub byte_size: i32,
    pub version: i32,
    pub type_flags: Option<i32>,
    pub variable_count: Option<i32>,
    pub index: Option<i32>,
    pub meta_flag: Option<i32>,
    pub ref_type_hash: Option<u64>,
    /// Indices of the children in the tree's node list.
    pub children: Vec<usize>,
    pub kind: Kind,
}

impl Node {
    /// A node with the given fields and no children.
    pub fn new(level: i32, type_name: &str, name: &str, byte_size: i32, version: i32) -> Self {
        Node {
            level,
            type_name: type_name.into(),
            name: name.into(),
            byte_size,
            version,
            type_flags: None,
            variable_count: None,
            index: None,
            meta_flag: None,
            ref_type_hash: None,
            children: Vec::new(),
            kind: Kind::of(type_name),
        }
    }

    /// The kAlignBytes flag.
    pub fn aligned(&self) -> bool {
        self.meta_flag.unwrap_or(0) & ALIGN != 0
    }
}

/// A class's type tree: its nodes in pre-order, the class itself first.
#[derive(Clone, Debug)]
pub struct TypeTree {
    pub nodes: Vec<Node>,
}

impl TypeTree {
    /// A tree from nodes in pre-order with their levels set (UnityPy's TypeTreeNode.from_list:
    /// a node deeper than the one before is its child, else a sibling of the nearest shallower
    /// ancestor; only the first node's own subtree is kept).
    pub fn from_list(mut list: Vec<Node>) -> Result<Self> {
        if list.is_empty() {
            return bad("empty type tree");
        }
        for n in list.iter_mut() {
            n.children.clear();
        }
        // stack of parents (indices; usize::MAX is the fake root at level -1)
        const ROOT: usize = usize::MAX;
        let level = |list: &Vec<Node>, i: usize| if i == ROOT { -1 } else { list[i].level };
        let mut stack: Vec<usize> = vec![ROOT];
        let mut parent = ROOT;
        let mut prev = ROOT;
        let mut root_children = Vec::new();
        for i in 0..list.len() {
            let l = list[i].level;
            if l > level(&list, prev) {
                stack.push(parent);
                parent = prev;
            } else if l < level(&list, prev) {
                while l <= level(&list, parent) {
                    match stack.pop() {
                        Some(p) => parent = p,
                        None => return bad("type tree: a node above the root's level"),
                    }
                }
            }
            if parent == ROOT {
                root_children.push(i);
            } else {
                list[parent].children.push(i);
            }
            prev = i;
        }
        // keep the subtree of the first node: in pre-order it is a prefix of the list
        let end = root_children.get(1).copied().unwrap_or(list.len());
        list.truncate(end);
        Ok(TypeTree { nodes: list })
    }

    pub fn root(&self) -> &Node {
        &self.nodes[0]
    }

    /// The class's type name (the root node's type, e.g. "Texture2D").
    pub fn class_name(&self) -> &str {
        &self.nodes[0].type_name
    }

    /// The root's direct fields: (index, node).
    pub fn fields(&self) -> impl Iterator<Item = (usize, &Node)> {
        self.nodes[0].children.iter().map(|&i| (i, &self.nodes[i]))
    }

    /// UnityPy's TypeTreeNode.parse_blob (serialized file format 10, 12 and later).
    pub fn parse_blob(r: &mut Reader, version: u32, common: &CommonStrings) -> Result<Self> {
        if version >= 23 {
            if r.take(4)? != b"mhtt" {
                return bad("type tree blob: bad magic");
            }
            if r.i32()? as u32 != version {
                return bad("type tree blob: inconsistent format version");
            }
        }
        let count = r.i32()?;
        let strsize = r.i32()?;
        if count < 0 || strsize < 0 {
            return bad("type tree blob: negative size");
        }
        let nsize = if version >= 19 { 32 } else { 24 };
        let raw = r.take(nsize * count as usize)?;
        let strings = r.take(strsize as usize)?;
        let mut nr = Reader::new(raw, r.big);
        let mut list = Vec::with_capacity(count as usize);
        let s = |off: u32| -> Result<Arc<str>> {
            if off & 0x8000_0000 == 0 {
                let o = off as usize;
                let rest = strings.get(o..).unwrap_or(&[]);
                let e = rest.iter().position(|&c| c == 0).unwrap_or(rest.len());
                std::str::from_utf8(&rest[..e])
                    .map(Arc::from)
                    .map_err(|_| Error::Format("type tree string is not UTF-8".into()))
            } else {
                if common.is_empty() {
                    return bad(
                        "type tree blob with common strings, and no class database for them",
                    );
                }
                let o = off & 0x7FFF_FFFF;
                Ok(common
                    .get(o)
                    .map(Arc::from)
                    .unwrap_or_else(|| Arc::from(o.to_string())))
            }
        };
        for _ in 0..count {
            let version = nr.i16()? as i32;
            let level = nr.u8()? as i32;
            let type_flags = nr.u8()? as i32;
            let tofs = nr.u32()?;
            let nofs = nr.u32()?;
            let byte_size = nr.i32()?;
            let index = nr.i32()?;
            let meta = nr.i32()?;
            let hash = if version_has_hash(nsize) {
                Some(nr.u64()?)
            } else {
                None
            };
            let t = s(tofs)?;
            let n = s(nofs)?;
            list.push(Node {
                level,
                kind: Kind::of(&t),
                type_name: t,
                name: n,
                byte_size,
                version,
                type_flags: Some(type_flags),
                variable_count: None,
                index: Some(index),
                meta_flag: Some(meta),
                ref_type_hash: hash,
                children: Vec::new(),
            });
        }
        Self::from_list(list)
    }

    /// UnityPy's TypeTreeNode.parse (the old nested format: formats below 10, and 11).
    pub fn parse_old(r: &mut Reader, version: u32) -> Result<Self> {
        let mut list = Vec::new();
        // (level of the node's children, children left)
        let mut stack: Vec<(i32, i32)> = vec![(0, 1)];
        while let Some(&mut (level, ref mut left)) = stack.last_mut() {
            *left -= 1;
            if *left == 0 {
                stack.pop();
            }
            let t = r.cstring()?;
            let n = r.cstring()?;
            let byte_size = r.i32()?;
            let variable_count = if version == 2 { Some(r.i32()?) } else { None };
            let index = if version != 3 { Some(r.i32()?) } else { None };
            let type_flags = r.i32()?;
            let ver = r.i32()?;
            let meta = if version != 3 { Some(r.i32()?) } else { None };
            let mut node = Node::new(level, &t, &n, byte_size, ver);
            node.variable_count = variable_count;
            node.index = index;
            node.type_flags = Some(type_flags);
            node.meta_flag = meta;
            list.push(node);
            let kids = r.i32()?;
            if kids > 0 {
                stack.push((level + 1, kids));
            }
        }
        Self::from_list(list)
    }

    /// UnityPy's TypeTreeNode.dump_blob: the nodes in pre-order, each type and name either a
    /// common string (the database's latest table; offset 0, "AABB", never counts as one) or
    /// in the tree's own string buffer, in the order first used.
    pub fn dump_blob(&self, w: &mut Writer, version: u32, common: &CommonStrings) -> Result<()> {
        let mut nodes = Writer::new(w.big);
        let mut strbuf: Vec<u8> = Vec::new();
        let mut offsets: HashMap<&str, u32> = HashMap::new();
        for n in self.preorder() {
            let n = &self.nodes[n];
            let v = n.version;
            if !(-32768..=32767).contains(&v) {
                return Err(Error::Value(format!(
                    "type tree: version {v} does not fit 16 bits"
                )));
            }
            let level = u8::try_from(n.level)
                .map_err(|_| Error::Value(format!("type tree: level {}", n.level)))?;
            let tf = n
                .type_flags
                .ok_or_else(|| Error::Value("type tree node without type flags".into()))?;
            let tf = u8::try_from(tf)
                .map_err(|_| Error::Value(format!("type tree: type flags {tf}")))?;
            nodes.i16(v as i16);
            nodes.u8(level);
            nodes.u8(tf);
            let t = blob_string(&n.type_name, &mut offsets, &mut strbuf, common);
            let nm = blob_string(&n.name, &mut offsets, &mut strbuf, common);
            nodes.u32(t);
            nodes.u32(nm);
            nodes.i32(n.byte_size);
            nodes.i32(
                n.index
                    .ok_or_else(|| Error::Value("type tree node without index".into()))?,
            );
            nodes.i32(
                n.meta_flag
                    .ok_or_else(|| Error::Value("type tree node without meta flag".into()))?,
            );
            if version >= 19 {
                nodes.u64(
                    n.ref_type_hash.ok_or_else(|| {
                        Error::Value("type tree node without ref type hash".into())
                    })?,
                );
            }
        }
        let count = self.preorder().count();
        if version >= 23 {
            w.bytes(b"mhtt");
            w.i32(version as i32);
        }
        w.i32(count as i32);
        w.i32(strbuf.len() as i32);
        w.bytes(&nodes.buf);
        w.bytes(&strbuf);
        Ok(())
    }

    /// Node indices in pre-order from the root (the list order for a tree built by from_list).
    pub fn preorder(&self) -> impl Iterator<Item = usize> + '_ {
        let mut stack = vec![0usize];
        std::iter::from_fn(move || {
            let n = stack.pop()?;
            stack.extend(self.nodes[n].children.iter().rev());
            Some(n)
        })
    }

    /// The shape UnityPy compares trees by in typetrees.py (level, type, name, meta flag of each node).
    pub fn shape(&self) -> Vec<Shape> {
        self.preorder()
            .map(|i| {
                let n = &self.nodes[i];
                (n.level, n.type_name.clone(), n.name.clone(), n.meta_flag)
            })
            .collect()
    }

    /// A copy with new top-level fields inserted after named ones (typetrees._with_fields):
    /// [(field it follows, type, name, aligned)]; None if a field it follows is missing or every
    /// field is already there. The copy's nodes are renumbered (m_Index) and have no children
    /// lists beyond the hierarchy.
    pub fn with_fields(&self, fields: &[(&str, &str, &str, bool)]) -> Option<TypeTree> {
        let mut list: Vec<Node> = self.preorder().map(|i| self.nodes[i].clone()).collect();
        let mut added = 0;
        for &(after, typ, name, aligned) in fields {
            let top: Vec<usize> = (0..list.len()).filter(|&i| list[i].level == 1).collect();
            if top.iter().any(|&i| &*list[i].name == name) {
                continue;
            }
            let at = top.iter().position(|&i| &*list[i].name == after)?;
            let mut i = top[at] + 1;
            while i < list.len() && list[i].level > 1 {
                i += 1;
            }
            let mut d = list[top[0]].clone();
            d.type_name = typ.into();
            d.kind = Kind::of(typ);
            d.name = name.into();
            d.byte_size = Kind::of(typ).size() as i32;
            d.version = 1;
            d.type_flags = Some(0);
            d.meta_flag = Some(if aligned { ALIGN } else { 0 });
            list.insert(i, d);
            added += 1;
        }
        if added == 0 {
            return None;
        }
        for (k, d) in list.iter_mut().enumerate() {
            d.index = Some(k as i32);
        }
        TypeTree::from_list(list).ok()
    }
}

fn version_has_hash(nsize: usize) -> bool {
    nsize == 32
}

/// A string's offset in a blob being written: one used before, a common string's (flagged),
/// or a new one appended to the tree's own buffer.
fn blob_string<'a>(
    s: &'a str,
    offsets: &mut HashMap<&'a str, u32>,
    strbuf: &mut Vec<u8>,
    common: &CommonStrings,
) -> u32 {
    if let Some(&o) = offsets.get(s) {
        return o;
    }
    let o = match common.offset_of(s) {
        Some(c) if c != 0 => c | 0x8000_0000,
        _ => {
            let o = strbuf.len() as u32;
            strbuf.extend_from_slice(s.as_bytes());
            strbuf.push(0);
            o
        }
    };
    offsets.insert(s, o);
    o
}

/// Unity's common strings (type and field names every file shares): offset -> string, from the
/// class database's latest table (UnityPy's get_common_strings()).
#[derive(Clone, Debug, Default)]
pub struct CommonStrings {
    by_offset: HashMap<u32, Arc<str>>,
    by_string: HashMap<Arc<str>, u32>,
}

impl CommonStrings {
    /// From (offset, string) in the table's order. As UnityPy's dicts: an offset listed twice
    /// keeps its first place with its last string, and {string: offset} is built in that order
    /// (a string at two offsets is written with the later one).
    pub fn new(entries: impl IntoIterator<Item = (u32, Arc<str>)>) -> Self {
        let mut c = CommonStrings::default();
        let mut order = Vec::new();
        for (o, s) in entries {
            if c.by_offset.insert(o, s).is_none() {
                order.push(o);
            }
        }
        for o in order {
            c.by_string.insert(c.by_offset[&o].clone(), o);
        }
        c
    }

    pub fn get(&self, offset: u32) -> Option<&str> {
        self.by_offset.get(&offset).map(|s| &**s)
    }

    pub fn offset_of(&self, s: &str) -> Option<u32> {
        self.by_string.get(s).copied()
    }

    pub fn len(&self) -> usize {
        self.by_offset.len()
    }

    pub fn is_empty(&self) -> bool {
        self.by_offset.is_empty()
    }
}
