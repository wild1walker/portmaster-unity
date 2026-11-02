//! A small texture through the whole textures step: choose, decode, scale, mipmaps, ASTC
//! encoding (search and guided, the footprints the config allows), and the strips path's rows.
#![no_main]
use libfuzzer_sys::fuzz_target;
use pmu_fuzz::Input;
use texcodec::decode::tf;
use texcodec::texture::{choose, convert, encoder, strip_blocks, Settings, Texture};

const FORMATS: [i32; 8] = [
    tf::ALPHA8,
    tf::RGB24,
    tf::RGBA32,
    tf::DXT1,
    tf::DXT5,
    tf::BC4,
    tf::BC5,
    tf::BC7,
];
const BLOCKS: [u64; 6] = [4, 5, 6, 8, 10, 12];

fuzz_target!(|data: &[u8]| {
    let mut inp = Input::new(data);
    let sel = inp.u8();
    let (w, h) = (1 + inp.u8() as usize % 32, 1 + inp.u8() as usize % 32);
    let format = FORMATS[sel as usize % FORMATS.len()];
    let b = BLOCKS[inp.u8() as usize % BLOCKS.len()];
    let scale = [1.0, 0.5, 0.375][inp.u8() as usize % 3];
    let mode = if sel & 0x80 != 0 { "search" } else { "guided" };
    let mips = inp.u8() as i64 % 6;
    let raw = inp.rest();
    let cfg = serde_json::json!({
        "scale": scale, "block": [b, b], "quality": "fastest", "mode": mode, "threads": 1,
        "min_side": 1, "keep_max": 1, "min_convert": 1, "mips": "keep", "prepare_split": false
    });
    let Ok(s) = Settings::from_config(Some(&cfg)) else {
        return;
    };
    let Ok(mut enc) = encoder(&s) else {
        return;
    };
    let t = Texture {
        name: "tex",
        width: w,
        height: h,
        format,
        mip_count: Some(mips),
        is_readable: false,
        dimension: Some(2),
        image_count: Some(1),
        platform: 13,
        platform_blob: &[],
        version: [2017, 4],
    };
    if let Some(ch) = choose(&t, &s) {
        let _ = convert(&t, raw, &ch, &s, &mut enc, "a.assets|1", Some("2017.4.9f1"));
        if (ch.width, ch.height) == (w, h) {
            let _ = strip_blocks(&t, raw, &ch, &mut enc);
        }
    }
});
