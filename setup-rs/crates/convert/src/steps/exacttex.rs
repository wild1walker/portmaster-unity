//! The data walk's helpers for `exact_shaders` (exactindex.rs): files, PPtrs, shader names and the
//! test of a material (its shader's name matches, it has every keyword). A shader that reads a texel's
//! channel as data (Blasphemous's `Maikel/MaikelSpriteMasterShader` with `PALETTE_ON`: the blue channel is
//! a palette index) samples textures that ASTC or a scale would change; exactindex.rs finds them.
use crate::job::Job;
use std::collections::{HashMap, HashSet};
use unityfs::{DataFile, Value};

/// A data file's name and a path id.
pub type Key = (String, i64);

/// The data files the walk reads (serialized files: not bundles), in the textures step's order.
pub(crate) fn data_files(job: &Job) -> Vec<String> {
    job.list("", |n| {
        n.ends_with(".assets") || n == "globalgamemanagers" || super::shaders::is_level(n)
    })
}

pub(crate) fn pptr_of(v: &Value) -> Option<(i32, i64)> {
    let f = i32::try_from(v.get("m_FileID")?.as_int()?).ok()?;
    let p = i64::try_from(v.get("m_PathID")?.as_int()?).ok()?;
    Some((f, p))
}

/// The file a PPtr's file id names, seen from `from` (0: `from` itself), by its base name.
pub(crate) fn file_name(f: &DataFile, from: &str, fid: i32) -> Option<String> {
    if fid == 0 {
        return Some(from.to_string());
    }
    let e = f.sf.externals.get(usize::try_from(fid - 1).ok()?)?;
    let p = String::from_utf8_lossy(&e.path).into_owned();
    Some(p.rsplit('/').next().unwrap_or(&p).to_string())
}

/// A PPtr as a key (None for a null reference or an unknown file id).
pub(crate) fn key(f: &DataFile, from: &str, v: &Value) -> Option<Key> {
    let (fid, pid) = pptr_of(v)?;
    if pid == 0 {
        return None;
    }
    Some((file_name(f, from, fid)?, pid))
}

pub(crate) fn class_of(f: &DataFile, pid: i64) -> Option<i32> {
    f.sf.object(pid).map(|o| o.class_id)
}

/// A material's name if its shader matches and it has every keyword.
pub(crate) fn material_matches(
    f: &DataFile,
    from: &str,
    t: &Value,
    shaders: &HashMap<Key, String>,
    rx: &regex::Regex,
    keywords: &[String],
) -> bool {
    let Some(s) = t.get("m_Shader").and_then(|v| key(f, from, v)) else {
        return false;
    };
    if !shaders.get(&s).is_some_and(|n| rx.is_match(n)) {
        return false;
    }
    let kw = t
        .get("m_ShaderKeywords")
        .and_then(Value::as_bytes)
        .map(|b| String::from_utf8_lossy(b).into_owned())
        .unwrap_or_default();
    let have: HashSet<&str> = kw.split_whitespace().collect();
    keywords.iter().all(|k| have.contains(k.as_str()))
}

pub(crate) fn shader_name(t: &Value) -> String {
    let n = t
        .path(&["m_ParsedForm", "m_Name"])
        .or_else(|| t.get("m_Name"))
        .and_then(Value::as_bytes)
        .unwrap_or(&[]);
    String::from_utf8_lossy(n).into_owned()
}
