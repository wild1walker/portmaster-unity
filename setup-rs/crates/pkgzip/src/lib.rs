//! Installers and zips, as the setup's Python tools read and write them (convert/pmunity):
//! macOS installer packages (macpkg.py: xar, cpio, gzip/bzip2/pbzx payloads), zip archives (reading,
//! `python -m zipfile -e`), the aligned STORE zip android-data packs (android_data.py `_write_zip`,
//! `verify_zip`), Unity's player out of donor APKs (player.py `donor`) and patch_obb.py's `new`,
//! `ggm` and `check`. Same files, same bytes, same exit statuses as the Python.
use std::fmt;
use std::io;

pub mod cpio;
pub mod macpkg;
pub mod patch_obb;
pub mod player;
pub mod py;
pub mod pyrandom;
pub mod streams;
pub mod xar;
pub mod zip;
pub mod zipwrite;

/// How a command stops, as the Python does: the message goes to stderr, the status is the exit's.
#[derive(Debug)]
pub enum Error {
    /// An installer cut short (macpkg.Incomplete): exit 3, the setup asks for a new copy.
    Incomplete(String),
    /// What the Python stops with on purpose (`SystemExit(message)`): exit 1.
    Exit(String),
    /// What the Python raises (an exception and its traceback): exit 1.
    Fail(String),
}

impl Error {
    /// The exit status the Python ends with.
    pub fn status(&self) -> u8 {
        match self {
            Error::Incomplete(_) => 3,
            Error::Exit(_) | Error::Fail(_) => 1,
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Incomplete(m) | Error::Exit(m) | Error::Fail(m) => f.write_str(m),
        }
    }
}

impl std::error::Error for Error {}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Self {
        // (an Error carried through an io::Error by a stream keeps its kind)
        if e.get_ref().is_some_and(|i| i.is::<Error>()) {
            return *e.into_inner().unwrap().downcast::<Error>().unwrap();
        }
        Error::Fail(e.to_string())
    }
}

impl From<Error> for io::Error {
    fn from(e: Error) -> Self {
        io::Error::other(e)
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// An exception the Python would raise, as an Err.
pub fn fail<T>(msg: impl Into<String>) -> Result<T> {
    Err(Error::Fail(msg.into()))
}

/// An io::Error with the path it is about (as Python's OSError messages name the file).
pub fn io_at(path: impl AsRef<std::path::Path>) -> impl FnOnce(io::Error) -> Error {
    move |e| Error::Fail(format!("{e}: {:?}", path.as_ref()))
}
