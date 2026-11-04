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

fn defaults() -> Vec<(&'static str, Json)> {
    let l = |a: i128, b: i128| Json::List(vec![Json::Int(a), Json::Int(b)]);
    vec![
        ("scale", Json::Float(1.0)),
        ("lowmem_scale", Json::Null),
        ("ram_mb", Json::Int(1024)),
        ("block", l(4, 4)),
        ("quality", Json::Str("medium".into())),
        ("mode", Json::Str("guided".into())),
        ("guided_max_err", Json::Int(64)),
        ("guided_line", Json::Int(128)),
        ("guided_near", Json::Int(1024)),
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
    ]
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
/// they come (StreamFile).
/// (The file is made with the first blob: most of a game's files have no textures.)
struct StreamFile {
    path: PathBuf,
    f: Option<unityfs::bgwrite::BgWriter>,
    size: u64,
    /// in place: its data synced as it is written (it is synced before its rename)
    sync: bool,
}

impl StreamFile {
    fn new(path: PathBuf, sync: bool) -> Result<StreamFile> {
        Ok(StreamFile {
            path,
            f: None,
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
            self.f = Some(w);
        }
        Ok(self.f.as_mut().unwrap())
    }

    fn add(&mut self, blob: &[u8]) -> Result<u64> {
        let pad = (16 - self.size % 16) % 16;
        let path = self.path.clone();
        let f = self.file()?;
        f.write_all(&vec![0u8; pad as usize])
            .and_then(|_| f.write_all(blob))
            .map_err(crate::error::at(&path))?;
        let off = self.size + pad;
        self.size = off + blob.len() as u64;
        Ok(off)
    }

    fn close(mut self) -> Result<PathBuf> {
        self.file()?;
        self.f
            .take()
            .unwrap()
            .finish()
            .map_err(crate::error::at(&self.path))?;
        Ok(self.path)
    }
}

/// Totals for the log (stats).
#[derive(Clone, Copy, Default)]
struct Stats {
    textures: u64,
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
type ReadReq = (PathBuf, u64, u64, SyncSender<std::io::Result<Vec<u8>>>);

/// The most a stream read ahead may be, and what the streams read ahead may hold together: they
/// wait beside the textures being converted.
const AHEAD_MAX: u64 = 32 << 20;
const AHEAD_BUDGET: u64 = 32 << 20;

impl Streams<'_> {
    /// Has a stream read by the reader thread (a plain file's: a bundle's nodes are read when
    /// needed), for read() to take: the card reads the next textures while the workers convert
    /// these. Left to read() when the streams ahead already hold AHEAD_BUDGET.
    /// False when the streams ahead hold AHEAD_BUDGET already: the caller offers this one again
    /// later (the queue is in the textures' order).
    fn read_ahead(&mut self, f: &unityfs::DataFile, path: &str, off: u64, size: u64) -> bool {
        if f.is_bundle || size > AHEAD_MAX {
            return true;
        }
        if self.ahead_bytes + size > AHEAD_BUDGET {
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
                    for (p, off, size, reply) in rx {
                        if stopped.load(std::sync::atomic::Ordering::Relaxed) {
                            break;
                        }
                        if open.as_ref().is_none_or(|(q, _)| *q != p) {
                            open = std::fs::File::open(&p).ok().map(|h| (p.clone(), h));
                        }
                        let r = match open.as_mut() {
                            Some((_, h)) => read_range(h, off, size),
                            None => Err(std::io::Error::other("not opened")),
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
        let p = self.resolve(path);
        if self
            .reader
            .as_ref()
            .unwrap()
            .tx
            .send((p, off, size, tx))
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

/// What convert_file leaves: the stream's name (None: no texture streams), the new stream file
/// and the scaled textures' sizes; None when no texture was converted.
type FileResult = Option<(
    Option<String>,
    Option<PathBuf>,
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
    Conv {
        pid: i64,
        t: Value,
        tex: Tex,
        sd: Option<(u64, u64, String)>,
        est: u64,
        out: Out,
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

/// How many textures ahead the step reads the objects of (and has their streams read).
const LOOK: usize = 8;

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
            Turn::Conv {
                pid,
                mut t,
                tex,
                sd,
                est,
                out,
            } => {
                let out = match out {
                    Out::Now(r) => r,
                    Out::Later(rx) => {
                        self.inflight -= est;
                        self.later -= 1;
                        rx.recv().unwrap_or_else(|_| stopped(self.rel))
                    }
                    Out::Strips {
                        rxs,
                        mut parts,
                        template,
                    } => {
                        self.inflight -= est;
                        self.later -= 1;
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
                if (out.width, out.height) != (tex.width, tex.height) {
                    self.sizes
                        .insert(pid, [tex.width, tex.height, out.width, out.height]);
                    self.order.push(pid);
                }
                stats.bytes_in += match &sd {
                    Some((_, size, _)) => *size,
                    None => t
                        .get("image data")
                        .and_then(Value::as_bytes)
                        .map_or(0, |b| b.len() as u64),
                };
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
) -> Result<FileResult> {
    let unity = String::from_utf8_lossy(&f.sf.unity_version).into_owned();
    let (vmaj, vmin) = {
        let v = f.sf.version.triple();
        (v.0 as i64, v.1 as i64)
    };
    let platform = f.sf.target_platform;
    let inline = opt(c, "inline").truthy();
    let mut stream_name: Option<String> = None;
    let mut turns = Turns {
        rel,
        q: Default::default(),
        inflight: 0,
        later: 0,
        stream: StreamFile::new(stream_path, streams.job.inplace)?,
        sizes: BTreeMap::new(),
        order: Vec::new(),
        converted: 0,
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
                let t = match next.pop_front() {
                    Some(t) => t?,
                    None => f.sf.read_typetree(pid)?,
                };
                let sd = stream_data(&t);
                looked = looked.max(k + 1);
                offered = offered.max(k + 1);
                while looked < textures.len() && looked < k + 1 + LOOK {
                    next.push_back(f.sf.read_typetree(textures[looked]).map_err(Error::from));
                    looked += 1;
                }
                // (their streams offered in order, as far as the budget goes)
                while offered < looked {
                    if let Ok(Some((off, size, path))) =
                        next[offered - k - 1].as_ref().map(stream_data)
                    {
                        if !streams.read_ahead(f, &path, off, size) {
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
                let tex = Tex {
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
                let Some(ch) = enc.choose(&tex) else {
                    return Ok(match &sd {
                        Some((off, size, path)) => Some(Turn::Keep {
                            pid,
                            blob: streams.read(f, path, *off, *size)?,
                            t,
                        }),
                        None => None,
                    });
                };
                let raw = match &sd {
                    Some((off, size, path)) => streams.read(f, path, *off, *size)?,
                    None => t
                        .get("image data")
                        .and_then(Value::as_bytes)
                        .unwrap_or(&[])
                        .to_vec(),
                };
                let key = format!("{rel}|{pid}");
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
                        est,
                        out: Out::Strips {
                            parts: (0..n).map(|_| None).collect(),
                            rxs,
                            template,
                        },
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
                        est,
                        out: Out::Now(out),
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
                    est,
                    out: Out::Later(rx),
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
    if turns.converted == 0 {
        return Ok(None);
    }
    let mut stream = turns.stream;
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

/// The step's settings: the config's over the defaults, and `lowmem_scale` (when set) as `scale`
/// where the system has at most `ram_mb` of RAM (a 1 GB handheld), with the line saying so.
fn settings(cfg: &Json) -> Result<(Json, Option<String>)> {
    let mut c = section(cfg, "textures", &defaults())?;
    let mut why = None;
    if let Some(s) = c
        .get("lowmem_scale")
        .filter(|v| !matches!(v, Json::Null))
        .cloned()
    {
        let ram = c
            .get("ram_mb")
            .and_then(Json::as_int)
            .ok_or_else(|| Error::Value("textures.ram_mb: not an integer".into()))?;
        let mem = super::quality::mem_total_mb();
        if mem <= ram {
            why = Some(format!(
                "textures: {mem} MB of RAM (at most {ram}): scale {} (lowmem_scale)",
                s.repr()
            ));
            c.set("scale", s);
        }
    }
    Ok((c, why))
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
        let mut done = false;
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
                )?;
                match r {
                    Err(why) => {
                        job.log(&format!("textures: {rel}: left as it is ({why})"));
                        skipped.push((rel.clone(), why));
                    }
                    Ok(None) => {}
                    Ok(Some(written)) => {
                        let names = f.file_names();
                        job.write_with(rel, |tmp| Ok(f.write_to(tmp, inline_streams)?))
                            .map_err(|e| left(&work, e, false))?;
                        drop(f);
                        for (k, (sizes, order)) in written.into_iter().enumerate() {
                            if !sizes.is_empty() {
                                let map = scaled_json(&sizes, &order);
                                if let Some(sc) = job.state.setdefault("scaled", Json::dict()) {
                                    sc.set(&format!("{rel}:{}", names[k]), map);
                                }
                            }
                        }
                        done = true;
                    }
                }
            }
            Ok(mut f) => {
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
                )
                .map_err(|e| left(&work, e, true))?;
                drop(streams);
                if let Some((stream_name, new_stream, sizes, order)) = r {
                    let node = stream_name
                        .as_deref()
                        .map(|n| n.rsplit('/').next().unwrap_or(n).to_string());
                    if f.is_bundle {
                        if let (Some(p), Some(node)) = (new_stream, &node) {
                            f.set_bundle_res(node, NodeData::File(p))?;
                        }
                        job.write_with(rel, |tmp| Ok(f.write_to(tmp, inline_streams)?))
                            .map_err(|e| left(&work, e, false))?;
                        drop(f);
                    } else {
                        let sf_new = work.join("file.new");
                        std::fs::create_dir_all(&work)?;
                        texcodec::prof::time(texcodec::prof::Stage::Write, || {
                            f.write_to(&sf_new, false)
                        })
                        .map_err(|e| left(&work, e.into(), false))?;
                        drop(f);
                        let mut pair = vec![(rel.clone(), sf_new)];
                        if let (Some(p), Some(node)) = (new_stream, node) {
                            let dir = Path::new(rel).parent().map(text).unwrap_or_default();
                            let srel = if dir.is_empty() {
                                node
                            } else {
                                format!("{dir}/{node}")
                            };
                            pair.insert(0, (srel, p));
                        }
                        job.commit_then_mark(&pair)
                            .map_err(|e| left(&work, e, false))?;
                    }
                    if !sizes.is_empty() {
                        let map = scaled_json(&sizes, &order);
                        if let Some(sc) = job.state.setdefault("scaled", Json::dict()) {
                            sc.set(rel, map);
                        }
                    }
                    done = true;
                }
            }
        }
        drop_spill(&work);
        if done {
            job.mark_done("textures", rel)?;
        } else {
            job.mark_unchanged("textures", rel)?;
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
    job.end_work(&work);
    job.log(&format!(
        "textures: {} converted ({} -> {} MB), {} atlas entries and {} sprites follow their scaled textures",
        stats.textures,
        stats.bytes_in >> 20,
        stats.bytes_out >> 20,
        stats.atlas_entries,
        stats.sprites
    ));
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
                if let (Some(name), Some(p)) = (name, path) {
                    let node = name.rsplit('/').next().unwrap_or(&name).to_string();
                    f.set_bundle_res(&node, NodeData::File(p))?;
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
}
