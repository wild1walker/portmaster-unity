//! Unity's files as the setup's Python reads and writes them (convert/pmunity's unity_io.py,
//! bigbundle.py, android_data.py, with UnityPy 1.25.4), byte for byte: what the Python writes for
//! a file, this crate writes, and memory stays bounded (a bundle is never held whole).
//!
//! The API the steps use:
//!
//! ```ignore
//! use unityfs::{ClassDb, DataFile, Options, Value};
//! let db = std::sync::Arc::new(ClassDb::default_db()?);        // UnityPy's lzma.tpk ($PMU_TPK)
//! let mut f = DataFile::open(path, &Options { db: Some(db), spill_dir: Some(work), ..Default::default() })?;
//! for pid in f.path_ids("Texture2D") {                          // Loaded.objects('Texture2D')
//!     let mut t = f.sf.read_typetree(pid)?;                     // o.read_typetree()
//!     t.set("m_Name", Value::str("new"))?;
//!     f.sf.save_typetree(pid, &t)?;                             // o.save_typetree(t)
//! }
//! let pixels = f.read_node("archive:/CAB-x/CAB-x.resS", off, size)?;  // a stream's bytes
//! f.set_bundle_res("CAB-x.resS", unityfs::NodeData::File(new_stream))?;
//! f.write_to(&out, inline)?;                                    // Loaded.write_to / save()
//! ```
//!
//! - [`data`]: [`DataFile`] (unity_io.Loaded: a plain serialized file or a bundle with one),
//!   [`data::BundleFiles`] (a bundle with any number: UnityPy.load(bundle)).
//! - [`serialized`]: [`SerializedFile`]: objects (`objects`, `object`, `objects_of`,
//!   `class_name`), their bytes (`original_bytes`, `bytes`), type trees (`typetree`), values
//!   (`read_typetree`, `read_typetree_with`, `current_typetree`, `save_typetree[_with]`,
//!   `set_raw`), the header fields (`target_platform`, `unity_version`), `save` / `save_to`.
//! - [`value`]: [`Value`], an ordered tree (structs keep their fields' order; numbers their
//!   bits; strings and byte arrays their bytes), `value::read` / `value::write` with a type tree.
//! - [`typetree`], [`tpk`]: [`TypeTree`] (a file's, or the class database's: [`ClassDb`]), with
//!   `with_fields` and `shape` for typetrees.py's fixed trees.
//! - [`bundle`]: UnityFS bundles: [`bundle::Header`], [`bundle::Bundle`] (nodes as sources read a
//!   block at a time; `unpack`), [`bundle::pack`] (UnityPy's save(packer=...) / bigbundle.pack).
//! - [`platform`]: android_data's in-place target platform patching; [`inline`]: texture
//!   streams moved into the textures; [`compress`]: LZ4 / LZ4 HC as python-lz4, LZMA blocks.
//! - [`classes`]: the class names UnityPy gives class ids.
pub mod bin;
pub mod bundle;
pub mod classes;
pub mod compress;
pub mod data;
pub mod error;
pub mod inline;
pub mod platform;
pub mod serialized;
pub mod source;
pub mod tpk;
pub mod typetree;
pub mod value;
pub mod version;

#[cfg(test)]
mod tests;

pub use data::{DataFile, NodeData, Options};
pub use error::{Error, Result};
pub use serialized::{ObjectInfo, SerializedFile};
pub use source::{FileSource, MemSource, SharedSource, Source};
pub use tpk::ClassDb;
pub use typetree::{Kind, Node, TypeTree};
pub use value::{Struct, Value};
pub use version::UnityVersion;
