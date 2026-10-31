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
//! (`scaled`). The codec is texcodec's; without it the step stops as not ported.
use super::{field, field_mut, float, int, objects, opt, section, Args};
use crate::error::{Error, Result};
use crate::job::{text, Job};
use crate::pyjson::Json;
use std::collections::BTreeMap;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
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
}

/// The step's codec for its settings (logging what the Python logs while it sets up: a missing
/// guide), or not ported.
fn codec(_c: &Json, _job: &Job) -> Result<Box<dyn Codec>> {
    Err(Error::NotPorted("the textures step (texcodec)".into()))
}

fn defaults() -> Vec<(&'static str, Json)> {
    let l = |a: i128, b: i128| Json::List(vec![Json::Int(a), Json::Int(b)]);
    vec![
        ("scale", Json::Float(1.0)),
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
struct StreamFile {
    path: PathBuf,
    f: std::io::BufWriter<std::fs::File>,
    size: u64,
}

impl StreamFile {
    fn new(path: PathBuf) -> Result<StreamFile> {
        let f = std::io::BufWriter::with_capacity(1 << 20, std::fs::File::create(&path)?);
        Ok(StreamFile { path, f, size: 0 })
    }

    fn add(&mut self, blob: &[u8]) -> Result<u64> {
        let pad = (16 - self.size % 16) % 16;
        self.f.write_all(&vec![0u8; pad as usize])?;
        self.f.write_all(blob)?;
        let off = self.size + pad;
        self.size = off + blob.len() as u64;
        Ok(off)
    }

    fn close(mut self) -> Result<PathBuf> {
        self.f.flush()?;
        Ok(self.path)
    }
}

/// Totals for the log (stats).
#[derive(Default)]
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

/// The texture a PPtr names in this file, when it is one of the scaled ones.
fn scaled_ref<'a>(
    r: &Value,
    sizes: &'a BTreeMap<i64, [usize; 4]>,
) -> Result<Option<&'a [usize; 4]>> {
    if int(r, "m_FileID")? != 0 {
        return Ok(None);
    }
    Ok(sizes.get(&(int(r, "m_PathID")? as i64)))
}

/// Sprite atlases and sprites of one file that use its scaled textures (fix_references), saved.
fn fix_references(
    f: &mut unityfs::DataFile,
    sizes: &BTreeMap<i64, [usize; 4]>,
    stats: &mut Stats,
) -> Result<()> {
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
            }
        }
    }
    for (pid, class) in objects(f) {
        if class == "Sprite" {
            let mut t = f.sf.read_typetree(pid)?;
            let s = scaled_ref(field(field(&t, "m_RD")?, "texture")?, sizes)?.copied();
            if let Some(s) = s {
                scale_own_sprite(&mut t, s[2] as f64 / s[0] as f64, s[3] as f64 / s[1] as f64)?;
                f.sf.save_typetree(pid, &t)?;
                stats.sprites += 1;
            }
        }
    }
    Ok(())
}

/// Reads a stream's bytes: a bundle's node, else the stream file beside the converted file (or
/// in the source folder).
struct Streams<'a> {
    job: &'a Job,
    rel: &'a str,
    res_dir: PathBuf,
    handles: BTreeMap<PathBuf, std::fs::File>,
}

impl Streams<'_> {
    fn read(&mut self, f: &unityfs::DataFile, path: &str, off: u64, size: u64) -> Result<Vec<u8>> {
        let name = path.rsplit('/').next().unwrap_or(path);
        if f.is_bundle {
            return Ok(f.read_node(name, off, size as usize)?);
        }
        let dir = Path::new(self.rel)
            .parent()
            .map(|d| d.to_string_lossy().into_owned())
            .unwrap_or_default();
        let rel = if dir.is_empty() {
            name.to_string()
        } else {
            format!("{dir}/{name}")
        };
        let mut p = self.job.path_in(&rel);
        if !p.exists() {
            p = self.res_dir.join(name);
        }
        if !self.handles.contains_key(&p) {
            self.handles.insert(p.clone(), std::fs::File::open(&p)?);
        }
        let h = self.handles.get_mut(&p).unwrap();
        h.seek(SeekFrom::Start(off))?;
        let mut buf = Vec::new();
        h.take(size).read_to_end(&mut buf)?;
        Ok(buf)
    }
}

/// What convert_file leaves: the stream's name (None: no texture streams), the new stream file
/// and the scaled textures' sizes; None when no texture was converted.
type FileResult = Option<(
    Option<String>,
    Option<PathBuf>,
    BTreeMap<i64, [usize; 4]>,
    Vec<i64>,
)>;

#[allow(clippy::too_many_arguments)]
fn convert_file(
    f: &mut unityfs::DataFile,
    rel: &str,
    c: &Json,
    enc: &mut dyn Codec,
    streams: &mut Streams,
    stats: &mut Stats,
    stream_path: PathBuf,
) -> Result<FileResult> {
    let unity = String::from_utf8_lossy(&f.sf.unity_version).into_owned();
    let (vmaj, vmin) = {
        let v = f.sf.version.triple();
        (v.0 as i64, v.1 as i64)
    };
    let platform = f.sf.target_platform;
    let inline = opt(c, "inline").truthy();
    let mut stream_name: Option<String> = None;
    let mut stream = Some(StreamFile::new(stream_path)?);
    let mut sizes = BTreeMap::new();
    let mut order = Vec::new();
    let mut converted = 0u64;
    let textures = f.path_ids("Texture2D");
    for pid in textures {
        let mut t = f.sf.read_typetree(pid)?;
        let sd = stream_data(&t);
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
            continue;
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
        let ch = enc.choose(&tex);
        let st = stream.as_mut().unwrap();
        let Some(ch) = ch else {
            if let Some((off, size, path)) = &sd {
                let blob = streams.read(f, path, *off, *size)?;
                let at = st.add(&blob)?;
                set_stream(&mut t, at, None)?;
                f.sf.save_typetree(pid, &t)?;
            }
            continue;
        };
        let raw = match &sd {
            Some((off, size, path)) => streams.read(f, path, *off, *size)?,
            None => t
                .get("image data")
                .and_then(Value::as_bytes)
                .unwrap_or(&[])
                .to_vec(),
        };
        let out = enc.convert(&tex, &raw, &ch, &format!("{rel}|{pid}"), Some(&unity))?;
        drop(raw);
        if (out.width, out.height) != (tex.width, tex.height) {
            sizes.insert(pid, [tex.width, tex.height, out.width, out.height]);
            order.push(pid);
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
            let at = st.add(&out.data)?;
            set_stream(&mut t, at, Some(out.data.len() as u64))?;
            t.set("image data", Value::Bytes(Vec::new()))?;
        } else {
            t.set("image data", Value::Bytes(out.data))?;
        }
        f.sf.save_typetree(pid, &t)?;
        converted += 1;
    }
    if converted == 0 {
        return Ok(None);
    }
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
                    let at = stream.as_mut().unwrap().add(&blob)?;
                    set_stream(&mut t, at, None)?;
                    f.sf.save_typetree(pid, &t)?;
                }
            }
        }
    }
    stats.textures += converted;
    fix_references(f, &sizes, stats)?;
    let path = stream.take().unwrap().close()?;
    Ok(Some((
        stream_name.clone(),
        stream_name.map(|_| path),
        sizes,
        order,
    )))
}

pub fn run(job: &mut Job, args: &Args) -> Result<()> {
    let c = section(&job.cfg, "textures", &defaults())?;
    let enc = codec(&c, job)?;
    run_with(job, args, enc)
}

/// The step with a given codec (texcodec's in a setup; tests give their own).
pub fn run_with(job: &mut Job, args: &Args, mut enc: Box<dyn Codec>) -> Result<()> {
    let c = section(&job.cfg, "textures", &defaults())?;
    let rx = |k: &str| {
        let s = opt(&c, k).to_str();
        regex::Regex::new(&s).map_err(|e| Error::Value(format!("textures.{k} {s:?}: {e}")))
    };
    let mut files = texture_files(job, &rx("files")?);
    if let Some(only) = &args.only {
        let r =
            regex::Regex::new(only).map_err(|e| Error::Value(format!("--only {only:?}: {e}")))?;
        files.retain(|f| r.is_match(f));
    }
    let spill_mb = opt(&c, "spill_mb").as_int().unwrap_or(0).max(0) as u64;
    let inline_streams = opt(&c, "inline_streams").truthy();
    let mut stats = Stats::default();
    let total = files.len();
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
        let work = job.work_dir("textures")?;
        let opts = unityfs::Options {
            resource_dir: Some(res_dir.clone()),
            spill_dir: Some(work.join("nodes")),
            spill_mb,
            ..Default::default()
        };
        let mut done = false;
        match job.open(&src, opts) {
            Err(Error::Unity(e)) if super::shaders::not_one(&src, &e) => {
                job.log(&format!("textures: {rel}: left as it is ({e})"));
            }
            Err(e) => return Err(e),
            Ok(mut f) => {
                let mut streams = Streams {
                    job,
                    rel,
                    res_dir,
                    handles: BTreeMap::new(),
                };
                let r = convert_file(
                    &mut f,
                    rel,
                    &c,
                    enc.as_mut(),
                    &mut streams,
                    &mut stats,
                    work.join("stream.new"),
                )?;
                drop(streams);
                if let Some((stream_name, new_stream, sizes, order)) = r {
                    let node = stream_name
                        .as_deref()
                        .map(|n| n.rsplit('/').next().unwrap_or(n).to_string());
                    if f.is_bundle {
                        if let (Some(p), Some(node)) = (new_stream, &node) {
                            f.set_bundle_res(node, NodeData::File(p))?;
                        }
                        job.write_with(rel, |tmp| Ok(f.write_to(tmp, inline_streams)?))?;
                        drop(f);
                    } else {
                        let sf_new = work.join("file.new");
                        f.write_to(&sf_new, false)?;
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
                        job.commit(&pair)?;
                    }
                    if !sizes.is_empty() {
                        let map = Json::Dict(
                            order
                                .iter()
                                .map(|pid| {
                                    let s = sizes[pid];
                                    (
                                        pid.to_string(),
                                        Json::List(
                                            s.iter().map(|&x| Json::Int(x as i128)).collect(),
                                        ),
                                    )
                                })
                                .collect(),
                        );
                        if let Some(sc) = job.state.setdefault("scaled", Json::dict()) {
                            sc.set(rel, map);
                        }
                    }
                    done = true;
                }
            }
        }
        job.end_work(&work);
        job.mark_done("textures", rel)?;
        if done {
            job.log(&format!(
                "textures: {rel}: {} -> {} MB ({}/{total})",
                (stats.bytes_in - before.0) >> 20,
                (stats.bytes_out - before.1) >> 20,
                i + 1
            ));
        }
    }
    job.log(&format!(
        "textures: {} converted ({} -> {} MB), {} atlas entries and {} sprites follow their scaled textures",
        stats.textures,
        stats.bytes_in >> 20,
        stats.bytes_out >> 20,
        stats.atlas_entries,
        stats.sprites
    ));
    for line in enc.summary() {
        job.log(&line);
    }
    Ok(())
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
}
