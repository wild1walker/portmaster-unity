//! Digests of what the tools wrote for a game (was tools/regress/digest.py): nothing of a game's
//! content is kept, only hashes.
//!   file      SHA-256 of the whole written file
//!   gles3     per file, SHA-256 over its Shaders' GLES3 platform blob entries (each program's code and
//!             keywords, in blob order) and its ComputeShaders' GLES3 variants (their kernels' code), in
//!             path-id order: what the shaders step adds
//!   total     SHA-256 over the files' (path, digest) pairs in path order
//! The digests are pmu's own (the Python's read the blobs through UnityPy): a baseline recorded by the
//! Python's regress.py differs in these values; record it again (`regress fast --update`).
use std::collections::{BTreeMap, BTreeSet};
use std::io::Read;
use std::path::Path;

use serde_json::{json, Value};
use sha2::{Digest, Sha256};

pub fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

pub fn sha256_file(p: &Path) -> std::io::Result<String> {
    let mut f = std::fs::File::open(p)?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(hex(&h.finalize()))
}

/// (digest, Shaders with GLES3 programs, ComputeShaders with a GLES3 variant) of one data file, or an
/// error (not a serialized file or bundle).
pub fn gles3_digest(path: &Path) -> Result<(String, usize, usize), String> {
    let db = std::sync::Arc::new(unityfs::ClassDb::default_db().map_err(|e| e.to_string())?);
    let o = unityfs::Options { all_serialized: true, db: Some(db), ..Default::default() };
    let mut f = unityfs::DataFile::open(path, &o).map_err(|e| e.to_string())?;
    let mut h = Sha256::new();
    let (mut ns, mut nc) = (0, 0);
    for k in 0..f.file_count() {
        f.select(k).map_err(|e| e.to_string())?;
        let mut objs = convert::steps::objects(&f);
        objs.sort();
        for (pid, kind) in objs {
            let d = match kind.as_str() {
                "Shader" => {
                    let t = f.sf.read_typetree(pid).map_err(|e| e.to_string())?;
                    match convert::steps::shaders::gles3_entries_digest(&t) {
                        Some(d) => {
                            ns += 1;
                            hex(&Sha256::digest(&d))
                        }
                        None => continue,
                    }
                }
                "ComputeShader" => {
                    let t = f.sf.read_typetree(pid).map_err(|e| e.to_string())?;
                    match convert::steps::shaders::gles3_kernels_digest(&t) {
                        Some(d) => {
                            nc += 1;
                            hex(&Sha256::digest(&d))
                        }
                        None => continue,
                    }
                }
                _ => continue,
            };
            h.update(format!("{pid} {kind} {d}\n").as_bytes());
        }
    }
    Ok((hex(&h.finalize()), ns, nc))
}

pub fn total(files: &BTreeMap<String, Value>) -> String {
    let mut h = Sha256::new();
    for (rel, f) in files {
        h.update(format!("{rel}\0{}\0{}\n", f["file"].as_str().unwrap_or(""), f["gles3"].as_str().unwrap_or("")).as_bytes());
    }
    hex(&h.finalize())
}

fn walk(root: &Path, rel: &str, out: &mut Vec<String>) {
    let mut names: Vec<(String, bool)> = std::fs::read_dir(root.join(rel))
        .map(|r| r.filter_map(|e| e.ok()).map(|e| (e.file_name().to_string_lossy().into_owned(), e.path().is_dir())).collect())
        .unwrap_or_default();
    names.sort();
    for (n, dir) in names {
        if n.starts_with('.') {
            continue;
        }
        let r = if rel.is_empty() { n.clone() } else { format!("{rel}/{n}") };
        if dir {
            walk(root, &r, out);
        } else {
            out.push(r);
        }
    }
}

/// `files`: the digests of files a `pmu convert` wrote into an overlay folder.
pub fn files(root: &Path, rels: &[String]) -> Value {
    let mut all = Vec::new();
    let rels: Vec<String> = if rels.is_empty() {
        walk(root, "", &mut all);
        all
    } else {
        rels.to_vec()
    };
    let mut out = BTreeMap::new();
    for rel in rels {
        let p = root.join(&rel);
        let mut rec = json!({"file": sha256_file(&p).unwrap_or_default(), "size": std::fs::metadata(&p).map_or(0, |m| m.len())});
        match gles3_digest(&p) {
            Ok((d, s, c)) => {
                rec["gles3"] = json!(d);
                rec["shaders"] = json!(s);
                rec["compute"] = json!(c);
            }
            Err(e) => {
                rec["gles3"] = json!("-");
                rec["shaders"] = json!(0);
                rec["compute"] = json!(0);
                rec["note"] = json!(e.split(':').next().unwrap_or("").trim());
            }
        }
        out.insert(rel, rec);
    }
    let t = total(&out);
    json!({"files": out, "total": t})
}

/// (digest, entries, bytes) of a zip's central directory: names, CRC-32s, sizes, compression, and
/// each member's header offset (alignment is part of what android-data's packer promises).
pub fn zip_members(p: &Path) -> Result<(String, usize, u64), String> {
    let a = pkgzip::zip::Archive::open(&p.to_string_lossy()).map_err(|e| e.to_string())?;
    let mut es: Vec<&pkgzip::zip::Entry> = a.entries.iter().collect();
    es.sort_by(|x, y| x.name.cmp(&y.name));
    let mut h = Sha256::new();
    let mut size = 0;
    for e in &es {
        h.update(format!("{}\0{:08x}\0{}\0{}\0{}\n", e.name, e.crc, e.file_size, e.method, e.header_offset).as_bytes());
        size += e.file_size;
    }
    Ok((hex(&h.finalize()), es.len(), size))
}

/// fnmatch.fnmatch: *, ?, [...] (with ! for negation), on the whole relative path.
pub fn fnmatch(name: &str, pat: &str) -> bool {
    fn m(n: &[char], p: &[char]) -> bool {
        match p.first() {
            None => n.is_empty(),
            Some('*') => (0..=n.len()).any(|i| m(&n[i..], &p[1..])),
            Some('?') => !n.is_empty() && m(&n[1..], &p[1..]),
            Some('[') => {
                let Some(end) = p.iter().skip(1).position(|c| *c == ']').map(|i| i + 1) else {
                    return n.first() == Some(&'[') && m(&n[1..], &p[1..]);
                };
                let Some(c) = n.first() else { return false };
                let set = &p[1..end];
                let (neg, set) = if set.first() == Some(&'!') { (true, &set[1..]) } else { (false, set) };
                let mut hit = false;
                let mut i = 0;
                while i < set.len() {
                    if i + 2 < set.len() && set[i + 1] == '-' {
                        hit |= set[i] <= *c && *c <= set[i + 2];
                        i += 3;
                    } else {
                        hit |= set[i] == *c;
                        i += 1;
                    }
                }
                hit != neg && m(&n[1..], &p[end + 1..])
            }
            Some(x) => n.first() == Some(x) && m(&n[1..], &p[1..]),
        }
    }
    m(&name.chars().collect::<Vec<_>>(), &pat.chars().collect::<Vec<_>>())
}

/// `tree`: every file under root but dot files and the globs: size and SHA-256 (android-data's tree),
/// an APK's or OBB's zip members. state: android-data's .android_data_state.json; the files it placed
/// unchanged ('link') count by their size only.
pub fn tree(root: &Path, skip: &[String], state: Option<&Path>) -> Value {
    let unchanged: BTreeSet<String> = state
        .and_then(|s| std::fs::read(s).ok())
        .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
        .and_then(|v| {
            v.as_object().map(|o| {
                o.iter()
                    .filter(|(_, v)| v.as_array().and_then(|a| a.last()).and_then(Value::as_str) == Some("link"))
                    .map(|(k, _)| k.clone())
                    .collect()
            })
        })
        .unwrap_or_default();
    let mut rels = Vec::new();
    walk(root, "", &mut rels);
    let (mut files, mut zips) = (BTreeMap::new(), BTreeMap::new());
    let mut h = Sha256::new();
    let mut nbytes = 0u64;
    for rel in rels {
        let p = root.join(&rel);
        if skip.iter().any(|g| fnmatch(&rel, g)) || (p.is_symlink() && !p.exists()) {
            continue;
        }
        let s = std::fs::metadata(&p).map_or(0, |m| m.len());
        let d = if rel.ends_with(".apk") || rel.ends_with(".obb") {
            let z = match zip_members(&p) {
                Ok((zd, ze, zb)) => json!({"members": zd, "entries": ze, "bytes": zb}),
                Err(_) => json!({"members": "bad zip", "entries": 0, "bytes": 0}),
            };
            let d = z["members"].as_str().unwrap_or("").to_string(); // (the members' CRCs stand for the zip's contents)
            zips.insert(rel.clone(), z);
            d
        } else if unchanged.contains(&rel) {
            "link".to_string()
        } else {
            sha256_file(&p).unwrap_or_default()
        };
        h.update(format!("{rel}\0{s}\0{d}\n").as_bytes());
        files.insert(rel, json!([s, d]));
        nbytes += s;
    }
    let count = files.len();
    json!({"files": files, "zips": zips, "total": hex(&h.finalize()), "count": count, "bytes": nbytes})
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn globs() {
        assert!(fnmatch("a/b.log", "*.log"));
        assert!(fnmatch("make_android_data.log", "make_android_data.log"));
        assert!(!fnmatch("x.apk", "*.log"));
        assert!(fnmatch("lib7", "lib[0-9]"));
    }

    #[test]
    fn tree_digest() {
        let d = std::env::temp_dir().join(format!("pmu-dev-tree-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join("sub")).unwrap();
        std::fs::write(d.join("a.txt"), "x").unwrap();
        std::fs::write(d.join("sub/b.log"), "y").unwrap();
        std::fs::write(d.join(".hidden"), "z").unwrap();
        let t = tree(&d, &["*.log".to_string()], None);
        assert_eq!(t["count"], 1);
        assert_eq!(t["files"]["a.txt"][0], 1);
        let _ = std::fs::remove_dir_all(&d);
    }
}
