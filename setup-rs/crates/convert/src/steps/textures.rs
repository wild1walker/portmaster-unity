//! Step `textures` (textures.py): desktop texture formats to ASTC, optionally scaled down, and
//! what points into a scaled texture made to follow it.
//!
//! This is the step's Unity-file layer: the files (`files` regex, StreamingAssets bundles), the
//! free-space check, each file opened as Loaded opens it (a big bundle unpacked to the work
//! folder), every Texture2D's fields and pixels handed to the [`Codec`] (texcodec: decoding,
//! scaling, ASTC encoding; the per-texture work as a pure function), the results stored as
//! store() does (format, size, mips, image size; the pixels into a new .resS stream file, 16-byte
//! aligned, or inline), the textures it leaves and the meshes that stream from the same file
//! copied into the new stream, sprite atlases and sprites of scaled textures fixed
//! (fix_references), then the file and its stream committed together and the sizes journaled
//! (`scaled`). The codec is texcodec's (decoding, scaling, guided ASTC, the guide).
//!
//! A bundle with several serialized files (a scene bundle) has each one's textures converted in
//! turn (each with its own new stream), then the sprites and atlases of all of them fixed, a
//! PPtr into another of them followed through its external path; the scaled sizes are journaled
//! under `<file>:<serialized file>` (pmu's own: the Python leaves such a bundle as it is). A
//! bundle the step cannot read, and one whose serialized files share a stream it would replace,
//! is left as it is with a warning at the end of the step's summary.
use super::{field, field_mut, float, int, objects, opt, section, Args};
use crate::error::{Error, Result};
use crate::job::{drop_spill, text, Job};
use crate::pyjson::{format_f, Json};
use std::collections::BTreeMap;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{sync_channel, Receiver, SyncSender};
use unityfs::{NodeData, Value};

/// The fields of a Texture2D the codec reads (choose and convert).
#[derive(Clone, Debug)]
pub struct Tex {
    pub name: Vec<u8>,
    pub width: usize,
    pub height: usize,
    pub format: i32,
    pub mip_count: Option<i64>,
    pub is_readable: bool,
    pub dimension: Option<i64>,
    pub image_count: Option<i64>,
    /// The file's target platform (BuildTarget) and the texture's m_PlatformBlob.
    pub platform: i32,
    pub platform_blob: Vec<u8>,
    /// The file's Unity version (major, minor).
    pub version: [i64; 2],
}

/// What to do with a texture (textures.Choice): block footprint, new size, alpha, mip levels.
#[derive(Clone, Debug, PartialEq)]
pub struct Choice {
    pub block: (u32, u32),
    pub width: usize,
    pub height: usize,
    pub alpha: bool,
    pub mips: usize,
}

/// A converted texture: the fields store() writes and its ASTC blocks.
#[derive(Clone, Debug)]
pub struct Converted {
    pub format: i32,
    pub width: usize,
    pub height: usize,
    pub mip_count: usize,
    pub data: Vec<u8>,
}

/// The per-texture work (texcodec): the choice, the conversion (its guide key: "rel|path id"),
/// and the summary lines it adds at the end (the guide's use, the guided blocks).
pub trait Codec {
    fn choose(&self, t: &Tex) -> Option<Choice>;
    fn convert(
        &mut self,
        t: &Tex,
        raw: &[u8],
        ch: &Choice,
        key: &str,
        unity: Option<&str>,
    ) -> Result<Converted>;
    fn summary(&mut self) -> Vec<String>;

    /// A codec with the same settings for a worker thread, encoding on one core: the step then
    /// converts several textures of a file at once (the blocks are the same: each is encoded on
    /// its own). None (the default): one texture at a time, on every core.
    fn worker(&self) -> Option<Box<dyn Codec + Send>> {
        None
    }

    /// Adds what a worker's codec counted for the summary to this one's.
    fn absorb(&mut self, _worker: &mut dyn Codec) {}

    /// When convert() takes this texture in strips that workers may encode one at a time (on one
    /// core each): how many. None (the default): it is converted whole.
    fn strips(&self, _t: &Tex, _raw: &[u8], _ch: &Choice) -> Option<usize> {
        None
    }

    /// Strip i's blocks (strips()'s cut).
    fn strip(&mut self, _t: &Tex, _raw: &[u8], _ch: &Choice, _i: usize) -> Result<Vec<u8>> {
        Err(Error::Value("this codec converts no strips".into()))
    }

    /// The texture converted in strips, from its strips' blocks in order (what convert() returns).
    fn join_strips(
        &self,
        _t: &Tex,
        _ch: &Choice,
        _data: Vec<u8>,
        _unity: Option<&str>,
    ) -> Result<Converted> {
        Err(Error::Value("this codec converts no strips".into()))
    }

    /// The guided blocks' counts (absorb's).
    fn take_counts(&mut self) -> [u64; 6] {
        [0; 6]
    }
}

/// A config value as texcodec takes its settings (serde_json).
fn to_serde(v: &Json) -> serde_json::Value {
    use serde_json::Value as V;
    match v {
        Json::Null => V::Null,
        Json::Bool(b) => V::Bool(*b),
        Json::Int(i) => match (i64::try_from(*i), u64::try_from(*i)) {
            (Ok(x), _) => V::from(x),
            (_, Ok(x)) => V::from(x),
            _ => V::from(*i as f64),
        },
        Json::Float(f) => serde_json::Number::from_f64(*f).map_or(V::Null, V::Number),
        Json::Str(s) => V::String(s.clone()),
        Json::List(l) => V::Array(l.iter().map(to_serde).collect()),
        Json::Dict(d) => V::Object(d.iter().map(|(k, x)| (k.clone(), to_serde(x))).collect()),
    }
}

fn tex_err(e: texcodec::Error) -> Error {
    match e {
        texcodec::Error::Io(e) => Error::Io(e),
        e => Error::Value(e.to_string()),
    }
}

/// texcodec as the step's codec: its settings, encoder and guide (textures.py's Astc and
/// astc_guide.Reader).
struct TexCodec {
    s: texcodec::texture::Settings,
    enc: texcodec::astc::Encoder,
    quality: String,
}

impl TexCodec {
    fn choice(ch: &Choice) -> texcodec::texture::Choice {
        texcodec::texture::Choice {
            block: ch.block,
            width: ch.width,
            height: ch.height,
            alpha: ch.alpha,
            mips: ch.mips,
        }
    }

    fn texture<'a>(t: &'a Tex, name: &'a str) -> texcodec::texture::Texture<'a> {
        texcodec::texture::Texture {
            name,
            width: t.width,
            height: t.height,
            format: t.format,
            mip_count: t.mip_count,
            is_readable: t.is_readable,
            dimension: t.dimension,
            image_count: t.image_count,
            platform: t.platform,
            platform_blob: &t.platform_blob,
            version: t.version,
        }
    }
}

impl Codec for TexCodec {
    fn choose(&self, t: &Tex) -> Option<Choice> {
        let name = String::from_utf8_lossy(&t.name);
        texcodec::texture::choose(&Self::texture(t, &name), &self.s).map(|c| Choice {
            block: c.block,
            width: c.width,
            height: c.height,
            alpha: c.alpha,
            mips: c.mips,
        })
    }

    fn convert(
        &mut self,
        t: &Tex,
        raw: &[u8],
        ch: &Choice,
        key: &str,
        unity: Option<&str>,
    ) -> Result<Converted> {
        let name = String::from_utf8_lossy(&t.name);
        let c = texcodec::texture::Choice {
            block: ch.block,
            width: ch.width,
            height: ch.height,
            alpha: ch.alpha,
            mips: ch.mips,
        };
        let out = texcodec::texture::convert(
            &Self::texture(t, &name),
            raw,
            &c,
            &self.s,
            &mut self.enc,
            key,
            unity,
        )
        .map_err(tex_err)?;
        Ok(Converted {
            format: out.format,
            width: out.width,
            height: out.height,
            mip_count: out.mip_count,
            data: out.data,
        })
    }

    fn worker(&self) -> Option<Box<dyn Codec + Send>> {
        // (a guide counts what it is asked for: kept on one codec)
        if self.enc.guide.is_some() {
            return None;
        }
        let mut s = self.s.clone();
        s.threads = 1;
        let es = s.encoder().ok()?;
        let enc = texcodec::astc::Encoder::new(es, None).ok()?;
        Some(Box::new(TexCodec {
            s,
            enc,
            quality: self.quality.clone(),
        }))
    }

    fn strips(&self, t: &Tex, raw: &[u8], ch: &Choice) -> Option<usize> {
        if self.enc.guide.is_some() {
            return None;
        }
        let name = String::from_utf8_lossy(&t.name);
        texcodec::texture::strips(
            &Self::texture(t, &name),
            raw,
            &Self::choice(ch),
            &self.s,
            true,
        )
        .map(|s| s.n)
    }

    fn strip(&mut self, t: &Tex, raw: &[u8], ch: &Choice, i: usize) -> Result<Vec<u8>> {
        let name = String::from_utf8_lossy(&t.name);
        let tx = Self::texture(t, &name);
        let c = Self::choice(ch);
        let st = texcodec::texture::strips(&tx, raw, &c, &self.s, true)
            .ok_or_else(|| Error::Value("a texture's strips changed".into()))?;
        let _t = texcodec::prof::Timer::new(texcodec::prof::Stage::Strips);
        texcodec::texture::strip(&tx, raw, &c, &st, i, &mut self.enc).map_err(tex_err)
    }

    fn join_strips(
        &self,
        t: &Tex,
        ch: &Choice,
        data: Vec<u8>,
        unity: Option<&str>,
    ) -> Result<Converted> {
        let name = String::from_utf8_lossy(&t.name);
        let out = texcodec::texture::from_strips(
            &Self::texture(t, &name),
            &Self::choice(ch),
            data,
            unity,
        )
        .map_err(tex_err)?;
        Ok(Converted {
            format: out.format,
            width: out.width,
            height: out.height,
            mip_count: out.mip_count,
            data: out.data,
        })
    }

    fn absorb(&mut self, worker: &mut dyn Codec) {
        for (a, b) in self.enc.counts.iter_mut().zip(worker.take_counts()) {
            *a += b;
        }
    }

    fn take_counts(&mut self) -> [u64; 6] {
        std::mem::take(&mut self.enc.counts)
    }

    fn summary(&mut self) -> Vec<String> {
        let mut out = Vec::new();
        if let Some(g) = &self.enc.guide {
            out.push(format!(
                "textures: ASTC guide: {} texture levels guided, {} not in it (searched at {})",
                g.used, g.missed, self.quality
            ));
        }
        let total: u64 = self.enc.counts.iter().sum();
        if self.enc.settings.guided && total > 0 {
            let names = [
                "one colour",
                "fitted",
                "DXT seed",
                "one partition",
                "two partitions",
                "searched",
            ];
            let parts: Vec<String> = names
                .iter()
                .zip(self.enc.counts)
                .map(|(k, n)| format!("{k} {} %", format_f(n as f64 * 100.0 / total as f64, 1)))
                .collect();
            out.push(format!("textures: guided blocks: {}", parts.join(", ")));
        }
        out
    }
}

/// The step's codec for its settings: textures.py's Astc (the quality and mode checked) and,
/// guided, the ASTC guide `guide` names, with what the Python logs while it sets them up.
fn codec(c: &Json, job: &Job) -> Result<Box<dyn Codec>> {
    let s = texcodec::texture::Settings::from_config(Some(&to_serde(c))).map_err(tex_err)?;
    let es = s.encoder().map_err(tex_err)?;
    let mut guide = None;
    if es.guided && opt(c, "guide").truthy() {
        let p = PathBuf::from(&s.guide);
        if p.is_file() {
            let g = texcodec::guide::Guide::open(&p, &s.json).map_err(tex_err)?;
            if !g.ok {
                job.log(&format!(
                    "textures: {} was made for other texture settings: not used",
                    p.file_name()
                        .map(|n| n.to_string_lossy())
                        .unwrap_or_default()
                ));
            }
            guide = Some(g);
        } else {
            job.log(&format!(
                "textures: no ASTC guide at {}",
                opt(c, "guide").to_str()
            ));
        }
    }
    let quality = opt(c, "quality").to_str();
    let enc = texcodec::astc::Encoder::new(es, guide).map_err(tex_err)?;
    Ok(Box::new(TexCodec { s, enc, quality }))
}

pub(crate) fn defaults() -> Vec<(&'static str, Json)> {
    let l = |a: i128, b: i128| Json::List(vec![Json::Int(a), Json::Int(b)]);
    vec![
        ("scale", Json::Float(1.0)),
        ("lowmem_scale", Json::Null),
        ("lowmem_block", Json::Null),
        ("ram_mb", Json::Int(1024)),
        ("block", l(4, 4)),
        ("quality", Json::Str("medium".into())),
        ("mode", Json::Str("guided".into())),
        ("guided_max_err", Json::Int(64)),
        ("guided_line", Json::Int(128)),
        ("guided_near", Json::Int(1024)),
        ("tiers", Json::Dict(Vec::new())),
        ("sharp_names", Json::Str(String::new())),
        ("scale_sharp", Json::Float(1.0)),
        ("sharp_block", l(4, 4)),
        ("keep_max", Json::Int(64)),
        ("min_side", Json::Int(16)),
        ("min_convert", Json::Int(4)),
        ("mips", Json::Str("keep".into())),
        ("inline", Json::Bool(true)),
        ("files", Json::Str(".".into())),
        ("threads", Json::Int(0)),
        ("pipeline", Json::Str("auto".into())),
        ("prepare_split", Json::Bool(true)),
        ("solo_mb", Json::Int(128)),
        ("strips", Json::Bool(true)),
        ("guide", Json::Str(String::new())),
        ("inline_streams", Json::Bool(false)),
        ("spill_mb", Json::Int(64)),
        ("readable", Json::Str("keep".into())),
        ("readable_keep", Json::Str(String::new())),
        ("exact_shaders", Json::Str(String::new())),
        ("exact_keywords", Json::Str(String::new())),
        ("exact_names", Json::Str(String::new())),
        ("exact_index", Json::Str(String::new())),
        ("exact_index_skip", Json::Str(String::new())),
        ("checkpoint_mb", Json::Int(64)),
    ]
}

/// What the step does with textures marked readable (m_IsReadable: Unity keeps a copy of their
/// pixels in system memory beside the GPU's, for scripts that read or write them): keep = as
/// they are, never converted (the default); release = the flag cleared, the pixels as they are
/// (the same picture, the copy gone); convert = the flag cleared and converted as the others.
/// release and convert are for a port that has checked no script reads or writes those pixels
/// (GetPixel(s), GetRawTextureData, SetPixel(s), Apply, Resize, EncodeTo*, PackTextures on them);
/// `readable_keep` (a regular expression on texture names) keeps those a script does use readable.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Readable {
    Keep,
    Release,
    Convert,
}

fn readable_mode(c: &Json) -> Result<Readable> {
    match opt(c, "readable").as_str().unwrap_or("keep") {
        "keep" => Ok(Readable::Keep),
        "release" => Ok(Readable::Release),
        "convert" => Ok(Readable::Convert),
        o => Err(Error::Value(format!(
            "textures.readable: {o:?} (keep, release or convert)"
        ))),
    }
}

/// Classes besides Texture2D whose data can be in a .resS stream (m_StreamData).
const OTHER_STREAMED: [&str; 5] = [
    "Mesh",
    "Cubemap",
    "CubemapArray",
    "Texture2DArray",
    "Texture3D",
];

/// The files the step looks at (texture_files): the serialized files, then every UnityFS bundle
/// under StreamingAssets, those whose relative path `files` matches.
fn texture_files(job: &Job, files: &regex::Regex) -> Vec<String> {
    let mut out = job.list("", |n| {
        n.ends_with(".assets") || n == "globalgamemanagers" || super::shaders::is_level(n)
    });
    for (rel, mut names) in job.walk("StreamingAssets") {
        names.sort();
        let dir = job.src.join(&rel);
        for n in names {
            if super::shaders::is_bundle(&dir.join(&n)) {
                out.push(format!("{rel}/{n}"));
            }
        }
    }
    out.retain(|r| files.is_match(r));
    out
}

/// bigbundle.size: the bytes of a UnityFS bundle's nodes, or 0.
fn bundle_size(path: &Path) -> u64 {
    if !super::shaders::is_bundle(path) {
        return 0;
    }
    unityfs::bundle::Bundle::open(path)
        .map(|b| b.header.nodes_size())
        .unwrap_or(0)
}

/// os.path.splitext(p)[0].
fn stem(p: &Path) -> PathBuf {
    let name = p
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let lead = name.len() - name.trim_start_matches('.').len();
    match name[lead..].rfind('.') {
        Some(i) => p.with_file_name(&name[..lead + i]),
        None => p.to_path_buf(),
    }
}

/// The new contents of a texture stream file: blobs, each 16-byte aligned, written to a file as
/// they come (StreamFile), or (a bundle's, while they come to at most STREAM_MEM) held in memory:
/// a bundle's new stream only goes into the bundle that is written next, so a file of it was
/// written to the card and read back for nothing.
/// (The file is made with the first blob: most of a game's files have no textures.)
struct StreamFile {
    path: PathBuf,
    f: Option<unityfs::bgwrite::BgWriter>,
    /// the blobs in memory, while they fit in `cap` (None: in the file)
    mem: Option<Vec<u8>>,
    cap: usize,
    size: u64,
    /// in place: its data synced as it is written (it is synced before its rename)
    sync: bool,
}

/// The most of a bundle's new stream held in memory (past it, the stream goes on in a file).
const STREAM_MEM: usize = 32 << 20;

impl StreamFile {
    /// `cap`: bytes held in memory before the file is made (0: a file from the first blob).
    fn new(path: PathBuf, sync: bool, cap: usize) -> Result<StreamFile> {
        Ok(StreamFile {
            path,
            f: None,
            mem: (cap > 0).then(Vec::new),
            cap,
            size: 0,
            sync,
        })
    }

    fn file(&mut self) -> Result<&mut unityfs::bgwrite::BgWriter> {
        if self.f.is_none() {
            if let Some(d) = self.path.parent() {
                std::fs::create_dir_all(d).map_err(crate::error::at(d))?;
            }
            let mut w = unityfs::bgwrite::BgWriter::create(&self.path)
                .map_err(crate::error::at(&self.path))?;
            if self.sync {
                // (in place, the file is synced before it is renamed: most of it on the way)
                w.sync_every(16 << 20)
                    .map_err(crate::error::at(&self.path))?;
            }
            // (what was held in memory first)
            if let Some(m) = self.mem.take() {
                w.write_all(&m).map_err(crate::error::at(&self.path))?;
            }
            self.f = Some(w);
        }
        Ok(self.f.as_mut().unwrap())
    }

    fn add(&mut self, blob: &[u8]) -> Result<u64> {
        let pad = (16 - self.size % 16) % 16;
        let off = self.size + pad;
        let end = off + blob.len() as u64;
        match &mut self.mem {
            Some(m) if end <= self.cap as u64 => {
                m.resize(off as usize, 0);
                m.extend_from_slice(blob);
            }
            _ => {
                let path = self.path.clone();
                let f = self.file()?;
                f.write_all(&vec![0u8; pad as usize])
                    .and_then(|_| f.write_all(blob))
                    .map_err(crate::error::at(&path))?;
            }
        }
        self.size = end;
        Ok(off)
    }

    /// The stream: its bytes, or the file they are in.
    fn close(mut self) -> Result<NodeData> {
        if let Some(m) = self.mem.take() {
            return Ok(NodeData::Bytes(m));
        }
        self.file()?;
        self.f
            .take()
            .unwrap()
            .finish()
            .map_err(crate::error::at(&self.path))?;
        Ok(NodeData::File(self.path))
    }
}

/// Totals for the log (stats).
#[derive(Clone, Copy, Default)]
struct Stats {
    textures: u64,
    /// readable textures whose flag `readable` cleared
    released: u64,
    /// textures `exact_*` left as they are
    exact: u64,
    sprites: u64,
    atlas_entries: u64,
    bytes_in: u64,
    bytes_out: u64,
}

/// A texture's StreamingInfo: (offset, size, path), or None when it has none or size 0.
fn stream_data(t: &Value) -> Option<(u64, u64, String)> {
    let sd = t.get("m_StreamData")?;
    let size = sd.get("size").and_then(Value::as_u64).unwrap_or(0);
    if size == 0 {
        return None;
    }
    let off = sd.get("offset").and_then(Value::as_u64).unwrap_or(0);
    let path = sd
        .get("path")
        .and_then(Value::as_bytes)
        .map(|b| String::from_utf8_lossy(b).into_owned())
        .unwrap_or_default();
    Some((off, size, path))
}

fn set_stream(t: &mut Value, offset: u64, size: Option<u64>) -> Result<()> {
    let sd = field_mut(t, "m_StreamData")?;
    sd.set("offset", Value::U64(offset))?;
    if let Some(s) = size {
        sd.set("size", Value::U64(s))?;
    }
    Ok(())
}

fn opt_int(t: &Value, name: &str) -> Option<i64> {
    t.get(name).and_then(Value::as_i64)
}

/// A float field multiplied as Python multiplies it (in double, stored back as a float32).
fn mul(v: &mut Value, keys: &[&str], f: f64) -> Result<()> {
    for k in keys {
        let x = float(v, k)? * f;
        v.set(k, Value::F64(x))?;
    }
    Ok(())
}

/// A SpriteRenderData's pixel-space fields (scale_render_data).
fn scale_render_data(rd: &mut Value, sx: f64, sy: f64) -> Result<()> {
    let r = field_mut(rd, "textureRect")?;
    mul(r, &["x", "width"], sx)?;
    mul(r, &["y", "height"], sy)?;
    let o = field_mut(rd, "textureRectOffset")?;
    mul(o, &["x"], sx)?;
    mul(o, &["y"], sy)?;
    let aro = field_mut(rd, "atlasRectOffset")?;
    if (float(aro, "x")?, float(aro, "y")?) != (-1.0, -1.0) {
        mul(aro, &["x"], sx)?;
        mul(aro, &["y"], sy)?;
    }
    let uv = field_mut(rd, "uvTransform")?;
    mul(uv, &["x", "y"], sx)?;
    mul(uv, &["z", "w"], sy)?;
    Ok(())
}

/// A sprite drawn from its own scaled texture (scale_own_sprite).
fn scale_own_sprite(sp: &mut Value, sx: f64, sy: f64) -> Result<()> {
    let r = field_mut(sp, "m_Rect")?;
    mul(r, &["x", "width"], sx)?;
    mul(r, &["y", "height"], sy)?;
    let o = field_mut(sp, "m_Offset")?;
    mul(o, &["x"], sx)?;
    mul(o, &["y"], sy)?;
    let b = field_mut(sp, "m_Border")?;
    mul(b, &["x", "z"], sx)?;
    mul(b, &["y", "w"], sy)?;
    mul(sp, &["m_PixelsToUnits"], sx)?;
    scale_render_data(field_mut(sp, "m_RD")?, sx, sy)
}

/// The scaled textures (path id -> sizes) of the file a PPtr's m_FileID names, when it is one
/// whose textures this step converted.
type Scaled<'a> = dyn Fn(i64) -> Option<&'a BTreeMap<i64, [usize; 4]>> + 'a;

/// The texture a PPtr names, when it is one of the scaled ones.
fn scaled_ref(r: &Value, sizes: &Scaled) -> Result<Option<[usize; 4]>> {
    let Some(m) = sizes(int(r, "m_FileID")? as i64) else {
        return Ok(None);
    };
    Ok(m.get(&(int(r, "m_PathID")? as i64)).copied())
}

/// The scaled textures of this file (m_FileID 0) alone.
fn own<'a>(
    sizes: &'a BTreeMap<i64, [usize; 4]>,
) -> impl Fn(i64) -> Option<&'a BTreeMap<i64, [usize; 4]>> + 'a {
    move |fid| (fid == 0).then_some(sizes)
}

/// Sprite atlases and sprites of one file that use its scaled textures (fix_references), saved.
/// Whether one changed.
fn fix_references(f: &mut unityfs::DataFile, sizes: &Scaled, stats: &mut Stats) -> Result<bool> {
    let mut changed = false;
    for (pid, class) in objects(f) {
        if class == "SpriteAtlas" {
            let mut t = f.sf.read_typetree(pid)?;
            let mut n = 0;
            let map = field_mut(&mut t, "m_RenderDataMap")?
                .as_array_mut()
                .ok_or_else(|| Error::Value("m_RenderDataMap: not a list".into()))?;
            for e in map.iter_mut() {
                let Value::Pair(p) = e else {
                    return Err(Error::Value("m_RenderDataMap: not pairs".into()));
                };
                let rd = &mut p.1;
                if let Some(s) = scaled_ref(field(rd, "texture")?, sizes)? {
                    let dm = rd
                        .get("downscaleMultiplier")
                        .and_then(Value::as_f64)
                        .unwrap_or(1.0);
                    rd.set(
                        "downscaleMultiplier",
                        Value::F64(dm * s[2] as f64 / s[0] as f64),
                    )?;
                    n += 1;
                }
            }
            if n > 0 {
                f.sf.save_typetree(pid, &t)?;
                stats.atlas_entries += n;
                changed = true;
            }
        }
    }
    for (pid, class) in objects(f) {
        if class == "Sprite" {
            let mut t = f.sf.read_typetree(pid)?;
            let s = scaled_ref(field(field(&t, "m_RD")?, "texture")?, sizes)?;
            if let Some(s) = s {
                scale_own_sprite(&mut t, s[2] as f64 / s[0] as f64, s[3] as f64 / s[1] as f64)?;
                f.sf.save_typetree(pid, &t)?;
                stats.sprites += 1;
                changed = true;
            }
        }
    }
    Ok(changed)
}

/// The stream files (their names) a bundle's serialized file reads for its textures and the
/// objects that stream along with them.
fn streams_read(f: &unityfs::DataFile) -> Result<std::collections::HashSet<String>> {
    let mut out = std::collections::HashSet::new();
    for (pid, class) in objects(f) {
        if class != "Texture2D" && !OTHER_STREAMED.contains(&class.as_str()) {
            continue;
        }
        if let Some((_, _, path)) = stream_data(&f.sf.current_typetree(pid)?) {
            out.insert(path.rsplit('/').next().unwrap_or(&path).to_string());
        }
    }
    Ok(out)
}

/// Reads a stream's bytes: a bundle's node, else the stream file beside the converted file (or
/// in the source folder).
struct Streams<'a> {
    job: &'a Job,
    rel: &'a str,
    res_dir: PathBuf,
    handles: BTreeMap<PathBuf, std::fs::File>,
    /// The streams of the next textures, read in order by a thread of their own: (path, offset,
    /// size, the read's result), and the bytes they hold together.
    ahead: std::collections::VecDeque<Ahead>,
    ahead_bytes: u64,
    reader: Option<Reader>,
}

/// The reader thread: its requests, a flag that has it skip what is left, and the thread (joined
/// when Streams goes: the files it reads are renamed over next, and a file kept open there would
/// stay behind as a FUSE file system's hidden copy if the process died meanwhile).
struct Reader {
    tx: std::sync::mpsc::Sender<ReadReq>,
    stop: std::sync::Arc<std::sync::atomic::AtomicBool>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Drop for Streams<'_> {
    fn drop(&mut self) {
        if let Some(mut r) = self.reader.take() {
            r.stop.store(true, std::sync::atomic::Ordering::Relaxed);
            drop(r.tx);
            if let Some(t) = r.thread.take() {
                let _ = t.join();
            }
        }
    }
}

type Ahead = (String, u64, u64, Receiver<std::io::Result<Vec<u8>>>);
/// What the reader thread reads: a plain file's range, or a bundle node's (its source, which
/// decompresses the blocks the range is in).
enum ReadFrom {
    File(PathBuf),
    Node(unityfs::source::SharedSource),
}
type ReadReq = (ReadFrom, u64, u64, SyncSender<std::io::Result<Vec<u8>>>);

/// The most a stream read ahead may be, and what the streams read ahead may hold together: they
/// wait beside the textures being converted.
const AHEAD_MAX: u64 = 32 << 20;
const AHEAD_BUDGET: u64 = 32 << 20;

impl Streams<'_> {
    /// Has a stream read by the reader thread (a plain file's, or a bundle node's: its blocks
    /// read and decompressed there), for read() to take: the card reads the next textures while
    /// the workers convert these. Left to read() when the streams ahead already hold AHEAD_BUDGET.
    /// False when the streams ahead hold AHEAD_BUDGET already: the caller offers this one again
    /// later (the queue is in the textures' order).
    /// `current`: the stream of the texture whose turn it is (path, offset, size). When it is the
    /// first in the queue it does not count: read() takes it before the conversion starts, so it
    /// never waits beside the ones read ahead (counted, it kept every second big texture from being
    /// read ahead: two streams of over half the budget each).
    fn read_ahead(
        &mut self,
        f: &unityfs::DataFile,
        path: &str,
        off: u64,
        size: u64,
        current: Option<(&str, u64, u64)>,
    ) -> bool {
        if size > AHEAD_MAX {
            return true;
        }
        let from = if f.is_bundle {
            match f.stream(path.rsplit('/').next().unwrap_or(path)) {
                Some(src) => ReadFrom::Node(src),
                // (read() says so)
                None => return true,
            }
        } else {
            ReadFrom::File(self.resolve(path))
        };
        let taken = match (self.ahead.front(), current) {
            (Some(a), Some(c)) if a.0 == c.0 && a.1 == c.1 && a.2 == c.2 => a.2,
            _ => 0,
        };
        if self.ahead_bytes - taken + size > AHEAD_BUDGET {
            return false;
        }
        if self.reader.is_none() {
            let (tx, rx) = std::sync::mpsc::channel::<ReadReq>();
            let stop = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
            let stopped = stop.clone();
            let spawned = std::thread::Builder::new()
                .name("pmu-reader".into())
                .spawn(move || {
                    let mut open: Option<(PathBuf, std::fs::File)> = None;
                    for (from, off, size, reply) in rx {
                        if stopped.load(std::sync::atomic::Ordering::Relaxed) {
                            break;
                        }
                        let r = match from {
                            ReadFrom::Node(src) => src.read_clipped(off, size as usize),
                            ReadFrom::File(p) => {
                                if open.as_ref().is_none_or(|(q, _)| *q != p) {
                                    open = std::fs::File::open(&p).ok().map(|h| (p.clone(), h));
                                }
                                match open.as_mut() {
                                    Some((_, h)) => read_range(h, off, size),
                                    None => Err(std::io::Error::other("not opened")),
                                }
                            }
                        };
                        let _ = reply.send(r);
                    }
                });
            match spawned {
                Ok(t) => {
                    self.reader = Some(Reader {
                        tx,
                        stop,
                        thread: Some(t),
                    })
                }
                Err(_) => return true,
            }
        }
        let (tx, rx) = sync_channel(1);
        if self
            .reader
            .as_ref()
            .unwrap()
            .tx
            .send((from, off, size, tx))
            .is_ok()
        {
            self.ahead.push_back((path.to_string(), off, size, rx));
            self.ahead_bytes += size;
        }
        true
    }

    /// The file a plain file's stream is read from: beside the converted file, else in the
    /// source folder.
    fn resolve(&self, path: &str) -> PathBuf {
        let name = path.rsplit('/').next().unwrap_or(path);
        let dir = Path::new(self.rel)
            .parent()
            .map(|d| d.to_string_lossy().into_owned())
            .unwrap_or_default();
        let rel = if dir.is_empty() {
            name.to_string()
        } else {
            format!("{dir}/{name}")
        };
        let p = self.job.path_in(&rel);
        if p.exists() {
            p
        } else {
            self.res_dir.join(name)
        }
    }

    fn read(&mut self, f: &unityfs::DataFile, path: &str, off: u64, size: u64) -> Result<Vec<u8>> {
        let _t = texcodec::prof::Timer::new(texcodec::prof::Stage::Read);
        // (the streams read ahead come in the textures' order: this one is the first, or it was
        // not read ahead)
        if self
            .ahead
            .front()
            .is_some_and(|a| a.0 == path && a.1 == off && a.2 == size)
        {
            let a = self.ahead.pop_front().unwrap();
            self.ahead_bytes -= a.2;
            if let Ok(Ok(v)) = a.3.recv() {
                return Ok(v);
            }
            // (a failed read ahead is read again here: its error comes from this read)
        }
        if f.is_bundle {
            let name = path.rsplit('/').next().unwrap_or(path);
            return Ok(f.read_node(name, off, size as usize)?);
        }
        let p = self.resolve(path);
        if !self.handles.contains_key(&p) {
            let h = std::fs::File::open(&p).map_err(crate::error::at(&p))?;
            self.handles.insert(p.clone(), h);
        }
        read_range(self.handles.get_mut(&p).unwrap(), off, size).map_err(crate::error::at(&p))
    }
}

/// `size` bytes of a file from `off` (fewer where it ends first, as a Python read).
fn read_range(h: &mut std::fs::File, off: u64, size: u64) -> std::io::Result<Vec<u8>> {
    h.seek(SeekFrom::Start(off))?;
    let mut buf = Vec::with_capacity(size.min(AHEAD_MAX) as usize);
    h.take(size).read_to_end(&mut buf)?;
    Ok(buf)
}

/// What convert_file leaves: the stream's name (None: no texture streams), the new stream (a
/// file, or a bundle's in memory) and the scaled textures' sizes; None when no texture was
/// converted.
type FileResult = Option<(
    Option<String>,
    Option<NodeData>,
    BTreeMap<i64, [usize; 4]>,
    Vec<i64>,
)>;

/// What a worker is handed: a texture (what Codec::convert takes) or one strip of a big one
/// (Codec::strip), and where the result goes.
enum Work {
    Whole {
        tex: Tex,
        raw: Vec<u8>,
        ch: Choice,
        key: String,
        unity: String,
        tx: SyncSender<Result<Converted>>,
    },
    Strip {
        tex: std::sync::Arc<Tex>,
        raw: std::sync::Arc<Vec<u8>>,
        ch: Choice,
        i: usize,
        tx: SyncSender<Result<Vec<u8>>>,
    },
}

/// A texture's turn in the file's order: its stream's bytes as they are, or its conversion (done,
/// or on a worker). Turns are finished in order, so the new stream, the objects and the totals
/// come out as one texture at a time makes them.
#[allow(clippy::large_enum_variant)]
enum Turn {
    Keep {
        pid: i64,
        t: Value,
        blob: Vec<u8>,
    },
    /// (a texture kept as it is but for its m_IsReadable, its pixels in the object: `readable`)
    Flag {
        pid: i64,
        t: Value,
    },
    Conv {
        pid: i64,
        /// (an inline texture's image data taken out: finish_front sets the new one)
        t: Value,
        tex: Tex,
        sd: Option<(u64, u64, String)>,
        /// the texture's size as the totals count it (its stream's size, or its image data's)
        size_in: u64,
        est: u64,
        out: Out,
        /// what it is converted from (texmemo::meta), and whether `out` came from the checkpoint
        meta: u64,
        memo: bool,
    },
}

enum Out {
    Now(Result<Converted>),
    Later(Receiver<Result<Converted>>),
    /// a texture in strips on the workers: the strips' results (those received), and the texture
    /// as Codec::join_strips makes it, without its blocks
    Strips {
        rxs: Vec<Receiver<Result<Vec<u8>>>>,
        parts: Vec<Option<Result<Vec<u8>>>>,
        template: Converted,
    },
}

/// A worker's message for a result channel it can no longer reach.
fn stopped<T>(rel: &str) -> Result<T> {
    Err(Error::Value(format!("{rel}: a texture's worker stopped")))
}

/// How many textures ahead the step reads the objects of (and has their streams read), and the most
/// their objects may hold together (one texture's ahead whatever its size).
const LOOK: usize = 8;
const LOOK_BYTES: u64 = 16 << 20;
/// A texture's image data this big is moved out of the object's bytes as they are read, not copied
/// (SerializedFile::read_typetree_owned): an inline texture is held once, not twice.
const CARVE_MIN: usize = 1 << 20;

/// Textures this big (their new size) are converted alone, on every core (which their own work
/// keeps busy), rather than on one worker while the others wait for their turn.
const SOLO_PX: usize = 1 << 20;

/// The memory a texture's conversion may take at most (its data, the decoded levels, the
/// blocks): what the turns on workers may hold together is kept under a budget.
fn estimate(tex: &Tex, ch: &Choice, raw: usize) -> u64 {
    let px = (tex.width * tex.height).max(ch.width * ch.height) as u64;
    let px = if ch.mips > 1 { px * 4 / 3 } else { px };
    raw as u64 + px * 5
}

/// The file's textures converted (convert_file's state while its turns are finished).
struct Turns<'a> {
    rel: &'a str,
    q: std::collections::VecDeque<Turn>,
    /// (the budget's use by turns on workers, and how many there are)
    inflight: u64,
    later: usize,
    stream: StreamFile,
    sizes: BTreeMap<i64, [usize; 4]>,
    order: Vec<i64>,
    converted: u64,
    /// textures whose m_IsReadable the `readable` setting cleared (the file is written for them too)
    released: u64,
    /// textures `exact_*` left as they are
    exact: u64,
    /// the file's checkpoint (`checkpoint_mb`): each texture's conversion kept as it is finished
    memo: Option<&'a mut super::texmemo::Memo>,
}

impl Turns<'_> {
    /// The first turn finished (waiting for its worker).
    fn finish_front(&mut self, f: &mut unityfs::DataFile, stats: &mut Stats) -> Result<()> {
        let Some(turn) = self.q.pop_front() else {
            return Ok(());
        };
        match turn {
            Turn::Keep { pid, mut t, blob } => {
                let at = self.stream.add(&blob)?;
                set_stream(&mut t, at, None)?;
                f.sf.save_typetree(pid, &t)?;
            }
            Turn::Flag { pid, t } => {
                f.sf.save_typetree(pid, &t)?;
            }
            Turn::Conv {
                pid,
                mut t,
                tex,
                sd,
                size_in,
                est,
                out,
                meta,
                memo,
            } => {
                let out = match out {
                    Out::Now(r) => r,
                    Out::Later(rx) => {
                        self.inflight -= est;
                        self.later -= 1;
                        let _w = texcodec::prof::Timer::new(texcodec::prof::Stage::Wait);
                        rx.recv().unwrap_or_else(|_| stopped(self.rel))
                    }
                    Out::Strips {
                        rxs,
                        mut parts,
                        template,
                    } => {
                        self.inflight -= est;
                        self.later -= 1;
                        let _w = texcodec::prof::Timer::new(texcodec::prof::Stage::Wait);
                        let mut data = Vec::new();
                        let mut r = Ok(());
                        for (i, rx) in rxs.iter().enumerate() {
                            let part = match parts[i].take() {
                                Some(p) => p,
                                None => rx.recv().unwrap_or_else(|_| stopped(self.rel)),
                            };
                            match part {
                                Ok(b) => data.extend(b),
                                Err(e) => {
                                    r = Err(e);
                                    break;
                                }
                            }
                        }
                        r.map(|_| Converted { data, ..template })
                    }
                }
                .map_err(|e| texture_error(self.rel, &tex, pid, e))?;
                if !memo {
                    if let Some(m) = self.memo.as_deref_mut() {
                        m.put(pid, meta, &out);
                    }
                }
                if (out.width, out.height) != (tex.width, tex.height) {
                    self.sizes
                        .insert(pid, [tex.width, tex.height, out.width, out.height]);
                    self.order.push(pid);
                }
                stats.bytes_in += size_in;
                stats.bytes_out += out.data.len() as u64;
                t.set("m_TextureFormat", Value::I64(out.format as i64))?;
                t.set("m_Width", Value::I64(out.width as i64))?;
                t.set("m_Height", Value::I64(out.height as i64))?;
                t.set("m_MipCount", Value::I64(out.mip_count as i64))?;
                t.set("m_CompleteImageSize", Value::I64(out.data.len() as i64))?;
                if sd.is_some() {
                    let at = self.stream.add(&out.data)?;
                    set_stream(&mut t, at, Some(out.data.len() as u64))?;
                    t.set("image data", Value::Bytes(Vec::new()))?;
                } else {
                    t.set("image data", Value::Bytes(out.data))?;
                }
                f.sf.save_typetree(pid, &t)?;
                self.converted += 1;
            }
        }
        Ok(())
    }

    /// The first turns finished while they are ready (without waiting).
    fn finish_ready(&mut self, f: &mut unityfs::DataFile, stats: &mut Stats) -> Result<()> {
        loop {
            match self.q.front_mut() {
                None => return Ok(()),
                Some(Turn::Conv {
                    out: Out::Strips { rxs, parts, .. },
                    ..
                }) => {
                    for (rx, part) in rxs.iter().zip(parts.iter_mut()) {
                        if part.is_some() {
                            continue;
                        }
                        match rx.try_recv() {
                            Ok(r) => *part = Some(r),
                            Err(std::sync::mpsc::TryRecvError::Empty) => return Ok(()),
                            Err(_) => *part = Some(stopped(self.rel)),
                        }
                    }
                }
                Some(Turn::Conv {
                    out: out @ Out::Later(_),
                    est,
                    ..
                }) => {
                    let Out::Later(rx) = out else { unreachable!() };
                    match rx.try_recv() {
                        Ok(r) => {
                            self.inflight -= *est;
                            self.later -= 1;
                            *out = Out::Now(r);
                        }
                        Err(std::sync::mpsc::TryRecvError::Empty) => return Ok(()),
                        Err(_) => {
                            self.inflight -= *est;
                            self.later -= 1;
                            *out = Out::Now(Err(Error::Value(format!(
                                "{}: a texture's worker stopped",
                                self.rel
                            ))));
                        }
                    }
                }
                Some(_) => {}
            }
            self.finish_front(f, stats)?;
        }
    }

    /// Every turn finished, in order; then `e` (an error met while reading a later texture comes
    /// after those of the textures before it, as one at a time).
    fn drain(&mut self, f: &mut unityfs::DataFile, stats: &mut Stats) -> Result<()> {
        while !self.q.is_empty() {
            self.finish_front(f, stats)?;
        }
        Ok(())
    }
}

/// A texture's conversion error, saying which texture it is.
fn texture_error(rel: &str, tex: &Tex, pid: i64, e: Error) -> Error {
    let what = format!(
        "{rel}: texture {} (path id {pid})",
        String::from_utf8_lossy(&tex.name)
    );
    match e {
        Error::Io(io) => Error::Io(std::io::Error::new(io.kind(), format!("{what}: {io}"))),
        Error::Value(m) => Error::Value(format!("{what}: {m}")),
        e => e,
    }
}

/// The workers' share of the step: their codecs (one core each) and the memory their turns may
/// hold together.
pub struct Pool {
    pub codecs: Vec<Box<dyn Codec + Send>>,
    pub budget: u64,
}

#[allow(clippy::too_many_arguments)]
/// The textures left exactly as they are (`exact_shaders`, `exact_keywords`, `exact_names`): those that
/// materials of the named shaders sample (exacttex.rs), by data file and path id, and those named.
#[derive(Default)]
pub struct Exact {
    pub keys: std::collections::HashSet<(String, i64)>,
    pub names: Option<regex::Regex>,
}

impl Exact {
    fn has(&self, rel: &str, pid: i64, name: &[u8]) -> bool {
        let base = rel.rsplit('/').next().unwrap_or(rel);
        self.keys.contains(&(base.to_string(), pid))
            || self
                .names
                .as_ref()
                .is_some_and(|rx| rx.is_match(&String::from_utf8_lossy(name)))
    }
}

#[allow(clippy::too_many_arguments)]
fn convert_file(
    f: &mut unityfs::DataFile,
    rel: &str,
    c: &Json,
    enc: &mut dyn Codec,
    pool: &mut Option<Pool>,
    streams: &mut Streams,
    stats: &mut Stats,
    stream_path: PathBuf,
    fix_refs: bool,
    exact: &Exact,
    memo: Option<&mut super::texmemo::Memo>,
) -> Result<FileResult> {
    let unity = String::from_utf8_lossy(&f.sf.unity_version).into_owned();
    let (vmaj, vmin) = {
        let v = f.sf.version.triple();
        (v.0 as i64, v.1 as i64)
    };
    let platform = f.sf.target_platform;
    let inline = opt(c, "inline").truthy();
    let readable = readable_mode(c)?;
    let readable_keep = match opt(c, "readable_keep").as_str().unwrap_or("") {
        "" => None,
        k => Some(
            regex::Regex::new(k)
                .map_err(|e| Error::Value(format!("textures.readable_keep {k:?}: {e}")))?,
        ),
    };
    let mut stream_name: Option<String> = None;
    let mut turns = Turns {
        rel,
        q: Default::default(),
        inflight: 0,
        later: 0,
        // (a bundle's new stream is packed into the bundle, which is synced: the stream is not;
        // a plain file's is renamed into place beside it)
        stream: if f.is_bundle {
            StreamFile::new(stream_path, false, STREAM_MEM)?
        } else {
            StreamFile::new(stream_path, streams.job.inplace, 0)?
        },
        sizes: BTreeMap::new(),
        order: Vec::new(),
        converted: 0,
        released: 0,
        exact: 0,
        memo,
    };
    let textures = f.path_ids("Texture2D");
    let (budget, nworkers) = pool.as_ref().map_or((0, 0), |p| (p.budget, p.codecs.len()));
    let (job_tx, job_rx) = std::sync::mpsc::channel::<Work>();
    let job_rx = std::sync::Mutex::new(job_rx);
    let r = std::thread::scope(|s| -> Result<()> {
        if let Some(p) = pool.as_mut() {
            for codec in p.codecs.iter_mut() {
                let rx = &job_rx;
                s.spawn(move || loop {
                    let w = rx.lock().unwrap().recv();
                    match w {
                        Ok(Work::Whole {
                            tex,
                            raw,
                            ch,
                            key,
                            unity,
                            tx,
                        }) => {
                            let r = codec.convert(&tex, &raw, &ch, &key, Some(&unity));
                            drop(raw);
                            let _ = tx.send(r);
                        }
                        Ok(Work::Strip {
                            tex,
                            raw,
                            ch,
                            i,
                            tx,
                        }) => {
                            let _ = tx.send(codec.strip(&tex, &raw, &ch, i));
                        }
                        Err(_) => break,
                    }
                });
            }
        }
        let job_tx = job_tx;
        // (the next textures' objects read a few turns early, their streams read ahead in order
        // on a thread; an error reading one is raised in its own turn, as without)
        let mut next: std::collections::VecDeque<Result<Value>> = Default::default();
        let (mut looked, mut offered) = (0usize, 0usize);
        for (k, &pid) in textures.iter().enumerate() {
            // (an error of this texture's own comes after the turns before it are finished; one
            // that finishing a turn before it met is the first, as is)
            let mut turn_failed = false;
            let step = (|| -> Result<Option<Turn>> {
                let mut t = match next.pop_front() {
                    Some(t) => t?,
                    None => texcodec::prof::time(texcodec::prof::Stage::Objects, || {
                        f.sf.read_typetree_owned(pid, CARVE_MIN)
                    })?,
                };
                let sd = stream_data(&t);
                looked = looked.max(k + 1);
                offered = offered.max(k + 1);
                // (at most LOOK_BYTES of objects ahead: an inline texture's object holds its pixels, 8 of
                // resources.assets's sprite sheets were up to 8 x 107 MB)
                let size_of = |pid: i64| f.sf.object(pid).map_or(0, |o| o.byte_size as u64);
                let mut ahead: u64 = textures[k + 1..looked].iter().map(|&p| size_of(p)).sum();
                while looked < textures.len() && looked < k + 1 + LOOK {
                    let size = size_of(textures[looked]);
                    if looked > k + 1 && ahead + size > LOOK_BYTES {
                        break;
                    }
                    ahead += size;
                    next.push_back(
                        texcodec::prof::time(texcodec::prof::Stage::Objects, || {
                            f.sf.read_typetree_owned(textures[looked], CARVE_MIN)
                        })
                        .map_err(Error::from),
                    );
                    looked += 1;
                }
                // (their streams offered in order, as far as the budget goes)
                let current = sd
                    .as_ref()
                    .map(|(off, size, path)| (path.as_str(), *off, *size));
                while offered < looked {
                    if let Ok(Some((off, size, path))) =
                        next[offered - k - 1].as_ref().map(stream_data)
                    {
                        if !streams.read_ahead(f, &path, off, size, current) {
                            break;
                        }
                    }
                    offered += 1;
                }
                if let Some((_, _, path)) = &sd {
                    if let Some(n) = &stream_name {
                        if n != path {
                            return Err(Error::Value(format!(
                                "{rel}: textures stream from more than one file ({n}, {path})"
                            )));
                        }
                    }
                    stream_name = Some(path.clone());
                } else if !inline
                    || t.get("image data")
                        .and_then(Value::as_bytes)
                        .is_none_or(<[u8]>::is_empty)
                {
                    return Ok(None);
                }
                let mut tex = Tex {
                    name: super::text(&t, "m_Name")?,
                    width: int(&t, "m_Width")? as usize,
                    height: int(&t, "m_Height")? as usize,
                    format: int(&t, "m_TextureFormat")? as i32,
                    mip_count: opt_int(&t, "m_MipCount"),
                    is_readable: t
                        .get("m_IsReadable")
                        .and_then(Value::as_bool)
                        .unwrap_or(false),
                    dimension: opt_int(&t, "m_TextureDimension"),
                    image_count: opt_int(&t, "m_ImageCount"),
                    platform,
                    platform_blob: t
                        .get("m_PlatformBlob")
                        .and_then(Value::as_bytes)
                        .map(<[u8]>::to_vec)
                        .unwrap_or_default(),
                    version: [vmaj, vmin],
                };
                // `readable`: release / convert clear the flag (Unity then keeps no copy of the
                // pixels in system memory once they are on the GPU); convert lets the texture be
                // converted as the others are
                let released = tex.is_readable
                    && readable != Readable::Keep
                    && !readable_keep
                        .as_ref()
                        .is_some_and(|rx| rx.is_match(&String::from_utf8_lossy(&tex.name)));
                if released {
                    t.set("m_IsReadable", Value::Bool(false))?;
                    turns.released += 1;
                    if readable == Readable::Convert {
                        tex.is_readable = false;
                    }
                }
                // `exact_*`: left as they are (a shader reads their texels as data)
                let keep = exact.has(rel, pid, &tex.name);
                if keep {
                    turns.exact += 1;
                }
                let Some(ch) = (if keep { None } else { enc.choose(&tex) }) else {
                    return Ok(match &sd {
                        Some((off, size, path)) => Some(Turn::Keep {
                            pid,
                            blob: streams.read(f, path, *off, *size)?,
                            t,
                        }),
                        None if released => Some(Turn::Flag { pid, t }),
                        None => None,
                    });
                };
                // (an inline texture's pixels taken out of its object, not copied: the biggest texture of a
                // file was held twice, Blasphemous's 4000x6720 sheet 2 x 107 MB; finish_front sets the new
                // image data, so the object written is the same)
                let raw = match &sd {
                    Some((off, size, path)) => streams.read(f, path, *off, *size)?,
                    None => match t.get_mut("image data") {
                        Some(Value::Bytes(b)) => std::mem::take(b),
                        _ => Vec::new(),
                    },
                };
                let size_in = match &sd {
                    Some((_, size, _)) => *size,
                    None => raw.len() as u64,
                };
                let key = format!("{rel}|{pid}");
                let meta = super::texmemo::meta(&tex, raw.len(), &ch);
                // (a run that goes on after a stop: the texture as the stopped run converted it, in
                // its turn, after the turns before it)
                if let Some(m) = turns.memo.as_deref_mut() {
                    if let Some(out) = m.take(pid, meta) {
                        drop(raw);
                        turns.drain(f, stats).inspect_err(|_| turn_failed = true)?;
                        return Ok(Some(Turn::Conv {
                            pid,
                            t,
                            tex,
                            sd,
                            size_in,
                            est: 0,
                            out: Out::Now(Ok(out)),
                            meta,
                            memo: true,
                        }));
                    }
                }
                // a big texture in strips: each strip a worker's (its strips' blocks in order)
                let n = if nworkers > 0 {
                    enc.strips(&tex, &raw, &ch)
                } else {
                    None
                };
                if let Some(n) = n {
                    let est = raw.len() as u64
                        + (ch.width * ch.height) as u64
                        + nworkers as u64 * (texcodec::texture::STRIP_PX as u64 * 6 / 4);
                    while turns.later > 0
                        && (turns.inflight + est > budget || turns.later >= 16 * nworkers)
                    {
                        turns
                            .finish_front(f, stats)
                            .inspect_err(|_| turn_failed = true)?;
                    }
                    let template = enc.join_strips(&tex, &ch, Vec::new(), Some(&unity))?;
                    let (traw, ttex) = (std::sync::Arc::new(raw), std::sync::Arc::new(tex.clone()));
                    let mut rxs = Vec::with_capacity(n);
                    for i in 0..n {
                        let (tx, rx) = sync_channel(1);
                        let w = Work::Strip {
                            tex: ttex.clone(),
                            raw: traw.clone(),
                            ch: ch.clone(),
                            i,
                            tx,
                        };
                        job_tx.send(w).map_err(|_| {
                            Error::Value(format!("{rel}: the texture workers stopped"))
                        })?;
                        rxs.push(rx);
                    }
                    turns.inflight += est;
                    turns.later += 1;
                    return Ok(Some(Turn::Conv {
                        pid,
                        t,
                        tex,
                        sd,
                        size_in,
                        est,
                        out: Out::Strips {
                            parts: (0..n).map(|_| None).collect(),
                            rxs,
                            template,
                        },
                        meta,
                        memo: false,
                    }));
                }
                let est = estimate(&tex, &ch, raw.len());
                if nworkers == 0 || est > budget / 2 || ch.width * ch.height >= SOLO_PX {
                    // (alone, on every core: the turns before it are finished first)
                    turns.drain(f, stats).inspect_err(|_| turn_failed = true)?;
                    let out = enc.convert(&tex, &raw, &ch, &key, Some(&unity));
                    return Ok(Some(Turn::Conv {
                        pid,
                        t,
                        tex,
                        sd,
                        size_in,
                        est,
                        out: Out::Now(out),
                        meta,
                        memo: false,
                    }));
                }
                while turns.later > 0
                    && (turns.inflight + est > budget || turns.later >= 16 * nworkers)
                {
                    turns
                        .finish_front(f, stats)
                        .inspect_err(|_| turn_failed = true)?;
                }
                let (tx, rx) = sync_channel(1);
                let w = Work::Whole {
                    tex: tex.clone(),
                    raw,
                    ch,
                    key,
                    unity: unity.clone(),
                    tx,
                };
                job_tx
                    .send(w)
                    .map_err(|_| Error::Value(format!("{rel}: the texture workers stopped")))?;
                turns.inflight += est;
                turns.later += 1;
                Ok(Some(Turn::Conv {
                    pid,
                    t,
                    tex,
                    sd,
                    size_in,
                    est,
                    out: Out::Later(rx),
                    meta,
                    memo: false,
                }))
            })();
            match step {
                Ok(Some(turn)) => {
                    turns.q.push_back(turn);
                    turns.finish_ready(f, stats)?;
                }
                Ok(None) => {}
                Err(e) => {
                    if !turn_failed {
                        turns.drain(f, stats)?;
                    }
                    return Err(e);
                }
            }
        }
        turns.drain(f, stats)
    });
    r?;
    stats.released += turns.released;
    stats.exact += turns.exact;
    if turns.converted == 0 && turns.released == 0 {
        return Ok(None);
    }
    let mut stream = turns.stream;
    let _o = texcodec::prof::Timer::new(texcodec::prof::Stage::Objects);
    if let Some(name) = &stream_name {
        // everything else that streams from the same file goes along into the new one
        for (pid, class) in objects(f) {
            if !OTHER_STREAMED.contains(&class.as_str()) {
                continue;
            }
            let mut t = f.sf.read_typetree(pid)?;
            if let Some((off, size, path)) = stream_data(&t) {
                if &path == name {
                    let blob = streams.read(f, &path, off, size)?;
                    let at = stream.add(&blob)?;
                    set_stream(&mut t, at, None)?;
                    f.sf.save_typetree(pid, &t)?;
                }
            }
        }
    }
    stats.textures += turns.converted;
    if fix_refs {
        fix_references(f, &own(&turns.sizes), stats)?;
    }
    let path = match &stream_name {
        Some(_) => Some(stream.close()?),
        None => None,
    };
    Ok(Some((stream_name.clone(), path, turns.sizes, turns.order)))
}

/// The workers for the step: one a core (`textures.threads`, else every core), and the memory
/// their textures may hold together: a quarter of what the system has available when the step
/// starts, at most 160 MB (PMU_TEXTURE_POOL_MB sets it; 0: no workers, one texture at a time).
fn pool(enc: &dyn Codec, threads: usize) -> Option<Pool> {
    let mb = match std::env::var("PMU_TEXTURE_POOL_MB")
        .ok()
        .and_then(|v| v.parse::<u64>().ok())
    {
        Some(mb) => mb,
        None => mem_available_mb().map_or(96, |a| (a / 4).min(160)),
    };
    if threads < 2 || mb < 16 {
        return None;
    }
    let codecs = (0..threads)
        .map(|_| enc.worker())
        .collect::<Option<Vec<_>>>()?;
    Some(Pool {
        codecs,
        budget: mb << 20,
    })
}

/// /proc/meminfo's MemAvailable in MB, or less where a memory cgroup (a container) leaves less:
/// its limit less its processes' anonymous memory (the page cache it is charged for can be
/// reclaimed, as MemAvailable counts it).
fn mem_available_mb() -> Option<u64> {
    let m = std::fs::read_to_string("/proc/meminfo").ok()?;
    let l = m.lines().find(|l| l.starts_with("MemAvailable:"))?;
    let avail = l.split_whitespace().nth(1)?.parse::<u64>().ok()? >> 10;
    let num = |p: &str| -> Option<u64> { std::fs::read_to_string(p).ok()?.trim().parse().ok() };
    let stat = |p: &str, key: &str| -> Option<u64> {
        let s = std::fs::read_to_string(p).ok()?;
        let l = s.lines().find(|l| l.split(' ').next() == Some(key))?;
        l.split(' ').nth(1)?.parse().ok()
    };
    let left = [
        (
            "/sys/fs/cgroup/memory.max",
            "/sys/fs/cgroup/memory.stat",
            "anon",
        ),
        (
            "/sys/fs/cgroup/memory/memory.limit_in_bytes",
            "/sys/fs/cgroup/memory/memory.stat",
            "rss",
        ),
    ]
    .iter()
    .find_map(|(max, st, key)| Some(num(max)?.saturating_sub(stat(st, key)?) >> 20));
    Some(left.map_or(avail, |l| l.min(avail)))
}

/// An error in a file's conversion, with the scratch folder as the Python leaves it there: made
/// (job.work_dir at the file's start), and with the new stream file once convert_file has begun.
fn left(work: &Path, e: Error, stream: bool) -> Error {
    let _ = std::fs::create_dir_all(work);
    let p = work.join("stream.new");
    if stream && !p.exists() {
        let _ = std::fs::File::create(p);
    }
    e
}

/// The step's settings: the config's over the defaults, and `lowmem_scale` / `lowmem_block` (when
/// set) as `scale` / `block` where the system has at most `ram_mb` of RAM (a 1 GB handheld), with
/// the line saying so.
fn settings(cfg: &Json) -> Result<(Json, Option<String>)> {
    let mut c = section(cfg, "textures", &defaults())?;
    let set = |k: &str| c.get(k).filter(|v| !matches!(v, Json::Null)).cloned();
    let lowmem: Vec<(&str, Json)> = [
        ("scale", set("lowmem_scale")),
        ("block", set("lowmem_block")),
    ]
    .into_iter()
    .filter_map(|(k, v)| Some((k, v?)))
    .collect();
    if lowmem.is_empty() {
        return Ok((c, None));
    }
    let ram = c
        .get("ram_mb")
        .and_then(Json::as_int)
        .ok_or_else(|| Error::Value("textures.ram_mb: not an integer".into()))?;
    let mem = super::quality::mem_total_mb();
    if mem > ram {
        return Ok((c, None));
    }
    let what: Vec<String> = lowmem
        .iter()
        .map(|(k, v)| format!("{k} {} (lowmem_{k})", v.repr()))
        .collect();
    let why = format!(
        "textures: {mem} MB of RAM (at most {ram}): {}",
        what.join(", ")
    );
    for (k, v) in lowmem {
        c.set(k, v);
    }
    Ok((c, Some(why)))
}

pub fn run(job: &mut Job, args: &Args) -> Result<()> {
    let (c, why) = settings(&job.cfg)?;
    if let Some(w) = why {
        job.log(&w);
    }
    let enc = codec(&c, job)?;
    run_with(job, args, enc)
}

/// The step with a given codec (texcodec's in a setup; tests give their own).
pub fn run_with(job: &mut Job, args: &Args, mut enc: Box<dyn Codec>) -> Result<()> {
    let c = settings(&job.cfg)?.0;
    let rx = |k: &str| {
        let s = opt(&c, k).to_str();
        regex::Regex::new(&s).map_err(|e| Error::Value(format!("textures.{k} {s:?}: {e}")))
    };
    let files_rx = rx("files")?;
    let mut files = texture_files(job, &files_rx);
    let only = match &args.only {
        Some(only) => Some(
            regex::Regex::new(only).map_err(|e| Error::Value(format!("--only {only:?}: {e}")))?,
        ),
        None => None,
    };
    if let Some(r) = &only {
        files.retain(|f| r.is_match(f));
    }
    let mut exact = Exact {
        names: match opt(&c, "exact_names").to_str().as_str() {
            "" => None,
            _ => Some(rx("exact_names")?),
        },
        ..Default::default()
    };
    if !opt(&c, "exact_shaders").to_str().is_empty() {
        let index = opt(&c, "exact_index").to_str() == "blue";
        let mut save_kept = None;
        // (the walk is the same work each time, minutes on a handheld: a run that goes on after a stop
        // takes what the first one found, kept beside the journal: exactindex.rs "The index kept")
        let need_plan = index && job.state.get(super::exactindex::STATE).is_none();
        let kept = super::exactindex::load_kept(job, &c, index)
            .filter(|k| !need_plan || k.none.is_some() || k.plan_err.is_some());
        let from_kept = kept.is_some();
        let found = match kept {
            Some(k) => match k.none {
                Some(why) => Err(why),
                None => Ok((k.keep, k.plan_err.map(Err))),
            },
            None => {
                let before = super::exactindex::fingerprint(job);
                let r = super::exactindex::analyze(job, &c, index);
                let kept = match &r {
                    Err(why) => super::exactindex::Kept {
                        none: Some(why.clone()),
                        ..Default::default()
                    },
                    Ok((keep, plan)) => super::exactindex::Kept {
                        keep: keep.clone(),
                        plan_err: plan.as_ref().err().cloned(),
                        none: None,
                    },
                };
                // (a plan found is in the journal before the index is kept: below)
                save_kept = Some((before, kept));
                r.map(|(keep, plan)| (keep, Some(plan)))
            }
        };
        match found {
            Err(why) => job.log(&format!("textures: exact_shaders: {why}")),
            Ok((keep, plan)) => {
                let mut by_file: BTreeMap<&str, usize> = BTreeMap::new();
                for (f, _) in &keep {
                    *by_file.entry(&**f).or_default() += 1;
                }
                job.log(&format!(
                    "textures: exact_shaders: {} textures the palette programs sample, left as they are{}{}",
                    keep.len(),
                    if from_kept {
                        " (the index of the run before)"
                    } else {
                        ""
                    },
                    if by_file.is_empty() {
                        String::new()
                    } else {
                        format!(
                            ": in {}",
                            by_file
                                .iter()
                                .map(|(f, n)| format!("{f} ({n})"))
                                .collect::<Vec<_>>()
                                .join(", ")
                        )
                    }
                ));
                if let (true, Some(plan)) = (need_plan, plan) {
                    match plan {
                        Ok(p) => {
                            for w in &p.why {
                                job.log(&format!("{}: {w}", super::exactindex::STEP));
                            }
                            job.state
                                .set(super::exactindex::STATE, super::exactindex::plan_json(&p));
                            job.save_state()?;
                        }
                        Err(why) => job.log(&format!(
                            "{}: {why}: the textures stay as the game has them",
                            super::exactindex::STEP
                        )),
                    }
                }
                exact.keys = keep.into_iter().map(|(f, p)| (f.to_string(), p)).collect();
            }
        }
        if let Some((before, kept)) = save_kept {
            super::exactindex::save_kept(job, &c, index, &before, &kept);
        }
    }
    // (a checkpoint is one file's, made with these settings and tools: texmemo.rs)
    let checkpoint = match opt(&c, "checkpoint_mb") {
        Json::Null | Json::Bool(_) => None,
        v => v.as_f64().filter(|mb| *mb >= 0.0),
    };
    let memo_key = format!(
        "{}\n{}\n{}",
        c.dumps(),
        super::texmemo::tool_id(),
        match opt(&c, "guide").to_str().as_str() {
            "" => String::new(),
            g => super::texmemo::file_id(Path::new(g)),
        }
    );
    let spill_mb = opt(&c, "spill_mb").as_int().unwrap_or(0).max(0) as u64;
    let inline_streams = opt(&c, "inline_streams").truthy();
    let mut stats = Stats::default();
    let total = files.len();
    let threads = match opt(&c, "threads").as_int() {
        Some(n) if n > 0 => n as usize,
        _ => texcodec::texture::cpu_count(),
    };
    let mut pool = pool(enc.as_ref(), threads);
    // (one scratch folder for the step, made when a file needs it, emptied after each file)
    let work = job.work_dir_lazy("textures")?;
    // (bundles left as they are: the summary's warnings)
    let mut skipped: Vec<(String, String)> = Vec::new();
    for (i, rel) in files.iter().enumerate() {
        if job.is_done("textures", rel) {
            continue;
        }
        let src = job.path_in(rel);
        let bsize = bundle_size(&src);
        let spilled = spill_mb > 0 && bsize > spill_mb << 20;
        let st = stem(&src);
        let beside: u64 = [
            crate::job::with_suffix(&src, ".resS"),
            crate::job::with_suffix(&st, ".resS"),
            crate::job::with_suffix(&st, ".resource"),
        ]
        .iter()
        .filter(|p| p.is_file())
        .map(|p| p.metadata().map_or(0, |m| m.len()))
        .sum();
        let need = std::fs::metadata(&src)?.len()
            + beside
            + (64 << 20)
            + if spilled { bsize * 2 } else { 0 };
        // (with the converted files that wait beside the ones they replace: write_deferred)
        let need = need + job.deferred_bytes();
        job.need_space(need, &format!("convert {rel}"))?;
        let before = (stats.bytes_in, stats.bytes_out);
        let res_dir = job
            .src
            .join(rel)
            .parent()
            .map(Path::to_path_buf)
            .unwrap_or_default();
        let opts = unityfs::Options {
            resource_dir: Some(res_dir.clone()),
            spill_dir: Some(work.join("nodes")),
            spill_mb,
            all_serialized: true,
            ..Default::default()
        };
        let (mut done, mut deferred) = (false, false);
        let mut drop_memo = false;
        match job.open(&src, opts) {
            Err(Error::Unity(e)) if super::shaders::not_one(&src, &e) => {
                job.log(&format!("textures: {rel}: left as it is ({e})"));
                skipped.push((rel.clone(), e.to_string()));
            }
            Err(e) => return Err(left(&work, e, false)),
            Ok(mut f) if f.file_count() > 1 => {
                let r = convert_bundle_files(
                    &mut f,
                    rel,
                    &c,
                    enc.as_mut(),
                    &mut pool,
                    &mut stats,
                    &work,
                    &res_dir,
                    job,
                    &exact,
                )?;
                match r {
                    Err(why) => {
                        job.log(&format!("textures: {rel}: left as it is ({why})"));
                        skipped.push((rel.clone(), why));
                    }
                    Ok(None) => {}
                    Ok(Some(written)) => {
                        let names = f.file_names();
                        // (the journal's scaled sizes first: written with the file's mark)
                        for (k, (sizes, order)) in written.into_iter().enumerate() {
                            if !sizes.is_empty() {
                                let map = scaled_json(&sizes, &order);
                                if let Some(sc) = job.state.setdefault("scaled", Json::dict()) {
                                    sc.set(&format!("{rel}:{}", names[k]), map);
                                }
                            }
                        }
                        job.write_deferred("textures", rel, |tmp| {
                            Ok(f.write_to(tmp, inline_streams)?)
                        })
                        .map_err(|e| left(&work, e, false))?;
                        drop(f);
                        deferred = true;
                        done = true;
                    }
                }
            }
            Ok(mut f) => {
                // the converted textures' new bytes past spill_mb wait in a scratch file, not in
                // memory: a file's inline image data can be more than a handheld has (Blasphemous's
                // resources.assets: 534 MB of ASTC; the G350 ran out at 825 MB); the file comes out
                // the same
                if spill_mb > 0 {
                    // (that scratch file is on the card beside the new file: room for the textures' objects past
                    // spill_mb, counted at their size now (an estimate: ASTC is a quarter of RGBA32, twice DXT1);
                    // a card too full then stops here with status 3, not part way through)
                    let tex: u64 = f
                        .sf
                        .objects
                        .iter()
                        .filter(|o| o.class_id == 28)
                        .map(|o| o.byte_size as u64)
                        .sum();
                    let more = tex.saturating_sub(spill_mb << 20);
                    if more > 0 {
                        job.need_space(need + more, &format!("convert {rel}"))?;
                    }
                    std::fs::create_dir_all(&work)?;
                    f.sf.spill_saved(&work.join("objects.spill"), spill_mb << 20)
                        .map_err(|e| left(&work, e.into(), false))?;
                }
                // `checkpoint_mb`: a file with this much to convert keeps each texture's conversion as
                // it is finished (texmemo.rs), and a run that goes on after a stop takes them from there
                let mut memo = match checkpoint {
                    Some(mb) => {
                        let tex: u64 = f
                            .sf
                            .objects
                            .iter()
                            .filter(|o| o.class_id == 28)
                            .map(|o| o.byte_size as u64)
                            .sum();
                        let weight = (tex + beside).max(if f.is_bundle { bsize } else { 0 });
                        if weight as f64 >= mb * 1048576.0 {
                            let key = format!(
                                "{rel}\n{}\n{}\n{memo_key}",
                                super::texmemo::file_id(&src),
                                String::from_utf8_lossy(&f.sf.unity_version)
                            );
                            super::texmemo::Memo::open(job, &key, need)
                        } else {
                            None
                        }
                    }
                    None => None,
                };
                let mut streams = Streams {
                    job,
                    rel,
                    res_dir,
                    handles: BTreeMap::new(),
                    ahead: Default::default(),
                    ahead_bytes: 0,
                    reader: None,
                };
                let r = convert_file(
                    &mut f,
                    rel,
                    &c,
                    enc.as_mut(),
                    &mut pool,
                    &mut streams,
                    &mut stats,
                    work.join("stream.new"),
                    true,
                    &exact,
                    memo.as_mut(),
                )
                .map_err(|e| left(&work, e, true))?;
                drop(streams);
                let had_memo = memo.is_some();
                if let Some(m) = memo.take() {
                    if m.taken > 0 {
                        job.log(&format!(
                            "textures: {rel}: {} textures ({} MB) as the stopped run converted them (checkpoint_mb)",
                            m.taken,
                            m.taken_bytes >> 20
                        ));
                    }
                }
                if let Some((stream_name, new_stream, sizes, order)) = r {
                    let node = stream_name
                        .as_deref()
                        .map(|n| n.rsplit('/').next().unwrap_or(n).to_string());
                    if f.is_bundle {
                        if let (Some(d), Some(node)) = (new_stream, &node) {
                            f.set_bundle_res(node, d)?;
                        }
                        if !sizes.is_empty() {
                            let map = scaled_json(&sizes, &order);
                            if let Some(sc) = job.state.setdefault("scaled", Json::dict()) {
                                sc.set(rel, map);
                            }
                        }
                        job.write_deferred("textures", rel, |tmp| {
                            Ok(f.write_to(tmp, inline_streams)?)
                        })
                        .map_err(|e| left(&work, e, false))?;
                        drop(f);
                        deferred = true;
                    } else {
                        let sf_new = work.join("file.new");
                        std::fs::create_dir_all(&work)?;
                        texcodec::prof::time(texcodec::prof::Stage::Write, || {
                            f.write_to(&sf_new, false)
                        })
                        .map_err(|e| left(&work, e.into(), false))?;
                        drop(f);
                        let mut pair = vec![(rel.clone(), sf_new)];
                        if let (Some(d), Some(node)) = (new_stream, node) {
                            let p = match d {
                                NodeData::File(p) => p,
                                // (a plain file's stream is always a file: StreamFile's cap 0)
                                NodeData::Bytes(b) => {
                                    let p = work.join("stream.new");
                                    std::fs::write(&p, b).map_err(crate::error::at(&p))?;
                                    p
                                }
                            };
                            let dir = Path::new(rel).parent().map(text).unwrap_or_default();
                            let srel = if dir.is_empty() {
                                node
                            } else {
                                format!("{dir}/{node}")
                            };
                            pair.insert(0, (srel, p));
                        }
                        // the scaled sizes are in the journal on the card before the file is in place: a
                        // run killed between its rename and its mark finds the file converted (nothing left
                        // to scale, so no sizes) and not marked, and the sprites of other files that use its
                        // textures (cross_file_pass) need the sizes then. A file with its stream has them
                        // written with `pending`; a file alone gets a write of the journal for them
                        if !sizes.is_empty() {
                            let map = scaled_json(&sizes, &order);
                            if let Some(sc) = job.state.setdefault("scaled", Json::dict()) {
                                sc.set(rel, map);
                            }
                            if pair.len() == 1 {
                                job.save_state().map_err(|e| left(&work, e, false))?;
                            }
                        }
                        job.commit_then_mark(&pair)
                            .map_err(|e| left(&work, e, false))?;
                        drop_memo = had_memo;
                    }
                    done = true;
                } else if had_memo {
                    drop_memo = true;
                }
            }
        }
        drop_spill(&work);
        if deferred {
            // (marked when its batch is put in place)
        } else if done {
            job.mark_done("textures", rel)?;
        } else {
            job.mark_unchanged("textures", rel)?;
        }
        if drop_memo {
            // (its file is in place and marked, or was left as it is: a bundle's checkpoint stays until
            // the next one or the step's end, its batch may not be in place yet)
            super::texmemo::remove(job);
        }
        if done {
            job.log(&format!(
                "textures: {rel}: {} -> {} MB ({}/{total})",
                (stats.bytes_in - before.0) >> 20,
                (stats.bytes_out - before.1) >> 20,
                i + 1
            ));
        }
    }
    // (the last batch of deferred files in place before the step's summary)
    job.commit_deferred()?;
    super::texmemo::remove(job);
    if only.is_none() {
        cross_file_pass(job, &mut stats)?;
    }
    job.end_work(&work);
    job.log(&format!(
        "textures: {} converted ({} -> {} MB), {} atlas entries and {} sprites follow their scaled textures",
        stats.textures,
        stats.bytes_in >> 20,
        stats.bytes_out >> 20,
        stats.atlas_entries,
        stats.sprites
    ));
    if stats.exact > 0 {
        job.log(&format!(
            "textures: {} textures left exactly as they are (exact_shaders, exact_names: a shader reads their texels as data)",
            stats.exact
        ));
    }
    if stats.released > 0 {
        job.log(&format!(
            "textures: {} readable textures no longer readable (readable = {}): no copy of their pixels in system memory",
            stats.released,
            opt(&c, "readable").as_str().unwrap_or("keep")
        ));
    }
    if let Some(p) = pool.as_mut() {
        for w in p.codecs.iter_mut() {
            enc.absorb(w.as_mut());
        }
    }
    for line in enc.summary() {
        job.log(&line);
    }
    let mut odd = super::shaders::odd_bundles(job);
    odd.retain(|(r, _)| files_rx.is_match(r) && only.as_ref().is_none_or(|o| o.is_match(r)));
    for line in super::shaders::skipped_warnings("textures", &skipped, &odd) {
        job.log(&line);
    }
    Ok(())
}

/// The journal's `scaled` entries of plain files: file -> path id -> sizes (a bundle's entries,
/// `<bundle>:<file>`, are fixed with the bundle).
fn journaled_sizes(state: &Json) -> BTreeMap<String, BTreeMap<i64, [usize; 4]>> {
    let mut out = BTreeMap::new();
    let Some(Json::Dict(files)) = state.get("scaled") else {
        return out;
    };
    for (rel, v) in files {
        let Json::Dict(texs) = v else { continue };
        let mut m = BTreeMap::new();
        for (pid, s) in texs {
            let (Ok(pid), Some(l)) = (pid.parse::<i64>(), s.as_list()) else {
                continue;
            };
            let n: Vec<usize> = l.iter().filter_map(|x| x.as_int()).map(|x| x as usize).collect();
            if let [a, b, c, d] = n[..] {
                m.insert(pid, [a, b, c, d]);
            }
        }
        if !m.is_empty() {
            out.insert(rel.clone(), m);
        }
    }
    out
}

/// After every file's textures: the sprites and atlas entries of other plain files that use a
/// scaled texture (a PPtr with m_FileID > 0, followed through the file's externals by name, in
/// the same folder). fix_references covers a file's own textures only: a sprite sheet's sprites
/// in another sharedassets file kept their full-size rectangles and were drawn from the wrong
/// part of the scaled sheet. A file is marked (textures-refs) when it was changed, with the
/// change (write_once): its values are scaled in place, so it must never be changed twice.
fn cross_file_pass(job: &mut Job, stats: &mut Stats) -> Result<()> {
    let scaled = journaled_sizes(&job.state);
    if scaled.is_empty() {
        return Ok(());
    }
    let all = regex::Regex::new("").map_err(|e| Error::Value(e.to_string()))?;
    for rel in texture_files(job, &all) {
        if job.is_done("textures-refs", &rel) {
            continue;
        }
        let src = job.path_in(&rel);
        let mut f = match job.open(&src, unityfs::Options::default()) {
            Err(Error::Unity(e)) if super::shaders::not_one(&src, &e) => continue,
            Err(e) => return Err(e),
            Ok(f) => f,
        };
        if f.is_bundle || f.file_count() > 1 {
            continue;
        }
        let dir = Path::new(&rel).parent().map(text).unwrap_or_default();
        let by_file: Vec<Option<&BTreeMap<i64, [usize; 4]>>> = f
            .sf
            .externals
            .iter()
            .map(|e| {
                let name = e.name();
                let key = if dir.is_empty() {
                    name
                } else {
                    format!("{dir}/{name}")
                };
                if key == rel {
                    None
                } else {
                    scaled.get(&key)
                }
            })
            .collect();
        if by_file.iter().all(Option::is_none) {
            continue;
        }
        let by = |fid: i64| -> Option<&BTreeMap<i64, [usize; 4]>> {
            *by_file.get(usize::try_from(fid.checked_sub(1)?).ok()?)?
        };
        let before = stats.sprites + stats.atlas_entries;
        if fix_references(&mut f, &by, stats)? {
            job.write_once("textures-refs", &rel, |tmp| Ok(f.write_to(tmp, false)?))?;
            job.log(&format!(
                "textures: {rel}: {} sprites / atlas entries follow textures scaled in other files",
                stats.sprites + stats.atlas_entries - before
            ));
        }
    }
    Ok(())
}

/// The journal's `scaled` entry of a file: path id -> [old w, old h, new w, new h], in the
/// textures' order.
fn scaled_json(sizes: &BTreeMap<i64, [usize; 4]>, order: &[i64]) -> Json {
    Json::Dict(
        order
            .iter()
            .map(|pid| {
                let s = sizes[pid];
                (
                    pid.to_string(),
                    Json::List(s.iter().map(|&x| Json::Int(x as i128)).collect()),
                )
            })
            .collect(),
    )
}

/// Each scaled-sizes map of a bundle's serialized files, with the textures' order.
type Written = Vec<(BTreeMap<i64, [usize; 4]>, Vec<i64>)>;

/// A bundle with several serialized files (a scene bundle): each one's textures converted in
/// turn, each into a new stream of its own, then the sprites and atlases of every one fixed (a
/// PPtr into another of them followed), the new streams set as the bundle's nodes. Ok(None): no
/// texture converted, nothing changed; Err(why) inside: left as it is (serialized files sharing a
/// stream one of them would replace).
#[allow(clippy::too_many_arguments)]
fn convert_bundle_files(
    f: &mut unityfs::DataFile,
    rel: &str,
    c: &Json,
    enc: &mut dyn Codec,
    pool: &mut Option<Pool>,
    stats: &mut Stats,
    work: &Path,
    res_dir: &Path,
    job: &Job,
    exact: &Exact,
) -> Result<std::result::Result<Option<Written>, String>> {
    let n = f.file_count();
    // (the totals as they were, should the bundle be left as it is)
    let before = *stats;
    let mut results = Vec::with_capacity(n);
    for k in 0..n {
        f.select(k)?;
        let label = super::shaders::label(rel, f);
        let mut streams = Streams {
            job,
            rel,
            res_dir: res_dir.to_path_buf(),
            handles: BTreeMap::new(),
            ahead: Default::default(),
            ahead_bytes: 0,
            reader: None,
        };
        let r = convert_file(
            f,
            &label,
            c,
            enc,
            pool,
            &mut streams,
            stats,
            work.join(format!("stream{k}.new")),
            false,
            exact,
            None,
        )
        .map_err(|e| left(work, e, false))?;
        results.push(r);
    }
    let sizes: Vec<BTreeMap<i64, [usize; 4]>> = results
        .iter()
        .map(|r| r.as_ref().map(|x| x.2.clone()).unwrap_or_default())
        .collect();
    let mut changed = results.iter().any(Option::is_some);
    if sizes.iter().any(|m| !m.is_empty()) {
        for k in 0..n {
            f.select(k)?;
            // (each of its PPtrs' files: itself, another serialized file of the bundle, or none)
            let files: Vec<Option<usize>> = (0..=f.sf.externals.len() as i64)
                .map(|fid| f.file_of(fid))
                .collect();
            let by = |fid: i64| -> Option<&BTreeMap<i64, [usize; 4]>> {
                let j = (*files.get(usize::try_from(fid).ok()?)?)?;
                Some(&sizes[j])
            };
            changed |= fix_references(f, &by, stats).map_err(|e| left(work, e, false))?;
        }
    }
    if !changed {
        return Ok(Ok(None));
    }
    // the new streams, each replacing the node its file's textures streamed from: no other file
    // may read that node
    let names = f.file_names();
    let mut replaced: BTreeMap<String, usize> = BTreeMap::new();
    for (k, r) in results.iter().enumerate() {
        if let Some((Some(name), Some(_), _, _)) = r {
            let node = name.rsplit('/').next().unwrap_or(name).to_string();
            if let Some(j) = replaced.insert(node.clone(), k) {
                *stats = before;
                return Ok(Err(format!(
                    "{} and {} both stream textures from {node}",
                    names[j], names[k]
                )));
            }
        }
    }
    if !replaced.is_empty() {
        for k in 0..n {
            f.select(k)?;
            for node in streams_read(f)? {
                if let Some(&j) = replaced.get(&node) {
                    if j != k {
                        *stats = before;
                        return Ok(Err(format!(
                            "{} streams from {node}, which the textures of {} replace",
                            names[k], names[j]
                        )));
                    }
                }
            }
        }
    }
    let mut written = Vec::with_capacity(n);
    for r in results {
        match r {
            Some((name, path, sizes, order)) => {
                if let (Some(name), Some(d)) = (name, path) {
                    let node = name.rsplit('/').next().unwrap_or(&name).to_string();
                    f.set_bundle_res(&node, d)?;
                }
                written.push((sizes, order));
            }
            None => written.push((BTreeMap::new(), Vec::new())),
        }
    }
    Ok(Ok(Some(written)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn splitext() {
        assert_eq!(stem(Path::new("/a/b.assets")), Path::new("/a/b"));
        assert_eq!(stem(Path::new("/a/atlas_x")), Path::new("/a/atlas_x"));
        assert_eq!(stem(Path::new("/a.b/.hidden")), Path::new("/a.b/.hidden"));
        assert_eq!(stem(Path::new("/a/x.tar.gz")), Path::new("/a/x.tar"));
    }

    #[test]
    fn lowmem_scale() {
        let scale = |cfg: &str| {
            let (c, why) = settings(&crate::pyjson::loads(cfg).unwrap()).unwrap();
            (c.get("scale").unwrap().repr(), why.is_some())
        };
        // within ram_mb (every system has at most 2^40 MB): lowmem_scale; above it, or unset: scale
        assert_eq!(
            scale(r#"{"textures": {"lowmem_scale": 0.5, "ram_mb": 1099511627776}}"#),
            ("0.5".into(), true)
        );
        assert_eq!(
            scale(r#"{"textures": {"lowmem_scale": 0.5, "ram_mb": 0}}"#),
            ("1.0".into(), false)
        );
        assert_eq!(
            scale(r#"{"textures": {"scale": 0.75}}"#),
            ("0.75".into(), false)
        );
    }

    #[test]
    fn lowmem_block() {
        let got = |cfg: &str| {
            let (c, why) = settings(&crate::pyjson::loads(cfg).unwrap()).unwrap();
            let k = |k: &str| c.get(k).unwrap().repr();
            (k("scale"), k("block"), k("sharp_block"), why)
        };
        // within ram_mb: both, and the line names both; the text atlases' sharp_block stays
        let (scale, block, sharp, why) = got(
            r#"{"textures": {"lowmem_scale": 0.375, "lowmem_block": [6, 6], "ram_mb": 1099511627776}}"#,
        );
        assert_eq!(
            (scale.as_str(), block.as_str(), sharp.as_str()),
            ("0.375", "[6, 6]", "[4, 4]")
        );
        let why = why.unwrap();
        assert!(
            why.ends_with("scale 0.375 (lowmem_scale), block [6, 6] (lowmem_block)"),
            "{why}"
        );
        // the block alone
        let (scale, block, _, why) =
            got(r#"{"textures": {"lowmem_block": [6, 6], "ram_mb": 1099511627776}}"#);
        assert_eq!((scale.as_str(), block.as_str()), ("1.0", "[6, 6]"));
        assert!(why.unwrap().ends_with(": block [6, 6] (lowmem_block)"));
        // above ram_mb: the config's block
        let (_, block, _, why) = got(r#"{"textures": {"lowmem_block": [6, 6], "ram_mb": 0}}"#);
        assert_eq!((block.as_str(), why), ("[4, 4]", None));
    }
}
