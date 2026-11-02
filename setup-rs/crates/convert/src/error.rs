//! How a conversion stops, as the Python's does: `sys.exit(message)` (status 1), a status whose
//! message the log already has (need_space: 3), a part not ported to Rust (status 2: the setup
//! runs the Python), and the errors the Python raises (a traceback there; status 1).
use std::fmt;

#[derive(Debug)]
pub enum Error {
    /// sys.exit(message) / SystemExit: the message on stderr, status 1.
    Exit(String),
    /// A status whose reason is already logged (need_space: 3).
    Status(u8),
    /// A step, option or source not ported: status 2 (run `python -m pmunity convert`).
    NotPorted(String),
    /// What the Python raises (ValueError, KeyError, ...): its message, status 1.
    Value(String),
    Unity(unityfs::Error),
    Io(std::io::Error),
}

pub type Result<T> = std::result::Result<T, Error>;

impl Error {
    /// The process's exit status for this error.
    pub fn status(&self) -> u8 {
        match self {
            Error::Status(s) => *s,
            Error::NotPorted(_) => 2,
            _ => 1,
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Exit(m) | Error::Value(m) => f.write_str(m),
            Error::Status(s) => write!(f, "exit status {s}"),
            Error::NotPorted(m) => {
                write!(f, "{m}: not ported yet (run `python -m pmunity convert`)")
            }
            Error::Unity(e) => e.fmt(f),
            Error::Io(e) => e.fmt(f),
        }
    }
}

impl std::error::Error for Error {}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::Io(e)
    }
}

impl From<unityfs::Error> for Error {
    fn from(e: unityfs::Error) -> Self {
        match e {
            unityfs::Error::Io(e) => Error::Io(e),
            e => Error::Unity(e),
        }
    }
}

/// An I/O error with the file it happened to (its kind kept: a full card is told by it).
pub fn at(p: &std::path::Path) -> impl FnOnce(std::io::Error) -> Error + '_ {
    move |e| {
        Error::Io(std::io::Error::new(
            e.kind(),
            format!("{}: {e}", p.display()),
        ))
    }
}

impl Error {
    /// The card filled up (ENOSPC, EDQUOT).
    pub fn is_full(&self) -> bool {
        let io = match self {
            Error::Io(e) => e,
            Error::Unity(unityfs::Error::Io(e)) => e,
            _ => return false,
        };
        matches!(
            io.kind(),
            std::io::ErrorKind::StorageFull | std::io::ErrorKind::QuotaExceeded
        )
    }
}

/// A ValueError-like error.
pub fn value<T>(msg: impl Into<String>) -> Result<T> {
    Err(Error::Value(msg.into()))
}
