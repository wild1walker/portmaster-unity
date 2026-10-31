//! Objects as ordered value trees, read and written by their type trees exactly as the Python
//! does: read as UnityPy's read_typetree (UnityPyBoost, as_dict), written as UnityPy's
//! write_typetree (save_typetree). Numbers keep their exact bits, strings their bytes; a
//! "UInt8"/"char" array and "TypelessData" are bytes.
//!
//! Alignment (to 4 bytes, from the object's start), as the Python:
//! - read: a field flagged kAlignBytes aligns after itself; a string always aligns after its
//!   bytes; an array aligns when its node or its Array node is flagged; an array of numbers,
//!   bools or pairs whose element node is flagged aligns once after the elements (UnityPyBoost),
//!   any other element after each element;
//! - write: every element aligns after itself when its node is flagged (UnityPy's write_value).
use crate::bin::{Reader, Writer};
use crate::error::{Error, Result};
use crate::typetree::{Kind, TypeTree};
use std::sync::Arc;

/// A value of a type tree node.
#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Bool(bool),
    I8(i8),
    U8(u8),
    I16(i16),
    U16(u16),
    I32(i32),
    U32(u32),
    I64(i64),
    U64(u64),
    /// (Rust keeps a float's bits as read: NaN payloads included)
    F32(f32),
    F64(f64),
    /// A "string": its bytes (UTF-8 as a rule; the Python decodes with surrogateescape, which
    /// gives the same bytes back).
    Str(Vec<u8>),
    /// "TypelessData", or an array of "UInt8"/"char".
    Bytes(Vec<u8>),
    Array(Vec<Value>),
    Pair(Box<(Value, Value)>),
    Struct(Struct),
}

/// A class's fields in their type tree order.
#[derive(Clone, Debug, PartialEq, Default)]
pub struct Struct {
    pub fields: Vec<(Arc<str>, Value)>,
}

impl Struct {
    /// A field by name (the last one of that name, as the Python's dict keeps).
    pub fn get(&self, name: &str) -> Option<&Value> {
        self.fields
            .iter()
            .rev()
            .find(|(n, _)| &**n == name)
            .map(|(_, v)| v)
    }

    pub fn get_mut(&mut self, name: &str) -> Option<&mut Value> {
        self.fields
            .iter_mut()
            .rev()
            .find(|(n, _)| &**n == name)
            .map(|(_, v)| v)
    }

    /// Sets a field (the last of that name), adding it at the end when there is none.
    pub fn set(&mut self, name: &str, v: Value) {
        match self.get_mut(name) {
            Some(f) => *f = v,
            None => self.fields.push((name.into(), v)),
        }
    }

    pub fn names(&self) -> impl Iterator<Item = &str> {
        self.fields.iter().map(|(n, _)| &**n)
    }
}

impl Value {
    /// A field of a struct value.
    pub fn get(&self, name: &str) -> Option<&Value> {
        match self {
            Value::Struct(s) => s.get(name),
            _ => None,
        }
    }

    pub fn get_mut(&mut self, name: &str) -> Option<&mut Value> {
        match self {
            Value::Struct(s) => s.get_mut(name),
            _ => None,
        }
    }

    /// A nested field: v.path(&["m_StreamData", "path"]).
    pub fn path(&self, names: &[&str]) -> Option<&Value> {
        names.iter().try_fold(self, |v, n| v.get(n))
    }

    pub fn path_mut(&mut self, names: &[&str]) -> Option<&mut Value> {
        names.iter().try_fold(self, |v, n| v.get_mut(n))
    }

    /// Sets a struct's field (see Struct::set); an error for any other value.
    pub fn set(&mut self, name: &str, v: Value) -> Result<()> {
        match self {
            Value::Struct(s) => {
                s.set(name, v);
                Ok(())
            }
            _ => Err(Error::Value(format!("set {name}: not a struct"))),
        }
    }

    pub fn as_struct(&self) -> Option<&Struct> {
        match self {
            Value::Struct(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_struct_mut(&mut self) -> Option<&mut Struct> {
        match self {
            Value::Struct(s) => Some(s),
            _ => None,
        }
    }

    /// Any integer (or bool) as i128, the range every integer kind fits.
    pub fn as_int(&self) -> Option<i128> {
        Some(match *self {
            Value::Bool(b) => b as i128,
            Value::I8(v) => v as i128,
            Value::U8(v) => v as i128,
            Value::I16(v) => v as i128,
            Value::U16(v) => v as i128,
            Value::I32(v) => v as i128,
            Value::U32(v) => v as i128,
            Value::I64(v) => v as i128,
            Value::U64(v) => v as i128,
            _ => return None,
        })
    }

    pub fn as_i64(&self) -> Option<i64> {
        self.as_int().and_then(|v| i64::try_from(v).ok())
    }

    pub fn as_u64(&self) -> Option<u64> {
        self.as_int().and_then(|v| u64::try_from(v).ok())
    }

    pub fn as_f64(&self) -> Option<f64> {
        match *self {
            Value::F32(v) => Some(v as f64),
            Value::F64(v) => Some(v),
            _ => self.as_int().map(|v| v as f64),
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match *self {
            Value::Bool(b) => Some(b),
            _ => self.as_int().map(|v| v != 0),
        }
    }

    /// A string's text (None when not UTF-8).
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::Str(b) => std::str::from_utf8(b).ok(),
            _ => None,
        }
    }

    pub fn as_bytes(&self) -> Option<&[u8]> {
        match self {
            Value::Bytes(b) | Value::Str(b) => Some(b),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<&Vec<Value>> {
        match self {
            Value::Array(a) => Some(a),
            _ => None,
        }
    }

    pub fn as_array_mut(&mut self) -> Option<&mut Vec<Value>> {
        match self {
            Value::Array(a) => Some(a),
            _ => None,
        }
    }

    /// A string value from text.
    pub fn str(s: &str) -> Value {
        Value::Str(s.as_bytes().to_vec())
    }
}

/// A serialized file's reference types, for "ReferencedObject" fields (format 20 and later):
/// (class name, namespace, assembly name, its type tree).
pub type RefType = (Vec<u8>, Vec<u8>, Vec<u8>, Option<Arc<TypeTree>>);
pub type RefTypes = [RefType];

struct Ctx<'a> {
    refs: Option<&'a RefTypes>,
    has_registry: bool,
}

/// An object's bytes read with a type tree: (value, bytes read). The bytes need not all be read
/// (UnityPy's check_read is `read_object`'s).
pub fn read(
    tree: &TypeTree,
    data: &[u8],
    big: bool,
    refs: Option<&RefTypes>,
) -> Result<(Value, usize)> {
    let mut r = Reader::new(data, big);
    let mut ctx = Ctx {
        refs,
        has_registry: false,
    };
    let v = read_value(tree, 0, &mut r, &mut ctx)?;
    Ok((v, r.pos))
}

/// An object's bytes read with a type tree, every byte of them (else Error::ReadSize, the
/// Python's "Expected to read N bytes, but only read M bytes").
pub fn read_object(
    tree: &TypeTree,
    data: &[u8],
    big: bool,
    refs: Option<&RefTypes>,
) -> Result<Value> {
    let (v, n) = read(tree, data, big, refs)?;
    if n != data.len() {
        return Err(Error::ReadSize {
            expected: data.len(),
            read: n,
        });
    }
    Ok(v)
}

fn eof(what: &str) -> Error {
    Error::Format(format!("{what} out of bounds"))
}

fn read_num(k: Kind, r: &mut Reader) -> Result<Value> {
    if r.remaining() < k.size() {
        return Err(eof("read_num"));
    }
    Ok(match k {
        Kind::U8 => Value::U8(r.u8()?),
        Kind::I8 => Value::I8(r.u8()? as i8),
        Kind::Bool => Value::Bool(r.u8()? != 0),
        Kind::U16 => Value::U16(r.u16()?),
        Kind::I16 => Value::I16(r.i16()?),
        Kind::U32 => Value::U32(r.u32()?),
        Kind::I32 => Value::I32(r.i32()?),
        Kind::U64 => Value::U64(r.u64()?),
        Kind::I64 => Value::I64(r.i64()?),
        Kind::F32 => Value::F32(f32::from_bits(r.u32()?)),
        Kind::F64 => Value::F64(f64::from_bits(r.u64()?)),
        _ => unreachable!(),
    })
}

fn read_len(r: &mut Reader) -> Result<usize> {
    if r.remaining() < 4 {
        return Err(eof("read_length"));
    }
    let n = r.i32()?;
    if n < 0 {
        return Err(Error::Format("Negative length read from TypeTree".into()));
    }
    Ok(n as usize)
}

fn read_value(t: &TypeTree, i: usize, r: &mut Reader, ctx: &mut Ctx) -> Result<Value> {
    let node = &t.nodes[i];
    let mut align = node.aligned();
    let v = match node.kind {
        k if k.is_number() => read_num(k, r)?,
        Kind::Str => {
            let n = read_len(r)?;
            if r.remaining() < n {
                return Err(eof("read_str"));
            }
            let s = r.take(n)?.to_vec();
            r.align(4);
            Value::Str(s)
        }
        Kind::Bytes => {
            let n = read_len(r)?;
            if r.remaining() < n {
                return Err(eof("read_bytes"));
            }
            Value::Bytes(r.take(n)?.to_vec())
        }
        Kind::Pair => read_pair(t, i, r, ctx)?,
        Kind::RefObject => {
            let mut s = Struct::default();
            for &c in &node.children {
                let child = &t.nodes[c];
                let v = if child.kind == Kind::RefObjectData {
                    match ref_type_tree(&s, ctx.refs)? {
                        None => continue,
                        Some(rt) => read_value(&rt, 0, r, ctx)?,
                    }
                } else {
                    read_value(t, c, r, ctx)?
                };
                s.fields.push((child.name.clone(), v));
            }
            Value::Struct(s)
        }
        _ => {
            let first = node.children.first().map(|&c| &t.nodes[c]);
            match first {
                Some(arr) if arr.kind == Kind::Array => {
                    if arr.children.len() != 2 {
                        return Err(Error::Format("Array node must have 2 children".into()));
                    }
                    if arr.aligned() {
                        align = true;
                    }
                    let n = read_len(r)?;
                    let e = arr.children[1];
                    read_array(t, e, n, r, ctx)?
                }
                _ => {
                    let mut s = Struct {
                        fields: Vec::with_capacity(node.children.len()),
                    };
                    let mut changed = false;
                    for &c in &node.children {
                        let child = &t.nodes[c];
                        if child.kind == Kind::RefRegistry {
                            if ctx.has_registry {
                                continue;
                            }
                            changed = true;
                            ctx.has_registry = true;
                        }
                        let v = read_value(t, c, r, ctx)?;
                        s.fields.push((child.name.clone(), v));
                    }
                    if changed {
                        ctx.has_registry = false;
                    }
                    Value::Struct(s)
                }
            }
        }
    };
    if align {
        r.align(4);
    }
    Ok(v)
}

fn read_pair(t: &TypeTree, i: usize, r: &mut Reader, ctx: &mut Ctx) -> Result<Value> {
    let node = &t.nodes[i];
    if node.children.len() != 2 {
        return Err(Error::Format("Pair node must have 2 children".into()));
    }
    let a = read_value(t, node.children[0], r, ctx)?;
    let b = read_value(t, node.children[1], r, ctx)?;
    Ok(Value::Pair(Box::new((a, b))))
}

fn read_array(t: &TypeTree, e: usize, n: usize, r: &mut Reader, ctx: &mut Ctx) -> Result<Value> {
    let node = &t.nodes[e];
    let k = node.kind;
    if k.is_number() || k == Kind::Pair {
        // UnityPyBoost's read_typetree_value_array: the elements, then one align for the
        // element node's flag
        let v = if k == Kind::U8 {
            if r.remaining() < n {
                return Err(eof("read_u8_array"));
            }
            Value::Bytes(r.take(n)?.to_vec())
        } else if k == Kind::Pair {
            let mut a = Vec::with_capacity(n.min(1 << 20));
            for _ in 0..n {
                a.push(read_pair(t, e, r, ctx)?);
            }
            Value::Array(a)
        } else {
            if r.remaining() < n.saturating_mul(k.size()) {
                return Err(eof("read_num_array"));
            }
            let mut a = Vec::with_capacity(n);
            for _ in 0..n {
                a.push(read_num(k, r)?);
            }
            Value::Array(a)
        };
        if node.aligned() {
            r.align(4);
        }
        return Ok(v);
    }
    let mut a = Vec::with_capacity(n.min(1 << 16));
    for _ in 0..n {
        a.push(read_value(t, e, r, ctx)?);
    }
    Ok(Value::Array(a))
}

/// The type tree of a ReferencedObject's data: by its "type" field's class, ns and asm in the
/// file's reference types; None for an empty class name (a null reference: no data).
fn ref_type_tree(s: &Struct, refs: Option<&RefTypes>) -> Result<Option<Arc<TypeTree>>> {
    let refs = refs.ok_or_else(|| Error::Format("No SerializedFile.ref_types".into()))?;
    let typ = s
        .get("type")
        .ok_or_else(|| Error::Format("Failed to get 'type'".into()))?;
    let field = |n: &str| {
        typ.get(n)
            .and_then(|v| v.as_bytes())
            .ok_or_else(|| Error::Format("Failed to get 'class', 'ns' or 'asm'".into()))
    };
    let (cls, ns, asm) = (field("class")?, field("ns")?, field("asm")?);
    if cls.is_empty() {
        return Ok(None);
    }
    for (c, n, a, tree) in refs {
        if c == cls && n == ns && a == asm {
            return tree
                .clone()
                .map(Some)
                .ok_or_else(|| Error::Format("reference type without a type tree".into()));
        }
    }
    Err(Error::Format("Failed to get ref type node".into()))
}

/// A value written with a type tree (UnityPy's write_typetree): the object's new bytes.
pub fn write(tree: &TypeTree, v: &Value, big: bool, refs: Option<&RefTypes>) -> Result<Vec<u8>> {
    let mut w = Writer::new(big);
    let mut ctx = Ctx {
        refs,
        has_registry: false,
    };
    write_value(tree, 0, v, &mut w, &mut ctx)?;
    Ok(w.buf)
}

fn int_of(v: &Value, name: &str) -> Result<i128> {
    v.as_int()
        .ok_or_else(|| Error::Value(format!("{name}: {v:?} is not an integer")))
}

fn write_num(k: Kind, v: &Value, w: &mut Writer, name: &str) -> Result<()> {
    macro_rules! int {
        ($t:ty, $f:ident) => {{
            let x = int_of(v, name)?;
            let x =
                <$t>::try_from(x).map_err(|_| Error::Value(format!("{name}: {x} out of range")))?;
            w.$f(x);
        }};
    }
    match k {
        Kind::Bool => w
            .u8(v
                .as_bool()
                .ok_or_else(|| Error::Value(format!("{name}: not a bool")))? as u8),
        Kind::U8 => int!(u8, u8),
        Kind::I8 => {
            let x = int_of(v, name)?;
            w.u8(
                i8::try_from(x).map_err(|_| Error::Value(format!("{name}: {x} out of range")))?
                    as u8,
            )
        }
        Kind::U16 => int!(u16, u16),
        Kind::I16 => int!(i16, i16),
        Kind::U32 => int!(u32, u32),
        Kind::I32 => int!(i32, i32),
        Kind::U64 => int!(u64, u64),
        Kind::I64 => int!(i64, i64),
        Kind::F32 => {
            let bits = match *v {
                Value::F32(f) => f.to_bits(),
                _ => (v
                    .as_f64()
                    .ok_or_else(|| Error::Value(format!("{name}: not a number")))?
                    as f32)
                    .to_bits(),
            };
            w.u32(bits)
        }
        Kind::F64 => {
            let bits = match *v {
                Value::F64(f) => f.to_bits(),
                _ => v
                    .as_f64()
                    .ok_or_else(|| Error::Value(format!("{name}: not a number")))?
                    .to_bits(),
            };
            w.u64(bits)
        }
        _ => unreachable!(),
    }
    Ok(())
}

fn write_value(t: &TypeTree, i: usize, v: &Value, w: &mut Writer, ctx: &mut Ctx) -> Result<()> {
    let node = &t.nodes[i];
    let mut align = node.aligned();
    let k = node.kind;
    // UnityPy's FUNCTION_WRITE_MAP by type name: the numbers, "string", "TypelessData"
    if k.is_number() {
        write_num(k, v, w, &node.name)?;
    } else if k == Kind::Str {
        let b = match v {
            Value::Str(b) | Value::Bytes(b) => b,
            _ => return Err(Error::Value(format!("{}: not a string", node.name))),
        };
        w.i32(b.len() as i32);
        w.bytes(b);
        w.align(4);
    } else if k == Kind::Bytes {
        let b = v
            .as_bytes()
            .ok_or_else(|| Error::Value(format!("{}: not bytes", node.name)))?;
        w.i32(b.len() as i32);
        w.bytes(b);
    } else if k == Kind::Pair {
        let Value::Pair(p) = v else {
            return Err(Error::Value(format!("{}: not a pair", node.name)));
        };
        if node.children.len() < 2 {
            return Err(Error::Value(format!(
                "{}: pair node without 2 children",
                node.name
            )));
        }
        write_value(t, node.children[0], &p.0, w, ctx)?;
        write_value(t, node.children[1], &p.1, w, ctx)?;
    } else if k == Kind::RefObject {
        let s = v
            .as_struct()
            .ok_or_else(|| Error::Value(format!("{}: not a struct", node.name)))?;
        for &c in &node.children {
            let child = &t.nodes[c];
            let fv = s
                .get(&child.name)
                .ok_or_else(|| Error::Value(format!("{}: no field {}", node.name, child.name)))?;
            if child.kind == Kind::RefObjectData {
                let rt = ref_type_tree(s, ctx.refs)?.ok_or_else(|| {
                    Error::Value(format!(
                        "{}: a null reference's data cannot be written",
                        node.name
                    ))
                })?;
                write_value(&rt, 0, fv, w, ctx)?;
            } else {
                write_value(t, c, fv, w, ctx)?;
            }
        }
    } else if node
        .children
        .first()
        .map(|&c| &*t.nodes[c].type_name == "Array")
        .unwrap_or(false)
    {
        let arr = &t.nodes[node.children[0]];
        if arr.aligned() {
            align = true;
        }
        let e = *arr
            .children
            .get(1)
            .ok_or_else(|| Error::Value(format!("{}: Array node without 2 children", node.name)))?;
        let en = &t.nodes[e];
        match v {
            Value::Bytes(b)
                if matches!(en.kind, Kind::U8 | Kind::I8 | Kind::Bool)
                    || en.kind.is_number() && en.kind.size() == 1 =>
            {
                w.i32(b.len() as i32);
                if en.kind == Kind::U8 && !en.aligned() && en.children.is_empty() {
                    w.bytes(b);
                } else {
                    for &x in b.iter() {
                        write_value(t, e, &Value::U8(x), w, ctx)?;
                    }
                }
            }
            Value::Array(a) => {
                w.i32(a.len() as i32);
                for x in a {
                    write_value(t, e, x, w, ctx)?;
                }
            }
            _ => return Err(Error::Value(format!("{}: not an array", node.name))),
        }
    } else {
        let s = v
            .as_struct()
            .ok_or_else(|| Error::Value(format!("{}: not a struct", node.name)))?;
        let mut changed = false;
        for &c in &node.children {
            let child = &t.nodes[c];
            if child.kind == Kind::RefRegistry {
                if ctx.has_registry {
                    continue;
                }
                changed = true;
                ctx.has_registry = true;
            }
            let fv = s
                .get(&child.name)
                .ok_or_else(|| Error::Value(format!("{}: no field {}", node.name, child.name)))?;
            write_value(t, c, fv, w, ctx)?;
        }
        if changed {
            ctx.has_registry = false;
        }
    }
    if align {
        w.align(4);
    }
    Ok(())
}
