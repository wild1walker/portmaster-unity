//! Shaders from Vulkan (vulkan.add_gles3_vulkan on the Shader object; its docstring has the
//! layouts by Unity version): every Vulkan subprogram's entry translated by shaderx::vulkan
//! (pmspirvcross) and stored as Unity stores GL programs of the data's version, the program's
//! shared parameters put back into its Vulkan entries.
use super::d3d11::{
    add_platform, bad, entry_of, gl_names, gl_twin, int, list_at, name_indices, pass_mut,
    pass_names, sp_type, strip_gles, tier_lists, variant_key, with_common, ListAt, VariantKey,
    INSTANCING_KEYWORDS, SKINNING_KEYWORDS, VR_KEYWORDS,
};
use super::fusedpost::{truthy, unity_of};
use crate::error::{Error, Result};
use shaderx::bindings::{self, Bindings};
use shaderx::blob::{self, Entry, ShaderBlobs, Slot};
use shaderx::convert::{Report, Status, STAGES};
use shaderx::dxbc;
use shaderx::tree;
use shaderx::vulkan::{self as vk, Translator, GPU_SPIRV};
use std::collections::{BTreeSet, HashMap};
use unityfs::Value;

const STAGE_FIELDS: [&str; 5] = [
    "progVertex",
    "progFragment",
    "progGeometry",
    "progHull",
    "progDomain",
];

/// convert.subprogram_lists: every list of the shader with its parameter indices.
fn subprogram_lists(t: &Value) -> Vec<(Vec<Value>, Option<Vec<i64>>)> {
    let mut out = Vec::new();
    for ss in t
        .path(&["m_ParsedForm", "m_SubShaders"])
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
    {
        for p in ss
            .get("m_Passes")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
        {
            for st in STAGES {
                let Some(prog) = p.get(st).filter(|x| truthy(Some(x))) else {
                    continue;
                };
                if let Some(Value::Array(l)) = prog.get("m_SubPrograms") {
                    out.push((l.clone(), None));
                }
                if truthy(prog.get("m_PlayerSubPrograms")) {
                    let pbis = prog
                        .get("m_ParameterBlobIndices")
                        .and_then(Value::as_array)
                        .cloned()
                        .unwrap_or_default();
                    for (k, l) in prog
                        .get("m_PlayerSubPrograms")
                        .and_then(Value::as_array)
                        .unwrap()
                        .iter()
                        .enumerate()
                    {
                        let pbi = super::d3d11::ints(pbis.get(k));
                        out.push((l.as_array().cloned().unwrap_or_default(), Some(pbi)));
                    }
                }
            }
        }
    }
    out
}

/// (program entry indices, parameter entry indices) the Vulkan subprograms name.
fn vulkan_indices(t: &Value) -> (BTreeSet<i64>, BTreeSet<i64>) {
    let (mut progs, mut params) = (BTreeSet::new(), BTreeSet::new());
    for (lst, pbi) in subprogram_lists(t) {
        for (i, sp) in lst.iter().enumerate() {
            if sp_type(sp) == GPU_SPIRV {
                if let Some(b) = sp.get("m_BlobIndex").and_then(Value::as_i64) {
                    progs.insert(b);
                }
                if let Some(p) = pbi.as_ref().and_then(|p| p.get(i)) {
                    params.insert(*p);
                }
            }
        }
    }
    (progs, params)
}

struct Todo {
    si: usize,
    pi: usize,
    stage: &'static str,
    tier: Option<usize>,
    sp: Value,
    pidx: Option<i64>,
    res: Option<(String, Option<Bindings>, i64)>,
}

type Cached = std::result::Result<(String, Option<Bindings>, i64), String>;

fn source_map(e: &Entry) -> i32 {
    if e.rest.len() >= 4 {
        i32::from_le_bytes(e.rest[..4].try_into().unwrap())
    } else {
        0
    }
}

/// vulkan.add_gles3_vulkan: GLES3 programs translated from the shader's Vulkan ones (tree edited
/// in place). unity: the file's version string.
#[allow(clippy::type_complexity, clippy::map_entry)]
pub fn add_gles3_vulkan(
    t: &mut Value,
    tr: &mut Translator,
    precision: &str,
    replace: bool,
    unity: &str,
) -> Result<Report> {
    let name = t
        .path(&["m_ParsedForm", "m_Name"])
        .and_then(Value::as_bytes)
        .map(|b| String::from_utf8_lossy(b).into_owned())
        .ok_or_else(|| bad("KeyError: 'm_Name'"))?;
    let mut r = Report::new(&name, false);
    r.source = "vulkan";
    let plats = super::d3d11::ints(t.get("platforms"));
    if !plats.contains(&blob::PLATFORM_VULKAN) {
        r.why = "no Vulkan programs".into();
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
    let (progs, params) = vulkan_indices(t);
    let mut entries = blobs
        .entries(blob::PLATFORM_VULKAN, &params, Some(&progs))
        .map_err(|e| bad(e.to_string()))?;
    let serializer = entries.iter().find_map(|e| match e {
        Some(Slot::Program(e)) => Some(e.version),
        _ => None,
    });
    if serializer.is_some_and(|s| s >= bindings::TYPED_FROM) {
        // (6000.6's typed binding info: read by the Python's Vulkan path only)
        return Err(Error::NotPorted(
            "shaders from Vulkan of Unity 6000.6 (typed binding info)".into(),
        ));
    }
    let mut line = unity_of(t, unity);
    if serializer.is_some() && line < dxbc::unity_line(serializer) {
        line = dxbc::unity_line(serializer);
    }
    if progs.is_empty() && line >= (2018, 1) {
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
    let separate = serializer == Some(bindings::SEPARATE_PARAMS_FROM)
        && subprogram_lists(t).iter().any(|(_, p)| p.is_some());
    let mut cache: HashMap<(i64, Option<i64>), Cached> = HashMap::new();
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
        for (pi_, p) in ss
            .get("m_Passes")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .enumerate()
        {
            let names = pass_names(p);
            for st in STAGE_FIELDS {
                let prog = p.get(st);
                for (tier, lst, pbi) in tier_lists(prog) {
                    for (j, sp) in lst.iter().enumerate() {
                        if sp_type(sp) != GPU_SPIRV {
                            continue;
                        }
                        let bi = int(sp, "m_BlobIndex")?;
                        let e = entry_of(&entries, bi)?;
                        let pidx = pbi.as_ref().map(|p| p.get(j).copied().unwrap_or(0));
                        let kws: Vec<String> = e
                            .map(|e| {
                                e.keywords
                                    .iter()
                                    .chain(e.local_keywords.iter().flatten())
                                    .cloned()
                                    .collect()
                            })
                            .unwrap_or_default();
                        let mut res = None;
                        let err: Option<String> =
                            if matches!(st, "progGeometry" | "progHull" | "progDomain") {
                                Some("geometry or tessellation stage (not in GLSL ES 3.00)".into())
                            } else if let Some(e) = e {
                                if e.code.is_empty() {
                                    if st == "progFragment" && !compact {
                                        None
                                    } else {
                                        Some("empty program entry".into())
                                    }
                                } else {
                                    let pe = match pidx {
                                        Some(i) => usize::try_from(i)
                                            .ok()
                                            .and_then(|i| entries.get(i))
                                            .ok_or_else(|| {
                                                bad("IndexError: list index out of range")
                                            })?
                                            .clone(),
                                        None => None,
                                    };
                                    let pdata = match &pe {
                                        Some(Slot::Params { data, .. }) => Some(data.clone()),
                                        _ => None,
                                    };
                                    let b = if separate && pdata.is_none() {
                                        Err("no parameter entry".to_string())
                                    } else {
                                        match bindings::entry_bindings(e, pdata.as_deref()) {
                                            Err(u) => Err(u.0),
                                            Ok(b) => with_common(
                                                b,
                                                prog.and_then(|p| p.get("m_CommonParameters")),
                                                &names,
                                            )?,
                                        }
                                    };
                                    match b {
                                        Err(m) => Some(m),
                                        Ok(b) => {
                                            let key = (bi, pidx);
                                            if !cache.contains_key(&key) {
                                                r.guessed_layout |= e.guessed;
                                                let c = match vk::translate_program(
                                                    tr,
                                                    &e.code,
                                                    &b,
                                                    serializer,
                                                    line,
                                                    precision,
                                                    &mut r.notes,
                                                ) {
                                                    Err(u) => Err(u.msg),
                                                    Ok((es, typ)) => {
                                                        let info = if gl_info {
                                                            bindings::gl_bindings(
                                                                &es,
                                                                line,
                                                                b.source_map,
                                                                None,
                                                            )
                                                            .map(Some)
                                                        } else {
                                                            Ok(None)
                                                        };
                                                        match info {
                                                            Err(u) => Err(u.0),
                                                            Ok(info) => Ok((
                                                                es,
                                                                info,
                                                                if typ == blob::GPU_GLES31 {
                                                                    blob::GPU_GLES31
                                                                } else {
                                                                    blob::GPU_GLES3
                                                                },
                                                            )),
                                                        }
                                                    }
                                                };
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
                            } else {
                                Some("no program entry".into())
                            };
                        match err {
                            None => todo.push(Todo {
                                si,
                                pi: pi_,
                                stage: st,
                                tier,
                                sp: sp.clone(),
                                pidx,
                                res,
                            }),
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
            "no Vulkan vertex/fragment programs".into()
        };
        r.changed = replace && had_gles3;
        return Ok(r);
    }
    let mut twins: Vec<(ListAt, Value, Option<usize>)> = Vec::new();
    if !compact {
        let mut out: Vec<Option<Slot>> = entries
            .iter()
            .map(|e| match e {
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
                _ => None,
            })
            .collect();
        let mut raised: HashMap<((usize, usize), VariantKey), i64> = HashMap::new();
        for (_, todo) in &plan {
            for td in todo {
                let Some((es, info, typ)) = &td.res else {
                    continue;
                };
                let i = int(&td.sp, "m_BlobIndex")? as usize;
                let e = entry_of(&entries, i as i64)?.unwrap();
                let empty = matches!(&out[i], Some(Slot::Program(o)) if o.code.is_empty());
                if empty {
                    let rest = match info {
                        Some(info) => info.serialize(),
                        None => bindings::gl_rest(source_map(e)),
                    };
                    out[i] = Some(Slot::Program(Entry {
                        version: e.version,
                        gpu_type: *typ as i32,
                        stats: vec![0; e.stats.len()],
                        keywords: e.keywords.clone(),
                        local_keywords: e.local_keywords.clone(),
                        code: es.clone().into_bytes(),
                        rest,
                        guessed: false,
                    }));
                    r.programs += 1;
                    r.es31 += (*typ == blob::GPU_GLES31) as usize;
                }
                raised.insert(((td.si, td.pi), variant_key(&td.sp)), *typ);
            }
        }
        for (_, todo) in &plan {
            for td in todo {
                let typ = *raised
                    .get(&((td.si, td.pi), variant_key(&td.sp)))
                    .unwrap_or(&blob::GPU_GLES3);
                let i = int(&td.sp, "m_BlobIndex")? as usize;
                if td.res.is_none() && typ == blob::GPU_GLES31 {
                    if let Some(Some(Slot::Program(o))) = out.get_mut(i) {
                        o.gpu_type = typ as i32;
                    }
                }
                let info = td
                    .res
                    .as_ref()
                    .and_then(|r| if gl_info { r.1.clone() } else { None });
                let tw = match &info {
                    Some(info) => {
                        let p = pass_mut(t, td.si, td.pi)?;
                        let names = name_indices(p, &gl_names(info))?;
                        gl_twin(&td.sp, i, Some((info, &names)), typ)?
                    }
                    None => gl_twin(&td.sp, i, None, typ)?,
                };
                twins.push((
                    ListAt {
                        si: td.si,
                        pi: td.pi,
                        stage: td.stage,
                        tier: td.tier,
                    },
                    tw,
                    None,
                ));
            }
        }
        let segs = if blobs.nested {
            blobs
                .segments(blob::PLATFORM_VULKAN)
                .map_err(|e| bad(e.to_string()))?
        } else {
            Vec::new()
        };
        blobs.set_entries(blob::PLATFORM_GLES3PLUS, &out, &segs, None);
    } else {
        let mut out: Vec<Option<Slot>> = Vec::new();
        let mut segs: Vec<i64> = Vec::new();
        let vk_segs = if blobs.nested {
            blobs
                .segments(blob::PLATFORM_VULKAN)
                .map_err(|e| bad(e.to_string()))?
        } else {
            vec![0; entries.len()]
        };
        let mut code_at: HashMap<(i64, Option<i64>), usize> = HashMap::new();
        let mut param_at: HashMap<Vec<u8>, usize> = HashMap::new();
        for (_, todo) in &plan {
            for td in todo {
                let (es, info, typ) = td
                    .res
                    .as_ref()
                    .ok_or_else(|| bad("TypeError: cannot unpack None"))?;
                let info = info.as_ref().ok_or_else(|| bad("no GL binding info"))?;
                let i = int(&td.sp, "m_BlobIndex")?;
                let e = entry_of(&entries, i)?.unwrap();
                let key = (i, td.pidx);
                if !code_at.contains_key(&key) {
                    code_at.insert(key, out.len());
                    out.push(Some(Slot::Program(Entry {
                        version: e.version,
                        gpu_type: *typ as i32,
                        stats: vec![0; e.stats.len()],
                        keywords: e.keywords.clone(),
                        local_keywords: e.local_keywords.clone(),
                        code: es.clone().into_bytes(),
                        rest: if separate {
                            info.head()
                        } else {
                            info.serialize()
                        },
                        guessed: false,
                    })));
                    segs.push(vk_segs[i as usize]);
                    r.programs += 1;
                    r.es31 += (*typ == blob::GPU_GLES31) as usize;
                }
                let mut gp = None;
                if separate {
                    let data = info.params_bytes();
                    if !param_at.contains_key(&data) {
                        param_at.insert(data.clone(), out.len());
                        out.push(Some(Slot::Params {
                            version: e.version,
                            data: data.clone(),
                        }));
                        segs.push(vk_segs[i as usize]);
                    }
                    gp = Some(param_at[&data]);
                }
                let tw = gl_twin(&td.sp, code_at[&key], None, *typ)?;
                twins.push((
                    ListAt {
                        si: td.si,
                        pi: td.pi,
                        stage: td.stage,
                        tier: td.tier,
                    },
                    tw,
                    gp,
                ));
            }
        }
        restore_common(t, &plan, &mut entries, &mut blobs, separate)?;
        let sc = blobs.stage_counts.is_some().then_some(1);
        blobs.set_entries(blob::PLATFORM_GLES3PLUS, &out, &segs, sc);
    }
    let f = blobs.store().map_err(|e| bad(e.to_string()))?;
    tree::set_blob(t, &f).map_err(bad)?;
    let mut seen: BTreeSet<(ListAt, i64, VariantKey)> = BTreeSet::new();
    for (at, tw, gp) in twins {
        let key = (at, int(&tw, "m_BlobIndex")?, variant_key(&tw));
        if !seen.insert(key) {
            continue;
        }
        let (lst, pbi) = list_at(t, at)?;
        lst.push(tw);
        if let Some(pbi) = pbi {
            pbi.push(Value::I64(gp.map_or(-1, |x| x as i64)));
        }
        r.twins += 1;
    }
    for (_, todo) in &plan {
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
    Ok(r)
}

/// vulkan._restore_common: the programs given GLES3 subprograms get their m_CommonParameters put
/// back into their Vulkan entries, and cleared.
fn restore_common(
    t: &mut Value,
    plan: &[(usize, Vec<Todo>)],
    entries: &mut [Option<Slot>],
    blobs: &mut ShaderBlobs,
    separate: bool,
) -> Result<()> {
    let mut progs: Vec<(usize, usize, &'static str)> = Vec::new();
    for (_, todo) in plan {
        for td in todo {
            if !progs.contains(&(td.si, td.pi, td.stage)) {
                progs.push((td.si, td.pi, td.stage));
            }
        }
    }
    let mut changed = false;
    for (si, pi, st) in progs {
        let p = pass_mut(t, si, pi)?;
        let names = pass_names(p);
        let Some(prog) = p.get(st).cloned() else {
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
                if sp_type(sp) != GPU_SPIRV {
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
        let p = pass_mut(t, si, pi)?;
        if let Some(Value::Struct(c)) = p.path_mut(&[st, "m_CommonParameters"]) {
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
            blobs
                .segments(blob::PLATFORM_VULKAN)
                .map_err(|e| bad(e.to_string()))?
        } else {
            Vec::new()
        };
        blobs.set_entries(blob::PLATFORM_VULKAN, entries, &segs, None);
    }
    Ok(())
}

/// The step's summary line for shaders translated from Vulkan (None without any).
pub fn summary(results: &[Report]) -> Option<String> {
    let v: Vec<&Report> = results.iter().filter(|r| r.source == "vulkan").collect();
    if v.is_empty() {
        return None;
    }
    let conv = |r: &&&Report| r.status == Status::Converted;
    Some(format!(
        "shaders: from Vulkan: {} of {} shaders ({} programs); {} SubShaders left out; {} VR-only and {} GPU instancing variants left out",
        v.iter().filter(conv).count(),
        v.len(),
        v.iter().filter(conv).map(|r| r.programs).sum::<usize>(),
        v.iter().map(|r| r.failed.len()).sum::<usize>(),
        v.iter().map(|r| r.dropped).sum::<usize>(),
        v.iter().map(|r| r.dropped_instancing).sum::<usize>(),
    ))
}
