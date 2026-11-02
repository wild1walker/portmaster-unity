//! What the Python's os, os.path and shutil do here: paths as strings joined as posixpath joins
//! them, folders walked in os.walk's order, files written through `.part` and renamed.
use crate::{Error, Result};
use std::fs;
use std::io::Write;
use std::path::Path;

pub use pkgzip::py::{basename, dirname, join, normpath};

/// os.path.join of several parts.
pub fn joins(parts: &[&str]) -> String {
    let mut out = parts[0].to_string();
    for p in &parts[1..] {
        out = join(&out, p);
    }
    out
}

/// os.getcwd()
pub fn cwd() -> String {
    std::env::current_dir()
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_else(|_| "/".into())
}

/// os.path.abspath
pub fn abspath(p: &str) -> String {
    if p.starts_with('/') {
        normpath(p)
    } else {
        normpath(&join(&cwd(), p))
    }
}

/// os.path.isabs
pub fn isabs(p: &str) -> bool {
    p.starts_with('/')
}

/// os.path.relpath(path, start)
pub fn relpath(path: &str, start: &str) -> String {
    let a = abspath(path);
    let b = abspath(start);
    let pa: Vec<&str> = a.split('/').filter(|c| !c.is_empty()).collect();
    let pb: Vec<&str> = b.split('/').filter(|c| !c.is_empty()).collect();
    let mut i = 0;
    while i < pa.len() && i < pb.len() && pa[i] == pb[i] {
        i += 1;
    }
    let mut parts: Vec<&str> = vec![".."; pb.len() - i];
    parts.extend(&pa[i..]);
    if parts.is_empty() {
        ".".into()
    } else {
        parts.join("/")
    }
}

pub fn isfile(p: &str) -> bool {
    Path::new(p).is_file()
}

pub fn isdir(p: &str) -> bool {
    Path::new(p).is_dir()
}

/// os.path.exists (a broken link: false)
pub fn exists(p: &str) -> bool {
    Path::new(p).exists()
}

/// os.path.lexists
pub fn lexists(p: &str) -> bool {
    fs::symlink_metadata(p).is_ok()
}

/// os.path.islink
pub fn islink(p: &str) -> bool {
    fs::symlink_metadata(p).is_ok_and(|m| m.file_type().is_symlink())
}

/// os.path.getsize
pub fn getsize(p: &str) -> Result<u64> {
    Ok(fs::metadata(p).map_err(at(p))?.len())
}

/// An OSError's message as Python gives it ("[Errno 2] No such file or directory: 'x'").
pub fn at(p: &str) -> impl FnOnce(std::io::Error) -> Error + '_ {
    move |e| Error::Fail(oserror(&e, p))
}

pub fn oserror(e: &std::io::Error, p: &str) -> String {
    match e.raw_os_error() {
        Some(n) => format!("[Errno {n}] {}: {}", strerror(n), pkgzip::py::str_repr(p)),
        None => format!("{e}: {}", pkgzip::py::str_repr(p)),
    }
}

/// The C library's message for an errno (as Python's OSError shows it).
pub fn strerror(n: i32) -> String {
    let e = std::io::Error::from_raw_os_error(n).to_string();
    // ("No such file or directory (os error 2)")
    match e.rfind(" (os error ") {
        Some(i) => e[..i].to_string(),
        None => e,
    }
}

/// os.listdir: the names in the directory's own order.
pub fn listdir(p: &str) -> Result<Vec<String>> {
    let mut out = Vec::new();
    for e in fs::read_dir(p).map_err(at(p))? {
        let e = e.map_err(at(p))?;
        out.push(e.file_name().to_string_lossy().into_owned());
    }
    Ok(out)
}

/// One folder of os.walk: its path, its folders and files (the directory's order).
pub struct Walk {
    pub root: String,
    pub dirs: Vec<String>,
    pub files: Vec<String>,
}

/// os.walk(top) (top-down, links to folders listed but not entered). `sorted`: the folders taken
/// in sorted order (`ds.sort()`) and the files sorted, as a caller that sorts both.
pub fn walk(top: &str, sorted: bool) -> Vec<Walk> {
    let mut out = Vec::new();
    walk_into(top, sorted, &mut out);
    out
}

fn walk_into(top: &str, sorted: bool, out: &mut Vec<Walk>) {
    let Ok(rd) = fs::read_dir(top) else { return };
    let (mut dirs, mut files) = (Vec::new(), Vec::new());
    for e in rd.flatten() {
        let n = e.file_name().to_string_lossy().into_owned();
        let p = join(top, &n);
        if Path::new(&p).is_dir() {
            dirs.push(n);
        } else {
            files.push(n);
        }
    }
    if sorted {
        dirs.sort();
        files.sort();
    }
    let sub: Vec<String> = dirs.clone();
    out.push(Walk {
        root: top.to_string(),
        dirs,
        files,
    });
    for d in sub {
        let p = join(top, &d);
        if !islink(&p) {
            walk_into(&p, sorted, out);
        }
    }
}

/// os.makedirs(p, exist_ok=True)
pub fn makedirs(p: &str) -> Result<()> {
    if p.is_empty() {
        return Ok(());
    }
    fs::create_dir_all(p).map_err(at(p))
}

/// shutil.rmtree(p, ignore_errors=True)
pub fn rmtree(p: &str) {
    if islink(p) {
        return;
    }
    let _ = fs::remove_dir_all(p);
}

/// os.unlink, ignoring a missing file where the caller checked first
pub fn unlink(p: &str) -> Result<()> {
    fs::remove_file(p).map_err(at(p))
}

/// os.replace
pub fn replace(a: &str, b: &str) -> Result<()> {
    fs::rename(a, b).map_err(at(a))
}

/// os.chmod
pub fn chmod(p: &str, mode: u32) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(p, fs::Permissions::from_mode(mode)).map_err(at(p))
}

/// os.access(p, os.X_OK)
pub fn executable(p: &str) -> bool {
    let c = match std::ffi::CString::new(p) {
        Ok(c) => c,
        Err(_) => return false,
    };
    // SAFETY: a valid C string
    unsafe { libc::access(c.as_ptr(), libc::X_OK) == 0 }
}

/// il2cpp._write: `data` into p through p.part (folders made), then os.chmod(mode).
pub fn write_file(p: &str, data: &[u8], mode: Option<u32>) -> Result<()> {
    makedirs(dirname(p))?;
    let part = format!("{p}.part");
    fs::write(&part, data).map_err(at(&part))?;
    replace(&part, p)?;
    if let Some(m) = mode {
        chmod(p, m)?;
    }
    Ok(())
}

/// open(p, 'w').write(text)
pub fn write_text(p: &str, text: &str) -> Result<()> {
    fs::write(p, text).map_err(at(p))
}

/// open(p, 'a').write(text)
pub fn append_text(p: &str, text: &str) -> Result<()> {
    let mut f = fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(p)
        .map_err(at(p))?;
    f.write_all(text.as_bytes()).map_err(at(p))
}

pub fn read(p: &str) -> Result<Vec<u8>> {
    fs::read(p).map_err(at(p))
}

/// shutil.copyfile: the contents only.
pub fn copyfile(a: &str, b: &str) -> Result<()> {
    let data = read(a)?;
    fs::write(b, data).map_err(at(b))
}

/// shutil.copy2: the contents, the mode bits and the times.
pub fn copy2(a: &str, b: &str) -> Result<()> {
    fs::copy(a, b).map_err(at(a))?;
    copy_times(a, b)
}

fn copy_times(a: &str, b: &str) -> Result<()> {
    let m = fs::metadata(a).map_err(at(a))?;
    let t = fs::FileTimes::new();
    let t = match (m.accessed(), m.modified()) {
        (Ok(x), Ok(y)) => t.set_accessed(x).set_modified(y),
        (_, Ok(y)) => t.set_modified(y),
        _ => return Ok(()),
    };
    let f = fs::OpenOptions::new().write(true).open(b).map_err(at(b))?;
    let _ = f.set_times(t);
    Ok(())
}

/// shutil.copytree(a, b) (b must not exist): files with copy2.
pub fn copytree(a: &str, b: &str) -> Result<()> {
    fs::create_dir_all(b).map_err(at(b))?;
    for w in walk(a, false) {
        let rel = relpath(&w.root, a);
        let d = if rel == "." {
            b.to_string()
        } else {
            join(b, &rel)
        };
        makedirs(&d)?;
        for f in &w.files {
            let s = join(&w.root, f);
            if islink(&s) {
                let t = fs::read_link(&s).map_err(at(&s))?;
                std::os::unix::fs::symlink(t, join(&d, f)).map_err(at(&s))?;
            } else {
                copy2(&s, &join(&d, f))?;
            }
        }
    }
    Ok(())
}

/// Python's round(x) (half to even) of a float to an int.
pub fn py_round(x: f64) -> i64 {
    let r = x.round();
    if (x - x.trunc()).abs() == 0.5 {
        let t = x.trunc();
        if (t as i64) % 2 == 0 {
            t as i64
        } else {
            r as i64
        }
    } else {
        r as i64
    }
}

/// Python's round(x, 2) (correctly rounded on the exact value).
pub fn py_round2(x: f64) -> f64 {
    format!("{x:.2}").parse().unwrap_or(x)
}

/// time.time()
pub fn now() -> f64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs_f64())
        .unwrap_or(0.0)
}

/// shlex.split (POSIX mode): words, quotes and backslashes as a shell reads them.
pub fn shlex_split(s: &str) -> Result<Vec<String>> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut have = false;
    let mut it = s.chars().peekable();
    while let Some(c) = it.next() {
        match c {
            ' ' | '\t' | '\n' | '\r' => {
                if have {
                    out.push(std::mem::take(&mut cur));
                    have = false;
                }
            }
            '\'' => {
                have = true;
                loop {
                    match it.next() {
                        Some('\'') => break,
                        Some(x) => cur.push(x),
                        None => return Err(Error::Fail("ValueError: No closing quotation".into())),
                    }
                }
            }
            '"' => {
                have = true;
                loop {
                    match it.next() {
                        Some('"') => break,
                        Some('\\') => match it.peek() {
                            Some(&x)
                                if x == '"' || x == '\\' || x == '$' || x == '`' || x == '\n' =>
                            {
                                cur.push(x);
                                it.next();
                            }
                            _ => cur.push('\\'),
                        },
                        Some(x) => cur.push(x),
                        None => return Err(Error::Fail("ValueError: No closing quotation".into())),
                    }
                }
            }
            '\\' => {
                have = true;
                match it.next() {
                    Some(x) => cur.push(x),
                    None => return Err(Error::Fail("ValueError: No escaped character".into())),
                }
            }
            '#' if !have => {
                // (shlex.split: comments=False, '#' is a word character)
                cur.push(c);
                have = true;
            }
            _ => {
                cur.push(c);
                have = true;
            }
        }
    }
    if have {
        out.push(cur);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn relpaths() {
        assert_eq!(relpath("/a/b/c", "/a"), "b/c");
        assert_eq!(relpath("/a", "/a"), ".");
        assert_eq!(relpath("/a/x", "/a/b"), "../x");
    }

    #[test]
    fn rounding() {
        assert_eq!(py_round(2.5), 2);
        assert_eq!(py_round(3.5), 4);
        assert_eq!(py_round(2.4), 2);
        assert_eq!(py_round(-2.5), -2);
        assert_eq!(py_round2(1.005), 1.0);
        assert_eq!(py_round2(1.2345), 1.23);
    }

    #[test]
    fn shlex() {
        assert_eq!(
            shlex_split("'/a b/asmpatch' --only= --with=x").unwrap(),
            vec!["/a b/asmpatch", "--only=", "--with=x"]
        );
        assert_eq!(
            shlex_split(r#"a "b \"c\"" d\ e"#).unwrap(),
            vec!["a", "b \"c\"", "d e"]
        );
        assert!(shlex_split("'x").is_err());
    }
}
