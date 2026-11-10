//! One data file opened as the setup's Python opens it (unity_io.Loaded): a plain serialized file
//! (.assets, level, globalgamemanagers) or a UnityFS bundle holding one serialized file
//! (CAB-...) and its streams (.resS). Memory stays small whatever the file's size: a plain file's
//! objects are read from the file where needed, a bundle's blocks one at a time as reads reach
//! them (or, a bundle of big LZMA blocks with a spill folder, its nodes unpacked there first),
//! and writing streams the objects and the bundle's chunks.
//!
//! With `Options::all_serialized` a bundle with several serialized files opens too (a scene
//! bundle: `BuildPlayer-<scene>` and `BuildPlayer-<scene>.sharedAssets`, the scene's objects
//! pointing into its shared assets): `sf` is one of them, `select(k)` makes the k-th (the
//! bundle's order) `sf`, and writing saves each one that changed (the others keep their bytes).
//! The Python's Loaded has no such mode: a step that does not ask for it gets the Python's error
//! (`not_one`).
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
    /// A bundle with several serialized files (scene bundles) opened with all of them, not
    /// refused (pmu's own: see the module's text).
    pub all_serialized: bool,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            db: None,
            resource_dir: None,
            streams: Vec::new(),
            spill_dir: None,
            spill_mb: 64,
            all_serialized: false,
        }
    }
}

/// A bundle with no serialized file or several (scene bundles; with `all_serialized`: none): not
/// opened (unity_io's NotOneSerializedFile).
pub fn not_one(path: &Path, n: usize) -> Error {
    Error::Unsupported(format!(
        "{}: expected one serialized file in the bundle, found {n}",
        path.display()
    ))
}

/// A writer that can seek (the header's sizes are written last).
pub trait WriteSeek: std::io::Write + std::io::Seek {}
impl<T: std::io::Write + std::io::Seek> WriteSeek for T {}

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
    /// The serialized files' nodes, in the bundle's order (one unless all_serialized).
    sf_nodes: Vec<usize>,
    /// The node of the file that is `DataFile::sf`.
    sf_node: usize,
    unpacked: Option<PathBuf>,
}

/// An opened data file.
pub struct DataFile {
    pub path: PathBuf,
    pub is_bundle: bool,
    /// The node name of the bundle's serialized file (with several: the selected one's).
    pub cab_name: Option<String>,
    /// The serialized file (a bundle with several: the selected one).
    pub sf: SerializedFile,
    /// A bundle's other serialized files (all_serialized), by node.
    parked: Vec<(usize, SerializedFile)>,
    bundle: Option<BundleState>,
    /// Streams registered by name (Loaded's register_cab): files beside, given ones, bundle nodes.
    streams: HashMap<String, SharedSource>,
    opts: Options,
}

/// The node names UnityPy parses as serialized files (check_file_type, and not a stream by name).
pub fn unitypy_serialized(b: &Bundle) -> Result<Vec<usize>> {
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

/// A bundle with objects of a class in its serialized files written again stored (no block
/// compressed: bundle::STORED_BLOCK_FLAG, the header's flags bundle::stored_data_flag), every
/// node's bytes as they are. Returns how many such objects it has when it was written; None
/// when it has none or is stored already, and the file is then not touched. `work`: a folder
/// of its own for the nodes of a bundle that cannot be read in place (big LZMA blocks), made
/// and removed here.
pub fn store_bundle_of_class(
    path: &Path,
    db: Option<Arc<ClassDb>>,
    class_id: i32,
    work: &Path,
) -> Result<Option<usize>> {
    let b = Bundle::open(path)?;
    if b.is_stored() {
        return Ok(None);
    }
    let _ = std::fs::remove_dir_all(work);
    let res = (|| -> Result<Option<usize>> {
        let srcs: Vec<SharedSource> = if b.lazy_ok() {
            (0..b.header.nodes.len()).map(|i| b.node(i)).collect()
        } else {
            b.unpack(work)?
                .iter()
                .map(|p| Ok(Arc::new(FileSource::open(p)?) as SharedSource))
                .collect::<Result<_>>()?
        };
        let mut found = 0usize;
        for (i, n) in b.header.nodes.iter().enumerate() {
            if [".resS", ".resource"].iter().any(|e| n.name.ends_with(e)) {
                continue;
            }
            let first = srcs[i].read_clipped(0, 48)?;
            if !looks_serialized(&first, srcs[i].len()) {
                continue;
            }
            let sf = SerializedFile::open(
                &n.name,
                srcs[i].clone(),
                db.clone(),
                Some(&b.header.engine),
            )?;
            found += sf.objects_of(class_id).count();
        }
        if found == 0 {
            return Ok(None);
        }
        let nodes: Vec<PackNode> = b
            .header
            .nodes
            .iter()
            .zip(&srcs)
            .map(|(n, s)| PackNode {
                name: n.name.clone(),
                flags: n.flags,
                src: s.clone(),
            })
            .collect();
        let mut tmp = path.as_os_str().to_owned();
        tmp.push(".tmp");
        crate::bundle::pack_file(
            Path::new(&tmp),
            &PackHeader::of(&b.header),
            &nodes,
            crate::bundle::stored_data_flag(&b.header.engine),
            crate::bundle::STORED_BLOCK_FLAG,
        )?;
        std::fs::rename(&tmp, path)?;
        Ok(Some(found))
    })();
    let _ = std::fs::remove_dir_all(work);
    res
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
            if sfs.len() != 1 && !(opts.all_serialized && sfs.len() > 1) {
                return Err(not_one(&path, sfs.len()));
            }
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
            let mut files = Vec::with_capacity(sfs.len());
            for &i in &sfs {
                let cab = b.header.nodes[i].name.clone();
                let first = srcs[i].read_clipped(0, 48)?;
                if !looks_serialized(&first, srcs[i].len()) {
                    return Err(Error::Unsupported(format!(
                        "{}: its node {cab} is not a serialized file",
                        path.display()
                    )));
                }
                let sf = SerializedFile::open(
                    &cab,
                    srcs[i].clone(),
                    opts.db.clone(),
                    Some(&b.header.engine),
                )?;
                files.push((i, sf));
            }
            for (i, n) in b.header.nodes.iter().enumerate() {
                if !sfs.contains(&i) {
                    streams
                        .entry(n.name.clone())
                        .or_insert_with(|| srcs[i].clone());
                }
            }
            let (sf_node, sf) = files.remove(0);
            return Ok(DataFile {
                path,
                is_bundle: true,
                cab_name: Some(b.header.nodes[sf_node].name.clone()),
                sf,
                parked: files,
                bundle: Some(BundleState {
                    bundle: b,
                    srcs,
                    removed: HashSet::new(),
                    sf_nodes: sfs,
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
            parked: Vec::new(),
            bundle: None,
            streams,
            opts: opts.clone(),
        })
    }

    /// The bundle's header (None for a plain file).
    pub fn bundle_header(&self) -> Option<&bundle::Header> {
        self.bundle.as_ref().map(|b| &b.bundle.header)
    }

    /// How many serialized files the file has (one but for a bundle opened with all_serialized).
    pub fn file_count(&self) -> usize {
        1 + self.parked.len()
    }

    /// The serialized files' names (their nodes'), in the bundle's order; a plain file's own.
    pub fn file_names(&self) -> Vec<String> {
        match &self.bundle {
            Some(b) => b
                .sf_nodes
                .iter()
                .map(|&i| b.bundle.header.nodes[i].name.clone())
                .collect(),
            None => vec![self.sf.name.clone()],
        }
    }

    /// Which of them (the bundle's order) `sf` is.
    pub fn selected(&self) -> usize {
        match &self.bundle {
            Some(b) => b.sf_nodes.iter().position(|&i| i == b.sf_node).unwrap_or(0),
            None => 0,
        }
    }

    /// Makes the k-th serialized file (the bundle's order) `sf`; the one that was keeps its
    /// changes until it is selected again or the file is written.
    pub fn select(&mut self, k: usize) -> Result<()> {
        let Some(b) = self.bundle.as_mut() else {
            return if k == 0 {
                Ok(())
            } else {
                Err(Error::Value(format!("no serialized file {k}")))
            };
        };
        let node = *b
            .sf_nodes
            .get(k)
            .ok_or_else(|| Error::Value(format!("no serialized file {k}")))?;
        if node == b.sf_node {
            return Ok(());
        }
        let at = self
            .parked
            .iter()
            .position(|(i, _)| *i == node)
            .ok_or_else(|| Error::Value(format!("no serialized file {k}")))?;
        std::mem::swap(&mut self.sf, &mut self.parked[at].1);
        self.parked[at].0 = b.sf_node;
        b.sf_node = node;
        self.cab_name = Some(b.bundle.header.nodes[node].name.clone());
        Ok(())
    }

    /// The serialized file (the bundle's order) a PPtr's m_FileID in the selected one names,
    /// when it is in this bundle: 0 is the file itself, n its external n - 1, matched by the
    /// external path's last part, as Unity writes it (`archive:/buildplayer-x/buildplayer-x.sharedassets`:
    /// lower case), against the nodes' names without regard to case.
    pub fn file_of(&self, file_id: i64) -> Option<usize> {
        if file_id == 0 {
            return Some(self.selected());
        }
        let ext = self.sf.externals.get(usize::try_from(file_id - 1).ok()?)?;
        let name = ext.name().to_lowercase();
        self.file_names()
            .iter()
            .position(|n| n.to_lowercase() == name)
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
                if !b.sf_nodes.contains(&i) && !b.removed.contains(&i) {
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

    /// The serialized files by node, the selected one too, in the bundle's order.
    fn all_files(&self) -> Vec<(usize, &SerializedFile)> {
        let b = self.bundle.as_ref().unwrap();
        let mut v: Vec<(usize, &SerializedFile)> = vec![(b.sf_node, &self.sf)];
        v.extend(self.parked.iter().map(|(i, sf)| (*i, sf)));
        v.sort_by_key(|x| x.0);
        v
    }

    /// Loaded.save(): the whole file in memory (a bundle: chunked LZ4, UnityPy's packer='lz4').
    /// A bundle with several serialized files: those that changed saved, the others as they are.
    pub fn save(&self) -> Result<Vec<u8>> {
        if !self.is_bundle {
            return self.sf.save();
        }
        let multi = !self.parked.is_empty();
        let mut saved: HashMap<usize, SharedSource> = HashMap::new();
        for (i, sf) in self.all_files() {
            if !multi || sf.changed() {
                saved.insert(i, Arc::new(MemSource(sf.save()?)));
            }
        }
        let nodes = self.pack_nodes(&saved);
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

    /// The nodes to pack: the saved serialized files' new bytes, every other node's current.
    fn pack_nodes(&self, saved: &HashMap<usize, SharedSource>) -> Vec<PackNode> {
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
                src: saved.get(&i).cloned().unwrap_or_else(|| b.srcs[i].clone()),
            })
            .collect()
    }

    /// Loaded.write_to: the file, changed, written to `out` as the Python writes it. `inline` (a
    /// bundle): textures' streamed pixels go into the texture objects, one texture in memory at a
    /// time, and the .resS nodes nothing reads any more are dropped (android_data's inlining).
    pub fn write_to(&mut self, out: &Path, inline: bool) -> Result<()> {
        let mut w = crate::bgwrite::BgWriter::create(out)?;
        let tmp = out.with_file_name(format!(
            "{}.cab.tmp",
            out.file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default()
        ));
        self.write_into(&mut w, inline, &tmp)?;
        w.finish()?;
        Ok(())
    }

    /// write_to into any writer. A bundle's new serialized file is held in memory while the
    /// bundle is packed when the bundle is no bigger than spill_mb (UnityPy holds it so), else
    /// in a file: in the spill folder, or at `tmp` without one. A bundle with several serialized
    /// files: each one that changed (or, `inline`, has textures to inline) is saved so, the
    /// others are packed as they are; a .resS node is dropped when none of them reads it.
    pub fn write_into<W: std::io::Write + std::io::Seek>(
        &mut self,
        out: &mut W,
        inline: bool,
        tmp: &Path,
    ) -> Result<()> {
        if !self.is_bundle {
            return self.sf.save_to(out, |_, _| Ok(None));
        }
        let small =
            self.bundle.as_ref().unwrap().bundle.header.nodes_size() <= self.opts.spill_mb << 20;
        let multi = !self.parked.is_empty();
        let mut tmps: Vec<PathBuf> = Vec::new();
        let res = (|| -> Result<()> {
            let mut saved: HashMap<usize, SharedSource> = HashMap::new();
            let mut still: HashSet<String> = HashSet::new();
            let this = &*self;
            let names = &this.bundle.as_ref().unwrap().bundle.header.nodes;
            for (k, (i, sf)) in this.all_files().into_iter().enumerate() {
                let plan = if inline {
                    let (plan, s) = crate::inline::plan(sf)?;
                    still.extend(s);
                    Some(plan)
                } else {
                    None
                };
                if multi && !sf.changed() && plan.as_ref().is_none_or(HashSet::is_empty) {
                    continue;
                }
                let save = |w: &mut dyn WriteSeek| -> Result<()> {
                    match &plan {
                        Some(plan) => sf.save_to(w, |sf, o| {
                            if !plan.contains(&o.path_id) {
                                return Ok(None);
                            }
                            crate::inline::inlined(sf, o.path_id, |p, off, size| {
                                this.read_node(p, off, size)
                            })
                            .map(Some)
                        }),
                        None => sf.save_to(w, |_, _| Ok(None)),
                    }
                };
                let src: SharedSource = if small {
                    let mut c = std::io::Cursor::new(Vec::new());
                    save(&mut c)?;
                    Arc::new(MemSource(c.into_inner()))
                } else {
                    let tmp = match &this.opts.spill_dir {
                        Some(d) => d.join(format!("{}.new", safe_name(&names[i].name))),
                        None if multi => tmp.with_file_name(format!(
                            "{}.{k}",
                            tmp.file_name()
                                .map(|n| n.to_string_lossy().into_owned())
                                .unwrap_or_default()
                        )),
                        None => tmp.to_path_buf(),
                    };
                    if let Some(d) = tmp.parent() {
                        std::fs::create_dir_all(d)?;
                    }
                    tmps.push(tmp.clone());
                    let f = std::fs::File::create(&tmp)?;
                    let mut w = std::io::BufWriter::with_capacity(1 << 20, f);
                    save(&mut w)?;
                    std::io::Write::flush(&mut w)?;
                    drop(w);
                    Arc::new(FileSource::open(&tmp)?)
                };
                saved.insert(i, src);
            }
            if inline {
                for n in self.node_names() {
                    if n.ends_with(".resS") && !still.contains(&n) {
                        self.remove_node(&n)?;
                    }
                }
            }
            let nodes = self.pack_nodes(&saved);
            bundle::pack(
                out,
                &self.pack_header(),
                &nodes,
                bundle::LZ4_DATA_FLAG,
                bundle::LZ4_BLOCK_FLAG,
                crate::compress::threads(),
            )
        })();
        for t in tmps {
            let _ = std::fs::remove_file(&t);
        }
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

/// A bundle with every node UnityPy opens as a serialized file (any number of them: scene
/// bundles have several), as `UnityPy.load(bundle)` gives them: streaming.repack_bundle sets
/// each one's platform and saves the bundle with packer='lz4'; android_data's verify reads them.
pub struct BundleFiles {
    pub path: PathBuf,
    pub bundle: Bundle,
    /// (node index, its serialized file), in the nodes' order.
    pub files: Vec<(usize, SerializedFile)>,
}

impl BundleFiles {
    pub fn open(path: impl AsRef<Path>, db: Option<Arc<ClassDb>>) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        let bundle = Bundle::open(&path)?;
        let mut files = Vec::new();
        for i in unitypy_serialized(&bundle)? {
            let name = bundle.header.nodes[i].name.clone();
            files.push((
                i,
                SerializedFile::open(
                    &name,
                    bundle.node(i),
                    db.clone(),
                    Some(&bundle.header.engine),
                )?,
            ));
        }
        Ok(BundleFiles {
            path,
            bundle,
            files,
        })
    }

    /// The bundle written to `out` as UnityPy's save(packer='lz4') writes it: every serialized
    /// file saved again (held in `work` while the bundle is packed, else in memory), the other
    /// nodes as they are.
    pub fn write_to(&self, out: &Path, work: Option<&Path>) -> Result<()> {
        self.write_with(out, work, &HashSet::new(), |_, _, _| Ok(None))
    }

    /// write_to with the nodes named in `drop` left out, and `prepare(k, file, object)` called
    /// before each object of the k-th serialized file is written: bytes to write for it this
    /// time (SerializedFile::save_to's prepare: a texture given its pixels).
    pub fn write_with(
        &self,
        out: &Path,
        work: Option<&Path>,
        drop: &HashSet<String>,
        mut prepare: impl FnMut(
            usize,
            &SerializedFile,
            &crate::serialized::ObjectInfo,
        ) -> Result<Option<Vec<u8>>>,
    ) -> Result<()> {
        let mut saved: HashMap<usize, SharedSource> = HashMap::new();
        let mut tmps = Vec::new();
        let res = (|| -> Result<()> {
            for (k, (i, sf)) in self.files.iter().enumerate() {
                let src: SharedSource = match work {
                    Some(d) => {
                        std::fs::create_dir_all(d)?;
                        let p = d.join(format!("sf{k}.new"));
                        tmps.push(p.clone());
                        let mut w =
                            std::io::BufWriter::with_capacity(1 << 20, std::fs::File::create(&p)?);
                        sf.save_to(&mut w, |sf, o| prepare(k, sf, o))?;
                        std::io::Write::flush(&mut w)?;
                        std::mem::drop(w);
                        Arc::new(FileSource::open(&p)?)
                    }
                    None => {
                        let mut c = std::io::Cursor::new(Vec::new());
                        sf.save_to(&mut c, |sf, o| prepare(k, sf, o))?;
                        Arc::new(MemSource(c.into_inner()))
                    }
                };
                saved.insert(*i, src);
            }
            let nodes: Vec<PackNode> = self
                .bundle
                .header
                .nodes
                .iter()
                .enumerate()
                .filter(|(_, n)| !drop.contains(&n.name))
                .map(|(i, n)| PackNode {
                    name: n.name.clone(),
                    flags: n.flags,
                    src: saved
                        .get(&i)
                        .cloned()
                        .unwrap_or_else(|| self.bundle.node(i)),
                })
                .collect();
            bundle::pack_file(
                out,
                &PackHeader::of(&self.bundle.header),
                &nodes,
                bundle::LZ4_DATA_FLAG,
                bundle::LZ4_BLOCK_FLAG,
            )
        })();
        for p in tmps {
            let _ = std::fs::remove_file(p);
        }
        res
    }
}
