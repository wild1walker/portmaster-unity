//! GLES3 programs for a Shader that only carries OpenGL Core programs: shaders/convert.py's
//! `add_gles3` (its docstring has the rules) as a pure function of the Shader's fields the step
//! reads ([`Shader`]) to the fields it writes ([`Edit`]) and its report ([`Report`]).
//!
//! The GLCore platform blob is copied as the GLES3Plus blob (platform 9) with each entry a GLCore
//! subprogram uses translated to GLSL ES 3.00 (or 3.10) and retyped GLES3 (or GLES 3.1); every
//! GLCore subprogram gets a twin, a copy with the GLES type appended to its list; passes with
//! twins list platform 9 in m_Platforms (2020.1+). The Unity file layer reads a Shader object
//! into a [`Shader`] and writes an [`Edit`] back: each [`ListEdit`] is a subprogram list's new
//! contents as copies of its old subprograms, retyped.
use std::collections::{BTreeMap, BTreeSet, HashMap};

use crate::blob::{self, BlobError, BlobFields, Entry, ShaderBlobs, Slot};
use crate::glsl::{self, Es, Optimize, Unsupported};
use crate::py;

/// The stage programs of a pass, in the order the Python walks them.
pub const STAGES: [&str; 5] = [
    "progVertex",
    "progFragment",
    "progGeometry",
    "progHull",
    "progDomain",
];
/// GPU program types of the GLES platforms.
const GLES_TYPES: [i64; 4] = [
    blob::GPU_GLES31AEP,
    blob::GPU_GLES31,
    blob::GPU_GLES3,
    blob::GPU_GLES,
];
/// GLSL 4.x programs (`#pragma target 4.5` and up).
const GLCORE4X: [i64; 2] = [blob::GPU_GLCORE41, blob::GPU_GLCORE43];
/// The GPU program types whose entries are in the GLCore platform's blob.
const GLCORE_TYPES: [i64; 3] = [blob::GPU_GLCORE32, blob::GPU_GLCORE41, blob::GPU_GLCORE43];
/// Keywords of variants only VR rendering uses: one of these that cannot be translated is left out.
pub const VR_KEYWORDS: [&str; 6] = [
    "STEREO_INSTANCING_ON",
    "STEREO_MULTIVIEW_ON",
    "UNITY_SINGLE_PASS_STEREO",
    "STEREO_CUBEMAP_RENDER_ON",
    "STEREO_INSTANCING_ENABLED",
    "STEREO_DOUBLEWIDE_TARGET",
];

/// A SerializedSubProgram: the fields the step reads (a twin is a copy of the whole subprogram).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SubProgram {
    pub blob_index: i64,
    pub gpu_program_type: i64,
    /// m_KeywordIndices (up to 2018.4), m_GlobalKeywordIndices and m_LocalKeywordIndices
    /// (2019.1 - 2021.1): None where the subprogram has no such field.
    pub keyword_indices: Option<Vec<i64>>,
    pub global_keyword_indices: Option<Vec<i64>>,
    pub local_keyword_indices: Option<Vec<i64>>,
    /// m_ShaderHardwareTier, where it has one.
    pub hardware_tier: Option<i64>,
}

/// A pass's stage program (SerializedProgram): its subprogram lists.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct StageProgram {
    /// m_SubPrograms (up to 2021.1; also in later trees, empty there).
    pub sub_programs: Option<Vec<SubProgram>>,
    /// 2021.2+: m_PlayerSubPrograms, a list per hardware tier, and the parallel
    /// m_ParameterBlobIndices.
    pub player_sub_programs: Option<Vec<Vec<SubProgram>>>,
    pub parameter_blob_indices: Option<Vec<Vec<i64>>>,
}

/// A pass: m_Platforms (2020.1+) and its stage programs in [`STAGES`] order.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Pass {
    pub platforms: Option<Vec<i64>>,
    pub programs: [Option<StageProgram>; 5],
}

/// What the step reads of a Shader object.
#[derive(Clone, Debug, PartialEq)]
pub struct Shader {
    /// m_ParsedForm.m_Name
    pub name: String,
    pub blob: BlobFields,
    /// m_ParsedForm.m_SubShaders[].m_Passes
    pub sub_shaders: Vec<Vec<Pass>>,
}

/// A subprogram list: pass `pass` of SubShader `sub_shader`, stage program `stage` ([`STAGES`]),
/// m_SubPrograms (tier None) or m_PlayerSubPrograms[tier] (with m_ParameterBlobIndices[tier]).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ListRef {
    pub sub_shader: usize,
    pub pass: usize,
    pub stage: usize,
    pub tier: Option<usize>,
}

/// An element of a new subprogram list: a copy of the old list's subprogram `from`, with this
/// m_GpuProgramType.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Item {
    pub from: usize,
    pub gpu_program_type: i64,
}

/// A subprogram list's new contents (and, for a tier list, its m_ParameterBlobIndices[tier]).
#[derive(Clone, Debug, PartialEq)]
pub struct ListEdit {
    pub at: ListRef,
    pub items: Vec<Item>,
    pub parameter_blob_indices: Option<Vec<i64>>,
}

/// A pass's new m_Platforms.
#[derive(Clone, Debug, PartialEq)]
pub struct PassPlatforms {
    pub sub_shader: usize,
    pub pass: usize,
    pub platforms: Vec<i64>,
}

/// The fields the step writes back into the Shader object (the step then saves it).
#[derive(Clone, Debug, PartialEq)]
pub struct Edit {
    pub blob: BlobFields,
    /// The lists whose contents changed.
    pub lists: Vec<ListEdit>,
    /// The passes whose m_Platforms changed.
    pub pass_platforms: Vec<PassPlatforms>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    /// The tree changed (GLES3 programs added, or old ones removed).
    Converted,
    Unchanged,
    /// Not translatable (a GLCore32 program has no ES equivalent).
    Skipped,
}

impl Status {
    pub fn as_str(self) -> &'static str {
        match self {
            Status::Converted => "converted",
            Status::Unchanged => "unchanged",
            Status::Skipped => "skipped",
        }
    }
}

/// convert.Result: what happened to one shader (the step's log sums these).
#[derive(Clone, Debug, PartialEq)]
pub struct Report {
    pub name: String,
    pub status: Status,
    /// Skipped, but older GLES programs were removed (the tree is saved).
    pub changed: bool,
    pub why: String,
    /// Translated blob entries.
    pub programs: usize,
    /// GLES3 subprograms added.
    pub twins: usize,
    pub guessed_layout: bool,
    /// The rewrites applied, per stage translated ("FRAGMENT unroll 1").
    pub notes: Vec<String>,
    /// VR-only variants left out (not translatable).
    pub dropped: usize,
    /// Programs that need GLSL ES 3.10 (GLES 3.1 programs).
    pub es31: usize,
    /// Of the translated entries, GLCore41/43 ones.
    pub programs4x: usize,
    /// No programs at all: given an empty GLES3 blob.
    pub empty: bool,
    /// (SubShader, why) of SubShaders whose GLCore41/43 variants got no GLES3 programs.
    pub failed: Vec<(usize, String)>,
    /// Whether GLCore41/43 programs were translated (the step's glcore4x for the file).
    pub glcore4x: bool,
    /// The shader's platforms after the step (step.py sets r.platforms from the tree).
    pub platforms: Vec<i64>,
}

impl Report {
    fn new(name: &str, glcore4x: bool) -> Report {
        Report {
            name: name.to_string(),
            status: Status::Unchanged,
            changed: false,
            why: String::new(),
            programs: 0,
            twins: 0,
            guessed_layout: false,
            notes: Vec::new(),
            dropped: 0,
            es31: 0,
            programs4x: 0,
            empty: false,
            failed: Vec::new(),
            glcore4x,
            platforms: Vec::new(),
        }
    }
}

impl std::fmt::Display for Report {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(py::rstrip(&format!(
            "{}: {} {}",
            self.name,
            self.status.as_str(),
            self.why
        )))
    }
}

/// What makes the Python step stop on a shader (a blob it cannot read, an inconsistent tree).
#[derive(Clone, Debug, PartialEq)]
pub struct Error(pub String);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

impl From<BlobError> for Error {
    fn from(e: BlobError) -> Self {
        Error(e.0)
    }
}

/// The step's options for one file: `precision` ('highp', 'default-samplers'), `replace`
/// (rebuild GLES programs a shader already has), the rewrites, `glcore4x`.
#[derive(Clone, Debug, PartialEq)]
pub struct Options {
    pub precision: String,
    pub replace: bool,
    pub optimize: Optimize,
    pub glcore4x: bool,
}

type Translated = (Result<Vec<u8>, Unsupported>, Vec<String>);

/// Each distinct program translated once (convert.program_to_es_once: Unity repeats the same
/// program across variants and shaders); its notes and Unsupported come again on every use.
#[derive(Default)]
pub struct Translations {
    cache: HashMap<(Vec<u8>, String, Optimize, Es), Translated>,
}

impl Translations {
    pub fn new() -> Self {
        Self::default()
    }

    /// glsl::program_to_es of a program's bytes (Err: the code is not UTF-8, where the Python stops).
    pub fn program_to_es(
        &mut self,
        code: &[u8],
        precision: &str,
        optimize: Optimize,
        notes: &mut Vec<String>,
        es: Es,
    ) -> Result<Result<Vec<u8>, Unsupported>, Error> {
        let key = (code.to_vec(), precision.to_string(), optimize, es);
        if !self.cache.contains_key(&key) {
            let text =
                std::str::from_utf8(code).map_err(|e| Error(format!("program code: {e}")))?;
            let mut added = Vec::new();
            let res = glsl::program_to_es(text, precision, optimize, &mut added, es)
                .map(String::into_bytes);
            self.cache.insert(key.clone(), (res, added));
        }
        let (res, added) = &self.cache[&key];
        notes.extend(added.iter().cloned());
        Ok(res.clone())
    }

    /// Every translation made: (code, precision, rewrites, version) -> (result, notes).
    pub fn iter(&self) -> impl Iterator<Item = (&(Vec<u8>, String, Optimize, Es), &Translated)> {
        self.cache.iter()
    }

    pub fn len(&self) -> usize {
        self.cache.len()
    }

    pub fn is_empty(&self) -> bool {
        self.cache.is_empty()
    }
}

/// One subprogram list while the shader is worked on: its items (copies of the old list's
/// subprograms) and parameter blob indices.
struct WorkList<'a> {
    at: ListRef,
    old: &'a [SubProgram],
    items: Vec<Item>,
    pbi: Option<Vec<i64>>,
}

impl WorkList<'_> {
    fn sp(&self, k: usize) -> (&SubProgram, i64) {
        let it = self.items[k];
        (&self.old[it.from], it.gpu_program_type)
    }
}

/// The subprogram lists in convert.subprogram_lists' order.
fn work_lists(shader: &Shader) -> Result<Vec<WorkList<'_>>, Error> {
    let mut out = Vec::new();
    for (si, passes) in shader.sub_shaders.iter().enumerate() {
        for (pi, p) in passes.iter().enumerate() {
            for (st, prog) in p.programs.iter().enumerate() {
                let Some(prog) = prog else { continue };
                let identity = |l: &[SubProgram]| -> Vec<Item> {
                    l.iter()
                        .enumerate()
                        .map(|(k, sp)| Item {
                            from: k,
                            gpu_program_type: sp.gpu_program_type,
                        })
                        .collect()
                };
                if let Some(l) = &prog.sub_programs {
                    let at = ListRef {
                        sub_shader: si,
                        pass: pi,
                        stage: st,
                        tier: None,
                    };
                    out.push(WorkList {
                        at,
                        old: l,
                        items: identity(l),
                        pbi: None,
                    });
                }
                if let Some(players) = prog.player_sub_programs.as_ref().filter(|p| !p.is_empty()) {
                    let pbi = prog.parameter_blob_indices.as_ref().ok_or_else(|| {
                        Error(format!(
                            "{}: m_PlayerSubPrograms without m_ParameterBlobIndices",
                            shader.name
                        ))
                    })?;
                    for (tier, l) in players.iter().enumerate() {
                        let t = pbi.get(tier).ok_or_else(|| {
                            Error(format!(
                                "{}: no m_ParameterBlobIndices for tier {tier}",
                                shader.name
                            ))
                        })?;
                        let at = ListRef {
                            sub_shader: si,
                            pass: pi,
                            stage: st,
                            tier: Some(tier),
                        };
                        out.push(WorkList {
                            at,
                            old: l,
                            items: identity(l),
                            pbi: Some(t.clone()),
                        });
                    }
                }
            }
        }
    }
    Ok(out)
}

/// The keyword lists (sorted) and hardware tier of a subprogram (convert._variant_key).
type VariantKey = (Vec<Vec<i64>>, Option<i64>);

fn variant_key(sp: &SubProgram) -> VariantKey {
    let kws = [
        &sp.keyword_indices,
        &sp.global_keyword_indices,
        &sp.local_keyword_indices,
    ]
    .into_iter()
    .flatten()
    .map(|k| {
        let mut k = k.clone();
        k.sort();
        k
    })
    .collect();
    (kws, sp.hardware_tier)
}

/// The shader's state while it is worked on: what the Python has in the tree.
struct Work<'a> {
    shader: &'a Shader,
    lists: Vec<WorkList<'a>>,
    pass_platforms: BTreeMap<(usize, usize), Vec<i64>>,
    /// The blob fields once stored (None: as they were).
    stored: Option<BlobFields>,
}

impl<'a> Work<'a> {
    fn new(shader: &'a Shader) -> Result<Self, Error> {
        let mut pass_platforms = BTreeMap::new();
        for (si, passes) in shader.sub_shaders.iter().enumerate() {
            for (pi, p) in passes.iter().enumerate() {
                if let Some(pl) = &p.platforms {
                    pass_platforms.insert((si, pi), pl.clone());
                }
            }
        }
        Ok(Work {
            shader,
            lists: work_lists(shader)?,
            pass_platforms,
            stored: None,
        })
    }

    fn fields(&self) -> &BlobFields {
        self.stored.as_ref().unwrap_or(&self.shader.blob)
    }

    /// The blob indices of the GLCore platform's parameter entries (2021.2+) and program entries.
    fn glcore_indices(&self) -> Result<(BTreeSet<i64>, BTreeSet<i64>), Error> {
        let (mut params, mut programs) = (BTreeSet::new(), BTreeSet::new());
        for l in &self.lists {
            for k in 0..l.items.len() {
                let (sp, t) = l.sp(k);
                if !GLCORE_TYPES.contains(&t) {
                    continue;
                }
                programs.insert(sp.blob_index);
                if let Some(pbi) = &l.pbi {
                    let p = pbi.get(k).ok_or_else(|| {
                        Error(format!(
                            "{}: fewer parameter blob indices than subprograms",
                            self.shader.name
                        ))
                    })?;
                    params.insert(*p);
                }
            }
        }
        Ok((params, programs))
    }

    fn edit(&self) -> Edit {
        let lists = self
            .lists
            .iter()
            .filter(|l| {
                let same = l.items.len() == l.old.len()
                    && l.items.iter().enumerate().all(|(k, it)| {
                        it.from == k && it.gpu_program_type == l.old[k].gpu_program_type
                    });
                let old_pbi = l.at.tier.map(|t| &self.shader_pbi(l.at)[t]);
                !same || l.pbi.as_ref() != old_pbi
            })
            .map(|l| ListEdit {
                at: l.at,
                items: l.items.clone(),
                parameter_blob_indices: l.pbi.clone(),
            })
            .collect();
        let pass_platforms = self
            .pass_platforms
            .iter()
            .filter(|((si, pi), pl)| {
                self.shader.sub_shaders[*si][*pi].platforms.as_ref() != Some(*pl)
            })
            .map(|(&(si, pi), pl)| PassPlatforms {
                sub_shader: si,
                pass: pi,
                platforms: pl.clone(),
            })
            .collect();
        Edit {
            blob: self.fields().clone(),
            lists,
            pass_platforms,
        }
    }

    fn shader_pbi(&self, at: ListRef) -> &Vec<Vec<i64>> {
        let prog = self.shader.sub_shaders[at.sub_shader][at.pass].programs[at.stage]
            .as_ref()
            .unwrap();
        prog.parameter_blob_indices.as_ref().unwrap()
    }

    /// convert._strip_gles: the GLES3Plus blob and every GLES subprogram removed.
    fn strip_gles(&mut self) -> Result<(), Error> {
        let mut blobs = ShaderBlobs::read(self.fields())?;
        blobs.remove(blob::PLATFORM_GLES3PLUS);
        self.stored = Some(blobs.store()?);
        for l in &mut self.lists {
            let keep: Vec<usize> = (0..l.items.len())
                .filter(|&k| !GLES_TYPES.contains(&l.items[k].gpu_program_type))
                .collect();
            if let Some(pbi) = &mut l.pbi {
                *pbi = keep
                    .iter()
                    .map(|&k| {
                        pbi.get(k).copied().ok_or_else(|| {
                            Error("fewer parameter blob indices than subprograms".into())
                        })
                    })
                    .collect::<Result<_, _>>()?;
            }
            l.items = keep.iter().map(|&k| l.items[k]).collect();
        }
        for pl in self.pass_platforms.values_mut() {
            if let Some(k) = pl.iter().position(|&p| p == blob::PLATFORM_GLES3PLUS) {
                pl.remove(k);
            }
        }
        Ok(())
    }
}

/// What one shader's translation leaves: the report, and the edit when the step saves the tree.
#[derive(Clone, Debug, PartialEq)]
pub struct Outcome {
    pub report: Report,
    pub edit: Option<Edit>,
}

fn done(w: &Work, mut r: Report) -> Result<Outcome, Error> {
    let save = r.status == Status::Converted || r.changed;
    let edit = save.then(|| w.edit());
    r.platforms = edit.as_ref().map_or_else(
        || w.fields().platforms.clone(),
        |e| e.blob.platforms.clone(),
    );
    Ok(Outcome { report: r, edit })
}

/// A translated entry: done (or no code), a VR-only program left out, or why it does not translate.
enum Translation {
    Done,
    Vr,
    Unsupported(Unsupported),
}

/// convert.add_gles3 with the step's arguments: GLES3 programs for one Shader.
pub fn add_gles3(shader: &Shader, opt: &Options, tr: &mut Translations) -> Result<Outcome, Error> {
    let name = &shader.name;
    let mut r = Report::new(name, opt.glcore4x);
    let plats = &shader.blob.platforms;
    if !plats.contains(&blob::PLATFORM_GLCORE) {
        r.why = "no OpenGL Core programs".into();
        r.platforms = plats.clone();
        return Ok(Outcome {
            report: r,
            edit: None,
        });
    }
    let had_gles3 = plats.contains(&blob::PLATFORM_GLES3PLUS);
    if had_gles3 && !opt.replace {
        r.why = "already has GLES3 programs".into();
        r.platforms = plats.clone();
        return Ok(Outcome {
            report: r,
            edit: None,
        });
    }
    let mut w = Work::new(shader)?;
    if had_gles3 {
        w.strip_gles()?; // rebuilt below, or (no translatable programs) left without any
        r.status = Status::Converted;
        r.why = "(old GLES programs removed)".into();
    }
    let mut blobs = ShaderBlobs::read(w.fields())?;
    let (params, programs) = w.glcore_indices()?;
    let entries = blobs.entries(blob::PLATFORM_GLCORE, &params, Some(&programs))?;
    let mut used = BTreeSet::new();
    let mut used4: BTreeMap<usize, BTreeSet<i64>> = BTreeMap::new();
    for l in &w.lists {
        for k in 0..l.items.len() {
            let (sp, t) = l.sp(k);
            if t == blob::GPU_GLCORE32 {
                used.insert(sp.blob_index);
            } else if opt.glcore4x && GLCORE4X.contains(&t) {
                used4
                    .entry(l.at.sub_shader)
                    .or_default()
                    .insert(sp.blob_index);
            }
        }
    }
    let stage_count = || {
        blobs
            .stage_counts
            .as_ref()
            .filter(|s| !s.is_empty())
            .and_then(|s| s.get(&blob::PLATFORM_GLCORE).copied())
    };
    if used.is_empty() && used4.is_empty() {
        if opt.glcore4x {
            // no GLCore program at all (every variant stripped): an empty GLES3 blob, as Unity's
            // own builds have a blob for each of their platforms
            let stages = stage_count();
            let segs = blobs.segments(blob::PLATFORM_GLCORE)?;
            blobs.set_entries(blob::PLATFORM_GLES3PLUS, &entries, &segs, stages);
            w.stored = Some(blobs.store()?);
            r.status = Status::Converted;
            r.empty = true;
            r.why = py::strip(&format!(
                "no programs (every variant stripped): an empty GLES3 blob {}",
                r.why
            ))
            .to_string();
            return done(&w, r);
        }
        r.why = py::strip(&format!("no GLCore32 programs {}", r.why)).to_string();
        return done(&w, r);
    }
    let all4: BTreeSet<i64> = used4.values().flatten().copied().collect();
    if let Some(i) = used.intersection(&all4).next() {
        return Err(Error(format!(
            "{name}: blob entry {i} is both a GLCore32 and a GLCore41/43 program"
        )));
    }
    let mut out = entries.clone();
    let mut dropped = BTreeSet::new();
    let mut es31 = BTreeSet::new();

    let mut translate_entry =
        |i: i64, r: &mut Report, out: &mut Vec<Option<Slot>>| -> Result<Translation, Error> {
            let ix = usize::try_from(i)
                .ok()
                .filter(|&x| x < out.len())
                .ok_or_else(|| Error(format!("{name}: blob index {i} out of range")))?;
            let Some(slot) = &mut out[ix] else {
                return Ok(Translation::Done);
            };
            let Slot::Program(e) = slot else {
                return Err(Error(format!(
                    "{name}: blob entry {i} is both a program and parameters"
                )));
            };
            r.guessed_layout |= e.guessed;
            if !e.code.is_empty() {
                let mut res = tr.program_to_es(
                    &e.code,
                    &opt.precision,
                    opt.optimize,
                    &mut r.notes,
                    Es::Es300,
                )?;
                if let Err(u) = &res {
                    if u.needs_es31 {
                        res = tr.program_to_es(
                            &e.code,
                            &opt.precision,
                            opt.optimize,
                            &mut r.notes,
                            Es::Es310,
                        )?;
                        if res.is_ok() {
                            es31.insert(i);
                        }
                    }
                }
                match res {
                    Ok(code) => e.code = code,
                    Err(ex) => {
                        let vr = e
                            .keywords
                            .iter()
                            .chain(e.local_keywords.iter().flatten())
                            .any(|k| VR_KEYWORDS.contains(&k.as_str()));
                        out[ix] = entries[ix].clone();
                        if vr {
                            // e.g. STEREO_INSTANCING_ON writes gl_Layer (not in ES 3.00)
                            dropped.insert(i);
                            r.dropped += 1;
                            return Ok(Translation::Vr);
                        }
                        return Ok(Translation::Unsupported(ex));
                    }
                }
                r.programs += 1;
            }
            e.gpu_type = blob::GPU_GLES3 as i32;
            Ok(Translation::Done)
        };

    for &i in &used {
        if let Translation::Unsupported(ex) = translate_entry(i, &mut r, &mut out)? {
            r.status = Status::Skipped;
            r.why = format!("program {i}: {ex}");
            r.changed = opt.replace && had_gles3;
            return done(&w, r);
        }
    }
    let mut done4: BTreeMap<i64, Option<Unsupported>> = BTreeMap::new();
    let mut counted = BTreeSet::new();
    let mut failed4 = BTreeSet::new();
    for (&si, set) in &used4 {
        for &i in set {
            if let std::collections::btree_map::Entry::Vacant(v) = done4.entry(i) {
                let n = r.programs;
                let t = translate_entry(i, &mut r, &mut out)?;
                v.insert(match t {
                    Translation::Unsupported(u) => Some(u),
                    _ => None,
                });
                if r.programs > n {
                    counted.insert(i);
                }
            }
            if let Some(ex) = &done4[&i] {
                failed4.insert(si);
                r.failed
                    .push((si, format!("GLCore41/43 program {i}: {ex}")));
                break;
            }
        }
    }
    // what only the failed SubShaders use goes back as it was (no GLES subprogram names it)
    let keep: BTreeSet<i64> = used4
        .iter()
        .filter(|(si, _)| !failed4.contains(*si))
        .flat_map(|(_, s)| s.iter().copied())
        .collect();
    for &i in done4.keys().filter(|i| !keep.contains(i)) {
        let ix = i as usize;
        out[ix] = entries[ix].clone();
        es31.remove(&i);
        if counted.contains(&i) {
            r.programs -= 1;
        }
    }
    r.programs4x = counted.intersection(&keep).count();

    let twinned = |at: ListRef, t: i64| {
        t == blob::GPU_GLCORE32
            || GLCORE4X.contains(&t) && opt.glcore4x && !failed4.contains(&at.sub_shader)
    };
    type Key = (usize, usize, VariantKey, i64);
    let key = |at: ListRef, sp: &SubProgram, t: i64| -> Key {
        (at.sub_shader, at.pass, variant_key(sp), t)
    };
    // the variants (per pass) whose program was left out get no twins; those made GLES 3.1
    // programs: all their subprograms (and entries) are typed GLES 3.1
    let (mut skip, mut raised) = (BTreeSet::<Key>::new(), BTreeSet::<Key>::new());
    for l in &w.lists {
        for k in 0..l.items.len() {
            let (sp, t) = l.sp(k);
            if twinned(l.at, t) {
                if dropped.contains(&sp.blob_index) {
                    skip.insert(key(l.at, sp, t));
                }
                if es31.contains(&sp.blob_index) {
                    raised.insert(key(l.at, sp, t));
                }
            }
        }
    }
    for l in &w.lists {
        for k in 0..l.items.len() {
            let (sp, t) = l.sp(k);
            if twinned(l.at, t) && raised.contains(&key(l.at, sp, t)) {
                if let Some(Some(Slot::Program(e))) = usize::try_from(sp.blob_index)
                    .ok()
                    .and_then(|x| out.get_mut(x))
                {
                    e.gpu_type = blob::GPU_GLES31 as i32;
                }
            }
        }
    }
    r.es31 = es31.len();
    // commit: the blob, then the twins
    let stages = stage_count();
    let mut twins_of: Vec<Vec<(Item, Option<i64>)>> = Vec::with_capacity(w.lists.len());
    for l in &w.lists {
        let mut twins = Vec::new();
        for k in 0..l.items.len() {
            let (sp, t) = l.sp(k);
            let kk = key(l.at, sp, t);
            if twinned(l.at, t) && !skip.contains(&kk) {
                let gt = if raised.contains(&kk) {
                    blob::GPU_GLES31
                } else {
                    blob::GPU_GLES3
                };
                twins.push((
                    Item {
                        from: l.items[k].from,
                        gpu_program_type: gt,
                    },
                    l.pbi.as_ref().map(|p| p[k]),
                ));
            }
        }
        twins_of.push(twins);
    }
    if !failed4.is_empty() && twins_of.iter().all(|t| t.is_empty()) {
        r.status = Status::Skipped;
        r.why = r
            .failed
            .iter()
            .map(|(si, why)| format!("SubShader {si}: {why}"))
            .collect::<Vec<_>>()
            .join("; ");
        r.changed = opt.replace && had_gles3;
        return done(&w, r);
    }
    let segs = blobs.segments(blob::PLATFORM_GLCORE)?;
    blobs.set_entries(blob::PLATFORM_GLES3PLUS, &out, &segs, stages);
    w.stored = Some(blobs.store()?);
    for (l, twins) in w.lists.iter_mut().zip(twins_of) {
        if twins.is_empty() {
            continue;
        }
        r.twins += twins.len();
        for (it, pi) in twins {
            l.items.push(it);
            if let (Some(pbi), Some(pi)) = (&mut l.pbi, pi) {
                pbi.push(pi);
            }
        }
        if let Some(pl) = w.pass_platforms.get_mut(&(l.at.sub_shader, l.at.pass)) {
            if !pl.contains(&blob::PLATFORM_GLES3PLUS) {
                pl.push(blob::PLATFORM_GLES3PLUS);
                pl.sort();
            }
        }
    }
    r.status = Status::Converted;
    if !r.failed.is_empty() {
        let left: Vec<String> = r
            .failed
            .iter()
            .map(|(si, why)| format!("SubShader {si} left out: {why}"))
            .collect();
        r.why = if r.why.is_empty() {
            left.join("; ")
        } else {
            format!("{} {}", r.why, left.join("; "))
        };
    }
    done(&w, r)
}

/// A Shader with an [`Edit`] applied, as the Unity file layer writes it (twins: copies retyped).
pub fn apply(shader: &Shader, edit: &Edit) -> Shader {
    let mut s = shader.clone();
    s.blob = edit.blob.clone();
    for le in &edit.lists {
        let p = &mut s.sub_shaders[le.at.sub_shader][le.at.pass];
        let prog = p.programs[le.at.stage].as_mut().unwrap();
        let old = match le.at.tier {
            None => prog.sub_programs.as_ref().unwrap(),
            Some(t) => &prog.player_sub_programs.as_ref().unwrap()[t],
        };
        let new: Vec<SubProgram> = le
            .items
            .iter()
            .map(|it| SubProgram {
                gpu_program_type: it.gpu_program_type,
                ..old[it.from].clone()
            })
            .collect();
        match le.at.tier {
            None => prog.sub_programs = Some(new),
            Some(t) => {
                prog.player_sub_programs.as_mut().unwrap()[t] = new;
                if let Some(pbi) = &le.parameter_blob_indices {
                    prog.parameter_blob_indices.as_mut().unwrap()[t] = pbi.clone();
                }
            }
        }
    }
    for pp in &edit.pass_platforms {
        s.sub_shaders[pp.sub_shader][pp.pass].platforms = Some(pp.platforms.clone());
    }
    s
}

/// convert.gles3_programs: (blob index, code) of the GLES3 programs GLES3 subprograms name.
pub fn gles3_programs(shader: &Shader) -> Result<Vec<(i64, String)>, Error> {
    if !shader.blob.platforms.contains(&blob::PLATFORM_GLES3PLUS) {
        return Ok(Vec::new());
    }
    let w = Work::new(shader)?;
    let (mut params, mut programs) = (BTreeSet::new(), BTreeSet::new());
    let mut idx = BTreeSet::new();
    for l in &w.lists {
        for k in 0..l.items.len() {
            let (sp, t) = l.sp(k);
            if [blob::GPU_GLES31AEP, blob::GPU_GLES31, blob::GPU_GLES3].contains(&t) {
                programs.insert(sp.blob_index);
                if let Some(pbi) = &l.pbi {
                    params.insert(pbi[k]);
                }
            }
            if t == blob::GPU_GLES3 || t == blob::GPU_GLES31 {
                idx.insert(sp.blob_index);
            }
        }
    }
    let entries = ShaderBlobs::read(&shader.blob)?.entries(
        blob::PLATFORM_GLES3PLUS,
        &params,
        Some(&programs),
    )?;
    let mut out = Vec::new();
    for i in idx {
        if let Some(Some(Slot::Program(Entry { code, .. }))) = entries.get(i as usize) {
            if !code.is_empty() {
                out.push((i, String::from_utf8_lossy(code).into_owned()));
            }
        }
    }
    Ok(out)
}
