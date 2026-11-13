//! The `lazy` step on made-up data: the objects named in `lazy.names` (in another file) are left out of a
//! scene's PreloadData and of the ResourceManager's dependency lists (each entry replaced by one the list's owner
//! loads anyway, so nothing changes length), everything else listed stays in its place, files without such lists
//! are written as they were, the scene file keeps its size, and a second run changes nothing. Needs $PMU_TPK (skipped
//! without).
mod madeup;

use convert::cli::{self, Cli};
use madeup::Obj;
use std::path::{Path, PathBuf};
use unityfs::{DataFile, Options, Value};

fn temp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("pmu-lazy-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn o(class_id: i32, path_id: i64, value: Value) -> Obj {
    Obj { class_id, path_id, value }
}

fn write(data: &Path, rel: &str, b: &[u8]) {
    let p = data.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, b).unwrap();
}

/// A list of PPtrs as (file id, path id).
fn pptrs(v: &Value) -> Vec<(i128, i128)> {
    v.as_array()
        .unwrap()
        .iter()
        .map(|p| (p.get("m_FileID").unwrap().as_int().unwrap(), p.get("m_PathID").unwrap().as_int().unwrap()))
        .collect()
}

fn cli(dir: &Path, names: &str) -> Cli {
    let cfg = dir.join("lazy.json");
    std::fs::write(&cfg, "{\"steps\": [\"lazy\"]}").unwrap();
    Cli {
        data: dir.join("Data").to_string_lossy().into_owned(),
        config: cfg.to_string_lossy().into_owned(),
        steps: Some("lazy".into()),
        sets: vec![format!("lazy.names={names}")],
        ..Default::default()
    }
}

#[test]
fn lazy_on_made_up_data() {
    let Some(db) = madeup::db() else {
        eprintln!("PMU_TPK not set: skipped");
        return;
    };
    let opts = Options { db: Some(db.clone()), ..Default::default() };
    let dir = temp("names");
    let data = dir.join("Data");
    // sharedassets0.assets: the fonts' file, the atlases of two languages
    let s0 = madeup::serialized(
        &db,
        &[
            o(28, 1, madeup::texture(&db, "latin Atlas", (8, 8), 1, &[0; 64], None)),
            o(28, 2, madeup::texture(&db, "chinese_body Atlas", (8, 8), 1, &[0; 64], None)),
            o(150, 3, madeup::preload_data(&db, &[])),
        ],
        &[],
        true,
    );
    write(&data, "sharedassets0.assets", &s0);
    // scene 1: its own object, both atlases from sharedassets0 (file id 1), one from elsewhere (file id 2)
    let s1 = madeup::serialized(
        &db,
        &[
            o(28, 1, madeup::texture(&db, "scene tex", (8, 8), 1, &[0; 64], None)),
            o(150, 2, madeup::preload_data(&db, &[(0, 1), (1, 1), (1, 2), (2, 2), (1, 2)])),
        ],
        &["sharedassets0.assets", "resources.assets"],
        false, // (no type trees, as a player build's: the files the setup edits)
    );
    write(&data, "sharedassets1.assets", &s1);
    // globalgamemanagers: a ResourceManager whose one resource depends on both atlases
    let rm_tree = madeup::tree(&db, 147);
    let mut rm = madeup::object(&db, 147);
    let mut dep = madeup::element(&rm_tree, &["m_DependentAssets"]);
    dep.set("m_Object", madeup::pptr(2, 9)).unwrap();
    dep.set("m_Dependencies", Value::Array(vec![madeup::pptr(1, 2), madeup::pptr(1, 1), madeup::pptr(2, 3)])).unwrap();
    madeup::set(&mut rm, &["m_DependentAssets"], Value::Array(vec![dep]));
    let ggm = madeup::serialized(&db, &[o(147, 1, rm)], &["sharedassets0.assets", "resources.assets"], false);
    write(&data, "globalgamemanagers", &ggm);
    let untouched = std::fs::read(data.join("sharedassets0.assets")).unwrap();

    cli::run(&cli(&dir, "chinese_body Atlas|no such Atlas")).unwrap();
    assert_eq!(std::fs::metadata(data.join("sharedassets1.assets")).unwrap().len() as usize, s1.len());
    let f = DataFile::open(data.join("sharedassets1.assets"), &opts).unwrap();
    let t = f.sf.read_typetree(2).unwrap();
    // (each entry for the named atlas replaced by the list's first other one: the same length)
    assert_eq!(pptrs(t.get("m_Assets").unwrap()), [(0, 1), (1, 1), (0, 1), (2, 2), (0, 1)]);
    let g = DataFile::open(data.join("globalgamemanagers"), &opts).unwrap();
    let t = g.sf.read_typetree(1).unwrap();
    let deps = t.get("m_DependentAssets").unwrap().as_array().unwrap();
    // (a resource's dependency on it replaced by the resource itself)
    assert_eq!(pptrs(deps[0].get("m_Dependencies").unwrap()), [(2, 9), (1, 1), (2, 3)]);
    assert_eq!(std::fs::read(data.join("sharedassets0.assets")).unwrap(), untouched);
    // again: nothing left to do, nothing written
    let before = std::fs::read(data.join("sharedassets1.assets")).unwrap();
    cli::run(&cli(&dir, "chinese_body Atlas|no such Atlas")).unwrap();
    assert_eq!(std::fs::read(data.join("sharedassets1.assets")).unwrap(), before);
    let _ = std::fs::remove_dir_all(&dir);
}
