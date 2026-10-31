//! Step `textures` (textures.py): desktop texture formats to ASTC, optionally scaled down.
//! Not ported yet: the per-texture work comes from texcodec.
use super::Args;
use crate::error::{Error, Result};
use crate::job::Job;

pub fn run(_job: &mut Job, _args: &Args) -> Result<()> {
    Err(Error::NotPorted("the textures step".into()))
}
