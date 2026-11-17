//! The textures step's `exact_shaders`: the textures a game samples through materials whose shader
//! reads them as data, not as colours, found in the data at setup so that the step leaves them as they
//! are. Blasphemous's `Maikel/MaikelSpriteMasterShader` (with the keyword `PALETTE_ON`) takes a sprite
//! texel's blue channel as the x of a 256x1 palette texture and outputs that palette colour: any change
//! of a blue value (ASTC's, a scale's) picks another palette entry, and the Penitent turns magenta.
//!
//! A texture is taken when a material whose shader's name matches `exact_shaders` (and that has every
//! keyword of `exact_keywords`) samples it:
//! - the material's own textures (m_SavedProperties' m_TexEnvs);
//! - the sprite of every SpriteRenderer drawn with such a material;
//! - every sprite that an Animator on that renderer's GameObject or one of its parents sets in its
//!   clips (the controller's clips, an override controller's and its base's): all of a clip's sprite
//!   keys (pptrCurveMapping), also those it sets on other renderers of the same Animator (more kept
//!   than needed, never fewer).
//!
//! Not found: a material a script makes at run time (`new Material(Shader.Find(...))`), a sprite a
//! script sets, UI Images (MonoBehaviours). `exact_names` (a regular expression on texture names) adds
//! those by hand.
use crate::job::Job;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use unityfs::{DataFile, Value};

/// A data file's name and a path id.
pub type Key = (String, i64);

const MATERIAL: i32 = 21;
const SHADER: i32 = 48;
const GAME_OBJECT: i32 = 1;
const TRANSFORM: i32 = 4;
const RECT_TRANSFORM: i32 = 224;
const ANIMATOR: i32 = 95;
const CONTROLLER: i32 = 91;
const OVERRIDE: i32 = 221;
const CLIP: i32 = 74;
const SPRITE: i32 = 213;
const SPRITE_RENDERER: i32 = 212;
const TEXTURE2D: i32 = 28;

/// The data files the walk reads (serialized files: not bundles), in the textures step's order.
fn data_files(job: &Job) -> Vec<String> {
    job.list("", |n| {
        n.ends_with(".assets") || n == "globalgamemanagers" || super::shaders::is_level(n)
    })
}

fn pptr_of(v: &Value) -> Option<(i32, i64)> {
    let f = i32::try_from(v.get("m_FileID")?.as_int()?).ok()?;
    let p = i64::try_from(v.get("m_PathID")?.as_int()?).ok()?;
    Some((f, p))
}

/// The file a PPtr's file id names, seen from `from` (0: `from` itself), by its base name.
fn file_name(f: &DataFile, from: &str, fid: i32) -> Option<String> {
    if fid == 0 {
        return Some(from.to_string());
    }
    let e = f.sf.externals.get(usize::try_from(fid - 1).ok()?)?;
    let p = String::from_utf8_lossy(&e.path).into_owned();
    Some(p.rsplit('/').next().unwrap_or(&p).to_string())
}

/// A PPtr as a key (None for a null reference or an unknown file id).
fn key(f: &DataFile, from: &str, v: &Value) -> Option<Key> {
    let (fid, pid) = pptr_of(v)?;
    if pid == 0 {
        return None;
    }
    Some((file_name(f, from, fid)?, pid))
}

fn class_of(f: &DataFile, pid: i64) -> Option<i32> {
    f.sf.object(pid).map(|o| o.class_id)
}

/// The data files opened on demand, kept open (the walk's second half opens few).
struct Files<'a> {
    job: &'a Job,
    open: BTreeMap<String, Option<DataFile>>,
}

impl Files<'_> {
    fn get(&mut self, name: &str) -> Option<&DataFile> {
        if !self.open.contains_key(name) {
            let p = self.job.path_in(name);
            let f = if p.is_file() {
                self.job.open(&p, Default::default()).ok()
            } else {
                None
            };
            self.open.insert(name.to_string(), f);
        }
        self.open.get(name).and_then(Option::as_ref)
    }

    fn tree(&mut self, k: &Key, class: i32) -> Option<(Value, String)> {
        let f = self.get(&k.0)?;
        if class_of(f, k.1)? != class {
            return None;
        }
        Some((f.sf.read_typetree(k.1).ok()?, k.0.clone()))
    }

    fn key_in(&mut self, from: &str, v: &Value) -> Option<Key> {
        let f = self.get(from)?;
        key(f, from, v)
    }
}

/// What the walk found: the textures (by file and path id), with the materials and renderers it
/// went through, for the log.
#[derive(Default, Debug)]
pub struct Found {
    pub textures: BTreeSet<Key>,
    pub materials: Vec<String>,
    pub renderers: usize,
    pub animators: usize,
}

/// A material's name if its shader matches and it has every keyword.
fn material_matches(
    f: &DataFile,
    from: &str,
    t: &Value,
    shaders: &HashMap<Key, String>,
    rx: &regex::Regex,
    keywords: &[String],
) -> bool {
    let Some(s) = t.get("m_Shader").and_then(|v| key(f, from, v)) else {
        return false;
    };
    if !shaders.get(&s).is_some_and(|n| rx.is_match(n)) {
        return false;
    }
    let kw = t
        .get("m_ShaderKeywords")
        .and_then(Value::as_bytes)
        .map(|b| String::from_utf8_lossy(b).into_owned())
        .unwrap_or_default();
    let have: HashSet<&str> = kw.split_whitespace().collect();
    keywords.iter().all(|k| have.contains(k.as_str()))
}

fn shader_name(t: &Value) -> String {
    let n = t
        .path(&["m_ParsedForm", "m_Name"])
        .or_else(|| t.get("m_Name"))
        .and_then(Value::as_bytes)
        .unwrap_or(&[]);
    String::from_utf8_lossy(n).into_owned()
}

/// The textures sampled through materials whose shader's name matches `rx` and that have all of
/// `keywords`.
pub fn find(job: &Job, rx: &regex::Regex, keywords: &[String]) -> Found {
    let names = data_files(job);
    // 1. the shaders' names
    let mut shaders: HashMap<Key, String> = HashMap::new();
    for n in &names {
        let Ok(f) = job.open(&job.path_in(n), Default::default()) else {
            continue;
        };
        for o in f.sf.objects.iter().filter(|o| o.class_id == SHADER) {
            if let Ok(t) = f.sf.read_typetree(o.path_id) {
                shaders.insert((n.clone(), o.path_id), shader_name(&t));
            }
        }
    }
    if !shaders.values().any(|s| rx.is_match(s)) {
        return Found::default();
    }
    // 2. the materials, their textures, the renderers drawn with them and the Animators above those
    let mut found = Found::default();
    let mut mats: HashSet<Key> = HashSet::new();
    let mut controllers: BTreeSet<Key> = BTreeSet::new();
    let mut sprites: BTreeSet<Key> = BTreeSet::new();
    for n in &names {
        let Ok(f) = job.open(&job.path_in(n), Default::default()) else {
            continue;
        };
        for o in f.sf.objects.iter().filter(|o| o.class_id == MATERIAL) {
            let Ok(t) = f.sf.read_typetree(o.path_id) else {
                continue;
            };
            if !material_matches(&f, n, &t, &shaders, rx, keywords) {
                continue;
            }
            mats.insert((n.clone(), o.path_id));
            found.materials.push(
                t.get("m_Name")
                    .and_then(Value::as_bytes)
                    .map(|b| String::from_utf8_lossy(b).into_owned())
                    .unwrap_or_default(),
            );
            for e in t
                .path(&["m_SavedProperties", "m_TexEnvs"])
                .and_then(Value::as_array)
                .into_iter()
                .flatten()
            {
                let env = match e {
                    Value::Pair(p) => &p.1,
                    other => other.get("second").unwrap_or(other),
                };
                if let Some(k) = env.get("m_Texture").and_then(|v| key(&f, n, v)) {
                    found.textures.insert(k);
                }
            }
        }
    }
    if mats.is_empty() {
        return found;
    }
    for n in &names {
        let Ok(f) = job.open(&job.path_in(n), Default::default()) else {
            continue;
        };
        for o in
            f.sf.objects
                .iter()
                .filter(|o| o.class_id == SPRITE_RENDERER)
        {
            let Ok(t) = f.sf.read_typetree(o.path_id) else {
                continue;
            };
            let uses = t
                .get("m_Materials")
                .and_then(Value::as_array)
                .into_iter()
                .flatten()
                .filter_map(|m| key(&f, n, m))
                .any(|k| mats.contains(&k));
            if !uses {
                continue;
            }
            found.renderers += 1;
            if let Some(s) = t.get("m_Sprite").and_then(|v| key(&f, n, v)) {
                sprites.insert(s);
            }
            // the Animators of its GameObject and its parents (a hierarchy lives in one file)
            let mut go = t.get("m_GameObject").and_then(pptr_of);
            for _ in 0..256 {
                let Some((0, g)) = go else { break };
                let Some(gt) = (class_of(&f, g) == Some(GAME_OBJECT))
                    .then(|| f.sf.read_typetree(g).ok())
                    .flatten()
                else {
                    break;
                };
                let mut father = None;
                for c in gt
                    .get("m_Component")
                    .and_then(Value::as_array)
                    .into_iter()
                    .flatten()
                {
                    let c = match c {
                        Value::Pair(p) => &p.1,
                        other => other.get("component").unwrap_or(other),
                    };
                    let Some((0, cp)) = pptr_of(c) else { continue };
                    match class_of(&f, cp) {
                        Some(ANIMATOR) => {
                            if let Some(k) =
                                f.sf.read_typetree(cp)
                                    .ok()
                                    .and_then(|a| a.get("m_Controller").and_then(|v| key(&f, n, v)))
                            {
                                controllers.insert(k);
                            }
                        }
                        Some(TRANSFORM) | Some(RECT_TRANSFORM) => {
                            father =
                                f.sf.read_typetree(cp)
                                    .ok()
                                    .and_then(|tr| tr.get("m_Father").and_then(pptr_of))
                                    .filter(|p| p.1 != 0);
                        }
                        _ => {}
                    }
                }
                let Some((0, fp)) = father else { break };
                go =
                    f.sf.read_typetree(fp)
                        .ok()
                        .and_then(|tr| tr.get("m_GameObject").and_then(pptr_of));
            }
        }
    }
    // 3. the controllers' clips and their sprite keys; the sprites' textures
    let mut files = Files {
        job,
        open: BTreeMap::new(),
    };
    found.animators = controllers.len();
    let mut seen: HashSet<Key> = HashSet::new();
    let mut todo: Vec<Key> = controllers.into_iter().collect();
    let mut clips: BTreeSet<Key> = BTreeSet::new();
    while let Some(c) = todo.pop() {
        if !seen.insert(c.clone()) {
            continue;
        }
        if let Some((t, from)) = files.tree(&c, CONTROLLER) {
            for v in t
                .get("m_AnimationClips")
                .and_then(Value::as_array)
                .into_iter()
                .flatten()
            {
                if let Some(k) = files.key_in(&from, v) {
                    clips.insert(k);
                }
            }
        } else if let Some((t, from)) = files.tree(&c, OVERRIDE) {
            if let Some(k) = t.get("m_Controller").and_then(|v| files.key_in(&from, v)) {
                todo.push(k);
            }
            for e in t
                .get("m_Clips")
                .and_then(Value::as_array)
                .into_iter()
                .flatten()
            {
                for side in ["m_OriginalClip", "m_OverrideClip"] {
                    if let Some(k) = e.get(side).and_then(|v| files.key_in(&from, v)) {
                        clips.insert(k);
                    }
                }
            }
        }
    }
    for c in &clips {
        let Some((t, from)) = files.tree(c, CLIP) else {
            continue;
        };
        for v in t
            .path(&["m_ClipBindingConstant", "pptrCurveMapping"])
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
        {
            if let Some(k) = files.key_in(&from, v) {
                sprites.insert(k);
            }
        }
    }
    for s in &sprites {
        let Some((t, from)) = files.tree(s, SPRITE) else {
            continue;
        };
        for field in ["texture", "alphaTexture"] {
            if let Some(k) = t
                .path(&["m_RD", field])
                .and_then(|v| files.key_in(&from, v))
            {
                if files.get(&k.0).and_then(|f| class_of(f, k.1)) == Some(TEXTURE2D) {
                    found.textures.insert(k);
                }
            }
        }
    }
    found
}
