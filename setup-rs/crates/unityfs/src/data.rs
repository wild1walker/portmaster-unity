//! One data file opened as the setup's Python opens it (unity_io.Loaded): a plain serialized file
//! (.assets, level, globalgamemanagers) or a UnityFS bundle holding one serialized file
//! (CAB-...) and its streams (.resS). Memory stays small whatever the file's size: a plain file's
//! objects are read from the file where needed, a bundle's blocks one at a time as reads reach
//! them (or, a bundle of big LZMA blocks with a spill folder, its nodes unpacked there first),
//! and writing streams the objects and the bundle's chunks.
//!
//! ```ignore
//! let mut f = DataFile::open(path, &Options { db: Some(db), ..Default::default() })?;
//! for pid in f.path_ids("Texture2D") {
//!     let mut t = f.sf.read_typetree(pid)?;
//!     // ... change t ...
//!     f.sf.save_typetree(pid, &t)?;
//! }
//! f.write_to(out, false)?; // Loaded.write_to / save: the bytes the Python writes
//! ```
use crate::bundle::{self, Bundle, PackHeader, PackNode};
use crate::error::{Error, Result};
use crate::serialized::{looks_serialized, ObjectInfo, SerializedFile};
use crate::source::{FileSource, MemSource, SharedSource};
use crate::tpk::ClassDb;
use crate::value::Value;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;

/// How a file is opened (Loaded's arguments).
#[derive(Clone)]
pub struct Options {
    /// The class database, for files without type trees.
    pub db: Option<Arc<ClassDb>>,
    /// Where a plain file's .resS / .resource streams are when they are not beside it.
    pub resource_dir: Option<PathBuf>,
    /// Streams elsewhere under their own names: (name, file).
    pub streams: Vec<(String, PathBuf)>,
    /// A scratch folder: a bundle of more than spill_mb MB is handled as bigbundle handles it
    /// (its serialized file found by node flags; big LZMA blocks unpacked here), and written
    /// bundles keep their new serialized file here while they are packed.
    pub spill_dir: Option<PathBuf>,
    pub spill_mb: u64,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            db: None,
            resource_dir: None,
            streams: Vec::new(),
            spill_dir: None,
            spill_mb: 64,
        }
    }
}

/// A bundle with no serialized file or several (scene bundles): not opened (unity_io's
/// NotOneSerializedFile).
pub fn not_one(path: &Path, n: usize) -> Error {
    Error::Unsupported(format!(
        "{}: expected one serialized file in the bundle, found {n}",
        path.display()
    ))
}

/// New contents for a bundle node.
pub enum NodeData {
    Bytes(Vec<u8>),
    File(PathBuf),
}

struct BundleState {
    bundle: Bundle,
    /// The nodes' current bytes (the bundle's, or unpacked files).
    srcs: Vec<SharedSource>,
    removed: HashSet<usize>,
    sf_node: usize,
    unpacked: Option<PathBuf>,
}

/// An opened data file.
pub struct DataFile {
    pub path: PathBuf,
    pub is_bundle: bool,
    /// The bundle's serialized file's node name.
    pub cab_name: Option<String>,
    pub sf: SerializedFile,
    bundle: Option<BundleState>,
    /// Streams registered by name (Loaded's register_cab): files beside, given ones, bundle nodes.
    streams: HashMap<String, SharedSource>,
    opts: Options,
}

/// The node names UnityPy parses as serialized files (check_file_type, and not a stream by name).
fn unitypy_serialized(b: &Bundle) -> Result<Vec<usize>> {
    let mut out = Vec::new();
    for (i, n) in b.header.nodes.iter().enumerate() {
        if [".resS", ".resource", ".config", ".xml", ".dat"]
            .iter()
            .any(|e| n.name.ends_with(e))
        {
            continue;
        }
        let src = b.node(i);
        let first = src.read_clipped(0, 48)?;
        if looks_serialized(&first, src.len()) {
            out.push(i);
        }
    }
    Ok(out)
}

impl DataFile {
    /// Opens a plain serialized file or a bundle (Loaded.__init__).
    pub fn open(path: impl AsRef<Path>, opts: &Options) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        let mut streams: HashMap<String, SharedSource> = HashMap::new();
        for (n, p) in &opts.streams {
            streams.insert(n.clone(), Arc::new(FileSource::open(p)?));
        }
        if bundle::is_unityfs(&path) {
            let b = Bundle::open(&path)?;
            let spill = opts.spill_dir.is_some()
                && opts.spill_mb > 0
                && b.header.nodes_size() > opts.spill_mb << 20;
            let sfs = if spill {
                b.header.serialized_nodes()
            } else {
                unitypy_serialized(&b)?
            };
            if sfs.len() != 1 {
                return Err(not_one(&path, sfs.len()));
            }
            let sf_node = sfs[0];
            let mut srcs: Vec<SharedSource> =
                (0..b.header.nodes.len()).map(|i| b.node(i)).collect();
            let mut unpacked = None;
            if !b.lazy_ok() {
                if let Some(d) = &opts.spill_dir {
                    let dir = d.join("unpacked");
                    let files = b.unpack(&dir)?;
                    srcs = files
                        .iter()
                        .map(|p| FileSource::open(p).map(|s| Arc::new(s) as SharedSource))
                        .collect::<std::io::Result<_>>()?;
                    unpacked = Some(dir);
                }
            }
            let cab = b.header.nodes[sf_node].name.clone();
            let first = srcs[sf_node].read_clipped(0, 48)?;
            if !looks_serialized(&first, srcs[sf_node].len()) {
                return Err(Error::Unsupported(format!(
                    "{}: its node {cab} is not a serialized file",
                    path.display()
                )));
            }
            let sf = SerializedFile::open(
                &cab,
                srcs[sf_node].clone(),
                opts.db.clone(),
                Some(&b.header.engine),
            )?;
            for (i, n) in b.header.nodes.iter().enumerate() {
                if i != sf_node {
                    streams
                        .entry(n.name.clone())
                        .or_insert_with(|| srcs[i].clone());
                }
            }
            return Ok(DataFile {
                path,
                is_bundle: true,
                cab_name: Some(cab),
                sf,
                bundle: Some(BundleState {
                    bundle: b,
                    srcs,
                    removed: HashSet::new(),
                    sf_node,
                    unpacked,
                }),
                streams,
                opts: opts.clone(),
            });
        }
        let src: SharedSource = Arc::new(FileSource::open(&path)?);
        let first = src.read_clipped(0, 48)?;
        if !looks_serialized(&first, src.len()) {
            return Err(Error::Unsupported(format!(
                "{}: not a serialized file",
                path.display()
            )));
        }
        let name = path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        let sf = SerializedFile::open(&name, src, opts.db.clone(), None)?;
        // a plain file's streams, beside it or in resource_dir, read from their files where needed
        let stem = Path::new(&name)
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        for n in [
            format!("{name}.resS"),
            format!("{stem}.resource"),
            format!("{stem}.resS"),
        ] {
            if streams.contains_key(&n) {
                continue;
            }
            for d in [
                path.parent().map(Path::to_path_buf),
                opts.resource_dir.clone(),
            ]
            .into_iter()
            .flatten()
            {
                let p = d.join(&n);
                if p.is_file() {
                    streams.insert(n.clone(), Arc::new(FileSource::open(&p)?));
                    break;
                }
            }
        }
        Ok(DataFile {
            path,
            is_bundle: false,
            cab_name: None,
            sf,
            bundle: None,
            streams,
            opts: opts.clone(),
        })
    }

    /// The bundle's header (None for a plain file).
    pub fn bundle_header(&self) -> Option<&bundle::Header> {
        self.bundle.as_ref().map(|b| &b.bundle.header)
    }

    /// The class name UnityPy gives an object (o.type.name).
    pub fn class_name(&self, o: &ObjectInfo) -> String {
        self.sf.class_name(o)
    }

    /// Path ids of the objects of a class (by name), in the file's order (Loaded.objects).
    pub fn path_ids(&self, class: &str) -> Vec<i64> {
        self.sf
            .objects
            .iter()
            .filter(|o| self.class_name(o) == class)
            .map(|o| o.path_id)
            .collect()
    }

    /// A stream registered under a name: a bundle node (its current contents) or a file.
    pub fn stream(&self, name: &str) -> Option<SharedSource> {
        if let Some(b) = &self.bundle {
            if let Some(i) = b.bundle.node_index(name) {
                if i != b.sf_node && !b.removed.contains(&i) {
                    return Some(b.srcs[i].clone());
                }
            }
        }
        self.streams.get(name).cloned()
    }

    /// Loaded.bundle_res: a node of the bundle by name (its current contents).
    pub fn bundle_res(&self, name: &str) -> Option<SharedSource> {
        let b = self.bundle.as_ref()?;
        let i = b.bundle.node_index(name)?;
        Some(b.srcs[i].clone())
    }

    /// Loaded._read_node / a read_stream: size bytes at off of the node or stream named by the
    /// path's last part (fewer at its end, as a slice).
    pub fn read_node(&self, path: &str, off: u64, size: usize) -> Result<Vec<u8>> {
        let name = path.rsplit('/').next().unwrap_or(path);
        let s = self
            .stream(name)
            .ok_or_else(|| Error::Value(format!("{}: no stream {name}", self.path.display())))?;
        Ok(s.read_clipped(off, size)?)
    }

    /// Loaded.set_bundle_res: a node's new contents.
    pub fn set_bundle_res(&mut self, name: &str, data: NodeData) -> Result<()> {
        let b = self
            .bundle
            .as_mut()
            .ok_or_else(|| Error::Value("not a bundle".into()))?;
        let i = b
            .bundle
            .node_index(name)
            .ok_or_else(|| Error::Value(format!("no node {name}")))?;
        b.srcs[i] = match data {
            NodeData::Bytes(v) => Arc::new(MemSource(v)),
            NodeData::File(p) => Arc::new(FileSource::open(p)?),
        };
        Ok(())
    }

    /// A node left out when the bundle is written (`del bundle.files[name]`).
    pub fn remove_node(&mut self, name: &str) -> Result<()> {
        let b = self
            .bundle
            .as_mut()
            .ok_or_else(|| Error::Value("not a bundle".into()))?;
        let i = b
            .bundle
            .node_index(name)
            .ok_or_else(|| Error::Value(format!("no node {name}")))?;
        b.removed.insert(i);
        Ok(())
    }

    /// The bundle's node names, in order (removed ones left out).
    pub fn node_names(&self) -> Vec<String> {
        match &self.bundle {
            Some(b) => b
                .bundle
                .header
                .nodes
                .iter()
                .enumerate()
                .filter(|(i, _)| !b.removed.contains(i))
                .map(|(_, n)| n.name.clone())
                .collect(),
            None => Vec::new(),
        }
    }

    /// Loaded.save(): the whole file in memory (a bundle: chunked LZ4, UnityPy's packer='lz4').
    pub fn save(&self) -> Result<Vec<u8>> {
        if !self.is_bundle {
            return self.sf.save();
        }
        let sfb = self.sf.save()?;
        let nodes = self.pack_nodes(Arc::new(MemSource(sfb)));
        let mut out = std::io::Cursor::new(Vec::new());
        bundle::pack(
            &mut out,
            &self.pack_header(),
            &nodes,
            bundle::LZ4_DATA_FLAG,
            bundle::LZ4_BLOCK_FLAG,
            crate::compress::threads(),
        )?;
        Ok(out.into_inner())
    }

    fn pack_header(&self) -> PackHeader {
        PackHeader::of(&self.bundle.as_ref().unwrap().bundle.header)
    }

    fn pack_nodes(&self, sf: SharedSource) -> Vec<PackNode> {
        let b = self.bundle.as_ref().unwrap();
        b.bundle
            .header
            .nodes
            .iter()
            .enumerate()
            .filter(|(i, _)| !b.removed.contains(i))
            .map(|(i, n)| PackNode {
                name: n.name.clone(),
                flags: n.flags,
                src: if i == b.sf_node {
                    sf.clone()
                } else {
                    b.srcs[i].clone()
                },
            })
            .collect()
    }

    /// Loaded.write_to: the file, changed, written to `out` as the Python writes it. `inline` (a
    /// bundle): textures' streamed pixels go into the texture objects, one texture in memory at a
    /// time, and the .resS nodes nothing reads any more are dropped (android_data's inlining).
    pub fn write_to(&mut self, out: &Path, inline: bool) -> Result<()> {
        if !self.is_bundle {
            let f = std::fs::File::create(out)?;
            let mut w = std::io::BufWriter::with_capacity(1 << 20, f);
            self.sf.save_to(&mut w, |_, _| Ok(None))?;
            std::io::Write::flush(&mut w)?;
            return Ok(());
        }
        let tmp = match &self.opts.spill_dir {
            Some(d) => {
                std::fs::create_dir_all(d)?;
                d.join(format!(
                    "{}.new",
                    safe_name(self.cab_name.as_deref().unwrap_or("cab"))
                ))
            }
            None => out.with_file_name(format!(
                "{}.cab.tmp",
                out.file_name()
                    .map(|n| n.to_string_lossy().into_owned())
                    .unwrap_or_default()
            )),
        };
        let res = (|| -> Result<()> {
            let mut drop_nodes = Vec::new();
            {
                let f = std::fs::File::create(&tmp)?;
                let mut w = std::io::BufWriter::with_capacity(1 << 20, f);
                if inline {
                    let (plan, still) = crate::inline::plan(&self.sf)?;
                    let this = &*self;
                    this.sf.save_to(&mut w, |sf, o| {
                        if !plan.contains(&o.path_id) {
                            return Ok(None);
                        }
                        crate::inline::inlined(sf, o.path_id, |p, off, size| {
                            this.read_node(p, off, size)
                        })
                        .map(Some)
                    })?;
                    for n in self.node_names() {
                        if n.ends_with(".resS") && !still.contains(&n) {
                            drop_nodes.push(n);
                        }
                    }
                } else {
                    self.sf.save_to(&mut w, |_, _| Ok(None))?;
                }
                std::io::Write::flush(&mut w)?;
            }
            for n in drop_nodes {
                self.remove_node(&n)?;
            }
            let nodes = self.pack_nodes(Arc::new(FileSource::open(&tmp)?));
            bundle::pack_file(
                out,
                &self.pack_header(),
                &nodes,
                bundle::LZ4_DATA_FLAG,
                bundle::LZ4_BLOCK_FLAG,
            )
        })();
        let _ = std::fs::remove_file(&tmp);
        res
    }

    /// The bundle's or file's objects' typetree read (o.read_typetree()) by path id.
    pub fn read(&self, path_id: i64) -> Result<Value> {
        self.sf.read_typetree(path_id)
    }
}

impl Drop for DataFile {
    fn drop(&mut self) {
        if let Some(d) = self.bundle.as_ref().and_then(|b| b.unpacked.clone()) {
            let _ = std::fs::remove_dir_all(d);
        }
    }
}

fn safe_name(n: &str) -> String {
    n.chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || "._-".contains(c) {
                c
            } else {
                '_'
            }
        })
        .collect()
}
