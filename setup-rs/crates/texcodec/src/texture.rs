//! textures.py's work on one Texture2D, as pure functions: the decision (`choose`), the decoded and
//! scaled levels (`prepare`: _prepare with decode_downscale, downscale_parallel, mip_chain), the
//! strip path of big unscaled textures (strip_blocks), and the result a texture's fields take
//! (`convert`: what store() writes). The Unity-file layer reads the fields and data and writes the
//! result back; nothing here reads files.

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;

use regex::Regex;
use serde_json::Value;

use crate::astc::{Encoder, EncoderSettings, Seed};
use crate::decode::{self, platform, tf};
use crate::image::{self, Rgba};
use crate::Error;

/// Single-level textures from this size up that keep their size are converted in strips.
pub const STRIP_MIN_PX: usize = 2048 * 2048;
/// Pixels a strip (8 MB of RGBA).
pub const STRIP_PX: usize = 1 << 21;
/// Textures from this size up are decoded and scaled on every core.
pub const PREP_MIN_PX: usize = 1 << 20;
/// Textures from 4096 x 4096 up that are scaled: decoded a band at a time.
pub const HUGE_PX: usize = 1 << 24;

/// The formats the step converts (textures.py CONVERTIBLE) and whether they have alpha.
pub fn convertible(format: i32) -> Option<bool> {
    Some(match format {
        tf::DXT1 | tf::DXT1_CRUNCHED | tf::BC4 | tf::BC5 | tf::RGB24 => false,
        tf::DXT5 | tf::DXT5_CRUNCHED | tf::BC7 | tf::RGBA32 | tf::ARGB32 | tf::BGRA32 => true,
        _ => return None,
    })
}

/// The textures settings (textures.py DEFAULTS with the config's `textures` over them).
#[derive(Clone, Debug)]
pub struct Settings {
    pub scale: f64,
    pub block: (u32, u32),
    pub quality: String,
    pub mode: String,
    pub guided_max_err: i32,
    pub guided_line: i32,
    pub guided_near: i32,
    pub sharp_names: String,
    pub scale_sharp: f64,
    pub sharp_block: (u32, u32),
    pub keep_max: f64,
    pub min_side: f64,
    pub min_convert: f64,
    /// "keep" (a full ASTC mip chain where the texture has mipmaps) or "drop" (one level)
    pub mips: String,
    pub inline: bool,
    pub threads: usize,
    pub prepare_split: bool,
    pub strips: bool,
    pub guide: String,
    /// the merged settings as JSON (the guide's header is checked against them)
    pub json: Value,
    sharp_rx: Option<Regex>,
}

/// textures.py DEFAULTS.
pub fn defaults() -> Value {
    serde_json::json!({
        "scale": 1.0, "block": [4, 4], "quality": "medium", "mode": "guided", "guided_max_err": 64,
        "guided_line": 128, "guided_near": 1024, "sharp_names": "", "scale_sharp": 1.0,
        "sharp_block": [4, 4], "keep_max": 64, "min_side": 16, "min_convert": 4, "mips": "keep",
        "inline": true, "files": ".", "threads": 0, "pipeline": "auto", "prepare_split": true,
        "solo_mb": 128, "strips": true, "guide": "", "inline_streams": false, "spill_mb": 64
    })
}

/// Python truthiness of a JSON value.
fn truthy(v: &Value) -> bool {
    match v {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => n.as_f64() != Some(0.0),
        Value::String(s) => !s.is_empty(),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
    }
}

impl Settings {
    /// From a config's `textures` object (None: all defaults).
    pub fn from_config(textures: Option<&Value>) -> Result<Settings, Error> {
        let mut c = defaults();
        if let (Some(Value::Object(m)), Value::Object(d)) = (textures, &mut c) {
            for (k, v) in m {
                d.insert(k.clone(), v.clone());
            }
        }
        let bad = |k: &str| Error::Config(format!("textures.{k}: unexpected value {}", c[k]));
        let num = |k: &str| c[k].as_f64().ok_or_else(|| bad(k));
        let int = |k: &str| {
            c[k].as_i64()
                .or_else(|| c[k].as_f64().map(|f| f as i64))
                .ok_or_else(|| bad(k))
        };
        let text = |k: &str| c[k].as_str().map(str::to_string).ok_or_else(|| bad(k));
        let block = |k: &str| -> Result<(u32, u32), Error> {
            match c[k]
                .as_array()
                .map(|a| a.iter().filter_map(Value::as_u64).collect::<Vec<_>>())
            {
                Some(v) if v.len() == 2 => Ok((v[0] as u32, v[1] as u32)),
                _ => Err(bad(k)),
            }
        };
        let sharp_names = text("sharp_names")?;
        let sharp_rx = if sharp_names.is_empty() {
            None
        } else {
            Some(
                Regex::new(&format!("(?i){sharp_names}"))
                    .map_err(|e| Error::Config(format!("textures.sharp_names: {e}")))?,
            )
        };
        let threads = int("threads")?;
        Ok(Settings {
            scale: num("scale")?,
            block: block("block")?,
            quality: text("quality")?,
            mode: text("mode")?,
            guided_max_err: int("guided_max_err")? as i32,
            guided_line: int("guided_line")? as i32,
            guided_near: int("guided_near")? as i32,
            sharp_names,
            scale_sharp: num("scale_sharp")?,
            sharp_block: block("sharp_block")?,
            keep_max: num("keep_max")?,
            min_side: num("min_side")?,
            min_convert: num("min_convert")?,
            mips: text("mips")?,
            inline: truthy(&c["inline"]),
            threads: if threads > 0 {
                threads as usize
            } else {
                cpu_count()
            },
            prepare_split: c.get("prepare_split").is_none_or(truthy),
            strips: truthy(&c["strips"]),
            guide: c["guide"].as_str().unwrap_or("").to_string(),
            json: c,
            sharp_rx,
        })
    }

    /// The encoder's settings (textures.py's Astc arguments).
    pub fn encoder(&self) -> Result<EncoderSettings, Error> {
        if !["guided", "search"].contains(&self.mode.as_str()) {
            return Err(Error::Config(format!(
                "textures mode {:?}: one of guided, search",
                self.mode
            )));
        }
        crate::astc::preset(&self.quality)?;
        Ok(EncoderSettings {
            quality: self.quality.clone(),
            threads: self.threads,
            guided: self.mode == "guided",
            max_err: self.guided_max_err,
            line: self.guided_line,
            near: self.guided_near,
        })
    }
}

/// The step's encoder (textures.py run()): guided or search, and in guided mode the guide when
/// `guide` names a file (else none: the caller logs "no ASTC guide at ..."; a guide made for other
/// settings is opened but never used: `guide.ok` is false).
pub fn encoder(s: &Settings) -> Result<Encoder, Error> {
    let es = s.encoder()?;
    let guide = if es.guided && !s.guide.is_empty() && std::path::Path::new(&s.guide).is_file() {
        Some(crate::guide::Guide::open(
            std::path::Path::new(&s.guide),
            &s.json,
        )?)
    } else {
        None
    };
    Encoder::new(es, guide)
}

/// os.cpu_count() on Linux as CPython takes it (the online CPUs: glibc's sysconf reads
/// /sys/devices/system/cpu/online), else the threads Rust may use.
pub fn cpu_count() -> usize {
    if let Ok(s) = std::fs::read_to_string("/sys/devices/system/cpu/online") {
        let mut n = 0;
        for part in s.trim().split(',') {
            match part.split_once('-') {
                Some((a, b)) => match (a.parse::<usize>(), b.parse::<usize>()) {
                    (Ok(a), Ok(b)) if b >= a => n += b - a + 1,
                    _ => return fallback_cpus(),
                },
                None if part.parse::<usize>().is_ok() => n += 1,
                None => return fallback_cpus(),
            }
        }
        if n > 0 {
            return n;
        }
    }
    fallback_cpus()
}

fn fallback_cpus() -> usize {
    std::thread::available_parallelism().map_or(4, |n| n.get())
}

/// A Texture2D's fields the step reads (its type tree; None where a field is absent).
#[derive(Clone, Debug)]
pub struct Texture<'a> {
    pub name: &'a str,
    pub width: usize,
    pub height: usize,
    /// m_TextureFormat
    pub format: i32,
    pub mip_count: Option<i64>,
    /// m_IsReadable, as Python truthiness
    pub is_readable: bool,
    pub dimension: Option<i64>,
    pub image_count: Option<i64>,
    /// the serialized file's BuildTarget (UnityPy's 3716 when unknown)
    pub platform: i32,
    pub platform_blob: &'a [u8],
    /// the file's Unity version, major and minor (UnityPy's reader version: the crunch flavour)
    pub version: [i64; 2],
}

/// What to do with a texture (textures.py Choice).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Choice {
    pub block: (u32, u32),
    pub width: usize,
    pub height: usize,
    pub alpha: bool,
    pub mips: usize,
}

/// textures.py choose(): a Choice, or None to leave the texture as it is.
pub fn choose(t: &Texture, s: &Settings) -> Option<Choice> {
    let (w, h) = (t.width, t.height);
    let alpha = convertible(t.format)?;
    if t.is_readable {
        return None; // scripts read its pixels: keep the format
    }
    if t.dimension.unwrap_or(2) != 2 || t.image_count.unwrap_or(1) != 1 {
        return None;
    }
    if (w.min(h) as f64) < s.min_convert {
        return None;
    }
    let sharp = s.sharp_rx.as_ref().is_some_and(|rx| rx.is_match(t.name));
    let mut sc = if sharp { s.scale_sharp } else { s.scale };
    if w.max(h) as f64 <= s.keep_max || w.min(h) as f64 * sc < s.min_side {
        sc = 1.0;
    }
    let nw = ((w as f64 * sc).round_ties_even() as i64).max(1) as usize;
    let nh = ((h as f64 * sc).round_ties_even() as i64).max(1) as usize;
    let mut mips = if s.mips == "keep" {
        t.mip_count.unwrap_or(1)
    } else {
        1
    };
    if mips > 1 {
        mips = mips.min((usize::BITS - nw.max(nh).leading_zeros()) as i64);
    }
    Some(Choice {
        block: if sharp { s.sharp_block } else { s.block },
        width: nw,
        height: nh,
        alpha,
        mips: mips.max(0) as usize,
    })
}

/// Unity's TextureFormat of ASTC LDR with this footprint (textures.py astc_format_id): up to Unity
/// 2018 RGB (48-53) and RGBA (54-59); from 2019 only the first set. unity: the data's version.
pub fn astc_format_id(block: (u32, u32), alpha: bool, unity: Option<&str>) -> Result<i32, Error> {
    let alpha = alpha
        && !unity
            .and_then(|u| u.split('.').next()?.parse::<i64>().ok())
            .is_some_and(|v| v >= 2019);
    let i = [4, 5, 6, 8, 10, 12]
        .iter()
        .position(|&n| block == (n, n))
        .ok_or_else(|| {
            Error::Config(format!(
                "no Unity ASTC format for blocks {}x{}",
                block.0, block.1
            ))
        })?;
    Ok(if alpha {
        tf::ASTC_RGBA_4X4
    } else {
        tf::ASTC_RGB_4X4
    } + i as i32)
}

/// A converted texture: the fields store() writes (m_TextureFormat, m_Width, m_Height, m_MipCount,
/// m_CompleteImageSize = data.len()) and its pixels. The Unity layer puts `data` in the stream
/// (m_StreamData: offset in the new stream, size = data.len(); `image data` emptied) or inline.
#[derive(Clone, Debug)]
pub struct Converted {
    pub format: i32,
    pub width: usize,
    pub height: usize,
    pub mip_count: usize,
    pub data: Vec<u8>,
    /// (old w, old h, new w, new h) when the size changed: for the sprites and atlases that point
    /// into it (fix_references) and the journal's `scaled`
    pub scaled: Option<[usize; 4]>,
}

/// One texture through the step: `raw` is its image data (inline, or read from its stream), `key`
/// its guide name without the level ("rel|path id"), `unity` the file's Unity version. The texture
/// must have a Choice (choose()).
pub fn convert(
    t: &Texture,
    raw: &[u8],
    ch: &Choice,
    s: &Settings,
    enc: &mut Encoder,
    key: &str,
    unity: Option<&str>,
) -> Result<Converted, Error> {
    let mut out = None;
    if s.strips
        && ch.mips == 1
        && t.mip_count.unwrap_or(1) == 1
        && (ch.width, ch.height) == (t.width, t.height)
        && ch.width * ch.height >= STRIP_MIN_PX
    {
        out = strip_blocks(t, raw, ch, enc)?.map(|b| (b, 1));
    }
    let (data, nlevels) = match out {
        Some(o) => o,
        None => {
            let p = prepare(t, raw, ch, enc.settings.threads, s.prepare_split)?;
            let mut blob = Vec::new();
            for (i, level) in p.levels.into_iter().enumerate() {
                let seed = if i == 0 {
                    p.seed.as_ref().map(|(b, d)| Seed {
                        blocks: b,
                        dxt5: *d,
                    })
                } else {
                    None
                };
                blob.extend(encode(enc, level, ch.block, seed, &format!("{key}|{i}"))?);
            }
            (blob, p.nlevels)
        }
    };
    Ok(Converted {
        format: astc_format_id(ch.block, ch.alpha, unity)?,
        width: ch.width,
        height: ch.height,
        mip_count: nlevels,
        data,
        scaled: ((ch.width, ch.height) != (t.width, t.height))
            .then_some([t.width, t.height, ch.width, ch.height]),
    })
}

/// Astc.encode: a level (rows top first) -> ASTC blocks, bottom row first as Unity stores textures.
pub fn encode(
    enc: &mut Encoder,
    mut level: Rgba,
    block: (u32, u32),
    seed: Option<Seed>,
    key: &str,
) -> Result<Vec<u8>, Error> {
    level.flip_rows();
    enc.encode_rows(&level.px, level.width, level.height, block, seed, Some(key))
}

/// The decoded and scaled levels of a texture (rows top first) and the first level's DXT blocks
/// when it keeps its size (textures.py _prepare with seeds).
pub struct Prepared {
    pub levels: Vec<Rgba>,
    pub nlevels: usize,
    pub seed: Option<(Vec<u8>, bool)>,
}

/// Formats whose blocks seed guided encoding, and whether they are DXT5 (DXT_SEED).
fn dxt_seed_format(format: i32) -> Option<bool> {
    match format {
        tf::DXT1 => Some(false),
        tf::DXT5 => Some(true),
        _ => None,
    }
}

/// Data the step cannot cut in rows (crunched, console-swizzled): strip_blocks / decode_parallel /
/// decode_downscale return None for it.
fn uncuttable(t: &Texture) -> bool {
    decode::format_name(t.format).is_some_and(|n| n.contains("Crunched"))
        || t.platform == platform::XBOX360
        || decode::is_switch_swizzled(t.platform, t.platform_blob)
}

/// UnityPy's parse_image_data(flip=False) for the data the step decodes, then texture_image().
fn decode_rgba(t: &Texture, data: &[u8], w: usize, h: usize) -> Result<Rgba, Error> {
    if t.platform == platform::XBOX360 || decode::is_switch_swizzled(t.platform, t.platform_blob) {
        return Err(Error::Decode(
            "console-swizzled texture data is not decoded here".into(),
        ));
    }
    let img = decode::parse_image_data(data, w, h, t.format, t.version)?;
    Ok(image::texture_image(img, t.format == tf::BC4))
}

/// textures.py _prepare(o, ch, fmt, pool, cores, seeds=True). `threads`: the step's cores (the
/// encoder's threads); `split`: prepare_split (big textures decoded and scaled on every core).
pub fn prepare(
    t: &Texture,
    raw: &[u8],
    ch: &Choice,
    threads: usize,
    split: bool,
) -> Result<Prepared, Error> {
    let (w, h) = (t.width, t.height);
    let same = (ch.width, ch.height) == (w, h);
    let mut seed = None;
    if let Some(dxt5) = dxt_seed_format(t.format).filter(|_| same) {
        // dxt_seed()
        let n = w.div_ceil(4) * h.div_ceil(4) * if dxt5 { 16 } else { 8 };
        if !raw.is_empty() && !uncuttable(t) && raw.len() >= n {
            seed = Some((raw[..n].to_vec(), dxt5));
        }
    }
    let huge = w * h >= HUGE_PX && !same;
    let small = if huge {
        decode_downscale(t, raw, ch.width, ch.height, if split { threads } else { 1 })?
    } else {
        None
    };
    let first = match small {
        Some(s) => s,
        None => {
            // tex.image (decoded top first; big ones a strip of rows a core), texture_image, then
            // downscale (on every core where downscale_parallel can)
            let big = w * h >= PREP_MIN_PX;
            let mut img = match (split && big).then(|| decode_parallel(t, raw, threads)) {
                Some(Some(img)) => img?,
                _ => decode_rgba(t, raw, w, h)?,
            };
            img.flip_rows();
            if split && big {
                downscale_parallel(img, ch.width, ch.height, threads)?
            } else {
                image::downscale(img, ch.width, ch.height)?
            }
        }
    };
    let levels = mip_chain(first, ch.mips)?;
    Ok(Prepared {
        nlevels: levels.len(),
        levels,
        seed,
    })
}

/// [img, img/2, ...] down to `levels` images (each side halved, at least 1).
pub fn mip_chain(img: Rgba, levels: usize) -> Result<Vec<Rgba>, Error> {
    let mut out = vec![img];
    while out.len() < levels {
        let last = out.last().unwrap();
        let (w, h) = (last.width, last.height);
        if w == 1 && h == 1 {
            break;
        }
        let next = image::downscale(last.clone(), (w / 2).max(1), (h / 2).max(1))?;
        out.push(next);
    }
    Ok(out)
}

fn gcd(a: usize, b: usize) -> usize {
    if b == 0 {
        a
    } else {
        gcd(b, a % b)
    }
}

/// Runs f(i) for i in 0..n on `threads` threads, each task's result kept in order.
fn par_map<T: Send>(
    n: usize,
    threads: usize,
    f: impl Fn(usize) -> Result<T, Error> + Sync,
) -> Result<Vec<T>, Error> {
    let next = AtomicUsize::new(0);
    let slots: Vec<Mutex<Option<Result<T, Error>>>> = (0..n).map(|_| Mutex::new(None)).collect();
    std::thread::scope(|s| {
        for _ in 0..threads.clamp(1, n.max(1)) {
            s.spawn(|| loop {
                let i = next.fetch_add(1, Ordering::Relaxed);
                if i >= n {
                    break;
                }
                let r = f(i);
                *slots[i].lock().unwrap() = Some(r);
            });
        }
    });
    slots
        .into_iter()
        .map(|m| m.into_inner().unwrap().unwrap())
        .collect()
}

/// textures.py decode_parallel() (before its flip): the texture decoded a strip of block rows a
/// task, each strip from the data's rows as the data's size divides them (the same pixels as one
/// decode when the data is just the first level). None when the data cannot be cut in rows.
fn decode_parallel(t: &Texture, raw: &[u8], cores: usize) -> Option<Result<Rgba, Error>> {
    let (bh, per) = row_units(t, raw)?;
    let (w, h) = (t.width, t.height);
    let rows = bh.max(h.div_ceil(4 * cores) / bh * bh);
    let mut img = Rgba::new(w, h);
    let r = par_chunks(&mut img.px, rows * w * 4, cores, |i, dst| {
        let y = i * rows;
        let sh = rows.min(h - y);
        let part =
            &raw[(y / bh * per).min(raw.len())..((y + sh).div_ceil(bh) * per).min(raw.len())];
        dst.copy_from_slice(&decode_rgba(t, part, w, sh)?.px);
        Ok(())
    });
    Some(r.map(|_| img))
}

/// Runs f(i, chunk) for each `len`-byte chunk of `out` on `threads` threads (each writes its own).
fn par_chunks(
    out: &mut [u8],
    len: usize,
    threads: usize,
    f: impl Fn(usize, &mut [u8]) -> Result<(), Error> + Sync,
) -> Result<(), Error> {
    let n = out.len().div_ceil(len.max(1));
    let chunks: Mutex<Vec<(usize, &mut [u8])>> =
        Mutex::new(out.chunks_mut(len.max(1)).enumerate().rev().collect());
    let failed: Mutex<Option<Error>> = Mutex::new(None);
    std::thread::scope(|s| {
        for _ in 0..threads.clamp(1, n.max(1)) {
            s.spawn(|| loop {
                if failed.lock().unwrap().is_some() {
                    break;
                }
                let Some((i, dst)) = chunks.lock().unwrap().pop() else {
                    break;
                };
                if let Err(e) = f(i, dst) {
                    *failed.lock().unwrap() = Some(e);
                }
            });
        }
    });
    match failed.into_inner().unwrap() {
        Some(e) => Err(e),
        None => Ok(()),
    }
}

/// textures.py downscale_parallel(): downscale() a strip of output rows per task where every strip
/// gets the whole image's filter (else the whole image at once, as downscale()).
pub fn downscale_parallel(img: Rgba, w: usize, h: usize, cores: usize) -> Result<Rgba, Error> {
    let (sw, sh) = (img.width, img.height);
    if (sw, sh) == (w, h) {
        return Ok(img);
    }
    let unit = h / gcd(sh, h);
    let m = h / unit;
    let n = (4 * cores).min(m);
    if n < 2 {
        return image::downscale(img, w, h);
    }
    let bounds: Vec<usize> = (0..=n)
        .map(|i| ((i * m) as f64 / n as f64).round_ties_even() as usize * unit)
        .collect();
    let ratio = sh as f64 / h as f64;
    if bounds.windows(2).any(|b| {
        let (b0, b1) = (b[0], b[1]);
        let r = (b1 * sh / h - b0 * sh / h) as f64 / (b1 - b0) as f64;
        (r - ratio).abs() > 0.0
    }) {
        return image::downscale(img, w, h);
    }
    let mut pm = img;
    pm.premultiply();
    let parts = par_map(n, cores, |i| {
        let (y0, y1) = (bounds[i], bounds[i + 1]);
        image::resize(
            &pm,
            w,
            y1 - y0,
            Some([0.0, (y0 * sh / h) as f64, sw as f64, (y1 * sh / h) as f64]),
        )
    })?;
    drop(pm);
    let mut out = Rgba::new(w, h);
    for (i, part) in parts.iter().enumerate() {
        out.paste_rows(part, bounds[i]);
    }
    out.unpremultiply();
    Ok(out)
}

/// The source's rows of blocks (or of pixels) and the bytes each takes, when the data can be cut in
/// rows (strip_blocks, decode_downscale); None otherwise.
fn row_units(t: &Texture, raw: &[u8]) -> Option<(usize, usize)> {
    if uncuttable(t) {
        return None;
    }
    let bh = decode::block_size(t.format).map_or(1, |b| b.1);
    let units = decode::compressed_size(t.width, t.height, t.format).1 / bh;
    if raw.is_empty() || units == 0 || raw.len() % units != 0 {
        return None;
    }
    Some((bh, raw.len() / units))
}

/// textures.py decode_downscale(): downscale(texture_image(tex.image)) a band of output rows at a
/// time, each band from just the source rows its filter reads, `threads` bands at once. None when
/// the data cannot be cut in rows.
pub fn decode_downscale(
    t: &Texture,
    raw: &[u8],
    w: usize,
    h: usize,
    threads: usize,
) -> Result<Option<Rgba>, Error> {
    let Some((bh, per)) = row_units(t, raw) else {
        return Ok(None);
    };
    let (sw, sh) = (t.width, t.height);
    let sy = sh as f64 / h as f64;
    let margin = (3.0 * sy.max(1.0)).ceil() as i64 + 2;
    let rows = ((512.0 / sy) as usize).max(1);
    let starts: Vec<usize> = (0..h).step_by(rows).collect();
    let band = |i: usize| -> Result<Rgba, Error> {
        let y0 = starts[i];
        let y1 = h.min(y0 + rows);
        // source rows, top first
        let a = ((y0 as f64 * sy).floor() as i64 - margin).max(0) as usize;
        let b = (sh as i64).min((y1 as f64 * sy).ceil() as i64 + margin) as usize;
        // stored rows (bottom first)
        let s0 = (sh - b) / bh * bh;
        let s1 = sh.min((sh - a).div_ceil(bh) * bh);
        let data = &raw[(s0 / bh * per).min(raw.len())..(s1 / bh * per).min(raw.len())];
        let stored = decode_rgba(t, data, sw, s1 - s0)?;
        // top first: row r is source row sh - s1 + r; keep rows a..b
        let off = sh - s1;
        let n = s1 - s0;
        let line = sw * 4;
        let mut img = Rgba {
            width: sw,
            height: b - a,
            px: Vec::with_capacity((b - a) * line),
        };
        for r in (a - off)..(b - off) {
            let sr = n - 1 - r;
            img.px
                .extend_from_slice(&stored.px[sr * line..sr * line + line]);
        }
        drop(stored);
        img.premultiply();
        image::resize(
            &img,
            w,
            y1 - y0,
            Some([
                0.0,
                y0 as f64 * sy - a as f64,
                sw as f64,
                y1 as f64 * sy - a as f64,
            ]),
        )
    };
    let mut out = Rgba::new(w, h);
    par_chunks(&mut out.px, rows * w * 4, threads, |i, dst| {
        dst.copy_from_slice(&band(i)?.px);
        Ok(())
    })?;
    out.unpremultiply();
    Ok(Some(out))
}

/// textures.py strip_blocks(): the ASTC blocks of a single-level texture kept at its size, decoded
/// and encoded a strip of rows at a time in the order Unity stores rows (bottom first). None when
/// the data cannot be cut in rows.
pub fn strip_blocks(
    t: &Texture,
    raw: &[u8],
    ch: &Choice,
    enc: &mut Encoder,
) -> Result<Option<Vec<u8>>, Error> {
    let Some((bh, per)) = row_units(t, raw) else {
        return Ok(None);
    };
    let (w, h) = (t.width, t.height);
    let by = ch.block.1 as usize;
    let step = bh * by / gcd(bh, by);
    let rows = step.max(STRIP_PX / w / step * step);
    let seed5 = dxt_seed_format(t.format);
    let mut out = Vec::new();
    let mut y = 0;
    while y < h {
        let sh = rows.min(h - y);
        let part =
            &raw[(y / bh * per).min(raw.len())..((y + sh).div_ceil(bh) * per).min(raw.len())];
        let img = decode_rgba(t, part, w, sh)?;
        let seed = seed5.map(|dxt5| Seed { blocks: part, dxt5 });
        out.extend(enc.encode_rows(&img.px, img.width, img.height, ch.block, seed, None)?);
        y += rows;
    }
    Ok(Some(out))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn tex(name: &str, w: usize, h: usize, format: i32) -> Texture<'_> {
        Texture {
            name,
            width: w,
            height: h,
            format,
            mip_count: Some(1),
            is_readable: false,
            dimension: Some(2),
            image_count: Some(1),
            platform: 2,
            platform_blob: &[],
            version: [2017, 4],
        }
    }

    fn cuphead() -> Settings {
        Settings::from_config(Some(&json!({
            "scale": 0.375, "block": [4, 4], "scale_sharp": 0.5, "sharp_block": [4, 4],
            "sharp_names": "text|font|glyph|title_card|_loc\\b|achievement", "keep_max": 64,
            "min_side": 16, "min_convert": 4, "quality": "fast", "mips": "drop", "inline": false
        })))
        .unwrap()
    }

    #[test]
    fn choose_scales_and_keeps() {
        let s = cuphead();
        let c = choose(&tex("boss_atlas", 2048, 1024, tf::DXT5), &s).unwrap();
        assert_eq!((c.width, c.height, c.alpha, c.mips), (768, 384, true, 1));
        // sharp names: scale_sharp
        let c = choose(&tex("Title_Card_x", 1000, 600, tf::DXT1), &s).unwrap();
        assert_eq!((c.width, c.height, c.alpha), (500, 300, false));
        // _loc\b: a word boundary after it
        assert_eq!(
            choose(&tex("menu_loc_es", 1000, 1000, tf::RGBA32), &s)
                .unwrap()
                .width,
            375
        );
        assert_eq!(
            choose(&tex("menu_loc", 1000, 1000, tf::RGBA32), &s)
                .unwrap()
                .width,
            500
        );
        // small ones keep their size, tiny ones and other formats are left alone
        assert_eq!(choose(&tex("a", 64, 32, tf::DXT5), &s).unwrap().width, 64);
        assert_eq!(choose(&tex("a", 40, 100, tf::DXT5), &s).unwrap().width, 40);
        assert!(choose(&tex("a", 3, 100, tf::DXT5), &s).is_none());
        assert!(choose(&tex("a", 512, 512, tf::ALPHA8), &s).is_none());
        let mut r = tex("a", 512, 512, tf::DXT5);
        r.is_readable = true;
        assert!(choose(&r, &s).is_none());
    }

    #[test]
    fn choose_rounds_half_to_even_and_limits_mips() {
        let s = Settings::from_config(Some(&json!({"scale": 0.5}))).unwrap();
        let mut t = tex("a", 101, 99, tf::RGBA32);
        t.mip_count = Some(12);
        let c = choose(&t, &s).unwrap();
        // 50.5 -> 50, 49.5 -> 50 (round half to even); 7 levels for 50 (bit_length)
        assert_eq!((c.width, c.height, c.mips), (50, 50, 6));
    }

    #[test]
    fn format_ids() {
        assert_eq!(
            astc_format_id((4, 4), true, Some("2017.4.9f1")).unwrap(),
            54
        );
        assert_eq!(
            astc_format_id((4, 4), false, Some("2017.4.9f1")).unwrap(),
            48
        );
        assert_eq!(
            astc_format_id((8, 8), true, Some("2019.4.1f1")).unwrap(),
            51
        );
        assert!(astc_format_id((6, 5), true, None).is_err());
    }

    #[test]
    fn strip_and_whole_encodes_agree() {
        // a 64 x 40 RGBA32 texture: strips of 4 rows give the same blocks as the whole image
        let mut x = 7u32;
        let raw: Vec<u8> = (0..64 * 40 * 4)
            .map(|i| {
                x = x.wrapping_mul(1_103_515_245).wrapping_add(12345);
                if i % 4 == 3 {
                    255
                } else {
                    ((x >> 16) as u8) / 8 + (i / 256) as u8
                }
            })
            .collect();
        let t = tex("a", 64, 40, tf::RGBA32);
        let s = Settings::from_config(Some(&json!({"quality": "fastest", "threads": 2}))).unwrap();
        let ch = choose(&t, &s).unwrap();
        let mut enc = Encoder::new(s.encoder().unwrap(), None).unwrap();
        let strips = strip_blocks(&t, &raw, &ch, &mut enc).unwrap().unwrap();
        let whole = enc.encode_rows(&raw, 64, 40, (4, 4), None, None).unwrap();
        assert_eq!(strips, whole);
    }

    #[test]
    fn banded_and_parallel_downscale_match_the_whole_resize() {
        // a 4096 x 4096 RGBA32 texture scaled by 3/8: decode_downscale's bands and
        // downscale_parallel's strips give downscale()'s pixels here
        let (w, h) = (4096usize, 4096usize);
        let raw: Vec<u8> = (0..w * h)
            .flat_map(|i| {
                [
                    (i % 251) as u8,
                    (i / w % 253) as u8,
                    ((i * 7) % 255) as u8,
                    (i % 3 * 120) as u8,
                ]
            })
            .collect();
        let t = tex("a", w, h, tf::RGBA32);
        let mut img = decode_rgba(&t, &raw, w, h).unwrap();
        img.flip_rows();
        let whole = image::downscale(img.clone(), 1536, 1536).unwrap();
        let banded = decode_downscale(&t, &raw, 1536, 1536, 3).unwrap().unwrap();
        assert!(banded == whole, "banded differs");
        let par = downscale_parallel(img, 1536, 1536, 4).unwrap();
        assert!(par == whole, "parallel differs");
    }
}
