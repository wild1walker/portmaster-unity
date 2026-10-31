//! The crate's error: I/O, or a file this crate cannot read (with what and where).
use std::fmt;
use std::io;

/// An error reading or writing a Unity file.
#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    /// The data is not what the format says (message: what, where).
    Format(String),
    /// A layout or feature the Python leaves alone or cannot write either.
    Unsupported(String),
    /// An object's type tree read a different number of bytes than the object has: UnityPy's
    /// `Expected to read {expected} bytes, but only read {read} bytes` (typetrees.py parses it).
    ReadSize {
        expected: usize,
        read: usize,
    },
    /// A value tree does not fit the type tree it is written with (field, what).
    Value(String),
}

pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io(e) => write!(f, "{e}"),
            Error::Format(s) | Error::Unsupported(s) | Error::Value(s) => f.write_str(s),
            Error::ReadSize { expected, read } => {
                write!(
                    f,
                    "Expected to read {expected} bytes, but only read {read} bytes"
                )
            }
        }
    }
}

impl std::error::Error for Error {}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Self {
        Error::Io(e)
    }
}

impl From<Error> for io::Error {
    fn from(e: Error) -> Self {
        match e {
            Error::Io(e) => e,
            e => io::Error::new(io::ErrorKind::InvalidData, e.to_string()),
        }
    }
}

/// A format error with a message.
pub(crate) fn bad<T>(msg: impl Into<String>) -> Result<T> {
    Err(Error::Format(msg.into()))
}
