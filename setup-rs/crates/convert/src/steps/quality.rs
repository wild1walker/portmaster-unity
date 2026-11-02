//! Step `quality` (quality.py; docs/quality.md): the handheld quality profile. On a 1 GB handheld
//! the picture is made cheaper, never the game play: QualitySettings capped (every level), lights
//! that stay per pixel, the Standard shaders' LOD-150 SubShader first, URP's assets, SSAO and post
//! components; only where the shader variants a setting needs are in the data. The caps also go
//! into the journal (`quality_caps`), from where android-data gives them to ua_host.
use super::{objects, section};
use crate::error::{Error, Result};
use crate::job::Job;
use crate::monotree;
use crate::pyjson::Json;
use androiddata::{clrmeta, typetrees};
use shaderx::blob::{self, BlobFields, Lengths, ShaderBlobs, Slot};
use shaderx::convert::STAGES;
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::path::{Path, PathBuf};
use unityfs::{DataFile, TypeTree, Value};

fn defaults() -> Vec<(&'static str, Json)> {
    let s = |x: &str| Json::Str(x.into());
    vec![
        ("profile", s("auto")),
        ("ram_mb", Json::Int(1536)),
        ("pixel_lights", Json::Int(1)),
        ("shadow_resolution", Json::Int(1)),
        ("hard_shadows", Json::Bool(true)),
        ("shadow_cascades", Json::Int(2)),
        ("msaa", Json::Int(0)),
        ("anisotropic", Json::Int(1)),
        ("soft_particles", Json::Bool(false)),
        ("standard_lod150", Json::Bool(true)),
        ("hdr_format", Json::Bool(false)),
        ("urp_shadow_resolution", Json::Int(2048)),
        ("urp_soft_shadows", s("low")),
        ("ssao", Json::Bool(true)),
        (
            "post",
            Json::List(vec![
                s("ChromaticAberration"),
                s("LensDistortion"),
                s("MotionBlur"),
            ]),
        ),
        ("render_scale", Json::Null),
        ("dry_run", Json::Bool(false)),
    ]
}

const STANDARD: [&str; 4] = [
    "Standard",
    "Standard (Specular setup)",
    "Standard (Roughness setup)",
    "Autodesk Interactive",
];
const SSAO_SHADER: &str = "Hidden/Universal Render Pipeline/ScreenSpaceAmbientOcclusion";
const URP_ASSETS: [&str; 2] = [
    "Unity.RenderPipelines.Universal.Runtime",
    "Unity.RenderPipelines.Core.Runtime",
];
const POST_CLASSES: [&str; 17] = [
    "Bloom",
    "ChromaticAberration",
    "LensDistortion",
    "FilmGrain",
    "MotionBlur",
    "DepthOfField",
    "Vignette",
    "PaniniProjection",
    "Tonemapping",
    "ColorAdjustments",
    "ColorLookup",
    "ChannelMixer",
    "LiftGammaGain",
    "ShadowsMidtonesHighlights",
    "SplitToning",
    "WhiteBalance",
    "ColorCurves",
];
const SPOT: i128 = 0;
const DIRECTIONAL: i128 = 1;
const POINT: i128 = 2;
const AUTO: i128 = 0;
const IMPORTANT: i128 = 1;
const BAKED: i128 = 2;
const URP_ASSET: &str = "UniversalRenderPipelineAsset";
const SSAO: &str = "ScreenSpaceAmbientOcclusion";

// ---------------------------------------------------------------------------- values

/// A value as a Python number (ints exactly, floats as floats, bools as 0 / 1).
#[derive(Clone, Copy, Debug, PartialEq)]
enum Num {
    I(i128),
    F(f64),
}

impl Num {
    fn of(v: &Value) -> Option<Num> {
        match v {
            Value::F32(x) => Some(Num::F(*x as f64)),
            Value::F64(x) => Some(Num::F(*x)),
            _ => v.as_int().map(Num::I),
        }
    }

    fn of_json(v: &Json) -> Option<Num> {
        match v {
            Json::Float(f) => Some(Num::F(*f)),
            Json::Bool(b) => Some(Num::I(*b as i128)),
            Json::Int(i) => Some(Num::I(*i)),
            _ => None,
        }
    }

    fn f(self) -> f64 {
        match self {
            Num::I(i) => i as f64,
            Num::F(f) => f,
        }
    }

    fn gt(self, o: Num) -> bool {
        match (self, o) {
            (Num::I(a), Num::I(b)) => a > b,
            _ => self.f() > o.f(),
        }
    }
}

/// `str(v)` of a tree value in a log line (True/False, ints, floats as repr).
fn show(v: &Value) -> String {
    match v {
        Value::Bool(b) => if *b { "True" } else { "False" }.into(),
        Value::F32(x) => crate::pyjson::float_repr(*x as f64),
        Value::F64(x) => crate::pyjson::float_repr(*x),
        Value::Str(b) => String::from_utf8_lossy(b).into_owned(),
        _ => v
            .as_int()
            .map_or_else(|| format!("{v:?}"), |i| i.to_string()),
    }
}

/// `type(old)(cap)`: the cap as a value of the old one's type.
fn like(old: &Value, cap: Num) -> Value {
    let i = match cap {
        Num::I(i) => i,
        Num::F(f) => f.trunc() as i128,
    };
    match old {
        Value::Bool(_) => Value::Bool(cap.f() != 0.0),
        Value::F32(_) => Value::F32(cap.f() as f32),
        Value::F64(_) => Value::F64(cap.f()),
        Value::I8(_) => Value::I8(i as i8),
        Value::U8(_) => Value::U8(i as u8),
        Value::I16(_) => Value::I16(i as i16),
        Value::U16(_) => Value::U16(i as u16),
        Value::I32(_) => Value::I32(i as i32),
        Value::U32(_) => Value::U32(i as u32),
        Value::U64(_) => Value::U64(i as u64),
        _ => Value::I64(i as i64),
    }
}

fn num_json(j: &Json) -> String {
    j.to_str()
}

fn int_field(t: &Value, k: &str) -> Option<i128> {
    t.get(k).and_then(Value::as_int)
}

fn text(t: &Value, k: &str) -> String {
    t.get(k)
        .and_then(Value::as_bytes)
        .map(|b| String::from_utf8_lossy(b).into_owned())
        .unwrap_or_default()
}

/// A PPtr field's m_PathID (0 when missing).
fn path_id(t: &Value, k: &str) -> i128 {
    t.get(k)
        .and_then(|p| p.get("m_PathID"))
        .and_then(Value::as_int)
        .unwrap_or(0)
}

fn cfg_num(c: &Json, k: &str) -> Option<Num> {
    c.get(k).and_then(Num::of_json)
}

fn cfg_strs(c: &Json, k: &str) -> Vec<String> {
    match c.get(k) {
        Some(Json::List(l)) => l.iter().map(Json::to_str).collect(),
        Some(Json::Str(s)) => s.chars().map(String::from).collect(),
        _ => Vec::new(),
    }
}

// ---------------------------------------------------------------------------- shader variants

/// One pass's GLES3 variants: (subshader index, light mode, pass name, keyword sets; the Python
/// keeps the SubShader's LOD too, which nothing reads).
#[derive(Clone, Debug)]
struct PassVariants {
    si: usize,
    mode: String,
    name: String,
    kws: Vec<BTreeSet<String>>,
}

/// The GLES3 variants of every shader in the data (quality.Variants).
#[derive(Default)]
struct Variants {
    shaders: Vec<(String, Vec<PassVariants>)>,
}

fn ints_of(v: &Value) -> Option<Vec<i64>> {
    match v {
        Value::Bytes(b) => Some(b.iter().map(|&x| x as i64).collect()),
        Value::Array(a) => a.iter().map(Value::as_i64).collect(),
        _ => None,
    }
}

fn lengths_of(v: &Value) -> Option<Lengths> {
    let a = v.as_array()?;
    if a.first().is_some_and(|x| x.as_array().is_some()) {
        Some(Lengths::Nested(
            a.iter().map(ints_of).collect::<Option<_>>()?,
        ))
    } else {
        Some(Lengths::Flat(ints_of(v)?))
    }
}

fn blob_fields(t: &Value) -> Option<BlobFields> {
    Some(BlobFields {
        platforms: ints_of(t.get("platforms")?)?,
        offsets: lengths_of(t.get("offsets")?)?,
        compressed_lengths: lengths_of(t.get("compressedLengths")?)?,
        decompressed_lengths: lengths_of(t.get("decompressedLengths")?)?,
        compressed_blob: t.get("compressedBlob")?.as_bytes()?.to_vec(),
        stage_counts: match t.get("stageCounts") {
            Some(v) => Some(ints_of(v)?),
            None => None,
        },
    })
}

const GLES3_TYPES: [i64; 3] = [blob::GPU_GLES31AEP, blob::GPU_GLES31, blob::GPU_GLES3];

/// Every subprogram list of a pass's stage: m_SubPrograms, then each tier's m_PlayerSubPrograms
/// (with that tier's m_ParameterBlobIndices).
fn stage_lists(prog: &Value) -> Vec<(&Vec<Value>, Option<Vec<i64>>)> {
    let mut out = Vec::new();
    if let Some(Value::Array(l)) = prog.get("m_SubPrograms") {
        out.push((l, None));
    }
    if let Some(Value::Array(tiers)) = prog.get("m_PlayerSubPrograms") {
        let pbi = prog.get("m_ParameterBlobIndices").and_then(Value::as_array);
        for (k, l) in tiers.iter().enumerate() {
            if let Value::Array(l) = l {
                let p = pbi.and_then(|p| p.get(k)).and_then(ints_of);
                out.push((l, p));
            }
        }
    }
    out
}

/// The blob indices of the GLES3 platform's parameter and program entries (convert.param_indices,
/// program_indices).
fn gles3_indices(pf: &Value) -> (BTreeSet<i64>, BTreeSet<i64>) {
    let (mut params, mut programs) = (BTreeSet::new(), BTreeSet::new());
    for ss in pf
        .get("m_SubShaders")
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
                let Some(prog) = p.get(st) else { continue };
                // (subprogram_lists: a program without fields counts as none)
                if prog.as_struct().is_some_and(|s| s.fields.is_empty()) {
                    continue;
                }
                for (lst, pbi) in stage_lists(prog) {
                    for (i, sp) in lst.iter().enumerate() {
                        let t = sp
                            .get("m_GpuProgramType")
                            .and_then(Value::as_i64)
                            .unwrap_or(-1);
                        if !GLES3_TYPES.contains(&t) {
                            continue;
                        }
                        if let Some(b) = sp.get("m_BlobIndex").and_then(Value::as_i64) {
                            programs.insert(b);
                        }
                        if let Some(x) = pbi.as_ref().and_then(|p| p.get(i)) {
                            params.insert(*x);
                        }
                    }
                }
            }
        }
    }
    (params, programs)
}

impl Variants {
    /// Adds one Shader tree's GLES3 variants (an unreadable blob: the shader has none).
    fn add(&mut self, tree: &Value) -> Result<()> {
        let pf = tree
            .get("m_ParsedForm")
            .ok_or_else(|| Error::Value("KeyError: 'm_ParsedForm'".into()))?;
        let plats = tree.get("platforms").and_then(ints_of).unwrap_or_default();
        if !plats.contains(&blob::PLATFORM_GLES3PLUS) {
            return Ok(());
        }
        let (params, programs) = gles3_indices(pf);
        let entries = match blob_fields(tree)
            .and_then(|f| ShaderBlobs::read(&f).ok())
            .and_then(|b| {
                b.entries(blob::PLATFORM_GLES3PLUS, &params, Some(&programs))
                    .ok()
            }) {
            Some(e) => e,
            None => return Ok(()),
        };
        let mut out = Vec::new();
        for (si, ss) in pf
            .get("m_SubShaders")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .enumerate()
        {
            for p in ss
                .get("m_Passes")
                .and_then(Value::as_array)
                .into_iter()
                .flatten()
            {
                let state = p.get("m_State");
                let mut mode = String::new();
                if let Some(Value::Array(tags)) = state.and_then(|s| s.path(&["m_Tags", "tags"])) {
                    for kv in tags {
                        if let Value::Pair(kv) = kv {
                            let k = String::from_utf8_lossy(kv.0.as_bytes().unwrap_or(&[]))
                                .to_uppercase();
                            if k == "LIGHTMODE" {
                                mode = String::from_utf8_lossy(kv.1.as_bytes().unwrap_or(&[]))
                                    .to_uppercase();
                            }
                        }
                    }
                }
                let mut kws: Vec<BTreeSet<String>> = Vec::new();
                for st in STAGES {
                    let Some(prog) = p.get(st) else { continue };
                    for (lst, _) in stage_lists(prog) {
                        for sp in lst {
                            let t = sp
                                .get("m_GpuProgramType")
                                .and_then(Value::as_i64)
                                .unwrap_or(-1);
                            if !GLES3_TYPES.contains(&t) {
                                continue;
                            }
                            let bi = sp
                                .get("m_BlobIndex")
                                .and_then(Value::as_i64)
                                .ok_or_else(|| Error::Value("m_BlobIndex".into()))?;
                            let e = entries.get(bi as usize).ok_or_else(|| {
                                Error::Value("IndexError: list index out of range".into())
                            })?;
                            let mut set = BTreeSet::new();
                            if let Some(Slot::Program(e)) = e {
                                set.extend(e.keywords.iter().cloned());
                                set.extend(e.local_keywords.iter().flatten().cloned());
                            }
                            if !kws.contains(&set) {
                                kws.push(set);
                            }
                        }
                    }
                }
                out.push(PassVariants {
                    si,
                    mode,
                    name: state.map(|s| text(s, "m_Name")).unwrap_or_default(),
                    kws,
                });
            }
        }
        let name = text(pf, "m_Name");
        match self.shaders.iter_mut().find(|(n, _)| *n == name) {
            Some(e) => e.1 = out,
            None => self.shaders.push((name, out)),
        }
        Ok(())
    }

    fn passes(&self) -> impl Iterator<Item = (&str, &PassVariants)> {
        self.shaders
            .iter()
            .flat_map(|(n, ps)| ps.iter().map(move |p| (n.as_str(), p)))
    }

    /// Shaders with a variant of `kw` whose twin without it is not in the data.
    fn without_twin(&self, kw: &str) -> Vec<String> {
        let mut bad = BTreeSet::new();
        for (name, p) in self.passes() {
            for k in &p.kws {
                if k.contains(kw) {
                    let mut rest = k.clone();
                    rest.remove(kw);
                    if !p.kws.contains(&rest) {
                        bad.insert(name.to_string());
                    }
                }
            }
        }
        bad.into_iter().collect()
    }

    /// (ForwardBase passes, of them with VERTEXLIGHT_ON variants).
    fn vertex_lights(&self) -> (usize, usize) {
        let (mut n, mut v) = (0, 0);
        for (_, p) in self.passes().filter(|(_, p)| p.mode == "FORWARDBASE") {
            n += 1;
            v += p.kws.iter().any(|k| k.contains("VERTEXLIGHT_ON")) as usize;
        }
        (n, v)
    }

    fn keyword_present(&self, kw: &str) -> bool {
        self.passes()
            .any(|(_, p)| p.kws.iter().any(|k| k.contains(kw)))
    }

    fn of(&self, name: &str) -> Option<&Vec<PassVariants>> {
        self.shaders.iter().find(|(n, _)| n == name).map(|(_, p)| p)
    }
}

/// Moves a Standard shader's first SubShader below LOD 300 with GLES3 programs for its forward
/// and shadow passes to the front: the (from, to) LODs, or None.
fn standard_reorder(tree: &mut Value) -> Result<Option<(String, String)>> {
    let name = tree
        .get("m_ParsedForm")
        .map(|pf| text(pf, "m_Name"))
        .unwrap_or_default();
    if !STANDARD.contains(&name.as_str()) {
        return Ok(None);
    }
    let lod = |ss: &Value| ss.get("m_LOD").and_then(Value::as_int).unwrap_or(0);
    let n = tree
        .path(&["m_ParsedForm", "m_SubShaders"])
        .and_then(Value::as_array)
        .map_or(0, Vec::len);
    let subs = tree
        .path(&["m_ParsedForm", "m_SubShaders"])
        .and_then(Value::as_array);
    let Some(subs) = subs else { return Ok(None) };
    if n < 2 || lod(&subs[0]) < 300 {
        return Ok(None);
    }
    let mut v = Variants::default();
    v.add(tree)?;
    let info = v.of(&name).cloned().unwrap_or_default();
    let subs = tree
        .path(&["m_ParsedForm", "m_SubShaders"])
        .and_then(Value::as_array)
        .unwrap();
    for i in 1..n {
        if lod(&subs[i]) >= 300 {
            continue;
        }
        let modes: BTreeSet<&str> = info
            .iter()
            .filter(|p| p.si == i && !p.kws.is_empty())
            .map(|p| p.mode.as_str())
            .collect();
        if ["FORWARDBASE", "FORWARDADD", "SHADOWCASTER"]
            .iter()
            .all(|m| modes.contains(m))
        {
            let show_lod = |ss: &Value| ss.get("m_LOD").map_or("None".into(), show);
            let lods = (show_lod(&subs[0]), show_lod(&subs[i]));
            let subs = tree
                .path_mut(&["m_ParsedForm", "m_SubShaders"])
                .and_then(Value::as_array_mut)
                .unwrap();
            let s = subs.remove(i);
            subs.insert(0, s);
            return Ok(Some(lods));
        }
        return Ok(None);
    }
    Ok(None)
}

// ---------------------------------------------------------------------------- the game's own code

fn volume_packages(n: &str) -> Option<[&'static str; 2]> {
    match n {
        "Cinemachine.dll" | "Unity.Cinemachine.dll" => {
            Some(["CinemachineVolumeSettings", "CinemachinePostProcessing"])
        }
        _ => None,
    }
}

/// The game's assemblies that reference URP's volumes or post components: (assembly, type names).
fn drives_volumes(managed: &Path, classes: &BTreeSet<String>) -> Vec<(String, Vec<String>)> {
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(managed) else {
        return out;
    };
    let classes: BTreeSet<&str> = classes
        .iter()
        .map(|c| c.rsplit('.').next().unwrap_or(c))
        .collect();
    let mut watch: BTreeSet<String> = [
        "UnityEngine.Rendering.Volume",
        "UnityEngine.Rendering.VolumeProfile",
        "UnityEngine.Rendering.VolumeComponent",
        "UnityEngine.Rendering.VolumeManager",
        "UnityEngine.Rendering.VolumeStack",
    ]
    .iter()
    .map(|s| s.to_string())
    .collect();
    watch.extend(
        POST_CLASSES
            .iter()
            .map(|c| format!("UnityEngine.Rendering.Universal.{c}")),
    );
    let mut names: Vec<String> = rd
        .filter_map(|e| e.ok().map(|e| e.file_name().to_string_lossy().into_owned()))
        .collect();
    names.sort();
    for n in names {
        if !n.ends_with(".dll")
            || [
                "UnityEngine",
                "Unity.RenderPipelines",
                "System",
                "mscorlib",
                "netstandard",
                "Mono.",
                "Microsoft.",
            ]
            .iter()
            .any(|p| n.starts_with(p))
        {
            continue;
        }
        let refs = (|| -> clrmeta::Result<BTreeSet<String>> {
            let a = clrmeta::Assembly::open(managed.join(&n))?;
            (1..=a.count(clrmeta::TYPEREF))
                .map(|r| a.typeref_name(r))
                .collect()
        })();
        let Ok(refs) = refs else { continue };
        let hit: Vec<String> = refs.intersection(&watch).cloned().collect();
        if !hit.is_empty()
            && volume_packages(&n).is_none_or(|v| v.iter().any(|c| classes.contains(c)))
        {
            out.push((n, hit));
        }
    }
    out
}

// ---------------------------------------------------------------------------- the step

#[derive(Default)]
struct Plan {
    done: Vec<String>,
    kept: Vec<String>,
}

/// QualitySettings fields -> the caps' names ua_host knows (runtime/host/quality.c).
const HOST_CAPS: [(&str, &str); 7] = [
    ("pixelLightCount", "pixel_lights"),
    ("shadows", "shadows"),
    ("shadowResolution", "shadow_resolution"),
    ("shadowCascades", "shadow_cascades"),
    ("antiAliasing", "msaa"),
    ("anisotropicTextures", "anisotropic"),
    ("softParticles", "soft_particles"),
];

/// The caps as ua_host reads them: "pixel_lights=1,msaa=0,...".
fn host_caps(q: &[(String, Num)]) -> String {
    HOST_CAPS
        .iter()
        .filter_map(|(k, h)| {
            q.iter().find(|(x, _)| x == k).map(|(_, v)| {
                let i = match v {
                    Num::I(i) => *i,
                    Num::F(f) => f.trunc() as i128,
                };
                format!("{h}={i}")
            })
        })
        .collect::<Vec<_>>()
        .join(",")
}

/// Two caps strings as one: every cap either names, the lower value where both do.
fn merge_caps(old: &str, new: &str) -> Result<String> {
    let mut caps: HashMap<String, i128> = HashMap::new();
    for s in [old, new] {
        for kv in s.split(',').filter(|x| !x.is_empty()) {
            let (k, v) = kv.split_once('=').unwrap_or((kv, ""));
            let v: i128 = v.trim().parse().map_err(|_| {
                Error::Value(format!("invalid literal for int() with base 10: {v:?}"))
            })?;
            let e = caps.entry(k.to_string()).or_insert(v);
            *e = (*e).min(v);
        }
    }
    Ok(HOST_CAPS
        .iter()
        .filter_map(|(_, h)| caps.get(*h).map(|v| format!("{h}={v}")))
        .collect::<Vec<_>>()
        .join(","))
}

/// (file, path id) of an object.
type ObjKey = (String, i64);
/// A URP script's (class name, assembly, full name).
type Script = (String, String, String);

/// What the data holds that the profile looks at (quality.Scan).
#[derive(Default)]
struct Scan {
    variants: Variants,
    quality: Option<Value>,
    graphics: Option<Value>,
    srp: bool,
    lights: Vec<(String, i64, Value)>,
    cameras: Vec<i128>,
    std_files: BTreeSet<String>,
    mbs: Vec<(ObjKey, Script)>,
    volume_scripts: BTreeSet<(String, i64)>,
    clip_scripts: Vec<(String, Option<String>, i128)>,
    script_classes: BTreeSet<String>,
    shaders_seen: usize,
    unity: Option<(u64, u64)>,
    qcaps: Vec<(String, Num)>,
    post_ok: bool,
}

impl Scan {
    fn mb(&self, rel: &str, pid: i64) -> Option<&(String, String, String)> {
        self.mbs
            .iter()
            .find(|((r, p), _)| r == rel && *p == pid)
            .map(|(_, v)| v)
    }
}

fn basename(rel: &str) -> String {
    rel.rsplit(['/', '\\']).next().unwrap_or(rel).to_string()
}

/// The file name a PPtr's file id names (0: the file itself), or None.
fn external(sf: &unityfs::SerializedFile, file_id: i128, rel: &str) -> Option<String> {
    if file_id == 0 {
        return Some(basename(rel));
    }
    let i = usize::try_from(file_id - 1).ok()?;
    let p = String::from_utf8_lossy(&sf.externals.get(i)?.path).replace('\\', "/");
    Some(p.rsplit('/').next().unwrap_or(&p).to_string())
}

/// The MonoScripts' files UnityPy finds for a PPtr (PPtr.deref and Environment.find_file).
struct Scripts {
    open: HashMap<PathBuf, Option<DataFile>>,
}

impl Scripts {
    /// The file an external reference names, from the folder of the file that holds it.
    fn find(dir: &Path, path: &str) -> Option<PathBuf> {
        let mut clean = path;
        if let Some(x) = clean.strip_prefix("archive:/") {
            clean = x;
        }
        if let Some(x) = clean.strip_prefix("assets/") {
            clean = x;
        }
        let clean = clean.rsplit('/').next().unwrap_or(clean).to_lowercase();
        let p = dir.join(&clean);
        if p.is_file() {
            return Some(p);
        }
        // (local_files: every file under the folder, whose path ends with the name)
        let mut files = Vec::new();
        let mut stack = vec![dir.to_path_buf()];
        while let Some(d) = stack.pop() {
            let Ok(rd) = std::fs::read_dir(&d) else {
                continue;
            };
            let mut es: Vec<PathBuf> = rd.filter_map(|e| e.ok().map(|e| e.path())).collect();
            es.sort();
            for e in es.into_iter().rev() {
                if e.is_dir() {
                    stack.push(e);
                } else {
                    files.push(e);
                }
            }
        }
        files.sort();
        if let Some(f) = files.iter().find(|f| f.to_string_lossy().ends_with(&clean)) {
            return Some(f.clone());
        }
        files.into_iter().find(|f| {
            let s = f
                .file_name()
                .map(|n| n.to_string_lossy().to_lowercase())
                .unwrap_or_default();
            let full = f.with_file_name(&s);
            full.to_string_lossy().ends_with(&clean)
        })
    }

    /// (assembly name, full class name) of a MonoScript, or None (monotree.script_of).
    fn script_of(
        &mut self,
        job: &Job,
        f: &DataFile,
        here: &Path,
        file_id: i128,
        pid: i128,
    ) -> Option<(String, String)> {
        if pid == 0 {
            return None;
        }
        let tree = if file_id == 0 {
            f.sf.read_typetree(pid as i64).ok()?
        } else {
            let ext = f.sf.externals.get(usize::try_from(file_id - 1).ok()?)?;
            let path = Self::find(here.parent()?, &String::from_utf8_lossy(&ext.path))?;
            let df = self
                .open
                .entry(path.clone())
                .or_insert_with(|| job.open(&path, Default::default()).ok());
            df.as_ref()?.sf.read_typetree(pid as i64).ok()?
        };
        let ns = text(&tree, "m_Namespace");
        let cls = text(&tree, "m_ClassName");
        let full = if ns.is_empty() {
            cls
        } else {
            format!("{ns}.{cls}")
        };
        Some((text(&tree, "m_AssemblyName"), full))
    }
}

/// A MonoBehaviour's m_Script (file id, path id), read with its base tree (check_read=False).
fn mb_script(sf: &unityfs::SerializedFile, pid: i64) -> Option<(i128, i128)> {
    let tree = sf.typetree_of(pid).ok()?;
    let data = sf.original_bytes(pid).ok()?;
    let (v, _) = unityfs::value::read(&tree, &data, sf.header.big, None).ok()?;
    let s = v.get("m_Script")?;
    Some((int_field(s, "m_FileID")?, int_field(s, "m_PathID")?))
}

fn data_files(job: &Job) -> Vec<String> {
    let mut files = job.list("", |n| {
        n.ends_with(".assets") || n == "globalgamemanagers" || super::shaders::is_level(n)
    });
    files.extend(job.list("Resources", |n| n == "unity_builtin_extra"));
    files
}

fn scan(job: &Job, files: &[String], c: &Json) -> Result<Scan> {
    let mut s = Scan {
        unity: crate::version::of_data(&job.src).ok().map(|v| v.line),
        ..Default::default()
    };
    let post = c.get("post").is_some_and(Json::truthy);
    #[allow(clippy::type_complexity)]
    let mut scripts: HashMap<(Option<String>, i128), Option<(String, String)>> = HashMap::new();
    let mut resolver = Scripts {
        open: HashMap::new(),
    };
    for rel in files {
        let here = job.path_in(rel);
        let f = job.open(&here, Default::default())?;
        for (pid, n) in objects(&f) {
            match n.as_str() {
                "Shader" => {
                    let t = f.sf.read_typetree(pid)?;
                    s.shaders_seen += 1;
                    s.variants.add(&t)?;
                    let name = t
                        .get("m_ParsedForm")
                        .map(|pf| text(pf, "m_Name"))
                        .unwrap_or_default();
                    if STANDARD.contains(&name.as_str()) {
                        s.std_files.insert(rel.clone());
                    }
                }
                "QualitySettings" => {
                    let q = typetrees::read(&f.sf, pid, &[])?.0;
                    if let Some(Value::Array(levels)) = q.get("m_QualitySettings") {
                        s.srp |= levels
                            .iter()
                            .any(|lv| path_id(lv, "customRenderPipeline") != 0);
                    }
                    s.quality = Some(q);
                }
                "GraphicsSettings" => {
                    let g = typetrees::read(&f.sf, pid, &[])?.0;
                    s.srp |= path_id(&g, "m_CustomRenderPipeline") != 0;
                    s.graphics = Some(g);
                }
                "Light" => {
                    if let Ok(t) = f.sf.read_typetree(pid) {
                        s.lights.push((rel.clone(), pid, t));
                    }
                }
                "Camera" => {
                    if let Ok(t) = f.sf.read_typetree(pid) {
                        s.cameras
                            .push(int_field(&t, "m_RenderingPath").unwrap_or(-1));
                    }
                }
                _ if !s.srp => continue,
                "MonoScript" => {
                    let Ok(m) = f.sf.read_typetree(pid) else {
                        continue;
                    };
                    let asm = text(&m, "m_AssemblyName").replace(".dll", "");
                    let cls = text(&m, "m_ClassName");
                    if URP_ASSETS.contains(&asm.as_str())
                        && (POST_CLASSES.contains(&cls.as_str())
                            || cls == "Volume"
                            || cls == "VolumeProfile")
                    {
                        s.volume_scripts.insert((basename(rel), pid));
                    }
                }
                "MonoBehaviour" => {
                    let Some((fid, spid)) = mb_script(&f.sf, pid) else {
                        continue;
                    };
                    let key = (external(&f.sf, fid, rel), spid);
                    if !scripts.contains_key(&key) {
                        let sc = resolver.script_of(job, &f, &here, fid, spid);
                        if let Some(sc) = &sc {
                            s.script_classes.insert(sc.1.clone());
                        }
                        scripts.insert(key.clone(), sc);
                    }
                    if let Some(Some((asm, full))) = scripts.get(&key) {
                        if URP_ASSETS.contains(&asm.replace(".dll", "").as_str()) {
                            let cls = full.rsplit('.').next().unwrap_or(full).to_string();
                            s.mbs
                                .push(((rel.clone(), pid), (cls, asm.clone(), full.clone())));
                        }
                    }
                }
                "AnimationClip" if post => {
                    let Ok(t) = f.sf.read_typetree(pid) else {
                        continue;
                    };
                    let binds = t
                        .path(&["m_ClipBindingConstant", "genericBindings"])
                        .and_then(Value::as_array);
                    for b in binds.into_iter().flatten() {
                        let Some(sp) = b.get("script") else { continue };
                        let spid = int_field(sp, "m_PathID").unwrap_or(0);
                        if spid != 0 {
                            let fid = int_field(sp, "m_FileID").unwrap_or(0);
                            s.clip_scripts.push((
                                text(&t, "m_Name"),
                                external(&f.sf, fid, rel),
                                spid,
                            ));
                        }
                    }
                }
                _ => {}
            }
        }
    }
    Ok(s)
}

fn levels(s: &Scan) -> &[Value] {
    s.quality
        .as_ref()
        .and_then(|q| q.get("m_QualitySettings"))
        .and_then(Value::as_array)
        .map_or(&[], Vec::as_slice)
}

fn lv_num(lv: &Value, k: &str) -> Num {
    lv.get(k).and_then(Num::of).unwrap_or(Num::I(0))
}

fn list5(names: &[String]) -> String {
    format!(
        "{}{}",
        names.iter().take(5).cloned().collect::<Vec<_>>().join(", "),
        if names.len() > 5 { " ..." } else { "" }
    )
}

type Edits = BTreeMap<String, BTreeSet<&'static str>>;

/// {rel: what to edit there} from the scan; what is left alone goes to plan.kept (quality._decide).
fn decide(job: &Job, s: &mut Scan, c: &Json, plan: &mut Plan) -> Edits {
    let mut edits: Edits = BTreeMap::new();
    let lvs = levels(s).to_vec();
    let mut gq: Vec<(String, Num)> = Vec::new();
    if !lvs.is_empty() {
        if let Some(pl) = cfg_num(c, "pixel_lights") {
            if !s.srp && lvs.iter().any(|lv| lv_num(lv, "pixelLightCount").gt(pl)) {
                let (n, v) = s.variants.vertex_lights();
                if n > 0 && v == 0 {
                    plan.kept.push(format!(
                        "pixel lights: none of the {n} ForwardBase passes has VERTEXLIGHT_ON variants (the build strips vertex lights; the other lights would be spherical harmonics only)"
                    ));
                } else {
                    gq.push(("pixelLightCount".into(), pl));
                }
            }
        }
        if c.get("hard_shadows").is_some_and(Json::truthy)
            && !s.srp
            && lvs.iter().any(|lv| {
                lv_num(lv, "shadows") == Num::I(2) || lv_num(lv, "shadows") == Num::F(2.0)
            })
        {
            let bad = s.variants.without_twin("SHADOWS_SOFT");
            if !bad.is_empty() {
                plan.kept
                    .push(format!("soft shadows: no hard variant in {}", list5(&bad)));
            } else {
                gq.push(("shadows".into(), Num::I(1)));
            }
        }
        for (k, ck) in [
            ("shadowResolution", "shadow_resolution"),
            ("shadowCascades", "shadow_cascades"),
            ("antiAliasing", "msaa"),
            ("anisotropicTextures", "anisotropic"),
        ] {
            if let Some(v) = cfg_num(c, ck) {
                gq.push((k.into(), v));
            }
        }
        // (`c['soft_particles'] is False`: the bool only)
        if matches!(c.get("soft_particles"), Some(Json::Bool(false)))
            && lvs.iter().any(|lv| {
                lv.get("softParticles")
                    .and_then(Num::of)
                    .is_some_and(|x| x.f() != 0.0)
            })
        {
            let bad = s.variants.without_twin("SOFTPARTICLES_ON");
            if !bad.is_empty() {
                plan.kept.push(format!(
                    "soft particles: no variant without SOFTPARTICLES_ON in {}",
                    list5(&bad)
                ));
            } else {
                gq.push(("softParticles".into(), Num::I(0)));
            }
        }
    }
    s.qcaps = gq.clone();
    if !gq.is_empty() || c.get("hdr_format").is_some_and(Json::truthy) {
        edits
            .entry("globalgamemanagers".into())
            .or_default()
            .insert("settings");
    }
    for (rel, _, l) in &s.lights {
        let mut l = l.clone();
        if light_edit(&mut l, &gq, c, false) {
            edits.entry(rel.clone()).or_default().insert("lights");
        }
    }
    if c.get("standard_lod150").is_some_and(Json::truthy) && !s.srp && !s.std_files.is_empty() {
        let mut paths: Vec<Option<i128>> = Vec::new();
        if let Some(g) = s.graphics.as_ref().and_then(Value::as_struct) {
            for (k, t) in &g.fields {
                if k.starts_with("m_TierSettings") && matches!(t, Value::Struct(_)) {
                    paths.push(int_field(t, "renderingPath"));
                }
            }
        }
        let deferred = paths
            .iter()
            .flatten()
            .chain(s.cameras.iter())
            .any(|p| *p == 2 || *p == 3);
        if deferred {
            plan.kept
                .push("Standard shaders: deferred rendering (their LOD-150 SubShader has no deferred pass)".into());
        } else {
            for rel in &s.std_files {
                edits.entry(rel.clone()).or_default().insert("shaders");
            }
        }
    }
    if s.srp && !s.mbs.is_empty() {
        let post = cfg_strs(c, "post");
        s.post_ok = c.get("post").is_some_and(Json::truthy);
        if s.post_ok {
            let drive = drives_volumes(&job.src.join("Managed"), &s.script_classes);
            if !drive.is_empty() {
                s.post_ok = false;
                plan.kept.push(format!(
                    "post effects: the game's code drives volumes ({})",
                    drive
                        .iter()
                        .map(|(a, t)| format!(
                            "{a}: {}",
                            t.iter()
                                .map(|x| x.rsplit('.').next().unwrap_or(x).to_string())
                                .collect::<Vec<_>>()
                                .join(", ")
                        ))
                        .collect::<Vec<_>>()
                        .join("; ")
                ));
            }
        }
        if s.post_ok {
            let clips: BTreeSet<String> = s
                .clip_scripts
                .iter()
                .filter(|(_, f, pid)| {
                    f.as_ref().is_some_and(|f| {
                        i64::try_from(*pid)
                            .is_ok_and(|p| s.volume_scripts.contains(&(f.clone(), p)))
                    })
                })
                .map(|(n, _, _)| n.clone())
                .collect();
            if !clips.is_empty() {
                s.post_ok = false;
                plan.kept.push(format!(
                    "post effects: animations drive volumes ({})",
                    clips.iter().take(4).cloned().collect::<Vec<_>>().join(", ")
                ));
            }
        }
        for ((rel, _), (cls, _, _)) in &s.mbs {
            if cls == URP_ASSET || cls == SSAO || (s.post_ok && post.contains(cls)) {
                edits.entry(rel.clone()).or_default().insert("urp");
            }
        }
    }
    edits
}

/// Whether a Light changes (and with apply, changes it): quality._light_edit.
fn light_edit(l: &mut Value, gq: &[(String, Num)], c: &Json, apply: bool) -> bool {
    let mut changed = false;
    let realtime = l.get("m_Lightmapping").and_then(Value::as_int).unwrap_or(4) != BAKED;
    let sh_type = l
        .path(&["m_Shadows", "m_Type"])
        .and_then(Value::as_int)
        .unwrap_or(0);
    if gq.iter().any(|(k, _)| k == "pixelLightCount")
        && realtime
        && l.get("m_RenderMode").and_then(Value::as_int) == Some(AUTO)
        && (l.get("m_Type").and_then(Value::as_int) == Some(SPOT)
            || sh_type != 0
            || path_id(l, "m_Cookie") != 0)
    {
        changed = true;
        if apply {
            let old = l.get("m_RenderMode").cloned().unwrap_or(Value::I32(0));
            let _ = l.set("m_RenderMode", like(&old, Num::I(IMPORTANT)));
        }
    }
    // (sh.get('m_Resolution', -1))
    let r = match l.path(&["m_Shadows", "m_Resolution"]) {
        Some(v) => Num::of(v),
        None => Some(Num::I(-1)),
    };
    if let (Some(cap), Some(r)) = (cfg_num(c, "shadow_resolution"), r) {
        if r.gt(cap) {
            changed = true;
            if apply {
                if let Some(sh) = l.get_mut("m_Shadows") {
                    let old = sh.get("m_Resolution").cloned().unwrap_or(Value::I32(0));
                    let _ = sh.set("m_Resolution", like(&old, cap));
                }
            }
        }
    }
    changed
}

/// `_cap`: a field capped at `cap` (nothing when it is not over it).
fn cap(t: &mut Value, key: &str, cap: Option<Num>, ch: &mut Vec<String>) {
    let Some(cap) = cap else { return };
    let Some(old) = t.get(key).cloned() else {
        return;
    };
    let Some(v) = Num::of(&old) else { return };
    if !v.gt(cap) {
        return;
    }
    let capv = like(&old, cap);
    let cap_s = match cap {
        Num::I(i) => i.to_string(),
        Num::F(f) => crate::pyjson::float_repr(f),
    };
    ch.push(format!("{key} {} -> {cap_s}", show(&old)));
    let _ = t.set(key, capv);
}

fn in_set(t: &Value, k: &str, vals: &[i128]) -> bool {
    match t.get(k) {
        None => true,
        Some(v) => Num::of(v).is_some_and(|n| match n {
            Num::I(i) => vals.contains(&i),
            Num::F(f) => vals.iter().any(|&x| x as f64 == f),
        }),
    }
}

/// Whether the fields the step may edit hold values they can hold (quality.plausible).
fn plausible(cls: &str, t: &Value) -> bool {
    let t = if cls == SSAO {
        match t.get("m_Settings") {
            Some(s @ Value::Struct(_)) => {
                if let Some(n) = s.get("SampleCount").and_then(Num::of) {
                    if !(-1.0..=64.0).contains(&n.f()) {
                        return false;
                    }
                }
                s
            }
            _ => return false,
        }
    } else {
        if cls == URP_ASSET {
            if let Some(n) = t.get("m_RenderScale").and_then(Num::of) {
                if !(0.05..=2.0).contains(&n.f()) {
                    return false;
                }
            }
        } else if let Some(a) = t.get("active") {
            if !Num::of(a).is_some_and(|n| n.f() == 0.0 || n.f() == 1.0) {
                return false;
            }
        }
        t
    };
    let sizes = [256, 512, 1024, 2048, 4096, 8192];
    match cls {
        URP_ASSET => {
            in_set(t, "m_MSAA", &[1, 2, 4, 8])
                && in_set(t, "m_MainLightShadowmapResolution", &sizes)
                && in_set(t, "m_AdditionalLightsShadowmapResolution", &sizes)
                && in_set(t, "m_SoftShadowQuality", &[0, 1, 2, 3])
                && in_set(t, "m_SupportsHDR", &[0, 1])
        }
        SSAO => {
            in_set(t, "Downsample", &[0, 1])
                && in_set(t, "BlurQuality", &[0, 1, 2])
                && in_set(t, "Samples", &[0, 1, 2])
                && in_set(t, "Source", &[0, 1])
                && in_set(t, "NormalSamples", &[0, 1, 2])
                && in_set(t, "AfterOpaque", &[0, 1])
        }
        _ => true,
    }
}

const SAMPLES: [&str; 3] = ["high", "medium", "low"];

fn sample_kw(i: i128) -> Option<&'static str> {
    match i {
        0 => Some("_SAMPLE_COUNT_HIGH"),
        1 => Some("_SAMPLE_COUNT_MEDIUM"),
        2 => Some("_SAMPLE_COUNT_LOW"),
        _ => None,
    }
}

fn depth_kw(i: i128) -> Option<&'static str> {
    match i {
        0 => Some("_SOURCE_DEPTH_LOW"),
        1 => Some("_SOURCE_DEPTH_MEDIUM"),
        2 => Some("_SOURCE_DEPTH_HIGH"),
        _ => None,
    }
}

fn source_kw(source: i128, normals: i128) -> Option<&'static str> {
    if source == 1 {
        Some("_SOURCE_DEPTH_NORMALS")
    } else {
        depth_kw(normals)
    }
}

fn is_occlusion_kw(k: &str) -> bool {
    [
        "_SAMPLE_COUNT_HIGH",
        "_SAMPLE_COUNT_MEDIUM",
        "_SAMPLE_COUNT_LOW",
        "_SOURCE_DEPTH_LOW",
        "_SOURCE_DEPTH_MEDIUM",
        "_SOURCE_DEPTH_HIGH",
        "_SOURCE_DEPTH_NORMALS",
    ]
    .contains(&k)
}

/// URP's SSAO settings made cheaper in place, as far as the SSAO shader's variants allow.
fn ssao_settings(st: &mut Value, variants: Option<&Vec<PassVariants>>) -> Vec<String> {
    let mut ch = Vec::new();
    let Some(variants) = variants.filter(|v| !v.is_empty()) else {
        return ch;
    };
    if let Some(d) = st.get("Downsample").cloned() {
        if !Num::of(&d).is_some_and(|n| n.f() != 0.0) {
            let _ = st.set(
                "Downsample",
                if matches!(d, Value::Bool(_)) {
                    Value::Bool(true)
                } else {
                    like(&d, Num::I(1))
                },
            );
            ch.push("SSAO downsampled".into());
        }
    }
    let names: BTreeSet<&str> = variants
        .iter()
        .filter(|p| !p.kws.is_empty())
        .map(|p| p.name.as_str())
        .collect();
    let bq = st.get("BlurQuality").and_then(Value::as_int).unwrap_or(2);
    if bq < 2 && names.contains("SSAO_Kawase") {
        let old = ["bilateral", "gaussian"]
            .get(bq as usize)
            .copied()
            .unwrap_or("?");
        ch.push(format!("SSAO blur {old} -> kawase"));
        let o = st.get("BlurQuality").cloned().unwrap_or(Value::I32(0));
        let _ = st.set("BlurQuality", like(&o, Num::I(2)));
    }
    let samples = st.get("Samples").and_then(Value::as_int);
    if let Some(cur) = samples.filter(|s| sample_kw(*s).is_some()) {
        let occ: Vec<&BTreeSet<String>> = variants
            .iter()
            .filter(|p| p.name == "SSAO_Occlusion")
            .flat_map(|p| p.kws.iter())
            .collect();
        let rest = |k: &BTreeSet<String>| -> BTreeSet<String> {
            k.iter().filter(|x| !is_occlusion_kw(x)).cloned().collect()
        };
        let src = st.get("Source").and_then(Value::as_int).unwrap_or(1);
        let nrm = st.get("NormalSamples").and_then(Value::as_int).unwrap_or(1);
        let with = |sk: &str, srck: Option<&str>| -> BTreeSet<BTreeSet<String>> {
            occ.iter()
                .filter(|k| k.contains(sk) && srck.is_some_and(|x| k.contains(x)))
                .map(|k| rest(k))
                .collect()
        };
        let need = with(sample_kw(cur).unwrap(), source_kw(src, nrm));
        let mut sources = vec![(src, nrm)];
        for n in 0..3 {
            if (0, n) != (src, nrm) {
                sources.push((0, n));
            }
        }
        let mut samples_try = 2;
        while samples_try > cur {
            let got = sources.iter().copied().find(|&(s_, n_)| {
                !need.is_empty()
                    && need.is_subset(&with(sample_kw(samples_try).unwrap(), source_kw(s_, n_)))
            });
            if let Some(got) = got {
                ch.push(format!(
                    "SSAO samples {} -> {}{}",
                    SAMPLES[cur as usize],
                    SAMPLES[samples_try as usize],
                    if got.0 == 0 && src == 1 {
                        ", normals rebuilt from depth"
                    } else {
                        ""
                    }
                ));
                let o = st.get("Samples").cloned().unwrap_or(Value::I32(0));
                let _ = st.set("Samples", like(&o, Num::I(samples_try)));
                if got != (src, nrm) {
                    let o = st.get("Source").cloned().unwrap_or(Value::I32(0));
                    let _ = st.set("Source", like(&o, Num::I(got.0)));
                    let o = st.get("NormalSamples").cloned().unwrap_or(Value::I32(0));
                    let _ = st.set("NormalSamples", like(&o, Num::I(got.1)));
                }
                break;
            }
            samples_try -= 1;
        }
    } else if st
        .get("SampleCount")
        .and_then(Num::of)
        .is_some_and(|n| n.gt(Num::I(4)))
    {
        let o = st.get("SampleCount").cloned().unwrap();
        ch.push(format!("SSAO samples {} -> 4", show(&o)));
        let _ = st.set("SampleCount", like(&o, Num::I(4)));
    }
    ch
}

fn keyword_soft(s: &Scan) -> (bool, bool) {
    let hi = s.variants.keyword_present("_SHADOWS_SOFT_HIGH")
        || s.variants.keyword_present("_SHADOWS_SOFT_MEDIUM");
    (hi, s.variants.keyword_present("_SHADOWS_SOFT_LOW"))
}

/// quality._urp_edit: None when the object's fields read with values they cannot hold.
fn urp_edit(cls: &str, t: &mut Value, s: &Scan, c: &Json) -> Option<Vec<String>> {
    let mut ch = Vec::new();
    if !plausible(cls, t) {
        return None;
    }
    let post = cfg_strs(c, "post");
    if cls == URP_ASSET {
        if let Some(m) = cfg_num(c, "msaa") {
            let m = if m.gt(Num::I(1)) { m } else { Num::I(1) };
            cap(t, "m_MSAA", Some(m), &mut ch);
        }
        let usr = cfg_num(c, "urp_shadow_resolution");
        cap(t, "m_MainLightShadowmapResolution", usr, &mut ch);
        cap(t, "m_AdditionalLightsShadowmapResolution", usr, &mut ch);
        if c.get("urp_soft_shadows") == Some(&Json::Str("low".into()))
            && t.get("m_SoftShadowQuality")
                .and_then(Num::of)
                .unwrap_or(Num::I(0))
                .gt(Num::I(1))
        {
            let (hi, low) = keyword_soft(s);
            if !hi || low {
                cap(t, "m_SoftShadowQuality", Some(Num::I(1)), &mut ch);
            }
        }
        if let Some(rs) = c.get("render_scale").filter(|v| v.truthy()) {
            if let Some(rs) = rs.as_f64() {
                cap(t, "m_RenderScale", Some(Num::F(rs)), &mut ch);
            }
        }
    } else if cls == SSAO && c.get("ssao").is_some_and(Json::truthy) {
        if let Some(st) = t.get_mut("m_Settings") {
            ch.extend(ssao_settings(st, s.variants.of(SSAO_SHADER)));
        }
    } else if post.iter().any(|p| p == cls) && s.post_ok {
        if let Some(a) = t.get("active").cloned() {
            if Num::of(&a).is_some_and(|n| n.f() != 0.0) {
                let _ = t.set(
                    "active",
                    if matches!(a, Value::Bool(_)) {
                        Value::Bool(false)
                    } else {
                        like(&a, Num::I(0))
                    },
                );
                ch.push("off".into());
            }
        }
    }
    Some(ch)
}

fn edit(
    job: &Job,
    rel: &str,
    what: &BTreeSet<&'static str>,
    s: &Scan,
    c: &Json,
    plan: &mut Plan,
) -> Result<()> {
    let mut f = job.open(&job.path_in(rel), Default::default())?;
    let mut n = 0;
    let mut gen: Option<monotree::Generator> = None;
    let mut lights: BTreeMap<String, usize> = BTreeMap::new();
    let post = cfg_strs(c, "post");
    for (pid, t) in objects(&f) {
        match t.as_str() {
            "QualitySettings" if what.contains("settings") => {
                let (mut tree, node) = typetrees::read(&f.sf, pid, &["m_QualitySettings"])?;
                let mut ch = Vec::new();
                if let Some(Value::Array(lvs)) = tree.get_mut("m_QualitySettings") {
                    for lv in lvs.iter_mut() {
                        for (k, capv) in &s.qcaps {
                            let Some(old) = lv.get(k).cloned() else {
                                continue;
                            };
                            let Some(v) = Num::of(&old) else { continue };
                            if v.gt(*capv) {
                                let new = like(&old, *capv);
                                ch.push(format!(
                                    "{}.{k} {} -> {}",
                                    text(lv, "name"),
                                    show(&old),
                                    show(&new)
                                ));
                                let _ = lv.set(k, new);
                            }
                        }
                    }
                }
                if !ch.is_empty() {
                    typetrees::save(&mut f.sf, pid, &tree, node.as_ref())?;
                    n += 1;
                    plan.done
                        .push(format!("QualitySettings: {}", ch.join(", ")));
                }
            }
            "GraphicsSettings"
                if what.contains("settings") && c.get("hdr_format").is_some_and(Json::truthy) =>
            {
                let (mut tree, node) = typetrees::read(&f.sf, pid, &[])?;
                let mut k = 0;
                if let Some(st) = tree.as_struct_mut() {
                    for (name, v) in st.fields.iter_mut() {
                        if name.starts_with("m_TierSettings")
                            && matches!(v, Value::Struct(_))
                            && v.get("hdrMode").and_then(Value::as_int) == Some(1)
                        {
                            let o = v.get("hdrMode").cloned().unwrap();
                            let _ = v.set("hdrMode", like(&o, Num::I(2)));
                            k += 1;
                        }
                    }
                }
                if k > 0 {
                    typetrees::save(&mut f.sf, pid, &tree, node.as_ref())?;
                    n += 1;
                    plan.done.push(format!(
                        "GraphicsSettings: HDR targets R11G11B10 ({k} tiers)"
                    ));
                }
            }
            "Light" if what.contains("lights") => {
                let Ok(mut tree) = f.sf.read_typetree(pid) else {
                    continue;
                };
                let before = (
                    tree.get("m_RenderMode").cloned(),
                    tree.path(&["m_Shadows", "m_Resolution"]).cloned(),
                );
                if light_edit(&mut tree, &s.qcaps, c, true) {
                    f.sf.save_typetree(pid, &tree)?;
                    n += 1;
                    let kind = match tree.get("m_Type").and_then(Value::as_int) {
                        Some(SPOT) => "spot",
                        Some(DIRECTIONAL) => "directional",
                        Some(POINT) => "point",
                        _ => "area",
                    };
                    if before.0.as_ref() != tree.get("m_RenderMode") {
                        *lights
                            .entry(format!("{kind} lights Important"))
                            .or_default() += 1;
                    }
                    if before.1.as_ref() != tree.path(&["m_Shadows", "m_Resolution"]) {
                        *lights
                            .entry("lights with their shadow resolution capped".into())
                            .or_default() += 1;
                    }
                }
            }
            "Shader" if what.contains("shaders") => {
                let mut tree = f.sf.read_typetree(pid)?;
                if let Some((a, b)) = standard_reorder(&mut tree)? {
                    f.sf.save_typetree(pid, &tree)?;
                    n += 1;
                    let name = tree
                        .get("m_ParsedForm")
                        .map(|pf| text(pf, "m_Name"))
                        .unwrap_or_default();
                    plan.done.push(format!(
                        "{rel}: {name}: SubShader LOD {b} first (was LOD {a})"
                    ));
                }
            }
            "MonoBehaviour" if what.contains("urp") => {
                let Some((cls, asm, full)) = s.mb(rel, pid).cloned() else {
                    continue;
                };
                if cls != URP_ASSET && cls != SSAO && !(s.post_ok && post.contains(&cls)) {
                    continue;
                }
                let g = gen.get_or_insert_with(|| {
                    monotree::Generator::new(&job.src.join("Managed"), s.unity)
                });
                let (mut tree, node): (Value, TypeTree) =
                    match monotree::read(&f.sf, pid, g, (&asm, &full)) {
                        Ok(x) => x,
                        Err(why) => {
                            plan.kept
                                .push(format!("{rel}: {cls} {pid}: not read ({why})"));
                            continue;
                        }
                    };
                match urp_edit(&cls, &mut tree, s, c) {
                    None => plan.kept.push(format!(
                        "{rel}: {cls} {pid}: left alone (its fields read with values they cannot hold)"
                    )),
                    Some(ch) if !ch.is_empty() => {
                        f.sf.save_typetree_with(pid, &tree, &node)?;
                        n += 1;
                        let name = text(&tree, "m_Name");
                        let name = if name.is_empty() { pid.to_string() } else { name };
                        plan.done.push(format!("{rel}: {cls} {name}: {}", ch.join(", ")));
                    }
                    Some(_) => {}
                }
            }
            _ => {}
        }
    }
    if !lights.is_empty() {
        plan.done.push(format!(
            "{rel}: {}",
            lights
                .iter()
                .map(|(k, v)| format!("{v} {k}"))
                .collect::<Vec<_>>()
                .join(", ")
        ));
    }
    if n > 0 && !c.get("dry_run").is_some_and(Json::truthy) {
        job.write_with(rel, |tmp| Ok(f.write_to(tmp, false)?))?;
    }
    Ok(())
}

/// The system's RAM in MB (textures.mem_total_mb: /proc/meminfo, else plenty).
fn mem_total_mb() -> i128 {
    if let Ok(t) = std::fs::read_to_string("/proc/meminfo") {
        for line in t.lines() {
            if let Some(rest) = line.strip_prefix("MemTotal:") {
                if let Some(kb) = rest
                    .split_whitespace()
                    .next()
                    .and_then(|x| x.parse::<i128>().ok())
                {
                    return kb >> 10;
                }
            }
        }
    }
    1 << 20
}

/// ('handheld' | 'full', why): quality.profile_of.
fn profile_of(c: &Json) -> Result<(bool, String)> {
    let p = c
        .get("profile")
        .cloned()
        .unwrap_or(Json::Str("auto".into()));
    let full = [
        Json::Null,
        Json::Bool(false),
        Json::Str("off".into()),
        Json::Str("full".into()),
    ];
    if full.contains(&p) {
        return Ok((false, "quality.profile = full".into()));
    }
    if p == Json::Str("handheld".into()) {
        return Ok((true, "quality.profile = handheld".into()));
    }
    if p != Json::Str("auto".into()) {
        return Err(Error::Value(format!(
            "quality.profile: {} (auto, handheld or full)",
            p.repr()
        )));
    }
    let mem = mem_total_mb();
    let ram = c.get("ram_mb").cloned().unwrap_or(Json::Int(1536));
    let le = match Num::of_json(&ram) {
        Some(r) => !Num::I(mem).gt(r),
        None => {
            return Err(Error::Value(format!(
                "'<=' not supported for ram_mb {}",
                ram.repr()
            )))
        }
    };
    if le {
        Ok((
            true,
            format!("{mem} MB of RAM (at most {})", num_json(&ram)),
        ))
    } else {
        Ok((
            false,
            format!("{mem} MB of RAM (more than {})", num_json(&ram)),
        ))
    }
}

fn bundles(job: &Job) -> usize {
    job.walk("StreamingAssets")
        .into_iter()
        .map(|(rel, names)| {
            names
                .iter()
                .filter(|n| super::shaders::is_bundle(&job.src.join(&rel).join(n)))
                .count()
        })
        .sum()
}

pub fn run(job: &mut Job) -> Result<()> {
    let c = section(&job.cfg, "quality", &defaults())?;
    let (handheld, why) = profile_of(&c)?;
    if !handheld {
        job.log(&format!("quality: full picture ({why}): nothing changed"));
        return Ok(());
    }
    if let Some(n) = cfg_num(&c, "shadow_cascades") {
        if n.f() < 2.0 {
            return Err(Error::Value(
                "quality.shadow_cascades: at least 2 (1 cascade is another shader variant)".into(),
            ));
        }
    }
    if job.is_done("quality", "all") {
        return Ok(());
    }
    job.log(&format!("quality: handheld profile ({why})"));
    let files = data_files(job);
    let mut plan = Plan::default();
    let mut s = scan(job, &files, &c)?;
    if s.shaders_seen > 0 && s.variants.shaders.is_empty() {
        job.log("quality: no shader has GLES3 programs yet: run this step after `shaders`; nothing changed");
        return Ok(());
    }
    let edits = decide(job, &mut s, &c, &mut plan);
    let dry = c.get("dry_run").is_some_and(Json::truthy);
    if !dry {
        let old = job
            .state
            .get("quality_caps")
            .map(Json::to_str)
            .unwrap_or_default();
        let old = if job.state.get("quality_caps").is_some_and(Json::truthy) {
            old
        } else {
            String::new()
        };
        let caps = merge_caps(&old, &host_caps(&s.qcaps))?;
        if !caps.is_empty() {
            job.state.set("quality_caps", Json::Str(caps));
            job.save_state()?;
        }
    }
    for rel in &files {
        if let Some(what) = edits.get(rel) {
            edit(job, rel, what, &s, &c, &mut plan)?;
        }
    }
    let b = bundles(job);
    if b > 0 {
        plan.kept.push(format!(
            "{b} asset bundles under StreamingAssets: not looked at (scenes, lights and URP data there keep the game's quality)"
        ));
    }
    for k in &plan.kept {
        job.log(&format!("quality: kept: {k}"));
    }
    let done = if plan.done.is_empty() {
        "nothing to change".to_string()
    } else {
        plan.done.join("; ")
    };
    job.log(&format!(
        "quality: {}{done}",
        if dry { "would change: " } else { "" }
    ));
    if dry {
        return Ok(());
    }
    if let Some(q) = job.state.get("quality_caps").filter(|q| q.truthy()) {
        job.log(&format!(
            "quality: run-time caps for ua_host: {}",
            q.to_str()
        ));
    }
    job.mark_done("quality", "all")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn caps() {
        let q = vec![
            ("shadowResolution".to_string(), Num::I(1)),
            ("pixelLightCount".to_string(), Num::I(1)),
            ("antiAliasing".to_string(), Num::I(0)),
        ];
        assert_eq!(host_caps(&q), "pixel_lights=1,shadow_resolution=1,msaa=0");
        assert_eq!(
            merge_caps("pixel_lights=2,msaa=0", "pixel_lights=1,anisotropic=1").unwrap(),
            "pixel_lights=1,msaa=0,anisotropic=1"
        );
        assert_eq!(merge_caps("", "").unwrap(), "");
    }
}
