//! A Shader object's tree (unityfs::Value, as UnityPy's read_typetree gives it) and its program
//! blob: the fields blob.ShaderBlobs reads and store() writes back, and convert.platform_entries
//! (a platform's entries, its parameter and program indices taken from the subprograms that name
//! them). For the steps that edit a Shader's tree themselves (grab_fetch, android-data's video
//! shader).
use crate::blob::{self, BlobError, BlobFields, Lengths, ShaderBlobs, Slot};
use crate::convert::STAGES;
use std::collections::BTreeSet;
use unityfs::Value;

fn ints(v: &Value, what: &str) -> Result<Vec<i64>, String> {
    let bad = || format!("{what}: not a list of integers");
    v.as_array()
        .ok_or_else(bad)?
        .iter()
        .map(|x| x.as_i64().ok_or_else(bad))
        .collect()
}

fn lengths(v: &Value, what: &str) -> Result<Lengths, String> {
    let a = v.as_array().ok_or_else(|| format!("{what}: not a list"))?;
    if a.first().is_some_and(|x| x.as_array().is_some()) {
        Ok(Lengths::Nested(
            a.iter().map(|x| ints(x, what)).collect::<Result<_, _>>()?,
        ))
    } else {
        Ok(Lengths::Flat(ints(v, what)?))
    }
}

fn field<'a>(t: &'a Value, name: &str) -> Result<&'a Value, String> {
    t.get(name).ok_or_else(|| format!("KeyError: '{name}'"))
}

/// The blob fields of a Shader's tree.
pub fn blob_fields(t: &Value) -> Result<BlobFields, String> {
    Ok(BlobFields {
        platforms: ints(field(t, "platforms")?, "platforms")?,
        offsets: lengths(field(t, "offsets")?, "offsets")?,
        compressed_lengths: lengths(field(t, "compressedLengths")?, "compressedLengths")?,
        decompressed_lengths: lengths(field(t, "decompressedLengths")?, "decompressedLengths")?,
        compressed_blob: field(t, "compressedBlob")?
            .as_bytes()
            .ok_or("compressedBlob: not bytes")?
            .to_vec(),
        stage_counts: t
            .get("stageCounts")
            .map(|x| ints(x, "stageCounts"))
            .transpose()?,
    })
}

fn int_list(v: &[i64]) -> Value {
    Value::Array(v.iter().map(|&x| Value::I64(x)).collect())
}

fn lengths_value(l: &Lengths) -> Value {
    match l {
        Lengths::Flat(v) => int_list(v),
        Lengths::Nested(v) => Value::Array(v.iter().map(|x| int_list(x)).collect()),
    }
}

/// The blob fields written into a Shader's tree (ShaderBlobs.store).
pub fn set_blob(t: &mut Value, b: &BlobFields) -> Result<(), String> {
    let e = |e: unityfs::Error| e.to_string();
    t.set("platforms", int_list(&b.platforms)).map_err(e)?;
    t.set("offsets", lengths_value(&b.offsets)).map_err(e)?;
    t.set("compressedLengths", lengths_value(&b.compressed_lengths))
        .map_err(e)?;
    t.set(
        "decompressedLengths",
        lengths_value(&b.decompressed_lengths),
    )
    .map_err(e)?;
    t.set("compressedBlob", Value::Bytes(b.compressed_blob.clone()))
        .map_err(e)?;
    if let Some(sc) = &b.stage_counts {
        t.set("stageCounts", int_list(sc)).map_err(e)?;
    }
    Ok(())
}

/// The blobs of a Shader's tree (ShaderBlobs(tree)).
pub fn blobs(t: &Value) -> Result<ShaderBlobs, String> {
    ShaderBlobs::read(&blob_fields(t)?).map_err(|e: BlobError| e.to_string())
}

/// convert.PLATFORM_TYPES: the GPU program types whose entries are in a platform's blob.
pub fn platform_types(platform: i64) -> Vec<i64> {
    match platform {
        blob::PLATFORM_GLCORE => vec![blob::GPU_GLCORE32, blob::GPU_GLCORE41, blob::GPU_GLCORE43],
        blob::PLATFORM_GLES3PLUS => vec![blob::GPU_GLES31AEP, blob::GPU_GLES31, blob::GPU_GLES3],
        blob::PLATFORM_GLES20 => vec![blob::GPU_GLES],
        blob::PLATFORM_D3D11 => (15..23).collect(),
        _ => Vec::new(),
    }
}

fn truthy(v: Option<&Value>) -> bool {
    match v {
        None => false,
        Some(Value::Array(a)) => !a.is_empty(),
        Some(Value::Struct(s)) => !s.fields.is_empty(),
        Some(_) => true,
    }
}

fn int_of(v: &Value, what: &str) -> Result<i64, String> {
    v.as_i64().ok_or_else(|| format!("{what}: not an integer"))
}

/// convert.param_indices and program_indices of a platform: the blob indices of its parameter
/// entries (2021.2+) and of its program entries.
pub fn indices(t: &Value, platform: i64) -> Result<(BTreeSet<i64>, BTreeSet<i64>), String> {
    let types = platform_types(platform);
    let (mut params, mut progs) = (BTreeSet::new(), BTreeSet::new());
    let list = |v: &Value| -> Result<Vec<Value>, String> {
        v.as_array()
            .cloned()
            .ok_or_else(|| "subprograms: not a list".to_string())
    };
    for ss in list(field(field(t, "m_ParsedForm")?, "m_SubShaders")?)? {
        for p in list(field(&ss, "m_Passes")?)? {
            for st in STAGES {
                let Some(prog) = p.get(st).filter(|x| truthy(Some(x))) else {
                    continue;
                };
                let mut lists: Vec<(Vec<Value>, Option<Vec<Value>>)> = Vec::new();
                if let Some(l) = prog.get("m_SubPrograms") {
                    lists.push((list(l)?, None));
                }
                if truthy(prog.get("m_PlayerSubPrograms")) {
                    let pbi = list(field(prog, "m_ParameterBlobIndices")?)?;
                    for (tier, l) in list(field(prog, "m_PlayerSubPrograms")?)?
                        .iter()
                        .enumerate()
                    {
                        let pi = pbi.get(tier).ok_or("IndexError: list index out of range")?;
                        lists.push((list(l)?, Some(list(pi)?)));
                    }
                }
                for (lst, pi) in lists {
                    for (i, sp) in lst.iter().enumerate() {
                        let ty = int_of(field(sp, "m_GpuProgramType")?, "m_GpuProgramType")?;
                        if !types.contains(&ty) {
                            continue;
                        }
                        progs.insert(int_of(field(sp, "m_BlobIndex")?, "m_BlobIndex")?);
                        if let Some(pi) = &pi {
                            let v = pi.get(i).ok_or("IndexError: list index out of range")?;
                            params.insert(int_of(v, "m_ParameterBlobIndices")?);
                        }
                    }
                }
            }
        }
    }
    Ok((params, progs))
}

/// convert.platform_entries: a platform's blob entries (programs, 2021.2+ parameters, entries
/// no subprogram names kept as bytes).
pub fn platform_entries(
    t: &Value,
    platform: i64,
    blobs: &ShaderBlobs,
) -> Result<Vec<Option<Slot>>, String> {
    let (params, progs) = indices(t, platform)?;
    blobs
        .entries(platform, &params, Some(&progs))
        .map_err(|e| e.to_string())
}
