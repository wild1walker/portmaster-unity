//! The shaders step's work on one shader, as pure functions with the Python's output byte for
//! byte (convert/pmunity/shaders: blob.py, glsl.py, convert.py's add_gles3, step.py's summary):
//!
//! * [`blob`]: the per-platform program blob of a Shader (LZ4 blocks, entry tables, entries);
//! * [`lz4`]: LZ4 blocks as python-lz4 reads and writes them (LZ4HC level 9, liblz4 1.9.4);
//! * [`glsl`]: OpenGL Core GLSL -> GLSL ES 3.00 / 3.10 (a program's text in, its text out), and
//!   compute kernels -> GLSL ES 3.10 (`kernel_to_es`);
//! * [`convert`]: a Shader's fields in ([`convert::Shader`]), the fields the step writes out
//!   ([`convert::Edit`]) and its report;
//! * [`step`]: the step's summary lines;
//! * [`bindings`]: Unity's binding info of a blob entry, and the binding info Unity's GL compiler
//!   gives a GL program (dxbc.py's Bindings and gl_bindings);
//! * [`fusedpost`]: `fused_post`'s fused program (the Shader object's side is the convert crate's);
//! * [`grabfetch`]: `grab_fetch`'s check of a program and its rewrite (the Shader object's side is
//!   the convert crate's);
//! * [`tree`] (feature `tree`): a Shader object's tree (unityfs) and its blob.
//!
//! Only the OpenGL Core source is ported (Direct3D 11, Vulkan and Metal programs are not).
pub mod bindings;
pub mod blob;
pub mod convert;
pub mod dxbc;
pub mod fusedpost;
pub mod glsl;
pub mod grabfetch;
pub mod lz4;
pub mod py;
pub mod spirv;
pub mod step;
#[cfg(feature = "tree")]
pub mod tree;
pub mod vulkan;

pub use convert::{add_gles3, Edit, Options, Outcome, Report, Shader, Status, Translations};
