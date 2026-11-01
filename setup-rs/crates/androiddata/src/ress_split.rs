//! A serialized file too big for one archive with its textures inlined (each archive must stay
//! under 2 GiB: Unity's zip reader seeks with a 32-bit long): its .resS is not inlined but kept,
//! cut into parts that each fit an archive. The cuts fall between streamed blobs (a texture's or
//! a mesh's stream never spans two parts); part 0 keeps the .resS's name and bytes 0 to the first
//! cut, so what streams from there is unchanged; part k (`<file>.<k>.resS`) holds the bytes from
//! cut k on, and every texture and mesh reading from it gets that path and its offset in the
//! part (StreamingInfo). The pixels and vertices are the same bytes: nothing is scaled or
//! re-encoded. Unity's Android players read external .resS streams through the VFS, from the
//! APK or an OBB (pack.rs places the parts).
use crate::android_data::{Ctx, Stop, ANDROID};
use pkgzip::py;
use std::collections::HashMap;
use std::fs;
use std::io::{Read, Seek, SeekFrom, Write};
use unityfs::inline::{restreamed, stream_refs, StreamRef};
use unityfs::SerializedFile;

/// Unity lines before this one: streamed textures are known not to load in the 2017.4 Android
/// player (android_data.py inline_texture_streams: its asynchronous upload never runs).
pub const STREAMS_FROM: (u32, u32) = (2018, 3);

/// When a file is split, and into parts of what size.
#[derive(Clone, Debug, PartialEq)]
pub struct SplitRule {
    /// The largest part (and the largest file with its textures inlined): one archive's room.
    pub part: u64,
    /// (tests) the files split whatever their size; None: by size.
    pub only: Option<Vec<String>>,
}

impl SplitRule {
    /// The rule for an archive limit (the APK's margin off it), or (tests only)
    /// $PMU_RESS_SPLIT = `<bytes>[:<file>,<file>...]`: parts of that size, and only the files
    /// named split (all that pass it inlined without names).
    pub fn new(limit: u64, env: Option<&str>) -> Result<SplitRule, String> {
        let part = limit - crate::pack::apk_margin(limit);
        let Some(v) = env else {
            return Ok(SplitRule { part, only: None });
        };
        let (n, names) = match v.split_once(':') {
            Some((n, l)) => (n, Some(l)),
            None => (v, None),
        };
        let part: u64 = n
            .trim()
            .parse()
            .ok()
            .filter(|&p| p > 0)
            .ok_or_else(|| format!("PMU_RESS_SPLIT={v}: <bytes>[:<file>,...] expected"))?;
        let only = names.map(|l| {
            l.split(',')
                .filter(|s| !s.is_empty())
                .map(String::from)
                .collect()
        });
        Ok(SplitRule { part, only })
    }

    /// Whether a file is split: its size with its textures inlined, a .resS kept beside it (for
    /// meshes) and its size.
    pub fn wants(&self, name: &str, inlined: u64, kept: Option<u64>) -> bool {
        match &self.only {
            Some(l) => l.iter().any(|n| n == name),
            None => inlined > self.part || kept.is_some_and(|k| k > self.part),
        }
    }
}

/// Where a stream file of `len` bytes is cut (each part's first byte; the first is 0) for
/// parts of at most `cap` bytes, as even as the blobs allow: each cut is the place no blob spans
/// (a blob's start or end) nearest an even share of what is left, ceil(left / cap) parts.
/// `ranges`: the blobs (start, end).
pub fn cut_points(ranges: &[(u64, u64)], len: u64, cap: u64) -> Result<Vec<u64>, String> {
    let mut r: Vec<(u64, u64)> = ranges.iter().copied().filter(|(s, e)| e > s).collect();
    r.sort();
    // the blobs merged where they overlap (not where they only touch): a cut may fall at the
    // start or end of each merged run
    let mut runs: Vec<(u64, u64)> = Vec::new();
    for (s, e) in r {
        match runs.last_mut() {
            Some(l) if s < l.1 => l.1 = l.1.max(e),
            _ => runs.push((s, e)),
        }
    }
    let mut cand: Vec<u64> = runs
        .iter()
        .flat_map(|&(s, e)| [s, e])
        .filter(|&p| p > 0 && p < len)
        .collect();
    cand.dedup();
    let mut cuts = vec![0u64];
    let mut start = 0u64;
    while len - start > cap {
        let left = len - start;
        let aim = start + left.div_ceil(left.div_ceil(cap));
        let c = cand
            .iter()
            .copied()
            .filter(|&c| c > start && c - start <= cap)
            .min_by_key(|&c| c.abs_diff(aim));
        let Some(c) = c else {
            let next = cand.iter().copied().find(|&c| c > start).unwrap_or(len);
            return Err(format!(
                "the streams from byte {start} to {next} ({} bytes) cannot be cut: they pass one archive ({cap} bytes)",
                next - start
            ));
        };
        cuts.push(c);
        start = c;
    }
    Ok(cuts)
}

/// Part k's name: the .resS's own for part 0, `<file>.<k>.resS` after it.
pub fn part_name(file: &str, k: usize) -> String {
    if k == 0 {
        format!("{file}.resS")
    } else {
        format!("{file}.{k}.resS")
    }
}

/// A path with its last part replaced.
fn with_name(path: &[u8], name: &str) -> Vec<u8> {
    let mut p = match path.iter().rposition(|&c| c == b'/') {
        Some(i) => path[..=i].to_vec(),
        None => Vec::new(),
    };
    p.extend_from_slice(name.as_bytes());
    p
}

/// What split_serialized did.
#[derive(Debug)]
pub struct Split {
    /// The parts' names and sizes, part 0 first.
    pub parts: Vec<(String, u64)>,
    pub textures: usize,
    pub meshes: usize,
    /// Streams moved to a later part (their objects rewritten).
    pub moved: usize,
    /// Bytes of the .resS no texture or mesh reads (carried along in the parts).
    pub unread: u64,
}

/// bytes [from, to) of src copied to dst (through dst.tmp).
fn copy_range(src: &str, from: u64, to: u64, dst: &str) -> Result<(), Stop> {
    let mut i =
        fs::File::open(src).map_err(|e| Stop::Fail(format!("{e}: {}", py::str_repr(src))))?;
    i.seek(SeekFrom::Start(from))?;
    let tmp = format!("{dst}.tmp");
    let mut o = std::io::BufWriter::with_capacity(
        1 << 20,
        fs::File::create(&tmp).map_err(|e| Stop::Fail(format!("{e}: {}", py::str_repr(&tmp))))?,
    );
    let n = std::io::copy(&mut (&mut i).take(to - from), &mut o)?;
    if n != to - from {
        return Err(Stop::Fail(format!(
            "{src}: {n} bytes read from {from}, {} expected",
            to - from
        )));
    }
    o.flush()?;
    drop(o);
    fs::rename(&tmp, dst)?;
    Ok(())
}

/// The serialized file `sf` (opened from `src`, its streams `refs`) written to `dst` with its
/// .resS kept and split into parts of at most `cap` bytes beside it, every stream in a later
/// part rewritten to it; the target platform Android, `prepare` (the retarget) applied before
/// it is saved. With `consume`, src is removed and its .resS becomes part 0 (moved, cut short),
/// else part 0 is a copy (or, one part, a hard link).
pub fn split_serialized(
    sf: &mut SerializedFile,
    mut refs: Vec<StreamRef>,
    src: &str,
    dst: &str,
    cap: u64,
    consume: bool,
    prepare: &dyn Fn(&mut SerializedFile) -> Result<(), Stop>,
) -> Result<Split, Stop> {
    let file = py::basename(src).to_string();
    let own = part_name(&file, 0);
    refs.extend(stream_refs(sf, &["CubemapArray"])?);
    if let Some(r) = refs.iter().find(|r| r.file() != own) {
        return Err(Stop::Exit(format!(
            "{src}: object {} streams from {} (not {own}): its .resS cannot be split",
            r.path_id,
            r.file()
        )));
    }
    let res = py::join(py::dirname(src), &own);
    let len = fs::metadata(&res)
        .map_err(|e| Stop::Fail(format!("{e}: {}", py::str_repr(&res))))?
        .len();
    let mut ranges = Vec::new();
    for r in refs.iter().filter(|r| r.size > 0) {
        let end = r.offset + r.size;
        if end > len {
            return Err(Stop::Exit(format!(
                "{src}: object {} streams bytes {}..{end} of {own}, which has {len}",
                r.path_id, r.offset
            )));
        }
        ranges.push((r.offset, end));
    }
    let cuts = cut_points(&ranges, len, cap).map_err(|m| Stop::Exit(format!("{src}: {m}")))?;
    let names: Vec<String> = (0..cuts.len()).map(|k| part_name(&file, k)).collect();
    let mut moves: HashMap<i64, (Vec<u8>, u64)> = HashMap::new();
    for r in &refs {
        let k = cuts.iter().rposition(|&c| c <= r.offset).unwrap_or(0);
        if k > 0 {
            moves.insert(
                r.path_id,
                (with_name(&r.path, &names[k]), r.offset - cuts[k]),
            );
        }
    }
    let mut covered = 0u64;
    let mut reach = 0u64;
    ranges.sort();
    for (s, e) in &ranges {
        let s = (*s).max(reach);
        if *e > s {
            covered += e - s;
        }
        reach = reach.max(*e);
    }
    sf.target_platform = ANDROID as i32;
    prepare(sf)?;
    let dd = py::dirname(dst).to_string();
    fs::create_dir_all(&dd)?;
    let mut parts = Vec::new();
    for k in 0..cuts.len() {
        let end = cuts.get(k + 1).copied().unwrap_or(len);
        parts.push((names[k].clone(), end - cuts[k]));
        if k > 0 {
            copy_range(&res, cuts[k], end, &py::join(&dd, &names[k]))?;
        }
    }
    let tmp = format!("{dst}.tmp");
    {
        let f = fs::File::create(&tmp)?;
        let mut w = std::io::BufWriter::with_capacity(1 << 20, f);
        sf.save_to(&mut w, |sf, o| match moves.get(&o.path_id) {
            Some((p, off)) => restreamed(sf, o.path_id, p, *off).map(Some),
            None => Ok(None),
        })?;
        w.flush()?;
    }
    fs::rename(&tmp, dst)?;
    let first = py::join(&dd, &own);
    if consume {
        fs::remove_file(src)?;
        crate::android_data::mv(&res, &first)?;
        if cuts.len() > 1 {
            fs::OpenOptions::new()
                .write(true)
                .open(&first)?
                .set_len(cuts[1])?;
        }
    } else if cuts.len() == 1 {
        crate::android_data::clone(&res, &first, true)?;
    } else {
        copy_range(&res, 0, cuts[1], &first)?;
    }
    Ok(Split {
        parts,
        textures: refs.iter().filter(|r| r.class != "Mesh").count(),
        meshes: refs.iter().filter(|r| r.class == "Mesh").count(),
        moved: moves.len(),
        unread: len - covered,
    })
}

/// split_serialized for android-data: the run's retarget.
pub fn split_for(
    ctx: &Ctx,
    sf: &mut SerializedFile,
    refs: Vec<StreamRef>,
    src: &str,
    dst: &str,
    consume: bool,
) -> Result<Split, Stop> {
    split_serialized(sf, refs, src, dst, ctx.split.part, consume, &|sf| {
        ctx.retarget_sf(sf).map(|_| ())
    })
}

/// The split files of a data tree: base (`<dir>/<file>.resS`) -> its parts' paths in order (the
/// base first), from the names (`<file>.<k>.resS` beside `<file>.resS`, k = 1, 2, ...).
pub fn groups<'a>(paths: impl IntoIterator<Item = &'a str>) -> Vec<(String, Vec<String>)> {
    let all: Vec<&str> = paths.into_iter().collect();
    let have: std::collections::HashSet<&str> = all.iter().copied().collect();
    let mut g: std::collections::BTreeMap<String, Vec<(usize, String)>> = Default::default();
    for p in &all {
        let Some(stem) = p.strip_suffix(".resS") else {
            continue;
        };
        let Some((file, ks)) = stem.rsplit_once('.') else {
            continue;
        };
        if ks.is_empty() || ks.starts_with('0') || !ks.bytes().all(|c| c.is_ascii_digit()) {
            continue;
        }
        let Ok(k) = ks.parse::<usize>() else {
            continue;
        };
        let base = format!("{file}.resS");
        if have.contains(base.as_str()) {
            g.entry(base).or_default().push((k, p.to_string()));
        }
    }
    g.into_iter()
        .map(|(base, mut v)| {
            v.sort();
            let mut l = vec![base.clone()];
            l.extend(v.into_iter().map(|(_, p)| p));
            (base, l)
        })
        .collect()
}

/// Whether a path looks like a later part of a split .resS (pack.rs: such parts may leave the
/// APK whatever their file).
pub fn is_part(groups: &[(String, Vec<String>)], p: &str) -> bool {
    groups.iter().any(|(_, l)| l.iter().any(|x| x == p))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cuts() {
        // three blobs of 40, 40 and 20 bytes, parts of at most 50: cut before each of the last two
        let r = [(0, 40), (40, 80), (80, 100)];
        assert_eq!(cut_points(&r, 100, 50).unwrap(), [0, 40, 80]);
        // parts of 100: one part
        assert_eq!(cut_points(&r, 100, 100).unwrap(), [0]);
        // as even as the blobs allow: 2 parts (40 + 60), not 3
        let r = [
            (0, 10),
            (10, 20),
            (20, 30),
            (30, 40),
            (40, 60),
            (60, 90),
            (90, 100),
        ];
        assert_eq!(cut_points(&r, 100, 60).unwrap(), [0, 40]);
        // the place nearest an even share, above it or below: 55, not 10
        let r = [(0, 10), (10, 55), (55, 100)];
        assert_eq!(cut_points(&r, 100, 60).unwrap(), [0, 55]);
        // a blob spanning another's start is never cut: (10, 50) covers 20 and 30
        let r = [(0, 10), (10, 50), (20, 30), (50, 70), (70, 100)];
        assert_eq!(cut_points(&r, 100, 50).unwrap(), [0, 50]);
        // the bytes no blob reads go with a part (a cut may fall where a blob ends)
        let r = [(0, 10), (30, 40)];
        assert_eq!(cut_points(&r, 40, 20).unwrap(), [0, 10, 30]);
        // a blob bigger than one part: refused
        let r = [(0, 10), (10, 90), (90, 100)];
        assert!(cut_points(&r, 100, 50).unwrap_err().contains("(80 bytes)"));
        let r = [(0, 10), (10, 100)];
        assert!(cut_points(&r, 100, 50).unwrap_err().contains("(90 bytes)"));
        // a big last blob that fits
        let r = [(0, 10), (10, 55)];
        assert_eq!(cut_points(&r, 55, 50).unwrap(), [0, 10]);
        // nothing read at all: one part if it fits
        assert_eq!(cut_points(&[], 10, 50).unwrap(), [0]);
    }

    #[test]
    fn rules_and_names() {
        let l = (1u64 << 31) - 1;
        let r = SplitRule::new(l, None).unwrap();
        assert_eq!(r.part, l - (64 << 20));
        assert!(!r.wants("resources.assets", r.part, None));
        assert!(r.wants("resources.assets", r.part + 1, None));
        assert!(r.wants("resources.assets", 1, Some(r.part + 1)));
        let t = SplitRule::new(l, Some("1000:resources.assets,level1")).unwrap();
        assert_eq!(t.part, 1000);
        assert!(t.wants("level1", 1, None) && !t.wants("level2", 1 << 40, None));
        assert_eq!(SplitRule::new(l, Some("4096")).unwrap().only, None);
        assert!(SplitRule::new(l, Some("x")).is_err());
        assert_eq!(part_name("resources.assets", 0), "resources.assets.resS");
        assert_eq!(part_name("resources.assets", 2), "resources.assets.2.resS");
        assert_eq!(with_name(b"a/b.resS", "b.1.resS"), b"a/b.1.resS");
        assert_eq!(with_name(b"b.resS", "b.1.resS"), b"b.1.resS");
        let g = groups([
            "d/resources.assets.resS",
            "d/resources.assets.2.resS",
            "d/resources.assets.1.resS",
            "d/level1.resS",
            "d/x.01.resS",
            "d/x.resS",
            "d/y.1.resS",
        ]);
        assert_eq!(
            g,
            [(
                "d/resources.assets.resS".to_string(),
                vec![
                    "d/resources.assets.resS".to_string(),
                    "d/resources.assets.1.resS".into(),
                    "d/resources.assets.2.resS".into()
                ]
            )]
        );
        assert!(is_part(&g, "d/resources.assets.2.resS") && !is_part(&g, "d/level1.resS"));
    }
}
