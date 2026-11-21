//! The textures step's Unity-file layer on made-up data with a stand-in codec (texcodec does the
//! real per-texture work): which textures it is given, where their pixels go (a new 16-byte
//! aligned stream, inline, a bundle's node; inline_streams), what follows a scaled texture
//! (sprites, atlas entries), the journal's `scaled` and the files committed together. Needs
//! $PMU_TPK (skipped without).
mod madeup;

use convert::pyjson::{self, Json};
use convert::steps::textures::{self, Choice, Codec, Converted, Tex};
use convert::steps::Args;
use convert::{Job, Result};
use madeup::Obj;
use std::path::PathBuf;
use unityfs::{DataFile, Options, Value};

/// Converts DXT1/DXT5/RGBA32 textures that are not readable: "big" ones at half size, each
/// texture's blocks a byte pattern of its name's length (16 bytes a 4x4 block).
struct Fake {
    seen: Vec<String>,
    /// whether the step may convert several textures at once (worker codecs)
    workers: bool,
}

impl Codec for Fake {
    fn choose(&self, t: &Tex) -> Option<Choice> {
        if ![10, 12, 4].contains(&t.format) || t.is_readable {
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
        unity: Option<&str>,
    ) -> Result<Converted> {
        assert_eq!(unity, Some("2017.4.9f1"));
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
        self.workers.then(|| {
            Box::new(Fake {
                seen: Vec::new(),
                workers: false,
            }) as Box<dyn Codec + Send>
        })
    }
}

fn temp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!(
        "pmu-textures-{}-{name}-{:?}",
        std::process::id(),
        std::thread::current().id()
    ));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).unwrap();
    d
}

fn f32_of(v: &Value, path: &[&str]) -> f32 {
    match v.path(path).unwrap() {
        Value::F32(x) => *x,
        other => panic!("{path:?}: {other:?}"),
    }
}

#[test]
fn textures_layer_with_a_stand_in_codec() {
    textures_layer(false);
}

/// The same with the file's small textures converted on workers: the same files.
#[test]
fn textures_layer_with_workers() {
    textures_layer(true);
}

fn textures_layer(workers: bool) {
    let Some(db) = madeup::db() else {
        eprintln!("PMU_TPK not set: skipped");
        return;
    };
    let dir = temp("layer");
    let data = dir.join("Data");
    std::fs::create_dir_all(data.join("StreamingAssets/AssetBundles")).unwrap();
    let o = |class_id, path_id, value| Obj {
        class_id,
        path_id,
        value,
    };
    // a scene's assets: streamed textures (one scaled, one readable: never given to the codec,
    // its pixels copied along), an inline one, sprites of a scaled and an unscaled texture, an
    // atlas with entries for both
    let stream: Vec<u8> = (0..3000u32).map(|i| (i * 7) as u8).collect();
    let mut t2 = madeup::texture(
        &db,
        "readable",
        (16, 16),
        4,
        &[],
        Some((1024, 1024, "sharedassets0.assets.resS")),
    );
    t2.set("m_IsReadable", Value::Bool(true)).unwrap();
    let file = madeup::serialized(
        &db,
        &[
            o(
                28,
                1,
                madeup::texture(
                    &db,
                    "big_atlas",
                    (64, 32),
                    12,
                    &[],
                    Some((0, 1024, "sharedassets0.assets.resS")),
                ),
            ),
            o(28, 2, t2),
            o(
                28,
                3,
                madeup::texture(
                    &db,
                    "icon",
                    (16, 8),
                    10,
                    &[],
                    Some((2048, 64, "sharedassets0.assets.resS")),
                ),
            ),
            o(
                28,
                4,
                madeup::texture(&db, "inline_tex", (8, 8), 4, &[9u8; 256], None),
            ),
            o(213, 5, madeup::sprite(&db, "big_sprite", 1)),
            o(213, 6, madeup::sprite(&db, "icon_sprite", 3)),
            o(687078895, 7, madeup::sprite_atlas(&db, "atlas", &[1, 3, 1])),
        ],
        &[],
        true,
    );
    std::fs::write(data.join("sharedassets0.assets"), &file).unwrap();
    std::fs::write(data.join("sharedassets0.assets.resS"), &stream).unwrap();
    // a bundle with a streamed texture (written inline: inline_streams)
    let cab = "CAB-00112233445566778899aabbccddeeff";
    let path = format!("archive:/{cab}/{cab}.resS");
    let bf = madeup::serialized(
        &db,
        &[o(
            28,
            1,
            madeup::texture(&db, "big_tiles", (32, 32), 12, &[], Some((0, 1024, &path))),
        )],
        &[],
        true,
    );
    std::fs::write(
        data.join("StreamingAssets/AssetBundles/atlas_tiles"),
        madeup::bundle(
            &[(cab, 4, bf), (&format!("{cab}.resS"), 0, vec![5u8; 1024])],
            1 << 12,
        ),
    )
    .unwrap();
    // globalgamemanagers for the data's version; a file the regex leaves out
    std::fs::write(
        data.join("globalgamemanagers"),
        madeup::serialized(&db, &[o(141, 1, madeup::build_settings(&db))], &[], false),
    )
    .unwrap();

    let cfg = pyjson::loads(
        r#"{"version": 1, "textures": {"inline": true, "inline_streams": true,
            "files": "^StreamingAssets/AssetBundles/(atlas_|tex_)|^[^/]*\\.assets$"}}"#,
    )
    .unwrap();
    let mut job = Job::new(&data, &data, cfg).unwrap();
    textures::run_with(
        &mut job,
        &Args::default(),
        Box::new(Fake {
            seen: Vec::new(),
            workers,
        }),
    )
    .unwrap();

    // the journal: both files done, the scaled textures' sizes in the order they were stored
    let j: Json =
        pyjson::loads(&std::fs::read_to_string(data.join(".pmunity.json")).unwrap()).unwrap();
    assert_eq!(
        j.get("done").unwrap().dumps(),
        r#"{"textures": ["sharedassets0.assets", "StreamingAssets/AssetBundles/atlas_tiles"]}"#
    );
    assert_eq!(
        j.get("scaled").unwrap().dumps(),
        r#"{"sharedassets0.assets": {"1": [64, 32, 32, 16]}, "StreamingAssets/AssetBundles/atlas_tiles": {"1": [32, 32, 16, 16]}}"#
    );
    assert!(!data.join(".pmunity-work").exists());

    let opts = Options {
        db: Some(db.clone()),
        ..Default::default()
    };
    let f = DataFile::open(data.join("sharedassets0.assets"), &opts).unwrap();
    let res = std::fs::read(data.join("sharedassets0.assets.resS")).unwrap();
    let tex = |pid| f.sf.read_typetree(pid).unwrap();
    let sd = |t: &Value| {
        let s = t.get("m_StreamData").unwrap();
        (
            s.get("offset").unwrap().as_u64().unwrap() as usize,
            s.get("size").unwrap().as_u64().unwrap() as usize,
        )
    };
    // the new stream: big_atlas's blocks (32 x 16: 32 blocks), readable's old pixels, icon's blocks
    let t1 = tex(1);
    assert_eq!(
        (
            t1.get("m_Width").unwrap().as_int(),
            t1.get("m_Height").unwrap().as_int()
        ),
        (Some(32), Some(16))
    );
    assert_eq!(t1.get("m_TextureFormat").unwrap().as_int(), Some(54));
    assert_eq!(t1.get("m_CompleteImageSize").unwrap().as_int(), Some(512));
    assert_eq!(sd(&t1), (0, 512));
    assert!(res[..512].iter().all(|&b| b == 9));
    let t2 = tex(2);
    assert_eq!(sd(&t2), (512, 1024));
    assert_eq!(&res[512..1536], &stream[1024..2048]);
    let t3 = tex(3);
    assert_eq!(sd(&t3), (1536, 128));
    assert!(res[1536..1664].iter().all(|&b| b == 4));
    assert_eq!(res.len(), 1664);
    let t4 = tex(4);
    assert_eq!(
        t4.get("image data").unwrap().as_bytes().unwrap(),
        &[10u8; 64][..]
    );
    // the sprite of the scaled texture follows it; the other keeps its rectangle
    let s5 = tex(5);
    assert_eq!(f32_of(&s5, &["m_Rect", "width"]), 50.5);
    assert_eq!(f32_of(&s5, &["m_Rect", "height"]), 38.5);
    assert_eq!(f32_of(&s5, &["m_PixelsToUnits"]), 50.0);
    assert_eq!(
        f32_of(&s5, &["m_Border", "w"]),
        (4.1f32 as f64 * 0.5) as f32
    );
    assert_eq!(f32_of(&s5, &["m_RD", "atlasRectOffset", "x"]), -1.0);
    assert_eq!(
        f32_of(&s5, &["m_RD", "uvTransform", "y"]),
        (53.3f32 as f64 * 0.5) as f32
    );
    assert_eq!(f32_of(&tex(6), &["m_Rect", "width"]), 101.0);
    // the atlas's entries for the scaled texture get its multiplier
    let a = tex(7);
    let m: Vec<f32> = a
        .get("m_RenderDataMap")
        .unwrap()
        .as_array()
        .unwrap()
        .iter()
        .map(|e| match e {
            Value::Pair(p) => f32_of(&p.1, &["downscaleMultiplier"]),
            _ => unreachable!(),
        })
        .collect();
    assert_eq!(m, [0.5, 0.75, 0.375]);
    // the bundle: its texture inline, its .resS node gone
    let b = DataFile::open(data.join("StreamingAssets/AssetBundles/atlas_tiles"), &opts).unwrap();
    assert_eq!(b.node_names(), [cab]);
    let bt = b.sf.read_typetree(1).unwrap();
    assert_eq!(bt.get("image data").unwrap().as_bytes().unwrap().len(), 256);
    assert_eq!(
        bt.get("m_StreamData")
            .unwrap()
            .get("size")
            .unwrap()
            .as_int(),
        Some(0)
    );
    let _ = std::fs::remove_dir_all(&dir);
}

/// A sprite sheet's sprites and atlas entries in another file than the sheet (a PPtr with
/// m_FileID > 0, through the file's externals) follow the scaled sheet as the sheet's own file's
/// do; the file is marked with its change (textures-refs), and a second run leaves it alone.
#[test]
fn textures_sprites_in_another_file() {
    let Some(db) = madeup::db() else {
        eprintln!("PMU_TPK not set: skipped");
        return;
    };
    let dir = temp("crossfile");
    let data = dir.join("Data");
    std::fs::create_dir_all(&data).unwrap();
    let o = |class_id, path_id, value| Obj {
        class_id,
        path_id,
        value,
    };
    // the sheets' file: a scaled sheet with a sprite of its own, and one that keeps its size
    let sheets = madeup::serialized(
        &db,
        &[
            o(
                28,
                1,
                madeup::texture(&db, "big_sheet", (64, 32), 12, &[7u8; 2048], None),
            ),
            o(
                28,
                2,
                madeup::texture(&db, "icon", (16, 8), 10, &[7u8; 64], None),
            ),
            o(213, 3, madeup::sprite(&db, "own", 1)),
        ],
        &[],
        true,
    );
    std::fs::write(data.join("sharedassets0.assets"), &sheets).unwrap();
    // another file: sprites of both sheets (the second external is the sheets' file), a sprite of
    // a texture with the same path id in a file that is not scaled, an atlas with entries for all
    let there = |name, file, pid| {
        let mut s = madeup::sprite(&db, name, pid);
        madeup::set(&mut s, &["m_RD", "texture"], madeup::pptr(file, pid));
        s
    };
    let mut atlas = madeup::sprite_atlas(&db, "atlas", &[1, 2, 1]);
    for e in atlas
        .get_mut("m_RenderDataMap")
        .unwrap()
        .as_array_mut()
        .unwrap()
    {
        let Value::Pair(p) = e else { unreachable!() };
        let pid = p.1.path(&["texture", "m_PathID"]).unwrap().as_int().unwrap();
        p.1.set("texture", madeup::pptr(2, pid as i64)).unwrap();
    }
    let users = madeup::serialized(
        &db,
        &[
            o(213, 1, there("of_big_sheet", 2, 1)),
            o(213, 2, there("of_icon", 2, 2)),
            o(213, 3, there("of_another_file", 1, 1)),
            o(687078895, 4, atlas),
        ],
        &["globalgamemanagers.assets", "sharedassets0.assets"],
        true,
    );
    std::fs::write(data.join("sharedassets1.assets"), &users).unwrap();
    std::fs::write(
        data.join("globalgamemanagers"),
        madeup::serialized(&db, &[o(141, 1, madeup::build_settings(&db))], &[], false),
    )
    .unwrap();

    let cfg = || pyjson::loads(r#"{"version": 1, "textures": {"inline": true}}"#).unwrap();
    let run = || {
        let mut job = Job::new(&data, &data, cfg()).unwrap();
        textures::run_with(
            &mut job,
            &Args::default(),
            Box::new(Fake {
                seen: Vec::new(),
                workers: false,
            }),
        )
        .unwrap();
    };
    run();
    let journal =
        || pyjson::loads(&std::fs::read_to_string(data.join(".pmunity.json")).unwrap()).unwrap();
    assert_eq!(
        journal().get("done").unwrap().dumps(),
        r#"{"textures": ["globalgamemanagers", "sharedassets0.assets", "sharedassets1.assets"], "textures-refs": ["sharedassets1.assets"]}"#
    );
    assert!(journal().get("once").is_none());
    let opts = Options {
        db: Some(db.clone()),
        ..Default::default()
    };
    let check = || {
        let f0 = DataFile::open(data.join("sharedassets0.assets"), &opts).unwrap();
        assert_eq!(
            f32_of(&f0.sf.read_typetree(3).unwrap(), &["m_Rect", "width"]),
            50.5
        );
        let f = DataFile::open(data.join("sharedassets1.assets"), &opts).unwrap();
        let tex = |pid| f.sf.read_typetree(pid).unwrap();
        // the scaled sheet's sprite in the other file: as the sheet's own (textures_layer)
        let s = tex(1);
        assert_eq!(f32_of(&s, &["m_Rect", "width"]), 50.5);
        assert_eq!(f32_of(&s, &["m_Rect", "height"]), 38.5);
        assert_eq!(f32_of(&s, &["m_PixelsToUnits"]), 50.0);
        assert_eq!(f32_of(&s, &["m_RD", "atlasRectOffset", "x"]), -1.0);
        assert_eq!(
            f32_of(&s, &["m_RD", "uvTransform", "y"]),
            (53.3f32 as f64 * 0.5) as f32
        );
        // the sheet that kept its size, and path id 1 of a file with no scaled texture: untouched
        assert_eq!(f32_of(&tex(2), &["m_Rect", "width"]), 101.0);
        assert_eq!(f32_of(&tex(3), &["m_Rect", "width"]), 101.0);
        let a = tex(4);
        let m: Vec<f32> = a
            .get("m_RenderDataMap")
            .unwrap()
            .as_array()
            .unwrap()
            .iter()
            .map(|e| match e {
                Value::Pair(p) => f32_of(&p.1, &["downscaleMultiplier"]),
                _ => unreachable!(),
            })
            .collect();
        assert_eq!(m, [0.5, 0.75, 0.375]);
    };
    check();
    // a second run (a setup started again): nothing is scaled twice
    run();
    check();
    let _ = std::fs::remove_dir_all(&dir);
}

/// `readable`: keep leaves readable textures as they are (and a file of only those unwritten);
/// release clears their m_IsReadable and keeps their pixels (streamed and inline, the file written
/// though nothing was converted); convert clears it and gives them to the codec as the others;
/// `readable_keep` leaves the textures it names readable and as they are either way.
#[test]
fn textures_readable_modes() {
    let Some(db) = madeup::db() else {
        eprintln!("PMU_TPK not set: skipped");
        return;
    };
    for mode in ["keep", "release", "convert"] {
        let dir = temp(&format!("readable-{mode}"));
        let data = dir.join("Data");
        std::fs::create_dir_all(&data).unwrap();
        let stream: Vec<u8> = (0..1024u32).map(|i| (i * 13) as u8).collect();
        let mut t1 = madeup::texture(
            &db,
            "atlas",
            (16, 16),
            4,
            &[],
            Some((0, 1024, "sharedassets0.assets.resS")),
        );
        t1.set("m_IsReadable", Value::Bool(true)).unwrap();
        let mut t2 = madeup::texture(&db, "small", (8, 8), 4, &[7u8; 256], None);
        t2.set("m_IsReadable", Value::Bool(true)).unwrap();
        let o = |path_id, value| Obj {
            class_id: 28,
            path_id,
            value,
        };
        let mut t3 = madeup::texture(&db, "scripted_src", (8, 8), 4, &[3u8; 256], None);
        t3.set("m_IsReadable", Value::Bool(true)).unwrap();
        let file = madeup::serialized(&db, &[o(1, t1), o(2, t2), o(3, t3)], &[], true);
        std::fs::write(data.join("sharedassets0.assets"), &file).unwrap();
        std::fs::write(data.join("sharedassets0.assets.resS"), &stream).unwrap();
        std::fs::write(
            data.join("globalgamemanagers"),
            madeup::serialized(
                &db,
                &[Obj {
                    class_id: 141,
                    path_id: 1,
                    value: madeup::build_settings(&db),
                }],
                &[],
                false,
            ),
        )
        .unwrap();
        let cfg = pyjson::loads(&format!(
            r#"{{"version": 1, "textures": {{"inline": true, "readable": "{mode}", "readable_keep": "^scripted_", "files": "^[^/]*\\.assets$"}}}}"#
        ))
        .unwrap();
        let mut job = Job::new(&data, &data, cfg).unwrap();
        let fake = Fake {
            seen: Vec::new(),
            workers: false,
        };
        textures::run_with(&mut job, &Args::default(), Box::new(fake)).unwrap();
        let opts = Options {
            db: Some(db.clone()),
            ..Default::default()
        };
        let f = DataFile::open(data.join("sharedassets0.assets"), &opts).unwrap();
        let res = std::fs::read(data.join("sharedassets0.assets.resS")).unwrap();
        let (a, s) = (
            f.sf.read_typetree(1).unwrap(),
            f.sf.read_typetree(2).unwrap(),
        );
        let kept = f.sf.read_typetree(3).unwrap();
        let readable = |t: &Value| t.get("m_IsReadable").unwrap().as_bool().unwrap();
        let format = |t: &Value| t.get("m_TextureFormat").unwrap().as_int().unwrap();
        assert!(kept.get("m_IsReadable").unwrap().as_bool().unwrap());
        assert_eq!(kept.get("m_TextureFormat").unwrap().as_int(), Some(4));
        assert_eq!(
            kept.get("image data").unwrap().as_bytes().unwrap(),
            &[3u8; 256][..]
        );
        match mode {
            "keep" => {
                assert!(readable(&a) && readable(&s));
                assert_eq!(
                    std::fs::read(data.join("sharedassets0.assets")).unwrap(),
                    file
                );
                assert_eq!(res, stream);
            }
            "release" => {
                assert!(!readable(&a) && !readable(&s));
                assert_eq!((format(&a), format(&s)), (4, 4));
                assert_eq!(res, stream);
                assert_eq!(
                    s.get("image data").unwrap().as_bytes().unwrap(),
                    &[7u8; 256][..]
                );
            }
            _ => {
                assert!(!readable(&a) && !readable(&s));
                assert_eq!((format(&a), format(&s)), (54, 54));
            }
        }
        let _ = std::fs::remove_dir_all(&dir);
    }
}

/// `exact_shaders` / `exact_keywords` / `exact_names`: the textures that materials of a palette shader
/// sample (a SpriteRenderer's sprite, the sprite its parent's Animator sets on it through a clip of a
/// controller in another file) and the named ones are left as they are by the textures step, the others
/// converted; then `exact_index = "blue"` (step exactindex) stores the palette ones as R8 and rewrites
/// the PALETTE_ON program.
#[test]
fn textures_exact_shaders() {
    let Some(db) = madeup::db() else {
        eprintln!("PMU_TPK not set: skipped");
        return;
    };
    use madeup::{element, object, pptr, set, tree};
    let dir = temp("exact");
    let data = dir.join("Data");
    std::fs::create_dir_all(&data).unwrap();
    let o = |class_id, path_id, value| Obj {
        class_id,
        path_id,
        value,
    };
    // 8x8 RGBA32: blue = an index (2 + the texel's number), alpha 0 or 255, red and green anything
    let pal_px: Vec<u8> = (0..64u32)
        .flat_map(|i| {
            [
                (i * 7) as u8,
                (i * 3) as u8,
                2 + i as u8,
                if i % 5 == 0 { 0 } else { 255 },
            ]
        })
        .collect();
    let tex = |name: &str, px: &[u8]| madeup::texture(&db, name, (8, 8), 4, px, None);
    let material = |shader_pid: i64, kw: &str| {
        let mut v = object(&db, 21);
        set(&mut v, &["m_Shader"], pptr(0, shader_pid));
        set(&mut v, &["m_ShaderKeywords"], Value::str(kw));
        v
    };
    let renderer = |go: i64, mat: i64, sprite: i64| {
        let mut v = object(&db, 212);
        set(&mut v, &["m_GameObject"], pptr(0, go));
        set(&mut v, &["m_Materials"], Value::Array(vec![pptr(0, mat)]));
        set(&mut v, &["m_Sprite"], pptr(0, sprite));
        v
    };
    let gt = tree(&db, 1);
    let game_object = |name: &str, comps: &[i64]| {
        let mut v = object(&db, 1);
        set(&mut v, &["m_Name"], Value::str(name));
        let list = comps
            .iter()
            .map(|&c| {
                let mut e = element(&gt, &["m_Component"]);
                e.set("component", pptr(0, c)).unwrap();
                e
            })
            .collect();
        set(&mut v, &["m_Component"], Value::Array(list));
        v
    };
    let transform = |go: i64, father: i64, children: &[i64]| {
        let mut v = object(&db, 4);
        set(&mut v, &["m_GameObject"], pptr(0, go));
        set(&mut v, &["m_Father"], pptr(0, father));
        set(
            &mut v,
            &["m_Children"],
            Value::Array(children.iter().map(|&c| pptr(0, c)).collect()),
        );
        v
    };
    let mut animator = object(&db, 95);
    set(&mut animator, &["m_Controller"], pptr(1, 2));
    // the parent (GameObject 30: Transform 31, Animator 32) and the child "Body" drawn with the palette
    // material (GameObject 40: Transform 41, SpriteRenderer 42 with sprite 51 of texture 61); sprite 52
    // (texture 62) only in the parent's clip, on "Body"; sprite 53 (texture 63) on a renderer of the
    // same shader without PALETTE_ON; texture 64 by name; texture 65 nobody's
    let objs = vec![
        o(
            48,
            10,
            madeup::palette_shader(&db, "Maikel/MaikelSpriteMasterShader"),
        ),
        o(21, 20, material(10, "ETC1_EXTERNAL_ALPHA PALETTE_ON")),
        o(21, 21, material(10, "ETC1_EXTERNAL_ALPHA")),
        o(1, 30, game_object("Penitent", &[31, 32])),
        o(4, 31, transform(30, 0, &[41])),
        o(95, 32, animator),
        o(1, 40, game_object("Body", &[41, 42])),
        o(4, 41, transform(40, 31, &[])),
        o(212, 42, renderer(40, 20, 51)),
        o(1, 43, game_object("Prop", &[44, 45])),
        o(4, 44, transform(43, 0, &[])),
        o(212, 45, renderer(43, 21, 53)),
        o(213, 51, madeup::sprite(&db, "body", 61)),
        o(213, 52, madeup::sprite(&db, "attack", 62)),
        o(213, 53, madeup::sprite(&db, "prop", 63)),
        o(28, 61, tex("penitent_body", &pal_px)),
        o(28, 62, tex("penitent_attack", &pal_px)),
        o(28, 63, tex("prop", &[9u8; 256])),
        o(28, 64, tex("byhand_lut", &[9u8; 256])),
        o(28, 65, tex("other", &[9u8; 256])),
    ];
    std::fs::write(
        data.join("sharedassets0.assets"),
        madeup::serialized(&db, &objs, &["sharedassets1.assets"], false),
    )
    .unwrap();
    let ct = tree(&db, 74);
    let mut clip = madeup::animation_clip(&db, "attack", &[], 0);
    let mut b = element(&ct, &["m_ClipBindingConstant", "genericBindings"]);
    b.set(
        "path",
        Value::U32(convert::steps::exactindex::crc32(b"Body")),
    )
    .unwrap();
    b.set("typeID", Value::I32(212)).unwrap();
    b.set("isPPtrCurve", Value::U8(1)).unwrap();
    set(
        &mut clip,
        &["m_ClipBindingConstant", "genericBindings"],
        Value::Array(vec![b]),
    );
    set(
        &mut clip,
        &["m_ClipBindingConstant", "pptrCurveMapping"],
        Value::Array(vec![pptr(1, 51), pptr(1, 52)]),
    );
    let mut ctrl = object(&db, 91);
    set(
        &mut ctrl,
        &["m_AnimationClips"],
        Value::Array(vec![pptr(0, 3)]),
    );
    std::fs::write(
        data.join("sharedassets1.assets"),
        madeup::serialized(
            &db,
            &[o(91, 2, ctrl), o(74, 3, clip)],
            &["sharedassets0.assets"],
            false,
        ),
    )
    .unwrap();
    std::fs::write(
        data.join("globalgamemanagers"),
        madeup::serialized(&db, &[o(141, 1, madeup::build_settings(&db))], &[], false),
    )
    .unwrap();
    let cfg = pyjson::loads(
        r#"{"version": 1, "textures": {"inline": true, "exact_shaders": "^Maikel/MaikelSpriteMasterShader", "exact_keywords": "PALETTE_ON", "exact_names": "_lut$", "exact_index": "blue", "files": "^[^/]*\\.assets$"}}"#,
    )
    .unwrap();
    let mut job = Job::new(&data, &data, cfg).unwrap();
    let fake = Fake {
        seen: Vec::new(),
        workers: false,
    };
    textures::run_with(&mut job, &Args::default(), Box::new(fake)).unwrap();
    let opts = Options {
        db: Some(db.clone()),
        ..Default::default()
    };
    let format_of = |f: &DataFile, pid| {
        f.sf.read_typetree(pid)
            .unwrap()
            .get("m_TextureFormat")
            .unwrap()
            .as_int()
            .unwrap()
    };
    let f = DataFile::open(data.join("sharedassets0.assets"), &opts).unwrap();
    assert_eq!(
        [61, 62, 63, 64, 65].map(|p| format_of(&f, p)),
        [4, 4, 54, 4, 54],
        "kept: the palette renderer's sprite, the clip's sprite on it, the named one"
    );
    drop(f);
    convert::steps::exactindex::run(&mut job).unwrap();
    let f = DataFile::open(data.join("sharedassets0.assets"), &opts).unwrap();
    assert_eq!([61, 62, 64].map(|p| format_of(&f, p)), [63, 63, 4]);
    // the index where the alpha is 255, the reserved index (1: no opaque texel uses it) where it is 0
    let want: Vec<u8> = pal_px
        .chunks(4)
        .map(|p| if p[3] == 0 { 1 } else { p[2] })
        .collect();
    let t = f.sf.read_typetree(61).unwrap();
    assert_eq!(t.get("image data").unwrap().as_bytes().unwrap(), &want[..]);
    let sh = f.sf.read_typetree(10).unwrap();
    let blobs = shaderx::tree::blobs(&sh).unwrap();
    let entries =
        shaderx::tree::platform_entries(&sh, shaderx::blob::PLATFORM_GLES3PLUS, &blobs).unwrap();
    let codes: Vec<String> = entries
        .iter()
        .flatten()
        .filter_map(|e| match e {
            shaderx::blob::Slot::Program(p) => Some(String::from_utf8_lossy(&p.code).into_owned()),
            _ => None,
        })
        .collect();
    assert_eq!(
        codes
            .iter()
            .filter(|c| c.contains("u_xlat0.z == 0.0 && u_xlat0.w == 0.0"))
            .count(),
        1
    );
    assert!(codes
        .iter()
        .any(|c| !c.contains("u_xlat0.z == 0.0") && !c.contains(".wxyz")));
}
