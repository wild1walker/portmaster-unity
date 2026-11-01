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
        for p in self.px.chunks_exact_mut(4) {
            let a = p[3] as u32;
            for c in &mut p[..3] {
                let t = *c as u32 * a + 128;
                *c = (((t >> 8) + t) >> 8) as u8;
            }
        }
    }

    /// convert('RGBA') of an RGBa image, in place (Convert.c rgba2rgbA).
    pub fn unpremultiply(&mut self) {
        for p in self.px.chunks_exact_mut(4) {
            let a = p[3] as u32;
            if a != 255 && a != 0 {
                for c in &mut p[..3] {
                    *c = ((255 * *c as u32) / a).min(255) as u8;
                }
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
    ) -> i32;
}

/// Pillow's IMAGING_TRANSFORM_LANCZOS.
const LANCZOS: i32 = 1;

/// Image.resize((w, h), LANCZOS, box) of a premultiplied (RGBa) image: Pillow 12.3's choices
/// (Image.resize, then _imaging.c's _resize) and its Resample.c. `bx`: the box as Python floats
/// (None: the whole image). Errors where Pillow raises.
pub fn resize(img: &Rgba, w: usize, h: usize, bx: Option<[f64; 4]>) -> Result<Rgba, Error> {
    let bx = bx.unwrap_or([0.0, 0.0, img.width as f64, img.height as f64]);
    if (img.width, img.height) == (w, h) && bx == [0.0, 0.0, w as f64, h as f64] {
        return Ok(img.clone());
    }
    if img.height > img.width * 100 && h < img.height {
        // (very tall images: the height first, then the width)
        let tall = im_resize(img, img.width, h, [0.0, bx[1], img.width as f64, bx[3]])?;
        return im_resize(&tall, w, h, [bx[0], 0.0, bx[2], h as f64]);
    }
    im_resize(img, w, h, bx)
}

/// _imaging.c's _resize with a filter other than NEAREST: the box taken as C floats.
fn im_resize(img: &Rgba, w: usize, h: usize, bx: [f64; 4]) -> Result<Rgba, Error> {
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
        )
    };
    if r != 0 {
        return Err(Error::Resize("resampling failed (out of memory)".into()));
    }
    Ok(out)
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

    #[test]
    fn resize_rejects_a_box_past_the_image() {
        let img = Rgba::new(4, 4);
        assert!(resize(&img, 2, 2, Some([0.0, 0.0, 4.0, 4.5])).is_err());
    }
}
