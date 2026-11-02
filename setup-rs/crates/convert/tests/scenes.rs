//! Bundles with several serialized files (scene bundles: `BuildPlayer-<scene>` and its
//! `.sharedAssets`) through the shaders, textures and edits steps, on made-up data: each
//! serialized file converted as the same objects are anywhere else (a one-file bundle, a plain
//! file), the files without changes and the other nodes kept byte for byte, PPtrs from the scene
//! into its shared assets followed, the bundle written as a one-file bundle is (chunked LZ4, the
//! nodes' flags, the platform patch in place), and the bundles the steps leave. Needs $PMU_TPK
//! (skipped without).
mod madeup;

use convert::cli::{self, Cli};
use convert::pyjson::{self, Json};
use convert::steps::textures::{self, Choice, Codec, Converted, Tex};
use convert::steps::Args;
use convert::{Job, Result};
use madeup::{Obj, ShaderKind};
use std::path::{Path, PathBuf};
use unityfs::bundle::Bundle;
use unityfs::{ClassDb, DataFile, Options, Value};

fn temp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("pmu-scenes-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn o(class_id: i32, path_id: i64, value: Value) -> Obj {
    Obj {
        class_id,
        path_id,
        value,
    }
}

fn write(data: &Path, rel: &str, b: &[u8]) {
    let p = data.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, b).unwrap();
}

fn journal(data: &Path) -> Json {
    pyjson::loads(&std::fs::read_to_string(data.join(".pmunity.json")).unwrap()).unwrap()
}

fn all(db: &std::sync::Arc<ClassDb>) -> Options {
    Options {
        db: Some(db.clone()),
        all_serialized: true,
        ..Default::default()
    }
}

/// A serialized file from its bytes.
fn sf_of(b: Vec<u8>, db: &std::sync::Arc<ClassDb>) -> unityfs::SerializedFile {
    unityfs::SerializedFile::open(
        "x",
        std::sync::Arc::new(unityfs::MemSource(b)),
        Some(db.clone()),
        None,
    )
    .unwrap()
}

/// The node bytes of a bundle, by name.
fn nodes(path: &Path) -> Vec<(String, u32, Vec<u8>)> {
    let b = Bundle::open(path).unwrap();
    b.header
        .nodes
        .iter()
        .enumerate()
        .map(|(i, n)| {
            (
                n.name.clone(),
                n.flags,
                b.node(i).read_vec(0, n.size as usize).unwrap(),
            )
        })
        .collect()
}

/// Shader objects' current bytes by name, in every serialized file of a file.
fn shader_bytes(path: &Path, db: &std::sync::Arc<ClassDb>) -> Vec<(String, String, Vec<u8>)> {
    let mut f = DataFile::open(path, &all(db)).unwrap();
    let mut out = Vec::new();
    for k in 0..f.file_count() {
        f.select(k).unwrap();
        for pid in f.path_ids("Shader") {
            let t = f.sf.read_typetree(pid).unwrap();
            let name = t
                .path(&["m_ParsedForm", "m_Name"])
                .unwrap()
                .as_str()
                .unwrap()
                .to_string();
            out.push((f.sf.name.clone(), name, f.sf.bytes(pid).unwrap()));
        }
    }
    out
}

const SCENE: &str = "BuildPlayer-s";
const SHARED: &str = "BuildPlayer-s.sharedAssets";
/// The scene's external file for its shared assets, as Unity writes it (lower case).
const SHARED_EXT: &str = "archive:/buildplayer-s/buildplayer-s.sharedassets";

/// The made-up game for the shaders step: the same three shaders in resources.assets, in a
/// one-file bundle and in scene bundles (in the shared assets; one also in the scene itself),
/// a scene bundle without shaders, a bundle with no serialized file, files that look like
/// bundles of other kinds.
fn shader_game(db: &ClassDb, data: &Path) {
    let shaders = || {
        vec![
            o(48, 11, madeup::shader(db, "Game/Good", ShaderKind::Good)),
            o(48, 12, madeup::shader(db, "Game/Gather", ShaderKind::Es31)),
            o(
                48,
                13,
                madeup::shader(db, "Game/Ready", ShaderKind::HasGles),
            ),
        ]
    };
    write(
        data,
        "globalgamemanagers",
        &madeup::serialized(db, &[o(141, 1, madeup::build_settings(db))], &[], false),
    );
    write(
        data,
        "resources.assets",
        &madeup::serialized(db, &shaders(), &[], true),
    );
    let one = madeup::serialized(db, &shaders(), &[], true);
    write(
        data,
        "StreamingAssets/b/single",
        &madeup::bundle(&[("CAB-1111", 4, one)], 1 << 12),
    );
    // the scene: a BuildSettings-like object and an input manager (no shaders), pointing at its
    // shared assets
    let scene = madeup::serialized(
        db,
        &[
            o(141, 1, madeup::build_settings(db)),
            o(13, 2, madeup::input_manager(db, &["Horizontal"])),
        ],
        &[SHARED_EXT, "library/unity default resources"],
        true,
    );
    let shared = madeup::serialized(db, &shaders(), &["library/unity default resources"], true);
    write(
        data,
        "StreamingAssets/b/scene",
        &madeup::bundle(
            &[
                (SHARED, 4, shared.clone()),
                (SCENE, 4, scene.clone()),
                ("BuildPlayer-s.sharedAssets.resS", 0, vec![3u8; 5000]),
            ],
            1 << 12,
        ),
    );
    // a scene with a shader of its own too (the scene first in the bundle)
    let scene2 = madeup::serialized(
        db,
        &[o(48, 21, madeup::shader(db, "Game/Good", ShaderKind::Good))],
        &[SHARED_EXT],
        true,
    );
    write(
        data,
        "StreamingAssets/b/scene2",
        &madeup::bundle(&[(SCENE, 4, scene2), (SHARED, 4, shared)], 1 << 17),
    );
    // a scene bundle without shaders
    write(
        data,
        "StreamingAssets/b/scene3",
        &madeup::bundle(&[(SHARED, 4, scene.clone()), (SCENE, 4, scene)], 1 << 17),
    );
    // a bundle with no serialized file; a UnityWeb archive; a UnityFS bundle behind 16 bytes
    write(
        data,
        "StreamingAssets/b/streams",
        &madeup::bundle(&[("x.resS", 0, vec![1u8; 100])], 1 << 17),
    );
    let mut web = b"UnityWeb\0".to_vec();
    web.extend_from_slice(&[0u8; 64]);
    write(data, "StreamingAssets/b/old", &web);
    let mut hid = vec![0x5au8; 16];
    hid.extend_from_slice(&madeup::bundle(&[("CAB-2", 4, vec![0u8; 64])], 1 << 17));
    write(data, "StreamingAssets/b/hidden", &hid);
}

fn shaders_cli(dir: &Path, cfg: &str) -> Cli {
    let c = dir.join("cfg.json");
    std::fs::write(&c, cfg).unwrap();
    Cli {
        data: dir.join("Data").to_string_lossy().into_owned(),
        config: c.to_string_lossy().into_owned(),
        steps: Some("shaders".into()),
        ..Default::default()
    }
}

#[test]
fn shaders_in_scene_bundles() {
    let Some(db) = madeup::db() else {
        eprintln!("PMU_TPK not set: skipped");
        return;
    };
    let dir = temp("shaders");
    let data = dir.join("Data");
    shader_game(&db, &data);
    let before: Vec<_> = ["scene", "scene2", "scene3", "streams", "old", "hidden"]
        .iter()
        .map(|n| std::fs::read(data.join("StreamingAssets/b").join(n)).unwrap())
        .collect();
    let cli = shaders_cli(
        &dir,
        r#"{"steps": ["shaders"], "shaders": {"compute": false}}"#,
    );
    cli::run(&cli).unwrap();

    // every copy of a shader converted to the same bytes as in the plain file and the one-file
    // bundle
    let want: Vec<(String, Vec<u8>)> = shader_bytes(&data.join("resources.assets"), &db)
        .into_iter()
        .map(|(_, n, b)| (n, b))
        .collect();
    assert_eq!(want.len(), 3);
    let single: Vec<(String, Vec<u8>)> = shader_bytes(&data.join("StreamingAssets/b/single"), &db)
        .into_iter()
        .map(|(_, n, b)| (n, b))
        .collect();
    assert_eq!(single, want);
    let orig = sf_of(
        madeup::serialized(
            &db,
            &[o(
                48,
                11,
                madeup::shader(&db, "Game/Good", ShaderKind::Good),
            )],
            &[],
            true,
        ),
        &db,
    );
    assert_ne!(
        want[0].1,
        orig.bytes(11).unwrap(),
        "the shader was converted"
    );
    let scene = shader_bytes(&data.join("StreamingAssets/b/scene"), &db);
    assert_eq!(
        scene
            .iter()
            .map(|(f, n, b)| (f.as_str(), n.clone(), b.clone()))
            .collect::<Vec<_>>(),
        want.iter()
            .map(|(n, b)| (SHARED, n.clone(), b.clone()))
            .collect::<Vec<_>>()
    );
    let scene2 = shader_bytes(&data.join("StreamingAssets/b/scene2"), &db);
    assert_eq!(scene2.len(), 4);
    assert_eq!((scene2[0].0.as_str(), &scene2[0].1), (SCENE, &want[0].0));
    assert_eq!(scene2[0].2, want[0].1, "the scene's own shader too");
    for (k, s) in scene2[1..].iter().enumerate() {
        assert_eq!((s.0.as_str(), &s.1, &s.2), (SHARED, &want[k].0, &want[k].1));
    }

    // the bundle: its nodes, their order and flags; the scene and the stream as they were, the
    // shared assets as resources.assets was saved
    let b0 = {
        let p = dir.join("before-scene");
        std::fs::write(&p, &before[0]).unwrap();
        nodes(&p)
    };
    let b1 = nodes(&data.join("StreamingAssets/b/scene"));
    assert_eq!(
        b1.iter().map(|n| (&n.0, n.1)).collect::<Vec<_>>(),
        b0.iter().map(|n| (&n.0, n.1)).collect::<Vec<_>>()
    );
    assert_ne!(b1[0].2, b0[0].2);
    assert_eq!(b1[1].2, b0[1].2, "the scene without shaders: its bytes");
    assert_eq!(b1[2].2, b0[2].2, "the stream: its bytes");
    let saved = sf_of(b1[0].2.clone(), &db);
    assert_eq!(
        saved.save().unwrap(),
        b1[0].2,
        "the shared assets as a serialized file saves"
    );
    // written as one-file bundles are: chunked LZ4 (UnityPy's packer='lz4'), the header's
    // flags, every block within the chunk size
    let bh = Bundle::open(data.join("StreamingAssets/b/scene")).unwrap();
    let sh = Bundle::open(data.join("StreamingAssets/b/single")).unwrap();
    assert_eq!(bh.header.flags, sh.header.flags);
    assert!(bh
        .header
        .blocks
        .iter()
        .all(|b| b.usize as usize <= unityfs::bundle::CHUNK));
    // the platform patched in place, as android-data does: every serialized file says Android
    let p = data.join("StreamingAssets/b/scene");
    unityfs::platform::patch_bundle_platform(&p, unityfs::platform::ANDROID).unwrap();
    let plats = unityfs::platform::bundle_platforms(&p).unwrap();
    assert_eq!(plats.len(), 2);
    assert!(plats.iter().all(|(_, x)| *x == unityfs::platform::ANDROID));
    // the bundles the step leaves: as they were
    for (k, n) in ["scene3", "streams", "old", "hidden"].iter().enumerate() {
        assert_eq!(
            std::fs::read(data.join("StreamingAssets/b").join(n)).unwrap(),
            before[k + 2],
            "{n}"
        );
    }
    // journaled: every bundle the step opened (the one without a serialized file left as it is)
    let j = journal(&data);
    assert_eq!(
        j.get("done").unwrap().dumps(),
        r#"{"shaders": ["globalgamemanagers", "resources.assets", "StreamingAssets/b/scene", "StreamingAssets/b/scene2", "StreamingAssets/b/scene3", "StreamingAssets/b/single", "StreamingAssets/b/streams"]}"#
    );
    // a second run: nothing to do
    let after = std::fs::read(data.join("StreamingAssets/b/scene2")).unwrap();
    cli::run(&cli).unwrap();
    assert_eq!(
        std::fs::read(data.join("StreamingAssets/b/scene2")).unwrap(),
        after
    );
    let _ = std::fs::remove_dir_all(&dir);
}

/// The textures step's stand-in codec (tests/textures.rs's): DXT textures not readable to "ASTC"
/// (a byte pattern of the name's length), "big" ones at half size; what it is given, by key.
struct Fake {
    seen: Vec<String>,
}

impl Codec for Fake {
    fn choose(&self, t: &Tex) -> Option<Choice> {
        if ![10, 12].contains(&t.format) || t.is_readable {
            return None;
        }
        let half = t.name.starts_with(b"big");
        let (w, h) = if half {
            (t.width / 2, t.height / 2)
        } else {
            (t.width, t.height)
        };
        Some(Choice {
            block: (4, 4),
            width: w,
            height: h,
            alpha: t.format != 10,
            mips: 1,
        })
    }

    fn convert(
        &mut self,
        t: &Tex,
        raw: &[u8],
        ch: &Choice,
        key: &str,
        _unity: Option<&str>,
    ) -> Result<Converted> {
        self.seen.push(format!(
            "{key} {} {}",
            String::from_utf8_lossy(&t.name),
            raw.len()
        ));
        let n = ch.width.div_ceil(4) * ch.height.div_ceil(4) * 16;
        Ok(Converted {
            format: 54,
            width: ch.width,
            height: ch.height,
            mip_count: 1,
            data: vec![t.name.len() as u8; n],
        })
    }

    fn summary(&mut self) -> Vec<String> {
        vec![format!("fake: {} textures", self.seen.len())]
    }

    fn worker(&self) -> Option<Box<dyn Codec + Send>> {
        None
    }
}

/// A scene bundle's textures: the shared assets' (streamed from their .resS: one scaled, one
/// kept), a sprite and an atlas there, a sprite in the scene pointing into the shared assets;
/// the same texture in a one-file bundle; a scene bundle whose two files stream from one node.
fn texture_game(db: &ClassDb, data: &Path) -> (Vec<u8>, Vec<u8>) {
    write(
        data,
        "globalgamemanagers",
        &madeup::serialized(db, &[o(141, 1, madeup::build_settings(db))], &[], false),
    );
    let res: Vec<u8> = (0..4096u32).map(|i| (i * 13 % 251) as u8).collect();
    let spath = format!("archive:/{SCENE}/{SHARED}.resS");
    let shared = madeup::serialized(
        db,
        &[
            o(
                28,
                1,
                madeup::texture(db, "big_wall", (64, 32), 12, &[], Some((0, 2048, &spath))),
            ),
            o(
                28,
                2,
                madeup::texture(db, "floor", (32, 16), 10, &[], Some((2048, 256, &spath))),
            ),
            o(213, 3, madeup::sprite(db, "wall_sprite", 1)),
            o(687078895, 4, madeup::sprite_atlas(db, "atlas", &[1, 2])),
        ],
        &["library/unity default resources"],
        true,
    );
    // the scene: a sprite of the wall (its m_RD.texture in the shared assets: file id 1)
    let mut sp = madeup::sprite(db, "scene_sprite", 1);
    if let Some(Value::Struct(t)) = sp.path_mut(&["m_RD", "texture"]) {
        for (n, v) in t.fields.iter_mut() {
            if &**n == "m_FileID" {
                *v = Value::I32(1);
            }
        }
    }
    let scene = madeup::serialized(
        db,
        &[o(213, 7, sp), o(213, 8, madeup::sprite(db, "own", 1))],
        &[SHARED_EXT],
        true,
    );
    write(
        data,
        "StreamingAssets/AssetBundles/atlas_scene",
        &madeup::bundle(
            &[
                (SHARED, 4, shared),
                (SCENE, 4, scene.clone()),
                (&format!("{SHARED}.resS"), 0, res.clone()),
            ],
            1 << 12,
        ),
    );
    // the same wall in a one-file bundle
    let cab = "CAB-5555";
    let cpath = format!("archive:/{cab}/{cab}.resS");
    let one = madeup::serialized(
        db,
        &[o(
            28,
            1,
            madeup::texture(db, "big_wall", (64, 32), 12, &[], Some((0, 2048, &cpath))),
        )],
        &[],
        true,
    );
    write(
        data,
        "StreamingAssets/AssetBundles/atlas_one",
        &madeup::bundle(
            &[
                (cab, 4, one),
                (&format!("{cab}.resS"), 0, res[..2048].to_vec()),
            ],
            1 << 12,
        ),
    );
    // a scene whose own texture streams from its shared assets' .resS: the step would replace
    // that node for the shared assets' textures, so it leaves the bundle
    let scene_tex = madeup::serialized(
        db,
        &[o(
            28,
            9,
            madeup::texture(
                db,
                "big_scene",
                (32, 32),
                12,
                &[],
                Some((2048, 1024, &spath)),
            ),
        )],
        &[SHARED_EXT],
        true,
    );
    let shared2 = madeup::serialized(
        db,
        &[o(
            28,
            1,
            madeup::texture(db, "big_wall", (64, 32), 12, &[], Some((0, 2048, &spath))),
        )],
        &[],
        true,
    );
    let shares = madeup::bundle(
        &[
            (SHARED, 4, shared2),
            (SCENE, 4, scene_tex),
            (&format!("{SHARED}.resS"), 0, res.clone()),
        ],
        1 << 12,
    );
    write(data, "StreamingAssets/AssetBundles/atlas_shares", &shares);
    (scene, shares)
}

fn f32_at(v: &Value, path: &[&str]) -> f32 {
    match v.path(path).unwrap() {
        Value::F32(x) => *x,
        other => panic!("{path:?}: {other:?}"),
    }
}

#[test]
fn textures_in_scene_bundles() {
    let Some(db) = madeup::db() else {
        eprintln!("PMU_TPK not set: skipped");
        return;
    };
    let dir = temp("textures");
    let data = dir.join("Data");
    let (scene, shares) = texture_game(&db, &data);
    let cfg = pyjson::loads(
        r#"{"version": 1, "textures": {"files": "^StreamingAssets/AssetBundles/atlas_"}}"#,
    )
    .unwrap();
    let mut job = Job::new(&data, &data, cfg).unwrap();
    textures::run_with(
        &mut job,
        &Args::default(),
        Box::new(Fake { seen: Vec::new() }),
    )
    .unwrap();
    job.flush().unwrap();
    let j = journal(&data);
    assert_eq!(
        j.get("done").unwrap().dumps(),
        r#"{"textures": ["StreamingAssets/AssetBundles/atlas_one", "StreamingAssets/AssetBundles/atlas_scene", "StreamingAssets/AssetBundles/atlas_shares"]}"#
    );
    assert_eq!(
        j.get("scaled").unwrap().dumps(),
        r#"{"StreamingAssets/AssetBundles/atlas_one": {"1": [64, 32, 32, 16]}, "StreamingAssets/AssetBundles/atlas_scene:BuildPlayer-s.sharedAssets": {"1": [64, 32, 32, 16]}}"#
    );
    assert!(!data.join(".pmunity-work").exists());

    // the wall: the same fields and pixels as in the one-file bundle
    let mut f = DataFile::open(
        data.join("StreamingAssets/AssetBundles/atlas_scene"),
        &all(&db),
    )
    .unwrap();
    let g = DataFile::open(
        data.join("StreamingAssets/AssetBundles/atlas_one"),
        &all(&db),
    )
    .unwrap();
    let wall = f.sf.read_typetree(1).unwrap();
    let one = g.sf.read_typetree(1).unwrap();
    for k in [
        "m_Width",
        "m_Height",
        "m_TextureFormat",
        "m_CompleteImageSize",
    ] {
        assert_eq!(wall.get(k), one.get(k), "{k}");
    }
    let px = |f: &DataFile, t: &Value| {
        let sd = t.get("m_StreamData").unwrap();
        f.read_node(
            sd.get("path").unwrap().as_str().unwrap(),
            sd.get("offset").unwrap().as_u64().unwrap(),
            sd.get("size").unwrap().as_u64().unwrap() as usize,
        )
        .unwrap()
    };
    assert_eq!(px(&f, &wall), px(&g, &one));
    assert_eq!(wall.get("m_Width").unwrap().as_int(), Some(32));
    // the floor, converted at its size, after the wall in the new stream
    let floor = f.sf.read_typetree(2).unwrap();
    assert_eq!(floor.get("m_TextureFormat").unwrap().as_int(), Some(54));
    assert_eq!(px(&f, &floor), vec![5u8; 512]);
    // the shared assets' sprite and atlas follow the wall
    assert_eq!(
        f32_at(&f.sf.read_typetree(3).unwrap(), &["m_Rect", "width"]),
        50.5
    );
    let a = f.sf.read_typetree(4).unwrap();
    let m: Vec<f32> = a
        .get("m_RenderDataMap")
        .unwrap()
        .as_array()
        .unwrap()
        .iter()
        .map(|e| match e {
            Value::Pair(p) => f32_at(&p.1, &["downscaleMultiplier"]),
            _ => unreachable!(),
        })
        .collect();
    assert_eq!(m, [0.5, 0.75]);
    // the scene's sprite of the wall (a PPtr into the shared assets) follows it too; the one
    // whose PPtr names the scene itself (path id 1 there is no texture) keeps its rectangle
    f.select(1).unwrap();
    assert_eq!(
        f32_at(&f.sf.read_typetree(7).unwrap(), &["m_Rect", "width"]),
        50.5
    );
    assert_eq!(
        f32_at(&f.sf.read_typetree(8).unwrap(), &["m_Rect", "width"]),
        101.0
    );
    assert_ne!(
        nodes(&data.join("StreamingAssets/AssetBundles/atlas_scene"))[1].2,
        scene,
        "the scene changed (its sprite)"
    );
    // the bundle whose files share a stream: left as it is
    assert_eq!(
        std::fs::read(data.join("StreamingAssets/AssetBundles/atlas_shares")).unwrap(),
        shares
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn edits_in_scene_bundles() {
    let Some(db) = madeup::db() else {
        eprintln!("PMU_TPK not set: skipped");
        return;
    };
    let dir = temp("edits");
    let data = dir.join("Data");
    shader_game(&db, &data);
    let plugin = dir.join("edits.json");
    std::fs::write(
        &plugin,
        r#"{"steps": {"names": [{"file": "StreamingAssets/b/scene*", "class": "Shader", "name": "Game/G*",
                                 "set": {"m_ParsedForm.m_Name": "Game/Edited"}},
                                {"file": "StreamingAssets/b/scene", "class": "InputManager",
                                 "set": {"m_Axes.0.sensitivity": 2.5}}]}}"#,
    )
    .unwrap();
    let c = dir.join("cfg.json");
    std::fs::write(&c, r#"{"steps": []}"#).unwrap();
    let cli = Cli {
        data: data.to_string_lossy().into_owned(),
        config: c.to_string_lossy().into_owned(),
        steps: Some("names".into()),
        plugins: vec![plugin.to_string_lossy().into_owned()],
        ..Default::default()
    };
    let scene3 = std::fs::read(data.join("StreamingAssets/b/scene3")).unwrap();
    cli::run(&cli).unwrap();
    let names: Vec<(String, String)> = shader_bytes(&data.join("StreamingAssets/b/scene2"), &db)
        .into_iter()
        .map(|(f, n, _)| (f, n))
        .collect();
    assert_eq!(
        names,
        [
            (SCENE, "Game/Edited"),
            (SHARED, "Game/Edited"),
            (SHARED, "Game/Edited"),
            (SHARED, "Game/Ready"),
        ]
        .map(|(a, b)| (a.to_string(), b.to_string()))
    );
    let mut f = DataFile::open(data.join("StreamingAssets/b/scene"), &all(&db)).unwrap();
    f.select(1).unwrap();
    let im = f.sf.read_typetree(2).unwrap();
    assert_eq!(
        im.path(&["m_Axes"]).unwrap().as_array().unwrap()[0].get("sensitivity"),
        Some(&Value::F32(2.5))
    );
    assert_eq!(
        std::fs::read(data.join("StreamingAssets/b/scene3")).unwrap(),
        scene3,
        "nothing to edit there"
    );
    let j = journal(&data);
    assert_eq!(
        j.get("done").unwrap().dumps(),
        r#"{"names": ["StreamingAssets/b/scene", "StreamingAssets/b/scene2", "StreamingAssets/b/scene3"]}"#
    );
    let _ = std::fs::remove_dir_all(&dir);
}
