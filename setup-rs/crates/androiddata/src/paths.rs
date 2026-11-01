//! os.path and shlex as the Python uses them: abspath, relpath, realpath, a sorted os.walk,
//! shlex.split (POSIX mode), fnmatch.
use pkgzip::py;
use std::path::Path;

/// os.getcwd() (the PWD the process started in when it names the same folder, as Python's
/// getcwd gives the resolved path: both resolve symlinks the same way here).
pub fn getcwd() -> String {
    std::env::current_dir()
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_else(|_| "/".into())
}

/// A new empty folder in the temporary folder (tempfile.mkdtemp: $TMPDIR, else /tmp).
pub fn temp_dir() -> std::io::Result<String> {
    let base = std::env::var("TMPDIR")
        .ok()
        .filter(|d| Path::new(d).is_dir())
        .unwrap_or_else(|| "/tmp".into());
    let seed = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0)
        ^ (std::process::id() as u128) << 64;
    let mut x = seed as u64 ^ (seed >> 64) as u64;
    for _ in 0..100 {
        x = x
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        let p = py::join(&base, &format!("tmp{:08x}", x >> 32));
        match std::fs::create_dir(&p) {
            Ok(()) => return Ok(p),
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(e),
        }
    }
    Err(std::io::Error::other("no temporary folder name free"))
}

/// path + suffix (a sibling file named after it: "<path>.tmp").
pub fn with_suffix(p: &Path, suffix: &str) -> std::path::PathBuf {
    let mut s = p.as_os_str().to_owned();
    s.push(suffix);
    s.into()
}

/// os.path.abspath: joined with the working folder, normalized (no symlinks resolved).
pub fn abspath(p: &str) -> String {
    if p.starts_with('/') {
        py::normpath(p)
    } else {
        py::normpath(&py::join(&getcwd(), p))
    }
}

/// os.path.relpath(path, start).
pub fn relpath(p: &str, start: &str) -> String {
    let a = abspath(p);
    let b = abspath(start);
    let pa: Vec<&str> = a.split('/').filter(|c| !c.is_empty()).collect();
    let pb: Vec<&str> = b.split('/').filter(|c| !c.is_empty()).collect();
    let common = pa.iter().zip(&pb).take_while(|(x, y)| x == y).count();
    let mut parts: Vec<&str> = vec![".."; pb.len() - common];
    parts.extend(&pa[common..]);
    if parts.is_empty() {
        ".".into()
    } else {
        parts.join("/")
    }
}

/// os.path.realpath: symlinks resolved as far as the path exists, the rest kept.
pub fn realpath(p: &str) -> String {
    let a = abspath(p);
    if let Ok(c) = std::fs::canonicalize(&a) {
        return c.to_string_lossy().into_owned();
    }
    // the longest existing prefix resolved, the rest appended
    let parts: Vec<&str> = a.split('/').filter(|c| !c.is_empty()).collect();
    for k in (0..parts.len()).rev() {
        let head = format!("/{}", parts[..k].join("/"));
        if let Ok(c) = std::fs::canonicalize(&head) {
            let mut s = c.to_string_lossy().into_owned();
            for x in &parts[k..] {
                s = py::join(&s, x);
            }
            return py::normpath(&s);
        }
    }
    a
}

/// sorted(os.listdir(d)) (names that are not UTF-8: lossily).
pub fn listdir(d: &str) -> std::io::Result<Vec<String>> {
    let mut v: Vec<String> = std::fs::read_dir(d)?
        .map(|e| e.map(|e| e.file_name().to_string_lossy().into_owned()))
        .collect::<std::io::Result<_>>()?;
    v.sort();
    Ok(v)
}

/// os.walk(top) top-down: (dir, subfolders, files), both in the folder's listing order (readdir's,
/// as os.scandir gives it), the subfolders sorted first when `sort_dirs` (the Python's ds.sort(),
/// which also orders the descent). Symlinked folders are listed but not entered; an unreadable
/// folder is skipped.
pub fn walk(top: &str, sort_dirs: bool) -> Vec<(String, Vec<String>, Vec<String>)> {
    let mut out = Vec::new();
    let mut stack = vec![top.to_string()];
    while let Some(r) = stack.pop() {
        let Ok(rd) = std::fs::read_dir(&r) else {
            continue;
        };
        let (mut ds, mut fs) = (Vec::new(), Vec::new());
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if Path::new(&r).join(&n).is_dir() {
                ds.push(n);
            } else {
                fs.push(n);
            }
        }
        if sort_dirs {
            ds.sort();
        }
        for d in ds.iter().rev() {
            let p = py::join(&r, d);
            if !Path::new(&p).is_symlink() {
                stack.push(p);
            }
        }
        out.push((r, ds, fs));
    }
    out
}

/// shlex.split(s) (POSIX mode, no comments): Err for an unclosed quote or a trailing escape.
pub fn shlex_split(s: &str) -> Result<Vec<String>, String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut have = false;
    let mut it = s.chars().peekable();
    while let Some(c) = it.next() {
        match c {
            ' ' | '\t' | '\r' | '\n' => {
                if have {
                    out.push(std::mem::take(&mut cur));
                    have = false;
                }
            }
            '\\' => match it.next() {
                Some(n) => {
                    cur.push(n);
                    have = true;
                }
                None => return Err("No escaped character".into()),
            },
            '\'' => {
                have = true;
                loop {
                    match it.next() {
                        Some('\'') => break,
                        Some(n) => cur.push(n),
                        None => return Err("No closing quotation".into()),
                    }
                }
            }
            '"' => {
                have = true;
                loop {
                    match it.next() {
                        Some('"') => break,
                        Some('\\') => match it.next() {
                            Some(n @ ('"' | '\\')) => cur.push(n),
                            Some(n) => {
                                cur.push('\\');
                                cur.push(n);
                            }
                            None => return Err("No closing quotation".into()),
                        },
                        Some(n) => cur.push(n),
                        None => return Err("No closing quotation".into()),
                    }
                }
            }
            c => {
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

/// fnmatch.fnmatch on POSIX (case-sensitive): * ? [seq] [!seq].
pub fn fnmatch(name: &str, pat: &str) -> bool {
    let n: Vec<char> = name.chars().collect();
    let p: Vec<char> = pat.chars().collect();
    fn class(p: &[char], i: usize, c: char) -> Option<(bool, usize)> {
        // p[i] == '['; returns (matched, index after ']') or None when there is no ']'
        let mut j = i + 1;
        let neg = j < p.len() && p[j] == '!';
        if neg {
            j += 1;
        }
        let start = j;
        if j < p.len() && p[j] == ']' {
            j += 1;
        }
        while j < p.len() && p[j] != ']' {
            j += 1;
        }
        if j >= p.len() {
            return None;
        }
        let set = &p[start..j];
        let mut hit = false;
        let mut k = 0;
        while k < set.len() {
            if k + 2 < set.len() && set[k + 1] == '-' {
                if set[k] <= c && c <= set[k + 2] {
                    hit = true;
                }
                k += 3;
            } else {
                if set[k] == c {
                    hit = true;
                }
                k += 1;
            }
        }
        Some((hit != neg, j + 1))
    }
    fn m(n: &[char], p: &[char]) -> bool {
        if p.is_empty() {
            return n.is_empty();
        }
        match p[0] {
            '*' => (0..=n.len()).any(|k| m(&n[k..], &p[1..])),
            '?' => !n.is_empty() && m(&n[1..], &p[1..]),
            '[' => match class(p, 0, n.first().copied().unwrap_or('\0')) {
                Some((hit, after)) => !n.is_empty() && hit && m(&n[1..], &p[after..]),
                None => !n.is_empty() && n[0] == '[' && m(&n[1..], &p[1..]),
            },
            c => !n.is_empty() && n[0] == c && m(&n[1..], &p[1..]),
        }
    }
    m(&n, &p)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shlex_as_python() {
        assert_eq!(
            shlex_split("env A=1 '/x y/codepatch' --target=android  {managed} {out}").unwrap(),
            [
                "env",
                "A=1",
                "/x y/codepatch",
                "--target=android",
                "{managed}",
                "{out}"
            ]
        );
        assert_eq!(
            shlex_split(r#"a "b\$c\"d" e\ f ''"#).unwrap(),
            ["a", "b\\$c\"d", "e f", ""]
        );
        assert!(shlex_split("a 'b").is_err());
    }

    #[test]
    fn fnmatch_as_python() {
        assert!(fnmatch("goggame-1.info", "goggame-*.info"));
        assert!(!fnmatch("goggame-1.hashdb", "goggame-*.info"));
        assert!(fnmatch("x.bank", "*.bank"));
        assert!(fnmatch("a/b.bank", "*.bank"));
        assert!(fnmatch("ab", "[a-c]b"));
        assert!(fnmatch("db", "[!a-c]b"));
        assert!(fnmatch("[x", "[x"));
    }

    #[test]
    fn relpaths() {
        assert_eq!(relpath("/a/b/c", "/a"), "b/c");
        assert_eq!(relpath("/a/x", "/a/b"), "../x");
        assert_eq!(relpath("/a", "/a"), ".");
    }
}
