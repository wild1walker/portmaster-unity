//! Where texcodec's blocks differ from the Python's for one texture: converts it (with the encoder
//! threads given, each run on fresh contexts) and lists the 16-byte blocks that differ from the
//! Python's data (textures_golden.py `one --blob`).
//!
//!   blockdiff <record.json> <raw> <header.json> <python data> [threads...]
use texcodec::texture::{self, Settings, Texture};

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let r: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&a[1]).unwrap()).unwrap();
    let raw = std::fs::read(&a[2]).unwrap();
    let header: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&a[3]).unwrap()).unwrap();
    let py = std::fs::read(&a[4]).unwrap();
    let threads: Vec<usize> = if a.len() > 5 {
        a[5..].iter().map(|t| t.parse().unwrap()).collect()
    } else {
        vec![header["threads"].as_u64().unwrap() as usize]
    };
    let t = Texture {
        name: r["name"].as_str().unwrap(),
        width: r["width"].as_u64().unwrap() as usize,
        height: r["height"].as_u64().unwrap() as usize,
        format: r["format"].as_i64().unwrap() as i32,
        mip_count: r["mips"].as_i64(),
        is_readable: r["readable"].as_bool().unwrap(),
        dimension: r["dimension"].as_i64(),
        image_count: r["images"].as_i64(),
        platform: r["platform"].as_i64().unwrap() as i32,
        platform_blob: &[],
        version: [r["version"][0].as_i64().unwrap(), r["version"][1].as_i64().unwrap()],
    };
    let key = format!("{}|{}", r["rel"].as_str().unwrap(), r["pid"]);
    let mut first: Option<Vec<u8>> = None;
    for &n in &threads {
        let mut s = Settings::from_config(Some(&header["textures"])).unwrap();
        s.threads = n;
        let mut enc = texture::encoder(&s).unwrap();
        let ch = texture::choose(&t, &s).unwrap();
        let out = texture::convert(&t, &raw, &ch, &s, &mut enc, &key, r["unity"].as_str()).unwrap();
        let bx = ch.width.div_ceil(ch.block.0 as usize);
        let diff: Vec<usize> = (0..out.data.len().min(py.len()) / 16)
            .filter(|&b| out.data[16 * b..16 * b + 16] != py[16 * b..16 * b + 16])
            .collect();
        println!(
            "threads {n}: {} blocks, {} differ (sizes {} / {}), counts {:?}, guide {:?}",
            out.data.len() / 16,
            diff.len(),
            out.data.len(),
            py.len(),
            enc.counts,
            enc.guide.as_ref().map(|g| (g.used, g.missed))
        );
        for &b in diff.iter().take(12) {
            println!(
                "  block {b} (x {}, y {} from the bottom): rust {:02x?} python {:02x?}",
                b % bx,
                b / bx,
                &out.data[16 * b..16 * b + 16],
                &py[16 * b..16 * b + 16]
            );
        }
        match &first {
            None => first = Some(out.data),
            Some(f) => println!("  same as the first run: {}", *f == out.data),
        }
    }
}
