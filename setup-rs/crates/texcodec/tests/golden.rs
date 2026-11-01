//! Golden test: texcodec against the Python's own textures step on real game data.
//!
//! setup-rs/tests/golden/textures_golden.py dumps, for each Texture2D the step looks at, its
//! fields, raw pixels and what textures.py made of it (manifest.jsonl). This test reads such a
//! dump from $TEXCODEC_GOLDEN and checks every texture: the same decision, and for converted ones
//! the same new fields and the same bytes (SHA-256 of the data), the same guided-block counts and
//! guide use. Without $TEXCODEC_GOLDEN it does nothing. $TEXCODEC_GOLDEN_SKIP_RAW=1: decisions only.
use std::io::BufRead;
use std::time::Instant;

use serde_json::Value;
use sha2::{Digest, Sha256};
use texcodec::texture::{self, Choice, Settings, Texture};

fn hex(b: &[u8]) -> String {
    Sha256::digest(b)
        .iter()
        .map(|x| format!("{x:02x}"))
        .collect()
}

fn hexbytes(s: &str) -> Vec<u8> {
    (0..s.len() / 2)
        .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap())
        .collect()
}

#[test]
fn golden() {
    let Ok(dir) = std::env::var("TEXCODEC_GOLDEN") else {
        eprintln!("TEXCODEC_GOLDEN not set: golden test skipped");
        return;
    };
    let dir = std::path::PathBuf::from(dir);
    let f = std::fs::File::open(dir.join("manifest.jsonl")).expect("manifest.jsonl");
    let mut lines = std::io::BufReader::new(f).lines();
    let header: Value = serde_json::from_str(&lines.next().unwrap().unwrap()).unwrap();
    let mut s = Settings::from_config(Some(&header["textures"])).unwrap();
    s.threads = header["threads"].as_u64().unwrap() as usize;
    let mut enc = texture::encoder(&s).unwrap();
    assert_eq!(enc.settings.guided, header["mode"] == "guided");
    assert_eq!(
        enc.guide.as_ref().is_some_and(|g| g.ok),
        header["guide_ok"].as_bool().unwrap()
    );
    let decisions_only = std::env::var("TEXCODEC_GOLDEN_SKIP_RAW").is_ok();
    let (mut seen, mut decided, mut converted, mut same, mut bad) = (0, 0, 0, 0, Vec::new());
    let (mut t_py, mut t_rs) = (0.0f64, 0.0f64);
    for line in lines {
        let r: Value = serde_json::from_str(&line.unwrap()).unwrap();
        seen += 1;
        let outcome = r["outcome"].as_str().unwrap();
        if outcome == "skip" {
            continue;
        }
        let blob = hexbytes(r["blob"].as_str().unwrap_or(""));
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
            platform_blob: &blob,
            version: [
                r["version"][0].as_i64().unwrap(),
                r["version"][1].as_i64().unwrap(),
            ],
        };
        let what = format!(
            "{}|{} {} {}x{} fmt {}",
            r["rel"].as_str().unwrap(),
            r["pid"],
            t.name,
            t.width,
            t.height,
            t.format
        );
        let ch = texture::choose(&t, &s);
        let want = r.get("choice").map(|c| Choice {
            block: (
                c["block"][0].as_u64().unwrap() as u32,
                c["block"][1].as_u64().unwrap() as u32,
            ),
            width: c["width"].as_u64().unwrap() as usize,
            height: c["height"].as_u64().unwrap() as usize,
            alpha: c["alpha"].as_bool().unwrap(),
            mips: c["mips"].as_u64().unwrap() as usize,
        });
        decided += 1;
        if ch != want {
            bad.push(format!("{what}: choice {ch:?}, Python {want:?}"));
            continue;
        }
        let (Some(ch), Some(raw)) = (ch, r.get("raw").and_then(Value::as_str)) else {
            continue;
        };
        if decisions_only {
            continue;
        }
        let raw = std::fs::read(dir.join(raw)).unwrap();
        converted += 1;
        let key = format!("{}|{}", r["rel"].as_str().unwrap(), r["pid"]);
        let before = (
            enc.counts,
            enc.guide.as_ref().map_or((0, 0), |g| (g.used, g.missed)),
        );
        let t0 = Instant::now();
        let out = texture::convert(&t, &raw, &ch, &s, &mut enc, &key, r["unity"].as_str());
        let dt = t0.elapsed().as_secs_f64();
        let out = match out {
            Ok(o) => o,
            Err(e) => {
                bad.push(format!("{what}: error {e}"));
                continue;
            }
        };
        t_rs += dt;
        t_py += r["seconds"].as_f64().unwrap_or(0.0);
        let o = &r["out"];
        let mut diffs = Vec::new();
        for (k, a, b) in [
            ("format", out.format as u64, o["format"].as_u64().unwrap()),
            ("width", out.width as u64, o["width"].as_u64().unwrap()),
            ("height", out.height as u64, o["height"].as_u64().unwrap()),
            ("mips", out.mip_count as u64, o["mips"].as_u64().unwrap()),
            ("size", out.data.len() as u64, o["size"].as_u64().unwrap()),
        ] {
            if a != b {
                diffs.push(format!("{k} {a} (Python {b})"));
            }
        }
        if hex(&out.data) != o["sha256"].as_str().unwrap() {
            diffs.push("data differs".into());
        }
        let counts: Vec<u64> = enc
            .counts
            .iter()
            .zip(before.0)
            .map(|(a, b)| a - b)
            .collect();
        let want_counts: Vec<u64> = r["counts"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_u64().unwrap())
            .collect();
        if counts != want_counts {
            diffs.push(format!("counts {counts:?} (Python {want_counts:?})"));
        }
        if let Some(g) = r.get("guide").and_then(Value::as_array) {
            let now = enc.guide.as_ref().map_or((0, 0), |g| (g.used, g.missed));
            let got = [now.0 - before.1 .0, now.1 - before.1 .1];
            let want = [g[0].as_u64().unwrap(), g[1].as_u64().unwrap()];
            if got != want {
                diffs.push(format!("guide used/missed {got:?} (Python {want:?})"));
            }
        }
        if !diffs.is_empty() {
            // where it starts: the decoded and scaled levels, or the encoding
            if let Some(levels) = r.get("levels").and_then(Value::as_array) {
                if let Ok(p) = texture::prepare(&t, &raw, &ch, s.threads, s.prepare_split) {
                    for (i, (lv, want)) in p.levels.iter().zip(levels).enumerate() {
                        if hex(&lv.px) != want.as_str().unwrap() {
                            diffs.push(format!("level {i} pixels differ"));
                        }
                    }
                }
            }
            bad.push(format!("{what} ({}): {}", r["path"], diffs.join(", ")));
        } else {
            same += 1;
        }
    }
    eprintln!(
        "golden {}: {seen} textures, {decided} decisions checked, {converted} converted: {same} identical, {} differ; \
         convert time Rust {t_rs:.1} s, Python {t_py:.1} s",
        dir.display(),
        bad.len()
    );
    for b in bad.iter().take(40) {
        eprintln!("  {b}");
    }
    assert!(bad.is_empty(), "{} textures differ", bad.len());
}
