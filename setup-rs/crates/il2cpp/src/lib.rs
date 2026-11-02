//! `pmu il2cpp <data> <out> --on-device ...` and `pmu il2cpp fetch <version> ...`: a Mono desktop
//! game's managed code compiled ahead of time with Unity's IL2CPP for Unity's 64-bit ARM IL2CPP
//! Android player, natively on the aarch64 handheld in a port's setup (convert/pmunity/il2cpp.py,
//! il2cpp_device.py, il2cpp_bee.py, il2cpp_mix.py; docs/il2cpp.md). Unity's IL2CPP and player are
//! read with range requests, Debian's compilers and Microsoft's .NET are fetched pinned by sha256,
//! then UnityLinker, il2cpp and the C++ build run as the Python runs them: the same files, command
//! lines, environment, messages, exit statuses and resume records. The Docker route (a porter's PC)
//! stays with the Python: without `--on-device` the command exits 2.
//!
//! Modules: `fetch` (range reads, NSIS and zip members), `tools` (Unity's editor parts, the NDK,
//! the player; il2cpp.py's toolchain), `mix` (Unity 2023.1+ built with 2022.3's il2cpp), `bee`
//! (Bee's build graph run in place of its native backend), `device` (the handheld's tool folder:
//! Debian packages, .NET, the wrappers), `native` (the steps run on the handheld: the scheduler,
//! resumable steps), `build` (the build itself and its output), `cli` (the command lines).
#![allow(clippy::type_complexity)]

pub mod bee;
pub mod build;
pub mod cli;
pub mod device;
pub mod fetch;
pub mod hash;
pub mod lock;
pub mod mix;
pub mod native;
pub mod tar;
pub mod tools;
pub mod util;

pub use build::{build, BuildOpts};

use std::fmt;

/// How the command stops, as the Python does: SystemExit's message (1), an exception (1), a part
/// that stays with the Python (2).
#[derive(Debug)]
pub enum Error {
    /// `SystemExit(message)` / `sys.exit(message)`: status 1.
    Exit(String),
    /// unity_version.Unsupported (data this route cannot build: the command's status 1 with the
    /// message; `pmunity apk --il2cpp` refuses the build with it).
    Unsupported(String),
    /// An exception (the Python's traceback): status 1, the message.
    Fail(String),
    /// What pmu does not do (the Docker route): status 2.
    NotPorted(String),
}

impl Error {
    pub fn status(&self) -> u8 {
        match self {
            Error::Exit(_) | Error::Unsupported(_) | Error::Fail(_) => 1,
            Error::NotPorted(_) => 2,
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Exit(m) | Error::Unsupported(m) | Error::Fail(m) | Error::NotPorted(m) => {
                f.write_str(m)
            }
        }
    }
}

impl std::error::Error for Error {}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        if e.get_ref().is_some_and(|i| i.is::<Error>()) {
            return *e.into_inner().unwrap().downcast::<Error>().unwrap();
        }
        Error::Fail(e.to_string())
    }
}

impl From<pkgzip::Error> for Error {
    fn from(e: pkgzip::Error) -> Self {
        match e {
            pkgzip::Error::Exit(m) => Error::Exit(m),
            e => Error::Fail(e.to_string()),
        }
    }
}

impl From<androiddata::uversion::Unsupported> for Error {
    fn from(e: androiddata::uversion::Unsupported) -> Self {
        Error::Unsupported(e.0)
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// SystemExit(message)
pub fn exit<T>(m: impl Into<String>) -> Result<T> {
    Err(Error::Exit(m.into()))
}

/// An exception's message
pub fn fail<T>(m: impl Into<String>) -> Result<T> {
    Err(Error::Fail(m.into()))
}

/// il2cpp.log: a line on stderr.
pub fn log(msg: &str) {
    eprintln!("{msg}");
}
