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
}

fn temp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("pmu-textures-{}-{name}", std::process::id()));
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
        Box::new(Fake { seen: Vec::new() }),
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
