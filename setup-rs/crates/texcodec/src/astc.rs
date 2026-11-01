//! ASTC encoding as textures.py's `Astc` does it: astc-encoder contexts (vendor/astc-encoder, through
//! csrc/astcenc_capi.cpp) per block footprint and effort, and the guided mode: guided_astc
//! (vendor/guided_astc) writes the blocks that need no search, the others are gathered by effort
//! (the line / near / other lists, or the guide's tiers) and encoded by astc-encoder, each block on
//! its own, then put in their places.

use std::collections::HashMap;
use std::ffi::{c_char, c_int, c_uint, c_void, CStr};
use std::sync::Once;

use crate::guide::{self, Guide, Limits};
use crate::Error;

/// The astc-encoder presets textures.py takes, with astcenc's quality values.
pub const PRESETS: [(&str, f32); 4] = [
    ("fastest", 0.0),
    ("fast", 10.0),
    ("medium", 60.0),
    ("thorough", 98.0),
];

/// ASTCENC_FLG_USE_DECODE_UNORM8, the flag textures.py sets.
const USE_DECODE_UNORM8: c_uint = 1 << 1;

/// Blocks across the image of the blocks left to the encoder (Astc.GATHER_COLS).
pub const GATHER_COLS: usize = 256;

#[allow(clippy::too_many_arguments)]
extern "C" {
    fn tc_astc_new(
        bw: c_uint,
        bh: c_uint,
        quality: f32,
        flags: c_uint,
        partition_limit: c_int,
        index2_limit: c_int,
        threads: c_uint,
        status: *mut c_int,
    ) -> *mut c_void;
    fn tc_astc_compress(
        ctx: *mut c_void,
        rgba: *const u8,
        w: c_uint,
        h: c_uint,
        out: *mut u8,
        out_len: usize,
        thread_index: c_uint,
    ) -> c_int;
    fn tc_astc_reset(ctx: *mut c_void) -> c_int;
    fn tc_astc_free(ctx: *mut c_void);
    fn tc_astc_error(status: c_int) -> *const c_char;
    fn ga_init();
    fn ga_encode(
        rgba: *const u8,
        w: c_int,
        h: c_int,
        bw: c_int,
        bh: c_int,
        max_err: c_int,
        threads: c_int,
        line: c_int,
        near: c_int,
        dxt: *const u8,
        dxt5: c_int,
        out: *mut u8,
        cls: *mut u8,
    ) -> i64;
}

fn astc_error(status: c_int) -> Error {
    // SAFETY: astcenc_get_error_string returns a static string or NULL.
    let s = unsafe { tc_astc_error(status) };
    let msg = if s.is_null() {
        format!("astcenc error {status}")
    } else {
        unsafe { CStr::from_ptr(s) }.to_string_lossy().into_owned()
    };
    Error::Astc(msg)
}

/// An astc-encoder context (astc_encoder_py's ASTCContext) for `threads` threads.
pub struct Context {
    ptr: *mut c_void,
    threads: usize,
}

// SAFETY: the context is used by one compress call at a time (&mut self), whose threads each pass
// their own index, as astcenc requires.
unsafe impl Send for Context {}

impl Context {
    /// An LDR context for blocks `block` at `quality` (astcenc's value), USE_DECODE_UNORM8, with
    /// `limits` lowered after the preset (as textures.py's setattr(cfg, k, min(v, ...))).
    pub fn new(
        block: (u32, u32),
        quality: f32,
        limits: Limits,
        threads: usize,
    ) -> Result<Context, Error> {
        let mut status = 0;
        let lim = |v: Option<u32>| v.map_or(-1, |v| v as c_int);
        // SAFETY: plain values in, a context pointer (or NULL and a status) out.
        let ptr = unsafe {
            tc_astc_new(
                block.0,
                block.1,
                quality,
                USE_DECODE_UNORM8,
                lim(limits.partitions),
                lim(limits.index2),
                threads as c_uint,
                &mut status,
            )
        };
        if ptr.is_null() {
            return Err(astc_error(status));
        }
        Ok(Context { ptr, threads })
    }

    /// ASTCContext.compress(ASTCImage(U8, w, h, 1, rgba), RGBA swizzle): every thread index on
    /// its own thread (as astc_encoder_py's std::async), then the reset.
    pub fn compress(
        &mut self,
        rgba: &[u8],
        w: usize,
        h: usize,
        block: (u32, u32),
    ) -> Result<Vec<u8>, Error> {
        assert_eq!(rgba.len(), w * h * 4);
        let (bw, bh) = (block.0 as usize, block.1 as usize);
        let len = w.div_ceil(bw) * h.div_ceil(bh) * 16;
        let mut out = vec![0u8; len];
        let ctx = self.ptr as usize;
        let out_ptr = out.as_mut_ptr() as usize;
        let run = |i: usize| -> c_int {
            // SAFETY: astcenc_compress_image may run concurrently with distinct thread indices on
            // one context; each writes only its own blocks of `out`, which outlives the threads.
            unsafe {
                tc_astc_compress(
                    ctx as *mut c_void,
                    rgba.as_ptr(),
                    w as c_uint,
                    h as c_uint,
                    out_ptr as *mut u8,
                    len,
                    i as c_uint,
                )
            }
        };
        let mut status = 0;
        if self.threads > 1 {
            std::thread::scope(|s| {
                let handles: Vec<_> = (0..self.threads).map(|i| s.spawn(move || run(i))).collect();
                for h in handles {
                    let st = h.join().unwrap_or(-1);
                    if st != 0 {
                        status = st;
                    }
                }
            });
        } else {
            status = run(0);
        }
        // SAFETY: every compress call has returned.
        let reset = unsafe { tc_astc_reset(self.ptr) };
        if status != 0 {
            return Err(astc_error(status));
        }
        if reset != 0 {
            return Err(astc_error(reset));
        }
        Ok(out)
    }
}

impl Drop for Context {
    fn drop(&mut self) {
        // SAFETY: allocated by tc_astc_new, freed once.
        unsafe { tc_astc_free(self.ptr) }
    }
}

/// What a group of hard blocks is searched with: one of textures.py's TIERS at the quality preset,
/// or a guide tier (astc_guide.TIERS[i]).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Tier {
    Line,
    Near,
    Other,
    Guide(usize),
}

/// textures.py's settings for the encoder (Astc's arguments).
#[derive(Clone, Debug)]
pub struct EncoderSettings {
    /// "fastest" | "fast" | "medium" | "thorough"
    pub quality: String,
    /// encoder threads (textures.py: c['threads'] or os.cpu_count())
    pub threads: usize,
    /// guided (guided_astc first) or search (every block through astc-encoder)
    pub guided: bool,
    pub max_err: i32,
    pub line: i32,
    pub near: i32,
}

/// textures.py's `Astc`: contexts kept for the whole run, the guided blocks' counts, the guide.
pub struct Encoder {
    pub settings: EncoderSettings,
    quality: f32,
    ctx: HashMap<((u32, u32), Tier), Context>,
    /// guided: one colour, fitted, DXT seed, one partition, two partitions, searched
    pub counts: [u64; 6],
    pub guide: Option<Guide>,
}

/// The DXT1 or DXT5 blocks an unscaled level was decoded from, in the order the encoder is given
/// its rows (textures.py dxt_seed / strip_blocks), and whether they are DXT5.
pub struct Seed<'a> {
    pub blocks: &'a [u8],
    pub dxt5: bool,
}

impl Encoder {
    pub fn new(settings: EncoderSettings, guide: Option<Guide>) -> Result<Encoder, Error> {
        let quality = preset(&settings.quality)?;
        static INIT: Once = Once::new();
        // SAFETY: guided_astc's table set-up, once, before any encode.
        INIT.call_once(|| unsafe { ga_init() });
        Ok(Encoder {
            settings,
            quality,
            ctx: HashMap::new(),
            counts: [0; 6],
            guide,
        })
    }

    fn context(&mut self, block: (u32, u32), tier: Tier) -> Result<&mut Context, Error> {
        let key = (block, tier);
        if !self.ctx.contains_key(&key) {
            let (q, limits) = match tier {
                Tier::Guide(i) => (preset(guide::TIERS[i].0)?, guide::TIERS[i].1),
                Tier::Line => (
                    self.quality,
                    Limits {
                        partitions: Some(1),
                        index2: None,
                    },
                ),
                Tier::Near => (
                    self.quality,
                    Limits {
                        partitions: Some(2),
                        index2: None,
                    },
                ),
                Tier::Other => (self.quality, Limits::NONE),
            };
            self.ctx
                .insert(key, Context::new(block, q, limits, self.settings.threads)?);
        }
        Ok(self.ctx.get_mut(&key).unwrap())
    }

    /// Astc.encode_rows: an RGBA image whose rows are in Unity's order (bottom first) -> its ASTC
    /// blocks. key: the texture level's name in the guide ("rel|path id|level").
    pub fn encode_rows(
        &mut self,
        rgba: &[u8],
        w: usize,
        h: usize,
        block: (u32, u32),
        seed: Option<Seed>,
        key: Option<&str>,
    ) -> Result<Vec<u8>, Error> {
        if !self.settings.guided {
            return self
                .context(block, Tier::Other)?
                .compress(rgba, w, h, block);
        }
        self.guided_rows(rgba, w, h, block, seed, key)
    }

    fn guided_rows(
        &mut self,
        rgba: &[u8],
        w: usize,
        h: usize,
        block: (u32, u32),
        seed: Option<Seed>,
        key: Option<&str>,
    ) -> Result<Vec<u8>, Error> {
        let (bw, bh) = (block.0 as usize, block.1 as usize);
        if w == 0 || h == 0 || !(4..=12).contains(&bw) || !(4..=12).contains(&bh) {
            return Err(Error::Astc("bad size or footprint".into()));
        }
        let (bx, by) = (w.div_ceil(bw), h.div_ceil(bh));
        let nb = bx * by;
        let seed = seed.filter(|_| block == (4, 4));
        if let Some(s) = &seed {
            if s.blocks.len() != nb * if s.dxt5 { 16 } else { 8 } {
                return Err(Error::Astc(
                    "dxt: one DXT block per 4x4 block expected".into(),
                ));
            }
        }
        let mut blocks = vec![0u8; nb * 16];
        let mut cls = vec![0u8; nb];
        let s = &self.settings;
        // SAFETY: the buffers have the sizes ga_encode reads and writes (checked above).
        unsafe {
            ga_encode(
                rgba.as_ptr(),
                w as c_int,
                h as c_int,
                bw as c_int,
                bh as c_int,
                s.max_err,
                s.threads as c_int,
                s.line,
                s.near,
                seed.as_ref()
                    .map_or(std::ptr::null(), |s| s.blocks.as_ptr()),
                seed.as_ref().is_some_and(|s| s.dxt5) as c_int,
                blocks.as_mut_ptr(),
                cls.as_mut_ptr(),
            );
        }
        // (guided_astc's encode(): counts and the line / near / other lists from the classes)
        const SLOT: [usize; 6] = [5, 0, 1, 2, 3, 4];
        for &c in &cls {
            self.counts[SLOT[c as usize]] += 1;
        }
        let mut rests: [Vec<u8>; 3] = Default::default();
        for (b, &c) in cls.iter().enumerate() {
            let k = match c {
                4 => 0, // LINE
                5 => 1, // NEAR
                0 => 2, // OTHER
                _ => continue,
            };
            rests[k].extend_from_slice(&(b as u32).to_le_bytes());
        }
        drop(cls);
        let mut groups: Vec<(Tier, Vec<u8>)> = vec![
            (Tier::Line, std::mem::take(&mut rests[0])),
            (Tier::Near, std::mem::take(&mut rests[1])),
            (Tier::Other, std::mem::take(&mut rests[2])),
        ];
        if let (Some(key), Some(g)) = (key, self.guide.as_mut()) {
            if groups.iter().any(|(_, r)| !r.is_empty()) {
                let hard: Vec<u8> = groups.iter().flat_map(|(_, r)| r.iter().copied()).collect();
                if let Some(codes) = g.codes(key, &hard)? {
                    groups = guide::split(&hard, &codes)
                        .into_iter()
                        .enumerate()
                        .map(|(i, p)| (Tier::Guide(i), p))
                        .collect();
                }
            }
        }
        for (tier, rest) in groups {
            let n = rest.len() / 4;
            if n == 0 {
                continue;
            }
            let cols = n.min(GATHER_COLS);
            let sub = gather(rgba, w, h, bw, bh, &rest, cols);
            let enc = self.context(block, tier)?.compress(
                &sub,
                cols * bw,
                n.div_ceil(cols) * bh,
                block,
            )?;
            place(&mut blocks, &enc, &rest)?;
        }
        Ok(blocks)
    }
}

/// The astcenc quality value of a preset name.
pub fn preset(name: &str) -> Result<f32, Error> {
    PRESETS
        .iter()
        .find(|(n, _)| *n == name)
        .map(|(_, q)| *q)
        .ok_or_else(|| {
            Error::Config(format!(
                "ASTC quality {name:?}: one of fastest, fast, medium, thorough"
            ))
        })
}

/// guided_astc's load_block: a block's texels, past the image's edge its last row and column.
fn load_block(
    img: &[u8],
    w: usize,
    h: usize,
    bw: usize,
    bh: usize,
    x: usize,
    y: usize,
    px: &mut [u8],
) {
    for j in 0..bh {
        let yy = (y * bh + j).min(h - 1);
        for i in 0..bw {
            let xx = (x * bw + i).min(w - 1);
            let s = 4 * (yy * w + xx);
            px[4 * (j * bw + i)..4 * (j * bw + i) + 4].copy_from_slice(&img[s..s + 4]);
        }
    }
}

/// guided_astc's gather(): the texels of the blocks in rest as an image of `cols` blocks across,
/// the last row filled with copies of the first block.
pub fn gather(
    img: &[u8],
    w: usize,
    h: usize,
    bw: usize,
    bh: usize,
    rest: &[u8],
    cols: usize,
) -> Vec<u8> {
    let n = rest.len() / 4;
    let bx = w.div_ceil(bw);
    let rows = n.div_ceil(cols);
    let wide = cols * bw;
    let mut dst = vec![0u8; wide * rows * bh * 4];
    let mut px = vec![0u8; 4 * bw * bh];
    for k in 0..rows * cols {
        let i = if k < n { k } else { 0 };
        let b = u32::from_le_bytes(rest[4 * i..4 * i + 4].try_into().unwrap()) as usize;
        load_block(img, w, h, bw, bh, b % bx, b / bx, &mut px);
        let (ox, oy) = ((k % cols) * bw, (k / cols) * bh);
        for j in 0..bh {
            let d = 4 * ((oy + j) * wide + ox);
            dst[d..d + 4 * bw].copy_from_slice(&px[4 * j * bw..4 * (j + 1) * bw]);
        }
    }
    dst
}

/// guided_astc's place(): the encoded blocks (gather's order) into their places.
pub fn place(blocks: &mut [u8], enc: &[u8], rest: &[u8]) -> Result<(), Error> {
    let n = rest.len() / 4;
    if enc.len() < n * 16 {
        return Err(Error::Astc("place: sizes do not match".into()));
    }
    for (i, pos) in rest.chunks_exact(4).enumerate() {
        let b = u32::from_le_bytes(pos.try_into().unwrap()) as usize;
        if (b + 1) * 16 > blocks.len() {
            return Err(Error::Astc("place: sizes do not match".into()));
        }
        blocks[16 * b..16 * b + 16].copy_from_slice(&enc[16 * i..16 * i + 16]);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn settings(guided: bool) -> EncoderSettings {
        EncoderSettings {
            quality: "fast".into(),
            threads: 2,
            guided,
            max_err: 64,
            line: 128,
            near: 1024,
        }
    }

    #[test]
    fn one_colour_is_a_void_extent_block() {
        let img = [10u8, 20, 30, 255].repeat(8 * 4);
        let mut e = Encoder::new(settings(true), None).unwrap();
        let out = e.encode_rows(&img, 8, 4, (4, 4), None, None).unwrap();
        let ve = [
            0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 10, 10, 20, 20, 30, 30, 255, 255,
        ];
        assert_eq!(out, [ve, ve].concat());
        assert_eq!(e.counts[0], 2);
        // astc-encoder writes the same block for it
        let mut s = Encoder::new(settings(false), None).unwrap();
        assert_eq!(s.encode_rows(&img, 8, 4, (4, 4), None, None).unwrap(), out);
    }

    #[test]
    fn guided_blocks_match_the_encoder_for_searched_blocks() {
        // a noisy image: every block is left to astc-encoder ("other"), so the guided output is
        // the encoder's own for the whole image
        let mut x = 12345u32;
        let img: Vec<u8> = (0..16 * 12 * 4)
            .map(|_| {
                x = x.wrapping_mul(1_103_515_245).wrapping_add(12345);
                (x >> 16) as u8
            })
            .collect();
        let mut g = EncoderSettings {
            line: -1,
            near: -1,
            ..settings(true)
        };
        g.max_err = -1;
        let mut e = Encoder::new(g, None).unwrap();
        let guided = e.encode_rows(&img, 16, 12, (4, 4), None, None).unwrap();
        assert_eq!(e.counts[5], 12);
        let mut s = Encoder::new(settings(false), None).unwrap();
        assert_eq!(
            s.encode_rows(&img, 16, 12, (4, 4), None, None).unwrap(),
            guided
        );
    }

    #[test]
    fn gather_repeats_edges_and_pads_with_the_first_block() {
        // a 5 x 5 image: block 1 (x 1) has one column, repeated
        let img: Vec<u8> = (0..25u8).flat_map(|i| [i, 0, 0, 255]).collect();
        let rest: Vec<u8> = [1u32, 3, 0].iter().flat_map(|v| v.to_le_bytes()).collect();
        let sub = gather(&img, 5, 5, 4, 4, &rest, 2);
        assert_eq!(sub.len(), 8 * 8 * 4);
        let at = |x: usize, y: usize| sub[4 * (y * 8 + x)];
        assert_eq!(
            (0..4).map(|x| at(x, 0)).collect::<Vec<_>>(),
            vec![4, 4, 4, 4]
        );
        assert_eq!(
            (4..8).map(|x| at(x, 3)).collect::<Vec<_>>(),
            vec![24, 24, 24, 24]
        );
        assert_eq!(at(0, 4), 0);
        assert_eq!(at(4, 4), 4); // the pad: block 1 again
    }
}
