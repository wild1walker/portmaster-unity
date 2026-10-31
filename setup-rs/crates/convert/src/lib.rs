//! The setup's `convert` command (convert/pmunity's __main__.py convert, job.py and the steps a
//! game's config runs) on unityfs and shaderx: what `python -m pmunity convert` writes, byte for
//! byte, journal included, with its log lines (timings apart) and exit statuses.
//!
//! - [`cli`]: the arguments, the config and `--set`, the step order, the run.
//! - [`job`]: [`job::Job`], the in-place conversion with its journal `.pmunity.json`.
//! - [`steps`]: shaders, settings, audio, textures and Cuphead's plugin step animclips.
//! - [`pyjson`]: JSON and the Python values the job prints, as Python has them.
//! - [`version`]: the data's Unity version and the check made first.
pub mod cli;
pub mod error;
pub mod job;
pub mod pyjson;
pub mod steps;
pub mod version;

pub use error::{Error, Result};
pub use job::Job;
