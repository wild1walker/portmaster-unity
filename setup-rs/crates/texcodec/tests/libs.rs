//! Library-level golden test: texcodec's decoders, resize and ASTC encoders against the Python's
//! libraries (UnityPy/Pillow decoding, Pillow's resize, astc_encoder, guided_astc) on made-up data
//! from setup-rs/tests/golden/texcodec_fuzz.py, read from $TEXCODEC_FUZZ (skipped when unset):
//! every format, odd sizes, boxes, every preset and tier, several block footprints.
use std::io::BufRead;

use serde_json::Value;
use sha2::{Digest, Sha256};
use texcodec::astc::{self, Context, Encoder, EncoderSettings, Seed};
use texcodec::guide::Limits;
use texcodec::image::{self, Rgba};
use texcodec::{decode, decode::tf};

fn hex(b: &[u8]) -> String {
    Sha256::digest(b)
        .iter()
        .map(|x| format!("{x:02x}"))
        .collect()
}

#[test]
fn libs() {
    let Ok(dir) = std::env::var("TEXCODEC_FUZZ") else {
        eprintln!("TEXCODEC_FUZZ not set: test skipped");
        return;
    };
    let dir = std::path::PathBuf::from(dir);
    let f = std::fs::File::open(dir.join("cases.jsonl")).expect("cases.jsonl");
    let mut counts = std::collections::BTreeMap::<String, (usize, usize)>::new();
    let mut bad = Vec::new();
    for line in std::io::BufReader::new(f).lines() {
        let c: Value = serde_json::from_str(&line.unwrap()).unwrap();
        let kind = c["kind"].as_str().unwrap().to_string();
        let input = std::fs::read(dir.join(c["input"].as_str().unwrap())).unwrap();
        let (w, h) = (
            c["width"].as_u64().unwrap() as usize,
            c["height"].as_u64().unwrap() as usize,
        );
        let to = |c: &Value| {
            (
                c["to"][0].as_u64().unwrap() as usize,
                c["to"][1].as_u64().unwrap() as usize,
            )
        };
        let got: String = match kind.as_str() {
            "decode" => {
                let fmt = c["format"].as_i64().unwrap() as i32;
                let img = decode::parse_image_data(&input, w, h, fmt, [2017, 4]).unwrap();
                hex(&image::texture_image(img, fmt == tf::BC4).px)
            }
            "resize" => {
                let bx = c["box"].as_array().map(|b| {
                    let v: Vec<f64> = b.iter().map(|x| x.as_f64().unwrap()).collect();
                    [v[0], v[1], v[2], v[3]]
                });
                let (nw, nh) = to(&c);
                match image::resize(
                    &Rgba {
                        width: w,
                        height: h,
                        px: input,
                    },
                    nw,
                    nh,
                    bx,
                ) {
                    Ok(r) => hex(&r.px),
                    Err(_) => "error".into(),
                }
            }
            "downscale" => {
                let (nw, nh) = to(&c);
                hex(&image::downscale(
                    Rgba {
                        width: w,
                        height: h,
                        px: input,
                    },
                    nw,
                    nh,
                )
                .unwrap()
                .px)
            }
            "astc" => {
                let block = (
                    c["block"][0].as_u64().unwrap() as u32,
                    c["block"][1].as_u64().unwrap() as u32,
                );
                let lim = &c["limits"];
                let limits = Limits {
                    partitions: lim
                        .get("tune_partition_count_limit")
                        .and_then(Value::as_u64)
                        .map(|v| v as u32),
                    index2: lim
                        .get("tune_2partition_index_limit")
                        .and_then(Value::as_u64)
                        .map(|v| v as u32),
                };
                let q = astc::preset(c["quality"].as_str().unwrap()).unwrap();
                let mut ctx = Context::new(block, q, limits, 3).unwrap();
                hex(&ctx.compress(&input, w, h, block).unwrap())
            }
            "guided" => {
                let s = EncoderSettings {
                    quality: c["quality"].as_str().unwrap().into(),
                    threads: 3,
                    guided: true,
                    max_err: 64,
                    line: 128,
                    near: 1024,
                };
                let mut enc = Encoder::new(s, None).unwrap();
                let seed_data = c["seed"]
                    .as_str()
                    .map(|p| std::fs::read(dir.join(p)).unwrap());
                let seed = seed_data.as_deref().map(|b| Seed {
                    blocks: b,
                    dxt5: c["dxt5"].as_bool().unwrap(),
                });
                let out = enc.encode_rows(&input, w, h, (4, 4), seed, None).unwrap();
                let want: Vec<u64> = c["counts"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .map(|v| v.as_u64().unwrap())
                    .collect();
                if enc.counts.to_vec() != want {
                    bad.push(format!(
                        "guided {w}x{h}: counts {:?} (Python {want:?})",
                        enc.counts
                    ));
                }
                hex(&out)
            }
            k => panic!("unknown case {k}"),
        };
        let e = counts.entry(kind.clone()).or_default();
        e.0 += 1;
        if got == c["out"].as_str().unwrap() {
            e.1 += 1;
        } else {
            let mut d = c.clone();
            d.as_object_mut().unwrap().remove("out");
            bad.push(format!("{kind}: {d}"));
        }
    }
    for (k, (n, same)) in &counts {
        eprintln!("libs {k}: {n} cases, {same} identical");
    }
    for b in bad.iter().take(30) {
        eprintln!("  {b}");
    }
    assert!(bad.is_empty(), "{} cases differ", bad.len());
}
