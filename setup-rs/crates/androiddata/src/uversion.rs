//! unity_version.py: a Unity version string, the version a data folder or file names, and the
//! range the converter supports.
use std::fmt;
use std::path::Path;

pub const FIRST: (u32, u32) = (5, 6);
pub const LAST: (u32, u32) = (6000, 6);

/// unity_version.Version: 2021.3.45f2 -> (2021, 3, 45), 'f', 2.
#[derive(Clone, Debug, PartialEq)]
pub struct Version {
    pub text: String,
    pub major: u32,
    pub minor: u32,
    pub patch: u32,
    pub kind: String,
    pub build: u32,
    /// False: only the line is known for sure.
    pub exact: bool,
}

impl Version {
    /// Version(text): ^(\d+)\.(\d+)\.(\d+)([abfpx]?)(\d*); Err (ValueError) otherwise.
    pub fn parse(text: &str) -> Result<Version, String> {
        let b = text.as_bytes();
        let mut i = 0;
        let num = |i: &mut usize| -> Option<u32> {
            let s = *i;
            while *i < b.len() && b[*i].is_ascii_digit() {
                *i += 1;
            }
            if *i == s {
                return None;
            }
            // (Python's int of any length; a version part past u32 is not a real one)
            text[s..*i].parse().ok()
        };
        let err = || format!("not a Unity version: {}", pkgzip::py::str_repr(text));
        let major = num(&mut i).ok_or_else(err)?;
        if b.get(i) != Some(&b'.') {
            return Err(err());
        }
        i += 1;
        let minor = num(&mut i).ok_or_else(err)?;
        if b.get(i) != Some(&b'.') {
            return Err(err());
        }
        i += 1;
        let patch = num(&mut i).ok_or_else(err)?;
        let mut kind = String::new();
        if let Some(&c) = b.get(i) {
            if b"abfpx".contains(&c) {
                kind.push(c as char);
                i += 1;
            }
        }
        let s = i;
        while i < b.len() && b[i].is_ascii_digit() {
            i += 1;
        }
        let build = if i > s {
            text[s..i].parse().unwrap_or(0)
        } else {
            0
        };
        Ok(Version {
            text: text.to_string(),
            major,
            minor,
            patch,
            kind,
            build,
            exact: true,
        })
    }

    pub fn line(&self) -> (u32, u32) {
        (self.major, self.minor)
    }

    pub fn triple(&self) -> (u32, u32, u32) {
        (self.major, self.minor, self.patch)
    }
}

impl fmt::Display for Version {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.text)
    }
}

/// (format, unity version string, offset of the target platform word, big endian) of a
/// SerializedFile's first bytes (format 9 and later).
pub fn sf_header(head: &[u8]) -> Result<(u32, String, usize, bool), String> {
    if head.len() < 40 {
        return Err("short file".into());
    }
    let fmt = u32::from_be_bytes(head[8..12].try_into().unwrap());
    if !(9..=23).contains(&fmt) {
        return Err(format!("unsupported SerializedFile format {fmt}"));
    }
    let p = if fmt >= 22 { 48 } else { 20 };
    let big = head[16] != 0;
    let end = head
        .get(p..)
        .and_then(|r| r.iter().position(|&c| c == 0))
        .map(|e| p + e)
        .ok_or("subsection not found")?;
    let uv = std::str::from_utf8(&head[p..end])
        .ok()
        .filter(|s| s.is_ascii())
        .ok_or("'ascii' codec can't decode the version")?;
    Ok((fmt, uv.to_string(), end + 1, big))
}

/// The Unity version of a serialized file or UnityFS bundle.
pub fn of_file(path: &Path) -> Result<Version, String> {
    use std::io::Read;
    let mut head = Vec::new();
    std::fs::File::open(path)
        .and_then(|f| f.take(256).read_to_end(&mut head))
        .map_err(|e| format!("{e}: {}", path.display()))?;
    if head.starts_with(b"UnityFS\0") {
        let parts: Vec<&[u8]> = head[12..].split(|&c| c == 0).collect();
        let v = parts.get(1).ok_or("list index out of range")?;
        let v = std::str::from_utf8(v).map_err(|e| e.to_string())?;
        return Version::parse(v);
    }
    Version::parse(&sf_header(&head)?.1)
}

/// Unsupported data (unity_version.Unsupported), with its message.
#[derive(Debug)]
pub struct Unsupported(pub String);

/// The Unity version of a game's data folder.
pub fn of_data(data_dir: &str) -> Result<Version, Unsupported> {
    for n in ["globalgamemanagers", "mainData", "data.unity3d"] {
        let p = Path::new(data_dir).join(n);
        if p.is_file() {
            return of_file(&p).map_err(Unsupported);
        }
    }
    Err(Unsupported(format!(
        "{data_dir}: no globalgamemanagers, mainData or data.unity3d (not a Unity data folder?)"
    )))
}

/// A version outside FIRST..LAST is Unsupported (unless force).
pub fn check(v: Version, force: bool) -> Result<Version, Unsupported> {
    if (FIRST <= v.line() && v.line() <= LAST) || force {
        return Ok(v);
    }
    Err(Unsupported(format!(
        "Unity {v}: the converter supports data of Unity {}.{} - {}.{} (their shader program and data formats are known; \
         see docs/compat/README.md). Use --any-unity-version to try anyway (shader layouts are then guessed and checked by \
         round trip).",
        FIRST.0, FIRST.1, LAST.0, LAST.1
    )))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions() {
        let v = Version::parse("2017.4.9f1").unwrap();
        assert_eq!(
            (v.major, v.minor, v.patch, v.kind.as_str(), v.build),
            (2017, 4, 9, "f", 1)
        );
        let v = Version::parse("2021.3.45").unwrap();
        assert_eq!((v.kind.as_str(), v.build), ("", 0));
        assert!(Version::parse("x.1.2").is_err());
        assert!(check(Version::parse("5.5.0f1").unwrap(), false).is_err());
        assert!(check(Version::parse("2017.4.40f1").unwrap(), false).is_ok());
    }
}
