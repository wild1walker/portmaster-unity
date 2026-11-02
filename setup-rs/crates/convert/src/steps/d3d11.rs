//! Shaders from Direct3D 11 (Windows builds): convert.add_gles3_d3d11 on the Shader object (its
//! docstring has the layouts by Unity version): every pass's D3D11 vertex subprograms paired with
//! the pixel subprogram of the same variant, each pair translated by shaderx::dxbc (pmhlslcc) and
//! glsl::program_to_es, stored as Unity stores GL programs of the data's version; Unity's binding
//! info with the program's shared parameters (with_common) and checked against the parsed form.
use super::fusedpost::{truthy, unity_of};
use crate::error::{Error, Result};
use shaderx::bindings::{
    self, Binding, Bindings, ConstantBuffer, Param, KIND_CBUFFER, KIND_SAMPLER, KIND_TEXTURE,
};
use shaderx::blob::{self, Entry, ShaderBlobs, Slot};
use shaderx::convert::{Report, Status, STAGES};
use shaderx::dxbc::{self, Translator};
use shaderx::glsl::{self, Es, Optimize};
use shaderx::tree;
use std::collections::{BTreeMap, BTreeSet, HashMap};
use unityfs::Value;

pub(super) const VR_KEYWORDS: [&str; 6] = [
    "STEREO_INSTANCING_ON",
    "STEREO_MULTIVIEW_ON",
    "UNITY_SINGLE_PASS_STEREO",
    "STEREO_CUBEMAP_RENDER_ON",
    "STEREO_INSTANCING_ENABLED",
    "STEREO_DOUBLEWIDE_TARGET",
];
pub(super) const INSTANCING_KEYWORDS: [&str; 3] = [
    "INSTANCING_ON",
    "PROCEDURAL_INSTANCING_ON",
    "DOTS_INSTANCING_ON",
];
pub(super) const SKINNING_KEYWORDS: [&str; 1] = ["SKINNED_SPRITE"];
const D3D_ONLY_LISTS: [&str; 8] = [
    "m_VectorParams",
    "m_MatrixParams",
    "m_TextureParams",
    "m_BufferParams",
    "m_ConstantBuffers",
    "m_ConstantBufferBindings",
    "m_UAVParams",
    "m_Samplers",
];
const OTHER_STAGES: [&str; 3] = ["progGeometry", "progHull", "progDomain"];
const KEY_FIELDS: [(&str, char); 3] = [
    ("m_KeywordIndices", 'k'),
    ("m_GlobalKeywordIndices", 'g'),
    ("m_LocalKeywordIndices", 'l'),
];
const GLES_TYPES: [i64; 4] = [
    blob::GPU_GLES31AEP,
    blob::GPU_GLES31,
    blob::GPU_GLES3,
    blob::GPU_GLES,
];

pub(super) fn bad(m: impl Into<String>) -> Error {
    Error::Value(m.into())
}

pub(super) fn ints(v: Option<&Value>) -> Vec<i64> {
    match v {
        Some(Value::Bytes(b)) => b.iter().map(|&x| x as i64).collect(),
        Some(Value::Array(a)) => a.iter().filter_map(Value::as_i64).collect(),
        _ => Vec::new(),
    }
}

pub(super) fn int(v: &Value, k: &str) -> Result<i64> {
    v.get(k)
        .and_then(Value::as_i64)
        .ok_or_else(|| bad(format!("KeyError: '{k}'")))
}

pub(super) fn is_list(v: Option<&Value>) -> bool {
    matches!(v, Some(Value::Array(_) | Value::Bytes(_)))
}

pub(super) fn sp_type(sp: &Value) -> i64 {
    sp.get("m_GpuProgramType")
        .and_then(Value::as_i64)
        .unwrap_or(-1)
}

/// Keywords and hardware tier of a subprogram (_variant_key).
pub(super) type VariantKey = (Vec<Vec<i64>>, Option<i64>);

pub(super) fn variant_key(sp: &Value) -> VariantKey {
    let kws = KEY_FIELDS
        .iter()
        .filter(|(k, _)| sp.get(k).is_some())
        .map(|(k, _)| {
            let mut v = ints(sp.get(k));
            v.sort();
            v
        })
        .collect();
    (kws, sp.get("m_ShaderHardwareTier").and_then(Value::as_i64))
}

pub(super) type KeySet = BTreeSet<(char, i64)>;

pub(super) fn keyset(sp: &Value) -> KeySet {
    KEY_FIELDS
        .iter()
        .flat_map(|(f, tag)| ints(sp.get(f)).into_iter().map(move |i| (*tag, i)))
        .collect()
}

/// Where a subprogram list is: (SubShader, pass, stage, tier: None for m_SubPrograms).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub(super) struct ListAt {
    pub(super) si: usize,
    pub(super) pi: usize,
    pub(super) stage: &'static str,
    pub(super) tier: Option<usize>,
}

/// A stage program's subprogram lists with their parameter indices (_tier_lists).
#[allow(clippy::type_complexity)]
pub(super) fn tier_lists(
    prog: Option<&Value>,
) -> Vec<(Option<usize>, Vec<Value>, Option<Vec<i64>>)> {
    let Some(prog) = prog.filter(|p| truthy(Some(p))) else {
        return Vec::new();
    };
    if truthy(prog.get("m_PlayerSubPrograms")) {
        let players = prog
            .get("m_PlayerSubPrograms")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default();
        let pbis = prog
            .get("m_ParameterBlobIndices")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default();
        return players
            .into_iter()
            .zip(pbis)
            .enumerate()
            .map(|(t, (l, p))| {
                (
                    Some(t),
                    l.as_array().cloned().unwrap_or_default(),
                    Some(ints(Some(&p))),
                )
            })
            .collect();
    }
    vec![(
        None,
        prog.get("m_SubPrograms")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default(),
        None,
    )]
}

#[allow(clippy::type_complexity)]
pub(super) fn list_at(
    t: &mut Value,
    at: ListAt,
) -> Result<(&mut Vec<Value>, Option<&mut Vec<Value>>)> {
    let p = t
        .path_mut(&["m_ParsedForm", "m_SubShaders"])
        .and_then(Value::as_array_mut)
        .and_then(|s| s.get_mut(at.si))
        .and_then(|s| s.get_mut("m_Passes"))
        .and_then(Value::as_array_mut)
        .and_then(|ps| ps.get_mut(at.pi))
        .ok_or_else(|| bad("pass"))?;
    let prog = p
        .get_mut(at.stage)
        .ok_or_else(|| bad(format!("KeyError: '{}'", at.stage)))?;
    let st = prog.as_struct_mut().ok_or_else(|| bad("program"))?;
    match at.tier {
        None => {
            let l = st
                .get_mut("m_SubPrograms")
                .and_then(Value::as_array_mut)
                .ok_or_else(|| bad("m_SubPrograms"))?;
            Ok((l, None))
        }
        Some(k) => {
            // (two fields of one struct at once)
            let mut lst = None;
            let mut pbi = None;
            for (n, v) in st.fields.iter_mut() {
                if &**n == "m_PlayerSubPrograms" {
                    lst = v
                        .as_array_mut()
                        .and_then(|a| a.get_mut(k))
                        .and_then(Value::as_array_mut);
                } else if &**n == "m_ParameterBlobIndices" {
                    pbi = v
                        .as_array_mut()
                        .and_then(|a| a.get_mut(k))
                        .and_then(Value::as_array_mut);
                }
            }
            Ok((lst.ok_or_else(|| bad("m_PlayerSubPrograms"))?, pbi))
        }
    }
}

pub(super) fn pass_mut(t: &mut Value, si: usize, pi: usize) -> Result<&mut Value> {
    t.path_mut(&["m_ParsedForm", "m_SubShaders"])
        .and_then(Value::as_array_mut)
        .and_then(|s| s.get_mut(si))
        .and_then(|s| s.get_mut("m_Passes"))
        .and_then(Value::as_array_mut)
        .and_then(|ps| ps.get_mut(pi))
        .ok_or_else(|| bad("pass"))
}

/// add_platform: a shader platform listed in a pass's m_Platforms (2020.1+), ascending.
pub(super) fn add_platform(p: &mut Value, platform: i64) -> Result<()> {
    if !is_list(p.get("m_Platforms")) {
        return Ok(());
    }
    let mut v = ints(p.get("m_Platforms"));
    if !v.contains(&platform) {
        v.push(platform);
        v.sort();
        p.set(
            "m_Platforms",
            Value::Array(v.into_iter().map(Value::I64).collect()),
        )?;
    }
    Ok(())
}

/// convert._strip_gles on the tree: the GLES3Plus blob and every GLES subprogram removed.
pub(super) fn strip_gles(t: &mut Value) -> Result<()> {
    let mut blobs = tree::blobs(t).map_err(bad)?;
    blobs.remove(blob::PLATFORM_GLES3PLUS);
    let f = blobs.store().map_err(|e| bad(e.to_string()))?;
    tree::set_blob(t, &f).map_err(bad)?;
    let subs = t
        .path_mut(&["m_ParsedForm", "m_SubShaders"])
        .and_then(Value::as_array_mut)
        .ok_or_else(|| bad("KeyError: 'm_SubShaders'"))?;
    for ss in subs {
        for p in ss
            .get_mut("m_Passes")
            .and_then(Value::as_array_mut)
            .into_iter()
            .flatten()
        {
            let mut any = false;
            for st in STAGES {
                let Some(prog) = p.get_mut(st).filter(|x| truthy(Some(x))) else {
                    continue;
                };
                if let Some(Value::Array(l)) = prog.get_mut("m_SubPrograms") {
                    l.retain(|sp| !GLES_TYPES.contains(&sp_type(sp)));
                    any = true;
                }
                if truthy(prog.get("m_PlayerSubPrograms")) {
                    let n = prog
                        .get("m_PlayerSubPrograms")
                        .and_then(Value::as_array)
                        .map_or(0, Vec::len);
                    for k in 0..n {
                        let lst = prog
                            .get("m_PlayerSubPrograms")
                            .and_then(Value::as_array)
                            .unwrap()[k]
                            .as_array()
                            .cloned()
                            .unwrap_or_default();
                        let keep: Vec<usize> = (0..lst.len())
                            .filter(|&i| !GLES_TYPES.contains(&sp_type(&lst[i])))
                            .collect();
                        let pbis = prog
                            .get_mut("m_ParameterBlobIndices")
                            .and_then(Value::as_array_mut)
                            .ok_or_else(|| bad("KeyError: 'm_ParameterBlobIndices'"))?;
                        let pbi = pbis
                            .get_mut(k)
                            .ok_or_else(|| bad("IndexError: list index out of range"))?;
                        let old = pbi.as_array().cloned().unwrap_or_default();
                        *pbi = Value::Array(
                            keep.iter().filter_map(|&i| old.get(i).cloned()).collect(),
                        );
                        let players = prog
                            .get_mut("m_PlayerSubPrograms")
                            .and_then(Value::as_array_mut)
                            .unwrap();
                        players[k] = Value::Array(keep.iter().map(|&i| lst[i].clone()).collect());
                        any = true;
                    }
                }
            }
            if any && is_list(p.get("m_Platforms")) {
                let v = ints(p.get("m_Platforms"));
                if let Some(i) = v.iter().position(|&x| x == blob::PLATFORM_GLES3PLUS) {
                    let mut v = v;
                    v.remove(i);
                    p.set(
                        "m_Platforms",
                        Value::Array(v.into_iter().map(Value::I64).collect()),
                    )?;
                }
            }
        }
    }
    Ok(())
}

/// {name index: name} of a pass (m_NameIndices).
pub(super) fn pass_names(p: &Value) -> BTreeMap<i64, String> {
    let mut out = BTreeMap::new();
    for kv in p
        .get("m_NameIndices")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
    {
        if let Value::Pair(kv) = kv {
            if let (Some(n), Some(i)) = (kv.0.as_bytes(), kv.1.as_i64()) {
                out.insert(i, String::from_utf8_lossy(n).into_owned());
            }
        }
    }
    out
}

fn name_of(names: &BTreeMap<i64, String>, v: &Value) -> Option<String> {
    v.get("m_NameIndex")
        .and_then(Value::as_i64)
        .and_then(|i| names.get(&i).cloned())
}

fn need_name(names: &BTreeMap<i64, String>, v: &Value) -> Result<String> {
    name_of(names, v).ok_or_else(|| bad("KeyError: name index"))
}

/// The Bindings with a program's m_CommonParameters put back (dxbc.with_common).
pub(super) fn with_common(
    b: Bindings,
    common: Option<&Value>,
    names: &BTreeMap<i64, String>,
) -> Result<std::result::Result<Bindings, String>> {
    const KEYS: [&str; 8] = [
        "m_ConstantBuffers",
        "m_ConstantBufferBindings",
        "m_TextureParams",
        "m_Samplers",
        "m_VectorParams",
        "m_MatrixParams",
        "m_BufferParams",
        "m_UAVParams",
    ];
    let Some(common) = common.filter(|c| truthy(Some(c))) else {
        return Ok(Ok(b));
    };
    let has = |k: &str| truthy(common.get(k));
    if !KEYS.iter().any(|k| has(k)) {
        return Ok(Ok(b));
    }
    if has("m_BufferParams") || has("m_UAVParams") {
        return Ok(Err("buffers or UAVs in the shared parameters".into()));
    }
    if has("m_VectorParams") || has("m_MatrixParams") {
        return Ok(Err("shared parameters outside constant buffers".into()));
    }
    let mut cbs = b.cbuffers.clone();
    let mut binds = b.bindings.clone();
    let mut used = BTreeSet::new();
    let arr = |v: &Value, k: &str| {
        v.get(k)
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default()
    };
    for c in arr(common, "m_ConstantBuffers") {
        let Some(name) = name_of(names, &c) else {
            return Ok(Err("shared constant buffer without a name".into()));
        };
        if truthy(c.get("m_StructParams")) {
            return Ok(Err(format!(
                "constant buffer {} has struct members (instancing data)",
                shaderx::py::repr(&name)
            )));
        }
        let mut params = Vec::new();
        for v in arr(&c, "m_VectorParams") {
            params.push(Param {
                name: need_name(names, &v)?,
                typ: int(&v, "m_Type")? as i32,
                rows: 1,
                cols: int(&v, "m_Dim")? as i32,
                is_matrix: 0,
                array: int(&v, "m_ArraySize")? as i32,
                offset: int(&v, "m_Index")? as i32,
            });
        }
        for m in arr(&c, "m_MatrixParams") {
            params.push(Param {
                name: need_name(names, &m)?,
                typ: int(&m, "m_Type")? as i32,
                rows: int(&m, "m_RowCount")? as i32,
                cols: 4,
                is_matrix: 1,
                array: int(&m, "m_ArraySize")? as i32,
                offset: int(&m, "m_Index")? as i32,
            });
        }
        match (0..cbs.len()).find(|&i| cbs[i].name == name && !used.contains(&i)) {
            None => {
                cbs.push(ConstantBuffer {
                    name,
                    size: int(&c, "m_Size")? as i32,
                    params,
                    structs: Vec::new(),
                });
                used.insert(cbs.len() - 1);
            }
            Some(i) => {
                used.insert(i);
                let mut all = cbs[i].params.clone();
                all.extend(params);
                all.sort_by_key(|p| p.offset);
                cbs[i].params = all;
            }
        }
    }
    for x in arr(common, "m_ConstantBufferBindings") {
        binds.push(Binding {
            name: need_name(names, &x)?,
            kind: KIND_CBUFFER,
            index: int(&x, "m_Index")? as i32,
            extra: vec![0],
            packed: true,
        });
    }
    for x in arr(common, "m_TextureParams") {
        let ms = x
            .get("m_MultiSampled")
            .and_then(Value::as_bool)
            .unwrap_or(false);
        binds.push(Binding {
            name: need_name(names, &x)?,
            kind: KIND_TEXTURE,
            index: int(&x, "m_Index")? as i32,
            extra: vec![
                int(&x, "m_SamplerIndex")? as i32,
                (int(&x, "m_Dim")? as i32) << 1 | ms as i32,
            ],
            packed: true,
        });
    }
    for x in arr(common, "m_Samplers") {
        binds.push(Binding {
            name: String::new(),
            kind: KIND_SAMPLER,
            index: int(&x, "bindPoint")? as i32,
            extra: vec![int(&x, "sampler")? as i32],
            packed: true,
        });
    }
    Ok(Ok(Bindings {
        source_map: b.source_map,
        channels: b.channels,
        cbuffers: cbs,
        bindings: binds,
    }))
}

fn show(n: Option<String>) -> String {
    n.unwrap_or_else(|| "None".into())
}

/// Differences between an entry's binding info and its subprogram's parsed form (dxbc.check_parsed_form).
fn check_parsed_form(b: &Bindings, sp: &Value, names: &BTreeMap<i64, String>) -> Vec<String> {
    let mut out = Vec::new();
    let sp = match sp.get("m_Parameters") {
        Some(p @ Value::Struct(_)) => p,
        _ => sp,
    };
    let arr = |k: &str| {
        sp.get(k)
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default()
    };
    let mut cbs: BTreeMap<&str, &ConstantBuffer> = BTreeMap::new();
    for cb in &b.cbuffers {
        cbs.insert(&cb.name, cb);
    }
    let have: BTreeSet<(String, i64)> = b
        .bindings
        .iter()
        .filter(|x| x.kind == KIND_CBUFFER)
        .map(|x| (x.name.clone(), x.index as i64))
        .collect();
    for x in arr("m_ConstantBufferBindings") {
        let n = name_of(names, &x);
        let i = x.get("m_Index").and_then(Value::as_i64).unwrap_or(0);
        if !n.as_ref().is_some_and(|n| have.contains(&(n.clone(), i))) {
            out.push(format!("constant buffer binding {}/{i}", show(n)));
        }
    }
    for cb in arr("m_ConstantBuffers") {
        let n = name_of(names, &cb);
        let mine = n.as_deref().and_then(|n| cbs.get(n));
        let size = cb.get("m_Size").and_then(Value::as_i64);
        let Some(mine) = mine.filter(|m| size.is_none_or(|s| s == m.size as i64)) else {
            out.push(format!("constant buffer {}", show(n)));
            continue;
        };
        let offs: BTreeMap<&str, i32> = mine
            .params
            .iter()
            .map(|p| (p.name.as_str(), p.offset))
            .collect();
        let ps = cb
            .get("m_VectorParams")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default()
            .into_iter()
            .chain(
                cb.get("m_MatrixParams")
                    .and_then(Value::as_array)
                    .cloned()
                    .unwrap_or_default(),
            );
        for p in ps {
            let pn = name_of(names, &p);
            let off = pn.as_deref().and_then(|x| offs.get(x)).map(|&o| o as i64);
            if off != p.get("m_Index").and_then(Value::as_i64) {
                out.push(format!("constant {} in {}", show(pn), mine.name));
            }
        }
    }
    let tex: BTreeSet<(String, i64, Option<i64>)> = b
        .bindings
        .iter()
        .filter(|x| x.kind == KIND_TEXTURE)
        .map(|x| (x.name.clone(), x.index as i64, x.dim().map(|d| d as i64)))
        .collect();
    for x in arr("m_TextureParams") {
        let n = name_of(names, &x);
        let i = x.get("m_Index").and_then(Value::as_i64).unwrap_or(0);
        let d = x.get("m_Dim").and_then(Value::as_i64);
        if !n.as_ref().is_some_and(|n| tex.contains(&(n.clone(), i, d))) {
            out.push(format!("texture {}/{i}", show(n)));
        }
    }
    out
}

/// (keywords, local keywords or None) of a GL program made of two D3D11 entries (_entry_keywords).
pub(super) fn entry_keywords(
    ve: Option<&Entry>,
    pe: Option<&Entry>,
) -> (Vec<String>, Option<Vec<String>>) {
    let merged = |a: &[String], b: &[String]| {
        let mut out = a.to_vec();
        out.extend(b.iter().filter(|k| !a.contains(k)).cloned());
        out
    };
    let mut vk = ve.map(|e| e.keywords.clone()).unwrap_or_default();
    let mut vl = ve.and_then(|e| e.local_keywords.clone());
    if let Some(pe) = pe {
        vk = merged(&vk, &pe.keywords);
        if let (Some(l), Some(pl)) = (&vl, &pe.local_keywords) {
            vl = Some(merged(l, pl));
        }
    }
    (vk, vl)
}

pub(super) fn entry_of(entries: &[Option<Slot>], i: i64) -> Result<Option<&Entry>> {
    let e = usize::try_from(i)
        .ok()
        .and_then(|i| entries.get(i))
        .ok_or_else(|| bad("IndexError: list index out of range"))?;
    Ok(match e {
        Some(Slot::Program(e)) => Some(e),
        _ => None,
    })
}

/// The GLES3 subprogram of a D3D11 one (gl_twin).
pub(super) fn gl_twin(
    sp: &Value,
    blob_index: usize,
    gl: Option<(&Bindings, &BTreeMap<String, i64>)>,
    gpu_type: i64,
) -> Result<Value> {
    let mut t = sp.clone();
    t.set("m_GpuProgramType", Value::I64(gpu_type))?;
    t.set("m_BlobIndex", Value::I64(blob_index as i64))?;
    for k in D3D_ONLY_LISTS {
        if is_list(t.get(k)) {
            t.set(k, Value::Array(Vec::new()))?;
        }
    }
    if let Some(Value::Struct(ps)) = t.get_mut("m_Parameters") {
        for (_, v) in ps.fields.iter_mut() {
            if matches!(v, Value::Array(_) | Value::Bytes(_)) {
                *v = Value::Array(Vec::new());
            }
        }
    }
    if let Some(ch @ Value::Struct(_)) = t.get_mut("m_Channels") {
        if is_list(ch.get("m_Channels")) {
            ch.set("m_Channels", Value::Array(Vec::new()))?;
        }
    }
    if let Some((b, names)) = gl {
        if is_list(t.get("m_ConstantBuffers")) {
            fill_parsed_form(&mut t, b, names)?;
        }
    }
    Ok(t)
}

fn obj(fields: Vec<(&str, Value)>) -> Value {
    Value::Struct(unityfs::Struct {
        fields: fields.into_iter().map(|(k, v)| (k.into(), v)).collect(),
    })
}

fn push(t: &mut Value, k: &str, v: Value) -> Result<()> {
    match t.get_mut(k) {
        Some(Value::Array(a)) => {
            a.push(v);
            Ok(())
        }
        Some(x @ Value::Bytes(_)) => {
            *x = Value::Array(vec![v]);
            Ok(())
        }
        _ => Err(bad(format!("KeyError: '{k}'"))),
    }
}

/// A 2019.x subprogram's parameter lists from a GL binding info (_fill_parsed_form).
fn fill_parsed_form(t: &mut Value, b: &Bindings, names: &BTreeMap<String, i64>) -> Result<()> {
    let nid = |n: &str| -> Result<Value> {
        names
            .get(n)
            .map(|&i| Value::I64(i))
            .ok_or_else(|| bad(format!("KeyError: '{n}'")))
    };
    for cb in &b.cbuffers {
        let mut vec = Vec::new();
        let mut mat = Vec::new();
        for p in &cb.params {
            if p.is_matrix == 0 {
                vec.push(obj(vec![
                    ("m_NameIndex", nid(&p.name)?),
                    ("m_Index", Value::I64(p.offset as i64)),
                    ("m_ArraySize", Value::I64(p.array as i64)),
                    ("m_Type", Value::I64(p.typ as i64)),
                    ("m_Dim", Value::I64(p.cols as i64)),
                ]));
            } else {
                mat.push(obj(vec![
                    ("m_NameIndex", nid(&p.name)?),
                    ("m_Index", Value::I64(p.offset as i64)),
                    ("m_ArraySize", Value::I64(p.array as i64)),
                    ("m_Type", Value::I64(p.typ as i64)),
                    ("m_RowCount", Value::I64(p.rows as i64)),
                ]));
            }
        }
        if cb.name.is_empty() {
            for v in vec {
                push(t, "m_VectorParams", v)?;
            }
            for m in mat {
                push(t, "m_MatrixParams", m)?;
            }
            continue;
        }
        push(
            t,
            "m_ConstantBuffers",
            obj(vec![
                ("m_NameIndex", nid(&cb.name)?),
                ("m_MatrixParams", Value::Array(mat)),
                ("m_VectorParams", Value::Array(vec)),
                ("m_StructParams", Value::Array(Vec::new())),
                ("m_Size", Value::I64(cb.size as i64)),
            ]),
        )?;
    }
    for x in &b.bindings {
        if x.kind == KIND_TEXTURE {
            push(
                t,
                "m_TextureParams",
                obj(vec![
                    ("m_NameIndex", nid(&x.name)?),
                    ("m_Index", Value::I64(x.index as i64)),
                    ("m_SamplerIndex", Value::I64(x.sampler().unwrap() as i64)),
                    ("m_MultiSampled", Value::Bool(x.extra[1] & 1 != 0)),
                    ("m_Dim", Value::I64(x.dim().unwrap() as i64)),
                ]),
            )?;
        } else if x.kind == KIND_CBUFFER {
            push(
                t,
                "m_ConstantBufferBindings",
                obj(vec![
                    ("m_NameIndex", nid(&x.name)?),
                    ("m_Index", Value::I64(x.index as i64)),
                ]),
            )?;
        }
    }
    Ok(())
}

/// The names a GL binding info uses (_gl_names).
pub(super) fn gl_names(info: &Bindings) -> Vec<String> {
    let mut out = Vec::new();
    for cb in info.cbuffers.iter().filter(|c| !c.name.is_empty()) {
        out.push(cb.name.clone());
        out.extend(cb.params.iter().map(|p| p.name.clone()));
    }
    for cb in info.cbuffers.iter().filter(|c| c.name.is_empty()) {
        out.extend(cb.params.iter().map(|p| p.name.clone()));
    }
    out.extend(
        info.bindings
            .iter()
            .filter(|x| !x.name.is_empty())
            .map(|x| x.name.clone()),
    );
    out
}

/// The pass's {name: index}, with the names it lacks added after the last (_name_indices).
pub(super) fn name_indices(p: &mut Value, needed: &[String]) -> Result<BTreeMap<String, i64>> {
    if p.get("m_NameIndices").is_none() {
        p.set("m_NameIndices", Value::Array(Vec::new()))?;
    }
    let pairs = p
        .get_mut("m_NameIndices")
        .and_then(Value::as_array_mut)
        .ok_or_else(|| bad("m_NameIndices"))?;
    let mut have: BTreeMap<String, i64> = BTreeMap::new();
    for kv in pairs.iter() {
        if let Value::Pair(kv) = kv {
            if let (Some(n), Some(i)) = (kv.0.as_bytes(), kv.1.as_i64()) {
                have.insert(String::from_utf8_lossy(n).into_owned(), i);
            }
        }
    }
    let mut nxt = have.values().copied().max().unwrap_or(-1) + 1;
    for n in needed {
        if !have.contains_key(n) {
            pairs.push(Value::Pair(Box::new((Value::str(n), Value::I64(nxt)))));
            have.insert(n.clone(), nxt);
            nxt += 1;
        }
    }
    Ok(have)
}

/// One planned GL program: its pass, tier, D3D11 vertex and pixel subprograms, translation.
pub(super) struct Todo {
    pub(super) si: usize,
    pub(super) pi: usize,
    pub(super) tier: usize,
    pub(super) vsp: Value,
    pub(super) vpi: Option<i64>,
    pub(super) psp: Value,
    pub(super) es: String,
    pub(super) info: Option<Bindings>,
    pub(super) typ: i64,
    pub(super) keys: KeySet,
}

type Cached = std::result::Result<(String, Option<Bindings>, i64), String>;

/// convert.add_gles3_d3d11: GLES3 programs translated from the shader's Direct3D 11 ones (tree
/// edited in place). unity: the file's version string.
#[allow(clippy::type_complexity, clippy::map_entry)]
pub fn add_gles3_d3d11(
    t: &mut Value,
    tr: &mut Translator,
    precision: &str,
    replace: bool,
    optimize: Optimize,
    unity: &str,
) -> Result<Report> {
    let name = t
        .path(&["m_ParsedForm", "m_Name"])
        .and_then(Value::as_bytes)
        .map(|b| String::from_utf8_lossy(b).into_owned())
        .ok_or_else(|| bad("KeyError: 'm_Name'"))?;
    let mut r = Report::new(&name, false);
    r.source = "d3d11";
    let plats = ints(t.get("platforms"));
    if !plats.contains(&blob::PLATFORM_D3D11) {
        r.why = "no Direct3D 11 programs".into();
        return Ok(r);
    }
    let had_gles3 = plats.contains(&blob::PLATFORM_GLES3PLUS);
    if had_gles3 {
        if !replace {
            r.why = "already has GLES3 programs".into();
            return Ok(r);
        }
        strip_gles(t)?;
        r.status = Status::Converted;
        r.why = "(old GLES programs removed)".into();
    }
    let mut blobs = tree::blobs(t).map_err(bad)?;
    let mut entries = tree::platform_entries(t, blob::PLATFORM_D3D11, &blobs).map_err(bad)?;
    let mut newer: Vec<i32> = entries
        .iter()
        .filter_map(|e| match e {
            Some(Slot::Program(e)) if !dxbc::SERIALIZERS.contains(&e.version) => Some(e.version),
            _ => None,
        })
        .collect();
    newer.sort();
    if let Some(v) = newer.first() {
        r.status = Status::Skipped;
        r.why = format!("Direct3D 11 programs of serializer {v}: layout not known");
        r.changed = replace && had_gles3;
        return Ok(r);
    }
    let serializer = entries.iter().find_map(|e| match e {
        Some(Slot::Program(e)) => Some(e.version),
        _ => None,
    });
    let mut line = unity_of(t, unity);
    if serializer.is_some() && line < dxbc::unity_line(serializer) {
        line = dxbc::unity_line(serializer);
    }
    if entries.is_empty() && line >= (2018, 1) {
        let sc = blobs.stage_counts.is_some().then_some(1);
        blobs.set_entries(blob::PLATFORM_GLES3PLUS, &[], &[], sc);
        let f = blobs.store().map_err(|e| bad(e.to_string()))?;
        tree::set_blob(t, &f).map_err(bad)?;
        r.status = Status::Converted;
        r.empty = true;
        r.why = shaderx::py::strip(&format!(
            "no programs (every variant stripped): an empty GLES3 blob {}",
            r.why
        ))
        .to_string();
        return Ok(r);
    }
    let gl_info = line >= (2019, 1);
    let compact = line >= (2020, 1);
    let separate = serializer == Some(bindings::SEPARATE_PARAMS_FROM);
    let mut cache: HashMap<((i64, Option<i64>), (i64, Option<i64>)), Cached> = HashMap::new();

    // the binding info of a subprogram (its entry, parameter entry, shared parameters), or why not
    let stage_bindings = |p: &Value,
                          prog: &Value,
                          sp: &Value,
                          e: Option<&Entry>,
                          pi: Option<i64>|
     -> Result<std::result::Result<Bindings, String>> {
        let Some(e) = e.filter(|e| !e.code.is_empty()) else {
            return Ok(Err("empty program entry".into()));
        };
        let names = pass_names(p);
        let pe = match pi {
            Some(i) => usize::try_from(i)
                .ok()
                .and_then(|i| entries.get(i))
                .ok_or_else(|| bad("IndexError: list index out of range"))?
                .clone(),
            None => None,
        };
        let pdata = match &pe {
            Some(Slot::Params { data, .. }) => Some(data.as_slice()),
            _ => None,
        };
        if separate && pdata.is_none() {
            return Ok(Err("no parameter entry".into()));
        }
        let b = match bindings::entry_bindings(e, pdata) {
            Ok(b) => b,
            Err(u) => return Ok(Err(u.0)),
        };
        let b = match with_common(b, prog.get("m_CommonParameters"), &names)? {
            Ok(b) => b,
            Err(m) => return Ok(Err(m)),
        };
        let badf = check_parsed_form(&b, sp, &names);
        if !badf.is_empty() {
            return Ok(Err(format!(
                "binding info disagrees with the parsed form ({})",
                badf.iter().take(3).cloned().collect::<Vec<_>>().join(", ")
            )));
        }
        Ok(Ok(b))
    };

    let mut plan: Vec<(usize, Vec<Todo>)> = Vec::new();
    let mut no_instancing: BTreeSet<(usize, usize)> = BTreeSet::new();
    let subs = t
        .path(&["m_ParsedForm", "m_SubShaders"])
        .and_then(Value::as_array)
        .cloned()
        .ok_or_else(|| bad("KeyError: 'm_SubShaders'"))?;
    for (si, ss) in subs.iter().enumerate() {
        let mut todo = Vec::new();
        let mut why: Option<String> = None;
        let passes = ss
            .get("m_Passes")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default();
        for (pi_, p) in passes.iter().enumerate() {
            let empty = Value::Struct(Default::default());
            let vprog = p.get("progVertex").unwrap_or(&empty);
            let fprog = p.get("progFragment").unwrap_or(&empty);
            let vlists = tier_lists(Some(vprog));
            let flists = tier_lists(Some(fprog));
            let mut others: BTreeSet<VariantKey> = BTreeSet::new();
            for st in OTHER_STAGES {
                for (_, lst, _) in tier_lists(p.get(st)) {
                    for sp in &lst {
                        if dxbc::OTHER_STAGE_TYPES.contains(&sp_type(sp)) {
                            others.insert(variant_key(sp));
                        }
                    }
                }
            }
            for (tn, (_, vlist, vpbi)) in vlists.iter().enumerate() {
                let (flist, fpbi) = flists
                    .get(tn)
                    .map_or((Vec::new(), None), |(_, l, p)| (l.clone(), p.clone()));
                let pick = |l: &[Value],
                            pbi: &Option<Vec<i64>>,
                            types: &[i64]|
                 -> Vec<(Value, Option<i64>)> {
                    l.iter()
                        .enumerate()
                        .filter(|(_, sp)| types.contains(&sp_type(sp)))
                        .map(|(j, sp)| {
                            (
                                sp.clone(),
                                pbi.as_ref().map(|p| p.get(j).copied().unwrap_or(0)),
                            )
                        })
                        .collect()
                };
                let vss = pick(vlist, vpbi, &dxbc::VERTEX_TYPES);
                let pss = pick(&flist, &fpbi, &dxbc::PIXEL_TYPES);
                let vuse: KeySet = vss.iter().flat_map(|(sp, _)| keyset(sp)).collect();
                let puse: KeySet = pss.iter().flat_map(|(sp, _)| keyset(sp)).collect();
                for (vsp, vpi) in &vss {
                    let a = keyset(vsp);
                    let tier = vsp.get("m_ShaderHardwareTier").and_then(Value::as_int);
                    let mut pairs: Vec<(Option<(&Value, Option<i64>)>, KeySet)> = Vec::new();
                    for (psp, ppi) in &pss {
                        let b = keyset(psp);
                        if psp.get("m_ShaderHardwareTier").and_then(Value::as_int) == tier
                            && b.intersection(&vuse).all(|k| a.contains(k))
                            && a.intersection(&puse).all(|k| b.contains(k))
                        {
                            pairs.push((Some((psp, *ppi)), a.union(&b).cloned().collect()));
                        }
                    }
                    if pairs.is_empty() {
                        pairs.push((None, a.clone()));
                    }
                    for (pp, keys) in pairs {
                        let vi = int(vsp, "m_BlobIndex")?;
                        let ve = entry_of(&entries, vi)?;
                        let (pe, pidx) = match pp {
                            Some((psp, _)) => {
                                let i = int(psp, "m_BlobIndex")?;
                                (entry_of(&entries, i)?, Some(i))
                            }
                            None => (None, None),
                        };
                        let (k0, k1) = entry_keywords(ve, pe);
                        let mut kws = k0;
                        kws.extend(k1.unwrap_or_default());
                        let mut res: Option<(String, Option<Bindings>, i64)> = None;
                        let err: Option<String> = match pp {
                            None => Some("no pixel shader for this variant".into()),
                            Some(_) if others.contains(&variant_key(vsp)) => {
                                Some("geometry or tessellation stage (not in GLSL ES 3.00)".into())
                            }
                            Some((psp, ppi)) => {
                                let vb = stage_bindings(p, vprog, vsp, ve, *vpi)?;
                                let pb = stage_bindings(p, fprog, psp, pe, ppi)?;
                                match (vb, pb) {
                                    (Err(m), _) | (_, Err(m)) => Some(m),
                                    (Ok(vb), Ok(pb)) => {
                                        let key = ((vi, *vpi), (pidx.unwrap(), ppi));
                                        if !cache.contains_key(&key) {
                                            let c = program(
                                                tr, ve, &vb, pe, &pb, line, gl_info, precision,
                                                optimize, &mut r,
                                            )?;
                                            cache.insert(key, c);
                                        }
                                        match &cache[&key] {
                                            Ok(x) => {
                                                res = Some(x.clone());
                                                None
                                            }
                                            Err(m) => Some(m.clone()),
                                        }
                                    }
                                }
                            }
                        };
                        match err {
                            None => {
                                let (es, info, typ) = res.unwrap();
                                todo.push(Todo {
                                    si,
                                    pi: pi_,
                                    tier: tn,
                                    vsp: vsp.clone(),
                                    vpi: *vpi,
                                    psp: pp.unwrap().0.clone(),
                                    es,
                                    info,
                                    typ,
                                    keys,
                                });
                            }
                            Some(err) => {
                                if kws.iter().any(|k| VR_KEYWORDS.contains(&k.as_str())) {
                                    r.dropped += 1;
                                } else if kws
                                    .iter()
                                    .any(|k| INSTANCING_KEYWORDS.contains(&k.as_str()))
                                {
                                    r.dropped_instancing += 1;
                                    no_instancing.insert((si, pi_));
                                } else if kws
                                    .iter()
                                    .any(|k| SKINNING_KEYWORDS.contains(&k.as_str()))
                                {
                                    r.dropped_skinning += 1;
                                } else if why.is_none() {
                                    why = Some(format!(
                                        "pass {pi_} variant {}: {err}",
                                        if kws.is_empty() {
                                            "(no keywords)".to_string()
                                        } else {
                                            kws.join(" ")
                                        }
                                    ));
                                }
                            }
                        }
                    }
                }
            }
        }
        match why {
            None if !todo.is_empty() => plan.push((si, todo)),
            Some(w) => r.failed.push((si, w)),
            None => {}
        }
    }
    if plan.is_empty() {
        r.status = Status::Skipped;
        r.why = if let Some((_, w)) = r.failed.first() {
            w.clone()
        } else if r.dropped > 0 {
            format!("only VR-only variants, none translatable ({})", r.dropped)
        } else {
            "no Direct3D 11 vertex/pixel programs".into()
        };
        r.changed = replace && had_gles3;
        return Ok(r);
    }

    store(
        t,
        &mut r,
        &plan,
        &mut entries,
        &mut blobs,
        &subs,
        &no_instancing,
        compact,
        separate,
        blob::PLATFORM_D3D11,
    )?;
    Ok(r)
}

/// The GLES3 programs and twins of a plan (add_gles3_d3d11 and metal.add_gles3_metal after
/// planning): stored as Unity stores GL programs of the data's version, the shared parameters put
/// back into the source platform's entries (2020.1+). platform: the source (Direct3D 11 or Metal).
#[allow(clippy::too_many_arguments, clippy::map_entry)]
pub(super) fn store(
    t: &mut Value,
    r: &mut Report,
    plan: &[(usize, Vec<Todo>)],
    entries: &mut [Option<Slot>],
    blobs: &mut ShaderBlobs,
    subs: &[Value],
    no_instancing: &BTreeSet<(usize, usize)>,
    compact: bool,
    separate: bool,
    platform: i64,
) -> Result<()> {
    let metal = platform == blob::PLATFORM_METAL;
    // (list, the twin, its parameter index)
    let mut twins: Vec<(ListAt, Value, Option<usize>)> = Vec::new();
    if !compact {
        let mut out: Vec<Option<Slot>> = Vec::with_capacity(entries.len());
        for e in entries.iter() {
            out.push(match e {
                None => None,
                Some(_) if metal && !matches!(e, Some(Slot::Program(_))) => None,
                Some(Slot::Program(e)) => Some(Slot::Program(Entry {
                    version: e.version,
                    gpu_type: blob::GPU_GLES3 as i32,
                    stats: vec![0; e.stats.len()],
                    keywords: e.keywords.clone(),
                    local_keywords: e.local_keywords.clone(),
                    code: Vec::new(),
                    rest: bindings::gl_rest(0),
                    guessed: false,
                })),
                Some(_) => return Err(bad("AttributeError: an entry without stats")),
            });
        }
        let mut segs = if blobs.nested {
            Some(blobs.segments(platform).map_err(|e| bad(e.to_string()))?)
        } else {
            None
        };
        let mut vslot: HashMap<(i64, i64), usize> = HashMap::new();
        let mut used: BTreeSet<usize> = BTreeSet::new();
        for (_, todo) in plan {
            for td in todo {
                let vi = int(&td.vsp, "m_BlobIndex")?;
                let fi = int(&td.psp, "m_BlobIndex")?;
                if !vslot.contains_key(&(vi, fi)) {
                    let slot = if !used.contains(&(vi as usize)) {
                        vi as usize
                    } else {
                        out.len()
                    };
                    if slot == out.len() {
                        out.push(None);
                        if let Some(s) = &mut segs {
                            let x = s[vi as usize];
                            s.push(x);
                        }
                    }
                    used.insert(slot);
                    vslot.insert((vi, fi), slot);
                    let e = entry_of(entries, vi)?.unwrap();
                    let rest = match &td.info {
                        Some(i) => i.serialize(),
                        None if metal => bindings::gl_rest(source_map(e)),
                        None => {
                            let sm = bindings::Bindings::parse(&e.rest, Some(e.version))
                                .map_err(|u| bad(u.0))?
                                .source_map;
                            bindings::gl_rest(sm)
                        }
                    };
                    let (kw, lkw) = entry_keywords(Some(e), entry_of(entries, fi)?);
                    out[slot] = Some(Slot::Program(Entry {
                        version: e.version,
                        gpu_type: td.typ as i32,
                        stats: vec![0; e.stats.len()],
                        keywords: kw,
                        local_keywords: lkw,
                        code: td.es.clone().into_bytes(),
                        rest,
                        guessed: false,
                    }));
                    r.programs += 1;
                    r.es31 += (td.typ == blob::GPU_GLES31) as usize;
                }
                used.insert(fi as usize);
                if td.typ == blob::GPU_GLES31 {
                    if let Some(Some(Slot::Program(e))) = out.get_mut(fi as usize) {
                        e.gpu_type = td.typ as i32;
                    }
                }
                let slot = vslot[&(vi, fi)];
                let tw = match &td.info {
                    Some(info) => {
                        let p = pass_mut(t, td.si, td.pi)?;
                        let names = name_indices(p, &gl_names(info))?;
                        gl_twin(&td.vsp, slot, Some((info, &names)), td.typ)?
                    }
                    None => gl_twin(&td.vsp, slot, None, td.typ)?,
                };
                let mut tw = tw;
                set_keywords(&mut tw, &td.keys)?;
                twins.push((
                    ListAt {
                        si: td.si,
                        pi: td.pi,
                        stage: "progVertex",
                        tier: None,
                    },
                    tw,
                    None,
                ));
                twins.push((
                    ListAt {
                        si: td.si,
                        pi: td.pi,
                        stage: "progFragment",
                        tier: None,
                    },
                    gl_twin(&td.psp, fi as usize, None, td.typ)?,
                    None,
                ));
            }
        }
        let segs_v = segs.unwrap_or_default();
        blobs.set_entries(blob::PLATFORM_GLES3PLUS, &out, &segs_v, None);
    } else {
        let mut out: Vec<Option<Slot>> = Vec::new();
        let mut segs: Vec<i64> = Vec::new();
        let d3d_segs = if blobs.nested {
            blobs.segments(platform).map_err(|e| bad(e.to_string()))?
        } else {
            vec![0; entries.len()]
        };
        let mut code_at: HashMap<(i64, Option<i64>, i64), usize> = HashMap::new();
        let mut param_at: HashMap<Vec<u8>, usize> = HashMap::new();
        for (_, todo) in plan {
            for td in todo {
                let vi = int(&td.vsp, "m_BlobIndex")?;
                let fi = int(&td.psp, "m_BlobIndex")?;
                let key = (vi, td.vpi, fi);
                let e = entry_of(entries, vi)?.unwrap();
                let info = td.info.as_ref().ok_or_else(|| bad("no GL binding info"))?;
                if !code_at.contains_key(&key) {
                    let rest = if separate {
                        info.head()
                    } else {
                        info.serialize()
                    };
                    code_at.insert(key, out.len());
                    let (kw, lkw) = entry_keywords(Some(e), entry_of(entries, fi)?);
                    out.push(Some(Slot::Program(Entry {
                        version: e.version,
                        gpu_type: td.typ as i32,
                        stats: vec![0; e.stats.len()],
                        keywords: kw,
                        local_keywords: lkw,
                        code: td.es.clone().into_bytes(),
                        rest,
                        guessed: false,
                    })));
                    segs.push(d3d_segs[vi as usize]);
                    r.programs += 1;
                    r.es31 += (td.typ == blob::GPU_GLES31) as usize;
                }
                let mut pidx = None;
                if separate {
                    let data = info.params_bytes();
                    if !param_at.contains_key(&data) {
                        param_at.insert(data.clone(), out.len());
                        out.push(Some(Slot::Params {
                            version: e.version,
                            data: data.clone(),
                        }));
                        segs.push(d3d_segs[vi as usize]);
                    }
                    pidx = Some(param_at[&data]);
                }
                let p = &subs[td.si]
                    .get("m_Passes")
                    .and_then(Value::as_array)
                    .unwrap()[td.pi];
                let tl = tier_lists(p.get("progVertex"));
                let tier = tl
                    .get(td.tier)
                    .map(|x| x.0)
                    .ok_or_else(|| bad("IndexError: list index out of range"))?;
                let mut tw = gl_twin(&td.vsp, code_at[&key], None, td.typ)?;
                set_keywords(&mut tw, &td.keys)?;
                twins.push((
                    ListAt {
                        si: td.si,
                        pi: td.pi,
                        stage: "progVertex",
                        tier,
                    },
                    tw,
                    pidx,
                ));
            }
        }
        restore_common(t, plan, entries, blobs, separate, platform)?;
        let sc = blobs.stage_counts.is_some().then_some(1);
        blobs.set_entries(blob::PLATFORM_GLES3PLUS, &out, &segs, sc);
    }
    let f = blobs.store().map_err(|e| bad(e.to_string()))?;
    tree::set_blob(t, &f).map_err(bad)?;
    // commit the twins (each fragment subprogram once, however many vertex ones share it)
    let mut seen: BTreeSet<(ListAt, i64, VariantKey)> = BTreeSet::new();
    for (at, tw, pidx) in twins {
        let key = (at, int(&tw, "m_BlobIndex")?, variant_key(&tw));
        if !seen.insert(key) {
            continue;
        }
        let (lst, pbi) = list_at(t, at)?;
        lst.push(tw);
        if let Some(pbi) = pbi {
            pbi.push(Value::I64(pidx.map_or(-1, |x| x as i64)));
        }
        r.twins += 1;
    }
    for (_, todo) in plan {
        for td in todo {
            let p = pass_mut(t, td.si, td.pi)?;
            add_platform(p, blob::PLATFORM_GLES3PLUS)?;
            if no_instancing.contains(&(td.si, td.pi)) {
                for f in ["m_HasInstancingVariant", "m_HasProceduralInstancingVariant"] {
                    if p.get(f).is_some() {
                        p.set(f, Value::Bool(false))?;
                    }
                }
            }
        }
    }
    r.status = Status::Converted;
    if !r.failed.is_empty() {
        let left: Vec<String> = r
            .failed
            .iter()
            .map(|(si, w)| format!("SubShader {si} left out: {w}"))
            .collect();
        r.why = if r.why.is_empty() {
            left.join("; ")
        } else {
            format!("{} {}", r.why, left.join("; "))
        };
    }
    Ok(())
}

/// A twin's keyword index lists from a keyword set (_set_keywords).
pub(super) fn set_keywords(t: &mut Value, keys: &KeySet) -> Result<()> {
    for (f, tag) in KEY_FIELDS {
        if t.get(f).is_some() {
            let v: Vec<Value> = keys
                .iter()
                .filter(|(g, _)| *g == tag)
                .map(|(_, i)| Value::I64(*i))
                .collect();
            t.set(f, Value::Array(v))?;
        }
    }
    Ok(())
}

/// One GL program translated (add_gles3_d3d11's program()): (GLSL ES, GL binding info, type) or why not.
#[allow(clippy::too_many_arguments)]
fn program(
    tr: &mut Translator,
    vs: Option<&Entry>,
    vb: &Bindings,
    ps: Option<&Entry>,
    pb: &Bindings,
    line: (u64, u64),
    gl_info: bool,
    precision: &str,
    optimize: Optimize,
    r: &mut Report,
) -> Result<Cached> {
    let (Some(vs), Some(ps)) = (
        vs.filter(|e| !e.code.is_empty()),
        ps.filter(|e| !e.code.is_empty()),
    ) else {
        return Ok(Err("empty program entry".into()));
    };
    r.guessed_layout |= vs.guessed || ps.guessed;
    let mut uv_notes = Vec::new();
    let gl = match dxbc::translate_program(tr, (&vs.code, vb), (&ps.code, pb), line, &mut uv_notes)
    {
        Ok(g) => g,
        Err(dxbc::Fault::Unsupported(u)) => return Ok(Err(u.0)),
        Err(dxbc::Fault::Crash(m)) => return Err(bad(m)),
    };
    let mut info = None;
    if gl_info {
        let mut arrays: HashMap<String, i32> = HashMap::new();
        for b in [vb, pb] {
            for cb in &b.cbuffers {
                for p in &cb.params {
                    arrays.insert(p.name.clone(), p.array);
                }
            }
        }
        match bindings::gl_bindings(&gl, line, vb.source_map, Some(&arrays)) {
            Ok(b) => info = Some(b),
            Err(u) => return Ok(Err(u.0)),
        }
    }
    let mut notes = Vec::new();
    let (es, typ) = match glsl::program_to_es(&gl, precision, optimize, &mut notes, Es::Es300) {
        Ok(s) => (s, blob::GPU_GLES3),
        Err(u) if u.needs_es31 => {
            notes.clear();
            match glsl::program_to_es(&gl, precision, optimize, &mut notes, Es::Es310) {
                Ok(s) => (s, blob::GPU_GLES31),
                Err(u) => return Ok(Err(u.msg)),
            }
        }
        Err(u) => return Ok(Err(u.msg)),
    };
    r.notes.extend(notes);
    r.notes.extend(uv_notes);
    Ok(Ok((es, info, typ)))
}

/// 2020.1+: a vertex program given GLES3 subprograms gets its m_CommonParameters put back into its
/// D3D11 subprograms' binding info, and cleared (_restore_common).
fn restore_common(
    t: &mut Value,
    plan: &[(usize, Vec<Todo>)],
    entries: &mut [Option<Slot>],
    blobs: &mut ShaderBlobs,
    separate: bool,
    platform: i64,
) -> Result<()> {
    let vs_types: &[i64] = if platform == blob::PLATFORM_METAL {
        &[blob::GPU_METAL_VS]
    } else {
        &dxbc::VERTEX_TYPES
    };
    let mut passes: Vec<(usize, usize)> = Vec::new();
    for (_, todo) in plan {
        for td in todo {
            if !passes.contains(&(td.si, td.pi)) {
                passes.push((td.si, td.pi));
            }
        }
    }
    let mut changed = false;
    for (si, pi) in passes {
        let p = pass_mut(t, si, pi)?;
        let names = pass_names(p);
        let Some(prog) = p.get("progVertex").cloned() else {
            continue;
        };
        let common = prog.get("m_CommonParameters").cloned();
        let any = common.as_ref().and_then(Value::as_struct).is_some_and(|c| {
            c.fields.iter().any(|(_, v)| {
                matches!(v, Value::Array(a) if !a.is_empty())
                    || matches!(v, Value::Bytes(b) if !b.is_empty())
            })
        });
        if !any {
            continue;
        }
        let mut done: BTreeSet<i64> = BTreeSet::new();
        for (_, lst, pbi) in tier_lists(Some(&prog)) {
            for (j, sp) in lst.iter().enumerate() {
                if !vs_types.contains(&sp_type(sp)) {
                    continue;
                }
                let pi_ = pbi.as_ref().map(|p| p.get(j).copied().unwrap_or(0));
                let bi = int(sp, "m_BlobIndex")?;
                let at = if separate { pi_.unwrap_or(-1) } else { bi };
                if !done.insert(at) {
                    continue;
                }
                let Some(e) = entry_of(entries, bi)?.cloned() else {
                    continue;
                };
                let pe = pi_
                    .and_then(|i| usize::try_from(i).ok())
                    .and_then(|i| entries.get(i))
                    .cloned()
                    .flatten();
                let pdata = match &pe {
                    Some(Slot::Params { data, .. }) => Some(data.clone()),
                    _ => None,
                };
                let Ok(b) = bindings::entry_bindings(&e, pdata.as_deref()) else {
                    continue;
                };
                let Ok(Ok(b)) = with_common(b, common.as_ref(), &names) else {
                    continue;
                };
                if separate {
                    let Some(Slot::Params { version, .. }) = pe else {
                        continue;
                    };
                    entries[pi_.unwrap() as usize] = Some(Slot::Params {
                        version,
                        data: b.params_bytes(),
                    });
                } else {
                    let mut ne = e.clone();
                    ne.rest = b.serialize();
                    entries[at as usize] = Some(Slot::Program(ne));
                }
                changed = true;
            }
        }
        // (_clear_common: every list of the program's m_CommonParameters emptied)
        let p = pass_mut(t, si, pi)?;
        if let Some(Value::Struct(c)) = p.path_mut(&["progVertex", "m_CommonParameters"]) {
            for (_, v) in c.fields.iter_mut() {
                match v {
                    Value::Array(a) => a.clear(),
                    Value::Bytes(b) => b.clear(),
                    _ => {}
                }
            }
        }
    }
    if changed {
        let segs = if blobs.nested {
            blobs.segments(platform).map_err(|e| bad(e.to_string()))?
        } else {
            Vec::new()
        };
        blobs.set_entries(platform, entries, &segs, None);
    }
    Ok(())
}

/// The source map of an entry's binding info (metal._source_map: its first 4 bytes, else 0).
pub(super) fn source_map(e: &Entry) -> i32 {
    if e.rest.len() >= 4 {
        i32::from_le_bytes(e.rest[..4].try_into().unwrap())
    } else {
        0
    }
}
