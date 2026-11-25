//! Step `lazyref` (pmu's own): a prefab that a scene's script names in a field, but that the game uses only at
//! one moment, is loaded by name at that moment instead of with the scene. A scene loads everything its objects
//! reach, so a script field naming a prefab loads the prefab and everything its animators reach with the scene
//! (Blasphemous: Crisanta's two ending prefabs, five sprite sheets, about 92 MB of textures, loaded with her arena,
//! which then runs out of memory on a 1 GB handheld). The lazy step cannot help there: it shortens the preload
//! lists, but the field still loads the prefab. For each prefab named in the settings the step:
//!   - clears the field: in every MonoBehaviour of the named script (its MonoScript's m_ClassName) the 12 bytes
//!     of a reference to the prefab become a null reference (the same length);
//!   - leaves the prefab and everything it reaches in its file out of every PreloadData and ResourceManager
//!     dependency list (lazy's edit: each entry replaced by another of the same list, so no list changes length);
//!   - adds the prefab to the ResourceManager's container in globalgamemanagers as "pmu-lazy/<name, lower case>",
//!     so `Resources.Load` finds it (its file must be one of globalgamemanagers' externals).
//! An asmpatch patch loads it by that name before the game's code reads the cleared field (Blasphemous:
//! blasphemous-crisanta-lazy). Without that patch the game would find the field null: a port names a prefab here
//! only together with its patch. The step checks every prefab before it changes anything: one object of that name
//! (a GameObject), the references to it only from the named script's MonoBehaviours, its file in globalgamemanagers'
//! externals; a prefab that fails a check is left as it is, with the reason in the log.
//!
//!   objects  "<file>:<GameObject name>@<script class>|..." e.g.
//!            "sharedassets1061.assets:ACT_CrisantaExecution@CrisantaBehaviour"
use super::{edits, lazy, objects, opt, section};
use crate::error::Result;
use crate::job::Job;
use crate::pyjson::Json;
use std::collections::{BTreeMap, BTreeSet, VecDeque};
use unityfs::Value;

pub const STEP: &str = "lazyref";
pub const PREFIX: &str = "pmu-lazy/";

/// One prefab of the settings.
#[derive(Clone, Debug)]
struct Want {
    file: String,
    name: String,
    script: String,
}

fn parse(s: &str) -> Vec<Want> {
    s.split('|')
        .map(str::trim)
        .filter(|x| !x.is_empty())
        .filter_map(|x| {
            let (file, rest) = x.split_once(':')?;
            let (name, script) = rest.rsplit_once('@')?;
            Some(Want { file: file.into(), name: name.into(), script: script.into() })
        })
        .collect()
}

fn base(rel: &str) -> String {
    rel.rsplit('/').next().unwrap_or(rel).to_string()
}

/// The PPtrs (file id, path id) anywhere in a value.
fn pptrs(v: &Value, out: &mut Vec<(i128, i64)>) {
    match v {
        Value::Struct(s) => {
            let (f, p) = (s.get("m_FileID"), s.get("m_PathID"));
            if let (Some(f), Some(p)) = (f.and_then(Value::as_int), p.and_then(Value::as_int)) {
                if s.fields.len() == 2 {
                    out.push((f, p as i64));
                    return;
                }
            }
            for (_, x) in &s.fields {
                pptrs(x, out);
            }
        }
        Value::Array(a) => a.iter().for_each(|x| pptrs(x, out)),
        Value::Pair(p) => {
            pptrs(&p.0, out);
            pptrs(&p.1, out);
        }
        _ => {}
    }
}

/// The 4-aligned offsets of a 12-byte reference (file id, path id) in an object's bytes.
fn ref_offsets(b: &[u8], fid: i32, pid: i64) -> Vec<usize> {
    let mut pat = [0u8; 12];
    pat[..4].copy_from_slice(&fid.to_le_bytes());
    pat[4..].copy_from_slice(&pid.to_le_bytes());
    (0..b.len().saturating_sub(11)).step_by(4).filter(|&i| b[i..i + 12] == pat).collect()
}

/// What a MonoBehaviour's raw bytes reach in its own file (no type tree for scripts in a player build): every
/// 4-aligned (0, path id) of an object of the file.
fn raw_local(b: &[u8], ids: &BTreeSet<i64>) -> Vec<i64> {
    (0..b.len().saturating_sub(11))
        .step_by(4)
        .filter(|&i| b[i..i + 4] == [0, 0, 0, 0])
        .map(|i| i64::from_le_bytes(b[i + 4..i + 12].try_into().unwrap()))
        .filter(|p| *p != 0 && ids.contains(p))
        .collect()
}

/// The objects of a file that `root` reaches through references within the file (itself included).
fn closure(f: &unityfs::DataFile, root: i64) -> BTreeSet<i64> {
    let ids: BTreeSet<i64> = f.sf.objects.iter().map(|o| o.path_id).collect();
    let mut seen = BTreeSet::from([root]);
    let mut q = VecDeque::from([root]);
    while let Some(x) = q.pop_front() {
        let next: Vec<i64> = match f.sf.read_typetree(x) {
            Ok(t) => {
                let mut v = Vec::new();
                pptrs(&t, &mut v);
                v.into_iter().filter(|(fid, p)| *fid == 0 && ids.contains(p)).map(|(_, p)| p).collect()
            }
            Err(_) => f.sf.bytes(x).map(|b| raw_local(&b, &ids)).unwrap_or_default(),
        };
        for p in next {
            if seen.insert(p) {
                q.push_back(p);
            }
        }
    }
    seen
}

/// A MonoBehaviour's script reference: m_GameObject (12 bytes), m_Enabled (aligned to 4), then m_Script.
fn script_ref(b: &[u8]) -> Option<(i32, i64)> {
    let s = b.get(16..28)?;
    Some((i32::from_le_bytes(s[..4].try_into().ok()?), i64::from_le_bytes(s[4..].try_into().ok()?)))
}

pub fn run(job: &mut Job) -> Result<()> {
    let c = section(&job.cfg, STEP, &[("objects", Json::Str(String::new()))])?;
    let wants = parse(opt(&c, "objects").as_str().unwrap_or(""));
    if wants.is_empty() {
        job.log(&format!("{STEP}: no objects (convert.{STEP}.objects): nothing to do"));
        return Ok(());
    }
    let files: Vec<String> = edits::data_files(&job.src)
        .into_iter()
        .filter(|rel| edits::unity_file(&job.path_in(rel)).unwrap_or(false))
        .collect();
    let by_base: BTreeMap<String, String> = files.iter().map(|r| (base(r), r.clone())).collect();
    let Some(ggm) = by_base.get("globalgamemanagers").cloned() else {
        job.log(&format!("{STEP}: no globalgamemanagers: nothing done"));
        return Ok(());
    };
    // every file's externals (by base name), read once
    let mut exts: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for rel in &files {
        if let Ok(f) = job.open(&job.path_in(rel), Default::default()) {
            exts.insert(rel.clone(), f.sf.externals.iter().map(|e| base(&String::from_utf8_lossy(&e.path))).collect());
        }
    }
    let mut class_names: BTreeMap<(String, i64), Option<Vec<u8>>> = BTreeMap::new();
    let mut class_of = |job: &Job, file: &str, pid: i64| -> Option<Vec<u8>> {
        class_names
            .entry((file.to_string(), pid))
            .or_insert_with(|| {
                let rel = by_base.get(file)?;
                let f = job.open(&job.path_in(rel), Default::default()).ok()?;
                let t = f.sf.read_typetree(pid).ok()?;
                t.get("m_ClassName")?.as_bytes().map(<[u8]>::to_vec)
            })
            .clone()
    };

    // the plan: per file, the edits; checked for every prefab before anything is written
    let mut nulls: BTreeMap<String, Vec<(i64, usize)>> = BTreeMap::new(); // file -> (object, offset)
    let mut drop: BTreeSet<lazy::Key> = BTreeSet::new();
    let mut resources: Vec<(String, i32, i64)> = Vec::new(); // (name, file id in ggm, path id)
    for w in &wants {
        let Some(rel) = by_base.get(&w.file) else {
            job.log(&format!("{STEP}: {}: no such file: left as it is", w.file));
            continue;
        };
        let f = job.open(&job.path_in(rel), Default::default())?;
        let hits: Vec<i64> = f
            .sf
            .objects
            .iter()
            .filter(|o| o.class_id == 1)
            .filter(|o| {
                f.sf.read_typetree(o.path_id)
                    .ok()
                    .and_then(|t| t.get("m_Name").and_then(Value::as_bytes).map(|n| n == w.name.as_bytes()))
                    .unwrap_or(false)
            })
            .map(|o| o.path_id)
            .collect();
        let [root] = hits[..] else {
            job.log(&format!("{STEP}: {}:{}: {} GameObjects of that name, not one: left as it is", w.file, w.name, hits.len()));
            continue;
        };
        let reach = closure(&f, root);
        let ggm_fid = match exts.get(&ggm).and_then(|e| e.iter().position(|x| *x == w.file)) {
            Some(i) => i as i32 + 1,
            None => {
                job.log(&format!("{STEP}: {}:{}: its file is not one of globalgamemanagers' externals: left as it is", w.file, w.name));
                continue;
            }
        };
        std::mem::drop(f);
        // the references to it from outside its closure
        let (mut mine, mut bad) = (Vec::new(), Vec::new());
        for rel2 in &files {
            let fid = if base(rel2) == w.file {
                0
            } else {
                match exts.get(rel2).and_then(|e| e.iter().position(|x| *x == w.file)) {
                    Some(i) => i as i32 + 1,
                    None => continue,
                }
            };
            let Ok(g) = job.open(&job.path_in(rel2), Default::default()) else { continue };
            for o in g.sf.objects.iter() {
                if fid == 0 && reach.contains(&o.path_id) {
                    continue;
                }
                let Ok(b) = g.sf.bytes(o.path_id) else { continue };
                let offs = ref_offsets(&b, fid, root);
                // the engine's lists of assets: PreloadData and the ResourceManager's dependencies (lazy's edit), and
                // the NetworkManager's prefabs by asset id (its m_AssetToPrefab names every prefab of the build; it
                // loads none of them until a network spawn asks for one)
                if offs.is_empty() || [147, 149, 150].contains(&o.class_id) {
                    continue;
                }
                let ok = o.class_id == 114
                    && script_ref(&b).and_then(|(sf, sp)| {
                        let sfile = if sf == 0 { base(rel2) } else { exts.get(rel2)?.get(sf as usize - 1)?.clone() };
                        class_of(job, &sfile, sp)
                    }) == Some(w.script.as_bytes().to_vec());
                for off in offs {
                    if ok { mine.push((rel2.clone(), o.path_id, off)) } else { bad.push(format!("{} {} ({})", base(rel2), o.path_id, g.class_name(o))) }
                }
            }
        }
        if !bad.is_empty() || mine.is_empty() {
            job.log(&format!(
                "{STEP}: {}:{}: {} reference(s) from {}, {}: left as it is",
                w.file, w.name, mine.len(), w.script,
                if bad.is_empty() { "none to clear".to_string() } else { format!("others from {}", bad.join(", ")) }
            ));
            continue;
        }
        job.log(&format!(
            "{STEP}: {}:{}: {} reference(s) from {} cleared, {} object(s) left out of the preload lists, Resources \"{PREFIX}{}\"",
            w.file, w.name, mine.len(), w.script, reach.len(), w.name.to_lowercase()
        ));
        for (r, pid, off) in mine {
            nulls.entry(r).or_default().push((pid, off));
        }
        drop.extend(reach.iter().map(|p| (w.file.clone(), *p)));
        resources.push((format!("{PREFIX}{}", w.name.to_lowercase()), ggm_fid, root));
    }
    if resources.is_empty() {
        return Ok(());
    }

    // the writes: every file with a cleared reference, a PreloadData or the ResourceManager
    let holders: BTreeSet<String> = drop.iter().map(|k| k.0.clone()).collect();
    let mut nfiles = 0;
    for rel in &files {
        if job.is_done(STEP, rel) {
            continue;
        }
        let can = exts.get(rel).is_some_and(|e| e.iter().any(|x| holders.contains(x))) || holders.contains(&base(rel));
        if !(nulls.contains_key(rel) || can || *rel == ggm) {
            continue;
        }
        let Ok(mut f) = job.open(&job.path_in(rel), Default::default()) else { continue };
        let mut changed = false;
        for (pid, off) in nulls.get(rel).cloned().unwrap_or_default() {
            let mut b = f.sf.bytes(pid)?;
            b[off..off + 12].fill(0);
            f.sf.set_raw(pid, b)?;
            changed = true;
        }
        if can {
            changed |= lazy::edit(&mut f, rel, &drop)? > 0;
        }
        if *rel == ggm {
            for (pid, class) in objects(&f) {
                if class != "ResourceManager" {
                    continue;
                }
                let mut t = f.sf.read_typetree(pid)?;
                let Some(list) = t.get_mut("m_Container").and_then(Value::as_array_mut) else { continue };
                let Some(like) = list.first().cloned() else { continue };
                for (name, fid, root) in &resources {
                    if list.iter().any(|p| matches!(p, Value::Pair(b) if b.0.as_bytes() == Some(name.as_bytes()))) {
                        continue;
                    }
                    let mut e = like.clone();
                    if let Value::Pair(b) = &mut e {
                        b.0 = Value::Str(name.as_bytes().to_vec());
                        if let Value::Struct(s) = &mut b.1 {
                            for (n, v) in s.fields.iter_mut() {
                                match &**n {
                                    "m_FileID" => *v = Value::I32(*fid),
                                    "m_PathID" => *v = Value::I64(*root),
                                    _ => {}
                                }
                            }
                        }
                    }
                    let at = list.partition_point(|p| match p {
                        Value::Pair(b) => b.0.as_bytes().unwrap_or(b"") < name.as_bytes(),
                        _ => true,
                    });
                    list.insert(at, e);
                    changed = true;
                }
                f.sf.save_typetree(pid, &t)?;
            }
        }
        if changed {
            job.write_with(rel, |tmp| Ok(f.write_to(tmp, false)?))?;
            std::mem::drop(f);
            job.mark_done(STEP, rel)?;
            nfiles += 1;
        } else {
            std::mem::drop(f);
            job.mark_unchanged(STEP, rel)?;
        }
    }
    job.log(&format!("{STEP}: {} prefab(s) loaded by name, {nfiles} file(s) written", resources.len()));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn settings_and_offsets() {
        let w = parse("sharedassets1061.assets:ACT_CrisantaExecution@CrisantaBehaviour| a:b@c ");
        assert_eq!(w.len(), 2);
        assert_eq!((w[0].file.as_str(), w[0].name.as_str(), w[0].script.as_str()),
                   ("sharedassets1061.assets", "ACT_CrisantaExecution", "CrisantaBehaviour"));
        let mut b = vec![0u8; 40];
        b[20..24].copy_from_slice(&4i32.to_le_bytes());
        b[24..32].copy_from_slice(&916i64.to_le_bytes());
        assert_eq!(ref_offsets(&b, 4, 916), vec![20]);
        assert!(ref_offsets(&b, 4, 926).is_empty());
        let ids = BTreeSet::from([916i64]);
        let mut c = vec![0u8; 24];
        c[4..12].copy_from_slice(&916i64.to_le_bytes());
        assert_eq!(raw_local(&c, &ids), vec![916]);
    }
}
