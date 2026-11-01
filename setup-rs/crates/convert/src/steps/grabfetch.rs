//! `shaders.grab_fetch` on one Shader object's tree (shaders/grabfetch.py's `apply`): each
//! SubShader whose unnamed GrabPasses each come right before the pass that samples
//! `_GrabTexture`, at the fragment's screen position in every GLES3 program of it, gets a copy put
//! before it: without the GrabPasses, only the GLES3 / GLES 3.1 subprograms, the reading pass's
//! programs as new GLES3 blob entries that read the framebuffer (shaderx::grabfetch). Mode `true`
//! makes the copy's reading subprograms require framebuffer fetch; "always" does not.
use super::{field, field_mut};
use crate::error::{Error, Result};
use shaderx::blob::{self, Entry, Slot};
use shaderx::convert::STAGES;
use shaderx::grabfetch::{self as gf, Fail};
use shaderx::tree;
use std::collections::{BTreeMap, HashMap};
use unityfs::Value;

const GRAB_PASS: i128 = 2; // SerializedPass m_Type: 0 normal, 1 use, 2 grab
const FBFETCH: i128 = 0x80000; // ShaderRequirements: framebufferfetch
const GLES3_TYPES: [i128; 2] = [blob::GPU_GLES3 as i128, blob::GPU_GLES31 as i128];
/// One shader's outcome (grabfetch.Result).
#[derive(Clone, Debug)]
pub struct GfResult {
    pub name: String,
    /// "changed", "unchanged" or "skipped" (a GrabPass, but no SubShader qualifies).
    pub status: &'static str,
    pub why: String,
    pub subshaders: usize,
    pub grabs: usize,
    pub programs: usize,
}

impl GfResult {
    pub fn changed(&self) -> bool {
        self.status == "changed"
    }
}

impl std::fmt::Display for GfResult {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let s = format!("{}: {} {}", self.name, self.status, self.why);
        f.write_str(shaderx::py::rstrip(&s))
    }
}

fn arr<'a>(v: &'a Value, what: &str) -> Result<&'a Vec<Value>> {
    v.as_array()
        .ok_or_else(|| Error::Value(format!("{what}: not a list")))
}

fn arr_mut<'a>(v: &'a mut Value, what: &str) -> Result<&'a mut Vec<Value>> {
    v.as_array_mut()
        .ok_or_else(|| Error::Value(format!("{what}: not a list")))
}

fn int_of(v: &Value, what: &str) -> Result<i128> {
    v.as_int()
        .ok_or_else(|| Error::Value(format!("{what}: not an integer")))
}

fn sp_type(sp: &Value) -> Result<i128> {
    int_of(field(sp, "m_GpuProgramType")?, "m_GpuProgramType")
}

fn sp_index(sp: &Value) -> Result<i128> {
    int_of(field(sp, "m_BlobIndex")?, "m_BlobIndex")
}

/// Python's truth value of a field (a missing one: false).
fn truthy(v: Option<&Value>) -> bool {
    match v {
        None => false,
        Some(Value::Array(a)) => !a.is_empty(),
        Some(Value::Struct(s)) => !s.fields.is_empty(),
        Some(Value::Str(b) | Value::Bytes(b)) => !b.is_empty(),
        Some(v) => v.as_int().is_none_or(|i| i != 0),
    }
}

/// convert._tier_lists: a stage program's subprogram lists (2021.2+: one per tier).
fn tier_lists(prog: Option<&Value>) -> Result<Vec<&Vec<Value>>> {
    let Some(prog) = prog.filter(|p| truthy(Some(p))) else {
        return Ok(Vec::new());
    };
    if truthy(prog.get("m_PlayerSubPrograms")) {
        // (zip with m_ParameterBlobIndices: as many lists as both have)
        let n = arr(
            field(prog, "m_ParameterBlobIndices")?,
            "m_ParameterBlobIndices",
        )?
        .len();
        return arr(field(prog, "m_PlayerSubPrograms")?, "m_PlayerSubPrograms")?
            .iter()
            .take(n)
            .map(|l| arr(l, "m_PlayerSubPrograms[]"))
            .collect();
    }
    match prog.get("m_SubPrograms") {
        Some(l) if truthy(Some(l)) => Ok(vec![arr(l, "m_SubPrograms")?]),
        _ => Ok(Vec::new()),
    }
}

fn entry(entries: &[Option<Slot>], i: i128) -> Option<&Entry> {
    match usize::try_from(i).ok().and_then(|i| entries.get(i)) {
        Some(Some(Slot::Program(e))) => Some(e),
        _ => None,
    }
}

/// _gles_programs: {blob index: code} of a pass's GLES3 / GLES 3.1 subprograms' code entries.
fn gles_programs(p: &Value, entries: &[Option<Slot>]) -> Result<BTreeMap<i128, String>> {
    let mut out = BTreeMap::new();
    for st in STAGES {
        for lst in tier_lists(p.get(st))? {
            for sp in lst {
                if !GLES3_TYPES.contains(&sp_type(sp)?) {
                    continue;
                }
                let i = sp_index(sp)?;
                if let Some(e) = entry(entries, i).filter(|e| !e.code.is_empty()) {
                    let code = String::from_utf8(e.code.clone()).map_err(|_| {
                        Error::Value("UnicodeDecodeError: a program entry is not UTF-8".into())
                    })?;
                    out.insert(i, code);
                }
            }
        }
    }
    Ok(out)
}

fn has_gles(p: &Value) -> Result<bool> {
    for st in STAGES {
        for lst in tier_lists(p.get(st))? {
            for sp in lst {
                if GLES3_TYPES.contains(&sp_type(sp)?) {
                    return Ok(true);
                }
            }
        }
    }
    Ok(false)
}

/// The same integer kind with another value (the type tree writes any kind at its width).
fn int_value(v: i128) -> Value {
    Value::I64(v as i64)
}

/// _gles_only: a pass (a copy, edited in place) with only its GLES3 / GLES 3.1 subprograms;
/// those whose entry was rewritten point to the new entry and, with require, ask for
/// framebuffer fetch.
fn gles_only(p: &mut Value, rewritten: Option<&HashMap<i128, usize>>, require: bool) -> Result<()> {
    for st in STAGES {
        let Some(prog) = p.get_mut(st) else { continue };
        if !truthy(Some(prog)) {
            continue;
        }
        if prog.get("m_SubPrograms").is_some() {
            let l = arr_mut(field_mut(prog, "m_SubPrograms")?, "m_SubPrograms")?;
            let mut keep = Vec::with_capacity(l.len());
            for sp in l.drain(..) {
                if GLES3_TYPES.contains(&sp_type(&sp)?) {
                    keep.push(sp);
                }
            }
            *l = keep;
        }
        if truthy(prog.get("m_PlayerSubPrograms")) {
            let n = arr(field(prog, "m_PlayerSubPrograms")?, "m_PlayerSubPrograms")?.len();
            for tier in 0..n {
                let lst =
                    arr(field(prog, "m_PlayerSubPrograms")?, "m_PlayerSubPrograms")?[tier].clone();
                let lst = arr(&lst, "m_PlayerSubPrograms[]")?.clone();
                let mut keep = Vec::new();
                for (i, sp) in lst.iter().enumerate() {
                    if GLES3_TYPES.contains(&sp_type(sp)?) {
                        keep.push(i);
                    }
                }
                let pbis = arr_mut(
                    field_mut(prog, "m_ParameterBlobIndices")?,
                    "m_ParameterBlobIndices",
                )?;
                let slot = pbis
                    .get_mut(tier)
                    .ok_or_else(|| Error::Value("IndexError: list index out of range".into()))?;
                let pbi = arr(slot, "m_ParameterBlobIndices[]")?.clone();
                let kept: Vec<Value> = keep
                    .iter()
                    .map(|&i| {
                        pbi.get(i).cloned().ok_or_else(|| {
                            Error::Value("IndexError: list index out of range".into())
                        })
                    })
                    .collect::<Result<_>>()?;
                *slot = Value::Array(kept);
                let players = arr_mut(
                    field_mut(prog, "m_PlayerSubPrograms")?,
                    "m_PlayerSubPrograms",
                )?;
                players[tier] = Value::Array(keep.iter().map(|&i| lst[i].clone()).collect());
            }
        }
        let Some(rw) = rewritten else { continue };
        // (_tier_lists: the tier lists when there are any, else m_SubPrograms)
        let lists: Vec<&mut Value> = if truthy(prog.get("m_PlayerSubPrograms")) {
            arr_mut(
                field_mut(prog, "m_PlayerSubPrograms")?,
                "m_PlayerSubPrograms",
            )?
            .iter_mut()
            .collect()
        } else {
            match prog.get_mut("m_SubPrograms") {
                Some(l) => vec![l],
                None => Vec::new(),
            }
        };
        for lst in lists {
            for sp in arr_mut(lst, "subprograms")? {
                let i = sp_index(sp)?;
                if let Some(&new) = rw.get(&i) {
                    sp.set("m_BlobIndex", int_value(new as i128))?;
                }
                if require {
                    let r = int_of(field(sp, "m_ShaderRequirements")?, "m_ShaderRequirements")?;
                    sp.set("m_ShaderRequirements", int_value(r | FBFETCH))?;
                }
            }
        }
    }
    if matches!(p.get("m_Platforms"), Some(Value::Array(_))) {
        p.set(
            "m_Platforms",
            Value::Array(vec![int_value(blob::PLATFORM_GLES3PLUS as i128)]),
        )?;
    }
    Ok(())
}

fn not_sp(m: String) -> Fail {
    Fail::Not(m)
}

/// grabfetch.apply: the tree edited in place; `always` is mode "always" (else true: the copies
/// require framebuffer fetch); `names`: only shaders whose names match.
pub fn apply(t: &mut Value, always: bool, names: Option<&regex::Regex>) -> Result<GfResult> {
    let pf = field(t, "m_ParsedForm")?;
    let name = String::from_utf8_lossy(&super::text(pf, "m_Name")?).into_owned();
    let mut r = GfResult {
        name,
        status: "unchanged",
        why: String::new(),
        subshaders: 0,
        grabs: 0,
        programs: 0,
    };
    let subs = arr(field(pf, "m_SubShaders")?, "m_SubShaders")?;
    let mut any_grab = false;
    for ss in subs {
        for p in arr(field(ss, "m_Passes")?, "m_Passes")? {
            if p.get("m_Type").and_then(Value::as_int) == Some(GRAB_PASS) {
                any_grab = true;
            }
        }
    }
    if !any_grab {
        r.why = "no GrabPass".into();
        return Ok(r);
    }
    if let Some(rx) = names {
        if !rx.is_match(&r.name) {
            r.why = "not in grab_fetch_names".into();
            return Ok(r);
        }
    }
    let platforms = arr(field(t, "platforms")?, "platforms")?;
    if !platforms
        .iter()
        .any(|p| p.as_int() == Some(blob::PLATFORM_GLES3PLUS as i128))
    {
        r.status = "skipped";
        r.why = "no GLES3 programs".into();
        return Ok(r);
    }
    let bad = |e: blob::BlobError| Error::Value(e.to_string());
    let mut blobs = tree::blobs(t).map_err(Error::Value)?;
    let entries =
        tree::platform_entries(t, blob::PLATFORM_GLES3PLUS, &blobs).map_err(Error::Value)?;
    let mark = gf::MARK.as_bytes();
    if entries.iter().any(
        |e| matches!(e, Some(Slot::Program(e)) if e.code.windows(mark.len()).any(|w| w == mark)),
    ) {
        r.why = "already reads the framebuffer (grab_fetch done before)".into();
        return Ok(r);
    }
    let mut out = entries.clone();
    let mut segs = blobs.segments(blob::PLATFORM_GLES3PLUS).map_err(bad)?;
    let mut done: HashMap<i128, usize> = HashMap::new(); // old entry -> new entry (one rewrite each)
    let mut plan: Vec<(usize, Value)> = Vec::new(); // (subshader index, its copy)
    let mut left: Vec<(usize, String)> = Vec::new();
    let subs = arr(
        field(field(t, "m_ParsedForm")?, "m_SubShaders")?,
        "m_SubShaders",
    )?;
    for (si, ss) in subs.iter().enumerate() {
        let passes = arr(field(ss, "m_Passes")?, "m_Passes")?;
        let grabs: Vec<usize> = passes
            .iter()
            .enumerate()
            .filter(|(_, p)| p.get("m_Type").and_then(Value::as_int) == Some(GRAB_PASS))
            .map(|(i, _)| i)
            .collect();
        if grabs.is_empty() {
            continue;
        }
        let mut readers: BTreeMap<usize, BTreeMap<i128, String>> = BTreeMap::new();
        let attempt = (|| -> Result<std::result::Result<HashMap<i128, usize>, Fail>> {
            let named: Vec<String> = grabs
                .iter()
                .filter_map(|&pi| match passes[pi].get("m_TextureName") {
                    Some(Value::Str(b)) if !b.is_empty() => {
                        Some(String::from_utf8_lossy(b).into_owned())
                    }
                    _ => None,
                })
                .collect();
            if let Some(n) = named.first() {
                return Ok(Err(not_sp(format!(
                    "named GrabPass {} (copied once a frame: not converted)",
                    shaderx::py::repr(n)
                ))));
            }
            for (pi, p) in passes.iter().enumerate() {
                if p.get("m_Type").and_then(Value::as_int) == Some(GRAB_PASS) {
                    continue;
                }
                if !has_gles(p)? {
                    return Ok(Err(not_sp(format!("pass {pi} has no GLES3 programs"))));
                }
                let progs = gles_programs(p, &entries)?;
                if progs.values().any(|c| gf::reads_grab(c)) {
                    readers.insert(pi, progs);
                }
            }
            // each reading pass right after an unnamed GrabPass; a GrabPass no pass reads after
            // it is left out as it is
            for &pi in readers.keys() {
                if pi == 0
                    || passes[pi - 1].get("m_Type").and_then(Value::as_int) != Some(GRAB_PASS)
                {
                    return Ok(Err(not_sp(format!(
                        "pass {pi} samples {} without a GrabPass right before it",
                        gf::GRAB_TEXTURE
                    ))));
                }
            }
            if !always {
                for &pi in readers.keys() {
                    for st in STAGES {
                        for lst in tier_lists(passes[pi].get(st))? {
                            if lst
                                .iter()
                                .any(|sp| sp.get("m_ShaderRequirements").is_none())
                            {
                                return Ok(Err(not_sp(
                                    "subprograms without m_ShaderRequirements (this Unity version): use \"always\""
                                        .into(),
                                )));
                            }
                        }
                    }
                }
            }
            let mut rewritten = HashMap::new();
            for (&pi, progs) in &readers {
                for (&i, code) in progs {
                    if !gf::reads_grab(code) {
                        continue; // (a variant that does not sample it: as it is)
                    }
                    if let std::collections::hash_map::Entry::Vacant(slot) = done.entry(i) {
                        let new = match gf::rewrite_program(code) {
                            Ok(n) => n,
                            Err(Fail::Not(why)) => {
                                let e = entry(&entries, i).unwrap();
                                let kws = if e.keywords.is_empty() {
                                    "(no keywords)".to_string()
                                } else {
                                    e.keywords.join(" ")
                                };
                                return Ok(Err(not_sp(format!(
                                    "pass {pi} program {i} ({kws}): {why}"
                                ))));
                            }
                            Err(Fail::Crash(m)) => return Err(Error::Value(m)),
                        };
                        let mut e = entry(&out, i).unwrap().clone();
                        e.code = new.into_bytes();
                        slot.insert(out.len());
                        out.push(Some(Slot::Program(e)));
                        segs.push(segs[i as usize]);
                        r.programs += 1;
                    }
                    rewritten.insert(i, done[&i]);
                }
            }
            Ok(Ok(rewritten))
        })()?;
        let rewritten = match attempt {
            Ok(rw) => rw,
            Err(f) => {
                left.push((si, f.to_string()));
                continue;
            }
        };
        let mut cp = ss.clone();
        let mut new_passes = Vec::new();
        for (pi, p) in passes.iter().enumerate() {
            if grabs.contains(&pi) {
                continue;
            }
            let mut p = p.clone();
            if readers.contains_key(&pi) {
                gles_only(&mut p, Some(&rewritten), !always)?;
            } else {
                gles_only(&mut p, None, false)?;
            }
            new_passes.push(p);
        }
        cp.set("m_Passes", Value::Array(new_passes))?;
        plan.push((si, cp));
        r.grabs += grabs.len();
    }
    let lefts = || {
        left.iter()
            .map(|(si, w)| format!("SubShader {si}: {w}"))
            .collect::<Vec<_>>()
            .join("; ")
    };
    if plan.is_empty() {
        r.status = "skipped";
        r.why = lefts();
        return Ok(r);
    }
    blobs.set_entries(blob::PLATFORM_GLES3PLUS, &out, &segs, None);
    let fields = blobs.store().map_err(bad)?;
    tree::set_blob(t, &fields).map_err(Error::Value)?;
    let subs = arr_mut(
        field_mut(field_mut(t, "m_ParsedForm")?, "m_SubShaders")?,
        "m_SubShaders",
    )?;
    plan.sort_by(|a, b| b.0.cmp(&a.0));
    let n = plan.len();
    for (si, cp) in plan {
        subs.insert(si, cp);
    }
    r.status = "changed";
    r.subshaders = n;
    r.why = format!(
        "{n} SubShader(s) given a copy without their {} GrabPass(es), {} programs reading the framebuffer{}",
        r.grabs,
        r.programs,
        if !always && r.programs > 0 {
            "; they require framebuffer fetch"
        } else {
            ""
        }
    );
    if !left.is_empty() {
        r.why.push_str(&format!(
            "; {}",
            left.iter()
                .map(|(si, w)| format!("SubShader {si} left as it is: {w}"))
                .collect::<Vec<_>>()
                .join("; ")
        ));
    }
    Ok(r)
}

/// step.py's summary line for grab_fetch.
pub fn summary(results: &[GfResult], always: bool) -> String {
    let gc: Vec<&GfResult> = results.iter().filter(|g| g.changed()).collect();
    format!(
        "shaders: grab_fetch ({}): {} shaders read the framebuffer instead of a GrabPass copy ({} SubShaders, {} \
         GrabPasses left out, {} programs); {} shaders with GrabPasses left as they are",
        if always {
            "always"
        } else {
            "where Unity reports framebuffer fetch"
        },
        gc.len(),
        gc.iter().map(|g| g.subshaders).sum::<usize>(),
        gc.iter().map(|g| g.grabs).sum::<usize>(),
        gc.iter().map(|g| g.programs).sum::<usize>(),
        results.iter().filter(|g| g.status == "skipped").count()
    )
}
