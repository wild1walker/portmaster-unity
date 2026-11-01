//! Step `shaders` (shaders/step.py): GLES3 programs for every Shader in a game's data, each
//! translated by shaderx from its OpenGL Core programs.
//!
//! Files searched: the serialized files (globalgamemanagers, level<n>, *.assets),
//! Resources/unity_builtin_extra, and the asset bundles under StreamingAssets whose names match
//! `bundles` (a regular expression; a bundle is rewritten only when one of its shaders changes,
//! one with several serialized files is left as it is). Each file is opened as the Python's
//! Loaded opens it (a bundle of more than 64 MB unpacked into the step's work folder), its
//! Shader trees read, given to shaderx, the changed ones saved and the file written in place.
//!
//! Ported: the OpenGL Core source (`from: glcore`), `precision`, `replace`, `optimize`,
//! `glcore4x`, `bundles`, `grab_fetch` and `grab_fetch_names` (grabfetch.rs). Direct3D 11, Vulkan
//! and Metal sources, compute shaders and `fused_post` stop the step as not ported (the setup
//! runs the Python for those).
use super::grabfetch;
use super::{field, field_mut, objects, Args};
use crate::error::{Error, Result};
use crate::job::Job;
use crate::pyjson::Json;
use shaderx::blob::{BlobFields, Lengths};
use shaderx::convert::{self, Edit, Pass, Shader, StageProgram, SubProgram, STAGES};
use shaderx::glsl::Optimize;
use shaderx::Translations;
use std::path::Path;
use unityfs::Value;

const SOURCES: [&str; 4] = ["glcore", "d3d11", "vulkan", "metal"];

/// The Shader.platforms value of a source's programs (step.SOURCES).
fn source_platform(src: &str) -> i64 {
    match src {
        "glcore" => shaderx::blob::PLATFORM_GLCORE,
        "d3d11" => shaderx::blob::PLATFORM_D3D11,
        "vulkan" => shaderx::blob::PLATFORM_VULKAN,
        _ => shaderx::blob::PLATFORM_METAL,
    }
}

/// The step's settings from the config's `shaders` section (__main__._builtin and step.run).
struct Settings {
    precision: String,
    replace: bool,
    bundles: Option<regex::Regex>,
    optimize: Optimize,
    sources: Vec<String>,
    glcore4x: bool,
    compute: bool,
    /// grab_fetch: None (off), Some(false) (true: where Unity reports framebuffer fetch),
    /// Some(true) ("always").
    grab_fetch: Option<bool>,
    /// grab_fetch_names: the regex, or why it does not compile.
    grab_fetch_names: Option<std::result::Result<regex::Regex, String>>,
}

/// A Python-style value check: `v in (allowed...)` with Python's == (1 == True).
fn one_of(v: &Json, allowed: &[Json]) -> bool {
    allowed.iter().any(|a| a == v)
}

fn str_list(v: &Json) -> Vec<String> {
    match v {
        Json::Str(s) => s.chars().map(String::from).collect(),
        Json::List(l) => l.iter().map(Json::to_str).collect(),
        _ => Vec::new(),
    }
}

fn settings(cfg: &Json) -> Result<Settings> {
    let none = Json::dict();
    let c = match cfg.get("shaders") {
        None => &none,
        Some(c @ Json::Dict(_)) => c,
        Some(Json::Null) => &none,
        Some(other) => return Err(Error::Value(format!("shaders: {}", other.repr()))),
    };
    let get = |k: &str| c.get(k);
    let precision = get("precision").map_or("highp".to_string(), Json::to_str);
    let replace = get("replace").is_some_and(Json::truthy);
    let bundles = match get("bundles") {
        None => Some(".".to_string()),
        Some(v) if v.truthy() => Some(v.to_str()),
        Some(_) => None,
    };
    let bundles = match bundles {
        Some(rx) => Some(
            regex::Regex::new(&rx)
                .map_err(|e| Error::Value(format!("shaders.bundles {rx:?}: {e}")))?,
        ),
        None => None,
    };
    let optimize = match get("optimize") {
        None => Optimize::ALL,
        Some(v) => Optimize::from_names(&str_list(v)),
    };
    let sources: Vec<String> = match get("from") {
        None => vec!["glcore".into()],
        Some(Json::Str(s)) => s.split(',').map(String::from).collect(),
        Some(v) => str_list(v),
    };
    for s in &sources {
        if !SOURCES.contains(&s.as_str()) {
            return Err(Error::Value(format!(
                "unknown shader source {} (known: {})",
                crate::pyjson::str_repr(s),
                SOURCES.join(", ")
            )));
        }
    }
    if sources.is_empty() {
        return Err(Error::Value("tuple index out of range".into()));
    }
    let glcore4x = get("glcore4x").cloned().unwrap_or(Json::Null);
    if !one_of(
        &glcore4x,
        &[Json::Null, Json::Bool(true), Json::Bool(false)],
    ) {
        return Err(Error::Value(format!(
            "shaders.glcore4x: {} (true or false)",
            glcore4x.repr()
        )));
    }
    let compute = get("compute").cloned().unwrap_or(Json::Bool(true));
    if !one_of(&compute, &[Json::Bool(true), Json::Bool(false)]) {
        return Err(Error::Value(format!(
            "shaders.compute: {} (true or false)",
            compute.repr()
        )));
    }
    // (step.grab_fetch_mode)
    let gf = get("grab_fetch").cloned().unwrap_or(Json::Null);
    let off = [
        Json::Null,
        Json::Bool(false),
        Json::Str("off".into()),
        Json::Str("false".into()),
    ];
    let on = [
        Json::Bool(true),
        Json::Str("on".into()),
        Json::Str("true".into()),
    ];
    let grab_fetch = if one_of(&gf, &off) {
        None
    } else if one_of(&gf, &on) {
        Some(false)
    } else if gf == Json::Str("always".into()) {
        Some(true)
    } else {
        return Err(Error::Value(format!(
            "shaders.grab_fetch: {} (false, true or \"always\")",
            gf.repr()
        )));
    };
    let grab_fetch_names = match get("grab_fetch_names") {
        None | Some(Json::Null) => None,
        Some(Json::Str(s)) if s.is_empty() => None,
        Some(Json::Str(s)) => Some(regex::Regex::new(s).map_err(|e| {
            format!(
                "shaders.grab_fetch_names {}: {e}",
                crate::pyjson::str_repr(s)
            )
        })),
        Some(v) if !v.truthy() => None,
        Some(v) => Some(Err(format!(
            "shaders.grab_fetch_names: {} is not a regular expression",
            v.repr()
        ))),
    };
    if get("fused_post").is_some_and(Json::truthy) {
        return Err(Error::NotPorted("shaders.fused_post".into()));
    }
    Ok(Settings {
        precision,
        replace,
        bundles,
        optimize,
        sources,
        glcore4x: glcore4x.is_null_or_true(),
        compute: compute.truthy(),
        grab_fetch,
        grab_fetch_names,
    })
}

impl Json {
    /// glcore4x_for: None or a true value.
    fn is_null_or_true(&self) -> bool {
        matches!(self, Json::Null) || self.truthy()
    }
}

/// Whether a file starts as a UnityFS bundle does.
pub fn is_bundle(path: &Path) -> bool {
    use std::io::Read;
    let mut head = [0u8; 8];
    std::fs::File::open(path)
        .and_then(|mut f| f.read_exact(&mut head))
        .is_ok()
        && &head == b"UnityFS\0"
}

/// `re.match(r'level\d+$', name)`.
pub fn is_level(name: &str) -> bool {
    static RX: std::sync::OnceLock<regex::Regex> = std::sync::OnceLock::new();
    RX.get_or_init(|| regex::Regex::new(r"^level\d+\n?\z").unwrap())
        .is_match(name)
}

/// The files the step searches (step.shader_files), in its order.
fn shader_files(job: &Job, bundles: Option<&regex::Regex>) -> Vec<String> {
    let mut files = job.list("", |n| {
        n.ends_with(".assets") || n == "globalgamemanagers" || is_level(n)
    });
    files.extend(job.list("Resources", |n| n == "unity_builtin_extra"));
    if let Some(rx) = bundles {
        for (rel, names) in job.walk("StreamingAssets") {
            let dir = job.src.join(&rel);
            let mut found: Vec<String> = names
                .into_iter()
                .filter(|n| {
                    rx.is_match(n)
                        && !n.ends_with(".manifest")
                        && !n.ends_with(".meta")
                        && is_bundle(&dir.join(n))
                })
                .map(|n| format!("{rel}/{n}"))
                .collect();
            found.sort();
            files.extend(found);
        }
    }
    files
}

// ---- a Shader object's tree as shaderx's Shader, and an Edit written back into it

fn ints(v: &Value, what: &str) -> Result<Vec<i64>> {
    let bad = || Error::Value(format!("{what}: not a list of integers"));
    v.as_array()
        .ok_or_else(bad)?
        .iter()
        .map(|x| x.as_i64().ok_or_else(bad))
        .collect()
}

fn lengths(v: &Value, what: &str) -> Result<Lengths> {
    let a = v
        .as_array()
        .ok_or_else(|| Error::Value(format!("{what}: not a list")))?;
    if a.first().is_some_and(|x| x.as_array().is_some()) {
        Ok(Lengths::Nested(
            a.iter().map(|x| ints(x, what)).collect::<Result<_>>()?,
        ))
    } else {
        Ok(Lengths::Flat(ints(v, what)?))
    }
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

fn sub_program(v: &Value) -> Result<SubProgram> {
    let opt = |k: &str| v.get(k).map(|x| ints(x, k)).transpose();
    Ok(SubProgram {
        blob_index: super::int(v, "m_BlobIndex")? as i64,
        gpu_program_type: super::int(v, "m_GpuProgramType")? as i64,
        keyword_indices: opt("m_KeywordIndices")?,
        global_keyword_indices: opt("m_GlobalKeywordIndices")?,
        local_keyword_indices: opt("m_LocalKeywordIndices")?,
        hardware_tier: v
            .get("m_ShaderHardwareTier")
            .map(|x| {
                x.as_i64()
                    .ok_or_else(|| Error::Value("m_ShaderHardwareTier".into()))
            })
            .transpose()?,
    })
}

fn sub_programs(v: &Value) -> Result<Vec<SubProgram>> {
    v.as_array()
        .ok_or_else(|| Error::Value("m_SubPrograms: not a list".into()))?
        .iter()
        .map(sub_program)
        .collect()
}

/// The fields add_gles3 reads of a Shader's tree.
pub fn shader_of(t: &Value) -> Result<Shader> {
    let form = field(t, "m_ParsedForm")?;
    let mut subs = Vec::new();
    for ss in field(form, "m_SubShaders")?
        .as_array()
        .ok_or_else(|| Error::Value("m_SubShaders: not a list".into()))?
    {
        let mut passes = Vec::new();
        for p in field(ss, "m_Passes")?
            .as_array()
            .ok_or_else(|| Error::Value("m_Passes: not a list".into()))?
        {
            let mut pass = Pass {
                platforms: p
                    .get("m_Platforms")
                    .map(|x| ints(x, "m_Platforms"))
                    .transpose()?,
                ..Default::default()
            };
            for (k, st) in STAGES.iter().enumerate() {
                let Some(prog) = p.get(st) else { continue };
                pass.programs[k] = Some(StageProgram {
                    sub_programs: prog.get("m_SubPrograms").map(sub_programs).transpose()?,
                    player_sub_programs: prog
                        .get("m_PlayerSubPrograms")
                        .map(|t| {
                            t.as_array()
                                .ok_or_else(|| Error::Value("m_PlayerSubPrograms".into()))?
                                .iter()
                                .map(sub_programs)
                                .collect::<Result<Vec<_>>>()
                        })
                        .transpose()?,
                    parameter_blob_indices: prog
                        .get("m_ParameterBlobIndices")
                        .map(|t| {
                            t.as_array()
                                .ok_or_else(|| Error::Value("m_ParameterBlobIndices".into()))?
                                .iter()
                                .map(|x| ints(x, "m_ParameterBlobIndices"))
                                .collect::<Result<Vec<_>>>()
                        })
                        .transpose()?,
                });
            }
            passes.push(pass);
        }
        subs.push(passes);
    }
    Ok(Shader {
        name: String::from_utf8_lossy(&super::text(form, "m_Name")?).into_owned(),
        blob: BlobFields {
            platforms: ints(field(t, "platforms")?, "platforms")?,
            offsets: lengths(field(t, "offsets")?, "offsets")?,
            compressed_lengths: lengths(field(t, "compressedLengths")?, "compressedLengths")?,
            decompressed_lengths: lengths(field(t, "decompressedLengths")?, "decompressedLengths")?,
            compressed_blob: field(t, "compressedBlob")?
                .as_bytes()
                .ok_or_else(|| Error::Value("compressedBlob: not bytes".into()))?
                .to_vec(),
            stage_counts: t
                .get("stageCounts")
                .map(|x| ints(x, "stageCounts"))
                .transpose()?,
        },
        sub_shaders: subs,
    })
}

/// The blob fields written into a Shader's tree (blob.ShaderBlobs.store).
pub fn set_blob(t: &mut Value, b: &shaderx::blob::BlobFields) -> Result<()> {
    t.set("platforms", int_list(&b.platforms))?;
    t.set("offsets", lengths_value(&b.offsets))?;
    t.set("compressedLengths", lengths_value(&b.compressed_lengths))?;
    t.set(
        "decompressedLengths",
        lengths_value(&b.decompressed_lengths),
    )?;
    t.set("compressedBlob", Value::Bytes(b.compressed_blob.clone()))?;
    if let Some(sc) = &b.stage_counts {
        t.set("stageCounts", int_list(sc))?;
    }
    Ok(())
}

/// The tree with an Edit written into it, as add_gles3 leaves the Python's tree: the blob fields,
/// each changed subprogram list (copies of its old subprograms, retyped), passes' m_Platforms.
pub fn apply(t: &mut Value, edit: &Edit) -> Result<()> {
    set_blob(t, &edit.blob)?;
    for le in &edit.lists {
        let pass = pass_mut(t, le.at.sub_shader, le.at.pass)?;
        let prog = field_mut(pass, STAGES[le.at.stage])?;
        let list = match le.at.tier {
            None => field_mut(prog, "m_SubPrograms")?,
            Some(k) => field_mut(prog, "m_PlayerSubPrograms")?
                .as_array_mut()
                .and_then(|a| a.get_mut(k))
                .ok_or_else(|| Error::Value(format!("m_PlayerSubPrograms[{k}]")))?,
        };
        let old = list
            .as_array()
            .ok_or_else(|| Error::Value("subprograms: not a list".into()))?
            .clone();
        let mut new = Vec::with_capacity(le.items.len());
        for it in &le.items {
            let mut sp = old
                .get(it.from)
                .ok_or_else(|| Error::Value(format!("subprogram {}", it.from)))?
                .clone();
            sp.set("m_GpuProgramType", Value::I64(it.gpu_program_type))?;
            new.push(sp);
        }
        *list = Value::Array(new);
        if let (Some(k), Some(pbi)) = (le.at.tier, &le.parameter_blob_indices) {
            let slot = field_mut(prog, "m_ParameterBlobIndices")?
                .as_array_mut()
                .and_then(|a| a.get_mut(k))
                .ok_or_else(|| Error::Value(format!("m_ParameterBlobIndices[{k}]")))?;
            *slot = int_list(pbi);
        }
    }
    for pp in &edit.pass_platforms {
        let pass = pass_mut(t, pp.sub_shader, pp.pass)?;
        pass.set("m_Platforms", int_list(&pp.platforms))?;
    }
    Ok(())
}

fn pass_mut(t: &mut Value, ss: usize, p: usize) -> Result<&mut Value> {
    let subs = field_mut(field_mut(t, "m_ParsedForm")?, "m_SubShaders")?;
    let passes = field_mut(
        subs.as_array_mut()
            .and_then(|a| a.get_mut(ss))
            .ok_or_else(|| Error::Value(format!("SubShader {ss}")))?,
        "m_Passes",
    )?;
    passes
        .as_array_mut()
        .and_then(|a| a.get_mut(p))
        .ok_or_else(|| Error::Value(format!("pass {p}")))
}

/// Whether a DataFile::open error is the Python's NotOneSerializedFile (a bundle with no or
/// several serialized files, or whose serialized node is not one).
pub fn not_one(path: &Path, e: &unityfs::Error) -> bool {
    match e {
        unityfs::Error::Unsupported(m) => {
            is_bundle(path)
                && (m.contains(": expected one serialized file in the bundle, found ")
                    || m.ends_with(" is not a serialized file"))
        }
        _ => false,
    }
}

pub fn run(job: &mut Job, _args: &Args) -> Result<()> {
    let s = settings(&job.cfg)?;
    let opts = convert::Options {
        precision: s.precision.clone(),
        replace: s.replace,
        optimize: s.optimize,
        glcore4x: s.glcore4x,
    };
    let mut tr = Translations::new();
    let mut results: Vec<convert::Report> = Vec::new();
    let mut gresults: Vec<grabfetch::GfResult> = Vec::new();
    let mut changed_files = 0usize;
    for rel in shader_files(job, s.bundles.as_ref()) {
        if job.is_done("shaders", &rel) {
            continue;
        }
        let work = job.work_dir("shaders")?;
        let path = job.path_in(&rel);
        let o = unityfs::Options {
            spill_dir: Some(work.clone()),
            ..Default::default()
        };
        let mut f = match job.open(&path, o) {
            Ok(f) => f,
            Err(Error::Unity(e)) if not_one(&path, &e) => {
                job.log(&format!("shaders: {rel}: left as it is ({e})"));
                let _ = std::fs::remove_dir_all(&work);
                job.mark_done("shaders", &rel)?;
                continue;
            }
            Err(e) => return Err(e),
        };
        let mut n = 0usize;
        for (pid, class) in objects(&f) {
            if class == "ComputeShader" && s.compute {
                return Err(Error::NotPorted(format!("{rel}: compute shaders")));
            }
            if class != "Shader" {
                continue;
            }
            let mut t = f.sf.read_typetree(pid)?;
            let platforms = ints(field(&t, "platforms")?, "platforms")?;
            let src = s
                .sources
                .iter()
                .find(|x| platforms.contains(&source_platform(x)))
                .unwrap_or(&s.sources[0]);
            if src != "glcore" {
                return Err(Error::NotPorted(format!("{rel}: shaders from {src}")));
            }
            let shader = shader_of(&t)?;
            let out = convert::add_gles3(&shader, &opts, &mut tr)
                .map_err(|e| Error::Value(e.to_string()))?;
            let r = out.report;
            let mut changed = false;
            if let Some(edit) = &out.edit {
                apply(&mut t, edit)?;
                changed = true;
            }
            if let Some(always) = s.grab_fetch {
                let names = match &s.grab_fetch_names {
                    None => None,
                    Some(Ok(rx)) => Some(rx),
                    Some(Err(m)) => return Err(Error::Value(m.clone())),
                };
                let g = grabfetch::apply(&mut t, always, names)?;
                changed |= g.changed();
                if g.status != "unchanged" {
                    job.log(&format!("shaders: {rel}: grab_fetch: {g}"));
                }
                gresults.push(g);
            }
            if changed {
                f.sf.save_typetree(pid, &t)?;
                n += 1;
            }
            if r.status == convert::Status::Skipped || !r.failed.is_empty() {
                job.log(&format!("shaders: {rel}: {r}"));
            }
            if r.guessed_layout {
                job.log(&format!(
                    "shaders: {rel}: {}: program layout of an unverified Unity version (guessed, checked by round trip)",
                    r.name
                ));
            }
            results.push(r);
        }
        if n > 0 {
            job.write_with(&rel, |tmp| Ok(f.write_to(tmp, false)?))?;
            changed_files += 1;
        }
        drop(f);
        job.end_work(&work);
        job.mark_done("shaders", &rel)?;
    }
    let sources: Vec<&str> = s.sources.iter().map(String::as_str).collect();
    if let Some(err) = shaderx::step::nothing_to_translate(&results, &sources) {
        return Err(Error::Exit(err));
    }
    for line in shaderx::step::summary(&results, changed_files, &s.precision) {
        job.log(&line);
    }
    if let Some(always) = s.grab_fetch {
        job.log(&grabfetch::summary(&gresults, always));
    }
    Ok(())
}
