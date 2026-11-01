//! One texture through texcodec, timed, with its peak memory (VmHWM after resetting it through
//! /proc/self/clear_refs, less the resident size before): what textures_golden.py `one` measures
//! for the Python.
//!
//!   one <manifest line (a file with one JSON record of textures_golden.py)> <raw> <header line>
use sha2::{Digest, Sha256};
use texcodec::texture::{self, Settings, Texture};

fn status_kb(field: &str) -> u64 {
    let s = std::fs::read_to_string("/proc/self/status").unwrap_or_default();
    s.lines()
        .find(|l| l.starts_with(field))
        .and_then(|l| l.split_whitespace().nth(1))
        .and_then(|v| v.parse().ok())
        .unwrap_or(0)
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let r: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(&a[1]).unwrap()).unwrap();
    let header: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(&a[3]).unwrap()).unwrap();
    let mut s = Settings::from_config(Some(&header["textures"])).unwrap();
    s.threads = header["threads"].as_u64().unwrap() as usize;
    let mut enc = texture::encoder(&s).unwrap();
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
        version: [
            r["version"][0].as_i64().unwrap(),
            r["version"][1].as_i64().unwrap(),
        ],
    };
    let ch = texture::choose(&t, &s).expect("a texture the step converts");
    let rss = status_kb("VmRSS:");
    let _ = std::fs::write("/proc/self/clear_refs", "5");
    let t0 = std::time::Instant::now();
    let raw = std::fs::read(&a[2]).unwrap(); // (inside: the step reads it too)
    let key = format!("{}|{}", r["rel"].as_str().unwrap(), r["pid"]);
    let out = texture::convert(&t, &raw, &ch, &s, &mut enc, &key, r["unity"].as_str()).unwrap();
    let dt = t0.elapsed().as_secs_f64();
    let hwm = status_kb("VmHWM:");
    let sha: String = Sha256::digest(&out.data)
        .iter()
        .map(|x| format!("{x:02x}"))
        .collect();
    println!(
        "{{\"time\": {dt:.3}, \"rss_before_mb\": {:.1}, \"peak_extra_mb\": {:.1}, \"same\": {}}}",
        rss as f64 / 1024.0,
        hwm.saturating_sub(rss) as f64 / 1024.0,
        sha == r["out"]["sha256"].as_str().unwrap_or("")
    );
}
