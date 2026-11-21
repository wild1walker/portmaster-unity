//! Step `exactindex` (pmu's own; after `textures`, with `textures.exact_index = "blue"`): palette-indexed
//! textures in one byte a texel, exactly. A shader that `exact_shaders` / `exact_keywords` name reads a
//! texel's blue channel as the x of a palette texture and its alpha (Blasphemous's
//! `Maikel/MaikelSpriteMasterShader` with `PALETTE_ON`): red and green never reach the picture. The
//! textures step leaves such textures as RGBA32 (exacttex.rs); this step stores those it can prove are
//! only ever drawn that way, and whose alpha is 0 or 255 only, as R8 (the index; transparent texels at a
//! reserved index K that no opaque texel uses), and rewrites the shader's GLES3 programs of those
//! keywords so that such a texel reads as the original did:
//!
//! ```text
//! V = texture(_MainTex, uv).wxyz;                      (the shader's own line: V = (A, R, G, B))
//! if (V.x == 1.0 && V.z == 0.0 && V.w == 0.0) { V.w = V.y; V.x = (V.y == K) ? 0.0 : 1.0; }
//! ```
//!
//! An R8 texel samples as (r, 0, 0, 1): the inserted line gives the index in V.w and the alpha in V.x,
//! as the RGBA32 texel had them. An RGBA32 texel keeps its value unless it is (r, 0, 0, 1) with r > 0;
//! the step checks that no RGBA32 texture such a program can sample has such a texel (a two-byte form
//! for textures with other alpha values would need a test that RGBA32 texels pass as well, and
//! Blasphemous's have texels like (255, 204, 0, 255): those stay RGBA32). The programs must not read V.y
//! or V.z after the line before writing them (checked statement by statement, writes in a block not
//! counted).
//!
//! A texture is converted only when every use of every sprite drawn from it is accounted for, all of them
//! through a material of those shaders with those keywords (fail safe: anything else, and it stays as the
//! game has it):
//! - a SpriteRenderer's sprite: all of its materials are such materials, and no script whose class
//!   `exact_index_skip` names sits on its GameObject or above it (a script that copies its sprite to
//!   renderers of other shaders: Blasphemous's `ReplicateSprite`);
//! - an Animator's clips: each clip's sprite curves all target one GameObject (the binding's path hash),
//!   and that GameObject's one SpriteRenderer is such a renderer; a clip with sprite curves on several
//!   paths, or any other object curve, counts as another use;
//! - any other object that names the sprite, the texture, a clip that sets the sprite or a controller
//!   that holds such a clip (a MonoBehaviour field, a UI Image, a material of another shader, a particle
//!   system, the ResourceManager's container: Resources.Load) is another use.
//!
//! Every texel is checked to read back exactly (index and alpha as the shader sees them). The plan (the
//! textures, K) is kept in the journal, so a run that stops continues with the same one.
use super::exacttex::{class_of, data_files, key, material_matches, shader_name, Key};
use super::{opt, section};
use crate::error::{Error, Result};
use crate::job::Job;
use crate::pyjson::Json;
use shaderx::blob::{self, Slot};
use shaderx::tree;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use unityfs::{DataFile, Value};

pub const STEP: &str = "exactindex";
pub const STATE: &str = "exactindex";

const GAME_OBJECT: i32 = 1;
const TRANSFORM: i32 = 4;
const MATERIAL: i32 = 21;
const TEXTURE2D: i32 = 28;
const SHADER: i32 = 48;
const CLIP: i32 = 74;
const CONTROLLER: i32 = 91;
const ANIMATOR: i32 = 95;
const MONO_BEHAVIOUR: i32 = 114;
const MONO_SCRIPT: i32 = 115;
const ASSET_BUNDLE: i32 = 142;
const RESOURCE_MANAGER: i32 = 147;
const PRELOAD_DATA: i32 = 150;
const SPRITE_RENDERER: i32 = 212;
const SPRITE: i32 = 213;
const OVERRIDE: i32 = 221;
const RECT_TRANSFORM: i32 = 224;
const RGBA32: i64 = 4;
const R8: i64 = 63;

/// CRC-32 (zlib's), as Unity hashes an animation binding's path.
pub fn crc32(b: &[u8]) -> u32 {
    let mut c = 0xffff_ffffu32;
    for &x in b {
        c ^= x as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 {
                (c >> 1) ^ 0xedb8_8320
            } else {
                c >> 1
            };
        }
    }
    !c
}

/// What a texture is stored as.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Form {
    R8,
}

/// The texel as the rewritten program reads an R8 texel r: (alpha, index).
pub fn decode(r: u8, k: u8) -> (u8, u8) {
    (if r == k { 0 } else { 255 }, r)
}

/// RGBA32 pixels to R8 (transparent texels at index k), with a check that every texel reads back as the
/// shader read the original: the alpha, and the index where the alpha is not 0.
pub fn encode(rgba: &[u8], _form: Form, k: u8) -> std::result::Result<Vec<u8>, String> {
    let mut out = Vec::with_capacity(rgba.len() / 4);
    for p in rgba.chunks_exact(4) {
        let (b, a) = (p[2], p[3]);
        if a != 0 && a != 255 {
            return Err(format!("alpha {a} in a one-byte texture"));
        }
        if a != 0 && b == k {
            return Err(format!("an opaque texel uses the reserved index {k}"));
        }
        let r = if a == 0 { k } else { b };
        let (da, di) = decode(r, k);
        if da != a || (a != 0 && di != b) {
            return Err(format!(
                "texel (b {b}, a {a}) reads back as (index {di}, alpha {da})"
            ));
        }
        out.push(r);
    }
    Ok(out)
}

/// The rewrite of one GLES program's code: after `V = texture(_MainTex, uv).wxyz;` the decoding line;
/// None (with why) when the code does not have that line exactly once, or reads V.y / V.z after it
/// before writing them.
pub fn rewrite_program(code: &str, k: u8) -> std::result::Result<String, String> {
    let rx = regex::Regex::new(r"(?m)^([ \t]*)(\w+) = texture\(_MainTex, ([\w.]+)\)\.wxyz;[ \t]*$")
        .unwrap();
    let hits: Vec<_> = rx.captures_iter(code).collect();
    // (a GLES3 entry has the vertex and the fragment program; the fetch is the fragment's)
    if hits.len() != 1 {
        return Err(format!("{} reads of _MainTex as .wxyz", hits.len()));
    }
    let c = &hits[0];
    let (ind, v) = (c.get(1).unwrap().as_str(), c.get(2).unwrap().as_str());
    let end = c.get(0).unwrap().end();
    check_flow(&code[end..], v)?;
    let kf = format!("{:.9}", k as f64 / 255.0);
    let line = format!(
        "\n{ind}if({v}.x == 1.0 && {v}.z == 0.0 && {v}.w == 0.0){{ {v}.w = {v}.y; {v}.x = (abs({v}.y - {kf}) < 0.001960784) ? 0.0 : 1.0; }}"
    );
    Ok(format!("{}{line}{}", &code[..end], &code[end..]))
}

/// The statements after the fetch: V.y and V.z must be written before anything reads them (or V
/// whole), up to the end of main.
fn check_flow(rest: &str, v: &str) -> std::result::Result<(), String> {
    let use_rx = regex::Regex::new(&format!(r"\b{}\b(\.[xyzw]+)?", regex::escape(v))).unwrap();
    let mut written = [false; 4]; // x y z w, written on every path (outside any block)
    let mut depth = 0i32;
    for line in rest.lines() {
        let l = line.trim();
        if l.starts_with("#endif") {
            return Ok(());
        }
        if depth == 0 && (l == "}" || l == "return;") {
            return Ok(()); // the end of main
        }
        let (lhs, rhs) = match l.find(" = ") {
            Some(i) if !l[..i].contains('(') => (&l[..i], &l[i + 3..]),
            _ => ("", l),
        };
        for m in use_rx.captures_iter(rhs) {
            let comps = m.get(1).map(|s| &s.as_str()[1..]).unwrap_or("xyzw");
            for (i, ch) in "xyzw".chars().enumerate() {
                if comps.contains(ch) && (i == 1 || i == 2) && !written[i] {
                    return Err(format!("reads {v}.{ch} after the fetch: {l}"));
                }
            }
        }
        if depth == 0 {
            if let Some(m) = use_rx.captures(lhs) {
                if m.get(0).unwrap().start() == 0 {
                    let comps = m.get(1).map(|s| &s.as_str()[1..]).unwrap_or("xyzw");
                    for (i, ch) in "xyzw".chars().enumerate() {
                        if comps.contains(ch) {
                            written[i] = true;
                        }
                    }
                }
            }
        }
        if written[1] && written[2] {
            return Ok(());
        }
        depth += l.matches('{').count() as i32 - l.matches('}').count() as i32;
        if depth < 0 {
            return Ok(());
        }
    }
    Ok(())
}

/// A raw object's PPtrs (4-byte aligned file id + 8-byte path id) that name objects of `want`.
fn raw_refs(f: &DataFile, from: &str, b: &[u8], want: &HashSet<Key>, out: &mut HashSet<Key>) {
    let mut i = 0;
    while i + 12 <= b.len() {
        let fid = i32::from_le_bytes(b[i..i + 4].try_into().unwrap());
        let pid = i64::from_le_bytes(b[i + 4..i + 12].try_into().unwrap());
        if pid != 0 && fid >= 0 && (fid as usize) <= f.sf.externals.len() {
            if let Some(n) = super::exacttex::file_name(f, from, fid) {
                let k = (n, pid);
                if want.contains(&k) {
                    out.insert(k);
                }
            }
        }
        i += 4;
    }
}

/// Every PPtr in a value tree, as keys.
fn tree_refs(f: &DataFile, from: &str, v: &Value, out: &mut HashSet<Key>) {
    match v {
        Value::Struct(st) => {
            if st.get("m_FileID").is_some() && st.get("m_PathID").is_some() {
                if let Some(k) = key(f, from, v) {
                    out.insert(k);
                }
                return;
            }
            for (_, x) in &st.fields {
                tree_refs(f, from, x, out);
            }
        }
        Value::Array(a) => a.iter().for_each(|x| tree_refs(f, from, x, out)),
        Value::Pair(p) => {
            tree_refs(f, from, &p.0, out);
            tree_refs(f, from, &p.1, out);
        }
        _ => {}
    }
}

/// The objects an object names, read exactly: None when its layout is not known (the raw match
/// then counts). The ResourceManager: only its container (what Resources.Load returns by path).
fn exact_refs(
    f: &DataFile,
    n: &str,
    pid: i64,
    class: i32,
    scripts: &HashMap<Key, (String, String)>,
    gen: &mut Option<crate::monotree::Generator>,
    job: &Job,
) -> Option<HashSet<Key>> {
    let mut out = HashSet::new();
    let t = if class == MONO_BEHAVIOUR {
        let b = f.sf.bytes(pid).ok()?;
        let fid = i32::from_le_bytes(b.get(16..20)?.try_into().ok()?);
        let spid = i64::from_le_bytes(b.get(20..28)?.try_into().ok()?);
        let sk = (super::exacttex::file_name(f, n, fid)?, spid);
        let (asm, full) = scripts.get(&sk)?;
        let g = gen.get_or_insert_with(|| {
            let unity = crate::version::of_data(&job.src).ok().map(|v| v.line);
            crate::monotree::Generator::of_data(&job.src, unity).0
        });
        crate::monotree::read(&f.sf, pid, g, (asm, full)).ok()?.0
    } else {
        f.sf.read_typetree(pid).ok()?
    };
    if class == RESOURCE_MANAGER {
        for e in arr(t.get("m_Container")) {
            if let Value::Pair(p) = e {
                if let Some(k) = key(f, n, &p.1) {
                    out.insert(k);
                }
            }
        }
    } else {
        tree_refs(f, n, &t, &mut out);
    }
    Some(out)
}

fn arr(v: Option<&Value>) -> impl Iterator<Item = &Value> {
    v.and_then(Value::as_array).into_iter().flatten()
}

fn text_of(v: Option<&Value>) -> String {
    v.and_then(Value::as_bytes)
        .map(|b| String::from_utf8_lossy(b).into_owned())
        .unwrap_or_default()
}

/// One GameObject: name, father GameObject, SpriteRenderers, Animator controllers, script classes.
#[derive(Default)]
struct Go {
    name: String,
    father: Option<Key>,
    children: Vec<Key>,
    renderers: Vec<Key>,
    controllers: Vec<Key>,
    /// a script of exact_index_skip on it (read when the GameObject is: the scripts' names are known by then)
    skipped: bool,
}

/// The plan: which textures, which form, the reserved index; and the shaders to rewrite.
pub struct Plan {
    pub k: u8,
    pub textures: BTreeMap<Key, Form>,
    pub shaders: BTreeSet<Key>,
    pub why: Vec<String>,
}

/// Finds the textures and shaders, or why none (fail safe).
/// The textures the palette programs may sample (left as they are by the textures step: the sprites
/// of the renderers drawn with those materials and the sprites the clips set on them, the materials'
/// own textures) and, with `index`, the plan; Err: no shader of exact_shaders.
pub fn analyze(
    job: &Job,
    c: &Json,
    index: bool,
) -> std::result::Result<(BTreeSet<Key>, std::result::Result<Plan, String>), String> {
    let mut keep = BTreeSet::new();
    let r = plan_inner(job, c, index, &mut keep);
    rss_note("after the walk");
    match r {
        Err(e) if keep.is_empty() && e.starts_with("no shader") => Err(e),
        r => Ok((keep, r)),
    }
}

fn rss_note(what: &str) {
    if std::env::var_os("PMU_EXACT_STATS").is_some() {
        let st = std::fs::read_to_string("/proc/self/status").unwrap_or_default();
        let v: Vec<&str> = st.lines().filter(|l| l.starts_with("VmRSS") || l.starts_with("RssAnon") || l.starts_with("VmHWM")).collect();
        eprintln!("[exact-stats] {what}: {}", v.join(" ").split_whitespace().collect::<Vec<_>>().join(" "));
    }
}

fn plan_inner(
    job: &Job,
    c: &Json,
    index: bool,
    keep: &mut BTreeSet<Key>,
) -> std::result::Result<Plan, String> {
    let rx = regex::Regex::new(&opt(c, "exact_shaders").to_str()).map_err(|e| e.to_string())?;
    let kw: Vec<String> = opt(c, "exact_keywords")
        .to_str()
        .split_whitespace()
        .map(str::to_string)
        .collect();
    let skip = match opt(c, "exact_index_skip").to_str().as_str() {
        "" => None,
        s => Some(regex::Regex::new(s).map_err(|e| e.to_string())?),
    };
    rss_note("start");
    let names: Vec<super::exacttex::Name> = data_files(job).iter().map(|n| super::exacttex::name(n)).collect();
    let open = |n: &str| job.open(&job.path_in(n), Default::default()).ok();
    // 1. shaders, materials, game objects, renderers, sprites, controllers, clips
    let mut shaders: HashMap<Key, String> = HashMap::new();
    let mut scripts: HashMap<Key, String> = HashMap::new();
    let mut scripts_full: HashMap<Key, (String, String)> = HashMap::new();
    // each file's classes and the files its PPtrs can name (itself, its externals): the later passes open again
    // only the files that hold what they read (a file they would skip object by object, or whose references
    // cannot reach what they look for, is left unopened: the same result, a fraction of the opens)
    let mut census: HashMap<super::exacttex::Name, (HashSet<i32>, HashSet<super::exacttex::Name>)> = HashMap::new();
    for n in &names {
        let Some(f) = open(n) else { continue };
        let mut can: HashSet<super::exacttex::Name> = (0..=f.sf.externals.len() as i32)
            .filter_map(|fid| super::exacttex::file_name(&f, n, fid))
            .collect();
        can.insert(n.clone());
        census.insert(n.clone(), (f.sf.objects.iter().map(|o| o.class_id).collect(), can));
        for o in &f.sf.objects {
            if o.class_id == SHADER {
                if let Ok(t) = f.sf.read_typetree(o.path_id) {
                    shaders.insert((n.clone(), o.path_id), shader_name(&t));
                }
            } else if o.class_id == MONO_SCRIPT {
                if let Ok(t) = f.sf.read_typetree(o.path_id) {
                    let (ns, cls) = (text_of(t.get("m_Namespace")), text_of(t.get("m_ClassName")));
                    let asm = text_of(t.get("m_AssemblyName"));
                    let full = if ns.is_empty() {
                        cls.clone()
                    } else {
                        format!("{ns}.{cls}")
                    };
                    scripts_full.insert(
                        (n.clone(), o.path_id),
                        (asm.trim_end_matches(".dll").to_string(), full),
                    );
                    scripts.insert((n.clone(), o.path_id), cls);
                }
            }
        }
    }
    rss_note("after pass 1");
    let targets: BTreeSet<Key> = shaders
        .iter()
        .filter(|(_, n)| rx.is_match(n))
        .map(|(k, _)| k.clone())
        .collect();
    if targets.is_empty() {
        return Err("no shader of exact_shaders".into());
    }
    let mut pal_mats: HashSet<Key> = HashSet::new();
    let mut gos: HashMap<Key, Go> = HashMap::new();
    let mut renderers: HashMap<Key, (Key, bool, Option<Key>)> = HashMap::new(); // go, palette, sprite
    let mut sprite_tex: HashMap<Key, Key> = HashMap::new();
    let mut ctrl_clips: HashMap<Key, (Vec<Key>, Option<Key>)> = HashMap::new();
    // clip -> (its PPtr curves' bindings (path hash, class, attribute), its PPtr keys)
    type Bindings = (Vec<(u32, i64, i64)>, Vec<Key>);
    let mut clip_pptr: HashMap<Key, Bindings> = HashMap::new();
    let mut mat_tex: Vec<(Key, Vec<Key>)> = Vec::new();
    const PASS2: [i32; 10] = [
        MATERIAL, GAME_OBJECT, SPRITE_RENDERER, SPRITE, CONTROLLER, OVERRIDE, CLIP, TRANSFORM, RECT_TRANSFORM, ANIMATOR,
    ];
    for n in &names {
        if !census.get(n).is_some_and(|c| c.0.iter().any(|x| PASS2.contains(x))) {
            continue;
        }
        let Some(f) = open(n) else { continue };
        for o in &f.sf.objects {
            let k = (n.clone(), o.path_id);
            let Ok(t) = (match o.class_id {
                MATERIAL | GAME_OBJECT | SPRITE_RENDERER | SPRITE | CONTROLLER | OVERRIDE
                | CLIP | TRANSFORM | RECT_TRANSFORM | ANIMATOR => f.sf.read_typetree(o.path_id),
                _ => continue,
            }) else {
                continue;
            };
            match o.class_id {
                MATERIAL => {
                    let texs: Vec<Key> = arr(t.path(&["m_SavedProperties", "m_TexEnvs"]))
                        .filter_map(|e| {
                            let env = match e {
                                Value::Pair(p) => &p.1,
                                other => other.get("second").unwrap_or(other),
                            };
                            env.get("m_Texture").and_then(|v| key(&f, n, v))
                        })
                        .collect();
                    if material_matches(&f, n, &t, &shaders, &rx, &kw) {
                        pal_mats.insert(k.clone());
                    }
                    mat_tex.push((k, texs));
                }
                GAME_OBJECT => {
                    let g = gos.entry(k.clone()).or_default();
                    g.name = text_of(t.get("m_Name"));
                    for c in arr(t.get("m_Component")) {
                        let c = match c {
                            Value::Pair(p) => &p.1,
                            other => other.get("component").unwrap_or(other),
                        };
                        let Some(ck) = key(&f, n, c) else { continue };
                        if ck.0 != *n {
                            continue;
                        }
                        match class_of(&f, ck.1) {
                            Some(SPRITE_RENDERER) => g.renderers.push(ck),
                            Some(MONO_BEHAVIOUR) => {
                                if let Ok(b) = f.sf.bytes(ck.1) {
                                    if b.len() >= 28 {
                                        let fid = i32::from_le_bytes(b[16..20].try_into().unwrap());
                                        let pid = i64::from_le_bytes(b[20..28].try_into().unwrap());
                                        if let (Some(sn), Some(s)) =
                                            (super::exacttex::file_name(&f, n, fid), &skip)
                                        {
                                            let cls = scripts.get(&(sn, pid)).map_or("", String::as_str);
                                            g.skipped |= s.is_match(cls);
                                        }
                                    }
                                }
                            }
                            Some(ANIMATOR) => {
                                if let Some(ctl) =
                                    f.sf.read_typetree(ck.1).ok().and_then(|a| {
                                        a.get("m_Controller").and_then(|v| key(&f, n, v))
                                    })
                                {
                                    g.controllers.push(ctl);
                                }
                            }
                            _ => {}
                        }
                    }
                }
                TRANSFORM | RECT_TRANSFORM => {
                    let go = t.get("m_GameObject").and_then(|v| key(&f, n, v));
                    let father = t.get("m_Father").and_then(|v| key(&f, n, v));
                    if let Some(go) = go {
                        let fg = father.and_then(|fk| open_go(&f, n, &fk));
                        gos.entry(go.clone()).or_default().father = fg.clone();
                        if let Some(fg) = fg {
                            gos.entry(fg).or_default().children.push(go);
                        }
                    }
                }
                SPRITE_RENDERER => {
                    let go = t.get("m_GameObject").and_then(|v| key(&f, n, v));
                    let sprite = t.get("m_Sprite").and_then(|v| key(&f, n, v));
                    if let Some(go) = go {
                        renderers.insert(k, (go, true, sprite));
                    }
                }
                SPRITE => {
                    if let Some(tk) = t.path(&["m_RD", "texture"]).and_then(|v| key(&f, n, v)) {
                        sprite_tex.insert(k, tk);
                    }
                }
                CONTROLLER => {
                    let clips = arr(t.get("m_AnimationClips"))
                        .filter_map(|v| key(&f, n, v))
                        .collect();
                    ctrl_clips.insert(k, (clips, None));
                }
                OVERRIDE => {
                    let mut clips = Vec::new();
                    for e in arr(t.get("m_Clips")) {
                        for side in ["m_OriginalClip", "m_OverrideClip"] {
                            if let Some(c) = e.get(side).and_then(|v| key(&f, n, v)) {
                                clips.push(c);
                            }
                        }
                    }
                    let base = t.get("m_Controller").and_then(|v| key(&f, n, v));
                    ctrl_clips.insert(k, (clips, base));
                }
                CLIP => {
                    let b = t.get("m_ClipBindingConstant");
                    let binds: Vec<(u32, i64, i64)> = arr(b.and_then(|b| b.get("genericBindings")))
                        .filter(|g| {
                            g.get("isPPtrCurve")
                                .and_then(|x| x.as_int())
                                .is_some_and(|x| x != 0)
                        })
                        .map(|g| {
                            let i = |n: &str| g.get(n).and_then(Value::as_int).unwrap_or(-1);
                            (i("path") as u32, i("typeID") as i64, i("attribute") as i64)
                        })
                        .collect();
                    let keys = arr(b.and_then(|b| b.get("pptrCurveMapping")))
                        .filter_map(|v| key(&f, n, v))
                        .collect();
                    clip_pptr.insert(k, (binds, keys));
                }
                _ => {}
            }
        }
    }
    rss_note("after pass 2");
    if std::env::var_os("PMU_EXACT_STATS").is_some() {
        let ch: usize = gos.values().map(|g| g.children.len()).sum();
        let sc: usize = gos.values().filter(|g| g.skipped).count();
        let cb: usize = clip_pptr.values().map(|c| c.0.len() + c.1.len()).sum();
        let mt: usize = mat_tex.iter().map(|m| m.1.len()).sum();
        eprintln!("[exact-stats] census {} shaders {} scripts {} gos {} (children {} skipped {}) renderers {} sprite_tex {} ctrl {} clips {} (entries {}) mat_tex {} ({})",
            census.len(), shaders.len(), scripts.len(), gos.len(), ch, sc, renderers.len(), sprite_tex.len(), ctrl_clips.len(), clip_pptr.len(), cb, mat_tex.len(), mt);
    }
    // the renderers' material test, now that every palette material is known
    let mut palette_renderer: HashMap<Key, bool> = HashMap::new();
    for n in &names {
        if !census.get(n).is_some_and(|c| c.0.contains(&SPRITE_RENDERER)) {
            continue;
        }
        let Some(f) = open(n) else { continue };
        for o in
            f.sf.objects
                .iter()
                .filter(|o| o.class_id == SPRITE_RENDERER)
        {
            let Ok(t) = f.sf.read_typetree(o.path_id) else {
                continue;
            };
            let mats: Vec<Option<Key>> = arr(t.get("m_Materials")).map(|m| key(&f, n, m)).collect();
            let pal = !mats.is_empty()
                && mats
                    .iter()
                    .all(|m| m.as_ref().is_some_and(|m| pal_mats.contains(m)));
            palette_renderer.insert((n.clone(), o.path_id), pal);
        }
    }
    rss_note("after the renderers");
    // a renderer is "ours" when its materials are all palette materials and no skipped script sits on
    // its GameObject or above it
    let ours = |r: &Key| -> bool {
        let Some((go, _, _)) = renderers.get(r) else {
            return false;
        };
        if !palette_renderer.get(r).copied().unwrap_or(false) {
            return false;
        }
        let mut g = Some(go.clone());
        for _ in 0..256 {
            let Some(gk) = g else { break };
            let Some(x) = gos.get(&gk) else { break };
            if x.skipped {
                return false;
            }
            g = x.father.clone();
        }
        true
    };
    // 2. every use of every sprite: true only when all of them go through our renderers; pal_use: the
    // sprites some palette renderer draws (ours or not: what the rewritten programs sample)
    let mut use_ok: HashMap<Key, bool> = HashMap::new();
    let mut pal_use: HashSet<Key> = HashSet::new();
    fn mark(m: &mut HashMap<Key, bool>, s: &Key, ok: bool) {
        let e = m.entry(s.clone()).or_insert(true);
        *e = *e && ok;
    }
    for (r, (_, _, sprite)) in &renderers {
        if let Some(s) = sprite {
            mark(&mut use_ok, s, ours(r));
            if palette_renderer.get(r) == Some(&true) {
                pal_use.insert(s.clone());
            }
        }
    }
    // the Animators: each clip's sprite keys to the GameObject its bindings name
    let mut clip_ctrls: HashMap<Key, Vec<Key>> = HashMap::new(); // clip -> the controllers that hold it
    for (ck, (cl, _)) in &ctrl_clips {
        for c in cl {
            clip_ctrls.entry(c.clone()).or_default().push(ck.clone());
        }
    }
    for (gk, g) in &gos {
        for ctl in &g.controllers {
            let mut clips = Vec::new();
            let mut c = Some(ctl.clone());
            for _ in 0..16 {
                let Some(ck) = c else { break };
                let Some((cl, base)) = ctrl_clips.get(&ck) else {
                    break;
                };
                clips.extend(cl.iter().cloned());
                c = base.clone();
            }
            // the GameObjects below this one by their path hash ("" = itself)
            let mut by_hash: HashMap<u32, Key> = HashMap::new();
            let mut stack = vec![(gk.clone(), String::new())];
            while let Some((k, p)) = stack.pop() {
                by_hash.insert(crc32(p.as_bytes()), k.clone());
                if let Some(x) = gos.get(&k) {
                    for ch in &x.children {
                        let nm = gos.get(ch).map(|y| y.name.clone()).unwrap_or_default();
                        let cp = if p.is_empty() {
                            nm
                        } else {
                            format!("{p}/{nm}")
                        };
                        stack.push((ch.clone(), cp));
                    }
                }
            }
            for cl in clips {
                let Some((binds, keys)) = clip_pptr.get(&cl) else {
                    continue;
                };
                let paths: BTreeSet<u32> = binds.iter().map(|b| b.0).collect();
                let only_sprites = binds
                    .iter()
                    .all(|b| b.1 == SPRITE_RENDERER as i64 && b.2 == 0);
                let targets_of: Vec<Key> = paths
                    .iter()
                    .filter_map(|h| by_hash.get(h))
                    .filter_map(|t| gos.get(t))
                    .flat_map(|t| t.renderers.iter().cloned())
                    .collect();
                let target_ok = paths.len() == 1
                    && only_sprites
                    && targets_of.len() == 1
                    && ours(&targets_of[0]);
                let any_pal = targets_of
                    .iter()
                    .any(|r| palette_renderer.get(r) == Some(&true));
                for s in keys {
                    mark(&mut use_ok, s, target_ok);
                    if any_pal {
                        pal_use.insert(s.clone());
                    }
                }
            }
        }
    }
    keep.extend(pal_use.iter().filter_map(|s| sprite_tex.get(s)).cloned());
    for (m, texs) in &mat_tex {
        if pal_mats.contains(m) {
            keep.extend(texs.iter().cloned());
        }
    }
    if !index {
        return Err("exact_index off".into());
    }
    // candidates: textures whose every sprite is used only through our renderers, at least one of them
    let mut by_tex: HashMap<Key, bool> = HashMap::new();
    for (s, t) in &sprite_tex {
        let e = by_tex.entry(t.clone()).or_insert(true);
        *e = *e && use_ok.get(s).copied().unwrap_or(true);
    }
    let mut candidate_tex: HashSet<Key> = sprite_tex
        .iter()
        .filter(|(s, t)| use_ok.get(*s) == Some(&true) && by_tex.get(*t) == Some(&true))
        .map(|(_, t)| t.clone())
        .collect();
    // materials of other shaders that name a candidate texture
    for (m, texs) in &mat_tex {
        if !pal_mats.contains(m) {
            for t in texs {
                candidate_tex.remove(t);
            }
        }
    }
    // 3. any other object that names a candidate texture, one of its sprites, a clip that sets one or a
    // controller that holds such a clip (a script can set it on another Animator)
    let cand_sprites: HashSet<Key> = sprite_tex
        .iter()
        .filter(|(_, t)| candidate_tex.contains(*t))
        .map(|(s, _)| s.clone())
        .collect();
    let mut want: HashSet<Key> = candidate_tex
        .iter()
        .cloned()
        .chain(cand_sprites.iter().cloned())
        .collect();
    let mut clip_of_sprites: HashMap<Key, Vec<Key>> = HashMap::new();
    for (cl, (_, keys)) in &clip_pptr {
        let hit: Vec<Key> = keys
            .iter()
            .filter(|k| cand_sprites.contains(*k))
            .cloned()
            .collect();
        if !hit.is_empty() {
            want.insert(cl.clone());
            for c in clip_ctrls.get(cl).into_iter().flatten() {
                want.insert(c.clone());
                clip_of_sprites
                    .entry(c.clone())
                    .or_default()
                    .extend(hit.iter().cloned());
            }
            clip_of_sprites.entry(cl.clone()).or_default().extend(hit);
        }
    }
    // override controllers of a held controller
    for (ck, (_, base)) in &ctrl_clips {
        if let Some(b) = base {
            if want.contains(b) {
                want.insert(ck.clone());
                let hit = clip_of_sprites.get(b).cloned().unwrap_or_default();
                clip_of_sprites.entry(ck.clone()).or_default().extend(hit);
            }
        }
    }
    let mut named: HashSet<Key> = HashSet::new();
    let debug = std::env::var_os("PMU_EXACTINDEX_DEBUG").is_some();
    let mut gen: Option<crate::monotree::Generator> = None;
    if debug {
        let walk: BTreeSet<&Key> = pal_use.iter().filter_map(|s| sprite_tex.get(s)).collect();
        for t in walk {
            let sprites: Vec<&Key> = sprite_tex
                .iter()
                .filter(|(_, tk)| *tk == t)
                .map(|(s, _)| s)
                .collect();
            let bad = sprites
                .iter()
                .filter(|s| use_ok.get(**s) == Some(&false))
                .count();
            eprintln!(
                "[exactindex] texture {}:{}: {} sprites, {} with other uses, candidate {}",
                t.0,
                t.1,
                sprites.len(),
                bad,
                candidate_tex.contains(t)
            );
        }
    }
    rss_note("before pass 4");
    const PASS4_SKIP: [i32; 15] = [
        GAME_OBJECT, TRANSFORM, RECT_TRANSFORM, SPRITE, TEXTURE2D, CLIP, CONTROLLER, OVERRIDE, SPRITE_RENDERER, ANIMATOR,
        MATERIAL, SHADER, MONO_SCRIPT, PRELOAD_DATA, ASSET_BUNDLE,
    ];
    let want_files: HashSet<&str> = want.iter().map(|k| &*k.0).collect();
    for n in &names {
        // (raw_refs finds only keys of the file itself or of its externals)
        if !census.get(n).is_some_and(|c| {
            c.0.iter().any(|x| !PASS4_SKIP.contains(x)) && c.1.iter().any(|x| want_files.contains(&**x))
        }) {
            continue;
        }
        let Some(f) = open(n) else { continue };
        for o in &f.sf.objects {
            if PASS4_SKIP.contains(&o.class_id) {
                continue;
            }
            if let Ok(b) = f.sf.bytes(o.path_id) {
                let before = named.len();
                let mut hit = HashSet::new();
                raw_refs(&f, n, &b, &want, &mut hit);
                if !hit.is_empty() {
                    // a raw match: read the object exactly where its layout is known (built-in classes
                    // by the class database, MonoBehaviours by their script's class); else it counts
                    if let Some(exact) =
                        exact_refs(&f, n, o.path_id, o.class_id, &scripts_full, &mut gen, job)
                    {
                        hit.retain(|k| exact.contains(k));
                    }
                }
                if debug && !hit.is_empty() {
                    for h in &hit {
                        eprintln!(
                            "[exactindex] {}:{} named by {}:{} class {}",
                            h.0, h.1, n, o.path_id, o.class_id
                        );
                    }
                }
                named.extend(hit);
                let _ = before;
            }
        }
    }
    let mut bad_sprites: HashSet<Key> = HashSet::new();
    for k in &named {
        if cand_sprites.contains(k) {
            bad_sprites.insert(k.clone());
        }
        if let Some(sp) = clip_of_sprites.get(k) {
            bad_sprites.extend(sp.iter().cloned());
        }
    }
    let mut why = Vec::new();
    let mut textures = BTreeMap::new();
    let mut opaque_idx = [false; 256];
    // (each candidate's pixels read twice, one texture at a time: kept together they were more than a 1 GB
    // handheld has, Blasphemous's 24 sprite sheets)
    let mut cands: Vec<(Key, bool)> = Vec::new();
    let mut candidates: Vec<&Key> = candidate_tex.iter().collect();
    candidates.sort();
    for t in candidates {
        if named.contains(t)
            || sprite_tex
                .iter()
                .any(|(s, tk)| tk == t && bad_sprites.contains(s))
        {
            continue;
        }
        let Some(f) = open(&t.0) else { continue };
        let Ok(tt) = f.sf.read_typetree_owned(t.1, 1 << 20) else {
            continue;
        };
        let fmt = tt
            .get("m_TextureFormat")
            .and_then(Value::as_int)
            .unwrap_or(-1) as i64;
        let mips = tt.get("m_MipCount").and_then(Value::as_int).unwrap_or(1);
        let streamed = tt
            .path(&["m_StreamData", "size"])
            .and_then(Value::as_int)
            .is_some_and(|s| s > 0);
        let filter = tt
            .path(&["m_TextureSettings", "m_FilterMode"])
            .and_then(Value::as_int);
        let w = tt.get("m_Width").and_then(Value::as_int).unwrap_or(0) as usize;
        let h = tt.get("m_Height").and_then(Value::as_int).unwrap_or(0) as usize;
        let px = tt.get("image data").and_then(Value::as_bytes).unwrap_or(&[]);
        if fmt != RGBA32
            || mips != 1
            || streamed
            || filter != Some(0)
            || px.len() != w * h * 4
            || px.is_empty()
        {
            why.push(format!(
                "{} {}: format {fmt}, mips {mips}, streamed {streamed}, filter {filter:?}: left as it is",
                t.0, t.1
            ));
            continue;
        }
        for p in px.chunks_exact(4) {
            if p[3] != 0 {
                opaque_idx[p[2] as usize] = true;
            }
        }
        let binary = px.chunks_exact(4).all(|p| p[3] == 0 || p[3] == 255);
        cands.push((t.clone(), binary));
    }
    let Some(k) = (1..256).find(|&i| !opaque_idx[i]) else {
        return Err("every index is used by an opaque texel: no reserved index".into());
    };
    let k = k as u8;
    for (t, binary) in &cands {
        if !*binary {
            why.push(format!(
                "{} {}: alpha other than 0 and 255: left as it is",
                t.0, t.1
            ));
            continue;
        }
        let form = Form::R8;
        let tt = open(&t.0)
            .ok_or_else(|| format!("{} {}: not readable again", t.0, t.1))?
            .sf
            .read_typetree_owned(t.1, 1 << 20)
            .map_err(|e| format!("{} {}: {e}", t.0, t.1))?;
        let px = tt.get("image data").and_then(Value::as_bytes).unwrap_or(&[]);
        match encode(px, form, k) {
            Ok(_) => {
                textures.insert(t.clone(), form);
            }
            Err(e) => why.push(format!("{} {}: {e}: left as it is", t.0, t.1)),
        }
    }
    if textures.is_empty() {
        return Err("no texture is drawn only through those materials".into());
    }
    // 4. the textures the rewritten programs still sample as they are (palette sprites not converted):
    // no RGBA32 texel with alpha 1 and blue 0 that the inserted line would read otherwise
    let kept: BTreeSet<Key> = pal_use
        .iter()
        .filter_map(|s| sprite_tex.get(s))
        .filter(|t| !textures.contains_key(*t))
        .cloned()
        .collect();
    for t in &kept {
        let Some(f) = open(&t.0) else { continue };
        let Ok(tt) = f.sf.read_typetree_owned(t.1, 1 << 20) else {
            continue;
        };
        let fmt = tt
            .get("m_TextureFormat")
            .and_then(Value::as_int)
            .unwrap_or(-1) as i64;
        if fmt != RGBA32 {
            why.push(format!(
                "{} {}: format {fmt}, drawn through the palette as it is (not a texture of exact_shaders' walk)",
                t.0, t.1
            ));
            continue;
        }
        let px = tt
            .get("image data")
            .and_then(Value::as_bytes)
            .unwrap_or(&[]);
        for p in px.chunks_exact(4) {
            if p[3] == 255 && p[2] == 0 && p[1] == 0 && p[0] != 0 {
                return Err(format!(
                    "{} {}: an RGBA32 texel ({}, {}, 0, 255) the inserted line would read otherwise",
                    t.0, t.1, p[0], p[1]
                ));
            }
        }
    }
    // 5. the programs: every GLES3 entry of the keywords must take the rewrite
    let mut shader_set = BTreeSet::new();
    for s in &targets {
        let Some(f) = open(&s.0) else { continue };
        let t = f.sf.read_typetree(s.1).map_err(|e| e.to_string())?;
        let n =
            rewrite_shader(&mut t.clone(), &kw, k).map_err(|e| format!("{}: {e}", shaders[s]))?;
        if n > 0 {
            shader_set.insert(s.clone());
        }
    }
    if shader_set.is_empty() {
        return Err("no GLES3 program of those keywords".into());
    }
    Ok(Plan {
        k,
        textures,
        shaders: shader_set,
        why,
    })
}

/// The GameObject of a Transform key, in the same file.
fn open_go(f: &DataFile, n: &str, tk: &Key) -> Option<Key> {
    if &*tk.0 != n {
        return None;
    }
    let t = f.sf.read_typetree(tk.1).ok()?;
    t.get("m_GameObject").and_then(|v| key(f, n, v))
}

/// Rewrites the GLES3 entries whose keywords include all of `kw`; the number rewritten.
pub fn rewrite_shader(t: &mut Value, kw: &[String], k: u8) -> std::result::Result<usize, String> {
    let platforms = t
        .get("platforms")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    if !platforms
        .iter()
        .any(|p| p.as_int() == Some(blob::PLATFORM_GLES3PLUS as i128))
    {
        return Ok(0);
    }
    let mut blobs = tree::blobs(t)?;
    let mut entries = tree::platform_entries(t, blob::PLATFORM_GLES3PLUS, &blobs)?;
    let segs = blobs
        .segments(blob::PLATFORM_GLES3PLUS)
        .map_err(|e| e.to_string())?;
    let mut n = 0;
    for e in entries.iter_mut().flatten() {
        let Slot::Program(p) = e else { continue };
        if p.code.is_empty() || !kw.iter().all(|k| p.keywords.iter().any(|x| x == k)) {
            continue;
        }
        let code =
            String::from_utf8(p.code.clone()).map_err(|_| "a program is not UTF-8".to_string())?;
        if code.contains(".w == 0.0){") {
            return Err("already rewritten".into());
        }
        let new = rewrite_program(&code, k)?;
        p.code = new.into_bytes();
        n += 1;
    }
    if n > 0 {
        blobs.set_entries(blob::PLATFORM_GLES3PLUS, &entries, &segs, None);
        let fields = blobs.store().map_err(|e| e.to_string())?;
        tree::set_blob(t, &fields)?;
    }
    Ok(n)
}

pub fn plan_json(p: &Plan) -> Json {
    let mut d = Json::dict();
    d.set("k", Json::Int(p.k as i128));
    d.set(
        "textures",
        Json::List(
            p.textures
                .iter()
                .map(|((f, pid), form)| {
                    Json::List(vec![
                        Json::Str(f.to_string()),
                        Json::Int(*pid as i128),
                        Json::Str(match form {
                            Form::R8 => "r8".into(),
                        }),
                    ])
                })
                .collect(),
        ),
    );
    d.set(
        "shaders",
        Json::List(
            p.shaders
                .iter()
                .map(|(f, pid)| Json::List(vec![Json::Str(f.to_string()), Json::Int(*pid as i128)]))
                .collect(),
        ),
    );
    d
}

fn plan_of(j: &Json) -> Option<Plan> {
    let k = j.get("k")?.as_int()? as u8;
    let mut textures = BTreeMap::new();
    for t in j.get("textures")?.as_list()? {
        let l = t.as_list()?;
        if l.get(2)?.as_str()? != "r8" {
            return None;
        }
        let form = Form::R8;
        textures.insert(
            (super::exacttex::name(l.first()?.as_str()?), l.get(1)?.as_int()? as i64),
            form,
        );
    }
    let mut shaders = BTreeSet::new();
    for s in j.get("shaders")?.as_list()? {
        let l = s.as_list()?;
        shaders.insert((super::exacttex::name(l.first()?.as_str()?), l.get(1)?.as_int()? as i64));
    }
    Some(Plan {
        k,
        textures,
        shaders,
        why: Vec::new(),
    })
}

/// The plan for the step when the journal has none: the walk, or why a walk before it found none
/// (the index kept), without walking again.
fn plan_or_kept(job: &Job, c: &Json) -> std::result::Result<Plan, String> {
    if let Some(k) = load_kept(job, c, true) {
        if let Some(why) = k.none.or(k.plan_err) {
            return Err(why);
        }
    }
    let before = fingerprint(job);
    let r = analyze(job, c, true);
    let kept = match &r {
        Err(why) => Some(Kept {
            none: Some(why.clone()),
            ..Default::default()
        }),
        Ok((keep, Err(why))) => Some(Kept {
            keep: keep.clone(),
            plan_err: Some(why.clone()),
            none: None,
        }),
        // (a plan found goes into the journal: the caller's)
        Ok((_, Ok(_))) => None,
    };
    if let Some(k) = kept {
        save_kept(job, c, true, &before, &k);
    }
    r.and_then(|(_, r)| r)
}

// ---- The index kept
//
// The walk above reads every data file of the game (Blasphemous: 3945 files, 150 - 160 s on a handheld)
// and finds the same each time. What it found is kept beside the journal (`.pmunity-exact.json`, written
// through a .tmp file, synced and renamed as the journal is), and a run that goes on after a stop reads
// it instead of walking again. It is used only when nothing it was made from has changed:
// - the settings the walk reads (`exact_shaders`, `exact_keywords`, `exact_index`, `exact_index_skip`) and
//   the program (its size and time: another build of pmu walks again);
// - the data files: the same names, each with the size and time it had when the walk began, or named in
//   the journal as done by a step (the conversion's own change: the journal already vouches for those). A
//   file that changed and that no step has marked (other game files, a file put in place a moment
//   before a stop and not yet marked) makes the run walk again and keep the new index.
// Anything else (no file, a damaged one, another version of it): the walk, as before.

/// The kept index's file name in the output folder (a dot file: no step lists it, packing leaves it out).
pub const KEPT: &str = ".pmunity-exact.json";
const KEPT_VERSION: i128 = 1;

/// What a walk found: the textures left as they are, why there is no plan (with `exact_index`; None
/// with a plan, which the journal holds, or without `exact_index`), or why there is nothing at all (no
/// shader of `exact_shaders`).
#[derive(Default, Debug, Clone, PartialEq)]
pub struct Kept {
    pub keep: BTreeSet<Key>,
    pub plan_err: Option<String>,
    pub none: Option<String>,
}

/// The data files the walk reads, each with its size and time (texmemo::file_id).
pub fn fingerprint(job: &Job) -> Vec<(String, String)> {
    data_files(job)
        .into_iter()
        .map(|n| {
            let id = super::texmemo::file_id(&job.path_in(&n));
            (n, id)
        })
        .collect()
}

fn kept_settings(c: &Json, index: bool) -> Json {
    Json::List(
        ["exact_shaders", "exact_keywords", "exact_index_skip"]
            .iter()
            .map(|k| Json::Str(opt(c, k).to_str()))
            .chain([Json::Bool(index), Json::Str(super::texmemo::tool_id())])
            .collect(),
    )
}

/// The index a run before this one kept, when it still holds (see above).
pub fn load_kept(job: &Job, c: &Json, index: bool) -> Option<Kept> {
    let text = std::fs::read_to_string(job.out.join(KEPT)).ok()?;
    let j = crate::pyjson::loads(&text).ok()?;
    if j.get("version")?.as_int()? != KEPT_VERSION
        || j.get("settings")?.dumps() != kept_settings(c, index).dumps()
    {
        return None;
    }
    let mut then: HashMap<&str, &str> = HashMap::new();
    for f in j.get("files")?.as_list()? {
        let l = f.as_list()?;
        then.insert(l.first()?.as_str()?, l.get(1)?.as_str()?);
    }
    let mut marked: HashSet<&str> = HashSet::new();
    if let Some(Json::Dict(done)) = job.state.get("done") {
        for (_, files) in done {
            marked.extend(
                files
                    .as_list()
                    .into_iter()
                    .flatten()
                    .filter_map(Json::as_str),
            );
        }
    }
    let now = fingerprint(job);
    if now.len() != then.len() {
        return None;
    }
    for (n, id) in &now {
        let was = then.get(n.as_str())?;
        if was != id && !marked.contains(n.as_str()) {
            return None;
        }
    }
    let mut keep = BTreeSet::new();
    for k in j.get("keep")?.as_list()? {
        let l = k.as_list()?;
        keep.insert((
            super::exacttex::name(l.first()?.as_str()?),
            i64::try_from(l.get(1)?.as_int()?).ok()?,
        ));
    }
    let why = |k: &str| match j.get(k) {
        Some(Json::Str(s)) => Some(Some(s.clone())),
        Some(Json::Null) => Some(None),
        _ => None,
    };
    Some(Kept {
        keep,
        plan_err: why("plan_err")?,
        none: why("none")?,
    })
}

/// Keeps what a walk found, with the data files as they were when it began (best effort: without it
/// the next run walks again).
pub fn save_kept(job: &Job, c: &Json, index: bool, before: &[(String, String)], k: &Kept) {
    let why = |w: &Option<String>| w.clone().map_or(Json::Null, Json::Str);
    let mut d = Json::dict();
    d.set("version", Json::Int(KEPT_VERSION));
    d.set("settings", kept_settings(c, index));
    d.set(
        "files",
        Json::List(
            before
                .iter()
                .map(|(n, id)| Json::List(vec![Json::Str(n.clone()), Json::Str(id.clone())]))
                .collect(),
        ),
    );
    d.set(
        "keep",
        Json::List(
            k.keep
                .iter()
                .map(|(f, pid)| Json::List(vec![Json::Str(f.to_string()), Json::Int(*pid as i128)]))
                .collect(),
        ),
    );
    d.set("plan_err", why(&k.plan_err));
    d.set("none", why(&k.none));
    let path = job.out.join(KEPT);
    let tmp = crate::job::with_suffix(&path, ".tmp");
    let r = std::fs::File::create(&tmp)
        .and_then(|mut f| {
            use std::io::Write as _;
            f.write_all(d.dumps().as_bytes())?;
            f.sync_all()
        })
        .and_then(|_| std::fs::rename(&tmp, &path));
    match r {
        Ok(()) => unityfs::bgwrite::sync_dir(&job.out),
        Err(_) => {
            let _ = std::fs::remove_file(&tmp);
        }
    }
}

pub fn run(job: &mut Job) -> Result<()> {
    let c = section(&job.cfg, "textures", &super::textures::defaults())?;
    if opt(&c, "exact_index").to_str() != "blue" || opt(&c, "exact_shaders").to_str().is_empty() {
        return Ok(());
    }
    let kw: Vec<String> = opt(&c, "exact_keywords")
        .to_str()
        .split_whitespace()
        .map(str::to_string)
        .collect();
    let p = match job.state.get(STATE).and_then(plan_of) {
        Some(p) => p,
        None => match plan_or_kept(job, &c) {
            Ok(p) => {
                for w in &p.why {
                    job.log(&format!("{STEP}: {w}"));
                }
                job.state.set(STATE, plan_json(&p));
                job.save_state()?;
                p
            }
            Err(why) => {
                job.log(&format!(
                    "{STEP}: {why}: the textures stay as the game has them"
                ));
                return Ok(());
            }
        },
    };
    let n8 = p.textures.len();
    job.log(&format!(
        "{STEP}: {n8} textures as R8 (reserved index {}), {} shaders' GLES3 programs with {} read them",
        p.k,
        p.shaders.len(),
        kw.join(" ")
    ));
    let mut files: BTreeSet<String> = p.textures.keys().map(|k| k.0.to_string()).collect();
    files.extend(p.shaders.iter().map(|k| k.0.to_string()));
    let (mut bytes_in, mut bytes_out) = (0u64, 0u64);
    for rel in files {
        if job.is_done(STEP, &rel) {
            continue;
        }
        let mut f = job.open(&job.path_in(&rel), Default::default())?;
        for (t, form) in p.textures.iter().filter(|(k, _)| *k.0 == *rel) {
            // (the pixels moved out of the object as it is read, and out of the value: held once, not
            // three times; "image data" is set again below)
            let mut tt = f.sf.read_typetree_owned(t.1, 1 << 20)?;
            let px = match tt.get_mut("image data") {
                Some(Value::Bytes(b)) => std::mem::take(b),
                _ => Vec::new(),
            };
            let data = encode(&px, *form, p.k)
                .map_err(|e| Error::Value(format!("{STEP}: {rel} {}: {e}", t.1)))?;
            bytes_in += px.len() as u64;
            bytes_out += data.len() as u64;
            tt.set("m_TextureFormat", Value::I32(R8 as i32))?;
            tt.set("m_CompleteImageSize", Value::I32(data.len() as i32))?;
            tt.set("image data", Value::Bytes(data))?;
            f.sf.save_typetree(t.1, &tt)?;
        }
        for s in p.shaders.iter().filter(|k| *k.0 == *rel) {
            let mut t = f.sf.read_typetree(s.1)?;
            rewrite_shader(&mut t, &kw, p.k)
                .map_err(|e| Error::Value(format!("{STEP}: {rel} {}: {e}", s.1)))?;
            f.sf.save_typetree(s.1, &t)?;
        }
        job.write_with(&rel, |tmp| Ok(f.write_to(tmp, false)?))?;
        job.mark_done(STEP, &rel)?;
    }
    if bytes_in > 0 {
        job.log(&format!(
            "{STEP}: {:.0} -> {:.0} MB",
            bytes_in as f64 / 1048576.0,
            bytes_out as f64 / 1048576.0
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc_is_zlibs() {
        assert_eq!(crc32(b"RecolorSprite"), 1955683018);
        assert_eq!(crc32(b""), 0);
    }

    #[test]
    fn encode_reads_back() {
        // (r, g, b, a): opaque, transparent, opaque
        let px = [9, 9, 7, 255, 1, 2, 3, 0, 5, 5, 200, 255];
        assert_eq!(encode(&px, Form::R8, 4).unwrap(), vec![7, 4, 200]);
        assert!(encode(&px, Form::R8, 7).is_err()); // the reserved index is used
        let half = [0, 0, 7, 128];
        assert!(encode(&half, Form::R8, 4).is_err());
    }

    #[test]
    fn rewrites_the_palette_fetch() {
        let code = "void main()\n{\n    u_xlat0 = texture(_MainTex, vs_TEXCOORD0.xy).wxyz;\n    u_xlat1.w = vs_COLOR0.w * vs_COLOR0.w;\n    u_xlat0.y = 0.0;\n    u_xlat10_2 = texture(_PaletteTex, u_xlat0.wy);\n    u_xlat0.yzw = vs_COLOR0.xyz;\n    SV_Target0 = u_xlat0;\n}\n";
        let r = rewrite_program(code, 3).unwrap();
        assert!(r.contains(
            "if(u_xlat0.x == 1.0 && u_xlat0.z == 0.0 && u_xlat0.w == 0.0){ u_xlat0.w = u_xlat0.y;"
        ));
        assert!(r.contains("0.011764706"));
        // a program that reads the texel's red or green
        let bad = "    u_xlat0 = texture(_MainTex, vs_TEXCOORD0.xy).wxyz;\n    SV_Target0.x = u_xlat0.y;\n}\n";
        assert!(rewrite_program(bad, 3).is_err());
        let bad2 = "    u_xlat0 = texture(_MainTex, vs_TEXCOORD0.xy).wxyz;\n    SV_Target0 = u_xlat0 * 2.0;\n}\n";
        assert!(rewrite_program(bad2, 3).is_err());
    }
}
