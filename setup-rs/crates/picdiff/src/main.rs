//! picdiff: how far two pictures of the same thing are apart, and where (docs/quality.md, "Picture
//! changes no one sees").
//!
//!   picdiff frames <dir A> <dir B> <out dir> [--flip]
//!       PPM pairs of the same name in both folders (glreplay --hash-dump: the window before each swap):
//!       per frame PSNR (RGB), SSIM (8x8 windows, step 4, the mean over R, G, B), the largest difference of
//!       a channel, the pixels whose largest channel difference is over 8 (of 255); the worst frame (lowest
//!       SSIM) written as <out>/worst-frame.png (A | B | the difference times 8) and its worst 64x64 region
//!       as <out>/worst-crop.png (A | B | difference, 4x), and the same for each frame in <out>/frames/
//!       with --all. --flip: rows bottom to top (glReadPixels' order) shown top to bottom.
//!   picdiff tex <dir A> <dir B> <out dir> [--block <n>] [--crops <n>]
//!       PMU_CPU_DUMP folders of two settings of the textures step (texcodec: <name>__<w>x<h>.src.rgba,
//!       the level the encoder was given, and .dec.rgba, its ASTC blocks decoded): per texture A and B
//!       against the source (PSNR over the channels it uses, SSIM, largest difference, texels over 8) and B
//!       against A, the worst block (the one B is furthest behind A on, by squared error against the
//!       source), its PSNR in A and B; crops of the <n> textures B loses most on (source | A | B around that
//!       block, 8x, over grey where there is alpha) in <out>/crops/; a table in <out>/tex.tsv.
//!   picdiff png <in.ppm> <out.png> [--flip]
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

struct Img {
    w: usize,
    h: usize,
    c: usize,       // channels in px (3 or 4)
    px: Vec<u8>,
}

fn read_ppm(p: &Path, flip: bool) -> Result<Img, String> {
    let d = fs::read(p).map_err(|e| format!("{}: {e}", p.display()))?;
    let mut f = Vec::new();
    let mut i = 0;
    while f.len() < 4 {
        while i < d.len() && d[i].is_ascii_whitespace() { i += 1; }
        if i < d.len() && d[i] == b'#' { while i < d.len() && d[i] != b'\n' { i += 1; } continue; }
        let s = i;
        while i < d.len() && !d[i].is_ascii_whitespace() { i += 1; }
        f.push(String::from_utf8_lossy(&d[s..i]).to_string());
    }
    i += 1;
    if f[0] != "P6" || f[3] != "255" { return Err(format!("{}: not an 8-bit P6 PPM", p.display())); }
    let (w, h): (usize, usize) = (f[1].parse().unwrap_or(0), f[2].parse().unwrap_or(0));
    if d.len() < i + w * h * 3 { return Err(format!("{}: short", p.display())); }
    let mut px = d[i..i + w * h * 3].to_vec();
    if flip { px = flip_rows(&px, w * 3, h); }
    Ok(Img { w, h, c: 3, px })
}

fn flip_rows(px: &[u8], stride: usize, h: usize) -> Vec<u8> {
    let mut o = Vec::with_capacity(px.len());
    for y in (0..h).rev() { o.extend_from_slice(&px[y * stride..(y + 1) * stride]); }
    o
}

fn read_rgba(p: &Path) -> Result<Img, String> {
    // <name>__<w>x<h>.<kind>.rgba
    let n = p.file_name().unwrap().to_string_lossy().to_string();
    let wh = n.rsplit_once("__").ok_or("no __WxH")?.1.split('.').next().unwrap_or("").to_string();
    let (w, h) = wh.split_once('x').ok_or("no WxH")?;
    let (w, h): (usize, usize) = (w.parse().map_err(|_| "w")?, h.parse().map_err(|_| "h")?);
    let px = fs::read(p).map_err(|e| format!("{}: {e}", p.display()))?;
    if px.len() != w * h * 4 { return Err(format!("{}: {} bytes for {w}x{h}", p.display(), px.len())); }
    // (Unity's rows: bottom first; shown top first)
    Ok(Img { w, h, c: 4, px: flip_rows(&px, w * 4, h) })
}

// ---- PNG (RGB, 8 bit), with miniz_oxide's zlib
fn crc32(data: &[u8]) -> u32 {
    let mut t = [0u32; 256];
    for (n, e) in t.iter_mut().enumerate() {
        let mut c = n as u32;
        for _ in 0..8 { c = if c & 1 != 0 { 0xedb88320 ^ (c >> 1) } else { c >> 1 }; }
        *e = c;
    }
    let mut c = 0xffffffffu32;
    for &b in data { c = t[((c ^ b as u32) & 0xff) as usize] ^ (c >> 8); }
    c ^ 0xffffffff
}
fn write_png(p: &Path, w: usize, h: usize, rgb: &[u8]) -> Result<(), String> {
    let mut raw = Vec::with_capacity((w * 3 + 1) * h);
    for y in 0..h { raw.push(0); raw.extend_from_slice(&rgb[y * w * 3..(y + 1) * w * 3]); }
    let z = miniz_oxide::deflate::compress_to_vec_zlib(&raw, 6);
    let mut o = b"\x89PNG\r\n\x1a\n".to_vec();
    let chunk = |o: &mut Vec<u8>, ty: &[u8], d: &[u8]| {
        o.extend_from_slice(&(d.len() as u32).to_be_bytes());
        let mut c = ty.to_vec(); c.extend_from_slice(d);
        o.extend_from_slice(&c);
        o.extend_from_slice(&crc32(&c).to_be_bytes());
    };
    let mut ihdr = Vec::new();
    ihdr.extend_from_slice(&(w as u32).to_be_bytes()); ihdr.extend_from_slice(&(h as u32).to_be_bytes());
    ihdr.extend_from_slice(&[8, 2, 0, 0, 0]);
    chunk(&mut o, b"IHDR", &ihdr);
    chunk(&mut o, b"IDAT", &z);
    chunk(&mut o, b"IEND", &[]);
    if let Some(d) = p.parent() { let _ = fs::create_dir_all(d); }
    fs::write(p, o).map_err(|e| format!("{}: {e}", p.display()))
}

// ---- metrics
#[derive(Clone, Copy, Default)]
struct M { psnr: f64, ssim: f64, max: u8, over8: usize, n: usize, sse: f64 }

fn metrics(a: &Img, b: &Img, ch: usize) -> M {
    let (w, h, c) = (a.w, a.h, a.c);
    let mut sse = 0f64; let mut max = 0u8; let mut over8 = 0usize;
    for i in 0..w * h {
        let mut m = 0u8;
        for k in 0..ch {
            let d = (a.px[i * c + k] as i32 - b.px[i * c + k] as i32).unsigned_abs() as u8;
            sse += (d as f64) * (d as f64);
            m = m.max(d);
        }
        max = max.max(m);
        if m > 8 { over8 += 1; }
    }
    let mse = sse / (w * h * ch) as f64;
    let psnr = if mse == 0.0 { f64::INFINITY } else { 10.0 * (255.0f64 * 255.0 / mse).log10() };
    M { psnr, ssim: ssim(a, b, ch), max, over8, n: w * h, sse }
}

fn ssim(a: &Img, b: &Img, ch: usize) -> f64 {
    let (w, h, c) = (a.w, a.h, a.c);
    let (c1, c2) = ((0.01f64 * 255.0).powi(2), (0.03f64 * 255.0).powi(2));
    let win = 8usize.min(w).min(h).max(1);
    let step = (win / 2).max(1);
    let (mut s, mut n) = (0f64, 0usize);
    for k in 0..ch {
        let mut y = 0;
        while y + win <= h {
            let mut x = 0;
            while x + win <= w {
                let (mut ma, mut mb, mut va, mut vb, mut cv) = (0f64, 0f64, 0f64, 0f64, 0f64);
                for yy in y..y + win { for xx in x..x + win {
                    let i = (yy * w + xx) * c + k;
                    let (p, q) = (a.px[i] as f64, b.px[i] as f64);
                    ma += p; mb += q; va += p * p; vb += q * q; cv += p * q;
                } }
                let nn = (win * win) as f64;
                ma /= nn; mb /= nn;
                va = va / nn - ma * ma; vb = vb / nn - mb * mb; cv = cv / nn - ma * mb;
                s += ((2.0 * ma * mb + c1) * (2.0 * cv + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
                n += 1;
                x += step;
            }
            y += step;
        }
    }
    if n == 0 { 1.0 } else { s / n as f64 }
}

// ---- pictures for looking
fn to_rgb(i: &Img, x0: usize, y0: usize, cw: usize, chh: usize) -> Vec<u8> {
    let mut o = Vec::with_capacity(cw * chh * 3);
    for y in y0..y0 + chh { for x in x0..x0 + cw {
        let p = &i.px[(y * i.w + x) * i.c..];
        if i.c == 4 {
            let a = p[3] as u32;
            // over a grey checker: alpha shows
            let g = if ((x / 4) + (y / 4)) % 2 == 0 { 96 } else { 160 };
            for k in 0..3 { o.push(((p[k] as u32 * a + g * (255 - a)) / 255) as u8); }
        } else { o.extend_from_slice(&p[..3]); }
    } }
    o
}
fn diff_rgb(a: &Img, b: &Img, x0: usize, y0: usize, cw: usize, chh: usize, gain: u32) -> Vec<u8> {
    let mut o = Vec::with_capacity(cw * chh * 3);
    for y in y0..y0 + chh { for x in x0..x0 + cw {
        let i = (y * a.w + x) * a.c;
        let mut m = 0u32;
        for k in 0..a.c.min(4) { m = m.max((a.px[i + k] as i32 - b.px[i + k] as i32).unsigned_abs()); }
        let v = (m * gain).min(255) as u8;
        o.extend_from_slice(&[v, v, v]);
    } }
    o
}
fn scale(rgb: &[u8], w: usize, h: usize, s: usize) -> Vec<u8> {
    let mut o = Vec::with_capacity(rgb.len() * s * s);
    for y in 0..h * s { for x in 0..w * s { let i = ((y / s) * w + x / s) * 3; o.extend_from_slice(&rgb[i..i + 3]); } }
    o
}
fn side_by_side(parts: &[Vec<u8>], w: usize, h: usize, gap: usize) -> (Vec<u8>, usize) {
    let tw = parts.len() * w + (parts.len() - 1) * gap;
    let mut o = vec![255u8; tw * h * 3];
    for (k, p) in parts.iter().enumerate() {
        for y in 0..h { let d = (y * tw + k * (w + gap)) * 3; o[d..d + w * 3].copy_from_slice(&p[y * w * 3..(y + 1) * w * 3]); }
    }
    (o, tw)
}

// the window (cw x ch, stride 4) with the largest summed difference
fn worst_region(a: &Img, b: &Img, cw: usize, chh: usize) -> (usize, usize, u64) {
    let (w, h) = (a.w, a.h);
    let (cw, chh) = (cw.min(w), chh.min(h));
    let mut col = vec![0u64; w * h];
    for i in 0..w * h { let mut s = 0u64; for k in 0..a.c.min(4) { s += (a.px[i * a.c + k] as i64 - b.px[i * a.c + k] as i64).unsigned_abs(); } col[i] = s; }
    // summed-area table
    let mut sat = vec![0u64; (w + 1) * (h + 1)];
    for y in 0..h { for x in 0..w { sat[(y + 1) * (w + 1) + x + 1] = col[y * w + x] + sat[y * (w + 1) + x + 1] + sat[(y + 1) * (w + 1) + x] - sat[y * (w + 1) + x]; } }
    let mut best = (0, 0, 0u64);
    let mut y = 0;
    while y + chh <= h {
        let mut x = 0;
        while x + cw <= w {
            let s = sat[(y + chh) * (w + 1) + x + cw] + sat[y * (w + 1) + x] - sat[y * (w + 1) + x + cw] - sat[(y + chh) * (w + 1) + x];
            if s > best.2 { best = (x, y, s); }
            x += 4;
        }
        y += 4;
    }
    best
}

fn frames(a: &Path, b: &Path, out: &Path, flip: bool, all: bool) -> Result<(), String> {
    let mut names: Vec<String> = fs::read_dir(a).map_err(|e| e.to_string())?.filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().to_string()).filter(|n| n.ends_with(".ppm") && b.join(n).exists()).collect();
    names.sort_by_key(|n| (n.split('-').next().and_then(|f| f.parse::<u64>().ok()).unwrap_or(0), n.clone()));
    if names.is_empty() { return Err("no PPM in both folders".into()); }
    println!("# frame\tPSNR_dB\tSSIM\tmax\tpx_over8\tpx\tpct_px_differ");
    let (mut worst, mut wm) = (String::new(), M { ssim: 2.0, ..Default::default() });
    let (mut ssum, mut psum, mut identical, mut gmax, mut gover) = (0f64, 0f64, 0usize, 0u8, 0usize);
    let mut differ_px = 0usize; let mut total_px = 0usize;
    for n in &names {
        let (ia, ib) = (read_ppm(&a.join(n), flip)?, read_ppm(&b.join(n), flip)?);
        if ia.w != ib.w || ia.h != ib.h { return Err(format!("{n}: sizes differ")); }
        let m = metrics(&ia, &ib, 3);
        let nd = (0..ia.w * ia.h).filter(|&i| ia.px[i * 3..i * 3 + 3] != ib.px[i * 3..i * 3 + 3]).count();
        differ_px += nd; total_px += ia.w * ia.h;
        println!("{n}\t{:.2}\t{:.5}\t{}\t{}\t{}\t{:.2}", m.psnr, m.ssim, m.max, m.over8, m.n, 100.0 * nd as f64 / m.n as f64);
        if m.ssim < wm.ssim || (m.ssim == wm.ssim && m.over8 > wm.over8) { wm = m; worst = n.clone(); }
        ssum += m.ssim; if m.psnr.is_finite() { psum += m.psnr; } else { identical += 1; }
        gmax = gmax.max(m.max); gover = gover.max(m.over8);
        if all { pictures(&ia, &ib, &out.join("frames"), n.trim_end_matches(".ppm"))?; }
    }
    let k = names.len() as f64;
    println!("# {} frames: SSIM mean {:.5}, lowest {:.5} ({worst}: PSNR {:.2} dB, max {}, {} px over 8); identical frames {identical}; PSNR mean of the others {:.2} dB; largest difference anywhere {gmax}; most px over 8 in a frame {gover}; px that differ at all {:.3} %",
             names.len(), ssum / k, wm.ssim, wm.psnr, wm.max, wm.over8,
             if names.len() > identical { psum / (k - identical as f64) } else { f64::INFINITY }, 100.0 * differ_px as f64 / total_px as f64);
    let (ia, ib) = (read_ppm(&a.join(&worst), flip)?, read_ppm(&b.join(&worst), flip)?);
    pictures(&ia, &ib, out, "worst")?;
    println!("# worst frame pictures: {}/worst-frame.png, {}/worst-crop.png", out.display(), out.display());
    Ok(())
}

fn pictures(ia: &Img, ib: &Img, out: &Path, stem: &str) -> Result<(), String> {
    let (w, h) = (ia.w, ia.h);
    let (full, tw) = side_by_side(&[to_rgb(ia, 0, 0, w, h), to_rgb(ib, 0, 0, w, h), diff_rgb(ia, ib, 0, 0, w, h, 8)], w, h, 8);
    write_png(&out.join(format!("{stem}-frame.png")), tw, h, &full)?;
    let (x, y, _) = worst_region(ia, ib, 64, 64);
    let (cw, chh) = (64.min(w), 64.min(h));
    let parts: Vec<Vec<u8>> = [to_rgb(ia, x, y, cw, chh), to_rgb(ib, x, y, cw, chh), diff_rgb(ia, ib, x, y, cw, chh, 8)]
        .iter().map(|p| scale(p, cw, chh, 4)).collect();
    let (crop, tw) = side_by_side(&parts, cw * 4, chh * 4, 8);
    write_png(&out.join(format!("{stem}-crop.png")), tw, chh * 4, &crop)?;
    fs::write(out.join(format!("{stem}-crop.txt")), format!("region x {x} y {y} size {cw}x{chh} (A | B | difference x8), scaled 4x\n")).map_err(|e| e.to_string())?;
    Ok(())
}

fn block_sse(src: &Img, d: &Img, bx: usize, by: usize, bs: usize, ch: usize) -> f64 {
    let mut e = 0f64;
    for y in by * bs..((by + 1) * bs).min(src.h) { for x in bx * bs..((bx + 1) * bs).min(src.w) {
        let i = (y * src.w + x) * 4;
        for k in 0..ch { let v = src.px[i + k] as f64 - d.px[i + k] as f64; e += v * v; }
    } }
    e
}
fn psnr_of(sse: f64, n: usize) -> f64 { if sse == 0.0 { 99.0 } else { 10.0 * (255.0f64 * 255.0 * n as f64 / sse).log10() } }

fn tex(a: &Path, b: &Path, out: &Path, bs: usize, ncrops: usize) -> Result<(), String> {
    let mut by_name: BTreeMap<String, (PathBuf, PathBuf)> = BTreeMap::new();
    for e in fs::read_dir(a).map_err(|e| e.to_string())?.filter_map(|e| e.ok()) {
        let n = e.file_name().to_string_lossy().to_string();
        if let Some(stem) = n.strip_suffix(".dec.rgba") {
            if b.join(&n).exists() { by_name.insert(stem.to_string(), (a.join(&n), b.join(&n))); }
        }
    }
    if by_name.is_empty() { return Err("no .dec.rgba in both folders".into()); }
    fs::create_dir_all(out).map_err(|e| e.to_string())?;
    let mut tsv = String::from("texture\tw\th\tch\tA_PSNR\tB_PSNR\tA_SSIM\tB_SSIM\tA_max\tB_max\tA_over8_pct\tB_over8_pct\tBvsA_SSIM\tBvsA_max\tBvsA_over8_pct\tworst_block_A_PSNR\tworst_block_B_PSNR\tA_worst1pct_PSNR\tB_worst1pct_PSNR\n");
    struct Row { name: String, loss: f64, bx: usize, by: usize, a_w1: f64, b_w1: f64, wa: f64, wb: f64 }
    let mut rows = Vec::new();
    let (mut sa, mut sb, mut nn) = (0f64, 0f64, 0usize);
    let (mut w1a, mut w1b) = (Vec::new(), Vec::new());
    for (stem, (pa, pb)) in &by_name {
        let src = read_rgba(&a.join(format!("{stem}.src.rgba")))?;
        let src_b = read_rgba(&b.join(format!("{stem}.src.rgba")))?;
        if src.px != src_b.px { return Err(format!("{stem}: the two settings were given other source texels")); }
        let (da, db) = (read_rgba(pa)?, read_rgba(pb)?);
        let ch = if src.px.chunks_exact(4).any(|p| p[3] != 255) { 4 } else { 3 };
        let (ma, mb, mab) = (metrics(&src, &da, ch), metrics(&src, &db, ch), metrics(&da, &db, ch));
        sa += ma.sse; sb += mb.sse; nn += src.w * src.h * ch;
        // blocks: the worst one for B relative to A, and the worst 1 % of each
        let (nbx, nby) = (src.w.div_ceil(bs), src.h.div_ceil(bs));
        let mut ea = Vec::with_capacity(nbx * nby); let mut eb = Vec::with_capacity(nbx * nby);
        let (mut best, mut bxy) = (f64::MIN, (0, 0));
        for y in 0..nby { for x in 0..nbx {
            let (e1, e2) = (block_sse(&src, &da, x, y, bs, ch), block_sse(&src, &db, x, y, bs, ch));
            ea.push(e1); eb.push(e2);
            if e2 - e1 > best { best = e2 - e1; bxy = (x, y); }
        } }
        let bn = |e: &[f64]| { let mut v = e.to_vec(); v.sort_by(|p, q| q.partial_cmp(p).unwrap()); let k = (v.len() / 100).max(1); (v[..k].iter().sum::<f64>(), k) };
        let ((s1, k1), (s2, k2)) = (bn(&ea), bn(&eb));
        let px_b = bs * bs * ch;
        let (a_w1, b_w1) = (psnr_of(s1, k1 * px_b), psnr_of(s2, k2 * px_b));
        w1a.push(a_w1); w1b.push(b_w1);
        let (wa, wb) = (psnr_of(ea[bxy.1 * nbx + bxy.0], px_b), psnr_of(eb[bxy.1 * nbx + bxy.0], px_b));
        tsv += &format!("{stem}\t{}\t{}\t{ch}\t{:.2}\t{:.2}\t{:.5}\t{:.5}\t{}\t{}\t{:.3}\t{:.3}\t{:.5}\t{}\t{:.3}\t{:.2}\t{:.2}\t{:.2}\t{:.2}\n",
            src.w, src.h, ma.psnr, mb.psnr, ma.ssim, mb.ssim, ma.max, mb.max, 100.0 * ma.over8 as f64 / ma.n as f64,
            100.0 * mb.over8 as f64 / mb.n as f64, mab.ssim, mab.max, 100.0 * mab.over8 as f64 / mab.n as f64, wa, wb, a_w1, b_w1);
        rows.push(Row { name: stem.clone(), loss: wa - wb, bx: bxy.0, by: bxy.1, a_w1, b_w1, wa, wb });
    }
    fs::write(out.join("tex.tsv"), &tsv).map_err(|e| e.to_string())?;
    let lower = rows.iter().filter(|r| r.b_w1 < r.a_w1 - 0.01).count();
    let mean = |v: &[f64]| v.iter().sum::<f64>() / v.len() as f64;
    println!("# {} textures: all texels PSNR A {:.3} dB, B {:.3} dB; worst-1 % blocks mean A {:.3}, B {:.3}; {lower} textures with B's worst 1 % lower than A's",
             rows.len(), psnr_of(sa, nn), psnr_of(sb, nn), mean(&w1a), mean(&w1b));
    rows.sort_by(|p, q| q.loss.partial_cmp(&p.loss).unwrap());
    for r in rows.iter().take(ncrops) {
        let src = read_rgba(&a.join(format!("{}.src.rgba", r.name)))?;
        let da = read_rgba(&a.join(format!("{}.dec.rgba", r.name)))?;
        let db = read_rgba(&b.join(format!("{}.dec.rgba", r.name)))?;
        let cw = (bs * 5).min(src.w); let chh = (bs * 5).min(src.h);
        let x0 = (r.bx * bs + bs / 2).saturating_sub(cw / 2).min(src.w - cw);
        let y0 = (r.by * bs + bs / 2).saturating_sub(chh / 2).min(src.h - chh);
        let parts: Vec<Vec<u8>> = [to_rgb(&src, x0, y0, cw, chh), to_rgb(&da, x0, y0, cw, chh), to_rgb(&db, x0, y0, cw, chh)]
            .iter().map(|p| scale(p, cw, chh, 8)).collect();
        let (img, tw) = side_by_side(&parts, cw * 8, chh * 8, 8);
        let safe: String = r.name.chars().map(|c| if c.is_ascii_alphanumeric() || c == '_' || c == '-' { c } else { '_' }).collect();
        write_png(&out.join("crops").join(format!("{safe}.png")), tw, chh * 8, &img)?;
        // the whole texture at its size too (source | A | B), for the context
        let parts = [to_rgb(&src, 0, 0, src.w, src.h), to_rgb(&da, 0, 0, src.w, src.h), to_rgb(&db, 0, 0, src.w, src.h)];
        let (img, tw) = side_by_side(&parts, src.w, src.h, 8);
        write_png(&out.join("crops").join(format!("{safe}-whole.png")), tw, src.h, &img)?;
        println!("crop\t{}\tworst block ({}, {}) A {:.2} dB -> B {:.2} dB\t{}", r.name, r.bx, r.by, r.wa, r.wb,
                 out.join("crops").join(format!("{safe}.png")).display());
    }
    Ok(())
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let has = |f: &str| a.iter().any(|x| x == f);
    let opt = |f: &str| a.iter().position(|x| x == f).and_then(|i| a.get(i + 1)).cloned();
    let r = match a.get(1).map(|s| s.as_str()) {
        Some("frames") if a.len() >= 5 => frames(Path::new(&a[2]), Path::new(&a[3]), Path::new(&a[4]), has("--flip"), has("--all")),
        Some("tex") if a.len() >= 5 => tex(Path::new(&a[2]), Path::new(&a[3]), Path::new(&a[4]),
            opt("--block").and_then(|v| v.parse().ok()).unwrap_or(6), opt("--crops").and_then(|v| v.parse().ok()).unwrap_or(12)),
        Some("png") if a.len() >= 4 => read_ppm(Path::new(&a[2]), has("--flip")).and_then(|i| write_png(Path::new(&a[3]), i.w, i.h, &i.px)),
        _ => Err("usage: picdiff frames <A> <B> <out> [--flip] [--all] | tex <A> <B> <out> [--block n] [--crops n] | png <in.ppm> <out.png> [--flip]".into()),
    };
    if let Err(e) = r { eprintln!("picdiff: {e}"); std::process::exit(1); }
}
