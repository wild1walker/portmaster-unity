//! `pmu-dev prefs` (was `python -m pmunity prefs`): a game's PlayerPrefs between a PC and the handheld.
//!
//!   pmu-dev prefs show <file>
//!   pmu-dev prefs copy <from> <to> [--all]
//!
//! Formats (by content; a new <to> by its name):
//!   - Unity's Linux player: `~/.config/unity3d/<Company>/<Product>/prefs`, XML
//!     (`<pref name="..." type="int|float|string">`, strings base64-encoded UTF-8);
//!   - ua_host (the Android player's SharedPreferences): `ports/<port>/data/shared_prefs/
//!     <package>.v2.playerprefs.prefs`, one `<i|f|s>\t<key>\t<value>` line per entry, `%XX` for
//!     tab, newline, carriage return and `%`.
//!
//! `copy` puts <from>'s entries into <to> (made if missing) and writes <to> in its own format; <to>'s
//! other entries stay. The display settings a player keeps per machine (`Screenmanager *`,
//! `UnitySelectMonitor`) are left as <to> has them unless --all. Windows keeps PlayerPrefs in the
//! registry and macOS in a plist: neither is read here (docs/android-data.md, "Saves").
use std::path::Path;

use regex::Regex;

#[derive(Clone, Debug, PartialEq)]
pub enum V {
    I(i64),
    F(f64),
    S(String),
}

type Entry = (String, char, V);

const B64: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

pub fn b64encode(b: &[u8]) -> String {
    let mut out = String::new();
    for c in b.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for k in 0..4 {
            if k <= c.len() {
                out.push(B64[(n >> (18 - 6 * k) & 63) as usize] as char);
            } else {
                out.push('=');
            }
        }
    }
    out
}

pub fn b64decode(s: &str) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    let (mut acc, mut bits) = (0u32, 0);
    for c in s.bytes() {
        if c == b'=' {
            break;
        }
        if c.is_ascii_whitespace() {
            continue;
        }
        let v = B64.iter().position(|&x| x == c).ok_or("Invalid base64-encoded string")? as u32;
        acc = acc << 6 | v;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
            acc &= (1 << bits) - 1;
        }
    }
    Ok(out)
}

fn esc(s: &str) -> String {
    s.chars().map(|c| if "%\t\n\r".contains(c) { format!("%{:02X}", c as u32) } else { c.to_string() }).collect()
}

fn unesc(s: &str) -> String {
    let re = Regex::new(r"%([0-9A-Fa-f]{2})").unwrap();
    re.replace_all(s, |c: &regex::Captures| char::from_u32(u32::from_str_radix(&c[1], 16).unwrap()).unwrap().to_string()).into_owned()
}

fn is_linux(path: &Path) -> Result<bool, String> {
    let b = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let head = &b[..b.len().min(256)];
    let t: &[u8] = &head[head.iter().position(|c| !c.is_ascii_whitespace()).unwrap_or(head.len())..];
    Ok(t.starts_with(b"<"))
}

fn xml_unattr(s: &str) -> String {
    s.replace("&quot;", "\"").replace("&lt;", "<").replace("&gt;", ">").replace("&apos;", "'").replace("&amp;", "&")
}

fn xml_attr(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;")
}

fn parse_num(t: char, v: &str, what: &str) -> Result<V, String> {
    let v = v.trim();
    match t {
        'i' => v.parse().map(V::I).map_err(|_| format!("{what}: invalid literal for int(): {v:?}")),
        _ => v.parse().map(V::F).map_err(|_| format!("{what}: could not convert string to float: {v:?}")),
    }
}

pub fn read(path: &Path) -> Result<Vec<Entry>, String> {
    let p = path.display().to_string();
    let mut out = vec![];
    if is_linux(path)? {
        let text = std::fs::read_to_string(path).map_err(|e| format!("{p}: {e}"))?;
        let root = Regex::new(r"<\s*([A-Za-z_][\w.-]*)").unwrap();
        let tag = text.find("<?xml").map_or(text.as_str(), |i| &text[text[i..].find("?>").map_or(i, |j| i + j + 2)..]);
        let first = root.captures(tag).map(|c| c[1].to_string()).ok_or(format!("{p}: no element found"))?;
        if first != "unity_prefs" {
            return Err(format!("{p}: not a Unity prefs file (<{first}>)"));
        }
        let pref = Regex::new(r#"(?s)<pref\s+([^>]*?)(/>|>(.*?)</pref\s*>)"#).unwrap();
        let attr = Regex::new(r#"([\w:-]+)\s*=\s*("([^"]*)"|'([^']*)')"#).unwrap();
        for m in pref.captures_iter(tag) {
            let mut name = None;
            let mut typ = None;
            for a in attr.captures_iter(&m[1]) {
                let v = xml_unattr(a.get(3).or(a.get(4)).map_or("", |x| x.as_str()));
                match &a[1] {
                    "name" => name = Some(v),
                    "type" => typ = Some(v),
                    _ => {}
                }
            }
            let name = name.unwrap_or_default();
            let t = match typ.as_deref() {
                Some("int") => 'i',
                Some("float") => 'f',
                Some("string") => 's',
                other => return Err(format!("{p}: pref {}: type {}", crate::re::arm::pyrepr(&name), other.map_or("None".into(), crate::re::arm::pyrepr))),
            };
            let text = xml_unattr(m.get(3).map_or("", |x| x.as_str()).trim());
            let v = if t == 's' {
                V::S(String::from_utf8(b64decode(&text)?).map_err(|e| format!("{p}: {e}"))?)
            } else {
                parse_num(t, &text, &p)?
            };
            out.push((name, t, v));
        }
        return Ok(out);
    }
    let text = std::fs::read_to_string(path).map_err(|e| format!("{p}: {e}"))?;
    for line in text.split_inclusive('\n') {
        let line = line.strip_suffix('\n').unwrap_or(line);
        let c: Vec<char> = line.chars().take(2).collect();
        if line.chars().count() < 3 || c[1] != '\t' || !"ifs".contains(c[0]) {
            continue;
        }
        let rest = &line[2..];
        let (key, val) = rest.split_once('\t').unwrap_or((rest, ""));
        let t = c[0];
        let v = if t == 's' { V::S(unesc(val)) } else { parse_num(t, val, &p)? };
        out.push((unesc(key), t, v));
    }
    Ok(out)
}

/// '%.9g'
fn fmt9(v: f64) -> String {
    if v == 0.0 || !v.is_finite() {
        return crate::util::fmt_g(v);
    }
    let e = format!("{:.8e}", v);
    let (m, exp) = e.split_once('e').unwrap();
    let exp: i32 = exp.parse().unwrap();
    if !(-4..9).contains(&exp) {
        let m = m.trim_end_matches('0').trim_end_matches('.');
        return format!("{m}e{}{:02}", if exp < 0 { '-' } else { '+' }, exp.abs());
    }
    let s = format!("{:.*}", (8 - exp).max(0) as usize, v);
    if s.contains('.') {
        s.trim_end_matches('0').trim_end_matches('.').to_string()
    } else {
        s
    }
}

fn val_text(v: &V, linux: bool) -> String {
    match v {
        V::I(i) => i.to_string(),
        V::F(f) => fmt9(*f),
        V::S(s) if linux => b64encode(s.as_bytes()),
        V::S(s) => esc(s),
    }
}

pub fn write(path: &Path, entries: &[Entry], linux: bool) -> Result<(), String> {
    let text = if linux {
        let mut l = vec![r#"<unity_prefs version_major="1" version_minor="1">"#.to_string()];
        for (k, t, v) in entries {
            let n = match t {
                'i' => "int",
                'f' => "float",
                _ => "string",
            };
            l.push(format!("\t<pref name=\"{}\" type=\"{n}\">{}</pref>", xml_attr(k), val_text(v, true)));
        }
        l.push("</unity_prefs>".into());
        l.join("\n") + "\n"
    } else {
        entries.iter().map(|(k, t, v)| format!("{t}\t{}\t{}\n", esc(k), val_text(v, false))).collect()
    };
    let full = if path.is_absolute() { path.to_path_buf() } else { std::env::current_dir().unwrap_or_default().join(path) };
    if let Some(d) = full.parent() {
        std::fs::create_dir_all(d).map_err(|e| format!("{}: {e}", d.display()))?;
    }
    let tmp = format!("{}.tmp", path.display());
    std::fs::write(&tmp, text).map_err(|e| format!("{tmp}: {e}"))?;
    std::fs::rename(&tmp, path).map_err(|e| format!("{}: {e}", path.display()))
}

pub fn copy(src: &Path, dst: &Path, all: bool) -> Result<(usize, usize), String> {
    let per_machine = Regex::new(r"^(Screenmanager .*|UnitySelectMonitor)$").unwrap();
    let new = read(src)?;
    let (old, linux) = if dst.exists() {
        (read(dst)?, is_linux(dst)?)
    } else {
        (vec![], !dst.to_string_lossy().ends_with(".prefs"))
    };
    let take: Vec<Entry> = new.iter().filter(|e| all || !per_machine.is_match(&e.0)).cloned().collect();
    let names: std::collections::HashSet<&String> = take.iter().map(|e| &e.0).collect();
    let mut merged: Vec<Entry> = old.into_iter().filter(|e| !names.contains(&e.0)).collect();
    merged.extend(take.iter().cloned());
    write(dst, &merged, linux)?;
    Ok((take.len(), new.len() - take.len()))
}

/// Python's repr() of a str
fn repr_str(s: &str) -> String {
    let q = if s.contains('\'') && !s.contains('"') { '"' } else { '\'' };
    let mut o = String::from(q);
    for c in s.chars() {
        match c {
            '\\' => o.push_str("\\\\"),
            '\n' => o.push_str("\\n"),
            '\r' => o.push_str("\\r"),
            '\t' => o.push_str("\\t"),
            c if c == q => {
                o.push('\\');
                o.push(c);
            }
            c if (c as u32) < 0x20 || c as u32 == 0x7f => o.push_str(&format!("\\x{:02x}", c as u32)),
            c => o.push(c),
        }
    }
    o.push(q);
    o
}

pub fn run(args: &[String]) -> Result<u8, String> {
    let usage = "usage: pmu-dev prefs show <file> | copy <from> <to> [--all]";
    let all = args.iter().any(|a| a == "--all");
    let pos: Vec<&String> = args.iter().filter(|a| *a != "--all").collect();
    let r = match pos.as_slice() {
        [c, f] if c.as_str() == "show" => read(Path::new(f.as_str())).map(|es| {
            for (k, t, v) in es {
                let tn = match t {
                    'i' => "int",
                    'f' => "float",
                    _ => "string",
                };
                let vr = match v {
                    V::I(i) => i.to_string(),
                    V::F(f) => port::val::float_repr(f),
                    V::S(s) => repr_str(&s),
                };
                println!("{tn:6} {k} = {vr}");
            }
        }),
        [c, a, b] if c.as_str() == "copy" => copy(Path::new(a.as_str()), Path::new(b.as_str()), all).map(|(n, skipped)| {
            let more = if skipped > 0 { format!(" ({skipped} display settings left as they were: --all copies them)") } else { String::new() };
            println!("{n} entries -> {b}{more}");
        }),
        _ => return Err(usage.into()),
    };
    match r {
        Ok(()) => Ok(0),
        Err(e) => {
            eprintln!("prefs: {e}");
            Ok(1)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        assert_eq!(b64encode(b"hello"), "aGVsbG8=");
        assert_eq!(b64decode("aGVsbG8=").unwrap(), b"hello");
        assert_eq!(fmt9(0.1), "0.1");
        assert_eq!(fmt9(1.0 / 3.0), "0.333333333");
        assert_eq!(fmt9(1e10), "1e+10");
        assert_eq!(esc("a%b\tc"), "a%25b%09c");
        assert_eq!(unesc("a%25b%09c"), "a%b\tc");
        let d = std::env::temp_dir().join(format!("pmu-dev-prefs-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&d);
        let lin = d.join("prefs");
        std::fs::write(&lin, "<unity_prefs version_major=\"1\" version_minor=\"1\">\n\t<pref name=\"Screenmanager Width\" type=\"int\">1280</pref>\n\t<pref name=\"vol &amp; fx\" type=\"float\">0.5</pref>\n\t<pref name=\"save\" type=\"string\">aGVsbG8=</pref>\n</unity_prefs>\n").unwrap();
        let hh = d.join("g.v2.playerprefs.prefs");
        assert_eq!(copy(&lin, &hh, false).unwrap(), (2, 1));
        assert_eq!(std::fs::read_to_string(&hh).unwrap(), "f\tvol & fx\t0.5\ns\tsave\thello\n");
        let back = d.join("prefs2");
        copy(&hh, &back, true).unwrap();
        assert_eq!(read(&back).unwrap(), vec![("vol & fx".into(), 'f', V::F(0.5)), ("save".into(), 's', V::S("hello".into()))]);
        assert!(std::fs::read_to_string(&back).unwrap().contains("name=\"vol &amp; fx\""));
        let _ = std::fs::remove_dir_all(&d);
    }
}
