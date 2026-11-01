//! What the Python's str and bytes methods do, where Rust's differ: whitespace (`str.isspace`),
//! `strip`, `splitlines` (more line boundaries than '\n'), `repr` of a str, slicing.

/// `str.isspace()` of one character: Unicode White_Space plus the separators \x1c - \x1f.
pub fn isspace(c: char) -> bool {
    c.is_whitespace() || ('\x1c'..='\x1f').contains(&c)
}

/// `str.strip()`.
pub fn strip(s: &str) -> &str {
    s.trim_matches(isspace)
}

/// `str.rstrip()`.
pub fn rstrip(s: &str) -> &str {
    s.trim_end_matches(isspace)
}

/// `bytes.lstrip()`: ASCII whitespace (space, \t, \n, \r, \x0b, \x0c).
pub fn bytes_lstrip(b: &[u8]) -> &[u8] {
    let n = b
        .iter()
        .take_while(|c| matches!(c, b' ' | b'\t' | b'\n' | b'\r' | b'\x0b' | b'\x0c'))
        .count();
    &b[n..]
}

/// The end of each line of `str.splitlines(keepends=True)` (byte offsets; the last is s.len() when
/// the text does not end with a line boundary). Boundaries: \n, \r, \r\n, \x0b, \x0c, \x1c - \x1e,
/// \x85,  ,  .
pub fn line_ends(s: &str) -> Vec<usize> {
    let mut out = Vec::new();
    let mut it = s.char_indices().peekable();
    while let Some((i, c)) = it.next() {
        match c {
            '\r' => {
                if let Some(&(_, '\n')) = it.peek() {
                    it.next();
                    out.push(i + 2);
                } else {
                    out.push(i + 1);
                }
            }
            '\n' | '\x0b' | '\x0c' | '\x1c' | '\x1d' | '\x1e' | '\u{85}' | '\u{2028}'
            | '\u{2029}' => out.push(i + c.len_utf8()),
            _ => {}
        }
    }
    if out.last().copied().unwrap_or(0) < s.len() {
        out.push(s.len());
    }
    out
}

/// `str.splitlines(keepends=True)`.
pub fn splitlines(s: &str) -> Vec<&str> {
    let mut start = 0;
    line_ends(s)
        .into_iter()
        .map(|e| {
            let l = &s[start..e];
            start = e;
            l
        })
        .collect()
}

/// `repr()` of a str: quoted as Python quotes it, with its escapes.
pub fn repr(s: &str) -> String {
    let q = if s.contains('\'') && !s.contains('"') {
        '"'
    } else {
        '\''
    };
    let mut out = String::with_capacity(s.len() + 2);
    out.push(q);
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c == q => {
                out.push('\\');
                out.push(c);
            }
            c if (c as u32) < 0x20 || c as u32 == 0x7f => {
                out.push_str(&format!("\\x{:02x}", c as u32))
            }
            c if (0x80..=0xa0).contains(&(c as u32)) || c == '\u{ad}' => {
                out.push_str(&format!("\\x{:02x}", c as u32))
            }
            c => out.push(c),
        }
    }
    out.push(q);
    out
}

/// `data[start:stop]` with Python's clamping and negative indices.
pub fn slice(data: &[u8], start: i64, stop: i64) -> &[u8] {
    let n = data.len() as i64;
    let fix = |i: i64| if i < 0 { (i + n).max(0) } else { i.min(n) };
    let (a, b) = (fix(start), fix(stop));
    if a >= b {
        &data[0..0]
    } else {
        &data[a as usize..b as usize]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn splitlines_like_python() {
        assert_eq!(
            splitlines("a\nb\r\nc\rd\x0be\x1ff"),
            vec!["a\n", "b\r\n", "c\r", "d\x0b", "e\x1ff"]
        );
        assert_eq!(splitlines("a\n"), vec!["a\n"]);
        assert!(splitlines("").is_empty());
    }

    #[test]
    fn strip_and_repr() {
        assert_eq!(strip("\x1c  x \u{3000}\n"), "x");
        assert_eq!(repr("gl_FragColor"), "'gl_FragColor'");
        assert_eq!(repr("it's"), "\"it's\"");
        assert_eq!(repr("a'\"\\\x01"), "'a\\'\"\\\\\\x01'");
    }

    #[test]
    fn slices_like_python() {
        let d = b"abcdef";
        assert_eq!(slice(d, 1, 3), b"bc");
        assert_eq!(slice(d, -2, 100), b"ef");
        assert_eq!(slice(d, 4, 2), b"");
    }
}
