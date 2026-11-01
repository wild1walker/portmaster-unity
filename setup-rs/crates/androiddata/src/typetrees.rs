//! Type trees UnityPy's bundled ones get wrong for some Unity versions (typetrees.py): an object
//! read with UnityPy's tree, else with a fixed one that reads every byte (fields later releases
//! added, unknown bytes at the end, another version's tree), and saved with the tree it was read
//! with.
use std::sync::Arc;
use unityfs::{Error, SerializedFile, TypeTree, Value};

/// Fields Unity releases added after the type trees UnityPy ships: class -> [(after, type, name,
/// aligned)].
fn fields(class: &str) -> &'static [(&'static str, &'static str, &'static str, bool)] {
    match class {
        "PlayerSettings" => &[(
            "insecureHttpOption",
            "bool",
            "captureStartupLogsBaked",
            true,
        )],
        _ => &[],
    }
}

/// Whether an error is what typetrees.py's _reads catches (a read that fails on the data).
fn read_fails(e: &Error) -> bool {
    !matches!(e, Error::Io(_))
}

fn reads(sf: &SerializedFile, pid: i64, tree: &TypeTree) -> Result<Option<Value>, Error> {
    match sf.read_typetree_with(pid, tree) {
        Ok(v) => Ok(Some(v)),
        Err(e) if read_fails(&e) => Ok(None),
        Err(e) => Err(e),
    }
}

fn candidates(sf: &SerializedFile, pid: i64, base: &TypeTree) -> Vec<TypeTree> {
    let o = sf.object(pid).unwrap();
    let f = fields(&sf.class_name(o));
    let mut out = Vec::new();
    for n in (1..=f.len()).rev() {
        for k in 0..=f.len() - n {
            if let Some(t) = base.with_fields(&f[k..k + n]) {
                out.push(t);
            }
        }
    }
    out
}

fn earliest_gap(
    sf: &SerializedFile,
    pid: i64,
    base: &TypeTree,
    k: usize,
) -> Result<Option<usize>, Error> {
    let typ = match k {
        1 => "UInt8",
        2 => "UInt16",
        4 => "int",
        8 => "SInt64",
        _ => return Ok(None),
    };
    let names: Vec<String> = base.fields().map(|(_, n)| n.name.to_string()).collect();
    for (i, n) in names.iter().enumerate() {
        for aligned in [false, true] {
            if let Some(t) = base.with_fields(&[(n.as_str(), typ, "_probe", aligned)]) {
                if reads(sf, pid, &t)?.is_some() {
                    return Ok(Some(i));
                }
            }
        }
    }
    Ok(None)
}

fn top(t: &TypeTree) -> Vec<(Arc<str>, Arc<str>)> {
    t.fields()
        .map(|(_, n)| (n.type_name.clone(), n.name.clone()))
        .collect()
}

/// UnityPy's trees of the object's class for the other versions it knows (older ones nearest
/// first, then newer ones) that differ from the one it reads the object with.
fn neighbours(sf: &SerializedFile, pid: i64) -> Vec<Arc<TypeTree>> {
    let o = sf.object(pid).unwrap();
    if o.type_index
        .and_then(|i| sf.types[i].node.as_ref())
        .is_some()
    {
        return Vec::new();
    }
    let Some(db) = sf.db() else {
        return Vec::new();
    };
    let Ok(own) = sf.typetree(o) else {
        return Vec::new();
    };
    let entries: Vec<_> = db
        .class_entries(o.class_id)
        .iter()
        .filter_map(|(v, c)| c.as_ref().map(|c| (*v, c)))
        .collect();
    let ver = sf.version;
    let older: Vec<_> = entries.iter().filter(|(v, _)| *v <= ver).collect();
    let newer: Vec<_> = entries.iter().filter(|(v, _)| *v > ver).collect();
    let mut seen = vec![own.shape()];
    let mut out = Vec::new();
    for (_, c) in older.into_iter().rev().chain(newer) {
        let Ok(n) = db.tree_of(o.class_id, c) else {
            continue;
        };
        let k = n.shape();
        if !seen.contains(&k) {
            seen.push(k);
            out.push(n);
        }
    }
    out
}

/// (value, the tree it was read with when not the object's own): typetrees.read(obj, edits).
pub fn read(
    sf: &SerializedFile,
    pid: i64,
    edits: &[&str],
) -> Result<(Value, Option<TypeTree>), Error> {
    let first = match sf.read_typetree(pid) {
        Ok(v) => return Ok((v, None)),
        Err(e @ Error::Io(_)) => return Err(e),
        Err(e) => e,
    };
    let short = match first {
        Error::ReadSize { expected, read } => Some((expected, read)),
        _ => None,
    };
    let base = sf.typetree_of(pid)?;
    let names: Vec<String> = base.fields().map(|(_, n)| n.name.to_string()).collect();
    let last = edits
        .iter()
        .filter_map(|e| names.iter().position(|n| n == e))
        .max()
        .map_or(-1, |i| i as i64);
    if let (false, Some((expected, read))) = (edits.is_empty(), short) {
        if expected >= read {
            if let Some(gap) = earliest_gap(sf, pid, &base, expected - read)? {
                if (gap as i64) < last {
                    let o = sf.object(pid).unwrap();
                    return Err(Error::Value(format!(
                        "{}: {first}; the missing field could be before {}, which this conversion edits: no fixed type tree used",
                        sf.class_name(o),
                        names[gap + 1]
                    )));
                }
            }
        }
    }
    for node in candidates(sf, pid, &base) {
        if reads(sf, pid, &node)?.is_some() {
            return Ok((sf.read_typetree_with(pid, &node)?, Some(node)));
        }
    }
    if let Some((expected, read)) = short {
        let diff = expected as i64 - read as i64;
        if 0 < diff && diff <= 16 {
            let mut node = Some((*base).clone());
            for i in 0..diff {
                node = node.and_then(|n| {
                    let last = n.fields().last().map(|(_, c)| c.name.to_string())?;
                    n.with_fields(&[(
                        last.as_str(),
                        "UInt8",
                        format!("_unknown_{i}").as_str(),
                        false,
                    )])
                });
            }
            if let Some(node) = node {
                if reads(sf, pid, &node)?.is_some() {
                    return Ok((sf.read_typetree_with(pid, &node)?, Some(node)));
                }
            }
        }
    }
    let btop = top(&base);
    for node in neighbours(sf, pid) {
        if !edits.is_empty() {
            let ntop = top(&node);
            let k = (last + 1) as usize;
            if last < 0
                || ntop.get(..k.min(ntop.len())) != btop.get(..k.min(btop.len()))
                || ntop.len().min(k) != btop.len().min(k)
            {
                continue;
            }
        }
        if reads(sf, pid, &node)?.is_some() {
            return Ok((sf.read_typetree_with(pid, &node)?, Some((*node).clone())));
        }
    }
    Err(first)
}

/// typetrees.save(obj, tree, node).
pub fn save(
    sf: &mut SerializedFile,
    pid: i64,
    v: &Value,
    node: Option<&TypeTree>,
) -> Result<(), Error> {
    match node {
        None => sf.save_typetree(pid, v),
        Some(t) => sf.save_typetree_with(pid, v, t),
    }
}
