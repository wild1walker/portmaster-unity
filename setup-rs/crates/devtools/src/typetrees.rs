//! `pmu-dev typetrees <out.json>` (was tools/prepatch/tools/export_typetrees.py): the type trees
//! prepatch.html's conversion steps need, from the class database pmu reads (UnityPy 1.25.4's lzma.tpk:
//! $PMU_TPK, else the one built into the program), for files built without type trees:
//!   classes   {class id: [[unity version (decimal string), node list index or null], ...]}  (ascending; the
//!             last entry at or below a file's version applies, as UnityPy's get_item_for_version)
//!   nodes     [[[level, type, name, byte size, meta flag], ...], ...]   (UnityPy's generate_node order)
//!   strings   {offset: common string}   (type tree blobs' shared strings, for files with type trees)
use std::collections::HashMap;

use serde_json::{json, Map, Value};

const CLASSES: [i32; 21] = [
    28, 213, 687078895, 43, 89, 187, 117, 188, 142, // textures, sprites, atlases, meshes, bundles
    141, 13, 11, 83, 150, 74, // the settings, audio and animation-clip steps
    30, 116, 129, 47, 72, // android-data's globalgamemanagers edits
    48,  // the shaders step
];

pub fn export(db: &unityfs::ClassDb) -> Result<(Value, String), String> {
    let (mut lists, mut index, mut classes): (Vec<Value>, HashMap<String, usize>, Map<String, Value>) = (vec![], HashMap::new(), Map::new());
    let mut n_entries = 0;
    for cid in CLASSES {
        let mut entries = vec![];
        for (version, cls) in db.class_entries(cid) {
            let mut li = Value::Null;
            if let Some(c) = cls.as_ref().filter(|c| c.release_root.is_some()) {
                let t = db.tree_of(cid, c).map_err(|e| e.to_string())?;
                let nodes: Vec<Value> = t
                    .nodes
                    .iter()
                    .map(|n| json!([n.level, &*n.type_name, &*n.name, n.byte_size, n.meta_flag.unwrap_or(0)]))
                    .collect();
                let key = serde_json::to_string(&nodes).unwrap();
                let i = *index.entry(key).or_insert_with(|| {
                    lists.push(Value::Array(nodes.clone()));
                    lists.len() - 1
                });
                li = json!(i);
            }
            entries.push(json!([version.0.to_string(), li]));
            n_entries += 1;
        }
        classes.insert(cid.to_string(), Value::Array(entries));
    }
    let mut strings = Map::new();
    let common = db.common_strings().entries();
    for (o, s) in &common {
        strings.insert(o.to_string(), json!(s));
    }
    let summary = format!("{} node lists, {n_entries} version entries, {} common strings", lists.len(), common.len());
    Ok((json!({"source": "UnityPy 1.25.4 lzma.tpk", "classes": classes, "nodes": lists, "strings": strings}), summary))
}

pub fn run(args: &[String]) -> Result<u8, String> {
    let [out] = args else { return Err("usage: pmu-dev typetrees <out.json>".into()) };
    let db = unityfs::ClassDb::default_db().map_err(|e| e.to_string())?;
    let (v, summary) = export(&db)?;
    std::fs::write(out, serde_json::to_string(&v).unwrap()).map_err(|e| format!("{out}: {e}"))?;
    println!("{out}: {summary}");
    Ok(0)
}
