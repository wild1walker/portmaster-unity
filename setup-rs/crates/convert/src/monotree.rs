//! Type trees of MonoBehaviour data from the game's own assemblies, as monotree.py builds them
//! (its docstring has Unity's serialization rules): the class's serialized fields and its bases'
//! (base first) from the assemblies' metadata (androiddata's clrmeta), laid out as Unity serializes
//! them. A tree is used only when it reads the object's bytes exactly ([`read`]).
use androiddata::clrmeta::{self as cm, Assembly, Coded};
use std::collections::{BTreeSet, HashMap};
use std::path::{Path, PathBuf};
use std::rc::Rc;
use unityfs::{Node, SerializedFile, TypeTree, Value};

const ALIGN: i32 = 0x4000;
const F_ACCESS: u32 = 0x7;
const F_PUBLIC: u32 = 0x6;
const F_STATIC: u32 = 0x10;
const F_INITONLY: u32 = 0x20;
const F_LITERAL: u32 = 0x40;
const F_NOTSERIALIZED: u32 = 0x80;
const T_INTERFACE: u32 = 0x20;
const T_ABSTRACT: u32 = 0x80;
const T_SERIALIZABLE: u32 = 0x2000;
const GENERICPARAM: u8 = 0x2A;
const TYPESPEC: u8 = 0x1B;
const MEMBERREF: u8 = 0x0A;
const CUSTOMATTRIBUTE: u8 = 0x0C;
const EXPORTEDTYPE: u8 = 0x27;
const MAX_DEPTH: usize = 10;

/// PRIM_TYPES: (type name, size) of a primitive element type.
fn prim(e: u8) -> Option<(&'static str, i32)> {
    Some(match e {
        cm::E_BOOL => ("bool", 1),
        cm::E_CHAR => ("UInt16", 2),
        cm::E_I1 => ("SInt8", 1),
        cm::E_U1 => ("UInt8", 1),
        cm::E_I2 => ("SInt16", 2),
        cm::E_U2 => ("UInt16", 2),
        cm::E_I4 => ("int", 4),
        cm::E_U4 => ("unsigned int", 4),
        cm::E_I8 => ("SInt64", 8),
        cm::E_U8 => ("UInt64", 8),
        cm::E_R4 => ("float", 4),
        cm::E_R8 => ("double", 8),
        _ => return None,
    })
}

/// A field of a BUILTIN layout: (type, name) or (type name, name, fields).
enum BField {
    Leaf(&'static str, String),
    Nested(&'static str, &'static str, Vec<BField>),
}

fn floats(names: &[&str]) -> Vec<BField> {
    names
        .iter()
        .map(|n| BField::Leaf("float", n.to_string()))
        .collect()
}

fn ints(names: &[&str]) -> Vec<BField> {
    names
        .iter()
        .map(|n| BField::Leaf("int", n.to_string()))
        .collect()
}

/// Unity's own value types, serialized natively (monotree.BUILTIN).
fn builtin(full: &str) -> Option<(&'static str, Vec<BField>)> {
    Some(match full {
        "UnityEngine.Vector2" => ("Vector2f", floats(&["x", "y"])),
        "UnityEngine.Vector3" => ("Vector3f", floats(&["x", "y", "z"])),
        "UnityEngine.Vector4" => ("Vector4f", floats(&["x", "y", "z", "w"])),
        "UnityEngine.Quaternion" => ("Quaternionf", floats(&["x", "y", "z", "w"])),
        "UnityEngine.Color" => ("ColorRGBA", floats(&["r", "g", "b", "a"])),
        "UnityEngine.Color32" => (
            "ColorRGBA",
            vec![BField::Leaf("unsigned int", "rgba".into())],
        ),
        "UnityEngine.Rect" => ("Rectf", floats(&["x", "y", "width", "height"])),
        "UnityEngine.Matrix4x4" => (
            "Matrix4x4f",
            (0..4)
                .flat_map(|r| (0..4).map(move |c| BField::Leaf("float", format!("e{r}{c}"))))
                .collect(),
        ),
        "UnityEngine.LayerMask" => (
            "BitField",
            vec![BField::Leaf("unsigned int", "m_Bits".into())],
        ),
        "UnityEngine.RenderingLayerMask" => (
            "RenderingLayerMask",
            vec![BField::Leaf("unsigned int", "m_Bits".into())],
        ),
        "UnityEngine.Bounds" => (
            "AABB",
            vec![
                BField::Nested("Vector3f", "m_Center", floats(&["x", "y", "z"])),
                BField::Nested("Vector3f", "m_Extent", floats(&["x", "y", "z"])),
            ],
        ),
        "UnityEngine.Vector2Int" => ("int2_storage", ints(&["x", "y"])),
        "UnityEngine.Vector3Int" => ("int3_storage", ints(&["x", "y", "z"])),
        "UnityEngine.RectInt" => ("RectInt", ints(&["x", "y", "width", "height"])),
        "UnityEngine.BoundsInt" => (
            "BoundsInt",
            vec![
                BField::Nested("int3_storage", "m_Position", ints(&["x", "y", "z"])),
                BField::Nested("int3_storage", "m_Size", ints(&["x", "y", "z"])),
            ],
        ),
        "UnityEngine.RectOffset" => (
            "RectOffset",
            ints(&["m_Left", "m_Right", "m_Top", "m_Bottom"]),
        ),
        "UnityEngine.Hash128" => (
            "Hash128",
            (0..16)
                .map(|i| BField::Leaf("UInt8", format!("bytes[{i}]")))
                .collect(),
        ),
        "UnityEngine.Rendering.SphericalHarmonicsL2" => (
            "SphericalHarmonicsL2",
            (0..27)
                .map(|i| BField::Leaf("float", format!("sh[{i:2}]")))
                .collect(),
        ),
        _ => return None,
    })
}

const NATIVE: [&str; 9] = [
    "UnityEngine.AnimationCurve",
    "UnityEngine.Gradient",
    "UnityEngine.GUIStyle",
    "UnityEngine.ExposedReference`1",
    "UnityEngine.LazyLoadReference`1",
    "UnityEngine.PropertyName",
    "UnityEngine.Keyframe",
    "UnityEngine.ParticleSystem/MinMaxCurve",
    "UnityEngine.ParticleSystem/MinMaxGradient",
];
const STOP: [&str; 7] = [
    "UnityEngine.MonoBehaviour",
    "UnityEngine.ScriptableObject",
    "UnityEngine.Behaviour",
    "UnityEngine.Component",
    "UnityEngine.Object",
    "System.Object",
    "System.ValueType",
];
const LIST: &str = "System.Collections.Generic.List`1";

/// Why no tree is built (monotree.Unsupported), or a fault reading the metadata (the Python's
/// ValueError, IndexError, KeyError, struct.error).
#[derive(Debug)]
pub enum Why {
    Unsupported(String),
    Fault(String, String),
}

impl From<cm::Error> for Why {
    fn from(e: cm::Error) -> Why {
        let kind = match e {
            cm::Error::Struct(_) => "error",
            cm::Error::Key(_) => "KeyError",
            _ => "ValueError",
        };
        Why::Fault(kind.into(), e.to_string())
    }
}

type R<T> = std::result::Result<T, Why>;

/// `rows[row - 1]` with Python's indexing (row 0: the last).
fn py_row<T>(rows: &[T], row: u32) -> R<&T> {
    let i = if row == 0 {
        rows.len().checked_sub(1)
    } else {
        Some(row as usize - 1)
    };
    i.and_then(|i| rows.get(i)).ok_or_else(index_error)
}

fn index_error() -> Why {
    Why::Fault("IndexError".into(), "list index out of range".into())
}

/// One TypeDef row of an assembly.
#[derive(Clone)]
pub struct TypeDef {
    asm: Rc<Assembly>,
    asm_name: String,
    full: String,
    name: String,
    flags: u32,
    extends: (Option<u8>, u32),
    fields: (u32, u32),
    nparams: u32,
}

/// A signature type (the Python's tuples): ('prim', e), ('string',), ('array', t), ('type',
/// TypeDef, args), ('ext', full[, args]), ('unsupported', why).
#[derive(Clone)]
enum Sig {
    Prim(u8),
    Str,
    Array(Box<Sig>),
    Type(Rc<TypeDef>, Vec<Sig>),
    Ext(String, Option<Vec<Sig>>),
    Unsupported,
}

fn full_of(t: &Sig) -> String {
    match t {
        Sig::Type(td, _) => td.full.clone(),
        Sig::Ext(f, _) => f.clone(),
        _ => String::new(),
    }
}

/// A node of a generated tree (monotree._node).
fn node(level: i32, typ: &str, name: &str, size: i32, flag: i32, typeflags: i32) -> Node {
    let mut n = Node::new(level, typ, name, size, 1);
    n.meta_flag = Some(flag);
    n.type_flags = Some(typeflags);
    n
}

fn string_nodes(level: i32, name: &str) -> Vec<Node> {
    vec![
        node(level, "string", name, -1, ALIGN, 0),
        node(level + 1, "Array", "Array", -1, ALIGN, 1),
        node(level + 2, "int", "size", 4, 0, 0),
        node(level + 2, "char", "data", 1, 0, 0),
    ]
}

fn vector_nodes(level: i32, name: &str, sub: Vec<Node>) -> Vec<Node> {
    let mut out = vec![
        node(level, "vector", name, -1, 0, 0),
        node(level + 1, "Array", "Array", -1, ALIGN, 1),
        node(level + 2, "int", "size", 4, 0, 0),
    ];
    out.extend(sub);
    out
}

fn pptr_nodes(level: i32, name: &str, cls: &str) -> Vec<Node> {
    vec![
        node(level, &format!("PPtr<{cls}>"), name, 12, 0, 0),
        node(level + 1, "int", "m_FileID", 4, 0, 0),
        node(level + 1, "SInt64", "m_PathID", 8, 0, 0),
    ]
}

fn builtin_nodes(level: i32, name: &str, typ: &str, fields: &[BField]) -> Vec<Node> {
    let mut out = vec![node(level, typ, name, -1, 0, 0)];
    for f in fields {
        match f {
            BField::Nested(t, n, sub) => out.extend(builtin_nodes(level + 1, n, t, sub)),
            BField::Leaf(t, n) => {
                let size = if *t == "UInt8" { 1 } else { 4 };
                out.push(node(level + 1, t, n, size, 0, 0));
            }
        }
    }
    out
}

/// An assembly with its {full type name: TypeDef row} index.
type AsmIndex = (Rc<Assembly>, Rc<HashMap<String, u32>>);

/// Trees of MonoBehaviour classes from the assemblies of a Managed folder (monotree.Generator), or
/// of an IL2CPP build's metadata made into assemblies (androiddata's il2cppmeta).
pub struct Generator {
    managed: PathBuf,
    il2cpp: Option<androiddata::il2cppmeta::Il2Cpp>,
    unity: (u64, u64),
    asm: HashMap<String, Option<AsmIndex>>,
    attrs: HashMap<String, Rc<HashMap<u32, BTreeSet<String>>>>,
    gparams: HashMap<String, Rc<HashMap<u32, u32>>>,
    cache: HashMap<(String, u32), Rc<TypeDef>>,
}

impl Generator {
    pub fn new(managed: &Path, unity: Option<(u64, u64)>) -> Generator {
        Generator {
            managed: managed.to_path_buf(),
            il2cpp: None,
            unity: unity.unwrap_or((2022, 3)),
            asm: HashMap::new(),
            attrs: HashMap::new(),
            gparams: HashMap::new(),
            cache: HashMap::new(),
        }
    }

    /// The generator of a game's data folder: its Managed assemblies, else (an IL2CPP build) its
    /// metadata's, else none; with why an IL2CPP build's metadata cannot be read.
    pub fn of_data(data: &Path, unity: Option<(u64, u64)>) -> (Generator, Option<String>) {
        let mut g = Generator::new(&data.join("Managed"), unity);
        if data.join("Managed").is_dir() {
            return (g, None);
        }
        let Some((meta, bin)) = androiddata::il2cppmeta::files_of(data) else {
            return (g, None);
        };
        match androiddata::il2cppmeta::Il2Cpp::open(&meta, &bin) {
            Ok(il) => {
                g.il2cpp = Some(il);
                (g, None)
            }
            Err(why) => (g, Some(format!("IL2CPP types not read: {why}"))),
        }
    }

    /// The assembly named `name` (without .dll) with its {full name: row} index, or None.
    fn assembly(&mut self, name: &str) -> Option<AsmIndex> {
        if let Some(a) = self.asm.get(name) {
            return a.clone();
        }
        let p = self.managed.join(format!("{name}.dll"));
        let made = match &mut self.il2cpp {
            Some(il) => il.assembly(name).ok().flatten(),
            None => p.is_file().then(|| Assembly::open(&p).ok()).flatten(),
        };
        // ({t['full']: i + 1 for ...}: the last row of a name wins)
        let a = made.and_then(|a| {
            let idx: HashMap<String, u32> = a
                .types()
                .ok()?
                .iter()
                .enumerate()
                .map(|(i, t)| (t.full.clone(), i as u32 + 1))
                .collect();
            Some((Rc::new(a), Rc::new(idx)))
        });
        self.asm.insert(name.to_string(), a.clone());
        a
    }

    /// The TypeDef of `full` in that assembly, following type forwarders, or None.
    fn find(&mut self, asm_name: &str, full: &str) -> R<Option<Rc<TypeDef>>> {
        let mut seen = BTreeSet::new();
        let mut name = asm_name.to_string();
        while !name.is_empty() && seen.insert(name.clone()) {
            let Some((a, idx)) = self.assembly(&name) else {
                return Ok(None);
            };
            if let Some(&row) = idx.get(full) {
                return Ok(Some(self.typedef(&name, &a, row)?));
            }
            match forwarded(&a, full)? {
                Some(n) => name = n,
                None => return Ok(None),
            }
        }
        Ok(None)
    }

    fn typedef(&mut self, asm_name: &str, a: &Rc<Assembly>, row: u32) -> R<Rc<TypeDef>> {
        let k = (asm_name.to_string(), row);
        if let Some(t) = self.cache.get(&k) {
            return Ok(t.clone());
        }
        let ts = a.types()?;
        let t = py_row(ts, row)?;
        let nparams = *self.generic_params(asm_name, a)?.get(&row).unwrap_or(&0);
        let td = Rc::new(TypeDef {
            asm: a.clone(),
            asm_name: asm_name.to_string(),
            full: t.full.clone(),
            name: t.name.clone(),
            flags: t.flags,
            extends: t.extends,
            fields: t.fields,
            nparams,
        });
        self.cache.insert(k, td.clone());
        Ok(td)
    }

    fn generic_params(&mut self, asm_name: &str, a: &Assembly) -> R<Rc<HashMap<u32, u32>>> {
        if let Some(g) = self.gparams.get(asm_name) {
            return Ok(g.clone());
        }
        let mut n: HashMap<u32, u32> = HashMap::new();
        for r in a.rows(GENERICPARAM)?.iter() {
            let (num, owner) = (r[0], r[2]);
            let (tab, row) = Assembly::coded(Coded::TypeOrMethodDef, owner)?;
            if tab == Some(cm::TYPEDEF) {
                let e = n.entry(row).or_insert(0);
                *e = (*e).max(num + 1);
            }
        }
        let g = Rc::new(n);
        self.gparams.insert(asm_name.to_string(), g.clone());
        Ok(g)
    }

    /// {field row: {attribute type full names}} of an assembly.
    fn field_attributes(
        &mut self,
        asm_name: &str,
        a: &Assembly,
    ) -> R<Rc<HashMap<u32, BTreeSet<String>>>> {
        if let Some(x) = self.attrs.get(asm_name) {
            return Ok(x.clone());
        }
        let mut out: HashMap<u32, BTreeSet<String>> = HashMap::new();
        let mrefs = a.rows(MEMBERREF)?;
        for r in a.rows(CUSTOMATTRIBUTE)?.iter() {
            let (ptab, prow) = Assembly::coded(Coded::HasCustomAttribute, r[0])?;
            if ptab != Some(cm::FIELD) {
                continue;
            }
            let (ttab, trow) = Assembly::coded(Coded::CustomAttributeType, r[1])?;
            let mut name = None;
            if ttab == Some(MEMBERREF) && 1 <= trow && trow as usize <= mrefs.len() {
                let (ctab, crow) =
                    Assembly::coded(Coded::MemberRefParent, mrefs[trow as usize - 1][0])?;
                if ctab == Some(cm::TYPEDEF) || ctab == Some(cm::TYPEREF) {
                    name = Some(a.type_name(ctab, crow)?);
                }
            } else if ttab == Some(cm::METHODDEF) {
                name = a.type_of_method(trow)?.map(|t| t.full.clone());
            }
            if let Some(n) = name.filter(|n| !n.is_empty()) {
                out.entry(prow).or_default().insert(n);
            }
        }
        let x = Rc::new(out);
        self.attrs.insert(asm_name.to_string(), x.clone());
        Ok(x)
    }

    /// A TypeDefOrRef of an assembly as a signature type.
    fn resolve_ref(
        &mut self,
        td_asm: (&str, &Rc<Assembly>),
        tab: Option<u8>,
        row: u32,
        args: &[Sig],
    ) -> R<Sig> {
        let (an, a) = td_asm;
        match tab {
            Some(cm::TYPEDEF) => Ok(Sig::Type(self.typedef(an, a, row)?, Vec::new())),
            Some(cm::TYPEREF) => {
                let full = a.typeref_name(row)?;
                let rows = a.rows(cm::TYPEREF)?;
                let r = py_row(&rows, row)?;
                let (scope, name) = (r[0], r[1]);
                let (stab, srow) = Assembly::coded(Coded::ResolutionScope, scope)?;
                if stab == Some(cm::TYPEREF) {
                    let outer = self.resolve_ref(td_asm, Some(cm::TYPEREF), srow, &[])?;
                    let full = format!("{}/{}", full_of(&outer), a.string(name)?);
                    let Sig::Type(o, _) = outer else {
                        return Ok(Sig::Ext(full, None));
                    };
                    let on = o.asm_name.clone();
                    return Ok(match self.find(&on, &full)? {
                        Some(t) => Sig::Type(t, Vec::new()),
                        None => Sig::Ext(full, None),
                    });
                }
                if stab == Some(cm::ASSEMBLYREF) {
                    let arefs = a.rows(cm::ASSEMBLYREF)?;
                    let ar = py_row(&arefs, srow)?;
                    let asm = a.string(ar[6])?;
                    return Ok(match self.find(&asm, &full)? {
                        Some(t) => Sig::Type(t, Vec::new()),
                        None => Sig::Ext(full, None),
                    });
                }
                let idx = self.assembly(an).map(|x| x.1);
                match idx.and_then(|i| i.get(&full).copied()) {
                    Some(t) => Ok(Sig::Type(self.typedef(an, a, t)?, Vec::new())),
                    None => Ok(Sig::Ext(full, None)),
                }
            }
            Some(TYPESPEC) => {
                let rows = a.rows(TYPESPEC)?;
                let r = py_row(&rows, row)?;
                let b = a.blob(r[0])?;
                Ok(self.sig(td_asm, &b, 0, args)?.0)
            }
            _ => Ok(Sig::Ext("?".into(), None)),
        }
    }

    /// (type, offset): a signature type, generic parameters replaced by `args`.
    fn sig(
        &mut self,
        td_asm: (&str, &Rc<Assembly>),
        b: &[u8],
        mut o: usize,
        args: &[Sig],
    ) -> R<(Sig, usize)> {
        let at = |o: usize| b.get(o).copied().ok_or_else(index_error);
        let cu = |o: usize| cm::cuint(b, o).map_err(Why::from);
        let mut e = at(o)?;
        o += 1;
        while e == cm::E_CMOD_REQD || e == cm::E_CMOD_OPT {
            o = cu(o)?.1;
            e = at(o)?;
            o += 1;
        }
        if prim(e).is_some() {
            return Ok((Sig::Prim(e), o));
        }
        match e {
            cm::E_STRING => Ok((Sig::Str, o)),
            cm::E_VALUETYPE | cm::E_CLASS => {
                let (v, o) = cu(o)?;
                let (tab, row) = Assembly::coded(Coded::TypeDefOrRef, v)?;
                Ok((self.resolve_ref(td_asm, tab, row, &[])?, o))
            }
            cm::E_SZARRAY => {
                let (t, o) = self.sig(td_asm, b, o, args)?;
                Ok((Sig::Array(Box::new(t)), o))
            }
            cm::E_GENERICINST => {
                o += 1;
                let (v, mut o) = cu(o)?;
                let (tab, row) = Assembly::coded(Coded::TypeDefOrRef, v)?;
                let (n, o2) = cu(o)?;
                o = o2;
                let mut targs = Vec::new();
                for _ in 0..n {
                    let (t, o2) = self.sig(td_asm, b, o, args)?;
                    o = o2;
                    targs.push(t);
                }
                let base = self.resolve_ref(td_asm, tab, row, &[])?;
                Ok((
                    match base {
                        Sig::Type(t, _) => Sig::Type(t, targs),
                        other => Sig::Ext(full_of(&other), Some(targs)),
                    },
                    o,
                ))
            }
            cm::E_VAR => {
                let (n, o) = cu(o)?;
                Ok((args.get(n as usize).cloned().unwrap_or(Sig::Unsupported), o))
            }
            cm::E_MVAR => Ok((Sig::Unsupported, cu(o)?.1)),
            cm::E_ARRAY => {
                let (_, mut o) = self.sig(td_asm, b, o, args)?;
                o = cu(o)?.1;
                let (n, o2) = cu(o)?;
                o = o2;
                for _ in 0..n {
                    o = cu(o)?.1;
                }
                let (n, o2) = cu(o)?;
                o = o2;
                for _ in 0..n {
                    o = cu(o)?.1;
                }
                Ok((Sig::Unsupported, o))
            }
            cm::E_PTR | cm::E_BYREF => Ok((Sig::Unsupported, self.sig(td_asm, b, o, args)?.1)),
            cm::E_OBJECT => Ok((Sig::Ext("System.Object".into(), None), o)),
            cm::E_I | cm::E_U => Ok((Sig::Unsupported, o)),
            _ => Err(Why::Unsupported(format!("element type {e:#x}"))),
        }
    }

    fn base(&mut self, t: &TypeDef) -> R<Option<Sig>> {
        let (tab, r) = t.extends;
        if r == 0 {
            return Ok(None);
        }
        let a = t.asm.clone();
        Ok(Some(self.resolve_ref((&t.asm_name, &a), tab, r, &[])?))
    }

    fn is_enum(&mut self, t: &TypeDef) -> R<bool> {
        Ok(self.base(t)?.is_some_and(|b| full_of(&b) == "System.Enum"))
    }

    fn is_unity_object(&mut self, t: &Rc<TypeDef>) -> R<bool> {
        let mut t = t.clone();
        for _ in 0..32 {
            if t.full == "UnityEngine.Object" {
                return Ok(true);
            }
            match self.base(&t)? {
                Some(Sig::Type(b, _)) => t = b,
                Some(b) => return Ok(full_of(&b) == "UnityEngine.Object"),
                None => return Ok(false),
            }
        }
        Ok(false)
    }

    /// [(name, type)] of the serialized fields of a class and its bases (base first).
    fn fields(&mut self, td: &Rc<TypeDef>, args: &[Sig]) -> R<Vec<(String, Sig)>> {
        let mut chain: Vec<(Rc<TypeDef>, Vec<Sig>)> = Vec::new();
        let (mut t, mut targs) = (Some(td.clone()), args.to_vec());
        while let Some(tt) = t.clone() {
            if STOP.contains(&tt.full.as_str()) {
                break;
            }
            chain.push((tt.clone(), targs.clone()));
            let Some(mut b) = self.base(&tt)? else { break };
            let Sig::Type(_, bargs) = &b else { break };
            if !bargs.is_empty() {
                let a = tt.asm.clone();
                b = self.resolve_ref((&tt.asm_name, &a), tt.extends.0, tt.extends.1, &targs)?;
            }
            match b {
                Sig::Type(bt, ba) => {
                    t = Some(bt);
                    targs = ba;
                }
                _ => break,
            }
        }
        let mut out = Vec::new();
        for (t, targs) in chain.iter().rev() {
            let a = t.asm.clone();
            let attrs = self.field_attributes(&t.asm_name, &a)?;
            let rows = a.rows(cm::FIELD)?;
            for r in t.fields.0..t.fields.1 {
                let row = py_row(&rows, r)?;
                let (flags, name, sig) = (row[0], row[1], row[2]);
                if flags & (F_STATIC | F_LITERAL | F_INITONLY | F_NOTSERIALIZED) != 0 {
                    continue;
                }
                let empty = BTreeSet::new();
                let at = attrs.get(&r).unwrap_or(&empty);
                if at.contains("UnityEngine.SerializeReference") {
                    return Err(Why::Unsupported(format!(
                        "{}.{}: [SerializeReference]",
                        t.full,
                        a.string(name)?
                    )));
                }
                if flags & F_ACCESS != F_PUBLIC && !at.contains("UnityEngine.SerializeField") {
                    continue;
                }
                let b = a.blob(sig)?;
                if b.first() != Some(&0x06) {
                    continue;
                }
                let (typ, _) = self.sig((&t.asm_name, &a), &b, 1, targs)?;
                out.push((a.string(name)?, typ));
            }
        }
        Ok(out)
    }

    /// The fields of a type without the serialization rules (monotree._raw_fields): (name, type).
    fn raw_fields(&mut self, td: &Rc<TypeDef>) -> R<Vec<(String, Sig)>> {
        let a = td.asm.clone();
        let rows = a.rows(cm::FIELD)?;
        let mut out = Vec::new();
        for r in td.fields.0..td.fields.1 {
            let row = py_row(&rows, r)?;
            let (flags, name, sig) = (row[0], row[1], row[2]);
            let b = a.blob(sig)?;
            if b.first() == Some(&0x06) && flags & F_STATIC == 0 {
                let n = a.string(name)?;
                out.push((n, self.sig((&td.asm_name, &a), &b, 1, &[])?.0));
            }
        }
        Ok(out)
    }

    /// Type tree nodes for one field, or None when Unity does not serialize it.
    fn nodes(
        &mut self,
        typ: &Sig,
        name: &str,
        level: i32,
        depth: usize,
        element: bool,
    ) -> R<Option<Vec<Node>>> {
        let small = |size: i32| if size < 4 && !element { ALIGN } else { 0 };
        match typ {
            Sig::Prim(e) => {
                let (tn, size) = prim(*e).unwrap();
                Ok(Some(vec![node(level, tn, name, size, small(size), 0)]))
            }
            Sig::Str => Ok(Some(string_nodes(level, name))),
            Sig::Array(inner) => {
                if matches!(**inner, Sig::Array(_))
                    || (matches!(**inner, Sig::Type(..) | Sig::Ext(..)) && full_of(inner) == LIST)
                {
                    return Ok(None);
                }
                Ok(self
                    .nodes(inner, "data", level + 2, depth, true)?
                    .map(|sub| vector_nodes(level, name, sub)))
            }
            Sig::Ext(full, targs) => {
                if full == LIST {
                    if let Some(t) = targs.as_ref().and_then(|t| t.first()) {
                        return self.nodes(
                            &Sig::Array(Box::new(t.clone())),
                            name,
                            level,
                            depth,
                            false,
                        );
                    }
                }
                if let Some((t, f)) = builtin(full) {
                    return Ok(Some(builtin_nodes(level, name, t, &f)));
                }
                if NATIVE.contains(&full.as_str()) {
                    return Err(Why::Unsupported(format!(
                        "{name}: {full} (a native layout)"
                    )));
                }
                if full == "UnityEngine.Object" {
                    return Ok(Some(pptr_nodes(level, name, "Object")));
                }
                Ok(None)
            }
            Sig::Unsupported => Ok(None),
            Sig::Type(td, args) => {
                let full = td.full.as_str();
                if full == LIST {
                    return match args.first() {
                        Some(a) => {
                            self.nodes(&Sig::Array(Box::new(a.clone())), name, level, depth, false)
                        }
                        None => Ok(None),
                    };
                }
                if full == "UnityEngine.Object" {
                    return Ok(Some(pptr_nodes(level, name, "Object")));
                }
                if full.starts_with("System.") {
                    return Ok(None);
                }
                if let Some((t, f)) = builtin(full) {
                    return Ok(Some(builtin_nodes(level, name, t, &f)));
                }
                if NATIVE.contains(&full) {
                    return Err(Why::Unsupported(format!(
                        "{name}: {full} (a native layout)"
                    )));
                }
                if self.is_enum(td)? {
                    let under = self
                        .raw_fields(td)?
                        .into_iter()
                        .find(|(n, _)| n == "value__")
                        .map(|(_, t)| t);
                    let Some(Sig::Prim(e)) = under else {
                        return Ok(None);
                    };
                    let (tn, size) = prim(e).unwrap();
                    return Ok(Some(vec![node(level, tn, name, size, small(size), 0)]));
                }
                if self.is_unity_object(td)? {
                    return Ok(Some(pptr_nodes(level, name, &format!("${}", td.name))));
                }
                if td.flags & (T_INTERFACE | T_ABSTRACT) != 0 || td.flags & T_SERIALIZABLE == 0 {
                    return Ok(None);
                }
                if (td.nparams > 0 || !args.is_empty()) && self.unity < (2020, 1) {
                    return Ok(None);
                }
                if depth >= MAX_DEPTH {
                    return Err(Why::Unsupported(format!(
                        "{name}: {full}: more than {MAX_DEPTH} levels of classes"
                    )));
                }
                let short = td.name.split('`').next().unwrap_or("").to_string();
                let mut out = vec![node(level, &short, name, -1, 0, 0)];
                for (fname, ft) in self.fields(td, args)? {
                    if let Some(sub) = self.nodes(&ft, &fname, level + 1, depth + 1, false)? {
                        out.extend(sub);
                    }
                }
                Ok(Some(out))
            }
        }
    }

    /// Nodes of a script class's own serialized fields at level 1.
    fn class_nodes(&mut self, assembly: &str, full: &str) -> R<Vec<Node>> {
        let asm = assembly.strip_suffix(".dll").unwrap_or(assembly);
        let Some(td) = self.find(asm, full)? else {
            return Err(Why::Unsupported(format!("{full}: not in {assembly}")));
        };
        let mut out = Vec::new();
        for (fname, ft) in self.fields(&td, &[])? {
            if let Some(sub) = self.nodes(&ft, &fname, 1, 0, false)? {
                out.extend(sub);
            }
        }
        Ok(out)
    }

    /// The MonoBehaviour base tree with the script class's fields after its own.
    pub fn tree(&mut self, base: &TypeTree, assembly: &str, full: &str) -> R<TypeTree> {
        let mut nodes: Vec<Node> = base.nodes.clone();
        nodes.extend(self.class_nodes(assembly, full)?);
        for (i, n) in nodes.iter_mut().enumerate() {
            n.index = Some(i as i32);
        }
        TypeTree::from_list(nodes).map_err(|e| Why::Fault("ValueError".into(), e.to_string()))
    }
}

/// The assembly a type forwarder sends `full` to, or None.
fn forwarded(a: &Assembly, full: &str) -> R<Option<String>> {
    for r in a.rows(EXPORTEDTYPE)?.iter() {
        let (name, ns, imp) = (r[2], r[3], r[4]);
        let ns = a.string(ns)?;
        let n = if ns.is_empty() {
            a.string(name)?
        } else {
            format!("{ns}.{}", a.string(name)?)
        };
        if n == full {
            let (tab, row) = Assembly::coded(Coded::Implementation, imp)?;
            if tab == Some(cm::ASSEMBLYREF) {
                let rows = a.rows(cm::ASSEMBLYREF)?;
                let ar = py_row(&rows, row)?;
                return Ok(Some(a.string(ar[6])?));
            }
        }
    }
    Ok(None)
}

/// (tree, the tree it was read with) of a MonoBehaviour read with a tree from its script's class
/// that reads every byte of it, else the reason (monotree.read).
pub fn read(
    sf: &SerializedFile,
    pid: i64,
    gen: &mut Generator,
    script: (&str, &str),
) -> std::result::Result<(Value, TypeTree), String> {
    let base = sf
        .typetree_of(pid)
        .map_err(|e| format!("{}: ValueError: {e}", script.1))?;
    let tree = match gen.tree(&base, script.0, script.1) {
        Ok(t) => t,
        Err(Why::Unsupported(m)) => return Err(m),
        Err(Why::Fault(kind, m)) => return Err(format!("{}: {kind}: {m}", script.1)),
    };
    match sf.read_typetree_with(pid, &tree) {
        Ok(v) => Ok((v, tree)),
        Err(e) => Err(format!(
            "{}: the tree does not read the object exactly ({e})",
            script.1
        )),
    }
}
