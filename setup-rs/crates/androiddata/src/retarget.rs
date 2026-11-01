//! A game's data for the Android player of another release of the same Unity line (retarget.py):
//! the player only opens serialized files that name its own version, and reads them with its own
//! layouts. A file's metadata gets the player's version (in place when the space allows, else
//! with the object data moved by a multiple of 16 bytes); objects of classes whose release layout
//! differs between the two versions (UnityPy's class database) are read with the data's tree and
//! written with the player's, fields kept by name, new ones zero / false / empty.
use crate::pyjson::float_repr;
use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::sync::{Arc, Mutex, OnceLock};
use unityfs::typetree::Kind;
use unityfs::{ClassDb, FileSource, SerializedFile, Source, TypeTree, UnityVersion, Value};

type Change = Option<(Arc<TypeTree>, Arc<TypeTree>)>;

fn cache() -> &'static Mutex<HashMap<(i32, String, String), Change>> {
    static C: OnceLock<Mutex<HashMap<(i32, String, String), Change>>> = OnceLock::new();
    C.get_or_init(Default::default)
}

/// A node's subtree as retarget._flat compares it: (level, type, name, size, align flag).
fn flat(t: &TypeTree, i: usize) -> Vec<(i32, Arc<str>, Arc<str>, i32, i32)> {
    let mut out = Vec::new();
    let mut stack = vec![i];
    while let Some(n) = stack.pop() {
        let d = &t.nodes[n];
        out.push((
            d.level,
            d.type_name.clone(),
            d.name.clone(),
            d.byte_size,
            d.meta_flag.unwrap_or(0) & unityfs::typetree::ALIGN,
        ));
        stack.extend(d.children.iter().rev());
    }
    out
}

/// (tree in src, tree in dst) of a class whose release layout differs between Unity versions src
/// and dst, else None (also for classes the database has no tree of).
pub fn layout_change(db: &ClassDb, class_id: i32, src: &str, dst: &str) -> Change {
    let key = (class_id, src.to_string(), dst.to_string());
    if let Some(c) = cache().lock().unwrap().get(&key) {
        return c.clone();
    }
    let tree = |v: &str| UnityVersion::parse(v).and_then(|v| db.typetree(class_id, v).ok());
    let c = match (tree(src), tree(dst)) {
        (Some(a), Some(b)) if flat(&a, 0) != flat(&b, 0) => Some((a, b)),
        _ => None,
    };
    cache().lock().unwrap().insert(key, c.clone());
    c
}

/// (class id, class name) of every class whose release layout differs between src and dst.
pub fn changed_classes(db: &ClassDb, src: &str, dst: &str) -> Vec<(i32, String)> {
    let mut out = Vec::new();
    for cid in db.class_ids() {
        if let Some((_, b)) = layout_change(db, cid, src, dst) {
            out.push((cid, b.class_name().to_string()));
        }
    }
    out
}

// ------------------------------------------------------------------- values

fn scalar(k: Kind) -> bool {
    k.is_number()
}

fn is_array(t: &TypeTree, i: usize) -> bool {
    t.nodes[i]
        .children
        .first()
        .is_some_and(|&c| &*t.nodes[c].type_name == "Array")
}

/// The value of a newly added field: 0, 0.0, False, '', empty array, a structure of those.
pub fn default(t: &TypeTree, i: usize) -> Value {
    let n = &t.nodes[i];
    match &*n.type_name {
        "bool" => return Value::Bool(false),
        "float" => return Value::F32(0.0),
        "double" => return Value::F64(0.0),
        "string" => return Value::Str(Vec::new()),
        "TypelessData" => return Value::Bytes(Vec::new()),
        _ => {}
    }
    if scalar(n.kind) {
        return Value::I64(0);
    }
    if &*n.type_name == "pair" {
        return Value::Pair(Box::new((
            default(t, n.children[0]),
            default(t, n.children[1]),
        )));
    }
    if is_array(t, i) {
        return Value::Array(Vec::new());
    }
    let mut s = unityfs::Struct::default();
    for &c in &n.children {
        s.set(&t.nodes[c].name, default(t, c));
    }
    Value::Struct(s)
}

/// repr() of a value as UnityPy reads it (dicts, lists, tuples, str, bytes, numbers).
pub fn repr(v: &Value) -> String {
    match v {
        Value::Bool(b) => (if *b { "True" } else { "False" }).into(),
        Value::F32(f) => float_repr(*f as f64),
        Value::F64(f) => float_repr(*f),
        Value::Str(b) => pkgzip::py::str_repr(&String::from_utf8_lossy(b)),
        Value::Bytes(b) => pkgzip::py::bytes_repr(b),
        Value::Array(a) => format!("[{}]", a.iter().map(repr).collect::<Vec<_>>().join(", ")),
        Value::Pair(p) => format!("({}, {})", repr(&p.0), repr(&p.1)),
        Value::Struct(s) => format!(
            "{{{}}}",
            s.fields
                .iter()
                .map(|(k, v)| format!("{}: {}", pkgzip::py::str_repr(k), repr(v)))
                .collect::<Vec<_>>()
                .join(", ")
        ),
        v => v.as_int().map(|i| i.to_string()).unwrap_or_default(),
    }
}

fn elements(v: &Value) -> Vec<Value> {
    match v {
        Value::Array(a) => a.clone(),
        Value::Bytes(b) | Value::Str(b) => b.iter().map(|&x| Value::U8(x)).collect(),
        _ => Vec::new(),
    }
}

/// value (read with node ai of tree a) as a value of node bi of tree b.
pub fn convert(
    v: &Value,
    a: &TypeTree,
    ai: usize,
    b: &TypeTree,
    bi: usize,
    path: &str,
    notes: &mut Vec<String>,
) -> Value {
    if flat(a, ai) == flat(b, bi) {
        return v.clone();
    }
    let (na, nb) = (&a.nodes[ai], &b.nodes[bi]);
    if scalar(na.kind) && scalar(nb.kind) {
        if &*nb.type_name == "bool" {
            return Value::Bool(v.as_f64().is_some_and(|x| x != 0.0));
        }
        if matches!(&*nb.type_name, "float" | "double") {
            return Value::F64(v.as_f64().unwrap_or(0.0));
        }
        return match v {
            Value::F32(f) => Value::I64(*f as i64),
            Value::F64(f) => Value::I64(*f as i64),
            v => Value::I64(v.as_int().unwrap_or(0) as i64),
        };
    }
    if &*na.type_name == "pair" && &*nb.type_name == "pair" {
        if let Value::Pair(p) = v {
            let x = convert(&p.0, a, na.children[0], b, nb.children[0], path, notes);
            let y = convert(&p.1, a, na.children[1], b, nb.children[1], path, notes);
            return Value::Pair(Box::new((x, y)));
        }
    }
    if is_array(a, ai) && is_array(b, bi) {
        let ea = a.nodes[na.children[0]].children[1];
        let eb = b.nodes[nb.children[0]].children[1];
        let p = format!("{path}[]");
        return Value::Array(
            elements(v)
                .iter()
                .map(|x| convert(x, a, ea, b, eb, &p, notes))
                .collect(),
        );
    }
    if let (Value::Struct(s), false) = (
        v,
        nb.children.is_empty() || is_array(b, bi) || scalar(nb.kind),
    ) {
        let have: HashMap<&str, usize> = na
            .children
            .iter()
            .map(|&c| (&*a.nodes[c].name, c))
            .collect();
        let mut out = unityfs::Struct::default();
        for &c in &nb.children {
            let name = &*b.nodes[c].name;
            match (have.get(name), s.get(name)) {
                (Some(&ca), Some(x)) => {
                    let nv = convert(x, a, ca, b, c, &format!("{path}.{name}"), notes);
                    out.set(name, nv);
                }
                _ => {
                    let d = default(b, c);
                    notes.push(format!(
                        "{path}.{name} added ({} {})",
                        b.nodes[c].type_name,
                        repr(&d)
                    ));
                    out.set(name, d);
                }
            }
        }
        // (the names of a's fields in their first order, as the Python's dict)
        let mut seen: Vec<&str> = Vec::new();
        for &c in &na.children {
            let n = &*a.nodes[c].name;
            if !seen.contains(&n) {
                seen.push(n);
            }
        }
        for n in seen {
            if out.get(n).is_none() {
                let was = s.get(n).map_or("None".to_string(), repr);
                notes.push(format!("{path}.{n} dropped (was {was})"));
            }
        }
        return Value::Struct(out);
    }
    let d = default(b, bi);
    notes.push(format!(
        "{path}: {} -> {}, reset to {}",
        na.type_name,
        nb.type_name,
        repr(&d)
    ));
    d
}

/// Objects of sf (data of Unity src) whose class layout differs in dst, rewritten in dst's
/// layout (an object saved before is converted from its saved bytes). Returns notes.
pub fn migrate(sf: &mut SerializedFile, src: &str, dst: &str) -> unityfs::Result<Vec<String>> {
    let Some(db) = sf.db().cloned() else {
        return Ok(Vec::new());
    };
    let mut notes = Vec::new();
    let objs: Vec<(i64, i32, bool)> = sf
        .objects
        .iter()
        .map(|o| (o.path_id, o.class_id, o.data.is_some()))
        .collect();
    for (pid, cid, saved) in objs {
        let Some((a, b)) = layout_change(&db, cid, src, dst) else {
            continue;
        };
        let tree = if saved {
            let data = sf.bytes(pid)?;
            sf.read_value(&a, &data)?
        } else {
            sf.read_typetree_with(pid, &a)?
        };
        let n0 = notes.len();
        let new = convert(&tree, &a, 0, &b, 0, b.class_name(), &mut notes);
        sf.save_typetree_with(pid, &new, &b)?;
        if notes.len() == n0 {
            notes.push(format!("{}: layout of {dst} written", b.class_name()));
        }
    }
    Ok(notes)
}

// ------------------------------------------------------------------- metadata only

/// (format, header size, metadata size, file size, data offset, first 64 bytes).
fn head(path: &Path) -> unityfs::Result<(u32, u64, u64, u64, u64, Vec<u8>)> {
    let mut h = Vec::new();
    File::open(path)?.take(64).read_to_end(&mut h)?;
    let be32 = |o: usize| -> unityfs::Result<u32> {
        h.get(o..o + 4)
            .map(|b| u32::from_be_bytes(b.try_into().unwrap()))
            .ok_or_else(|| unityfs::Error::Format("unpack_from: short header".into()))
    };
    let fmt = be32(8)?;
    if fmt < 9 {
        return Err(unityfs::Error::Format(format!(
            "{}: SerializedFile format {fmt} (before Unity 5) keeps its metadata at the end",
            path.display()
        )));
    }
    if fmt >= 22 {
        let be64 = |o: usize| u64::from_be_bytes(h[o..o + 8].try_into().unwrap());
        if h.len() < 40 {
            return Err(unityfs::Error::Format("unpack_from: short header".into()));
        }
        return Ok((fmt, 48, be32(20)? as u64, be64(24), be64(32), h));
    }
    Ok((
        fmt,
        20,
        be32(0)? as u64,
        be32(4)? as u64,
        be32(12)? as u64,
        h,
    ))
}

/// The header and metadata of a serialized file (its objects not read).
pub fn parse_metadata(path: &Path, db: Option<Arc<ClassDb>>) -> unityfs::Result<SerializedFile> {
    let src: Arc<dyn Source> = Arc::new(FileSource::open(path)?);
    let name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    SerializedFile::open(&name, src, db, None)
}

/// Rewrites path's metadata with Unity version `version` and target platform `platform`.
/// Returns the bytes the object data moved (0: rewritten in place).
pub fn retarget_metadata(
    path: &Path,
    version: &str,
    platform: u32,
    sf: &mut SerializedFile,
) -> unityfs::Result<u64> {
    let (fmt, hsize, _msize, fsize, doff, h) = head(path)?;
    sf.unity_version = version.as_bytes().to_vec();
    sf.target_platform = platform as i32;
    let meta = sf.metadata_in_place()?;
    let end = hsize + meta.len() as u64;
    let shift = if end <= doff {
        0
    } else {
        (end - doff).div_ceil(16) * 16
    };
    let ndoff = doff + shift;
    let mut blob = Vec::new();
    if fmt >= 22 {
        blob.extend(&h[..20]);
        blob.extend((meta.len() as u32).to_be_bytes());
        blob.extend((fsize + shift).to_be_bytes());
        blob.extend(ndoff.to_be_bytes());
        blob.extend(&h[40..48]);
    } else {
        blob.extend((meta.len() as u32).to_be_bytes());
        blob.extend(((fsize + shift) as u32).to_be_bytes());
        blob.extend(fmt.to_be_bytes());
        blob.extend((ndoff as u32).to_be_bytes());
        blob.extend(&h[16..20]);
    }
    blob.extend(&meta);
    blob.resize(blob.len() + (ndoff - end) as usize, 0);
    if shift == 0 {
        let mut f = OpenOptions::new().write(true).open(path)?;
        f.write_all(&blob)?;
        return Ok(0);
    }
    let tmp = crate::paths::with_suffix(path, ".retarget");
    {
        let mut fi = File::open(path)?;
        let mut fo = std::io::BufWriter::with_capacity(1 << 20, File::create(&tmp)?);
        fo.write_all(&blob)?;
        fi.seek(SeekFrom::Start(doff))?;
        std::io::copy(&mut fi, &mut fo)?;
        fo.flush()?;
    }
    std::fs::rename(&tmp, path)?;
    Ok(shift)
}

/// A plain serialized file (data of Unity src) made for the player of Unity dst, in place.
/// Returns a note for the report.
pub fn retarget_file(
    path: &Path,
    src: &str,
    dst: &str,
    platform: u32,
    db: &Arc<ClassDb>,
) -> unityfs::Result<String> {
    let mut sf = parse_metadata(path, Some(db.clone()))?;
    if sf.enable_type_tree {
        let (_, hsize, ..) = head(path)?;
        let mut f = OpenOptions::new().write(true).open(path)?;
        f.seek(SeekFrom::Start(hsize + sf.unity_version.len() as u64 + 1))?;
        f.write_all(&if sf.header.big {
            platform.to_be_bytes()
        } else {
            platform.to_le_bytes()
        })?;
        return Ok(format!(
            "platform -> {platform}; type trees: version {} kept",
            String::from_utf8_lossy(&sf.unity_version)
        ));
    }
    let classes: Vec<i32> = sf.types.iter().map(|t| t.class_id).collect();
    if !classes
        .iter()
        .any(|&c| layout_change(db, c, src, dst).is_some())
    {
        let shift = retarget_metadata(path, dst, platform, &mut sf)?;
        let moved = if shift != 0 {
            format!(" (object data moved by {shift} bytes)")
        } else {
            String::new()
        };
        return Ok(format!(
            "platform -> {platform}, version {src} -> {dst}{moved}"
        ));
    }
    drop(sf);
    let mut full = unityfs::DataFile::open(
        path,
        &unityfs::Options {
            db: Some(db.clone()),
            ..Default::default()
        },
    )?;
    let notes = migrate(&mut full.sf, src, dst)?;
    full.sf.target_platform = platform as i32;
    full.sf.unity_version = dst.as_bytes().to_vec();
    let tmp = crate::paths::with_suffix(path, ".retarget");
    full.write_to(&tmp, false)?;
    drop(full);
    std::fs::rename(&tmp, path)?;
    Ok(format!(
        "platform -> {platform}, version {src} -> {dst}; {}",
        notes.join("; ")
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use unityfs::Node;

    fn tree(nodes: &[(i32, &str, &str, i32, i32)]) -> TypeTree {
        TypeTree::from_list(
            nodes
                .iter()
                .map(|&(l, t, n, s, m)| {
                    let mut x = Node::new(l, t, n, s, 1);
                    x.meta_flag = Some(m);
                    x
                })
                .collect(),
        )
        .unwrap()
    }

    #[test]
    fn convert_by_name() {
        let a = tree(&[
            (0, "PlayerSettings", "Base", -1, 0),
            (1, "bool", "displayResolutionDialog", 1, 0x4000),
            (1, "int", "iosAppInBackgroundBehavior", 4, 0),
            (1, "float", "x", 4, 0),
        ]);
        let b = tree(&[
            (0, "PlayerSettings", "Base", -1, 0),
            (1, "bool", "displayResolutionDialog", 1, 0),
            (1, "bool", "iosUseCustomAppBackgroundBehavior", 1, 0x4000),
            (1, "int", "x", 4, 0),
        ]);
        let mut s = unityfs::Struct::default();
        s.set("displayResolutionDialog", Value::Bool(true));
        s.set("iosAppInBackgroundBehavior", Value::I32(0));
        s.set("x", Value::F32(2.5));
        let mut notes = Vec::new();
        let v = convert(
            &Value::Struct(s),
            &a,
            0,
            &b,
            0,
            "PlayerSettings",
            &mut notes,
        );
        assert_eq!(
            notes,
            [
                "PlayerSettings.iosUseCustomAppBackgroundBehavior added (bool False)",
                "PlayerSettings.iosAppInBackgroundBehavior dropped (was 0)"
            ]
        );
        let bytes = unityfs::value::write(&b, &v, false, None).unwrap();
        assert_eq!(bytes, [1, 0, 0, 0, 2, 0, 0, 0]);
    }

    /// A format 17 serialized file without type trees: objects (path id, bytes), the data at
    /// `data_offset` (room after the metadata, as Unity leaves).
    fn no_tree_file(version: &str, objs: &[(i64, &[u8])], pad: usize) -> Vec<u8> {
        let mut m = unityfs::bin::Writer::new(false);
        m.cstr(version.as_bytes());
        m.i32(2);
        m.u8(0);
        m.i32(1);
        m.i32(129); // class id
        m.u8(0);
        m.i16(-1);
        m.bytes(&[0x11; 16]);
        m.i32(objs.len() as i32);
        let mut data = Vec::new();
        for (pid, b) in objs {
            while (m.len() + 20) % 4 != 0 {
                m.u8(0);
            }
            while data.len() % 8 != 0 {
                data.push(0);
            }
            m.i64(*pid);
            m.u32(data.len() as u32);
            m.u32(b.len() as u32);
            m.i32(0);
            data.extend_from_slice(b);
        }
        m.i32(0);
        m.i32(0);
        m.cstr(b"");
        let data_offset = (20 + m.len()).div_ceil(16) * 16 + pad;
        let mut f = unityfs::bin::Writer::new(true);
        f.u32(m.len() as u32);
        f.u32((data_offset + data.len()) as u32);
        f.u32(17);
        f.u32(data_offset as u32);
        f.bytes(&[0, 0, 0, 0]);
        f.bytes(&m.buf);
        f.buf.resize(data_offset, 0);
        f.bytes(&data);
        f.buf
    }

    #[test]
    fn metadata_retargeted() {
        let dir = std::env::temp_dir().join(format!("androiddata-rt-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let objs: &[(i64, &[u8])] = &[(1, b"abcdefgh"), (7, b"xyz")];
        // (one byte more fits the alignment's slack; 25 more move the data by 16, unless there is room)
        for (k, (to, pad, moved)) in [
            ("2017.4.40f1", 0usize, 0u64),
            ("2017.4.40f1-and-thirty-more-bytes!!", 0, 16),
            ("2017.4.40f1-and-thirty-more-bytes!!", 32, 0),
        ]
        .into_iter()
        .enumerate()
        {
            let p = dir.join(format!("level{k}"));
            std::fs::write(&p, no_tree_file("2017.4.9f1", objs, pad)).unwrap();
            let mut sf = parse_metadata(&p, None).unwrap();
            let shift = retarget_metadata(&p, to, 13, &mut sf).unwrap();
            assert_eq!(shift, moved, "{to} {pad}");
            let back = parse_metadata(&p, None).unwrap();
            assert_eq!(back.unity_version, to.as_bytes());
            assert_eq!(back.target_platform, 13);
            for (pid, b) in objs {
                assert_eq!(back.original_bytes(*pid).unwrap(), *b);
            }
            assert_eq!(back.header.file_size, std::fs::metadata(&p).unwrap().len());
        }
        let _ = std::fs::remove_dir_all(&dir);
    }
}
