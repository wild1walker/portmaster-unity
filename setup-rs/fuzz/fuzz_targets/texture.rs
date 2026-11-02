//! A texture's data decoded as the textures step decodes it: its fields as a Texture2D gives
//! them (width and height as the object's integers, a format, the Unity version), the pixels
//! (BCn, crunched DXT, raw formats) made an image, scaled down, and the step's preparation of
//! its levels.
#![no_main]
use libfuzzer_sys::fuzz_target;
use pmu_fuzz::Input;
use texcodec::decode::{parse_image_data, tf};
use texcodec::image::{downscale, texture_image};
use texcodec::texture::{choose, decode_downscale, prepare, Settings, Texture};

const FORMATS: [i32; 15] = [
    tf::ALPHA8,
    tf::RGB24,
    tf::RGBA32,
    tf::ARGB32,
    tf::DXT1,
    tf::DXT3,
    tf::DXT5,
    tf::BGRA32,
    tf::BC6H,
    tf::BC7,
    tf::BC4,
    tf::BC5,
    tf::DXT1_CRUNCHED,
    tf::DXT5_CRUNCHED,
    tf::ASTC_RGB_4X4,
];

fuzz_target!(|data: &[u8]| {
    let mut inp = Input::new(data);
    let sel = inp.u8();
    // small sizes mostly; with the top bit, any of the object's 64-bit integers
    let (w, h) = if sel & 0x80 != 0 {
        (inp.int(8) as usize, inp.int(8) as usize)
    } else {
        (inp.u8() as usize % 65, inp.u8() as usize % 65)
    };
    let format = if sel & 0x40 != 0 {
        inp.int(4) as i32
    } else {
        FORMATS[(sel & 0x3f) as usize % FORMATS.len()]
    };
    let version = [[5, 6], [2017, 2], [2017, 4], [2021, 3]][inp.u8() as usize % 4];
    let mips = inp.u8() as i64 % 8;
    let raw = inp.rest();
    if let Ok(img) = parse_image_data(raw, w, h, format, version) {
        let rgba = texture_image(img, format == tf::BC4);
        if rgba.width > 1 && rgba.height > 1 {
            let (nw, nh) = (rgba.width / 2, rgba.height / 2);
            let _ = downscale(rgba, nw, nh);
        }
    }
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
        version,
    };
    let cfg = serde_json::json!({"scale": 0.5, "min_side": 1, "keep_max": 1, "min_convert": 1, "threads": 2});
    let Ok(s) = Settings::from_config(Some(&cfg)) else {
        return;
    };
    if let Some(ch) = choose(&t, &s) {
        let _ = prepare(&t, raw, &ch, 2, true);
        let _ = decode_downscale(&t, raw, ch.width, ch.height, 2);
    }
});
