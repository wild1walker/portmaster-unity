//! Unity texture data -> pixels, as UnityPy 1.25's `parse_image_data` turns it into a Pillow image
//! for the formats the textures step converts (textures.py CONVERTIBLE): the BCn block formats
//! through Pillow's "bcn" decoder (a port of Pillow 12.3's libImaging/BcnDecode.c, CC0), crunched
//! DXT first unpacked by texture2ddecoder's crunch code (vendor/texture2ddecoder), the
//! uncompressed ones through Pillow's raw unpackers. Rows come out in the data's order (UnityPy's
//! flip=False), each pixel as the 4 bytes Pillow stores: RGBA for RGBA images, RGB plus a pad byte
//! for RGB ones, the value for L ones (one byte a pixel).

use crate::Error;

/// Unity's TextureFormat numbers (UnityPy.enums.TextureFormat) this crate knows by name.
pub mod tf {
    pub const ALPHA8: i32 = 1;
    pub const RGB24: i32 = 3;
    pub const RGBA32: i32 = 4;
    pub const ARGB32: i32 = 5;
    pub const DXT1: i32 = 10;
    pub const DXT3: i32 = 11;
    pub const DXT5: i32 = 12;
    pub const BGRA32: i32 = 14;
    pub const BC6H: i32 = 24;
    pub const BC7: i32 = 25;
    pub const BC4: i32 = 26;
    pub const BC5: i32 = 27;
    pub const DXT1_CRUNCHED: i32 = 28;
    pub const DXT5_CRUNCHED: i32 = 29;
    pub const ASTC_RGB_4X4: i32 = 48;
    pub const ASTC_RGBA_4X4: i32 = 54;
}

/// BuildTarget numbers UnityPy's texture code checks.
pub mod platform {
    pub const XBOX360: i32 = 11;
    pub const SWITCH: i32 = 38;
    /// UnityPy's default when a reader has no platform.
    pub const UNKNOWN: i32 = 3716;
}

/// The Pillow mode a decoder makes (UnityPy's CONV_TABLE): how many meaningful channels the 4 (or
/// 1) bytes a pixel hold.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    /// 4 bytes a pixel: R, G, B, A.
    Rgba,
    /// 4 bytes a pixel: R, G, B and a byte Pillow ignores.
    Rgb,
    /// 1 byte a pixel.
    L,
}

/// A decoded image as Pillow holds it: `mode`'s bytes a pixel, rows one after the other.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Decoded {
    pub mode: Mode,
    pub width: usize,
    pub height: usize,
    pub px: Vec<u8>,
}

/// The block footprint of a format (UnityPy's TEXTURE_FORMAT_BLOCK_SIZE_TABLE), None for formats
/// stored a pixel at a time.
pub fn block_size(format: i32) -> Option<(usize, usize)> {
    let name = format_name(format)?;
    if let Some(rest) = name.strip_prefix("ASTC") {
        let (_, dims) = rest.rsplit_once('_')?;
        let (a, b) = dims.split_once('x')?;
        return Some((a.parse().ok()?, b.parse().ok()?));
    }
    if name.starts_with("DXT")
        || name.starts_with("BC")
        || name.starts_with("ETC")
        || name.starts_with("EAC")
    {
        return Some((4, 4));
    }
    if name.starts_with("PVRTC") {
        return Some((if name.ends_with('2') { 8 } else { 4 }, 4));
    }
    None
}

/// UnityPy's get_compressed_image_size: the size padded to whole blocks.
pub fn compressed_size(width: usize, height: usize, format: i32) -> (usize, usize) {
    match block_size(format) {
        None => (width, height),
        Some((bw, bh)) => (
            width.div_ceil(bw).saturating_mul(bw),
            height.div_ceil(bh).saturating_mul(bh),
        ),
    }
}

/// Whether `len` bytes are too few for a width x height image of a format decoded here
/// (parse_image_data's "not enough image data"); a size too big to count is too big for any
/// data. False for the formats this crate does not decode or cannot size (crunched).
pub fn too_short(len: usize, width: usize, height: usize, format: i32) -> bool {
    let (pw, ph) = compressed_size(width, height, format);
    let need = match format {
        tf::ALPHA8 => pw.checked_mul(ph),
        tf::RGB24 => pw.checked_mul(ph).and_then(|n| n.checked_mul(3)),
        tf::RGBA32 | tf::ARGB32 | tf::BGRA32 => pw.checked_mul(ph).and_then(|n| n.checked_mul(4)),
        tf::DXT1 | tf::BC4 => (pw / 4).checked_mul(ph / 4).and_then(|n| n.checked_mul(8)),
        tf::DXT3 | tf::DXT5 | tf::BC5 | tf::BC7 => {
            (pw / 4).checked_mul(ph / 4).and_then(|n| n.checked_mul(16))
        }
        _ => return false,
    };
    need.is_none_or(|n| len < n)
}

/// Is the data swizzled for the Switch (UnityPy's TextureSwizzler.is_switch_swizzled)?
pub fn is_switch_swizzled(platform: i32, blob: &[u8]) -> bool {
    if platform != platform::SWITCH || blob.len() < 12 {
        return false;
    }
    let shift = u32::from_le_bytes([blob[8], blob[9], blob[10], blob[11]]);
    // 1 << shift > 1
    shift > 0
}

extern "C" {
    fn tc_crunch_unpack(
        data: *const u8,
        len: u32,
        unity: i32,
        out: *mut *mut u8,
        out_len: *mut u32,
    ) -> bool;
    fn tc_crunch_free(p: *mut u8);
}

/// texture2ddecoder's unpack_unity_crunch (unity) or unpack_crunch: the first level's DXT blocks.
pub fn unpack_crunch(data: &[u8], unity: bool) -> Result<Vec<u8>, Error> {
    let (mut out, mut len) = (std::ptr::null_mut(), 0u32);
    let len_in = u32::try_from(data.len()).map_err(|_| Error::Decode("Unpacking failed".into()))?;
    // SAFETY: the unpacker reads `data`, and on success hands back a buffer of `len` bytes that
    // tc_crunch_free releases.
    unsafe {
        if !tc_crunch_unpack(data.as_ptr(), len_in, unity as i32, &mut out, &mut len) {
            return Err(Error::Decode("Unpacking failed".into()));
        }
        let v = std::slice::from_raw_parts(out, len as usize).to_vec();
        tc_crunch_free(out);
        Ok(v)
    }
}

/// UnityPy's parse_image_data(data, width, height, format, version, ..., flip=False) for the
/// formats this crate decodes: the image Pillow makes of it. `version`: the Unity version of the
/// texture's file (major, minor; it picks the crunch flavour). Console-swizzled data is not
/// handled (the caller checks the platform).
pub fn parse_image_data(
    data: &[u8],
    width: usize,
    height: usize,
    format: i32,
    version: [i64; 2],
) -> Result<Decoded, Error> {
    if width == 0 || height == 0 {
        // Image.new("RGBA", (0, 0))
        return Ok(Decoded {
            mode: Mode::Rgba,
            width: 0,
            height: 0,
            px: Vec::new(),
        });
    }
    if data.is_empty() {
        return Err(Error::Decode("Texture2D has no image data".into()));
    }
    let (pw, ph) = compressed_size(width, height, format);
    let unpacked;
    let data = if format == tf::DXT1_CRUNCHED || format == tf::DXT5_CRUNCHED {
        unpacked = unpack_crunch(
            data,
            version[0] > 2017 || (version[0] == 2017 && version[1] >= 3),
        )?;
        &unpacked[..]
    } else {
        data
    };
    let img = match format {
        tf::DXT1 | tf::DXT1_CRUNCHED => bcn(data, pw, ph, 1, Mode::Rgba)?,
        tf::DXT3 => bcn(data, pw, ph, 2, Mode::Rgba)?,
        tf::DXT5 | tf::DXT5_CRUNCHED => bcn(data, pw, ph, 3, Mode::Rgba)?,
        tf::BC4 => bcn(data, pw, ph, 4, Mode::L)?,
        tf::BC5 => bcn(data, pw, ph, 5, Mode::Rgb)?,
        tf::BC7 => bcn(data, pw, ph, 7, Mode::Rgba)?,
        tf::RGBA32 => raw(data, pw, ph, 4, Mode::Rgba, |s, d| d.copy_from_slice(s))?,
        tf::ARGB32 => raw(data, pw, ph, 4, Mode::Rgba, |s, d| {
            d.copy_from_slice(&[s[1], s[2], s[3], s[0]])
        })?,
        tf::BGRA32 => raw(data, pw, ph, 4, Mode::Rgba, |s, d| {
            d.copy_from_slice(&[s[2], s[1], s[0], s[3]])
        })?,
        // (Pillow's RGB unpacker writes the pad byte as 255)
        tf::RGB24 => raw(data, pw, ph, 3, Mode::Rgb, |s, d| {
            d.copy_from_slice(&[s[0], s[1], s[2], 255])
        })?,
        // "RGBA" from "A": colour 0, the value as alpha
        tf::ALPHA8 => raw(data, pw, ph, 1, Mode::Rgba, |s, d| {
            d.copy_from_slice(&[0, 0, 0, s[0]])
        })?,
        _ => {
            let name = format_name(format).unwrap_or("unknown");
            return Err(Error::Decode(format!(
                "texture format {name} ({format}) is not decoded here"
            )));
        }
    };
    Ok(if (pw, ph) != (width, height) {
        crop(&img, width, height)
    } else {
        img
    })
}

/// The top-left width x height of an image (Pillow's crop((0, 0, width, height))).
fn crop(img: &Decoded, width: usize, height: usize) -> Decoded {
    let bpp = bytes_per_pixel(img.mode);
    let mut px = Vec::with_capacity(width * height * bpp);
    for y in 0..height {
        let row = y * img.width * bpp;
        px.extend_from_slice(&img.px[row..row + width * bpp]);
    }
    Decoded {
        mode: img.mode,
        width,
        height,
        px,
    }
}

pub fn bytes_per_pixel(mode: Mode) -> usize {
    match mode {
        Mode::L => 1,
        _ => 4,
    }
}

/// Image.frombytes(mode, (w, h), data, "raw", ...): `n` bytes a pixel in, `f` writes the 4 out.
fn raw(
    data: &[u8],
    w: usize,
    h: usize,
    n: usize,
    mode: Mode,
    f: impl Fn(&[u8], &mut [u8]),
) -> Result<Decoded, Error> {
    // (sizes from the texture's fields: a product too big to count is more than the data)
    if w.checked_mul(h)
        .and_then(|p| p.checked_mul(n))
        .is_none_or(|need| data.len() < need)
    {
        return Err(Error::Decode("not enough image data".into()));
    }
    let mut px = vec![0u8; w * h * 4];
    for (s, d) in data[..w * h * n]
        .chunks_exact(n)
        .zip(px.chunks_exact_mut(4))
    {
        f(s, d);
    }
    Ok(Decoded {
        mode,
        width: w,
        height: h,
        px,
    })
}

/// Image.frombytes(mode, (w, h), data, "bcn", n): w and h are whole blocks (UnityPy pads them).
fn bcn(data: &[u8], w: usize, h: usize, n: u8, mode: Mode) -> Result<Decoded, Error> {
    let bpp = bytes_per_pixel(mode);
    let size = if n == 1 || n == 4 { 8 } else { 16 };
    let (bx, by) = (w / 4, h / 4);
    if bx
        .checked_mul(by)
        .and_then(|p| p.checked_mul(size))
        .is_none_or(|need| data.len() < need)
    {
        return Err(Error::Decode("not enough image data".into()));
    }
    let mut px = vec![0u8; w * h * bpp];
    for b in 0..bx * by {
        let src = &data[b * size..b * size + size];
        // (DECODE_LOOP's memset; BC5 fills with 0, BC5S would with 128)
        let mut col = [[0u8; 4]; 16];
        match n {
            1 => decode_bc1_color(&mut col, src, false),
            2 => {
                decode_bc1_color(&mut col, &src[8..], true);
                for (i, c) in col.iter_mut().enumerate() {
                    let av = (src[i >> 1] >> ((i & 1) * 4)) & 0xf;
                    c[3] = (av << 4) | av;
                }
            }
            3 => {
                decode_bc1_color(&mut col, &src[8..], true);
                decode_bc3_alpha(&mut col, src, 3);
            }
            4 => decode_bc3_alpha(&mut col, src, 0),
            5 => {
                decode_bc3_alpha(&mut col, src, 0);
                decode_bc3_alpha(&mut col, &src[8..], 1);
            }
            7 => decode_bc7_block(&mut col, src),
            _ => unreachable!(),
        }
        // put_block (whole blocks, ystep 0: rows as stored)
        let (x0, y0) = ((b % bx) * 4, (b / bx) * 4);
        for j in 0..4 {
            let row = ((y0 + j) * w + x0) * bpp;
            for i in 0..4 {
                let c = &col[j * 4 + i];
                px[row + i * bpp..row + i * bpp + bpp].copy_from_slice(&c[..bpp]);
            }
        }
    }
    Ok(Decoded {
        mode,
        width: w,
        height: h,
        px,
    })
}

fn decode_565(x: u16) -> [u8; 4] {
    let mut r = ((x & 0xf800) >> 8) as u32;
    r |= r >> 5;
    let mut g = ((x & 0x7e0) >> 3) as u32;
    g |= g >> 6;
    let mut b = ((x & 0x1f) << 3) as u32;
    b |= b >> 5;
    [r as u8, g as u8, b as u8, 0xff]
}

fn decode_bc1_color(dst: &mut [[u8; 4]; 16], src: &[u8], separate_alpha: bool) {
    let c0 = u16::from_le_bytes([src[0], src[1]]);
    let c1 = u16::from_le_bytes([src[2], src[3]]);
    let mut p = [[0u8; 4]; 4];
    p[0] = decode_565(c0);
    p[1] = decode_565(c1);
    let (r0, g0, b0) = (p[0][0] as u32, p[0][1] as u32, p[0][2] as u32);
    let (r1, g1, b1) = (p[1][0] as u32, p[1][1] as u32, p[1][2] as u32);
    // BC2 and BC3 reuse BC1 colour blocks but always act like c0 > c1
    if c0 > c1 || separate_alpha {
        p[2] = [
            ((2 * r0 + r1) / 3) as u8,
            ((2 * g0 + g1) / 3) as u8,
            ((2 * b0 + b1) / 3) as u8,
            0xff,
        ];
        p[3] = [
            ((r0 + 2 * r1) / 3) as u8,
            ((g0 + 2 * g1) / 3) as u8,
            ((b0 + 2 * b1) / 3) as u8,
            0xff,
        ];
    } else {
        p[2] = [
            ((r0 + r1) / 2) as u8,
            ((g0 + g1) / 2) as u8,
            ((b0 + b1) / 2) as u8,
            0xff,
        ];
        p[3] = [0, 0, 0, 0];
    }
    for n in 0..4 {
        for o in 0..4 {
            let cw = 3 & (src[4 + n] >> (2 * o));
            dst[n * 4 + o] = p[cw as usize];
        }
    }
}

/// The BC3 alpha block (also BC4's and BC5's unsigned channels) into channel `o` of each texel.
fn decode_bc3_alpha(dst: &mut [[u8; 4]; 16], src: &[u8], o: usize) {
    let a0 = src[0] as u32;
    let a1 = src[1] as u32;
    let lut1 = src[2] as u32 | (src[3] as u32) << 8 | (src[4] as u32) << 16;
    let lut2 = src[5] as u32 | (src[6] as u32) << 8 | (src[7] as u32) << 16;
    let mut a = [0u8; 8];
    a[0] = a0 as u8;
    a[1] = a1 as u8;
    if a0 > a1 {
        for (i, v) in a[2..].iter_mut().enumerate() {
            let i = i as u32;
            *v = (((6 - i) * a0 + (1 + i) * a1) / 7) as u8;
        }
    } else {
        for (i, v) in a[2..6].iter_mut().enumerate() {
            let i = i as u32;
            *v = (((4 - i) * a0 + (1 + i) * a1) / 5) as u8;
        }
        a[6] = 0;
        a[7] = 0xff;
    }
    for n in 0..8 {
        dst[n][o] = a[(7 & (lut1 >> (3 * n))) as usize];
        dst[8 + n][o] = a[(7 & (lut2 >> (3 * n))) as usize];
    }
}

// ---------------------------------------------------------------- BC7

struct Bc7Mode {
    ns: u8,
    pb: u8,
    rb: u8,
    isb: u8,
    cb: u8,
    ab: u8,
    epb: u8,
    spb: u8,
    ib: u8,
    ib2: u8,
}

#[allow(clippy::too_many_arguments)]
const fn m(
    ns: u8,
    pb: u8,
    rb: u8,
    isb: u8,
    cb: u8,
    ab: u8,
    epb: u8,
    spb: u8,
    ib: u8,
    ib2: u8,
) -> Bc7Mode {
    Bc7Mode {
        ns,
        pb,
        rb,
        isb,
        cb,
        ab,
        epb,
        spb,
        ib,
        ib2,
    }
}

const BC7_MODES: [Bc7Mode; 8] = [
    m(3, 4, 0, 0, 4, 0, 1, 0, 3, 0),
    m(2, 6, 0, 0, 6, 0, 0, 1, 3, 0),
    m(3, 6, 0, 0, 5, 0, 0, 0, 2, 0),
    m(2, 6, 0, 0, 7, 0, 1, 0, 2, 0),
    m(1, 0, 2, 1, 5, 6, 0, 0, 2, 3),
    m(1, 0, 2, 0, 7, 8, 0, 0, 2, 2),
    m(1, 0, 0, 0, 7, 7, 1, 0, 4, 0),
    m(2, 6, 0, 0, 5, 5, 1, 0, 2, 0),
];

const BC7_SI2: [u16; 64] = [
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec, 0xfe80, 0xe800,
    0xffe8, 0xff00, 0xfff0, 0xf000, 0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce,
    0x088c, 0x3110, 0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c, 0xaaaa, 0xf0f0, 0x5a5a, 0x33cc,
    0x3c3c, 0x55aa, 0x9696, 0xa55a, 0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660,
    0x0272, 0x04e4, 0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718,
    0xccf0, 0x0fcc, 0x7744, 0xee22,
];

const BC7_SI3: [u32; 64] = [
    0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0, 0x5a5a5050,
    0xaa550000, 0xaa555500, 0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250,
    0xa5945040, 0x0a425054, 0xa5a5a500, 0x55a0a0a0, 0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500,
    0x0050a4a4, 0xaaa59090, 0x14696914, 0x69691400, 0xa08585a0, 0xaa821414, 0x50a4a450, 0x6a5a0200,
    0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424, 0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50,
    0x500aa550, 0xaaaa4444, 0x66660000, 0xa5a0a5a0, 0x50a050a0, 0x69286928, 0x44aaaa44, 0x66666600,
    0xaa444444, 0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580, 0xaa141414, 0x96960000,
    0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000, 0x40804080, 0xa9a8a9a8, 0xaaaaaa44, 0x2a4a5254,
];

const BC7_AI0: [u8; 64] = [
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8,
    2, 2, 8, 8, 2, 2, 15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2,
    2, 15, 15, 15, 15, 15, 2, 2, 15,
];

const BC7_AI1: [u8; 64] = [
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5,
    15, 15, 8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5,
    10, 8, 13, 15, 12, 3, 3,
];

const BC7_AI2: [u8; 64] = [
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6,
    10, 15, 15, 10, 8, 15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
];

const BC7_W2: [u8; 4] = [0, 21, 43, 64];
const BC7_W3: [u8; 8] = [0, 9, 18, 27, 37, 46, 55, 64];
const BC7_W4: [u8; 16] = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64];

fn bc7_weights(n: u8) -> &'static [u8] {
    match n {
        2 => &BC7_W2,
        3 => &BC7_W3,
        _ => &BC7_W4,
    }
}

#[cfg(test)]
fn bc7_subset(ns: u8, partition: u8, n: usize) -> usize {
    match ns {
        2 => (1 & (BC7_SI2[partition as usize] >> n)) as usize,
        3 => (3 & (BC7_SI3[partition as usize] >> (2 * n))) as usize,
        _ => 0,
    }
}

#[cfg(test)]
/// Pillow's get_bits: at most 8 bits (as UINT8), reading at most two bytes.
fn get_bits(src: &[u8], bit: usize, count: u8) -> u8 {
    if count == 0 {
        return 0;
    }
    let by = bit >> 3;
    let bit = bit & 7;
    let count = count as usize;
    if bit + count <= 8 {
        (src[by] >> bit) & ((1u32 << count) - 1) as u8
    } else {
        let x = src[by] as u32 | (src.get(by + 1).copied().unwrap_or(0) as u32) << 8;
        ((x >> bit) & ((1 << count) - 1)) as u8
    }
}

fn expand_quantized(v: u8, bits: u8) -> u8 {
    // (C's int arithmetic: 8 bits shift a UINT8 to 0)
    let v = ((v as u32) << (8 - bits as u32)) & 0xff;
    (v | (v >> bits)) as u8
}

fn bc7_lerp(dst: &mut [u8; 4], e0: &[u8; 4], e1: &[u8; 4], s0: u8, s1: u8) {
    let (s0, s1) = (s0 as u32, s1 as u32);
    let (t0, t1) = (64 - s0, 64 - s1);
    for c in 0..3 {
        dst[c] = ((t0 * e0[c] as u32 + s0 * e1[c] as u32 + 32) >> 6) as u8;
    }
    dst[3] = ((t1 * e0[3] as u32 + s1 * e1[3] as u32 + 32) >> 6) as u8;
}

#[cfg(test)]
fn decode_bc7_block_pillow(col: &mut [[u8; 4]; 16], src: &[u8]) {
    let first = src[0];
    if first == 0 {
        // degenerate case when no bits set
        for c in col.iter_mut() {
            *c = [0, 0, 0, 255];
        }
        return;
    }
    let mode = first.trailing_zeros() as usize;
    let mut bit = mode + 1;
    let info = &BC7_MODES[mode];
    let mut cb = info.cb;
    let mut ab = info.ab;
    let cw = bc7_weights(info.ib);
    let aw = bc7_weights(if ab != 0 && info.ib2 != 0 {
        info.ib2
    } else {
        info.ib
    });
    let mut load = |n: u8| {
        let v = get_bits(src, bit, n);
        bit += n as usize;
        v
    };
    let partition = load(info.pb);
    let rotation = load(info.rb);
    let index_sel = load(info.isb);
    let numep = (info.ns as usize) << 1;
    let mut ep = [[0u8; 4]; 6];
    for c in 0..3 {
        for e in ep.iter_mut().take(numep) {
            e[c] = load(cb);
        }
    }
    for e in ep.iter_mut().take(numep) {
        e[3] = if ab != 0 { load(ab) } else { 255 };
    }
    let assign = |x: &mut u8, val: u8| *x = (*x << 1) | val;
    if info.epb != 0 {
        // per endpoint
        cb += 1;
        if ab != 0 {
            ab += 1;
        }
        for e in ep.iter_mut().take(numep) {
            let val = load(1);
            for v in &mut e[..3] {
                assign(v, val);
            }
            if ab != 0 {
                assign(&mut e[3], val);
            }
        }
    }
    if info.spb != 0 {
        // per subset
        cb += 1;
        if ab != 0 {
            ab += 1;
        }
        for i in (0..numep).step_by(2) {
            let val = load(1);
            for e in ep[i..i + 2].iter_mut() {
                for v in &mut e[..3] {
                    assign(v, val);
                }
                if ab != 0 {
                    assign(&mut e[3], val);
                }
            }
        }
    }
    for e in ep.iter_mut().take(numep) {
        for v in &mut e[..3] {
            *v = expand_quantized(*v, cb);
        }
        if ab != 0 {
            e[3] = expand_quantized(e[3], ab);
        }
    }
    let mut cibit = bit;
    let mut aibit = cibit + 16 * info.ib as usize - info.ns as usize;
    for (i, c) in col.iter_mut().enumerate() {
        let s = bc7_subset(info.ns, partition, i) << 1;
        let mut ib = info.ib;
        if i == 0 {
            ib -= 1;
        } else if info.ns == 2 {
            if i == BC7_AI0[partition as usize] as usize {
                ib -= 1;
            }
        } else if info.ns == 3
            && (i == BC7_AI1[partition as usize] as usize
                || i == BC7_AI2[partition as usize] as usize)
        {
            ib -= 1;
        }
        let i0 = get_bits(src, cibit, ib);
        cibit += ib as usize;
        let (e0, e1) = (ep[s], ep[s + 1]);
        if ab != 0 && info.ib2 != 0 {
            let mut ib2 = info.ib2;
            if i == 0 {
                ib2 -= 1;
            }
            let i1 = get_bits(src, aibit, ib2);
            aibit += ib2 as usize;
            if index_sel != 0 {
                bc7_lerp(c, &e0, &e1, aw[i1 as usize], cw[i0 as usize]);
            } else {
                bc7_lerp(c, &e0, &e1, cw[i0 as usize], aw[i1 as usize]);
            }
        } else {
            bc7_lerp(c, &e0, &e1, cw[i0 as usize], cw[i0 as usize]);
        }
        match rotation {
            1 => c.swap(0, 3),
            2 => c.swap(1, 3),
            3 => c.swap(2, 3),
            _ => {}
        }
    }
}

/// Pillow's decode_bc7_block (the reference above, kept for the tests) for one mode at a time
/// (the mode's field sizes constants), the block read as one 128-bit number, and the colours of
/// each subset and index worked out once (bc7_lerp of the same endpoints and weights: a texel's
/// colour depends only on them) instead of once a texel. Every block decodes to the same texels
/// (the tests compare them on random blocks of each mode).
fn decode_bc7_block(col: &mut [[u8; 4]; 16], src: &[u8]) {
    let first = src[0];
    if first == 0 {
        // degenerate case when no bits set
        for c in col.iter_mut() {
            *c = [0, 0, 0, 255];
        }
        return;
    }
    let v = u128::from_le_bytes(src[..16].try_into().unwrap());
    match first.trailing_zeros() {
        0 => bc7_mode::<0>(col, v),
        1 => bc7_mode::<1>(col, v),
        2 => bc7_mode::<2>(col, v),
        3 => bc7_mode::<3>(col, v),
        4 => bc7_mode::<4>(col, v),
        5 => bc7_mode::<5>(col, v),
        6 => bc7_mode::<6>(col, v),
        _ => bc7_mode::<7>(col, v),
    }
}

#[inline(always)]
fn bc7_mode<const MODE: usize>(col: &mut [[u8; 4]; 16], v: u128) {
    let info = &BC7_MODES[MODE];
    // (get_bits: at most 8 bits; none past the block, as its bytes past it read 0)
    let mut bits = v >> (MODE + 1);
    let mut take = |n: u8| -> u8 {
        let x = (bits as u32 & ((1u32 << n) - 1)) as u8;
        bits >>= n;
        x
    };
    let mut cb = info.cb;
    let mut ab = info.ab;
    let partition = take(info.pb) as usize;
    let rotation = take(info.rb);
    let index_sel = take(info.isb);
    let ns = info.ns as usize;
    let numep = ns << 1;
    let mut ep = [[0u8; 4]; 6];
    for c in 0..3 {
        for e in ep.iter_mut().take(numep) {
            e[c] = take(cb);
        }
    }
    for e in ep.iter_mut().take(numep) {
        e[3] = if ab != 0 { take(ab) } else { 255 };
    }
    if info.epb != 0 {
        cb += 1;
        if ab != 0 {
            ab += 1;
        }
        for e in ep.iter_mut().take(numep) {
            let val = take(1);
            for x in &mut e[..3] {
                *x = (*x << 1) | val;
            }
            if ab != 0 {
                e[3] = (e[3] << 1) | val;
            }
        }
    }
    if info.spb != 0 {
        cb += 1;
        if ab != 0 {
            ab += 1;
        }
        for i in (0..numep).step_by(2) {
            let val = take(1);
            for e in ep[i..i + 2].iter_mut() {
                for x in &mut e[..3] {
                    *x = (*x << 1) | val;
                }
                if ab != 0 {
                    e[3] = (e[3] << 1) | val;
                }
            }
        }
    }
    for e in ep.iter_mut().take(numep) {
        for x in &mut e[..3] {
            *x = expand_quantized(*x, cb);
        }
        if ab != 0 {
            e[3] = expand_quantized(e[3], ab);
        }
    }
    // the colour indices follow (the index bits fit in 64), the second set (modes 4, 5) after them
    let two = ab != 0 && info.ib2 != 0;
    let ib = info.ib as u32;
    let mut ci = bits as u64;
    let mut ai = if two {
        (bits >> (16 * info.ib as usize - ns)) as u64
    } else {
        0
    };
    // texels whose index has a bit fewer: texel 0, and the subsets' anchors
    let anchors: u32 = 1
        | match ns {
            2 => 1u32 << BC7_AI0[partition],
            3 => (1u32 << BC7_AI1[partition]) | (1u32 << BC7_AI2[partition]),
            _ => 0,
        };
    let subsets: u32 = match ns {
        2 => BC7_SI2[partition] as u32,
        3 => BC7_SI3[partition],
        _ => 0,
    };
    let cw = bc7_weights(info.ib);
    let swap = |c: &mut [u8; 4]| match rotation {
        1 => c.swap(0, 3),
        2 => c.swap(1, 3),
        3 => c.swap(2, 3),
        _ => {}
    };
    if !two {
        // a palette of each subset's colours by index (bc7_lerp(e0, e1, w, w), rotated)
        let mut pal = [[[0u8; 4]; 16]; 3];
        for (s, p) in pal.iter_mut().enumerate().take(ns) {
            let (e0, e1) = (&ep[2 * s], &ep[2 * s + 1]);
            for (k, &w) in cw.iter().enumerate() {
                bc7_lerp(&mut p[k], e0, e1, w, w);
                swap(&mut p[k]);
            }
        }
        for (i, c) in col.iter_mut().enumerate() {
            let s = match ns {
                2 => ((subsets >> i) & 1) as usize,
                3 => ((subsets >> (2 * i)) & 3) as usize,
                _ => 0,
            };
            let n = ib - ((anchors >> i) & 1);
            let i0 = (ci & ((1u64 << n) - 1)) as usize;
            ci >>= n;
            *c = pal[s][i0];
        }
        return;
    }
    // modes 4 and 5 (one subset): the colour from one index, the alpha from the other
    let aw = bc7_weights(info.ib2);
    let (e0, e1) = (&ep[0], &ep[1]);
    let (mut rgb, mut alpha) = ([[0u8; 4]; 8], [0u8; 8]);
    // (index_sel: the second indices weight the colour, the first the alpha)
    let (wc, wa) = if index_sel != 0 { (aw, cw) } else { (cw, aw) };
    for (k, &w) in wc.iter().enumerate() {
        bc7_lerp(&mut rgb[k], e0, e1, w, w);
    }
    for (k, &w) in wa.iter().enumerate() {
        let mut t = [0u8; 4];
        bc7_lerp(&mut t, e0, e1, w, w);
        alpha[k] = t[3];
    }
    let ib2 = info.ib2 as u32;
    for (i, c) in col.iter_mut().enumerate() {
        let first = (i == 0) as u32;
        let n = ib - first;
        let i0 = (ci & ((1u64 << n) - 1)) as usize;
        ci >>= n;
        let n2 = ib2 - first;
        let i1 = (ai & ((1u64 << n2) - 1)) as usize;
        ai >>= n2;
        let (kc, ka) = if index_sel != 0 { (i1, i0) } else { (i0, i1) };
        *c = [rgb[kc][0], rgb[kc][1], rgb[kc][2], alpha[ka]];
        swap(c);
    }
}

/// UnityPy's TextureFormat name of a number (None: not a format it knows).
pub fn format_name(format: i32) -> Option<&'static str> {
    Some(match format {
        1 => "Alpha8",
        2 => "ARGB4444",
        3 => "RGB24",
        4 => "RGBA32",
        5 => "ARGB32",
        6 => "ARGBFloat",
        7 => "RGB565",
        8 => "BGR24",
        9 => "R16",
        10 => "DXT1",
        11 => "DXT3",
        12 => "DXT5",
        13 => "RGBA4444",
        14 => "BGRA32",
        15 => "RHalf",
        16 => "RGHalf",
        17 => "RGBAHalf",
        18 => "RFloat",
        19 => "RGFloat",
        20 => "RGBAFloat",
        21 => "YUY2",
        22 => "RGB9e5Float",
        23 => "RGBFloat",
        24 => "BC6H",
        25 => "BC7",
        26 => "BC4",
        27 => "BC5",
        28 => "DXT1Crunched",
        29 => "DXT5Crunched",
        30 => "PVRTC_RGB2",
        31 => "PVRTC_RGBA2",
        32 => "PVRTC_RGB4",
        33 => "PVRTC_RGBA4",
        34 => "ETC_RGB4",
        35 => "ATC_RGB4",
        36 => "ATC_RGBA8",
        41 => "EAC_R",
        42 => "EAC_R_SIGNED",
        43 => "EAC_RG",
        44 => "EAC_RG_SIGNED",
        45 => "ETC2_RGB",
        46 => "ETC2_RGBA1",
        47 => "ETC2_RGBA8",
        48 => "ASTC_RGB_4x4",
        49 => "ASTC_RGB_5x5",
        50 => "ASTC_RGB_6x6",
        51 => "ASTC_RGB_8x8",
        52 => "ASTC_RGB_10x10",
        53 => "ASTC_RGB_12x12",
        54 => "ASTC_RGBA_4x4",
        55 => "ASTC_RGBA_5x5",
        56 => "ASTC_RGBA_6x6",
        57 => "ASTC_RGBA_8x8",
        58 => "ASTC_RGBA_10x10",
        59 => "ASTC_RGBA_12x12",
        60 => "ETC_RGB4_3DS",
        61 => "ETC_RGBA8_3DS",
        62 => "RG16",
        63 => "R8",
        64 => "ETC_RGB4Crunched",
        65 => "ETC2_RGBA8Crunched",
        66 => "ASTC_HDR_4x4",
        67 => "ASTC_HDR_5x5",
        68 => "ASTC_HDR_6x6",
        69 => "ASTC_HDR_8x8",
        70 => "ASTC_HDR_10x10",
        71 => "ASTC_HDR_12x12",
        72 => "RG32",
        73 => "RGB48",
        74 => "RGBA64",
        75 => "R8_SIGNED",
        76 => "RG16_SIGNED",
        77 => "RGB24_SIGNED",
        78 => "RGBA32_SIGNED",
        79 => "R16_SIGNED",
        80 => "RG32_SIGNED",
        81 => "RGB48_SIGNED",
        82 => "RGBA64_SIGNED",
        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn px(img: &Decoded, x: usize, y: usize) -> &[u8] {
        let b = bytes_per_pixel(img.mode);
        &img.px[(y * img.width + x) * b..(y * img.width + x + 1) * b]
    }

    /// The 128-bit reader against Pillow's byte reader: random blocks of every mode (the mode
    /// bits set, the rest random), and the degenerate first byte.
    #[test]
    fn bc7_blocks_decode_as_pillows() {
        let mut seed = 0x9e37_79b9_7f4a_7c15u64;
        let mut rnd = || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };
        for n in 0..400_000u32 {
            let mut b = [0u8; 16];
            b[..8].copy_from_slice(&rnd().to_le_bytes());
            b[8..].copy_from_slice(&rnd().to_le_bytes());
            let mode = n % 9;
            if mode == 8 {
                b[0] = 0;
            } else {
                b[0] = (b[0] & !(((2u16 << mode) - 1) as u8)) | (1 << mode);
            }
            let (mut x, mut y) = ([[0u8; 4]; 16], [[0u8; 4]; 16]);
            decode_bc7_block(&mut x, &b);
            decode_bc7_block_pillow(&mut y, &b);
            assert_eq!(x, y, "block {b:02x?}");
        }
    }

    #[test]
    fn dxt1_four_and_three_colour_blocks() {
        // c0 red > c1 blue: index 2 is 2/3 red; c0 <= c1: index 3 is transparent black
        let four = [0x00, 0xF8, 0x1F, 0x00, 0xAA, 0xAA, 0xAA, 0xAA];
        let three = [0x1F, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF];
        let img = parse_image_data(&[four, three].concat(), 8, 4, tf::DXT1, [2017, 4]).unwrap();
        assert_eq!(px(&img, 0, 0), [170, 0, 85, 255]);
        assert_eq!(px(&img, 7, 3), [0, 0, 0, 0]);
    }

    #[test]
    fn dxt5_alpha_ramp_and_crop() {
        // a0 255 > a1 0: index 2 is 6/7 of a0; the 5 x 3 image keeps the block's top-left texels
        let mut b = [0u8; 16];
        b[0] = 255;
        b[2] = 0b0000_0010; // texel 0: index 2
        b[8..12].copy_from_slice(&[0xFF, 0xFF, 0x00, 0x00]); // white over the colour block
        let img = parse_image_data(&[b, b].concat(), 5, 3, tf::DXT5, [2017, 4]).unwrap();
        assert_eq!((img.width, img.height, img.px.len()), (5, 3, 5 * 3 * 4));
        assert_eq!(px(&img, 0, 0), [255, 255, 255, 218]);
        assert_eq!(px(&img, 1, 0)[3], 255);
    }

    #[test]
    fn bc7_empty_mode_is_opaque_black_and_bc4_is_luminance() {
        let img = parse_image_data(&[0u8; 16], 4, 4, tf::BC7, [2017, 4]).unwrap();
        assert!(img.px.chunks(4).all(|p| p == [0, 0, 0, 255]));
        let l = parse_image_data(&[200, 10, 0, 0, 0, 0, 0, 0], 4, 4, tf::BC4, [2017, 4]).unwrap();
        assert_eq!((l.mode, l.px.len()), (Mode::L, 16));
        assert!(l.px.iter().all(|&v| v == 200));
    }

    #[test]
    fn raw_formats_reorder_channels() {
        let argb = parse_image_data(&[1, 2, 3, 4], 1, 1, tf::ARGB32, [2017, 4]).unwrap();
        assert_eq!(argb.px, [2, 3, 4, 1]);
        let bgra = parse_image_data(&[1, 2, 3, 4], 1, 1, tf::BGRA32, [2017, 4]).unwrap();
        assert_eq!(bgra.px, [3, 2, 1, 4]);
        let rgb = parse_image_data(&[1, 2, 3], 1, 1, tf::RGB24, [2017, 4]).unwrap();
        assert_eq!((rgb.mode, rgb.px), (Mode::Rgb, vec![1, 2, 3, 255]));
    }

    #[test]
    fn short_and_bad_data_are_errors() {
        assert!(parse_image_data(&[0u8; 8], 8, 4, tf::DXT1, [2017, 4]).is_err());
        assert!(parse_image_data(&[0u8; 7], 1, 2, tf::RGBA32, [2017, 4]).is_err());
        assert!(
            parse_image_data(b"not crunched data", 4, 4, tf::DXT5_CRUNCHED, [2017, 4]).is_err()
        );
        assert!(parse_image_data(&[0u8; 16], 4, 4, 34, [2017, 4]).is_err()); // ETC: not here
    }

    #[test]
    fn sizes_too_big_to_count_are_short_data() {
        // a damaged texture's width and height (the object's integers): the products overflow,
        // the data is too short, never a panic (the fuzz target's findings)
        for (w, h, f) in [
            (usize::MAX / 2, 3, tf::RGBA32),
            (usize::MAX / 3, 5, tf::RGB24),
            (usize::MAX - 2, 4, tf::DXT1),
            (1 << 40, 1 << 40, tf::BC7),
        ] {
            let e = parse_image_data(&[1, 2, 3, 4], w, h, f, [2017, 4]).unwrap_err();
            assert_eq!(e.to_string(), "not enough image data");
            assert!(too_short(4, w, h, f));
        }
        assert_eq!(compressed_size(usize::MAX, 1, tf::DXT1), (usize::MAX, 4));
        assert!(!too_short(64, 4, 4, tf::DXT5) && too_short(15, 4, 4, tf::DXT5));
        assert!(!too_short(3, 1 << 30, 1 << 30, tf::DXT1_CRUNCHED)); // (not sized here)
    }

    #[test]
    fn bc7_eight_bit_endpoints() {
        // modes 6 and 7 add a p-bit to 7 or 5 bits: 8 bits expand to themselves (C's shift by 8
        // of a UINT8 promoted to int), where Rust's shift of a u8 by 8 overflowed
        assert_eq!(expand_quantized(0xAB, 8), 0xAB);
        assert_eq!(expand_quantized(0x15, 5), 0xAD);
        let mut block = [0xA5u8; 16];
        block[0] = 0x40; // mode 6
        let img = parse_image_data(&block, 4, 4, tf::BC7, [2017, 4]).unwrap();
        assert_eq!(img.px.len(), 64);
    }
}
