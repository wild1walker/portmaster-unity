//! Unity's files as the setup's Python reads and writes them (convert/pmunity's unity_io.py,
//! bigbundle.py, android_data.py, with UnityPy 1.25.4), byte for byte:
//!
//! - [`bundle`]: UnityFS bundles: header, blocks info, nodes; blocks read one at a time
//!   ([`bundle::Blocks`]), unpacked to files ([`bundle::Bundle::unpack`]); written as UnityPy's
//!   save(packer=...) and bigbundle.pack write them ([`bundle::pack`]).
//! - [`serialized`]: serialized files: header, types (with type trees or from the class
//!   database), objects, scripts, externals, ref types; objects' bytes; saved as UnityPy saves
//!   them, in memory or streamed ([`serialized::SerializedFile::save_to`]).
//! - [`typetree`], [`tpk`]: type trees (the files' own blobs; the TPK class database for files
//!   without them; the common strings).
//! - [`value`]: an object as an ordered value tree and back, with the Python's alignment rules.
//! - [`data`]: one data file as unity_io.Loaded opens it ([`data::DataFile`]): list, read, edit,
//!   write; streams (.resS) read where they are.
//! - [`platform`]: the target platform set in place (android_data's patching, LZ4 literal tricks).
//! - [`inline`]: streamed texture pixels moved into the textures.
//! - [`compress`]: LZ4 / LZ4 HC (the C library python-lz4 bundles), LZMA decompression.
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

pub use data::{DataFile, NodeData, Options};
pub use error::{Error, Result};
pub use serialized::{ObjectInfo, SerializedFile};
pub use source::{FileSource, MemSource, SharedSource, Source};
pub use tpk::ClassDb;
pub use typetree::{Kind, Node, TypeTree};
pub use value::{Struct, Value};
pub use version::UnityVersion;
