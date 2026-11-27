//! Screenshots and sound of the test scripts (were Pillow and the wave module in their inline Python):
//!
//! `pmu-dev png <in.ppm|png> <out.png>`: a screenshot (ua_host's shot-<n>.ppm) as a PNG (tools/device/drive.sh).
//! `pmu-dev montage <dir> <out.png>`: the first four shot-<n>.ppm of a folder (by n) in a 2x2 picture, as
//! tools/port/rehearse.sh's shots.png; prints how many and each one's red range.
//! `pmu-dev pcmwav <emu dir> <out dir>`: the emulated rig's OpenSL ES output (sound.pcm*, its format in
//! run.log's "output to /emu/<file> (<rate> Hz, <ch> ch, fmt 0x<fmt>)") as WAV files in <out dir>, each
//! with how loud it was (peak, RMS, the seconds with sound above -40 dBFS); the raw files are removed.
use std::io::{Read, Write};
use std::path::Path;

use regex::Regex;

use crate::shaders::read_image;

/// An RGB PNG of w x h pixels (zlib level 6, filter 0 on every row).
pub fn write_png(p: &Path, w: usize, h: usize, rgb: &[u8]) -> Result<(), String> {
    let mut rows = Vec::with_capacity((w * 3 + 1) * h);
    for y in 0..h {
        rows.push(0);
        rows.extend_from_slice(&rgb[y * w * 3..(y + 1) * w * 3]);
    }
    let chunk = |kind: &[u8], data: &[u8]| -> Vec<u8> {
        let mut c = (data.len() as u32).to_be_bytes().to_vec();
        c.extend_from_slice(kind);
        c.extend_from_slice(data);
        let mut hs = crc32fast::Hasher::new();
        hs.update(kind);
        hs.update(data);
        c.extend_from_slice(&hs.finalize().to_be_bytes());
        c
    };
    let mut ihdr = (w as u32).to_be_bytes().to_vec();
    ihdr.extend_from_slice(&(h as u32).to_be_bytes());
    ihdr.extend_from_slice(&[8, 2, 0, 0, 0]);
    let mut z = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::new(6));
    z.write_all(&rows).map_err(|e| e.to_string())?;
    let idat = z.finish().map_err(|e| e.to_string())?;
    let mut png = b"\x89PNG\r\n\x1a\n".to_vec();
    png.extend(chunk(b"IHDR", &ihdr));
    png.extend(chunk(b"IDAT", &idat));
    png.extend(chunk(b"IEND", b""));
    std::fs::write(p, png).map_err(|e| format!("{}: {e}", p.display()))
}

pub fn png_run(args: &[String]) -> Result<u8, String> {
    let [src, dst] = args else { return Err("usage: pmu-dev png <in.ppm> <out.png>".into()) };
    let (w, h, px) = read_image(Path::new(src))?;
    write_png(Path::new(dst), w, h, &px)?;
    Ok(0)
}

/// shot-<n>.ppm files of a folder, by n
pub fn shots(dir: &Path, ext: &str) -> Vec<std::path::PathBuf> {
    let mut v: Vec<(u64, std::path::PathBuf)> = std::fs::read_dir(dir)
        .map(|r| {
            r.filter_map(|e| e.ok())
                .filter_map(|e| {
                    let n = e.file_name().to_string_lossy().into_owned();
                    let k = n.strip_prefix("shot-")?.strip_suffix(ext)?.parse().ok()?;
                    Some((k, e.path()))
                })
                .collect()
        })
        .unwrap_or_default();
    v.sort();
    v.into_iter().map(|x| x.1).collect()
}

pub fn montage_run(args: &[String]) -> Result<u8, String> {
    let [dir, out] = args else { return Err("usage: pmu-dev montage <dir> <out.png>".into()) };
    let fs = shots(Path::new(dir), ".ppm");
    let mut ims = Vec::new();
    for f in &fs {
        ims.push(read_image(f)?);
    }
    let mut ranges = Vec::new();
    if let Some((w, h, _)) = ims.first().map(|x| (x.0, x.1, ())) {
        let mut s = vec![0u8; w * 2 * h * 2 * 3];
        for (i, (iw, ih, px)) in ims.iter().take(4).enumerate() {
            let (ox, oy) = ((i % 2) * w, (i / 2) * h);
            for y in 0..(*ih).min(h * 2 - oy) {
                for x in 0..(*iw).min(w * 2 - ox) {
                    let d = ((oy + y) * w * 2 + ox + x) * 3;
                    s[d..d + 3].copy_from_slice(&px[(y * iw + x) * 3..(y * iw + x) * 3 + 3]);
                }
            }
        }
        write_png(Path::new(out), w * 2, h * 2, &s)?;
    }
    for (_, _, px) in &ims {
        let r = px.iter().step_by(3);
        ranges.push(format!("({}, {})", r.clone().min().unwrap_or(&0), r.max().unwrap_or(&0)));
    }
    println!("{} shots [{}] -> {out}", ims.len(), ranges.join(", "));
    Ok(0)
}

fn db(x: f64) -> String {
    if x > 0.0 {
        format!("{:.1} dBFS", 20.0 * (x / 32768.0).log10())
    } else {
        "silent".into()
    }
}

pub fn pcmwav_run(args: &[String]) -> Result<u8, String> {
    let [e, work] = args else { return Err("usage: pmu-dev pcmwav <emu dir> <out dir>".into()) };
    let (e, work) = (Path::new(e), Path::new(work));
    let log = std::fs::read(e.join("run.log")).map(|b| String::from_utf8_lossy(&b).into_owned()).unwrap_or_default();
    let rx = Regex::new(r"output to /emu/(\S+) \((\d+) Hz, (\d+) ch, fmt 0x([0-9a-f]+)\)").unwrap();
    let mut fmts = std::collections::HashMap::new();
    for c in rx.captures_iter(&log) {
        fmts.insert(c[1].to_string(), (c[2].parse::<u32>().unwrap_or(48000), c[3].parse::<u16>().unwrap_or(2), u32::from_str_radix(&c[4], 16).unwrap_or(0)));
    }
    let mut pcms: Vec<std::path::PathBuf> = std::fs::read_dir(e)
        .map(|r| r.filter_map(|x| x.ok()).map(|x| x.path()).filter(|p| p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.starts_with("sound.pcm"))).collect())
        .unwrap_or_default();
    pcms.sort();
    if pcms.is_empty() {
        println!("sound: none (the game opened no OpenSL ES output)");
    }
    for p in pcms {
        let name = p.file_name().unwrap().to_string_lossy().into_owned();
        let (rate, ch, fmt) = fmts.get(&name).copied().unwrap_or((48000, 2, 0x8010));
        if fmt != 0x8010 {
            // (SDL's AUDIO_S16LSB; others kept raw)
            println!("sound: {}: format 0x{fmt:x}, {rate} Hz, {ch} ch, left raw", p.display());
            continue;
        }
        let out = work.join(format!("{}.wav", name.replace("sound.pcm", "sound")));
        let mut f = std::fs::File::open(&p).map_err(|e| e.to_string())?;
        let mut wav = std::io::BufWriter::new(std::fs::File::create(&out).map_err(|e| format!("{}: {e}", out.display()))?);
        let hdr = |n: u32| -> Vec<u8> {
            let mut h = b"RIFF".to_vec();
            h.extend((36 + n).to_le_bytes());
            h.extend(b"WAVEfmt ");
            h.extend(16u32.to_le_bytes());
            h.extend(1u16.to_le_bytes());
            h.extend(ch.to_le_bytes());
            h.extend(rate.to_le_bytes());
            h.extend((rate * ch as u32 * 2).to_le_bytes());
            h.extend((ch * 2).to_le_bytes());
            h.extend(16u16.to_le_bytes());
            h.extend(b"data");
            h.extend(n.to_le_bytes());
            h
        };
        wav.write_all(&hdr(0)).map_err(|e| e.to_string())?;
        let (mut peak, mut sq, mut n, mut loud, mut secs, mut bytes) = (0i32, 0f64, 0u64, 0, 0, 0u32);
        let mut buf = vec![0u8; rate as usize * ch as usize * 2];
        loop {
            // a second at a time
            let mut got = 0;
            while got < buf.len() {
                let k = f.read(&mut buf[got..]).map_err(|e| e.to_string())?;
                if k == 0 {
                    break;
                }
                got += k;
            }
            if got < 2 {
                break;
            }
            let b = &buf[..got / 2 * 2];
            wav.write_all(b).map_err(|e| e.to_string())?;
            bytes += b.len() as u32;
            let mut m = 0i32;
            for s in b.chunks(2) {
                let v = i16::from_le_bytes([s[0], s[1]]) as i32;
                m = m.max(v.abs());
                sq += (v * v) as f64;
            }
            n += (b.len() / 2) as u64;
            peak = peak.max(m);
            secs += 1;
            loud += (m > 328) as i32; // (above -40 dBFS)
            if got < buf.len() {
                break;
            }
        }
        let mut wav = wav.into_inner().map_err(|e| e.to_string())?;
        use std::io::Seek;
        wav.seek(std::io::SeekFrom::Start(0)).map_err(|e| e.to_string())?;
        wav.write_all(&hdr(bytes)).map_err(|e| e.to_string())?;
        drop(wav);
        std::fs::remove_file(&p).map_err(|e| e.to_string())?;
        let rms = if n != 0 { (sq / n as f64).sqrt() } else { 0.0 };
        println!(
            "sound: {}: {:.0} s, peak {}, RMS {}, sound in {loud} of {secs} seconds",
            out.display(),
            n as f64 / ch as f64 / rate as f64,
            db(peak as f64),
            db(rms)
        );
    }
    Ok(0)
}

/// `pmu-dev imgdiff <dirA> <dirB> [suffix]` (was tools/emu/imgdiff.py): exact comparison of screenshots
/// from two deterministic runs. For each file in both (default the .png ones; [suffix] e.g. .ppm): identical,
/// or the number of differing pixels, the largest channel difference and the box around the differences
/// (and <name>.diff.png beside B, the differences in white).
pub fn imgdiff_run(args: &[String]) -> Result<u8, String> {
    let (Some(a), Some(b)) = (args.first(), args.get(1)) else { return Err("usage: pmu-dev imgdiff <dirA> <dirB> [suffix]".into()) };
    let suffix = args.get(2).map(|s| s.trim_start_matches('*').to_string()).unwrap_or_else(|| ".png".into());
    let mut names: Vec<String> = std::fs::read_dir(a)
        .map_err(|e| format!("{a}: {e}"))?
        .filter_map(|e| e.ok().map(|e| e.file_name().to_string_lossy().into_owned()))
        .filter(|n| n.ends_with(&suffix) && !n.ends_with(".diff.png"))
        .collect();
    names.sort();
    for n in names {
        let (fa, fb) = (Path::new(a).join(&n), Path::new(b).join(&n));
        if !fb.exists() {
            continue;
        }
        let (ia, ib) = (read_image(&fa)?, read_image(&fb)?);
        if (ia.0, ia.1) != (ib.0, ib.1) {
            println!("{n}: sizes differ ({}, {}) ({}, {})", ia.0, ia.1, ib.0, ib.1);
            continue;
        }
        let (w, h) = (ia.0, ia.1);
        let (mut count, mut mx, mut box_) = (0usize, 0u8, None::<(usize, usize, usize, usize)>);
        let mut white = vec![0u8; w * h * 3];
        for y in 0..h {
            for x in 0..w {
                let i = (y * w + x) * 3;
                let d: Vec<u8> = (0..3).map(|k| ia.2[i + k].abs_diff(ib.2[i + k])).collect();
                if d.iter().any(|v| *v != 0) {
                    count += 1;
                    mx = mx.max(*d.iter().max().unwrap());
                    let bx = box_.get_or_insert((x, y, x + 1, y + 1));
                    *bx = (bx.0.min(x), bx.1.min(y), bx.2.max(x + 1), bx.3.max(y + 1));
                    for k in 0..3 {
                        if d[k] != 0 {
                            white[i + k] = 255;
                        }
                    }
                }
            }
        }
        match box_ {
            None => println!("{n}: identical"),
            Some(bx) => {
                println!("{n}: {count} pixels differ (of {}), max channel difference {mx}, in {bx:?}", w * h);
                let stem = fb.to_string_lossy();
                let stem = stem.rsplit_once('.').map_or(stem.to_string(), |s| s.0.to_string());
                write_png(Path::new(&format!("{stem}.diff.png")), w, h, &white)?;
            }
        }
    }
    Ok(0)
}
