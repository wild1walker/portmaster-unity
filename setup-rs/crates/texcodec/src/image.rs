//! RGBA8 images and the Pillow operations textures.py does on them, with Pillow 12.3's arithmetic:
//! premultiplying (convert('RGBa')) and back (convert('RGBA')), flips, crops, and resize with
//! Pillow's own Resample.c (vendor/pillow) behind Image.resize's choices (the box, the integer-crop
//! shortcut, the two-step resize of very tall images).

use crate::decode::{Decoded, Mode};
use crate::Error;

/// An image of 4 bytes a pixel (RGBA, or premultiplied RGBa where a function says so), rows top
/// first unless a function says otherwise.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Rgba {
    pub width: usize,
    pub height: usize,
    pub px: Vec<u8>,
}

impl Rgba {
    pub fn new(width: usize, height: usize) -> Rgba {
        Rgba {
            width,
            height,
            px: vec![0; width * height * 4],
        }
    }

    /// The bytes of rows y0..y1.
    pub fn rows(&self, y0: usize, y1: usize) -> &[u8] {
        &self.px[y0 * self.width * 4..y1 * self.width * 4]
    }

    /// Image.transpose(FLIP_TOP_BOTTOM), in place.
    pub fn flip_rows(&mut self) {
        let line = self.width * 4;
        let (mut a, mut b) = (0, self.height);
        while a + 1 < b {
            b -= 1;
            let (top, bottom) = self.px.split_at_mut(b * line);
            top[a * line..a * line + line].swap_with_slice(&mut bottom[..line]);
            a += 1;
        }
    }

    /// Rows y0..y1 as an image (Pillow's crop((0, y0, width, y1)) inside the image).
    pub fn crop_rows(&self, y0: usize, y1: usize) -> Rgba {
        Rgba {
            width: self.width,
            height: y1 - y0,
            px: self.rows(y0, y1).to_vec(),
        }
    }

    /// paste(part, (0, y)) of an image as wide as this one.
    pub fn paste_rows(&mut self, part: &Rgba, y: usize) {
        let line = self.width * 4;
        self.px[y * line..y * line + part.px.len()].copy_from_slice(&part.px);
    }

    /// convert('RGBa') of an RGBA image, in place (Convert.c rgbA2rgba: MULDIV255).
    pub fn premultiply(&mut self) {
        premultiply_px(&mut self.px);
    }

    /// convert('RGBA') of an RGBa image, in place (Convert.c rgba2rgbA).
    pub fn unpremultiply(&mut self) {
        unpremultiply_px(&mut self.px);
    }
}

/// Rgba::premultiply on pixels (any run of whole pixels: a band of an image).
pub fn premultiply_px(px: &mut [u8]) {
    #[cfg(target_arch = "aarch64")]
    let px = {
        let n = px.len() / 16 * 16;
        // SAFETY: NEON is part of aarch64; the loads and stores stay inside px[..n].
        unsafe { premultiply_neon(&mut px[..n]) };
        &mut px[n..]
    };
    for p in px.chunks_exact_mut(4) {
        let a = p[3] as u32;
        for c in &mut p[..3] {
            let t = *c as u32 * a + 128;
            *c = (((t >> 8) + t) >> 8) as u8;
        }
    }
}

/// MULDIV255 four pixels at a time in 16-bit lanes (c * a + 128 is at most 65153, and adding its
/// high byte at most 65407: no lane overflows, so the bytes are the scalar ones).
#[cfg(target_arch = "aarch64")]
unsafe fn premultiply_neon(px: &mut [u8]) {
    use std::arch::aarch64::*;
    // (byte 3 of each pixel: its alpha, to every byte of the pixel)
    let idx: [u8; 16] = [3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15];
    let idx = vld1q_u8(idx.as_ptr());
    let keep: [u8; 16] = [0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255];
    let keep = vld1q_u8(keep.as_ptr());
    let r = vdupq_n_u16(128);
    for c in px.chunks_exact_mut(16) {
        let v = vld1q_u8(c.as_ptr());
        let a = vqtbl1q_u8(v, idx);
        let lo = vmlal_u8(r, vget_low_u8(v), vget_low_u8(a));
        let hi = vmlal_u8(r, vget_high_u8(v), vget_high_u8(a));
        let lo = vshrq_n_u16::<8>(vaddq_u16(lo, vshrq_n_u16::<8>(lo)));
        let hi = vshrq_n_u16::<8>(vaddq_u16(hi, vshrq_n_u16::<8>(hi)));
        let m = vcombine_u8(vmovn_u16(lo), vmovn_u16(hi));
        // (alpha itself kept)
        vst1q_u8(c.as_mut_ptr(), vbslq_u8(keep, v, m));
    }
}

/// floor(2^24 / a) + 1 for a in 1..=255 (0 for 0): floor(x * R[a] / 2^24) is floor(x / a) for
/// every x up to 255 * 255 (the error x / 2^24 is under 1/256, less than the 1/a any fraction
/// below 1 leaves; checked over every x and a by the tests).
const RECIP: [u32; 256] = {
    let mut t = [0u32; 256];
    let mut a = 1;
    while a < 256 {
        t[a] = (1u32 << 24) / a as u32 + 1;
        a += 1;
    }
    t
};

/// Rgba::unpremultiply on pixels: 255 * c / a (at most 255) by a reciprocal, the same quotients.
pub fn unpremultiply_px(px: &mut [u8]) {
    for p in px.chunks_exact_mut(4) {
        let a = p[3] as usize;
        if a != 255 && a != 0 {
            let r = RECIP[a] as u64;
            for c in &mut p[..3] {
                *c = (((255 * *c as u64) * r) >> 24).min(255) as u8;
            }
        }
    }
}

/// textures.py texture_image(): a decoded texture -> the RGBA a GPU samples from it (BC4's
/// luminance as (r, 0, 0, 1), RGB with alpha 255).
pub fn texture_image(img: Decoded, bc4: bool) -> Rgba {
    let (width, height) = (img.width, img.height);
    if bc4 {
        // UnityPy decodes BC4 as "L"; Image.merge('RGBA', (r, zero, zero, 255))
        let mut px = Vec::with_capacity(width * height * 4);
        let bpp = crate::decode::bytes_per_pixel(img.mode);
        for p in img.px.chunks_exact(bpp) {
            px.extend_from_slice(&[p[0], 0, 0, 255]);
        }
        return Rgba { width, height, px };
    }
    match img.mode {
        Mode::Rgba => Rgba {
            width,
            height,
            px: img.px,
        },
        Mode::Rgb => {
            let mut px = img.px;
            for p in px.chunks_exact_mut(4) {
                p[3] = 255;
            }
            Rgba { width, height, px }
        }
        Mode::L => {
            let mut px = Vec::with_capacity(width * height * 4);
            for &v in &img.px {
                px.extend_from_slice(&[v, v, v, 255]);
            }
            Rgba { width, height, px }
        }
    }
}

extern "C" {
    fn tc_resample(
        input: *const u8,
        w: i32,
        h: i32,
        out: *mut u8,
        ow: i32,
        oh: i32,
        filter: i32,
        boxp: *const f32,
        pillow: i32,
    ) -> i32;
    fn tc_resample_rows(
        rows: *const *const u8,
        w: i32,
        h: i32,
        out: *mut u8,
        ow: i32,
        oh: i32,
        filter: i32,
        boxp: *const f32,
        pillow: i32,
    ) -> i32;
}

/// Pillow's IMAGING_TRANSFORM_LANCZOS.
const LANCZOS: i32 = 1;

/// Image.resize((w, h), LANCZOS, box) of a premultiplied (RGBa) image: Pillow 12.3's choices
/// (Image.resize, then _imaging.c's _resize) and its Resample.c. `bx`: the box as Python floats
/// (None: the whole image). Errors where Pillow raises.
pub fn resize(img: &Rgba, w: usize, h: usize, bx: Option<[f64; 4]>) -> Result<Rgba, Error> {
    resize_with(img, w, h, bx, false)
}

/// resize() with Pillow's own resampling loops, not the vector ones (the same bytes: the tests
/// compare them).
pub fn resize_pillow(img: &Rgba, w: usize, h: usize, bx: Option<[f64; 4]>) -> Result<Rgba, Error> {
    resize_with(img, w, h, bx, true)
}

fn resize_with(
    img: &Rgba,
    w: usize,
    h: usize,
    bx: Option<[f64; 4]>,
    pillow: bool,
) -> Result<Rgba, Error> {
    let bx = bx.unwrap_or([0.0, 0.0, img.width as f64, img.height as f64]);
    if (img.width, img.height) == (w, h) && bx == [0.0, 0.0, w as f64, h as f64] {
        return Ok(img.clone());
    }
    if img.height > img.width * 100 && h < img.height {
        // (very tall images: the height first, then the width)
        let tall = im_resize(
            img,
            img.width,
            h,
            [0.0, bx[1], img.width as f64, bx[3]],
            pillow,
        )?;
        return im_resize(&tall, w, h, [bx[0], 0.0, bx[2], h as f64], pillow);
    }
    im_resize(img, w, h, bx, pillow)
}

/// _imaging.c's _resize with a filter other than NEAREST: the box taken as C floats.
fn im_resize(img: &Rgba, w: usize, h: usize, bx: [f64; 4], pillow: bool) -> Result<Rgba, Error> {
    let b = bx.map(|v| v as f32);
    if w < 1 || h < 1 {
        return Err(Error::Resize("height and width must be > 0".into()));
    }
    if b[0] < 0.0 || b[1] < 0.0 {
        return Err(Error::Resize("box offset can't be negative".into()));
    }
    if b[2] > img.width as f32 || b[3] > img.height as f32 {
        return Err(Error::Resize("box can't exceed original image size".into()));
    }
    if b[2] - b[0] < 0.0 || b[3] - b[1] < 0.0 {
        return Err(Error::Resize("box can't be empty".into()));
    }
    let mut out = Rgba::new(w, h);
    // integer box of the requested size: a crop
    if b[0] - (b[0] as i32) as f32 == 0.0
        && b[2] - b[0] == w as f32
        && b[1] - (b[1] as i32) as f32 == 0.0
        && b[3] - b[1] == h as f32
    {
        let (x0, y0) = (b[0] as i32 as isize, b[1] as i32 as isize);
        // (ImagingCrop: pixels outside the image are 0)
        for y in 0..h {
            for x in 0..w {
                let (sx, sy) = (x0 + x as isize, y0 + y as isize);
                if sx >= 0 && sy >= 0 && (sx as usize) < img.width && (sy as usize) < img.height {
                    let s = (sy as usize * img.width + sx as usize) * 4;
                    let d = (y * w + x) * 4;
                    out.px[d..d + 4].copy_from_slice(&img.px[s..s + 4]);
                }
            }
        }
        return Ok(out);
    }
    // SAFETY: the buffers have the sizes passed; tc_resample reads `img` and writes `out` only.
    let r = unsafe {
        tc_resample(
            img.px.as_ptr(),
            img.width as i32,
            img.height as i32,
            out.px.as_mut_ptr(),
            w as i32,
            h as i32,
            LANCZOS,
            b.as_ptr(),
            pillow as i32,
        )
    };
    if r != 0 {
        return Err(Error::Resize("resampling failed (out of memory)".into()));
    }
    Ok(out)
}

/// resize() of the image whose rows (top first, `width` pixels each) are `rows`, into `out` (w x h
/// pixels): the same bytes, without the rows put together first.
pub fn resize_rows_into(
    rows: &[&[u8]],
    width: usize,
    out: &mut [u8],
    w: usize,
    h: usize,
    bx: [f64; 4],
) -> Result<(), Error> {
    let height = rows.len();
    assert!(rows.iter().all(|r| r.len() == width * 4) && out.len() == w * h * 4);
    let b = bx.map(|v| v as f32);
    // (Pillow's other paths, and its refusals: as resize() takes them, from the rows put together)
    let plain = !((width, height) == (w, h) && bx == [0.0, 0.0, w as f64, h as f64])
        && height <= width * 100
        && w >= 1
        && h >= 1
        && b[0] >= 0.0
        && b[1] >= 0.0
        && b[2] <= width as f32
        && b[3] <= height as f32
        && b[2] - b[0] >= 0.0
        && b[3] - b[1] >= 0.0
        && !(b[0] - (b[0] as i32) as f32 == 0.0
            && b[2] - b[0] == w as f32
            && b[1] - (b[1] as i32) as f32 == 0.0
            && b[3] - b[1] == h as f32);
    if !plain {
        let img = Rgba {
            width,
            height,
            px: rows.concat(),
        };
        out.copy_from_slice(&resize(&img, w, h, Some(bx))?.px);
        return Ok(());
    }
    let ptrs: Vec<*const u8> = rows.iter().map(|r| r.as_ptr()).collect();
    // SAFETY: `height` rows of width * 4 bytes, and out of w * h * 4 (checked above); tc_resample_rows
    // reads the rows and writes `out` only.
    let r = unsafe {
        tc_resample_rows(
            ptrs.as_ptr(),
            width as i32,
            height as i32,
            out.as_mut_ptr(),
            w as i32,
            h as i32,
            LANCZOS,
            b.as_ptr(),
            0,
        )
    };
    if r != 0 {
        return Err(Error::Resize("resampling failed (out of memory)".into()));
    }
    Ok(())
}

/// textures.py downscale(): an RGBA image -> w x h, filtered with premultiplied alpha.
pub fn downscale(img: Rgba, w: usize, h: usize) -> Result<Rgba, Error> {
    if (img.width, img.height) == (w, h) {
        return Ok(img);
    }
    let mut pm = img;
    pm.premultiply();
    let mut out = resize(&pm, w, h, None)?;
    out.unpremultiply();
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn premultiply_matches_pillow_formulas() {
        let mut img = Rgba {
            width: 3,
            height: 1,
            px: vec![200, 100, 7, 128, 255, 1, 0, 0, 10, 20, 30, 255],
        };
        img.premultiply();
        // MULDIV255(200, 128) = 100, (100, 128) = 50, (7, 128) = 4 (3.51 rounds up)
        assert_eq!(img.px, vec![100, 50, 4, 128, 0, 0, 0, 0, 10, 20, 30, 255]);
        img.unpremultiply();
        // 255 * 100 / 128 = 199, 255 * 50 / 128 = 99, 255 * 4 / 128 = 7
        assert_eq!(img.px, vec![199, 99, 7, 128, 0, 0, 0, 0, 10, 20, 30, 255]);
    }

    #[test]
    fn premultiply_and_back_match_the_formulas_for_every_value() {
        let mut px = Vec::with_capacity(256 * 256 * 4 + 8);
        for a in 0..=255u8 {
            for c in 0..=255u8 {
                px.extend_from_slice(&[c, 255 - c, c / 3, a]);
            }
        }
        px.extend_from_slice(&[7, 9, 200, 31, 0, 0, 0, 0]);
        let mut pre = px.clone();
        premultiply_px(&mut pre);
        let mut back = px.clone();
        unpremultiply_px(&mut back);
        for (i, p) in px.chunks_exact(4).enumerate() {
            let a = p[3] as u32;
            for k in 0..3 {
                let t = p[k] as u32 * a + 128;
                assert_eq!(pre[4 * i + k] as u32, ((t >> 8) + t) >> 8);
                let u = if a == 0 || a == 255 {
                    p[k] as u32
                } else {
                    (255 * p[k] as u32 / a).min(255)
                };
                assert_eq!(back[4 * i + k] as u32, u);
            }
            assert_eq!((pre[4 * i + 3], back[4 * i + 3]), (p[3], p[3]));
        }
    }

    #[test]
    fn flip_rows_reverses_rows() {
        let mut img = Rgba {
            width: 1,
            height: 3,
            px: (0..12).collect(),
        };
        img.flip_rows();
        assert_eq!(img.px, vec![8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3]);
    }

    #[test]
    fn resize_keeps_a_flat_image_flat() {
        let img = Rgba {
            width: 16,
            height: 8,
            px: [9, 80, 160, 255].repeat(16 * 8),
        };
        let out = resize(&img, 6, 3, None).unwrap();
        assert_eq!(out.px, [9, 80, 160, 255].repeat(6 * 3));
    }

    #[test]
    fn resize_integer_box_of_the_same_size_crops() {
        let img = Rgba {
            width: 4,
            height: 4,
            px: (0..64).collect(),
        };
        let out = resize(&img, 2, 2, Some([1.0, 2.0, 3.0, 4.0])).unwrap();
        assert_eq!(
            out.px,
            [36, 37, 38, 39, 40, 41, 42, 43, 52, 53, 54, 55, 56, 57, 58, 59]
        );
    }

    /// The vector passes against Pillow's loops: images of noise, hard edges and alpha (sums
    /// past 0-255 at the edges, clamped), scaled down and up, with boxes, and wide and narrow.
    #[test]
    fn vector_resampling_matches_pillow() {
        let mut seed = 0x2545_f491_4f6c_dd1du64;
        let mut rnd = || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };
        let sizes = [
            (37, 23, 14, 9),
            (64, 64, 24, 24),
            (300, 41, 113, 15),
            (17, 200, 7, 75),
            (9, 9, 31, 20),
            (129, 3, 48, 1),
            (1, 50, 1, 19),
            (513, 257, 192, 96),
            (800, 600, 300, 225),
            (300, 300, 150, 150),
            (40, 30, 120, 90),
        ];
        for (k, &(w, h, ow, oh)) in sizes.iter().enumerate() {
            for kind in 0..5 {
                let mut px = vec![0u8; w * h * 4];
                for (i, p) in px.chunks_exact_mut(4).enumerate() {
                    let r = rnd();
                    match kind {
                        0 => p.copy_from_slice(&r.to_le_bytes()[..4]),
                        // hard edges: 0 / 255 stripes and checks (Lanczos rings past both ends)
                        1 => {
                            let on = ((i % w) / 2 + (i / w) / 3) % 2 == 0;
                            p.copy_from_slice(if on { &[255, 255, 255, 255] } else { &[0, 0, 0, 0] });
                        }
                        2 => {
                            let a = (r >> 32) as u8;
                            let b = r.to_le_bytes();
                            p.copy_from_slice(&[b[0].min(a), b[1].min(a), b[2].min(a), a]);
                        }
                        // flat tiles of a few sizes (the windows that read one pixel only), a
                        // pixel of noise here and there
                        3 => {
                            let (x, y) = (i % w, i / w);
                            let t = ((x / (7 + y / 50) + 3 * (y / 13)) % 5) as u8;
                            let c = [t * 60, 255 - t * 50, t * 17, if t == 0 { 0 } else { 255 - t }];
                            if r % 97 == 0 {
                                p.copy_from_slice(&r.to_le_bytes()[..4]);
                            } else {
                                p.copy_from_slice(&c);
                            }
                        }
                        // columns: every row the same but a few (the vertical pass's flat windows)
                        _ => {
                            let (x, y) = (i % w, i / w);
                            let v = ((x * 37) % 251) as u8;
                            if y % 29 == 5 {
                                p.copy_from_slice(&[255 - v, v, 9, 200]);
                            } else {
                                p.copy_from_slice(&[v, v / 2, 255 - v, 255]);
                            }
                        }
                    }
                }
                let img = Rgba { width: w, height: h, px };
                let boxes = [
                    None,
                    Some([0.5, 1.25, w as f64 - 0.75, h as f64 - 0.5]),
                    Some([0.0, h as f64 / 3.0, w as f64, h as f64]),
                ];
                for bx in boxes {
                    if bx.is_some() && (w < 4 || h < 4) {
                        continue;
                    }
                    let a = resize(&img, ow, oh, bx).unwrap();
                    let b = resize_pillow(&img, ow, oh, bx).unwrap();
                    assert_eq!(a, b, "size {k}, kind {kind}, box {bx:?}");
                }
            }
        }
    }

    #[test]
    fn resize_rejects_a_box_past_the_image() {
        let img = Rgba::new(4, 4);
        assert!(resize(&img, 2, 2, Some([0.0, 0.0, 4.0, 4.5])).is_err());
    }
}
