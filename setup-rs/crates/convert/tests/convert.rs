//! The convert job on made-up data (tests/madeup): Cuphead's steps (textures apart) and what each
//! leaves in the files and the journal, a second run that finds everything done, and a run
//! killed between the renames of files committed together. Comparing with the Python's output
//! on the same data is tests/golden/convert_madeup.sh. Needs $PMU_TPK (skipped without).
mod madeup;

use convert::cli::{self, Cli};
use convert::pyjson::{self, Json};
use convert::Job;
use std::path::{Path, PathBuf};
use unityfs::{DataFile, Options, Value};

fn temp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("pmu-convert-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

/// Every file under a folder with its bytes, by relative path.
fn tree(dir: &Path) -> Vec<(String, Vec<u8>)> {
    fn rec(base: &Path, d: &Path, out: &mut Vec<(String, Vec<u8>)>) {
        for e in std::fs::read_dir(d).unwrap() {
            let p = e.unwrap().path();
            if p.is_dir() {
                rec(base, &p, out);
            } else {
                let rel = p.strip_prefix(base).unwrap().to_string_lossy().into_owned();
                out.push((rel, std::fs::read(&p).unwrap()));
            }
        }
    }
    let mut out = Vec::new();
    rec(dir, dir, &mut out);
    out.sort();
    out
}

fn cuphead(dir: &Path, steps: &str) -> Cli {
    let cfg = dir.join("cuphead.json");
    std::fs::write(&cfg, madeup::CUPHEAD_JSON).unwrap();
    Cli {
        data: dir.join("Data").to_string_lossy().into_owned(),
        config: cfg.to_string_lossy().into_owned(),
        steps: Some(steps.into()),
        plugins: vec!["/somewhere/convert/animclips.py".into()],
        sets: vec![
            "textures.quality=fast".into(),
            "textures.guide=/somewhere/astc_guide_fast.bin".into(),
        ],
        ..Default::default()
    }
}

fn journal(data: &Path) -> Json {
    pyjson::loads(&std::fs::read_to_string(data.join(".pmunity.json")).unwrap()).unwrap()
}

fn strs(v: &Json) -> Vec<String> {
    v.as_list()
        .unwrap()
        .iter()
        .map(|x| x.as_str().unwrap().to_string())
        .collect()
}

fn names(v: &Value) -> Vec<String> {
    v.as_array()
        .unwrap()
        .iter()
        .map(|a| a.get("m_Name").unwrap().as_str().unwrap().to_string())
        .collect()
}

#[test]
fn cuphead_steps_on_made_up_data() {
    let Some(db) = madeup::db() else {
        eprintln!("PMU_TPK not set: skipped");
        return;
    };
    let dir = temp("cuphead");
    let data = dir.join("Data");
    madeup::game(&db, &data);
    let before = tree(&data);
    cli::run(&cuphead(&dir, "shaders,settings,audio,animclips")).unwrap();

    let j = journal(&data);
    let done = j.get("done").unwrap();
    assert_eq!(
        strs(done.get("shaders").unwrap()),
        [
            "globalgamemanagers",
            "level0",
            "resources.assets",
            "sharedassets0.assets",
            "sharedassets1.assets",
            "sharedassets2.assets",
            "sharedassets3.assets",
            "Resources/unity_builtin_extra",
            "StreamingAssets/AssetBundles/tmpfont_big",
            "StreamingAssets/AssetBundles/tmpfont_main",
            "StreamingAssets/AssetBundles/tmpfont_ready",
            "StreamingAssets/AssetBundles/tmpfont_scenes",
        ]
    );
    assert_eq!(strs(done.get("settings").unwrap()), ["globalgamemanagers"]);
    assert_eq!(
        strs(done.get("audio").unwrap()),
        [
            "resources.assets",
            "sharedassets1.assets",
            "sharedassets2.assets"
        ]
    );
    assert_eq!(strs(done.get("animclips").unwrap()), ["animclips.txt"]);
    let audio = j.get("audio").unwrap();
    assert_eq!(
        audio.get("summary").unwrap().as_str().unwrap(),
        "3 clips kept compressed in memory (63.7 MiB of PCM), 4 decompressed on load as the game has them (6.5 MiB); budget 16 MiB a scene, the most a scene holds 13.3 MiB (scene 0)"
    );
    assert_eq!(
        audio.get("cfg").unwrap().dumps(),
        r#"{"pcm_budget_mb": 16, "compressed_min_s": 2.0, "resident_scenes": 2}"#
    );
    assert_eq!(
        audio.get("switch").unwrap().dumps(),
        r#"{"resources.assets": [1], "sharedassets1.assets": [1], "sharedassets2.assets": [1]}"#
    );
    assert!(j
        .dumps()
        .starts_with(r#"{"version": 1, "done": {"shaders": ["#));

    // settings: graphics APIs, mouse events, joystick axes
    let opts = Options {
        db: Some(db.clone()),
        ..Default::default()
    };
    let ggm = DataFile::open(data.join("globalgamemanagers"), &opts).unwrap();
    let bs = ggm.sf.read_typetree(1).unwrap();
    assert_eq!(
        bs.get("m_GraphicsAPIs").unwrap(),
        &Value::Array(vec![Value::I32(11), Value::I32(17)])
    );
    assert_eq!(bs.get("usesOnMouseEvents").unwrap(), &Value::Bool(false));
    let im = ggm.sf.read_typetree(2).unwrap();
    assert_eq!(
        names(im.get("m_Axes").unwrap()),
        ["Horizontal", "Vertical", "Joy1Axis3x", "Joy2Axis"]
    );

    // audio: the three long clips switched, the rest as they were
    let load = |rel: &str, pid: i64| {
        let f = DataFile::open(data.join(rel), &opts).unwrap();
        f.sf.read_typetree(pid)
            .unwrap()
            .get("m_LoadType")
            .unwrap()
            .as_int()
            .unwrap()
    };
    assert_eq!(load("resources.assets", 1), 1);
    assert_eq!(load("resources.assets", 3), 0);
    assert_eq!(load("sharedassets1.assets", 1), 1);
    assert_eq!(load("sharedassets2.assets", 1), 1);
    assert_eq!(load("sharedassets0.assets", 1), 0);

    // animclips.txt
    assert_eq!(
        std::fs::read_to_string(data.join("animclips.txt")).unwrap(),
        "attack\t-\t77\nidle\tH\t90\nmenu_blink\tS\t\nrun\tH\t77,78\n"
    );

    // shaders: GLES3 programs where they translate; files without changes left alone
    let plats = |rel: &str, pid: i64| {
        let f = DataFile::open(data.join(rel), &opts).unwrap();
        let t = f.sf.read_typetree(pid).unwrap();
        t.get("platforms")
            .unwrap()
            .as_array()
            .unwrap()
            .iter()
            .map(|x| x.as_int().unwrap())
            .collect::<Vec<_>>()
    };
    assert_eq!(plats("resources.assets", 20), [9, 15]);
    assert_eq!(plats("resources.assets", 21), [15]);
    assert_eq!(plats("resources.assets", 23), [9, 15]);
    assert_eq!(plats("level0", 5), [9, 15]);
    assert_eq!(plats("Resources/unity_builtin_extra", 8), [4]);
    assert_eq!(
        plats("StreamingAssets/AssetBundles/tmpfont_main", 1),
        [9, 15]
    );
    assert_eq!(
        plats("StreamingAssets/AssetBundles/tmpfont_big", 1),
        [9, 15]
    );
    let after = tree(&data);
    let same = |rel: &str| {
        before.iter().find(|(r, _)| r == rel).map(|x| &x.1)
            == after.iter().find(|(r, _)| r == rel).map(|x| &x.1)
    };
    for rel in [
        "StreamingAssets/AssetBundles/tmpfont_ready",
        "StreamingAssets/AssetBundles/tmpfont_scenes",
        "StreamingAssets/AssetBundles/atlas_other",
        "sharedassets0.assets",
    ] {
        assert!(same(rel), "{rel} changed");
    }
    assert!(!same("resources.assets"));
    // (the last file the shaders step saw was left as it is: as the Python, its work folder goes
    // and .pmunity-work stays, empty)
    assert!(!data.join(".pmunity-work/shaders").exists());

    // a second run finds every file done and changes nothing
    cli::run(&cuphead(&dir, "shaders,settings,audio,animclips")).unwrap();
    assert_eq!(tree(&data), after);

    // the textures step: the files its regex names, the scaled textures journaled
    cli::run(&cuphead(&dir, "textures")).unwrap();
    let j = journal(&data);
    assert_eq!(
        strs(j.get("done").unwrap().get("textures").unwrap()),
        [
            "resources.assets",
            "sharedassets0.assets",
            "sharedassets1.assets",
            "sharedassets2.assets",
            "sharedassets3.assets",
            "StreamingAssets/AssetBundles/atlas_main",
            "StreamingAssets/AssetBundles/atlas_other",
        ]
    );
    assert_eq!(
        j.get("scaled").unwrap().dumps(),
        r#"{"sharedassets3.assets": {"1": [256, 128, 96, 48], "2": [128, 64, 64, 32], "5": [2048, 2048, 768, 768]}, "StreamingAssets/AssetBundles/atlas_main": {"1": [512, 512, 192, 192], "2": [256, 256, 96, 96]}}"#
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn journal_pending_renames_finished() {
    let dir = temp("pending");
    let data = dir.join("Data");
    std::fs::create_dir_all(data.join("sub")).unwrap();
    std::fs::write(data.join("sub/a.assets"), b"old a").unwrap();
    std::fs::write(data.join("sub/a.assets.resS"), b"old s").unwrap();
    let cfg = pyjson::loads(r#"{"version": 1}"#).unwrap();
    let mut job = Job::new(&data, &data, cfg.clone()).unwrap();
    assert!(job.inplace);
    let w = job.work_dir("textures").unwrap();
    std::fs::write(w.join("file.new"), b"new a").unwrap();
    std::fs::write(w.join("stream.new"), b"new s").unwrap();
    job.commit(&[
        ("sub/a.assets.resS".into(), w.join("stream.new")),
        ("sub/a.assets".into(), w.join("file.new")),
    ])
    .unwrap();
    job.end_work(&w);
    assert_eq!(std::fs::read(data.join("sub/a.assets")).unwrap(), b"new a");
    // (the last file the shaders step saw was left as it is: as the Python, its work folder goes
    // and .pmunity-work stays, empty)
    assert!(!data.join(".pmunity-work/shaders").exists());
    // the journal had the pair listed while it was renamed, and no longer has it
    assert_eq!(
        std::fs::read_to_string(data.join(".pmunity.json")).unwrap(),
        r#"{"version": 1, "done": {}, "scaled": {}}"#
    );
    // a run killed after the first of two renames: the next one finishes the second
    let w = job.work_dir("textures").unwrap();
    std::fs::write(w.join("file.new"), b"newer a").unwrap();
    let pending = format!(
        r#"{{"version": 1, "done": {{"textures": ["x"]}}, "scaled": {{}}, "pending": [["{}", "{}"], ["{}", "{}"]]}}"#,
        w.join("gone").display(),
        data.join("sub/a.assets.resS").display(),
        w.join("file.new").display(),
        data.join("sub/a.assets").display()
    );
    std::fs::write(data.join(".pmunity.json"), pending).unwrap();
    assert_eq!(convert::job::journal_files(&data.join(".pmunity.json")), 1);
    assert_eq!(convert::job::journal_files(&data.join("none.json")), 0);
    let job = Job::new(&data, &data, cfg).unwrap();
    assert!(job.is_done("textures", "x"));
    assert_eq!(
        std::fs::read(data.join("sub/a.assets")).unwrap(),
        b"newer a"
    );
    assert_eq!(
        std::fs::read_to_string(data.join(".pmunity.json")).unwrap(),
        r#"{"version": 1, "done": {"textures": ["x"]}, "scaled": {}}"#
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn errors_and_statuses() {
    let dir = temp("errors");
    let data = dir.join("Data");
    std::fs::create_dir_all(&data).unwrap();
    let cfg = dir.join("c.json");
    std::fs::write(&cfg, "{}").unwrap();
    let cli = Cli {
        data: data.to_string_lossy().into_owned(),
        config: cfg.to_string_lossy().into_owned(),
        ..Default::default()
    };
    // not a Unity data folder
    let e = cli::run(&cli).unwrap_err();
    assert_eq!(e.status(), 1);
    assert!(e
        .to_string()
        .ends_with("no globalgamemanagers, mainData or data.unity3d (not a Unity data folder?)"));
    // a version outside the supported range
    let mut h = vec![0u8; 20];
    h[11] = 17;
    h.extend_from_slice(b"5.5.1f1\0");
    h.resize(64, 0);
    std::fs::write(data.join("globalgamemanagers"), &h).unwrap();
    let e = cli::run(&cli).unwrap_err();
    assert!(e
        .to_string()
        .starts_with("Unity 5.5.1f1: the converter supports data of Unity 5.6 - 6000.6"));
    // unknown steps stop the run with the steps there are
    let e = cli::run(&Cli {
        steps: Some("nope".into()),
        any_unity_version: true,
        ..cli.clone()
    })
    .unwrap_err();
    assert_eq!(
        e.to_string(),
        "unknown step nope; steps: shaders, textures, settings, audio, quality"
    );
    // a plugin pmu does not have
    let e = cli::run(&Cli {
        plugins: vec!["x/other.py".into()],
        any_unity_version: true,
        ..cli.clone()
    })
    .unwrap_err();
    assert_eq!(e.status(), 2);
    let _ = std::fs::remove_dir_all(&dir);
}
