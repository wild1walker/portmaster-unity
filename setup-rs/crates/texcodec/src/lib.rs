//! texcodec: the textures step's work on one texture (convert/pmunity/textures.py, astc_guide.py)
//! for pmu, with the bytes the Python writes: UnityPy's decoding of the source formats (Pillow's BCn
//! decoder and raw unpackers, ported), Pillow's Lanczos resize on premultiplied alpha (Pillow's
//! Resample.c), and ASTC encoding with astc-encoder and guided_astc (their C/C++ code, vendored) at
//! the guide's tiers.
//!
//! The Unity-file layer gives each Texture2D's fields and data to [`texture::choose`] and
//! [`texture::convert`] and writes the result back; nothing here reads Unity files.
//!
//! ```no_run
//! # fn main() -> Result<(), texcodec::Error> {
//! use texcodec::texture::{choose, convert, encoder, Settings, Texture};
//! let cfg: serde_json::Value = serde_json::from_str(r#"{"scale": 0.375, "quality": "fast"}"#).unwrap();
//! let s = Settings::from_config(Some(&cfg))?;
//! let mut enc = encoder(&s)?;
//! let t = Texture { name: "atlas_0", width: 2048, height: 2048, format: 12, mip_count: Some(1),
//!                   is_readable: false, dimension: Some(2), image_count: Some(1), platform: 2,
//!                   platform_blob: &[], version: [2017, 4] };
//! let raw = vec![0u8; 2048 * 2048];
//! if let Some(ch) = choose(&t, &s) {
//!     let out = convert(&t, &raw, &ch, &s, &mut enc, "sharedassets0.assets|7", Some("2017.4.9f1"))?;
//!     assert_eq!((out.width, out.height), (768, 768));
//! }
//! # Ok(()) }
//! ```

pub mod astc;
pub mod decode;
pub mod guide;
pub mod image;
pub mod texture;

use std::fmt;

/// What can go wrong; where the Python raises, so does this.
#[derive(Debug)]
pub enum Error {
    /// the settings (textures config)
    Config(String),
    /// texture data that cannot be decoded here, or too short
    Decode(String),
    /// Pillow's resize refused the request
    Resize(String),
    /// astc-encoder or guided_astc failed
    Astc(String),
    /// an ASTC guide file is not one, or is damaged
    Guide(String),
    Io(std::io::Error),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Config(m)
            | Error::Decode(m)
            | Error::Resize(m)
            | Error::Astc(m)
            | Error::Guide(m) => f.write_str(m),
            Error::Io(e) => e.fmt(f),
        }
    }
}

impl std::error::Error for Error {}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Error {
        Error::Io(e)
    }
}
