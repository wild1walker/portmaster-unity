//! `pmu-dev rigstats <out dir> [--png]` (was tools/box64/rigstats.py): the box64 test rig's numbers and
//! pictures, on the host (tools/box64/rig.sh): screenshots (X window dumps, .xwd) -> .png, then frames
//! per second and CPU per thread per frame -> rig.txt (printed); --png: the screenshots only.
//!
//! Reads only what rig_inside.sh wrote: shot_<s>.xwd, glxes_fps or hud/fps (frames per second, one line
//! a second), cpu0.txt / cpu1.txt (tid name ticks), t0.txt / t1.txt, gpid.txt, hz.txt.
use std::collections::HashMap;
use std::io::Write;
use std::path::Path;

fn be32(d: &[u8], o: usize) -> Result<u32, String> {
    d.get(o..o + 4).map(|b| u32::from_be_bytes(b.try_into().unwrap())).ok_or_else(|| "the dump ends early".to_string())
}

/// A 24/32-bit TrueColor ZPixmap X window dump (xwd -root) as an RGB PNG.
pub fn xwd_to_png(src: &Path, dst: &Path) -> Result<(), String> {
    let d = std::fs::read(src).map_err(|e| format!("{}: {e}", src.display()))?;
    let h: Vec<u32> = (0..25).map(|i| be32(&d, 4 * i)).collect::<Result<_, _>>()?;
    let (size, fmt, w, ht, byte_order, bpp, bpl, ncolors) = (h[0], h[2], h[4], h[5], h[7], h[11], h[12], h[19]);
    let masks = [h[14], h[15], h[16]];
    if fmt != 2 || !(bpp == 24 || bpp == 32) {
        return Err(format!("{}: unsupported dump (format {fmt}, {bpp} bits per pixel)", src.display()));
    }
    let off = size as usize + 12 * ncolors as usize;
    let shifts = masks.map(|m| if m == 0 { 0 } else { m.trailing_zeros() });
    let step = (bpp / 8) as usize;
    let mut rows = Vec::with_capacity((w as usize * 3 + 1) * ht as usize);
    for y in 0..ht as usize {
        rows.push(0u8);
        let a = off + y * bpl as usize;
        let line = d.get(a..(a + w as usize * step).min(d.len())).unwrap_or(&[]);
        for px in line.chunks(step) {
            let mut v: u64 = 0;
            if byte_order == 0 {
                for (i, b) in px.iter().enumerate() {
                    v |= (*b as u64) << (8 * i);
                }
            } else {
                for b in px {
                    v = v << 8 | *b as u64;
                }
            }
            for k in 0..3 {
                rows.push(((v & masks[k] as u64) >> shifts[k]) as u8);
            }
        }
    }
    let chunk = |kind: &[u8], data: &[u8]| -> Vec<u8> {
        let mut c = (data.len() as u32).to_be_bytes().to_vec();
        c.extend_from_slice(kind);
        c.extend_from_slice(data);
        let mut h = crc32fast::Hasher::new();
        h.update(kind);
        h.update(data);
        c.extend_from_slice(&h.finalize().to_be_bytes());
        c
    };
    let mut ihdr = w.to_be_bytes().to_vec();
    ihdr.extend_from_slice(&ht.to_be_bytes());
    ihdr.extend_from_slice(&[8, 2, 0, 0, 0]);
    let mut z = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::new(6));
    z.write_all(&rows).map_err(|e| e.to_string())?;
    let idat = z.finish().map_err(|e| e.to_string())?;
    let mut png = b"\x89PNG\r\n\x1a\n".to_vec();
    png.extend(chunk(b"IHDR", &ihdr));
    png.extend(chunk(b"IDAT", &idat));
    png.extend(chunk(b"IEND", b""));
    std::fs::write(dst, png).map_err(|e| format!("{}: {e}", dst.display()))
}

fn read(p: &Path) -> Option<String> {
    std::fs::read(p).ok().map(|b| String::from_utf8_lossy(&b).into_owned())
}

fn isdigit(s: &str) -> bool {
    !s.is_empty() && s.bytes().all(|c| c.is_ascii_digit())
}

pub fn stats(out: &Path) -> Vec<String> {
    let mut lines = Vec::new();
    let mut fps_file = out.join("glxes_fps");
    let mut src = "glxes";
    if std::fs::metadata(&fps_file).map_or(true, |m| m.len() == 0) {
        // no glxes: Mesa's HUD
        fps_file = out.join("hud").join("fps");
        src = "HUD";
    }
    let vals: Vec<f64> = read(&fps_file)
        .unwrap_or_default()
        .lines()
        .filter(|l| !l.trim().is_empty())
        .filter_map(|l| l.split_whitespace().next()?.parse().ok())
        .collect();
    let (t0, t1) = (read(&out.join("t0.txt")).unwrap_or_default(), read(&out.join("t1.txt")).unwrap_or_default());
    let (t0, t1) = (t0.trim(), t1.trim());
    let (m0, m1): (usize, usize) = if isdigit(t0) && isdigit(t1) { (t0.parse().unwrap(), t1.parse().unwrap()) } else { (0, 0) };
    let win: &[f64] = if m1 != 0 && vals.len() >= m1 {
        if m0 <= m1 { &vals[m0..m1] } else { &[] }
    } else {
        let n = (m1 as i64 - m0 as i64).max(1) as usize;
        &vals[vals.len().saturating_sub(n)..]
    };
    let fps = if win.is_empty() { 0.0 } else { win.iter().fold(0.0f64, |a, b| a + b) / win.len() as f64 };
    let mn = vals.iter().copied().fold(f64::INFINITY, f64::min);
    let mx = vals.iter().copied().fold(f64::NEG_INFINITY, f64::max);
    let (mn, mx) = if vals.is_empty() { (0.0, 0.0) } else { (mn, mx) };
    lines.push(format!("fps {fps:.1} over {} s ({src} seconds {}; whole run min/max {mn:.0}/{mx:.0})", win.len(), vals.len()));
    let ticks = |name: &str| -> Vec<(String, (String, i64))> {
        let mut v: Vec<(String, (String, i64))> = Vec::new();
        for l in read(&out.join(name)).unwrap_or_default().lines() {
            let f: Vec<&str> = l.split_whitespace().collect();
            if f.len() == 3 {
                let Ok(t) = f[2].parse() else { continue };
                let e = (f[1].to_string(), t);
                match v.iter_mut().find(|x| x.0 == f[0]) {
                    Some(x) => x.1 = e,
                    None => v.push((f[0].to_string(), e)),
                }
            }
        }
        v
    };
    let (c0, c1) = (ticks("cpu0.txt"), ticks("cpu1.txt"));
    if c0.is_empty() || c1.is_empty() || m1 <= m0 {
        lines.push("no thread CPU counts (the game ended before the measured window)".into());
        return lines;
    }
    let c0: HashMap<&String, i64> = c0.iter().map(|(t, (_, n))| (t, *n)).collect();
    let hz: f64 = read(&out.join("hz.txt")).map(|s| s.trim().to_string()).filter(|s| !s.is_empty()).and_then(|s| s.parse().ok()).unwrap_or(100.0);
    let secs = (m1 - m0) as f64;
    let pid = read(&out.join("gpid.txt")).unwrap_or_default().trim().to_string();
    let mut rows: Vec<(f64, String, String)> =
        c1.iter().map(|(t, (n, k))| ((k - c0.get(t).copied().unwrap_or(0)) as f64 / hz, t.clone(), n.clone())).collect();
    rows.sort_by(|a, b| b.partial_cmp(a).unwrap_or(std::cmp::Ordering::Equal));
    lines.push(format!("cpu total {:.0}% of a core over {} s", 100.0 * rows.iter().map(|r| r.0).fold(0.0f64, |a, b| a + b) / secs, m1 - m0));
    for (cpu, tid, n) in rows.iter().take(8) {
        if *cpu > 0.0 {
            let per_frame = if fps != 0.0 { 1000.0 * cpu / (fps * secs) } else { 0.0 };
            let who = if *tid == pid { "main".to_string() } else { format!("tid {tid}") };
            lines.push(format!("  {who} {n}: {:.0}%, {per_frame:.1} ms/frame", 100.0 * cpu / secs));
        }
    }
    lines
}

pub fn run(args: &[String]) -> Result<u8, String> {
    let Some(out) = args.first() else { return Err("usage: pmu-dev rigstats <out dir> [--png]".into()) };
    let out = Path::new(out);
    let png_only = args[1..].iter().any(|a| a == "--png");
    let mut shots: Vec<_> = std::fs::read_dir(out)
        .map_err(|e| format!("{}: {e}", out.display()))?
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.starts_with("shot_") && n.ends_with(".xwd")))
        .collect();
    shots.sort();
    for f in shots {
        let png = f.with_extension("png");
        match xwd_to_png(&f, &png).and_then(|_| std::fs::remove_file(&f).map_err(|e| e.to_string())) {
            Ok(()) => {}
            Err(e) => eprintln!("{}: {e}", f.display()),
        }
    }
    if png_only {
        return Ok(0);
    }
    let text = read(&out.join("rig.txt")).unwrap_or_default() + &stats(out).join("\n") + "\n";
    std::fs::write(out.join("rig.txt"), &text).map_err(|e| e.to_string())?;
    print!("{text}");
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn numbers_and_png() {
        let d = std::env::temp_dir().join(format!("pmu-dev-rig-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        let w = |n: &str, t: &str| std::fs::write(d.join(n), t).unwrap();
        w("glxes_fps", "30\n40\n50\n60\n");
        w("t0.txt", "1\n");
        w("t1.txt", "3\n");
        w("cpu0.txt", "10 main 100\n11 worker 50\n");
        w("cpu1.txt", "10 main 250\n11 worker 60\n");
        w("gpid.txt", "10\n");
        let s = stats(&d).join("\n");
        assert!(s.contains("fps 45.0 over 2 s (glxes seconds 4; whole run min/max 30/60)"), "{s}");
        assert!(s.contains("cpu total 80% of a core over 2 s") && s.contains("  main main: 75%, 16.7 ms/frame"), "{s}");
        // a 2x1 32-bit little-endian dump: red, then blue
        let mut h = [0u32; 25];
        h[0] = 100;
        h[2] = 2;
        h[4] = 2;
        h[5] = 1;
        h[11] = 32;
        h[12] = 8;
        h[14] = 0xff0000;
        h[15] = 0xff00;
        h[16] = 0xff;
        let mut x: Vec<u8> = h.iter().flat_map(|v| v.to_be_bytes()).collect();
        x.resize(100, 0);
        x.extend([0, 0, 255, 0, 255, 0, 0, 0]);
        w("shot_1.xwd", "");
        std::fs::write(d.join("shot_1.xwd"), x).unwrap();
        xwd_to_png(&d.join("shot_1.xwd"), &d.join("shot_1.png")).unwrap();
        let png = std::fs::read(d.join("shot_1.png")).unwrap();
        assert!(png.starts_with(b"\x89PNG\r\n\x1a\n") && png.len() > 40);
        let _ = std::fs::remove_dir_all(&d);
    }
}
