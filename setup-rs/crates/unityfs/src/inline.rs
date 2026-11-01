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

/// _inline_plan: (path ids of the textures to inline, .resS names still read: by a mesh's
/// stream, or a texture with no "image data" field).
pub fn plan(sf: &SerializedFile) -> Result<(HashSet<i64>, HashSet<String>)> {
    let mut plan = HashSet::new();
    let mut still = HashSet::new();
    for o in &sf.objects {
        let name = sf.class_name(o);
        if name == "Mesh" {
            let t = sf.current_typetree(o.path_id)?;
            if let Some(sd) = t.get("m_StreamData") {
                if truthy(sd.get("path")) && truthy(sd.get("size")) {
                    still.insert(last_part(
                        sd.get("path").and_then(|v| v.as_bytes()).unwrap_or(b""),
                    ));
                }
            }
            continue;
        }
        if !TEXTURES.contains(&name.as_str()) {
            continue;
        }
        let t = sf.current_typetree(o.path_id)?;
        let Some(sd) = t.get("m_StreamData") else {
            continue;
        };
        if !truthy(Some(sd)) || !truthy(sd.get("path")) {
            continue;
        }
        if t.get("image data").is_none() {
            still.insert(last_part(
                sd.get("path").and_then(|v| v.as_bytes()).unwrap_or(b""),
            ));
            continue;
        }
        plan.insert(o.path_id);
    }
    Ok((plan, still))
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
