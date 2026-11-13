//! Step `lazy` (pmu's own): assets that every scene lists for loading although the game shows them only in
//! some cases are left out of those lists. Each scene's PreloadData (and the ResourceManager's dependency lists
//! in globalgamemanagers) names everything the scene's objects reach, so Unity loads all of it with every scene,
//! and loads again what was given back. Hollow Knight keeps every language's TextMeshPro atlases and the CJK font
//! files referenced from its text: about 87 MB, loaded in every language and with every room. Left out of the
//! lists, such an asset is loaded when something uses it (Unity resolves a reference to an object of a loaded
//! file when it is dereferenced), not with each scene; ua_host's UA_UNLOAD_ASSETS (the same names) gives back
//! what the first scenes loaded. A game that shows them (its language's atlas) loads them once, the picture the
//! same; one that never does keeps them out of memory (docs/porting.md, "Small handhelds"). Each entry for such
//! an asset is replaced by a copy of one its list's owner loads anyway (the scene's first other entry; for a
//! resource's dependencies the resource itself), so no list, object or file changes length.
//!
//!   names   "<m_Name>|<m_Name>..." the objects' exact names; Texture2D, Font and TextAsset objects are looked up
use super::{edits, int, objects, opt, section};
use crate::error::Result;
use crate::job::Job;
use crate::pyjson::Json;
use std::collections::BTreeSet;
use unityfs::Value;

pub const STEP: &str = "lazy";

/// The classes whose objects can be named: Texture2D, Font, TextAsset.
const NAMED: [i32; 3] = [28, 128, 49];

/// An object's m_Name (its first field) read from its first bytes, without reading the whole object.
fn name_of(f: &unityfs::DataFile, o: &unityfs::ObjectInfo) -> Option<Vec<u8>> {
    let head = f.sf.source().read_vec(o.byte_start, (o.byte_size as usize).min(4 + 255)).ok()?;
    if head.len() < 4 {
        return None;
    }
    let n = i32::from_le_bytes([head[0], head[1], head[2], head[3]]);
    (0 < n && 4 + n as usize <= head.len()).then(|| head[4..4 + n as usize].to_vec())
}

/// The external file a PPtr's file id names (its last path component; 0: the file itself).
fn file_of(ext: &[String], rel: &str, fid: i128) -> Option<String> {
    if fid == 0 {
        return Some(rel.rsplit('/').next().unwrap_or(rel).to_string());
    }
    ext.get(usize::try_from(fid - 1).ok()?).cloned()
}

pub type Key = (String, i64);

/// A list of PPtrs with those in `drop` replaced by `instead` (an entry the list's owner loads anyway: the
/// list's first other entry, or the resource itself), so every list keeps its length and no object or file
/// changes size (the same edit can be written into an installed APK's stored entries in place); without one,
/// they are taken out. The number left out.
fn filter(
    list: &mut Value,
    ext: &[String],
    rel: &str,
    drop: &BTreeSet<Key>,
    instead: Option<&Value>,
) -> Result<usize> {
    let Some(items) = list.as_array_mut() else {
        return Ok(0);
    };
    let mut hit = Vec::with_capacity(items.len());
    for p in items.iter() {
        let fid = int(p, "m_FileID")?;
        let pid = int(p, "m_PathID")? as i64;
        hit.push(file_of(ext, rel, fid).is_some_and(|f| drop.contains(&(f, pid))));
    }
    let n = hit.iter().filter(|h| **h).count();
    if n == 0 {
        return Ok(0);
    }
    let first_other = hit.iter().position(|h| !*h).map(|i| items[i].clone());
    match instead.cloned().or(first_other) {
        Some(other) => {
            for (p, h) in items.iter_mut().zip(&hit) {
                if *h {
                    *p = other.clone();
                }
            }
        }
        None => {
            let mut k = hit.iter();
            items.retain(|_| !*k.next().unwrap());
        }
    }
    Ok(n)
}

/// The names of a step's settings ("<name>|<name>...").
pub fn names_of(c: &Json) -> BTreeSet<Vec<u8>> {
    opt(c, "names")
        .as_str()
        .unwrap_or("")
        .split('|')
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(|s| s.as_bytes().to_vec())
        .collect()
}

/// The objects of a file named in `names`: (the file's name, path id), and the names found.
pub fn survey(f: &unityfs::DataFile, rel: &str, names: &BTreeSet<Vec<u8>>) -> (Vec<Key>, Vec<Vec<u8>>) {
    let base = rel.rsplit('/').next().unwrap_or(rel).to_string();
    let (mut keys, mut found) = (Vec::new(), Vec::new());
    for o in f.sf.objects.iter().filter(|o| NAMED.contains(&o.class_id)) {
        if let Some(n) = name_of(f, o) {
            if names.contains(&n) {
                keys.push((base.clone(), o.path_id));
                found.push(n);
            }
        }
    }
    (keys, found)
}

/// A file's PreloadData and ResourceManager lists without the objects in `drop` (saved in `f`, the same length):
/// the number of entries left out.
pub fn edit(f: &mut unityfs::DataFile, rel: &str, drop: &BTreeSet<Key>) -> Result<usize> {
    let ext: Vec<String> = f
        .sf
        .externals
        .iter()
        .map(|e| {
            let p = String::from_utf8_lossy(&e.path).into_owned();
            p.rsplit('/').next().unwrap_or("").to_string()
        })
        .collect();
    let mut n = 0;
    for (pid, class) in objects(f) {
        match class.as_str() {
            "PreloadData" => {
                let mut t = f.sf.read_typetree(pid)?;
                let k = match t.get_mut("m_Assets") {
                    Some(list) => filter(list, &ext, rel, drop, None)?,
                    None => 0,
                };
                if k > 0 {
                    f.sf.save_typetree(pid, &t)?;
                    n += k;
                }
            }
            "ResourceManager" => {
                let mut t = f.sf.read_typetree(pid)?;
                let mut k = 0;
                if let Some(deps) = t.get_mut("m_DependentAssets").and_then(Value::as_array_mut) {
                    for d in deps.iter_mut() {
                        let own = d.get("m_Object").cloned();
                        if let Some(list) = d.get_mut("m_Dependencies") {
                            k += filter(list, &ext, rel, drop, own.as_ref())?;
                        }
                    }
                }
                if k > 0 {
                    f.sf.save_typetree(pid, &t)?;
                    n += k;
                }
            }
            _ => {}
        }
    }
    Ok(n)
}

pub fn run(job: &mut Job) -> Result<()> {
    let c = section(&job.cfg, STEP, &[("names", Json::Str(String::new()))])?;
    let names = names_of(&c);
    if names.is_empty() {
        job.log(&format!("{STEP}: no names (convert.{STEP}.names): nothing to do"));
        return Ok(());
    }
    let files: Vec<String> = edits::data_files(&job.src)
        .into_iter()
        .filter(|rel| edits::unity_file(&job.path_in(rel)).unwrap_or(false))
        .collect();
    // the objects of those names, by (file, path id)
    let mut drop: BTreeSet<Key> = BTreeSet::new();
    let mut found: BTreeSet<Vec<u8>> = BTreeSet::new();
    for rel in &files {
        let f = match job.open(&job.path_in(rel), Default::default()) {
            Ok(f) => f,
            Err(_) => continue, // (a bundle or a file the step cannot read: no objects of ours)
        };
        let (keys, names_found) = survey(&f, rel, &names);
        drop.extend(keys);
        found.extend(names_found);
    }
    for n in names.difference(&found) {
        job.log(&format!("{STEP}: no object named {:?} in the data", String::from_utf8_lossy(n)));
    }
    if drop.is_empty() {
        return Ok(());
    }
    // every PreloadData and the ResourceManager's dependencies without them
    let (mut total, mut nfiles) = (0usize, 0usize);
    for rel in &files {
        if job.is_done(STEP, rel) {
            continue;
        }
        let mut f = match job.open(&job.path_in(rel), Default::default()) {
            Ok(f) => f,
            Err(_) => continue,
        };
        let n = edit(&mut f, rel, &drop)?;
        if n > 0 {
            job.write_with(rel, |tmp| Ok(f.write_to(tmp, false)?))?;
            nfiles += 1;
            total += n;
        }
        std::mem::drop(f);
        job.mark_done(STEP, rel)?;
    }
    job.log(&format!(
        "{STEP}: {} object(s) named ({} of {} names found), left out of {total} preload or dependency entries in {nfiles} file(s)",
        drop.len(),
        found.len(),
        names.len()
    ));
    Ok(())
}
