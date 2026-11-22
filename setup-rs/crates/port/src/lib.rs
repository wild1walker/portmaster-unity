//! The port packager (convert/pmunity/port.py's `port new` and `port package`, and the route
//! hooks they call: route.py's box64 part, il2cpp.py's, mono64.py's and apk.py's port_new): a
//! port's folder from port.toml and the route's templates (setup-rs/templates/<route>/route.json), and
//! its PortMaster release zip. Same inputs, same files: port.build.json, port.json, setup.env and
//! the rest byte for byte, the zip's entries, order, modes and times as the Python writes them.
//!
//! Not here: the check of a game's files (`port check`, `port new --data <game>`: crates/check, which
//! hands `new` its report), and extension hook modules named in $PMUNITY_PORT_HOOKS.
use std::path::{Path, PathBuf};

pub mod config;
pub mod new;
pub mod package;
pub mod pyjson;
pub mod routes;
pub mod rx;
pub mod tar;
pub mod text;
pub mod toml;
pub mod val;
pub mod zipw;

pub use pkgzip::{Error, Result};

/// What the Python stops with on purpose (SystemExit(message)): exit 1, the message on stderr.
pub fn exit(msg: impl Into<String>) -> Error {
    Error::Exit(msg.into())
}

/// The portmaster-unity checkout the packager works from (port.py's REPO): its templates
/// (setup-rs/templates), the converter it ships for ports pmu does not cover
/// (convert/pmunity), LICENSE and THIRD_PARTY.md, tools/ and runtime/.
#[derive(Clone, Debug)]
pub struct Repo {
    pub root: PathBuf,
}

impl Repo {
    pub fn new(root: impl Into<PathBuf>) -> Repo {
        Repo { root: root.into() }
    }

    /// The checkout: $PMUNITY_REPO, else the first folder above `start` (the program's own place,
    /// then the working folder) that has setup-rs/templates.
    pub fn find(starts: &[PathBuf]) -> Option<Repo> {
        if let Some(r) = std::env::var_os("PMUNITY_REPO").filter(|r| !r.is_empty()) {
            return Some(Repo::new(abspath(Path::new(&r))));
        }
        for s in starts {
            let mut d = Some(abspath(s));
            while let Some(x) = d {
                if x.join("setup-rs/templates").is_dir() {
                    return Some(Repo::new(x));
                }
                d = x.parent().map(Path::to_path_buf);
            }
        }
        None
    }

    pub fn pmunity(&self) -> PathBuf {
        self.root.join("convert").join("pmunity")
    }

    pub fn templates(&self) -> PathBuf {
        self.root.join("setup-rs").join("templates")
    }
}

/// os.path.abspath: joined to the working folder, normalized (lexically).
pub fn abspath(p: &Path) -> PathBuf {
    let s = p.to_string_lossy();
    let joined = if s.starts_with('/') {
        s.into_owned()
    } else {
        let cwd = std::env::current_dir().map(|c| c.to_string_lossy().into_owned()).unwrap_or_else(|_| "/".into());
        pkgzip::py::join(&cwd, &s)
    };
    PathBuf::from(pkgzip::py::normpath(&joined))
}

/// os.path.relpath(path, start)
pub fn relpath(path: &Path, start: &Path) -> String {
    let a = abspath(path);
    let b = abspath(start);
    let a: Vec<String> = a.to_string_lossy().split('/').filter(|x| !x.is_empty()).map(String::from).collect();
    let b: Vec<String> = b.to_string_lossy().split('/').filter(|x| !x.is_empty()).map(String::from).collect();
    let n = a.iter().zip(&b).take_while(|(x, y)| x == y).count();
    let mut parts: Vec<String> = vec!["..".to_string(); b.len() - n];
    parts.extend(a[n..].iter().cloned());
    if parts.is_empty() {
        ".".into()
    } else {
        parts.join("/")
    }
}

/// macOS's own files, never a port's: .DS_Store and AppleDouble `._<name>` files.
pub fn mac_junk(name: &str) -> bool {
    name == ".DS_Store" || name.starts_with("._")
}

/// A path as a string (the packager's paths are the Python's strings).
pub fn ps(p: &Path) -> String {
    p.to_string_lossy().into_owned()
}
