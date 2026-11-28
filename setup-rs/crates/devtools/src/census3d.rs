//! `pmu-dev census3d <Game_Data> [--json out.json] [--top N] [--bundles REGEX]`: what a 3D game's frames
//! are made of, read from its data before it ever runs (docs/3d.md): per scene (a levelN file or a
//! scene bundle) the renderers (static, skinned, shadow casters, static-batched, lightmapped), the
//! triangles they draw, LOD groups, lights (realtime / baked, shadows), cameras (far plane, HDR, MSAA,
//! rendering path), reflection probes, particle systems, terrains, animators by culling mode,
//! occlusion data and lightmaps; for the whole game the shaders the most renderers draw with and
//! their passes (a pass a lit renderer draws each frame is a draw call), the post effects
//! (MonoBehaviours by script) and QualitySettings. It changes nothing.
use std::collections::{BTreeMap, HashMap};
use std::path::Path;

use unityfs::Value;

#[derive(Default, Clone, serde::Serialize)]
struct Scene {
    files: Vec<String>,
    renderers: u64,
    renderers_off: u64,
    skinned: u64,
    skinned_bones: u64,
    shadow_casters: u64,
    shadows_only: u64,
    static_batched: u64,
    lightmapped: u64,
    tris: u64,
    tris_shadow: u64,
    tris_skinned: u64,
    unresolved_meshes: u64,
    materials_slots: u64,
    lod_groups: u64,
    lod_levels: u64,
    lights: BTreeMap<String, u64>,
    cameras: BTreeMap<String, u64>,
    reflection_probes: BTreeMap<String, u64>,
    particle_systems: u64,
    terrains: u64,
    animators: BTreeMap<String, u64>,
    occlusion_data: bool,
    lightmaps: u64,
    scripts: BTreeMap<String, u64>,
}

#[derive(Default, serde::Serialize)]
struct ShaderInfo {
    name: String,
    passes: Vec<String>,
    renderers: u64,
}

type Key = (String, i64); // (serialized file name, path id)

struct Pending {
    scene: usize,
    mesh: Option<Key>,
    materials: Vec<Key>,
    cast: i64,
    skinned: bool,
}

fn int(v: &Value, k: &str) -> i64 {
    v.get(k).and_then(Value::as_int).map_or(0, |x| x as i64)
}

fn pptr(v: Option<&Value>) -> (i64, i64) {
    match v {
        Some(p) => (int(p, "m_FileID"), int(p, "m_PathID")),
        None => (0, 0),
    }
}

fn sname(v: &Value, k: &str) -> String {
    v.get(k).and_then(Value::as_str).unwrap_or("").to_string()
}

fn base(p: &str) -> String {
    let p = p.replace('\\', "/");
    p.rsplit('/').next().unwrap_or(&p).to_lowercase()
}

/// The (file, path id) a PPtr names, from the serialized file `sf` that holds it.
fn key_of(sf: &unityfs::SerializedFile, own: &str, v: Option<&Value>) -> Option<Key> {
    let (fid, pid) = pptr(v);
    if pid == 0 {
        return None;
    }
    let file = if fid == 0 {
        own.to_string()
    } else {
        let e = sf.externals.get(usize::try_from(fid - 1).ok()?)?;
        base(&String::from_utf8_lossy(&e.path))
    };
    Some((file, pid))
}

/// Every LightMode tag (or the pass name) of each pass of a shader's first SubShader.
fn passes(t: &Value) -> (String, Vec<String>) {
    let Some(pf) = t.get("m_ParsedForm") else { return (String::new(), vec![]) };
    let name = sname(pf, "m_Name");
    let mut out = vec![];
    if let Some(ss) = pf.get("m_SubShaders").and_then(Value::as_array).and_then(|a| a.first()) {
        for p in ss.get("m_Passes").and_then(Value::as_array).into_iter().flatten() {
            let st = p.get("m_State");
            let mut mode = String::new();
            if let Some(tags) = st.and_then(|s| s.path(&["m_Tags", "tags"])).and_then(Value::as_array) {
                for tg in tags {
                    if let Value::Pair(kv) = tg {
                        if kv.0.as_str() == Some("LightMode") {
                            mode = kv.1.as_str().unwrap_or("").to_string();
                        }
                    }
                }
            }
            let pname = st.map(|s| sname(s, "m_Name")).unwrap_or_default();
            out.push(match (mode.is_empty(), pname.is_empty()) {
                (false, true) => mode,
                (false, false) => format!("{mode}:{pname}"),
                (true, false) => pname,
                (true, true) => "Always".into(),
            });
        }
    }
    (name, out)
}

fn mesh_tris(m: &Value) -> u64 {
    let mut n = 0;
    for s in m.get("m_SubMeshes").and_then(Value::as_array).into_iter().flatten() {
        let topo = int(s, "topology");
        if topo == 0 {
            n += int(s, "indexCount").max(0) as u64 / 3;
        }
    }
    n
}

pub fn run(args: &[String]) -> Result<u8, String> {
    let usage = "usage: pmu-dev census3d <Game_Data> [--json out.json] [--top N] [--bundles REGEX]";
    let (mut pos, mut json_out, mut top, mut bundles) = (vec![], None, 25usize, String::from("."));
    let mut i = 0;
    while i < args.len() {
        let next = |i: usize| args.get(i + 1).cloned().ok_or(usage.to_string());
        match args[i].as_str() {
            "--json" => { json_out = Some(next(i)?); i += 1; }
            "--top" => { top = next(i)?.parse().map_err(|_| usage)?; i += 1; }
            "--bundles" => { bundles = next(i)?; i += 1; }
            a if a.starts_with('-') => return Err(usage.into()),
            a => pos.push(a.to_string()),
        }
        i += 1;
    }
    let [data] = &pos[..] else { return Err(usage.into()) };
    let rx = regex::Regex::new(&bundles).map_err(|e| e.to_string())?;
    let cfg = convert::pyjson::Json::Dict(vec![("version".into(), convert::pyjson::Json::Int(1))]);
    let dp = Path::new(data);
    let job = convert::Job::new(dp, dp, cfg).map_err(|e| e.to_string())?;

    // the files: the data folder's own, then StreamingAssets' bundles (any file that opens as one)
    let mut files: Vec<String> = job.list("", |n| {
        n.ends_with(".assets") || n == "globalgamemanagers" || (n.starts_with("level") && !n.contains('.'))
    });
    files.sort_by_key(|n| (n.starts_with("level"), n.trim_start_matches("level").parse::<u32>().unwrap_or(0), n.clone()));
    for (dir, names) in job.walk("StreamingAssets") {
        for n in names {
            let rel = if dir.is_empty() { format!("StreamingAssets/{n}") } else { format!("{dir}/{n}") };
            let rel = if rel.starts_with("StreamingAssets") { rel } else { format!("StreamingAssets/{rel}") };
            if rx.is_match(&rel) && !n.ends_with(".json") && !n.ends_with(".txt") && !n.ends_with(".manifest") {
                files.push(rel);
            }
        }
    }

    let mut scenes: Vec<Scene> = vec![Scene { files: vec!["(not a scene: assets, globalgamemanagers, asset bundles)".into()], ..Default::default() }];
    let mut mesh_tris_of: HashMap<Key, u64> = HashMap::new();
    let mut shader_of_mat: HashMap<Key, Key> = HashMap::new();
    let mut shaders: HashMap<Key, ShaderInfo> = HashMap::new();
    let mut scripts_of: HashMap<Key, String> = HashMap::new();
    let mut filter_mesh: HashMap<Key, Key> = HashMap::new(); // (file, GameObject) -> mesh
    let mut pending: Vec<(Pending, Key)> = vec![]; // with the renderer's GameObject
    let mut mono_scripts: Vec<(usize, Key)> = vec![];
    let mut quality = String::new();
    let mut skipped = vec![];

    for rel in &files {
        let opts = unityfs::Options { all_serialized: true, ..Default::default() };
        let mut f = match job.open(&job.path_in(rel), opts) {
            Ok(f) => f,
            Err(e) => {
                if !rel.starts_with("StreamingAssets") {
                    skipped.push(format!("{rel}: {e}"));
                }
                continue;
            }
        };
        let names = f.file_names();
        // a scene: a levelN file, or a bundle with more than one serialized file / scene settings
        let is_scene_bundle = f.is_bundle
            && names.iter().any(|n| !n.ends_with(".sharedAssets") && !n.contains('.'))
            && (names.len() > 1);
        let level = rel.starts_with("level");
        let scene_ix = if level || is_scene_bundle {
            scenes.push(Scene { files: vec![rel.clone()], ..Default::default() });
            scenes.len() - 1
        } else {
            0
        };
        for k in 0..f.file_count() {
            if f.select(k).is_err() {
                continue;
            }
            let own = if f.is_bundle { base(&names[k]) } else { base(rel) };
            let objs: Vec<(i64, String)> = f.sf.objects.iter().map(|o| (o.path_id, f.class_name(o))).collect();
            let sc = &mut scenes[scene_ix];
            for (pid, cls) in objs {
                let read = |f: &unityfs::DataFile| f.read(pid).ok();
                match cls.as_str() {
                    "Mesh" => {
                        if let Some(m) = read(&f) {
                            mesh_tris_of.insert((own.clone(), pid), mesh_tris(&m));
                        }
                    }
                    "MeshFilter" => {
                        if let Some(m) = read(&f) {
                            let go = key_of(&f.sf, &own, m.get("m_GameObject"));
                            if let (Some(go), Some(mk)) = (go, key_of(&f.sf, &own, m.get("m_Mesh"))) {
                                filter_mesh.insert(go, mk);
                            }
                        }
                    }
                    "MeshRenderer" | "SkinnedMeshRenderer" => {
                        let Some(r) = read(&f) else { continue };
                        let skinned = cls == "SkinnedMeshRenderer";
                        if r.get("m_Enabled").and_then(Value::as_int) == Some(0) {
                            sc.renderers_off += 1;
                            continue;
                        }
                        sc.renderers += 1;
                        let cast = int(&r, "m_CastShadows");
                        if cast != 0 { sc.shadow_casters += 1; }
                        if cast == 3 { sc.shadows_only += 1; }
                        if r.path(&["m_StaticBatchInfo", "subMeshCount"]).and_then(Value::as_int).unwrap_or(0) > 0 {
                            sc.static_batched += 1;
                        }
                        let lm = int(&r, "m_LightmapIndex");
                        if lm >= 0 && lm < 0xFFFE { sc.lightmapped += 1; }
                        let mats: Vec<Key> = r.get("m_Materials").and_then(Value::as_array).into_iter().flatten()
                            .filter_map(|m| key_of(&f.sf, &own, Some(m))).collect();
                        sc.materials_slots += mats.len() as u64;
                        let mesh = if skinned {
                            sc.skinned += 1;
                            sc.skinned_bones += r.get("m_Bones").and_then(Value::as_array).map_or(0, |a| a.len() as u64);
                            key_of(&f.sf, &own, r.get("m_Mesh"))
                        } else {
                            None
                        };
                        let go = key_of(&f.sf, &own, r.get("m_GameObject")).unwrap_or_default();
                        pending.push((Pending { scene: scene_ix, mesh, materials: mats, cast, skinned }, go));
                    }
                    "Material" => {
                        if let Some(m) = read(&f) {
                            if let Some(s) = key_of(&f.sf, &own, m.get("m_Shader")) {
                                shader_of_mat.insert((own.clone(), pid), s);
                            }
                        }
                    }
                    "Shader" => {
                        if let Some(t) = read(&f) {
                            let (name, p) = passes(&t);
                            shaders.insert((own.clone(), pid), ShaderInfo { name, passes: p, renderers: 0 });
                        }
                    }
                    "LODGroup" => {
                        if let Some(l) = read(&f) {
                            sc.lod_groups += 1;
                            sc.lod_levels += l.get("m_LODs").and_then(Value::as_array).map_or(0, |a| a.len() as u64);
                        }
                    }
                    "Light" => {
                        if let Some(l) = read(&f) {
                            if l.get("m_Enabled").and_then(Value::as_int) == Some(0) { continue; }
                            let ty = ["spot", "directional", "point", "area", "disc"].get(int(&l, "m_Type") as usize).copied().unwrap_or("?");
                            let bake = match int(&l, "m_Lightmapping") { 1 => "mixed", 2 => "baked", _ => "realtime" };
                            let sh = match l.path(&["m_Shadows", "m_Type"]).and_then(Value::as_int).unwrap_or(0) { 1 => " hard shadows", 2 => " soft shadows", _ => "" };
                            *sc.lights.entry(format!("{bake} {ty}{sh}")).or_default() += 1;
                        }
                    }
                    "Camera" => {
                        if let Some(c) = read(&f) {
                            if c.get("m_Enabled").and_then(Value::as_int) == Some(0) { continue; }
                            let far = c.get("far clip plane").or(c.get("m_FarClipPlane")).and_then(Value::as_f64).unwrap_or(0.0);
                            let path = ["vertex lit", "forward", "deferred", "legacy deferred"].get(int(&c, "m_RenderingPath") as usize).copied().unwrap_or("player setting");
                            let rt = if pptr(c.get("m_TargetTexture")).1 != 0 { " to a texture" } else { "" };
                            *sc.cameras.entry(format!("far {far:.0}, {path}, HDR {}, MSAA {}{rt}", int(&c, "m_HDR"), int(&c, "m_AllowMSAA"))).or_default() += 1;
                        }
                    }
                    "ReflectionProbe" => {
                        if let Some(p) = read(&f) {
                            let m = match int(&p, "m_Mode") { 0 => "baked", 1 => "realtime", _ => "custom" };
                            let r = if int(&p, "m_Mode") == 1 { if int(&p, "m_RefreshMode") == 1 { " every frame" } else { " on awake/script" } } else { "" };
                            *sc.reflection_probes.entry(format!("{m}{r}")).or_default() += 1;
                        }
                    }
                    "ParticleSystem" => sc.particle_systems += 1,
                    "Terrain" => sc.terrains += 1,
                    "Animator" => {
                        if let Some(a) = read(&f) {
                            let m = ["always animate", "cull update transforms", "cull completely"].get(int(&a, "m_CullingMode") as usize).copied().unwrap_or("?");
                            *sc.animators.entry(m.into()).or_default() += 1;
                        }
                    }
                    "OcclusionCullingSettings" => {
                        if let Some(o) = read(&f) {
                            sc.occlusion_data |= pptr(o.get("m_OcclusionCullingData")).1 != 0;
                        }
                    }
                    "LightmapSettings" => {
                        if let Some(o) = read(&f) {
                            sc.lightmaps += o.get("m_Lightmaps").and_then(Value::as_array).map_or(0, |a| a.len() as u64);
                        }
                    }
                    "MonoScript" => {
                        if let Some(o) = read(&f) {
                            let ns = sname(&o, "m_Namespace");
                            let cn = sname(&o, "m_ClassName");
                            scripts_of.insert((own.clone(), pid), if ns.is_empty() { cn } else { format!("{ns}.{cn}") });
                        }
                    }
                    "MonoBehaviour" => {
                        // only the script's PPtr: the fields need the game's assemblies
                        if let Ok(b) = f.sf.read_typetree(pid) {
                            if let Some(k) = key_of(&f.sf, &own, b.get("m_Script")) {
                                mono_scripts.push((scene_ix, k));
                            }
                        }
                    }
                    "QualitySettings" if quality.is_empty() => {
                        if let Ok((q, _)) = androiddata::typetrees::read(&f.sf, pid, &[]) {
                            quality = quality_text(&q);
                        }
                    }
                    _ => {}
                }
            }
        }
    }

    // resolve meshes, triangles and shaders
    for (p, go) in &pending {
        let mk = p.mesh.clone().or_else(|| filter_mesh.get(go).cloned());
        let sc = &mut scenes[p.scene];
        match mk.as_ref().and_then(|k| mesh_tris_of.get(k)) {
            Some(&t) => {
                sc.tris += t;
                if p.cast != 0 { sc.tris_shadow += t; }
                if p.skinned { sc.tris_skinned += t; }
            }
            None => sc.unresolved_meshes += 1,
        }
        for m in &p.materials {
            if let Some(s) = shader_of_mat.get(m).and_then(|s| shaders.get_mut(s)) {
                s.renderers += 1;
            }
        }
    }
    for (scene, k) in &mono_scripts {
        let n = scripts_of.get(k).cloned().unwrap_or_else(|| format!("(script in {})", k.0));
        *scenes[*scene].scripts.entry(n).or_default() += 1;
    }

    // the report
    println!("census3d: {} files, {} scenes{}", files.len(), scenes.len() - 1,
        if skipped.is_empty() { String::new() } else { format!(", {} not read", skipped.len()) });
    for s in &skipped { println!("  not read: {s}"); }
    if !quality.is_empty() { println!("\nQualitySettings:\n{quality}"); }
    let post_rx = regex::Regex::new(r"(?i)postprocess|bloom|ambientocclusion|depthoffield|motionblur|colorgrading|vignette|chromatic|grain|screenspace|ssr|ssao|antialias|fxaa|smaa|taa|outline|fog|volume|beautify|amplify|lut|tonemap|blur|flare|godray|sunshaft").unwrap();
    for (i, s) in scenes.iter().enumerate() {
        if i == 0 && s.renderers == 0 && s.lights.is_empty() { continue; }
        println!("\n== {}", s.files.join(", "));
        println!("  renderers {} (+{} off): {} skinned ({} bones), {} cast shadows ({} shadow only), {} static-batched, {} lightmapped; {} material slots",
            s.renderers, s.renderers_off, s.skinned, s.skinned_bones, s.shadow_casters, s.shadows_only, s.static_batched, s.lightmapped, s.materials_slots);
        println!("  triangles {} (every renderer and LOD level summed: an upper bound, not a frame; {} in shadow casters, {} skinned){}", s.tris, s.tris_shadow, s.tris_skinned,
            if s.unresolved_meshes > 0 { format!("; {} renderers' meshes not found", s.unresolved_meshes) } else { String::new() });
        println!("  LOD groups {} ({} levels), particle systems {}, terrains {}, occlusion data {}, lightmaps {}",
            s.lod_groups, s.lod_levels, s.particle_systems, s.terrains, if s.occlusion_data { "yes" } else { "no" }, s.lightmaps);
        if !s.lights.is_empty() { println!("  lights: {}", join(&s.lights)); }
        if !s.reflection_probes.is_empty() { println!("  reflection probes: {}", join(&s.reflection_probes)); }
        if !s.animators.is_empty() { println!("  animators: {}", join(&s.animators)); }
        if !s.cameras.is_empty() { println!("  cameras: {}", join(&s.cameras)); }
        let post: BTreeMap<String, u64> = s.scripts.iter().filter(|(k, _)| post_rx.is_match(k)).map(|(k, v)| (k.clone(), *v)).collect();
        if !post.is_empty() { println!("  post / render scripts: {}", join(&post)); }
    }
    let mut sh: Vec<&ShaderInfo> = shaders.values().filter(|s| s.renderers > 0).collect();
    sh.sort_by(|a, b| b.renderers.cmp(&a.renderers).then(a.name.cmp(&b.name)));
    println!("\nshaders by renderer material slots (first SubShader's passes):");
    for s in sh.iter().take(top) {
        println!("  {:6}  {}  [{}]", s.renderers, s.name, s.passes.join(", "));
    }
    let mut all_scripts: BTreeMap<String, u64> = BTreeMap::new();
    for s in &scenes { for (k, v) in &s.scripts { *all_scripts.entry(k.clone()).or_default() += v; } }
    let mut sv: Vec<_> = all_scripts.into_iter().collect();
    sv.sort_by(|a, b| b.1.cmp(&a.1));
    println!("\nMonoBehaviours by script (top {top}):");
    for (k, v) in sv.iter().take(top) { println!("  {v:6}  {k}"); }
    if let Some(p) = json_out {
        let j = serde_json::json!({ "scenes": scenes, "shaders": sh });
        std::fs::write(&p, serde_json::to_string_pretty(&j).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
    }
    Ok(0)
}

fn join(m: &BTreeMap<String, u64>) -> String {
    m.iter().map(|(k, v)| format!("{v} {k}")).collect::<Vec<_>>().join(", ")
}

fn quality_text(q: &Value) -> String {
    let cur = int(q, "m_CurrentQuality");
    let mut out = String::new();
    for (i, lv) in q.get("m_QualitySettings").and_then(Value::as_array).into_iter().flatten().enumerate() {
        let g = |k: &str| lv.get(k).map(|v| v.as_f64().map_or(format!("{v:?}"), |x| format!("{x}"))).unwrap_or("-".into());
        out += &format!("  {}{} {}: pixel lights {}, shadows {} res {} dist {} cascades {}, LOD bias {}, max LOD {}, MSAA {}, aniso {}, textures 1/{}, vsync {}, soft particles {}, billboards face camera {}\n",
            if i as i64 == cur { "*" } else { " " }, i, sname(lv, "name"), g("pixelLightCount"), g("shadows"), g("shadowResolution"), g("shadowDistance"),
            g("shadowCascades"), g("lodBias"), g("maximumLODLevel"), g("antiAliasing"), g("anisotropicTextures"),
            1 << lv.get("globalTextureMipmapLimit").or(lv.get("textureQuality")).and_then(Value::as_int).unwrap_or(0), g("vSyncCount"), g("softParticles"), g("billboardsFaceCameraPosition"));
    }
    out
}
