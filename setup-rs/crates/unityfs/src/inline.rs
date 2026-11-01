//! Streamed texture pixels moved into the texture objects (android_data.inline_texture_streams,
//! _inline_plan, _inline_streamed): Unity 2017.4's Android player never finishes an asynchronous
//! texture upload, so a texture that streams its pixels from a .resS must carry them inline.
use crate::error::{Error, Result};
use crate::serialized::SerializedFile;
use crate::value::Value;
use std::collections::HashSet;

/// Texture classes whose pixels can stream.
pub const TEXTURES: &[&str] = &["Texture2D", "Cubemap", "Texture2DArray", "Texture3D"];

fn last_part(p: &[u8]) -> String {
    let s = String::from_utf8_lossy(p);
    s.rsplit('/').next().unwrap_or("").to_string()
}

fn truthy(v: Option<&Value>) -> bool {
    match v {
        None => false,
        Some(Value::Str(b)) | Some(Value::Bytes(b)) => !b.is_empty(),
        Some(Value::Struct(s)) => !s.fields.is_empty(),
        Some(Value::Array(a)) => !a.is_empty(),
        Some(v) => v
            .as_int()
            .map(|x| x != 0)
            .or_else(|| v.as_f64().map(|x| x != 0.0))
            .unwrap_or(true),
    }
}

/// A texture's or mesh's stream (its m_StreamData): the bytes it reads from a .resS.
#[derive(Clone, Debug, PartialEq)]
pub struct StreamRef {
    pub path_id: i64,
    pub class: String,
    /// The texture has an "image data" field (its pixels can move inline).
    pub image_data: bool,
    pub path: Vec<u8>,
    pub offset: u64,
    pub size: u64,
}

impl StreamRef {
    /// The stream file's name (the path's last part).
    pub fn file(&self) -> String {
        last_part(&self.path)
    }
}

/// The streams of the objects of these classes, as _inline_plan finds them: a Mesh's when its
/// path and size are set, a texture's when its m_StreamData and path are.
pub fn stream_refs(sf: &SerializedFile, classes: &[&str]) -> Result<Vec<StreamRef>> {
    let mut out = Vec::new();
    for o in &sf.objects {
        let name = sf.class_name(o);
        if !classes.contains(&name.as_str()) {
            continue;
        }
        let t = sf.current_typetree(o.path_id)?;
        let Some(sd) = t.get("m_StreamData") else {
            continue;
        };
        let ok = if name == "Mesh" {
            truthy(sd.get("path")) && truthy(sd.get("size"))
        } else {
            truthy(Some(sd)) && truthy(sd.get("path"))
        };
        if !ok {
            continue;
        }
        out.push(StreamRef {
            path_id: o.path_id,
            image_data: t.get("image data").is_some(),
            path: sd
                .get("path")
                .and_then(|v| v.as_bytes())
                .unwrap_or(b"")
                .to_vec(),
            offset: sd.get("offset").and_then(|v| v.as_u64()).unwrap_or(0),
            size: sd.get("size").and_then(|v| v.as_u64()).unwrap_or(0),
            class: name,
        });
    }
    Ok(out)
}

/// The classes _inline_plan reads: meshes and the texture classes.
pub const PLANNED: &[&str] = &[
    "Mesh",
    "Texture2D",
    "Cubemap",
    "Texture2DArray",
    "Texture3D",
];

/// _inline_plan of the streams found: (path ids of the textures to inline, .resS names still
/// read: by a mesh's stream, or a texture with no "image data" field).
pub fn plan_of(refs: &[StreamRef]) -> (HashSet<i64>, HashSet<String>) {
    let mut plan = HashSet::new();
    let mut still = HashSet::new();
    for r in refs {
        if r.class == "Mesh" || (TEXTURES.contains(&r.class.as_str()) && !r.image_data) {
            still.insert(r.file());
        } else if TEXTURES.contains(&r.class.as_str()) {
            plan.insert(r.path_id);
        }
    }
    (plan, still)
}

/// _inline_plan: (path ids of the textures to inline, .resS names still read: by a mesh's
/// stream, or a texture with no "image data" field).
pub fn plan(sf: &SerializedFile) -> Result<(HashSet<i64>, HashSet<String>)> {
    Ok(plan_of(&stream_refs(sf, PLANNED)?))
}

fn zero_like(v: Option<&Value>) -> Value {
    match v {
        Some(Value::U64(_)) => Value::U64(0),
        Some(Value::I64(_)) => Value::I64(0),
        Some(Value::I32(_)) => Value::I32(0),
        _ => Value::U32(0),
    }
}

/// A texture's bytes with its streamed pixels inline (the prepare step of _inline_streamed):
/// "image data" read from its stream, m_StreamData emptied.
pub fn inlined(
    sf: &SerializedFile,
    path_id: i64,
    read_stream: impl Fn(&str, u64, usize) -> Result<Vec<u8>>,
) -> Result<Vec<u8>> {
    let mut t = sf.current_typetree(path_id)?;
    let sd = t
        .get("m_StreamData")
        .cloned()
        .ok_or_else(|| Error::Value("no m_StreamData".into()))?;
    let path = sd
        .get("path")
        .and_then(|v| v.as_bytes())
        .unwrap_or(b"")
        .to_vec();
    let off = sd.get("offset").and_then(|v| v.as_u64()).unwrap_or(0);
    let size = sd.get("size").and_then(|v| v.as_u64()).unwrap_or(0) as usize;
    let data = read_stream(&String::from_utf8_lossy(&path), off, size)?;
    if data.len() != size {
        let name = t
            .get("m_Name")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();
        return Err(Error::Format(format!(
            "{name}: stream {{'offset': {off}, 'size': {size}, 'path': {:?}}} gave {} bytes",
            String::from_utf8_lossy(&path),
            data.len()
        )));
    }
    t.set("image data", Value::Bytes(data))?;
    let mut nsd = sd.clone();
    nsd.set("offset", zero_like(sd.get("offset")))?;
    nsd.set("size", zero_like(sd.get("size")))?;
    nsd.set("path", Value::Str(Vec::new()))?;
    t.set("m_StreamData", nsd)?;
    let tree = sf.typetree_of(path_id)?;
    sf.write_value(&tree, &t)
}

/// An integer of the same kind as `like` (a StreamingInfo's offset: UInt32 or UInt64).
fn int_like(like: Option<&Value>, x: u64) -> Result<Value> {
    let bad = || Error::Value(format!("stream offset {x} does not fit its field"));
    Ok(match like {
        Some(Value::U64(_)) => Value::U64(x),
        Some(Value::I64(_)) => Value::I64(i64::try_from(x).map_err(|_| bad())?),
        Some(Value::I32(_)) => Value::I32(i32::try_from(x).map_err(|_| bad())?),
        _ => Value::U32(u32::try_from(x).map_err(|_| bad())?),
    })
}

/// An object's bytes with its stream moved: m_StreamData's path and offset set (the size and
/// every other field as they are). The object must write back to its own bytes unchanged first
/// (so nothing but the stream can change).
pub fn restreamed(sf: &SerializedFile, path_id: i64, path: &[u8], offset: u64) -> Result<Vec<u8>> {
    let mut t = sf.current_typetree(path_id)?;
    let tree = sf.typetree_of(path_id)?;
    if sf.write_value(&tree, &t)? != sf.bytes(path_id)? {
        return Err(Error::Format(format!(
            "{}: object {path_id} does not write back to its own bytes",
            sf.name
        )));
    }
    let mut sd = t
        .get("m_StreamData")
        .cloned()
        .ok_or_else(|| Error::Value("no m_StreamData".into()))?;
    let off = int_like(sd.get("offset"), offset)?;
    sd.set("offset", off)?;
    sd.set("path", Value::Str(path.to_vec()))?;
    t.set("m_StreamData", sd)?;
    sf.write_value(&tree, &t)
}

/// inline_texture_streams, in memory: every planned texture's new bytes set on the file.
/// Returns (textures inlined, .resS names still read).
pub fn inline_texture_streams(
    sf: &mut SerializedFile,
    read_stream: impl Fn(&str, u64, usize) -> Result<Vec<u8>>,
) -> Result<(usize, HashSet<String>)> {
    let (plan, still) = plan(sf)?;
    let ids: Vec<i64> = sf
        .objects
        .iter()
        .map(|o| o.path_id)
        .filter(|p| plan.contains(p))
        .collect();
    for &pid in &ids {
        let b = inlined(sf, pid, &read_stream)?;
        sf.set_raw(pid, b)?;
    }
    Ok((ids.len(), still))
}
