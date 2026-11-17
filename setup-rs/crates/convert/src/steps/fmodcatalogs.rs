//! Step `fmodcatalogs` (pmu's own, for Blasphemous and asmpatch's `blasphemous-catalogs`): what
//! `Resources.LoadAll<Framework.Audio.FMODAudioCatalog>("FMODCatalogs/Enemies")` returns, in its order, and
//! the class of each catalog's `Owner`, written to `fmodcatalogs.txt` in the data folder (with
//! `android_data.data_path`: `Game_Data/` beside the APK).
//!
//! The game loads every enemy's sound catalog at startup, and each catalog's `Owner` field references an
//! enemy or boss component: the load pulls every enemy with all its sprite sheets into memory (more than a
//! 1 GB handheld has), only so that each level's `AudioLoader` can pick the catalogs whose owner has the type
//! of one of the level's enemies. With this list the patch loads, per level, only the catalogs whose owner
//! class is one of the level's enemy types, in the same order, and the game's own loop picks from those
//! (docs/compat/games.md, Blasphemous).
//!
//! One line per catalog, in the order of the ResourceManager's container (sorted by path, Unity's map), after
//! the header `# pmu fmodcatalogs 1`: `<container path> TAB <owner's namespace.class, or - for none>`. The
//! data is read as the game's build has it (no type trees): a catalog's fields from its bytes, in the order
//! FMODAudioCatalog declares them (m_GameObject, m_Enabled, m_Script, m_Name, AssociatedCatalog,
//! HasAssociatedCatalog, Owner), which asmpatch's patch checks in the game's assembly before it uses the
//! list. Anything the step cannot tell for sure (a key that matches the path only partly, a container out of
//! order, a prefab with two catalogs or one in a child, an owner without its script) writes no list: the
//! patched game then loads them all, as the original does.
use super::objects;
use crate::error::Result;
use crate::job::Job;
use std::collections::{BTreeMap, BTreeSet};
use unityfs::{DataFile, Value};

pub const STEP: &str = "fmodcatalogs";
pub const OUT: &str = "fmodcatalogs.txt";
pub const HEADER: &str = "# pmu fmodcatalogs 1";
/// Resources.LoadAll's path as the game passes it, lowercased as the container's keys are.
pub const PREFIX: &str = "fmodcatalogs/enemies";
const CATALOG: (&str, &str) = ("Framework.Audio", "FMODAudioCatalog");

/// A catalog's fields, from its bytes.
#[derive(Debug, PartialEq)]
pub struct Catalog {
    pub game_object: (i32, i64),
    pub script: (i32, i64),
    pub associated: (i32, i64),
    pub has_associated: bool,
    pub owner: (i32, i64),
}

fn pptr_at(b: &[u8], p: usize) -> Option<(i32, i64)> {
    let fid = i32::from_le_bytes(b.get(p..p + 4)?.try_into().ok()?);
    let pid = i64::from_le_bytes(b.get(p + 4..p + 12)?.try_into().ok()?);
    Some((fid, pid))
}

/// A bool with its 3 bytes of alignment (all zero), as Unity writes it.
fn bool_at(b: &[u8], p: usize) -> Option<bool> {
    let x = b.get(p..p + 4)?;
    (x[0] <= 1 && x[1..] == [0, 0, 0]).then_some(x[0] == 1)
}

/// FMODAudioCatalog's first fields from a MonoBehaviour's bytes (PPtrs of 4 + 8 bytes: format 14 and later).
pub fn parse_catalog(b: &[u8]) -> Option<Catalog> {
    let game_object = pptr_at(b, 0)?;
    bool_at(b, 12)?;
    let script = pptr_at(b, 16)?;
    let n = i32::from_le_bytes(b.get(28..32)?.try_into().ok()?);
    let n = usize::try_from(n).ok()?;
    let mut p = 32 + n;
    b.get(32..p)?;
    p = (p + 3) & !3;
    let associated = pptr_at(b, p)?;
    let has_associated = bool_at(b, p + 12)?;
    let owner = pptr_at(b, p + 16)?;
    Some(Catalog {
        game_object,
        script,
        associated,
        has_associated,
        owner,
    })
}

/// The container's entries LoadAll(PREFIX) returns, by index, or why that cannot be told: the keys must be in
/// Unity's (byte) order, and every key starting with the prefix must be the prefix or under it ("x/...").
pub fn range(keys: &[Vec<u8>]) -> std::result::Result<Vec<usize>, String> {
    if keys.windows(2).any(|w| w[0] > w[1]) {
        return Err("the ResourceManager's container is not in its keys' order".into());
    }
    let pre = PREFIX.as_bytes();
    let mut out = Vec::new();
    for (i, k) in keys.iter().enumerate() {
        if !k.starts_with(pre) {
            continue;
        }
        if k.len() > pre.len() && k[pre.len()] != b'/' {
            return Err(format!(
                "{:?} starts with {PREFIX:?} without being under it",
                String::from_utf8_lossy(k)
            ));
        }
        out.push(i);
    }
    Ok(out)
}

/// The data files the step opens, by name (each opened once).
struct Files<'a> {
    job: &'a Job,
    open: BTreeMap<String, Option<DataFile>>,
}

impl<'a> Files<'a> {
    fn get(&mut self, name: &str) -> Option<&DataFile> {
        if !self.open.contains_key(name) {
            let p = self.job.path_in(name);
            let f = if p.is_file() {
                match self.job.open(&p, Default::default()) {
                    Ok(f) => Some(f),
                    Err(e) => {
                        self.job.log(&format!("{STEP}: {name}: {e}"));
                        None
                    }
                }
            } else {
                None
            };
            self.open.insert(name.to_string(), f);
        }
        self.open.get(name).and_then(Option::as_ref)
    }
}

/// The name of the file a PPtr's file id names, as seen from `from` (0: `from` itself).
fn file_name(f: &DataFile, from: &str, fid: i32) -> Option<String> {
    if fid == 0 {
        return Some(from.to_string());
    }
    let e = f.sf.externals.get(usize::try_from(fid - 1).ok()?)?;
    let p = String::from_utf8_lossy(&e.path).into_owned();
    Some(p.rsplit('/').next().unwrap_or(&p).to_string())
}

fn int_of(v: &Value, name: &str) -> Option<i64> {
    v.get(name)?.as_int().and_then(|x| i64::try_from(x).ok())
}

fn pptr_of(v: &Value) -> Option<(i32, i64)> {
    Some((
        i32::try_from(int_of(v, "m_FileID")?).ok()?,
        int_of(v, "m_PathID")?,
    ))
}

/// A MonoScript's (namespace, class), from the file a MonoBehaviour's m_Script names.
fn script_of(files: &mut Files, file: &str, script: (i32, i64)) -> Option<(String, String)> {
    let f = files.get(file)?;
    let name = file_name(f, file, script.0)?;
    let s = files.get(&name)?;
    let o = s.sf.object(script.1)?;
    if o.class_id != 115 {
        return None;
    }
    let t = s.sf.read_typetree(script.1).ok()?;
    let text = |n: &str| {
        t.get(n)
            .and_then(Value::as_bytes)
            .map(|b| String::from_utf8_lossy(b).into_owned())
    };
    Some((text("m_Namespace")?, text("m_ClassName")?))
}

fn full_name((ns, class): &(String, String)) -> String {
    if ns.is_empty() {
        class.clone()
    } else {
        format!("{ns}.{class}")
    }
}

/// A GameObject's components (PPtrs), from its tree.
fn components(f: &DataFile, go: i64) -> Option<Vec<(i32, i64)>> {
    let t = f.sf.read_typetree(go).ok()?;
    let list = t.get("m_Component")?.as_array()?;
    list.iter()
        .map(|c| match c {
            Value::Pair(p) => pptr_of(&p.1),
            _ => pptr_of(c.get("component")?),
        })
        .collect()
}

/// The root GameObject of a GameObject's hierarchy (its Transform's m_Father chain), in the same file.
fn root_of(f: &DataFile, go: i64) -> Option<i64> {
    let tr = components(f, go)?.into_iter().find(|&(fid, pid)| {
        fid == 0
            && f.sf
                .object(pid)
                .is_some_and(|o| o.class_id == 4 || o.class_id == 224)
    })?;
    let mut t = tr.1;
    for _ in 0..256 {
        let v = f.sf.read_typetree(t).ok()?;
        let father = pptr_of(v.get("m_Father")?)?;
        if father.1 == 0 {
            return pptr_of(v.get("m_GameObject")?)
                .filter(|g| g.0 == 0)
                .map(|g| g.1);
        }
        if father.0 != 0 {
            return None;
        }
        t = father.1;
    }
    None
}

/// The lines (path, owner) or why there are none.
fn survey(job: &Job) -> std::result::Result<Vec<(String, String)>, String> {
    let mut files = Files {
        job,
        open: BTreeMap::new(),
    };
    let ggm = "globalgamemanagers";
    let g = files.get(ggm).ok_or("no globalgamemanagers")?;
    let rm = objects(g)
        .into_iter()
        .find(|(_, c)| c == "ResourceManager")
        .ok_or("no ResourceManager")?;
    let t =
        g.sf.read_typetree(rm.0)
            .map_err(|e| format!("ResourceManager: {e}"))?;
    let entries = t
        .get("m_Container")
        .and_then(Value::as_array)
        .ok_or("no m_Container")?;
    let mut keys = Vec::new();
    let mut ptrs = Vec::new();
    for e in entries {
        let Value::Pair(p) = e else {
            return Err("m_Container: not pairs".into());
        };
        keys.push(
            p.0.as_bytes()
                .ok_or("m_Container: a key is not a string")?
                .to_vec(),
        );
        ptrs.push(pptr_of(&p.1).ok_or("m_Container: not a PPtr")?);
    }
    let sel = range(&keys)?;
    if sel.is_empty() {
        return Err(format!("no resources under {PREFIX:?}"));
    }
    // each entry: a prefab's root GameObject (or a catalog itself) in a data file
    let mut lines = Vec::new();
    let mut roots: BTreeMap<String, BTreeSet<i64>> = BTreeMap::new();
    let mut taken: BTreeSet<(String, i64)> = BTreeSet::new();
    for &i in &sel {
        let key = String::from_utf8(keys[i].clone()).map_err(|_| "a key is not UTF-8")?;
        if key.contains(['\t', '\n', '\r']) {
            return Err(format!("{key:?}: a tab or line break in the path"));
        }
        let g = files.get(ggm).unwrap();
        let file =
            file_name(g, ggm, ptrs[i].0).ok_or_else(|| format!("{key}: an unknown file id"))?;
        let pid = ptrs[i].1;
        let f = files
            .get(&file)
            .ok_or_else(|| format!("{key}: {file} is not in the data"))?;
        let class =
            f.sf.object(pid)
                .map(|o| o.class_id)
                .ok_or_else(|| format!("{key}: no object {pid} in {file}"))?;
        let comps = match class {
            1 => {
                roots.entry(file.clone()).or_default().insert(pid);
                components(f, pid).ok_or_else(|| format!("{key}: the GameObject's components"))?
            }
            114 => vec![(0, pid)],
            _ => continue, // (not a catalog: LoadAll<FMODAudioCatalog> leaves it out)
        };
        let mut found = Vec::new();
        for (fid, c) in comps {
            let f = files.get(&file).unwrap();
            if fid != 0 || f.sf.object(c).map(|o| o.class_id) != Some(114) {
                continue;
            }
            let b = f.sf.bytes(c).map_err(|e| format!("{key}: {e}"))?;
            let Some(script) = pptr_at(&b, 16) else {
                continue;
            };
            let s = script_of(&mut files, &file, script)
                .ok_or_else(|| format!("{key}: a component without its script"))?;
            if (s.0.as_str(), s.1.as_str()) == CATALOG {
                found.push((c, b));
            }
        }
        if found.len() > 1 {
            return Err(format!("{key}: {} catalogs on one GameObject", found.len()));
        }
        let Some((c, b)) = found.pop() else { continue };
        let cat = parse_catalog(&b)
            .ok_or_else(|| format!("{key}: the catalog's fields are not as FMODAudioCatalog's"))?;
        if class == 1 && cat.game_object != (0, pid) {
            return Err(format!(
                "{key}: the catalog's m_GameObject is not its GameObject"
            ));
        }
        taken.insert((file.clone(), c));
        let owner = if cat.owner.1 == 0 {
            "-".to_string()
        } else {
            let f = files.get(&file).unwrap();
            let of = file_name(f, &file, cat.owner.0)
                .ok_or_else(|| format!("{key}: the owner's file id"))?;
            let o = files
                .get(&of)
                .ok_or_else(|| format!("{key}: the owner's file {of} is not in the data"))?;
            match o.sf.object(cat.owner.1).map(|x| x.class_id) {
                None => "-".to_string(), // (a reference to nothing: Owner == null in the game)
                Some(114) => {
                    let ob = o.sf.bytes(cat.owner.1).map_err(|e| format!("{key}: {e}"))?;
                    let script =
                        pptr_at(&ob, 16).ok_or_else(|| format!("{key}: the owner's bytes"))?;
                    let s = script_of(&mut files, &of, script)
                        .ok_or_else(|| format!("{key}: the owner without its script"))?;
                    let n = full_name(&s);
                    if n.is_empty() || n == "-" || n.contains(['\t', '\n', '\r']) {
                        return Err(format!("{key}: the owner's class {n:?}"));
                    }
                    n
                }
                Some(k) => {
                    return Err(format!(
                        "{key}: the owner is a class {k} object, not a component"
                    ))
                }
            }
        };
        lines.push((key, owner));
    }
    // a catalog anywhere else in a prefab of the range (a child's): which of them LoadAll returns is not told here
    for (file, rs) in &roots {
        let f = files.get(file).unwrap();
        let mbs: Vec<i64> = f.sf.objects_of(114).map(|o| o.path_id).collect();
        for c in mbs {
            if taken.contains(&(file.clone(), c)) {
                continue;
            }
            let f = files.get(file).unwrap();
            let b = f.sf.bytes(c).map_err(|e| e.to_string())?;
            let Some(script) = pptr_at(&b, 16) else {
                continue;
            };
            let Some((go_fid, go)) = pptr_at(&b, 0) else {
                continue;
            };
            if go_fid != 0 || go == 0 {
                continue;
            }
            match script_of(&mut files, file, script) {
                Some(s) if (s.0.as_str(), s.1.as_str()) == CATALOG => {}
                _ => continue,
            }
            let f = files.get(file).unwrap();
            match root_of(f, go) {
                Some(r) if !rs.contains(&r) => {}
                _ => return Err(format!(
                    "{file}: a catalog ({c}) in a prefab under {PREFIX:?} that is not its root's"
                )),
            }
        }
    }
    if lines.is_empty() {
        return Err(format!("no catalogs under {PREFIX:?}"));
    }
    Ok(lines)
}

pub fn run(job: &mut Job) -> Result<()> {
    if job.is_done(STEP, OUT) {
        return Ok(());
    }
    match survey(job) {
        Ok(lines) => {
            let mut text = String::from(HEADER);
            for (k, o) in &lines {
                text.push('\n');
                text.push_str(k);
                text.push('\t');
                text.push_str(o);
            }
            text.push('\n');
            job.write(OUT, text.as_bytes())?;
            let owned = lines.iter().filter(|(_, o)| o != "-").count();
            let classes: BTreeSet<&str> = lines
                .iter()
                .map(|(_, o)| o.as_str())
                .filter(|o| *o != "-")
                .collect();
            job.log(&format!(
                "{STEP}: {} catalogs under {PREFIX:?} ({owned} with an owner, {} owner classes) -> {OUT}",
                lines.len(),
                classes.len()
            ));
        }
        // (no list: the patched game loads them all at startup, as the original)
        Err(why) => job.log(&format!("{STEP}: no {OUT}: {why}")),
    }
    job.mark_done(STEP, OUT)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pp(b: &mut Vec<u8>, fid: i32, pid: i64) {
        b.extend(fid.to_le_bytes());
        b.extend(pid.to_le_bytes());
    }

    #[test]
    fn catalog_fields() {
        let mut b = Vec::new();
        pp(&mut b, 0, 7);
        b.extend([1, 0, 0, 0]);
        pp(&mut b, 1, 99);
        b.extend(5i32.to_le_bytes());
        b.extend(b"abcde\0\0\0");
        pp(&mut b, 0, 8);
        b.extend([1, 0, 0, 0]);
        pp(&mut b, 0, 1234);
        b.extend([0; 4]); // (SoundEffects: not read)
        let c = parse_catalog(&b).unwrap();
        assert_eq!(
            c,
            Catalog {
                game_object: (0, 7),
                script: (1, 99),
                associated: (0, 8),
                has_associated: true,
                owner: (0, 1234)
            }
        );
        // a bool that is not one, or too few bytes: none
        let mut bad = b.clone();
        bad[52] = 2; // (HasAssociatedCatalog, after the name's 5 bytes aligned and AssociatedCatalog)
        assert!(parse_catalog(&bad).is_none());
        assert!(parse_catalog(&b[..50]).is_none());
    }

    #[test]
    fn container_range() {
        let k = |v: &[&str]| v.iter().map(|s| s.as_bytes().to_vec()).collect::<Vec<_>>();
        assert_eq!(
            range(&k(&[
                "a",
                "fmodcatalogs/enemies/a",
                "fmodcatalogs/enemies/b",
                "fmodcatalogs/penitent/p"
            ])),
            Ok(vec![1, 2])
        );
        assert!(range(&k(&["fmodcatalogs/enemies/b", "fmodcatalogs/enemies/a"])).is_err());
        assert!(range(&k(&["fmodcatalogs/enemies/a", "fmodcatalogs/enemiesx"])).is_err());
        assert_eq!(
            range(&k(&["fmodcatalogs/enemies", "fmodcatalogs/enemies/a"])),
            Ok(vec![0, 1])
        );
    }
}
