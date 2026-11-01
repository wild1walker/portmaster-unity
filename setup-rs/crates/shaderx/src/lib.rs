//! The shaders step's work on one shader, as pure functions with the Python's output byte for
//! byte (convert/pmunity/shaders: blob.py, glsl.py, convert.py's add_gles3, step.py's summary):
//!
//! * [`blob`]: the per-platform program blob of a Shader (LZ4 blocks, entry tables, entries);
//! * [`lz4`]: LZ4 blocks as python-lz4 reads and writes them (LZ4HC level 9, liblz4 1.9.4);
//! * [`glsl`]: OpenGL Core GLSL -> GLSL ES 3.00 / 3.10 (a program's text in, its text out);
//! * [`convert`]: a Shader's fields in ([`convert::Shader`]), the fields the step writes out
//!   ([`convert::Edit`]) and its report;
//! * [`step`]: the step's summary lines.
//!
//! Only the OpenGL Core source is ported (Direct3D 11, Vulkan and Metal programs, compute
//! kernels, grab_fetch and fused_post are not).
pub mod blob;
pub mod convert;
pub mod glsl;
pub mod lz4;
pub mod py;
pub mod step;

pub use convert::{add_gles3, Edit, Options, Outcome, Report, Shader, Status, Translations};
