//! port.py's regular expressions, as matchers. `re.match(r'^...$', s)` also matches s with one
//! newline at its end (Python's `$`): `dollar` keeps that.

/// The text a `$`-anchored pattern sees: without one final newline.
pub fn dollar(s: &str) -> &str {
    s.strip_suffix('\n').unwrap_or(s)
}

fn all_of(s: &str, ok: impl Fn(char) -> bool) -> bool {
    !s.is_empty() && s.chars().all(ok)
}

/// NAME_RX `^[a-z0-9][a-z0-9._-]*$`
pub fn name(s: &str) -> bool {
    let s = dollar(s);
    let mut c = s.chars();
    matches!(c.next(), Some(x) if x.is_ascii_lowercase() || x.is_ascii_digit())
        && c.all(|x| x.is_ascii_lowercase() || x.is_ascii_digit() || matches!(x, '.' | '_' | '-'))
}

/// ID_RX `^[A-Z][A-Z0-9_]*$`
pub fn id(s: &str) -> bool {
    let s = dollar(s);
    let mut c = s.chars();
    matches!(c.next(), Some(x) if x.is_ascii_uppercase())
        && c.all(|x| x.is_ascii_uppercase() || x.is_ascii_digit() || x == '_')
}

/// VALUE_RX `^[A-Za-z0-9._:+-]+$`
pub fn value(s: &str) -> bool {
    all_of(dollar(s), |x| x.is_ascii_alphanumeric() || matches!(x, '.' | '_' | ':' | '+' | '-'))
}

/// ENV_RX `^[A-Z_][A-Z0-9_]*$`
pub fn env(s: &str) -> bool {
    let s = dollar(s);
    let mut c = s.chars();
    matches!(c.next(), Some(x) if x.is_ascii_uppercase() || x == '_')
        && c.all(|x| x.is_ascii_uppercase() || x.is_ascii_digit() || x == '_')
}

/// REQ_RX `^!?[a-z0-9_:.]+(\|!?[a-z0-9_:.]+)*$`
pub fn req(s: &str) -> bool {
    dollar(s).split('|').all(|part| {
        let p = part.strip_prefix('!').unwrap_or(part);
        all_of(p, |x| x.is_ascii_lowercase() || x.is_ascii_digit() || matches!(x, '_' | ':' | '.'))
    })
}

/// DATE_RX `^(\d{4})-(\d{2})-(\d{2})$`: the three groups.
pub fn date(s: &str) -> Option<(&str, &str, &str)> {
    let s = dollar(s);
    let b = s.as_bytes();
    if b.len() == 10
        && b[4] == b'-'
        && b[7] == b'-'
        && [0, 1, 2, 3, 5, 6, 8, 9].iter().all(|&i| b[i].is_ascii_digit())
    {
        Some((&s[..4], &s[5..7], &s[8..]))
    } else {
        None
    }
}

/// `^\d{4}\.\d+\.\d+[abfpx]\d+$`: a Unity version.
pub fn unity_version(s: &str) -> bool {
    version_prefix(dollar(s)).is_some_and(|n| n == dollar(s).len())
}

/// The length of the Unity version `\d{4}\.\d+\.\d+[abfpx]\d+` at the start of s.
fn version_prefix(s: &str) -> Option<usize> {
    let b = s.as_bytes();
    let digits = |i: usize| b[i.min(b.len())..].iter().take_while(|c| c.is_ascii_digit()).count();
    if b.len() < 4 || !b[..4].iter().all(u8::is_ascii_digit) || b.get(4) != Some(&b'.') {
        return None;
    }
    let mut i = 5;
    let n = digits(i);
    if n == 0 || b.get(i + n) != Some(&b'.') {
        return None;
    }
    i += n + 1;
    let n = digits(i);
    if n == 0 || !matches!(b.get(i + n), Some(b'a' | b'b' | b'f' | b'p' | b'x')) {
        return None;
    }
    i += n + 1;
    let n = digits(i);
    if n == 0 {
        return None;
    }
    Some(i + n)
}

/// DONOR_RX `^donor-(\d{4}\.\d+\.\d+[abfpx]\d+)-(mono|il2cpp)-(armv7|arm64)\.apk$`: the groups.
pub fn donor(s: &str) -> Option<(&str, &str, &str)> {
    let s = dollar(s);
    let rest = s.strip_prefix("donor-")?;
    let n = version_prefix(rest)?;
    let (ver, rest) = rest.split_at(n);
    let rest = rest.strip_prefix('-')?.strip_suffix(".apk")?;
    let (backend, abi) = rest.split_once('-')?;
    if matches!(backend, "mono" | "il2cpp") && matches!(abi, "armv7" | "arm64") {
        Some((ver, backend, abi))
    } else {
        None
    }
}

/// `^\.[A-Za-z0-9._-]+$`: port.ready_marker.
pub fn ready_marker(s: &str) -> bool {
    dollar(s)
        .strip_prefix('.')
        .is_some_and(|r| all_of(r, |x| x.is_ascii_alphanumeric() || matches!(x, '.' | '_' | '-')))
}

/// `re.fullmatch(r'[\w .+/-]+', b)`: a banks folder.
pub fn banks(s: &str) -> bool {
    all_of(s, |x| x.is_alphanumeric() || matches!(x, '_' | ' ' | '.' | '+' | '/' | '-'))
}

/// `re.fullmatch(r'[\w.-]+\.dll', x)`: an assembly file name.
pub fn dll_name(s: &str) -> bool {
    s.strip_suffix(".dll").is_some_and(|b| all_of(b, |x| x.is_alphanumeric() || matches!(x, '_' | '.' | '-')))
}

/// `^[A-Za-z0-9_-]+$`: a bare TOML key.
pub fn bare_key(s: &str) -> bool {
    all_of(dollar(s), |x| x.is_ascii_alphanumeric() || matches!(x, '_' | '-'))
}

/// `re.match(r'(\d+\.\d+)', s)`: the Unity line at the start.
pub fn line(s: &str) -> Option<&str> {
    let a = s.bytes().take_while(u8::is_ascii_digit).count();
    if a == 0 || s.as_bytes().get(a) != Some(&b'.') {
        return None;
    }
    let b = s[a + 1..].bytes().take_while(u8::is_ascii_digit).count();
    if b == 0 {
        return None;
    }
    Some(&s[..a + 1 + b])
}

/// Python's `\s` (str patterns): the characters str.isspace() takes.
pub fn is_space(c: char) -> bool {
    pkgzip::py::is_space(c)
}

/// The `^\s*` of a MULTILINE pattern from line start i: where the whitespace (newlines too) ends.
fn skip_ws(t: &str, i: usize) -> usize {
    t[i..].char_indices().find(|(_, c)| !is_space(*c)).map_or(t.len(), |(k, _)| i + k)
}

/// The starts of lines (re.M's `^`).
fn line_starts(t: &str) -> impl Iterator<Item = usize> + '_ {
    std::iter::once(0).chain(t.match_indices('\n').map(|(i, _)| i + 1))
}

/// `re.search(r'^\s*(export\s+)?SDL_VIDEODRIVER=', text, re.M)`
pub fn sets_videodriver(t: &str) -> bool {
    line_starts(t).any(|i| {
        let j = skip_ws(t, i);
        if t[j..].starts_with("SDL_VIDEODRIVER=") {
            return true;
        }
        if let Some(r) = t[j..].strip_prefix("export") {
            if r.starts_with(is_space) {
                let k = skip_ws(t, j + 6);
                return t[k..].starts_with("SDL_VIDEODRIVER=");
            }
        }
        false
    })
}

/// `re.search(r'"\$\{?GPTOKEYB2?\}?"', text)`
pub fn quoted_gptokeyb(t: &str) -> bool {
    t.match_indices("\"$").any(|(i, _)| {
        let mut r = &t[i + 2..];
        r = r.strip_prefix('{').unwrap_or(r);
        let Some(mut r) = r.strip_prefix("GPTOKEYB") else {
            return false;
        };
        r = r.strip_prefix('2').unwrap_or(r);
        r = r.strip_prefix('}').unwrap_or(r);
        r.starts_with('"')
    })
}

/// `re.search(r'\*\*|`|\[[^\]]+\]\(|^\s*[-#] ', t, re.M)`: Markdown in a plain text.
pub fn markdown(t: &str) -> bool {
    if t.contains("**") || t.contains('`') {
        return true;
    }
    for (i, _) in t.match_indices('[') {
        if let Some(j) = t[i + 1..].find(']') {
            if j > 0 && t[i + 1 + j + 1..].starts_with('(') {
                return true;
            }
        }
    }
    line_starts(t).any(|i| {
        let j = skip_ws(t, i);
        t[j..].starts_with("- ") || t[j..].starts_with("# ")
    })
}

/// `re.search(r'<image>\./[^/<]+/([^<]+)</image>', gi).group(1)`
pub fn gameinfo_image(gi: &str) -> Option<&str> {
    for (i, _) in gi.match_indices("<image>./") {
        let r = &gi[i + 9..];
        let n = r.find(|c| c == '/' || c == '<').unwrap_or(r.len());
        if n == 0 || !r[n..].starts_with('/') {
            continue;
        }
        let r2 = &r[n + 1..];
        let m = r2.find('<').unwrap_or(r2.len());
        if m > 0 && r2[m..].starts_with("</image>") {
            return Some(&r2[..m]);
        }
    }
    None
}

/// `re.sub(r'\s+', ' ', s.replace('_', ' ')).strip()`
pub fn title_case(s: &str) -> String {
    let mut out = String::new();
    let mut ws = false;
    for c in s.replace('_', " ").chars() {
        if is_space(c) {
            ws = true;
        } else {
            if ws {
                out.push(' ');
            }
            ws = false;
            out.push(c);
        }
    }
    if ws {
        out.push(' ');
    }
    pkgzip::py::strip(&out).to_string()
}

/// shlex.quote(s)
pub fn shquote(s: &str) -> String {
    if s.is_empty() {
        return "''".into();
    }
    if s.chars().all(|c| c.is_ascii_alphanumeric() || "_@%+=:,./-".contains(c)) {
        return s.to_string();
    }
    format!("'{}'", s.replace('\'', "'\"'\"'"))
}

/// str.casefold() (lower case, ß as ss: what genres need).
pub fn casefold(s: &str) -> String {
    s.to_lowercase().replace('ß', "ss")
}

/// str.title(): a capital at the start of each run of letters, the rest lower case.
pub fn title(s: &str) -> String {
    let mut out = String::new();
    let mut prev = false;
    for c in s.chars() {
        if c.is_alphabetic() {
            if prev {
                out.extend(c.to_lowercase());
            } else {
                out.extend(c.to_uppercase());
            }
            prev = true;
        } else {
            out.push(c);
            prev = false;
        }
    }
    out
}

/// os.path.splitext(p)[1]
pub fn splitext_ext(p: &str) -> &str {
    let base = pkgzip::py::basename(p);
    let stripped = base.trim_start_matches('.');
    match stripped.rfind('.') {
        Some(i) => &stripped[i..],
        None => "",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matchers() {
        assert!(name("mygame") && name("my-game.2") && !name("My") && !name("") && name("g\n"));
        assert!(id("TEXTURE_QUALITY") && !id("q") && env("_X1") && !env("ua-x"));
        assert!(value("medium") && !value("a b") && req("!lowres|2gb") && !req("Not Lowres") && !req("a||b"));
        assert_eq!(date("2017-09-29"), Some(("2017", "09", "29")));
        assert!(date("29/09/2017").is_none());
        assert!(unity_version("2019.4.8f1") && !unity_version("x") && !unity_version("2019.4.8"));
        assert_eq!(donor("donor-2019.4.8f1-mono-armv7.apk"), Some(("2019.4.8f1", "mono", "armv7")));
        assert!(donor("donor-2019.4.8f1-mono-x86.apk").is_none());
        assert!(ready_marker(".setup_done64") && !ready_marker("setup"));
        assert_eq!(line("2019.4.8f1"), Some("2019.4"));
        assert!(sets_videodriver("x\n  export  SDL_VIDEODRIVER=kmsdrm\n") && !sets_videodriver("# SDL_VIDEODRIVER=x"));
        assert!(quoted_gptokeyb("\"$GPTOKEYB2\" \"x\"") && quoted_gptokeyb("\"${GPTOKEYB}\"") && !quoted_gptokeyb("$GPTOKEYB2 \"x\""));
        assert!(markdown("a **b**") && markdown("see [x](y)") && markdown("x\n - y") && !markdown("plain [x] (y)"));
        assert_eq!(gameinfo_image("<image>./g/cover.png</image>"), Some("cover.png"));
        assert_eq!(title_case(" my_game  x "), "my game x");
        assert_eq!(shquote("a b'c"), "'a b'\"'\"'c'");
        assert_eq!(shquote("start:escape,x:space"), "start:escape,x:space");
        assert_eq!(title("platformer"), "Platformer");
        assert_eq!(splitext_ext("dir/shot.BMP"), ".BMP");
        assert_eq!(splitext_ext(".png"), "");
    }
}
