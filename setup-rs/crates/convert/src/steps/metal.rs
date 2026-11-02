//! Shaders from Metal (macOS builds without OpenGL Core): metal.add_gles3_metal on the Shader
//! object (its docstring has the layouts by Unity version): every pass's Metal vertex subprograms
//! paired with the fragment subprogram of the same variant, as Direct3D 11's are, each pair's MSL
//! translated by shaderx::metal and stored as d3d11.rs stores GL programs; the vertex program's
//! shared parameters go back into its Metal entries.
use super::d3d11::{
    self, bad, entry_keywords, entry_of, int, keyset, sp_type, strip_gles, tier_lists, KeySet,
    Todo, INSTANCING_KEYWORDS, SKINNING_KEYWORDS, VR_KEYWORDS,
};
use super::fusedpost::unity_of;
use super::vulkan::subprogram_lists;
use crate::error::Result;
use shaderx::bindings::{self, Bindings};
use shaderx::blob::{self, Slot};
use shaderx::convert::{Report, Status};
use shaderx::dxbc;
use shaderx::metal::{self as mt, Fault};
use shaderx::tree;
use std::collections::{BTreeSet, HashMap};
use unityfs::Value;

const VS: i64 = blob::GPU_METAL_VS;
const FS: i64 = blob::GPU_METAL_FS;

type Cached = std::result::Result<(String, Option<Bindings>, i64), String>;

/// metal.add_gles3_metal: GLES3 programs translated from the shader's Metal ones (tree edited in
/// place). unity: the file's version string.
#[allow(clippy::type_complexity, clippy::map_entry)]
pub fn add_gles3_metal(
    t: &mut Value,
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
    r.source = "metal";
    let plats = d3d11::ints(t.get("platforms"));
    if !plats.contains(&blob::PLATFORM_METAL) {
        r.why = "no Metal programs".into();
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
    let lists = subprogram_lists(t);
    let (mut progs, mut params) = (BTreeSet::new(), BTreeSet::new());
    for (lst, pbi) in &lists {
        for (i, sp) in lst.iter().enumerate() {
            if [VS, FS].contains(&sp_type(sp)) {
                progs.insert(int(sp, "m_BlobIndex")?);
                if let Some(pbi) = pbi {
                    let p = pbi
                        .get(i)
                        .ok_or_else(|| bad("IndexError: list index out of range"))?;
                    params.insert(*p);
                }
            }
        }
    }
    let mut entries = blobs
        .entries(blob::PLATFORM_METAL, &params, Some(&progs))
        .map_err(|e| bad(e.to_string()))?;
    let serializer = entries.iter().find_map(|e| match e {
        Some(Slot::Program(e)) => Some(e.version),
        _ => None,
    });
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
        && lists.iter().any(|(_, pbi)| pbi.is_some());
    let mut cache: HashMap<(i64, i64), Cached> = HashMap::new();

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
            let vlists = tier_lists(p.get("progVertex"));
            let flists = tier_lists(p.get("progFragment"));
            for (tn, (_, vlist, vpbi)) in vlists.iter().enumerate() {
                let flist = flists.get(tn).map_or(Vec::new(), |(_, l, _)| l.clone());
                let vss: Vec<(Value, Option<i64>)> = vlist
                    .iter()
                    .enumerate()
                    .filter(|(_, sp)| sp_type(sp) == VS)
                    .map(|(j, sp)| {
                        (
                            sp.clone(),
                            vpbi.as_ref().map(|p| p.get(j).copied().unwrap_or(0)),
                        )
                    })
                    .collect();
                let pss: Vec<Value> = flist.into_iter().filter(|sp| sp_type(sp) == FS).collect();
                // (convert._variant_pairs)
                let vuse: KeySet = vss.iter().flat_map(|(sp, _)| keyset(sp)).collect();
                let puse: KeySet = pss.iter().flat_map(keyset).collect();
                for (vsp, vpi) in &vss {
                    let a = keyset(vsp);
                    let tier = vsp.get("m_ShaderHardwareTier").and_then(Value::as_int);
                    let mut pairs: Vec<(Option<&Value>, KeySet)> = Vec::new();
                    for psp in &pss {
                        let b = keyset(psp);
                        if psp.get("m_ShaderHardwareTier").and_then(Value::as_int) == tier
                            && b.intersection(&vuse).all(|k| a.contains(k))
                            && a.intersection(&puse).all(|k| b.contains(k))
                        {
                            pairs.push((Some(psp), a.union(&b).cloned().collect()));
                        }
                    }
                    if pairs.is_empty() {
                        pairs.push((None, a.clone()));
                    }
                    for (psp, keys) in pairs {
                        let vi = int(vsp, "m_BlobIndex")?;
                        let ve = entry_of(&entries, vi)?;
                        let (fe, fi) = match psp {
                            Some(psp) => {
                                let i = int(psp, "m_BlobIndex")?;
                                (entry_of(&entries, i)?, Some(i))
                            }
                            None => (None, None),
                        };
                        let (k0, k1) = entry_keywords(ve, fe);
                        let mut kws = k0;
                        kws.extend(k1.unwrap_or_default());
                        let mut res: Option<(String, Option<Bindings>, i64)> = None;
                        let err = match fi {
                            None => Some("no fragment shader for this variant".to_string()),
                            Some(fi) => {
                                if !cache.contains_key(&(vi, fi)) {
                                    let c = program(ve, fe, precision, line, gl_info, &mut r)?;
                                    cache.insert((vi, fi), c);
                                }
                                match &cache[&(vi, fi)] {
                                    Ok(x) => {
                                        res = Some(x.clone());
                                        None
                                    }
                                    Err(m) => Some(m.clone()),
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
                                    psp: psp.unwrap().clone(),
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
            "no Metal vertex/fragment programs".into()
        };
        r.changed = replace && had_gles3;
        return Ok(r);
    }
    d3d11::store(
        t,
        &mut r,
        &plan,
        &mut entries,
        &mut blobs,
        &subs,
        &no_instancing,
        compact,
        separate,
        blob::PLATFORM_METAL,
    )?;
    Ok(r)
}

/// One GL program from a vertex and a fragment entry (add_gles3_metal's program()): (GLSL ES, GL
/// binding info, type) or why not. The translator's notes go to the report however it ends, as
/// the Python passes it r.notes.
fn program(
    ve: Option<&shaderx::blob::Entry>,
    fe: Option<&shaderx::blob::Entry>,
    precision: &str,
    line: (u64, u64),
    gl_info: bool,
    r: &mut Report,
) -> Result<Cached> {
    let (Some(ve), Some(fe)) = (
        ve.filter(|e| !e.code.is_empty()),
        fe.filter(|e| !e.code.is_empty()),
    ) else {
        return Ok(Err("empty program entry".into()));
    };
    r.guessed_layout |= ve.guessed || fe.guessed;
    let fault = |f: Fault| -> Result<Cached> {
        match f {
            Fault::Uns(m) | Fault::Es31(m) => Ok(Err(m)),
            Fault::Crash(m) => Err(bad(m)),
        }
    };
    let vs = match mt::msl_of(&ve.code) {
        Ok(s) => s,
        Err(f) => return fault(f),
    };
    let fs = match mt::msl_of(&fe.code) {
        Ok(s) => s,
        Err(f) => return fault(f),
    };
    let mut notes = Vec::new();
    let got = mt::translate_program(&vs, &fs, precision, line, &mut notes);
    r.notes.extend(notes);
    let (text, typ) = match got {
        Ok(x) => x,
        Err(f) => return fault(f),
    };
    let mut info = None;
    if gl_info {
        match bindings::gl_bindings(&text, line, d3d11::source_map(ve), None) {
            Ok(b) => info = Some(b),
            Err(u) => return Ok(Err(u.0)),
        }
    }
    let typ = if typ == blob::GPU_GLES31 {
        blob::GPU_GLES31
    } else {
        blob::GPU_GLES3
    };
    Ok(Ok((text, info, typ)))
}

/// The step's summary line for shaders translated from Metal (None without any).
pub fn summary(results: &[Report]) -> Option<String> {
    let v: Vec<&Report> = results.iter().filter(|r| r.source == "metal").collect();
    if v.is_empty() {
        return None;
    }
    let conv = |r: &&&Report| r.status == Status::Converted;
    Some(format!(
        "shaders: from Metal: {} of {} shaders ({} programs); {} SubShaders left out; {} VR-only and {} GPU instancing variants left out",
        v.iter().filter(conv).count(),
        v.len(),
        v.iter().filter(conv).map(|r| r.programs).sum::<usize>(),
        v.iter().map(|r| r.failed.len()).sum::<usize>(),
        v.iter().map(|r| r.dropped).sum::<usize>(),
        v.iter().map(|r| r.dropped_instancing).sum::<usize>(),
    ))
}
