//! StreamingAssets on Unity's Android player (streaming.py): bundle CRCs and Addressables
//! catalogs that follow the patched bundles (their CRCs and file sizes), the folder beside the APK (--streaming-assets
//! both|loose), the data folder's own files (--data-path), and a bundle saved again whole when
//! its platform cannot be patched in place.
use crate::pyjson::{self, Json};
use regex::bytes::Regex;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;
use std::sync::{Arc, OnceLock};

pub const FOLDER: &str = "Game_Data";
pub const SA_DIR: &str = "Game_Data/StreamingAssets";
/// Addressables' placeholder for the build's own aa folder in a catalog's internal ids.
pub const RUNTIME_PATH: &str = "{UnityEngine.AddressableAssets.Addressables.RuntimePath}";

/// asmpatch's opt-in patch for a mode.
pub fn patch_of(mode: &str) -> Option<&'static str> {
    match mode {
        "both" => Some("streaming-assets"),
        "loose" => Some("streaming-assets-all"),
        _ => None,
    }
}

/// Unity's CRC of an asset bundle: CRC-32 of its uncompressed data, block by block.
pub fn bundle_crc(path: &Path) -> unityfs::Result<u32> {
    let b = unityfs::platform::MiniBundle::open(path)?;
    let mut f = std::fs::File::open(path)?;
    let mut h = crc32fast::Hasher::new();
    for i in 0..b.blocks.len() {
        let [usz, csz, bfl] = b.blocks[i];
        if bfl & 0x3F != 0 || csz != usz {
            h.update(&b.read_block(&mut f, i)?.1);
            continue;
        }
        // a stored block, in pieces: a stored bundle is one block of all its data
        f.seek(SeekFrom::Start(b.block_pos[i].0))?;
        let mut buf = vec![0u8; (1usize << 20).min(usz as usize)];
        let mut left = usz as usize;
        while left > 0 {
            let k = left.min(buf.len());
            f.read_exact(&mut buf[..k])?;
            h.update(&buf[..k]);
            left -= k;
        }
    }
    Ok(h.finalize())
}

/// An Addressables content catalog in StreamingAssets (aa/catalog*.json or .bin).
pub fn is_catalog(rel: &str) -> bool {
    static R: OnceLock<regex::Regex> = OnceLock::new();
    R.get_or_init(|| regex::Regex::new(r"^aa[/\\]catalog[^/\\]*\.(json|bin)$").unwrap())
        .is_match(rel)
}

fn win_ids(binary: bool) -> &'static Regex {
    static B: OnceLock<Regex> = OnceLock::new();
    static J: OnceLock<Regex> = OnceLock::new();
    const RT: &str = r"\{UnityEngine\.AddressableAssets\.Addressables\.RuntimePath\}";
    if binary {
        B.get_or_init(|| {
            Regex::new(&format!(r#"(?-u){RT}(?:\\[^\\\x00-\x1f"]+?)+?\.bundle"#)).unwrap()
        })
    } else {
        J.get_or_init(|| Regex::new(&format!(r#"(?-u){RT}(?:\\\\[^\\"]+?)+?\.bundle"#)).unwrap())
    }
}

/// (data, n): the catalog with the backslashes of its {RuntimePath} ids made '/'.
pub fn catalog_fix_paths(data: &[u8], binary: bool) -> (Vec<u8>, usize) {
    let sep: &[u8] = if binary { b"\\" } else { b"\\\\" };
    let mut n = 0;
    let out = win_ids(binary).replace_all(data, |m: &regex::bytes::Captures| {
        n += 1;
        replace_bytes(&m[0], sep, b"/")
    });
    (out.into_owned(), n)
}

fn replace_bytes(hay: &[u8], old: &[u8], new: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(hay.len());
    let mut i = 0;
    while i < hay.len() {
        if hay[i..].starts_with(old) && !old.is_empty() {
            out.extend_from_slice(new);
            i += old.len();
        } else {
            out.push(hay[i]);
            i += 1;
        }
    }
    out
}

/// bytes.count (non-overlapping).
fn count(hay: &[u8], needle: &[u8]) -> usize {
    let mut n = 0;
    let mut i = 0;
    while i + needle.len() <= hay.len() {
        if &hay[i..i + needle.len()] == needle {
            n += 1;
            i += needle.len();
        } else {
            i += 1;
        }
    }
    n
}

/// Whether a catalog is rewritten after the bundles (bundle CRCs or Windows paths).
pub fn catalog_needs_fix(path: &str) -> bool {
    let Ok(data) = std::fs::read(path) else {
        return false;
    };
    let binary = path.ends_with(".bin");
    let (crcs, sizes) = catalog_has(&data);
    binary || catalog_fix_paths(&data, binary).1 > 0 || crcs || sizes
}

/// Whether a catalog holds bundle CRCs to follow (then the bundles' CRCs are worth computing).
pub fn catalog_wants_crcs(path: &str) -> bool {
    let Ok(data) = std::fs::read(path) else {
        return false;
    };
    path.ends_with(".bin") || catalog_has(&data).0
}

/// (data, n): a binary catalog with each bundle CRC (old -> new) replaced where its 4 bytes occur
/// exactly once.
pub fn catalog_bin_crcs(data: &[u8], crcs: &[(u32, u32)]) -> (Vec<u8>, usize) {
    let mut out = data.to_vec();
    let mut n = 0;
    for &(old, new) in crcs {
        let b = old.to_le_bytes();
        if old != new && count(data, &b) == 1 {
            let i = pkgzip::py::find(data, &b).unwrap();
            out[i..i + 4].copy_from_slice(&new.to_le_bytes());
            n += 1;
        }
    }
    (out, n)
}

/// (data, CRCs replaced, bundle sizes replaced, paths made '/'): a catalog as the Android
/// player's Addressables reads it, describing the bundles as they are placed. size_of gives the
/// placed file's size for a bundle's internal id (None: not a file of this build, left alone).
pub fn fix_catalog(
    path: &str,
    data: &[u8],
    crcs: &[(u32, u32)],
    size_of: &dyn Fn(&str) -> Option<u64>,
) -> Result<(Vec<u8>, usize, usize, usize), String> {
    let binary = path.ends_with(".bin");
    let (data, n_paths) = catalog_fix_paths(data, binary);
    let (data, n, n_sizes) = if binary {
        let (d, n) = catalog_bin_crcs(&data, crcs);
        (d, n, 0)
    } else {
        catalog_follow(&data, crcs, size_of)?
    };
    Ok((data, n, n_sizes, n_paths))
}

// base64 as binascii (b64decode without validation: other characters skipped)
const B64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

pub fn b64encode(b: &[u8]) -> String {
    let mut s = String::with_capacity(b.len().div_ceil(3) * 4);
    for c in b.chunks(3) {
        let v = (c[0] as u32) << 16
            | (*c.get(1).unwrap_or(&0) as u32) << 8
            | *c.get(2).unwrap_or(&0) as u32;
        for k in 0..4 {
            if k <= c.len() {
                s.push(B64[(v >> (18 - 6 * k) & 63) as usize] as char);
            } else {
                s.push('=');
            }
        }
    }
    s
}

pub fn b64decode(s: &str) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    let (mut acc, mut bits, mut quad, mut pads) = (0u32, 0, 0, 0);
    for c in s.bytes() {
        if c == b'=' {
            if quad >= 2 {
                pads += 1;
                if quad + pads >= 4 {
                    // (a complete group: what follows starts anew)
                    acc = 0;
                    bits = 0;
                    quad = 0;
                    pads = 0;
                }
            }
            continue;
        }
        let Some(v) = B64.iter().position(|&x| x == c) else {
            continue;
        };
        if pads > 0 {
            // binascii: data after padding ends the group early
            return Err("Excess data after padding".into());
        }
        acc = acc << 6 | v as u32;
        bits += 6;
        quad = (quad + 1) % 4;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
            acc &= (1 << bits) - 1;
        }
    }
    if quad != 0 && quad + pads < 4 {
        return Err(if quad == 1 {
            "Invalid base64-encoded string: number of data characters cannot be 1 more than a multiple of 4".into()
        } else {
            "Incorrect padding".into()
        });
    }
    Ok(out)
}

fn le_i32(b: &[u8], o: i64) -> Result<i32, String> {
    if o < 0 {
        return Err("offset must be non-negative".into());
    }
    b.get(o as usize..o as usize + 4)
        .map(|x| i32::from_le_bytes(x.try_into().unwrap()))
        .ok_or_else(|| "unpack_from requires a buffer of at least 4 bytes".into())
}

/// The internal id an entry names (m_InternalIds, with "<n>#" for m_InternalIdPrefixes[n]).
fn internal_id(c: &Json, index: i32) -> Option<String> {
    let id = c.get("m_InternalIds")?.as_list()?.get(usize::try_from(index).ok()?)?.as_str()?;
    let prefix = id.split_once('#').and_then(|(n, rest)| {
        let n: usize = n.parse().ok()?;
        let pre = c.get("m_InternalIdPrefixes")?.as_list()?.get(n)?.as_str()?;
        Some(format!("{pre}{rest}"))
    });
    Some(prefix.unwrap_or_else(|| id.to_string()))
}

/// The internal ids of the entries whose data is the record at each offset.
fn record_ids(c: &Json, entries: &[u8]) -> Result<std::collections::BTreeMap<i64, Vec<String>>, String> {
    let n = le_i32(entries, 0)?;
    let mut ids: std::collections::BTreeMap<i64, Vec<String>> = Default::default();
    for k in 0..n.max(0) as i64 {
        let off = le_i32(entries, 4 + 28 * k + 16)? as i64;
        if off < 0 {
            continue;
        }
        if let Some(id) = internal_id(c, le_i32(entries, 4 + 28 * k)?) {
            let v = ids.entry(off).or_default();
            if !v.contains(&id) {
                v.push(id);
            }
        }
    }
    Ok(ids)
}

/// A catalog's JsonObject records its entries point at, highest offset first:
/// (entries, extra, [(record offset, json offset, json size, json text)]).
#[allow(clippy::type_complexity)]
fn records(c: &Json) -> Result<(Vec<u8>, Vec<u8>, Vec<(i64, i64, i32, String)>), String> {
    let get = |k: &str| {
        c.get(k)
            .and_then(Json::as_str)
            .ok_or_else(|| format!("KeyError: '{k}'"))
    };
    let extra = b64decode(get("m_ExtraDataString")?)?;
    let entries = b64decode(get("m_EntryDataString")?)?;
    let n = le_i32(&entries, 0)?;
    let mut offs = std::collections::BTreeSet::new();
    for k in 0..n.max(0) as i64 {
        offs.insert(le_i32(&entries, 4 + 28 * k + 16)? as i64);
    }
    offs.remove(&-1);
    let mut out = Vec::new();
    let at = |o: i64| -> Result<u8, String> {
        let i = if o < 0 { o + extra.len() as i64 } else { o };
        extra
            .get(i as usize)
            .copied()
            .filter(|_| i >= 0)
            .ok_or_else(|| "index out of range".to_string())
    };
    for &off in offs.iter().rev() {
        if off >= extra.len() as i64 || at(off)? != 7 {
            continue;
        }
        let mut p = off + 1;
        p += 1 + at(p)? as i64;
        p += 1 + at(p)? as i64;
        let size = le_i32(&extra, p)?;
        let s = (p + 4).max(0) as usize;
        let e = ((p + 4 + size as i64).max(0) as usize)
            .min(extra.len())
            .max(s.min(extra.len()));
        let js = extra.get(s.min(extra.len())..e).unwrap_or(&[]);
        if js.len() % 2 != 0 {
            return Err("'utf-16-le' codec can't decode: truncated data".into());
        }
        let units: Vec<u16> = js
            .chunks(2)
            .map(|c| u16::from_le_bytes([c[0], c[1]]))
            .collect();
        let text = String::from_utf16(&units).map_err(|e| e.to_string())?;
        out.push((off, p, size, text));
    }
    Ok((entries, extra, out))
}

fn crc_re() -> &'static regex::Regex {
    static R: OnceLock<regex::Regex> = OnceLock::new();
    R.get_or_init(|| regex::Regex::new(r#""m_Crc":(\d+)"#).unwrap())
}

fn size_re() -> &'static regex::Regex {
    static R: OnceLock<regex::Regex> = OnceLock::new();
    R.get_or_init(|| regex::Regex::new(r#""m_BundleSize":(\d+)"#).unwrap())
}

/// (a record has a non-zero bundle CRC, a record has a bundle size) of a catalog.json.
fn catalog_has(data: &[u8]) -> (bool, bool) {
    let Ok(c) = pyjson::loads_bytes(data) else {
        return (false, false);
    };
    let Ok((_, _, recs)) = records(&c) else {
        return (false, false);
    };
    let crcs = recs.iter().any(|(_, _, _, js)| {
        crc_re()
            .captures_iter(js)
            .any(|m| !matches!(m[1].parse::<u128>(), Ok(0)))
    });
    (crcs, recs.iter().any(|(_, _, _, js)| size_re().is_match(js)))
}

/// A catalog.json with every bundle CRC in crcs (old -> new) replaced, and every bundle size
/// (m_BundleSize) made the size of the bundle's placed file, where size_of knows the file of the
/// record's internal id: (bytes, CRCs replaced, sizes replaced).
pub fn catalog_follow(
    text: &[u8],
    crcs: &[(u32, u32)],
    size_of: &dyn Fn(&str) -> Option<u64>,
) -> Result<(Vec<u8>, usize, usize), String> {
    let c = pyjson::loads_bytes(text)?;
    let (mut entries, mut extra, recs) = records(&c)?;
    let ids = record_ids(&c, &entries)?;
    let n = le_i32(&entries, 0)?;
    let lookup = |v: u128| {
        crcs.iter()
            .find(|(o, _)| *o as u128 == v)
            .map(|(_, n)| *n as u128)
    };
    let (mut done, mut sized) = (0, 0);
    for (off, p, size, js) in recs {
        let new = crc_re().replace_all(&js, |m: &regex::Captures| {
            let v: u128 = m[1].parse().unwrap_or(0);
            format!("\"m_Crc\":{}", lookup(v).unwrap_or(v))
        });
        // the file's size: one all the record's ids agree on (a record is one bundle's)
        let mut sizes = ids.get(&off).into_iter().flatten().map(|id| size_of(id));
        let file = match sizes.next() {
            Some(first) if sizes.all(|s| s == first) => first,
            _ => None,
        };
        let new = match file {
            Some(file) => size_re()
                .replace_all(&new, |m: &regex::Captures| {
                    if m[1].parse::<u64>().ok() != Some(file) {
                        sized += 1;
                    }
                    format!("\"m_BundleSize\":{file}")
                })
                .into_owned(),
            None => new.into_owned(),
        };
        if new == js {
            continue;
        }
        done += crc_re()
            .captures_iter(&js)
            .filter(|m| m[1].parse::<u128>().ok().and_then(lookup).is_some())
            .count();
        let nb: Vec<u8> = new.encode_utf16().flat_map(u16::to_le_bytes).collect();
        let delta = nb.len() as i64 - size as i64;
        let (p, end) = (p as usize, (p + 4 + size as i64) as usize);
        let mut rep = (nb.len() as i32).to_le_bytes().to_vec();
        rep.extend(&nb);
        drop(extra.splice(p..end.min(extra.len()), rep));
        if delta != 0 {
            for k in 0..n.max(0) as usize {
                let q = 4 + 28 * k + 16;
                let d = le_i32(&entries, q as i64)? as i64;
                if d > off {
                    entries[q..q + 4].copy_from_slice(&((d + delta) as i32).to_le_bytes());
                }
            }
        }
    }
    if done == 0 && sized == 0 {
        return Ok((text.to_vec(), 0, 0));
    }
    let mut out = text.to_vec();
    for (key, val) in [
        ("m_ExtraDataString", &extra),
        ("m_EntryDataString", &entries),
    ] {
        let old = format!(
            "\"{key}\":\"{}\"",
            c.get(key).and_then(Json::as_str).unwrap_or("")
        );
        if count(&out, old.as_bytes()) != 1 {
            return Err(format!("catalog: {key} not found once as written"));
        }
        let new = format!("\"{key}\":\"{}\"", b64encode(val));
        out = replace_bytes(&out, old.as_bytes(), new.as_bytes());
    }
    Ok((out, done, sized))
}

/// --data-path: the data folder's own sub-folders (not Unity's) under Game_Data/.
pub fn data_folder_items(src: &str, dst: &str) -> std::io::Result<Vec<(String, String, String)>> {
    const UNITY_DIRS: &[&str] = &[
        "Managed",
        "Resources",
        "StreamingAssets",
        "Plugins",
        "Mono",
        "MonoBleedingEdge",
        "il2cpp_data",
        "Il2CppData",
        "UnitySubsystems",
        "GI",
        "Raw",
    ];
    let mut items = Vec::new();
    for n in crate::paths::listdir(src)? {
        let p = pkgzip::py::join(src, &n);
        if !Path::new(&p).is_dir() || UNITY_DIRS.contains(&n.as_str()) || n.starts_with('.') {
            continue;
        }
        for (r, _, mut fs) in crate::paths::walk(&p, true) {
            fs.sort();
            for f in fs {
                let s = pkgzip::py::join(&r, &f);
                items.push((
                    pkgzip::py::join(dst, &crate::paths::relpath(&s, src)),
                    s,
                    "link".to_string(),
                ));
            }
        }
    }
    Ok(items)
}

/// --asmpatch with the patch the mode needs.
pub fn asmpatch_command(cmd: &str, mode: &str) -> String {
    match patch_of(mode) {
        Some(p) => format!("{cmd} --with={p}"),
        None => cmd.to_string(),
    }
}

/// A bundle android-data cannot patch in place: every serialized file's platform set and the
/// bundle saved again (chunked LZ4). Returns (node, old platform, method).
pub fn repack_bundle(
    path: &Path,
    platform: u32,
    why: &str,
    db: Option<Arc<unityfs::ClassDb>>,
) -> unityfs::Result<Vec<(String, u32, String)>> {
    let mut b = unityfs::data::BundleFiles::open(path, db)?;
    let mut res = Vec::new();
    for (i, sf) in b.files.iter_mut() {
        let name = b.bundle.header.nodes[*i].name.clone();
        let method = if why.is_empty() {
            "repack".to_string()
        } else {
            format!("repack ({why})")
        };
        res.push((name, sf.target_platform as u32, method));
        sf.target_platform = platform as i32;
    }
    let tmp = crate::paths::with_suffix(path, ".tmp");
    b.write_to(&tmp, None)?;
    drop(b);
    std::fs::rename(&tmp, path)?;
    Ok(res)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base64_round_trip() {
        for s in [&b""[..], b"a", b"ab", b"abc", b"abcd", b"\x00\xff\x10"] {
            assert_eq!(b64decode(&b64encode(s)).unwrap(), s);
        }
        assert_eq!(b64encode(b"ab"), "YWI=");
        assert!(b64decode("YWI").is_err());
        assert_eq!(b64decode("YW\nI=").unwrap(), b"ab");
    }

    fn catalog(js: &str) -> Vec<u8> {
        // one entry pointing at a JsonObject record at offset 0
        let mut extra = vec![7u8, 1, b'a', 1, b'c'];
        let u: Vec<u8> = js.encode_utf16().flat_map(u16::to_le_bytes).collect();
        extra.extend((u.len() as i32).to_le_bytes());
        extra.extend(&u);
        let mut entries = 1i32.to_le_bytes().to_vec();
        let mut e = [0u8; 28];
        e[0..4].copy_from_slice(&1i32.to_le_bytes()); // its internal id: m_InternalIds[1]
        e[16..20].copy_from_slice(&0i32.to_le_bytes());
        entries.extend(e);
        format!(
            "{{\"m_X\":1,\"m_InternalIds\":[\"x\",\"0#/a.bundle\"],\"m_InternalIdPrefixes\":[\"{{RT}}\"],\"m_ExtraDataString\":\"{}\",\"m_EntryDataString\":\"{}\"}}",
            b64encode(&extra),
            b64encode(&entries)
        )
        .into_bytes()
    }

    fn no_size(_: &str) -> Option<u64> {
        None
    }

    #[test]
    fn catalog_crcs_follow() {
        let c = catalog(r#"{"m_Crc":123,"m_Hash":"x"}"#);
        assert_eq!(catalog_has(&c), (true, false));
        let (out, n, sized) = catalog_follow(&c, &[(123, 4567890)], &no_size).unwrap();
        assert_eq!((n, sized), (1, 0));
        assert_eq!(out, catalog(r#"{"m_Crc":4567890,"m_Hash":"x"}"#));
        let (same, n, sized) = catalog_follow(&c, &[(9, 10)], &no_size).unwrap();
        assert_eq!((same, n, sized), (c.clone(), 0, 0));
        assert_eq!(catalog_has(&catalog(r#"{"m_Crc":0}"#)), (false, false));
    }

    #[test]
    fn catalog_sizes_follow() {
        let c = catalog(r#"{"m_Crc":0,"m_BundleSize":100,"m_Hash":"x"}"#);
        assert_eq!(catalog_has(&c), (false, true));
        // the record's bundle is the entry's internal id, its prefix put back
        let size = |id: &str| (id == "{RT}/a.bundle").then_some(12345u64);
        let (out, n, sized) = catalog_follow(&c, &[], &size).unwrap();
        assert_eq!((n, sized), (0, 1));
        assert_eq!(out, catalog(r#"{"m_Crc":0,"m_BundleSize":12345,"m_Hash":"x"}"#));
        // a size that is the file's already, and a bundle that is no file here: as written
        let same = |id: &str| (id == "{RT}/a.bundle").then_some(100u64);
        assert_eq!(catalog_follow(&c, &[], &same).unwrap(), (c.clone(), 0, 0));
        assert_eq!(catalog_follow(&c, &[], &no_size).unwrap(), (c.clone(), 0, 0));
        // both at once
        let c = catalog(r#"{"m_Crc":7,"m_BundleSize":100}"#);
        let (out, n, sized) = catalog_follow(&c, &[(7, 8)], &size).unwrap();
        assert_eq!((n, sized), (1, 1));
        assert_eq!(out, catalog(r#"{"m_Crc":8,"m_BundleSize":12345}"#));
    }

    #[test]
    fn windows_paths() {
        let j = br#"x {UnityEngine.AddressableAssets.Addressables.RuntimePath}\\StandaloneWindows64\\a b.bundle y"#;
        let (out, n) = catalog_fix_paths(j, false);
        assert_eq!(n, 1);
        assert_eq!(
            out,
            br#"x {UnityEngine.AddressableAssets.Addressables.RuntimePath}/StandaloneWindows64/a b.bundle y"#
        );
        let (b, n) = catalog_bin_crcs(
            b"\x01\x00\x00\x00 \x02\x00\x00\x00\x02\x00\x00\x00",
            &[(1, 5), (2, 6)],
        );
        assert_eq!(
            (b.as_slice(), n),
            (&b"\x05\x00\x00\x00 \x02\x00\x00\x00\x02\x00\x00\x00"[..], 1)
        );
    }
}
