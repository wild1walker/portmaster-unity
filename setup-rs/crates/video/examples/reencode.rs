//! reencode <file> <offset> <size> <out.webm> [max_height] [cpu_used] [cq_level] [threads]:
//! one clip (a VideoClip's bytes inside a .resource file) through the video step's re-encode,
//! with the time it took and both sizes; then the result decoded once more and timed (what a
//! player would pay).
use std::io::{Read, Seek, SeekFrom};
use std::time::Instant;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 5 {
        eprintln!("usage: reencode <file> <offset> <size> <out.webm> [max_height] [cpu_used] [cq_level] [threads]");
        std::process::exit(2);
    }
    let arg = |i: usize, d: i64| a.get(i).map_or(d, |s| s.parse().unwrap());
    let mut f = std::fs::File::open(&a[1]).unwrap();
    f.seek(SeekFrom::Start(a[2].parse().unwrap())).unwrap();
    let mut clip = vec![0u8; a[3].parse().unwrap()];
    f.read_exact(&mut clip).unwrap();
    let s = video::Settings {
        max_height: arg(5, 720) as u64,
        encode: video::vpx::EncodeSettings {
            threads: arg(8, 4) as usize,
            log2_partitions: 2,
            cpu_used: arg(6, 4) as i32,
            cq_level: arg(7, 10) as i32,
            max_kbps: 4000,
        },
    };
    let t0 = Instant::now();
    let (out, o) = video::reencode(&clip, &s)
        .unwrap()
        .expect("not taller than max_height");
    let dt = t0.elapsed().as_secs_f64();
    std::fs::write(&a[4], &out).unwrap();
    println!(
        "{}x{} -> {}x{}, {} frames in {:.1} s ({:.1} ms a frame), {} -> {} bytes",
        o.from.0,
        o.from.1,
        o.to.0,
        o.to.1,
        o.frames,
        dt,
        dt * 1000.0 / o.frames as f64,
        clip.len(),
        out.len()
    );
    // the new clip decoded as a player would
    let w = video::webm::WebM::parse(&out).unwrap();
    let mut dec = video::vpx::Decoder::new(4).unwrap();
    let t1 = Instant::now();
    let mut n = 0;
    for p in &w.parts {
        if let video::webm::Part::Cluster(c) = p {
            for k in c
                .items
                .iter()
                .filter_map(video::webm::Item::block)
                .filter(|k| k.track == 1)
            {
                if dec.decode(&k.payload).unwrap().is_some() {
                    n += 1;
                }
            }
        }
    }
    println!(
        "decoded again: {n} pictures, {:.2} ms a frame (4 threads)",
        t1.elapsed().as_secs_f64() * 1000.0 / n.max(1) as f64
    );
    // quality: each new picture against the original's, halved the same way (luma PSNR); the
    // middle frame of both written as PGM (luma) beside the output
    let blocks = |w: &video::webm::WebM| -> Vec<Vec<u8>> {
        w.parts
            .iter()
            .filter_map(|p| match p {
                video::webm::Part::Cluster(c) => Some(c),
                _ => None,
            })
            .flat_map(|c| {
                c.items
                    .iter()
                    .filter_map(video::webm::Item::block)
                    .filter(|k| k.track == 1)
                    .map(|k| k.payload.clone())
                    .collect::<Vec<_>>()
            })
            .collect()
    };
    let src = video::webm::WebM::parse(&clip).unwrap();
    let (mut da, mut db) = (
        video::vpx::Decoder::new(1).unwrap(),
        video::vpx::Decoder::new(1).unwrap(),
    );
    let (ba, bb) = (blocks(&src), blocks(&w));
    let (mut sum, mut worst, mut m) = (0.0f64, f64::MAX, 0usize);
    let mid = ba.len() / 2;
    for (i, (x, y)) in ba.iter().zip(bb.iter()).enumerate() {
        let (Some(mut p), Some(q)) = (da.decode(x).unwrap(), db.decode(y).unwrap()) else {
            continue;
        };
        while p.h > q.h {
            p = video::scale::half(&p);
        }
        let mse =
            p.y.iter()
                .zip(&q.y)
                .map(|(a, b)| {
                    let d = *a as f64 - *b as f64;
                    d * d
                })
                .sum::<f64>()
                / p.y.len() as f64;
        let psnr = if mse == 0.0 {
            99.0
        } else {
            10.0 * (255.0f64 * 255.0 / mse).log10()
        };
        sum += psnr;
        worst = worst.min(psnr);
        m += 1;
        if i == mid {
            for (tag, pic) in [("src", &p), ("new", &q)] {
                let mut pgm = format!("P5\n{} {}\n255\n", pic.w, pic.h).into_bytes();
                pgm.extend_from_slice(&pic.y);
                std::fs::write(format!("{}.{tag}.pgm", a[4]), pgm).unwrap();
            }
        }
    }
    println!(
        "luma PSNR against the halved original: {:.1} dB average, {:.1} dB worst, {m} frames",
        sum / m.max(1) as f64,
        worst
    );
}
