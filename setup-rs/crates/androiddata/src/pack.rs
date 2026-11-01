//! The tree as archives for the stock Mount path (android_data.py pack): <name>.apk =
//! assets/bin/Data/**, main.<code>.<package>.obb = the rest and the build-id marker (a patch OBB
//! for the overflow), the space for both checked before either is written (exit 4, NO_SPACE),
//! android_package.json and the files beside the archives.
use crate::android_data::{
    clone, gc_time_slice, min_sdk, quality_caps, write_plugin_map, Ctx, Stop, NO_SPACE,
};
use crate::paths;
use crate::plugins::Survey;
use crate::pyjson::{self, Json};
use crate::ress_split;
use crate::streaming as ST;
use pkgzip::py;
use pkgzip::zipwrite::{verify_zip, write_zip, Src, ZIP_LIMIT};
use std::fs;
use std::path::Path;

/// str.splitlines() (no line ends kept).
pub fn splitlines(t: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut start = 0;
    let mut it = t.char_indices().peekable();
    while let Some((i, c)) = it.next() {
        let n = match c {
            '\r' => {
                if let Some(&(_, '\n')) = it.peek() {
                    it.next();
                    2
                } else {
                    1
                }
            }
            '\n' | '\x0b' | '\x0c' | '\x1c' | '\x1d' | '\x1e' | '\u{85}' | '\u{2028}'
            | '\u{2029}' => c.len_utf8(),
            _ => continue,
        };
        out.push(&t[start..i]);
        start = i + n;
    }
    if start < t.len() {
        out.push(&t[start..]);
    }
    out
}

fn size_of(s: &Src) -> Result<u64, Stop> {
    Ok(match s {
        Src::Bytes(b) => b.len() as u64,
        Src::Path(p) => fs::metadata(p)
            .map_err(|e| Stop::Fail(format!("{e}: {}", py::str_repr(p))))?
            .len(),
    })
}

/// An entry's room in an archive as split_big_apk and the OBB split reckon it.
fn room(a: &str, s: &Src) -> Result<u64, Stop> {
    Ok(size_of(s)? + 30 + 46 + 2 * a.chars().count() as u64 + 8)
}

fn src_clone(s: &Src) -> Src {
    match s {
        Src::Path(p) => Src::Path(p.clone()),
        Src::Bytes(b) => Src::Bytes(b.clone()),
    }
}

type Entry = (String, Src);

/// The entries of the APK, the main OBB (the marker first) and the patch OBB.
pub type Archives = (Vec<Entry>, Vec<Entry>, Vec<Entry>);

/// Each archive's limit: 2 GiB - 1 (Unity's zip reader seeks with a 32-bit long), or (tests
/// only: a small one to try the split and spread paths on small data) $PMU_ZIP_LIMIT bytes.
pub fn limit_from(v: Option<&str>) -> Result<u64, String> {
    match v {
        None => Ok(ZIP_LIMIT),
        Some(t) => t
            .trim()
            .parse::<u64>()
            .ok()
            .filter(|&n| (1 << 10..=ZIP_LIMIT).contains(&n))
            .ok_or_else(|| {
                format!("PMU_ZIP_LIMIT={t}: a number of bytes (1024 to {ZIP_LIMIT}) expected")
            }),
    }
}

/// The room split_big_apk leaves in the APK: 64 MiB at 2 GiB, the same share of a lower limit.
pub fn apk_margin(limit: u64) -> u64 {
    (limit + 1) / 32
}

/// The room the OBBs keep: 16 MiB at 2 GiB, the same share of a lower limit.
pub fn obb_margin(limit: u64) -> u64 {
    (limit + 1) / 128
}

/// An entry's size in an archive as write_zip reckons it (headers and data).
fn entry_size(a: &str, s: &Src) -> Result<u64, Stop> {
    Ok(30 + 46 + 2 * a.len() as u64 + 8 + size_of(s)?)
}

/// An archive's size as write_zip reckons it (its end record, each entry's headers and data).
fn zip_size(entries: &[&Entry]) -> Result<u64, Stop> {
    let mut t = 22;
    for (a, s) in entries {
        t += entry_size(a, s)?;
    }
    Ok(t)
}

fn apk_first(rel: &str) -> bool {
    rel.starts_with("Managed/")
        || rel.starts_with("Resources/")
        || rel == "boot.config"
        || rel.starts_with("globalgamemanagers")
        || rel == "unity default resources"
        || rel == "level0"
        || rel.starts_with("sharedassets0.")
        || rel == "ScriptingAssemblies.json"
        || rel == "RuntimeInitializeOnLoads.json"
        || rel == "unity_obb_guid"
}

/// A game whose assets/bin/Data passes the APK's 2 GiB: the largest files of later scenes and
/// resources move to the OBBs until the APK fits (`parts`: a split .resS's parts, which may move
/// whatever their file). Returns (apk entries, moved entries).
fn split_big_apk(
    apk: Vec<Entry>,
    limit: u64,
    parts: &[(String, Vec<String>)],
) -> Result<(Vec<Entry>, Vec<Entry>), Stop> {
    let limit = limit - apk_margin(limit);
    let mut total = 0;
    for (a, s) in &apk {
        total += room(a, s)?;
    }
    if total <= limit {
        return Ok((apk, Vec::new()));
    }
    let pre = "assets/bin/Data/";
    let mut movable: Vec<(u64, usize)> = Vec::new();
    for (i, (a, s)) in apk.iter().enumerate() {
        if a.starts_with(pre)
            && matches!(s, Src::Path(_))
            && (!apk_first(&a[pre.len()..]) || ress_split::is_part(parts, a))
        {
            movable.push((room(a, s)?, i));
        }
    }
    // sorted(movable, key=size, reverse=True): stable, the largest first
    movable.sort_by(|x, y| y.0.cmp(&x.0));
    let mut moved_idx = Vec::new();
    for (sz, i) in movable {
        if total <= limit {
            break;
        }
        moved_idx.push(i);
        total -= sz;
    }
    if total > limit {
        return Err(Stop::Exit(format!(
            "pack: the APK would still hold {:.2} GiB after moving every later scene and resource to the OBBs",
            total as f64 / (1u64 << 30) as f64
        )));
    }
    let keep: Vec<String> = moved_idx.iter().map(|&i| apk[i].0.clone()).collect();
    let mut moved: Vec<Entry> = moved_idx
        .iter()
        .map(|&i| (apk[i].0.clone(), src_clone(&apk[i].1)))
        .collect();
    moved.sort_by(|x, y| {
        x.0.cmp(&y.0)
            .then_with(|| src_key(&x.1).cmp(&src_key(&y.1)))
    });
    let rest = apk.into_iter().filter(|(a, _)| !keep.contains(a)).collect();
    Ok((rest, moved))
}

fn src_key(s: &Src) -> String {
    match s {
        Src::Path(p) => p.clone(),
        Src::Bytes(_) => String::new(),
    }
}

/// Where the OBBs' entries go: (indices into `obb` added to the APK, the main OBB's, the patch
/// OBB's). In order, as the main OBB holds them and the rest in the patch OBB; when that passes
/// the limit, spread by size instead: the largest first, each into the first archive with room,
/// the APK (`apk_room`: its free bytes, files of bin/Data only), the main OBB (its margin kept,
/// as in order), the patch OBB (up to the limit, as in order). The flag: spread.
#[allow(clippy::type_complexity)]
fn obb_places(
    obb: &[Entry],
    marker: &[Entry],
    apk_room: Option<u64>,
    limit: u64,
) -> Result<(Vec<usize>, Vec<usize>, Vec<usize>, bool), Stop> {
    let cap = limit - obb_margin(limit);
    let (mut main, mut rest) = (Vec::new(), Vec::new());
    let mut size = 0;
    for (i, (a, s)) in obb.iter().enumerate() {
        let sz = room(a, s)?;
        if size + sz < cap && rest.is_empty() {
            main.push(i);
            size += sz;
        } else {
            rest.push(i);
        }
    }
    let patch: Vec<&Entry> = marker.iter().chain(rest.iter().map(|&i| &obb[i])).collect();
    if rest.is_empty() || zip_size(&patch)? <= limit {
        return Ok((Vec::new(), main, rest, false));
    }
    let mut order: Vec<(u64, usize)> = Vec::new();
    for (i, (a, s)) in obb.iter().enumerate() {
        order.push((room(a, s)?, i));
    }
    order.sort_by(|x, y| y.0.cmp(&x.0));
    let (mut to_apk, mut main, mut patch) = (Vec::new(), Vec::new(), Vec::new());
    let (mut a_used, mut m_used) = (0, 0);
    let mut p_size = zip_size(&marker.iter().collect::<Vec<_>>())?;
    for (sz, i) in order {
        let (a, s) = &obb[i];
        let bytes = entry_size(a, s)?;
        if a.starts_with("assets/bin/") && apk_room.is_some_and(|r| a_used + sz < r) {
            to_apk.push(i);
            a_used += sz;
        } else if m_used + sz < cap {
            main.push(i);
            m_used += sz;
        } else if p_size + bytes <= limit {
            patch.push(i);
            p_size += bytes;
        } else {
            return Err(Stop::Exit(format!(
                "pack: {} ({:.2} GiB) fits in none of the archives: the APK and both OBBs are full ({:.2} GiB each at most)",
                obb[i].0,
                gib(sz as f64),
                gib(limit as f64)
            )));
        }
    }
    for v in [&mut to_apk, &mut main, &mut patch] {
        v.sort();
    }
    Ok((to_apk, main, patch, true))
}

/// Where the entries go (`what`: --pack all, apk or obb): the APK's entries, the main OBB's (the
/// marker first) and the patch OBB's. With all, the APK's later files move to the OBBs when it
/// passes the limit (split_big_apk); the OBBs' entries fill the main OBB in order and the rest go
/// to the patch OBB, or, when that would pass the limit, are spread by size (obb_places). The
/// log says where a split .resS's parts went. `names`: the archives' file names.
pub fn place_archives(
    mut apk: Vec<Entry>,
    obb: Vec<Entry>,
    marker: &[Entry],
    limit: u64,
    what: &str,
    names: &[String; 3],
    log: &mut dyn FnMut(&str),
) -> Result<Archives, Stop> {
    let mut obb = obb;
    // a .resS split into parts (ress_split.rs: too big for one archive inlined)
    let groups = ress_split::groups(
        apk.iter()
            .map(|(a, _)| a.as_str())
            .filter(|a| a.starts_with("assets/bin/")),
    );
    if what == "all" {
        let (a, moved) = split_big_apk(apk, limit, &groups)?;
        apk = a;
        if !moved.is_empty() {
            let mut sz = 0;
            for (_, s) in &moved {
                sz += size_of(s)?;
            }
            log(&format!(
                "pack: the data passes 2 GiB: {} files of later scenes and resources ({:.2} GiB) go into the OBBs, as Unity's \
                 \"split application binary\" does (the player looks bin/Data up in every mounted archive)",
                moved.len(),
                gib(sz as f64)
            ));
            let mut v = moved;
            v.append(&mut obb);
            obb = v;
        }
    }
    let both_apk = matches!(what, "apk" | "all");
    let mut main: Vec<Entry> = marker
        .iter()
        .map(|(a, s)| (a.clone(), src_clone(s)))
        .collect();
    let mut rest: Vec<Entry> = Vec::new();
    if matches!(what, "obb" | "all") {
        let apk_room = if both_apk {
            let mut used = 0;
            for (a, s) in &apk {
                used += room(a, s)?;
            }
            Some((limit - apk_margin(limit)).saturating_sub(used))
        } else {
            None
        };
        let (to_apk, m, r, spread) = obb_places(&obb, marker, apk_room, limit)?;
        if spread {
            log(&format!(
                "pack: in order the patch OBB would pass {:.2} GiB: the files spread by size instead, the largest first: {} into the APK, \
                 {} into the main OBB, {} into the patch OBB",
                gib(limit as f64),
                to_apk.len(),
                m.len(),
                r.len()
            ));
        }
        let mut slots: Vec<Option<Entry>> = obb.into_iter().map(Some).collect();
        for i in to_apk {
            apk.push(slots[i].take().unwrap());
        }
        main.extend(m.into_iter().map(|i| slots[i].take().unwrap()));
        rest.extend(r.into_iter().map(|i| slots[i].take().unwrap()));
    }
    let nm = marker.len();
    for (base, members) in &groups {
        let at = |m: &str| {
            let has = |v: &[Entry]| v.iter().any(|(a, _)| a == m);
            if both_apk && has(&apk) {
                names[0].clone()
            } else if has(&main[nm..]) {
                names[1].clone()
            } else if has(&rest) {
                names[2].clone()
            } else {
                "no archive (not packed)".into()
            }
        };
        let wh: Vec<String> = members
            .iter()
            .map(|m| format!("{} in {}", py::basename(m), at(m)))
            .collect();
        log(&format!(
            "pack: {} split in {} parts (its file too big for one archive with its textures inlined): {}",
            py::basename(base),
            members.len(),
            wh.join(", ")
        ));
    }
    Ok((apk, main, rest))
}

/// Free bytes for an unprivileged writer (f_bavail * f_frsize; u32 on 32-bit targets).
#[allow(clippy::unnecessary_cast)]
fn free_space(dir: &str) -> Result<u64, Stop> {
    let c = std::ffi::CString::new(dir).map_err(|e| Stop::Fail(e.to_string()))?;
    let mut st: libc::statvfs = unsafe { std::mem::zeroed() };
    // SAFETY: c is a valid C string and st a valid statvfs buffer.
    let r = unsafe { libc::statvfs(c.as_ptr(), &mut st) };
    if r != 0 {
        return Err(Stop::Fail(format!(
            "{}: {}",
            std::io::Error::last_os_error(),
            py::str_repr(dir)
        )));
    }
    if let Ok(v) = std::env::var("PMU_TEST_FREE_BYTES") {
        // (tests: a card this full)
        if let Ok(v) = v.parse() {
            return Ok(v);
        }
    }
    Ok(st.f_bavail as u64 * st.f_frsize as u64)
}

fn gib(n: f64) -> f64 {
    n / (1u64 << 30) as f64
}

/// What check_space says when the archives (need bytes) would leave less than 1 GiB of free.
fn short_of_space(need: u64, free: u64) -> Option<String> {
    (free < need + (1 << 30)).then(|| {
        format!(
            "pack: the APK and OBB need {:.2} GiB, {:.2} GiB free (keeping 1 GiB spare): free {:.1} GiB more on the card",
            gib(need as f64),
            gib(free as f64),
            gib((need + (1 << 30) - free) as f64)
        )
    })
}

/// Exits NO_SPACE when the archives of these entries would leave less than 1 GiB free.
fn check_space(
    ctx: &mut Ctx,
    zdir: &str,
    entries: &[&Entry],
    replaced: &[&str],
) -> Result<(), Stop> {
    let mut need = 0u64;
    for (_, s) in entries {
        need += size_of(s)?;
    }
    let mut free = free_space(zdir)?;
    for p in replaced {
        if Path::new(p).is_file() {
            free += fs::metadata(p)?.len();
        }
    }
    if let Some(m) = short_of_space(need, free) {
        ctx.log(&m);
        return Err(Stop::Status(NO_SPACE));
    }
    Ok(())
}

pub fn pack(ctx: &mut Ctx, survey: &Survey) -> Result<Json, Stop> {
    let out = ctx.args.out.clone();
    let zdir = paths::abspath(
        &ctx.args
            .zip_dir
            .clone()
            .unwrap_or_else(|| py::join(&out, "zip")),
    );
    fs::create_dir_all(&zdir)?;
    let root = py::join(&out, &ctx.name);
    let mut files: Vec<(String, String)> = Vec::new();
    for (r, _, mut fs_) in paths::walk(&root, true) {
        fs_.sort();
        for n in fs_ {
            let p = py::join(&r, &n);
            files.push((paths::relpath(&p, &root), p));
        }
    }
    let ggm_path = format!("{root}/assets/bin/Data/globalgamemanagers");
    let bid = crate::ggm::build_id(Path::new(&ggm_path), ctx)?;
    let mut apk_entries: Vec<Entry> = files
        .iter()
        .filter(|(a, _)| a.starts_with("assets/bin/"))
        .map(|(a, p)| (a.clone(), Src::Path(p.clone())))
        .collect();
    let obb_all: Vec<(String, String)> = files
        .iter()
        .filter(|(a, _)| a.starts_with("assets/") && !a.starts_with("assets/bin/"))
        .cloned()
        .collect();
    let loose: Vec<(String, String)> = obb_all
        .iter()
        .filter(|(a, _)| {
            ctx.loose_assets.iter().any(|g| {
                paths::fnmatch(&a["assets/".len()..], g) || paths::fnmatch(py::basename(a), g)
            })
        })
        .cloned()
        .collect();
    let mode = ctx.args.streaming_assets.clone();
    let keep = place(ctx, &mode, &obb_all, &zdir)?;
    let mut obb_entries: Vec<Entry> = obb_all
        .iter()
        .filter(|e| !loose.contains(e) && keep.contains(e))
        .map(|(a, p)| (a.clone(), Src::Path(p.clone())))
        .collect();
    let beside: Vec<(String, String)> = files
        .iter()
        .filter(|(a, _)| !a.starts_with("assets/"))
        .cloned()
        .collect();
    let mut marker: Vec<Entry> = vec![(format!("assets/{bid}"), Src::Bytes(Vec::new()))];
    let mut res = Json::obj();
    res.set("build_id", bid.as_str().into());
    res.set("package", ctx.package.as_str().into());
    res.set("version_code", ctx.args.version_code.into());
    res.set("unity_version", ctx.unity.to_string().into());
    if ctx.args.mono64 {
        res.set("abi", "arm64-v8a".into());
        res.set("scripting", "mono".into());
        let slice = gc_time_slice(&ctx.args.src);
        if slice != 0 {
            res.set("gc_time_slice_ms", slice.into());
        }
    }
    if ctx.unity.line() >= crate::android_data::OBB_GUID_FROM {
        apk_entries.push((
            "assets/unity_obb_guid".into(),
            Src::Bytes(bid.as_bytes().to_vec()),
        ));
        marker.push(("unity_obb_guid".into(), Src::Bytes(bid.as_bytes().to_vec())));
        res.set("unity_obb_guid", bid.as_str().into());
    }
    let name = ctx.name.clone();
    let code = ctx.args.version_code;
    let apk = py::join(&zdir, &format!("{name}.apk"));
    let main_obb = py::join(&zdir, &format!("main.{code}.{}.obb", ctx.package));
    let patch_obb = py::join(&zdir, &format!("patch.{code}.{}.obb", ctx.package));
    let limit = ctx.zip_limit;
    let mut pack_what = ctx.args.pack.clone();
    if ctx.args.mono64 && pack_what == "all" {
        let mut total = 0;
        for (_, s) in apk_entries.iter().chain(obb_entries.iter()) {
            total += size_of(s)?;
        }
        if total < limit - apk_margin(limit) {
            apk_entries.append(&mut obb_entries);
            pack_what = "apk".into();
            for old in [&main_obb, &patch_obb] {
                if Path::new(old).exists() {
                    fs::remove_file(old)?;
                }
            }
            ctx.log("pack: IL2CPP data: one APK, no OBB");
        } else {
            ctx.log("pack: WARNING: IL2CPP data over 2 GiB: APK + OBB (Unity 2020.3 then extracts no IL2CPP data: untested)");
        }
    }
    let both_apk = matches!(pack_what.as_str(), "apk" | "all");
    let both_obb = matches!(pack_what.as_str(), "obb" | "all");
    let names = [
        py::basename(&apk).to_string(),
        py::basename(&main_obb).to_string(),
        py::basename(&patch_obb).to_string(),
    ];
    let (apk_entries, main, rest) = place_archives(
        apk_entries,
        obb_entries,
        &marker,
        limit,
        &pack_what,
        &names,
        &mut |m| ctx.log(m),
    )?;
    let nm = marker.len();
    let mut space: Vec<&Entry> = Vec::new();
    if both_apk {
        space.extend(apk_entries.iter());
    }
    if both_obb {
        space.extend(main[nm..].iter());
        space.extend(rest.iter());
    }
    check_space(ctx, &zdir, &space, &[&apk, &main_obb, &patch_obb])?;
    if both_apk {
        write_zip(&apk, &apk_entries, &mut |m| ctx_log(m), 4)?;
        verify_zip(&apk, &apk_entries, &mut |m| ctx_log(m), 40)?;
        res.set("apk", apk.as_str().into());
    }
    flush_log(ctx);
    if both_obb {
        let mut need = 0;
        for (_, s) in main[nm..].iter().chain(rest.iter()) {
            need += size_of(s)?;
        }
        let free = free_space(&zdir)?;
        if free < need + (1 << 30) {
            let m = format!(
                "pack: OBB needs {:.2} GiB, only {:.2} GiB free (keeping 1 GiB spare)",
                gib(need as f64),
                gib(free as f64)
            );
            ctx.log(&m);
            return Err(Stop::Status(NO_SPACE));
        }
        write_zip(&main_obb, &main, &mut |m| ctx_log(m), 4)?;
        verify_zip(&main_obb, &main, &mut |m| ctx_log(m), 40)?;
        res.set("main_obb", main_obb.as_str().into());
        if !rest.is_empty() {
            let mut all: Vec<Entry> = marker
                .iter()
                .map(|(a, s)| (a.clone(), src_clone(s)))
                .collect();
            all.extend(rest);
            write_zip(&patch_obb, &all, &mut |m| ctx_log(m), 4)?;
            verify_zip(&patch_obb, &all, &mut |m| ctx_log(m), 40)?;
            res.set("patch_obb", patch_obb.as_str().into());
        }
        flush_log(ctx);
    }
    for (a, p) in &files {
        if a.starts_with("assets/bin/Data/Managed/etc/") {
            clone(p, &py::join(&zdir, a), true)?;
        }
    }
    for n in paths::listdir(&out)? {
        if ctx.beside.iter().any(|g| paths::fnmatch(&n, g)) {
            clone(&py::join(&out, &n), &py::join(&zdir, &n), true)?;
        }
    }
    for (a, p) in &beside {
        clone(p, &py::join(&zdir, a), true)?;
    }
    for (a, p) in &loose {
        clone(p, &py::join(&zdir, a), true)?;
    }
    if ctx.args.streaming_assets != "obb" {
        let mut s = Json::obj();
        s.set("mode", ctx.args.streaming_assets.as_str().into());
        s.set("folder", ST::SA_DIR.into());
        res.set("streaming_assets", s);
    }
    if ctx.args.data_path {
        fs::create_dir_all(py::join(&zdir, ST::FOLDER))?;
        res.set("data_path", ST::FOLDER.into());
    }
    if !loose.is_empty() {
        res.set(
            "loose_assets",
            Json::List(loose.iter().map(|(a, _)| a.as_str().into()).collect()),
        );
        let m = format!(
            "pack: {} StreamingAssets files beside the APK ({})",
            loose.len(),
            ctx.loose_assets.join(", ")
        );
        ctx.log(&m);
    }
    write_plugin_map(ctx, survey, &zdir)?;
    let sdk = min_sdk(ctx)?;
    if sdk != 0 {
        res.set("min_sdk", sdk.into());
        ctx.log(&format!("pack: min_sdk {sdk} (ua_host reports Android API {sdk}: OpenGL ES 3.1+, compute shaders)"));
    }
    let caps = quality_caps(&ctx.args.src);
    if !caps.is_empty() {
        res.set("quality", caps.as_str().into());
        ctx.log(&format!(
            "pack: quality caps {caps} (ua_host applies them to the settings the game changes)"
        ));
    }
    let mut pj = Json::obj();
    for (k, v) in res.as_obj().unwrap() {
        let v = if matches!(k.as_str(), "apk" | "main_obb" | "patch_obb") {
            Json::from(py::basename(&v.py_str()))
        } else {
            v.clone()
        };
        pj.set(k, v);
    }
    let mut md = Json::obj();
    md.set("unity.build-id", bid.as_str().into());
    pj.set("meta_data", md);
    let pjp = py::join(&zdir, "android_package.json");
    fs::write(&pjp, pyjson::dumps(&pj, Some(1)))?;
    let m = format!(
        "pack: build-id {bid}; host: getPackageCodePath -> {apk}, getObbDir -> {zdir}, PackageInfo.versionCode {code}, package {}",
        ctx.package
    );
    ctx.log(&m);
    Ok(res)
}

// write_zip and verify_zip log through a closure that cannot borrow ctx (it is borrowed for the
// entries): their lines are kept here and logged right after
thread_local! {
    static PENDING: std::cell::RefCell<Vec<String>> = const { std::cell::RefCell::new(Vec::new()) };
}

fn ctx_log(m: &str) {
    PENDING.with(|p| p.borrow_mut().push(m.to_string()));
}

fn flush_log(ctx: &mut Ctx) {
    let lines: Vec<String> = PENDING.with(|p| std::mem::take(&mut *p.borrow_mut()));
    for l in lines {
        // (pack(..., log) in the Python: android-data's log)
        ctx.log(&l);
    }
}

/// streaming.place: StreamingAssets beside the APK as zip/Game_Data/StreamingAssets/ (both,
/// loose). Returns the OBB's share.
fn place(
    ctx: &mut Ctx,
    mode: &str,
    files: &[(String, String)],
    zdir: &str,
) -> Result<Vec<(String, String)>, Stop> {
    let dst = py::join(zdir, ST::SA_DIR);
    let _ = fs::remove_dir_all(&dst);
    if mode == "obb" {
        let g = py::join(zdir, ST::FOLDER);
        if Path::new(&g).is_dir() && paths::listdir(&g)?.is_empty() {
            fs::remove_dir(&g)?;
        }
        return Ok(files.to_vec());
    }
    let mut size = 0;
    for (a, p) in files {
        clone(p, &py::join(&dst, &a["assets/".len()..]), true)?;
        size += fs::metadata(p)?.len();
    }
    fs::create_dir_all(&dst)?;
    let tail = if mode == "both" {
        ", and in the OBB (or the APK)"
    } else {
        " (not in the OBB or APK)"
    };
    let m = format!(
        "pack: StreamingAssets: {} files ({:.1} MiB) beside the APK in {}/{tail}",
        files.len(),
        size as f64 / (1u64 << 20) as f64,
        ST::SA_DIR
    );
    ctx.log(&m);
    Ok(if mode == "both" {
        files.to_vec()
    } else {
        Vec::new()
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lines() {
        assert_eq!(splitlines("ok a\r\nok b\n\nx"), ["ok a", "ok b", "", "x"]);
        assert!(apk_first("Managed/x.dll"));
        assert!(apk_first("sharedassets0.assets.resS"));
        assert!(!apk_first("level1"));
    }

    #[test]
    fn space() {
        assert_eq!(short_of_space(1 << 30, 3 << 30), None);
        assert_eq!(
            short_of_space(1_500_000_000, 1 << 30).unwrap(),
            "pack: the APK and OBB need 1.40 GiB, 1.00 GiB free (keeping 1 GiB spare): free 1.4 GiB more on the card"
        );
    }

    #[test]
    fn big_apk_split() {
        let d = std::env::temp_dir().join(format!("androiddata-split-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        let file = |n: &str, size: u64| {
            let p = d.join(n);
            fs::File::create(&p).unwrap().set_len(size).unwrap(); // (sparse)
            p.to_string_lossy().into_owned()
        };
        let small: Vec<Entry> = vec![(
            "assets/bin/Data/level1".into(),
            Src::Path(file("a", 1 << 20)),
        )];
        let (apk, moved) = split_big_apk(small, ZIP_LIMIT, &[]).unwrap();
        assert_eq!((apk.len(), moved.len()), (1, 0));
        let big: Vec<Entry> = vec![
            (
                "assets/bin/Data/Managed/x.dll".into(),
                Src::Path(file("m", 900 << 20)),
            ),
            (
                "assets/bin/Data/level0".into(),
                Src::Path(file("l0", 300 << 20)),
            ),
            (
                "assets/bin/Data/level2".into(),
                Src::Path(file("l2", 500 << 20)),
            ),
            (
                "assets/bin/Data/sharedassets1.assets".into(),
                Src::Path(file("s1", 600 << 20)),
            ),
            (
                "assets/bin/Data/level1".into(),
                Src::Path(file("l1", 400 << 20)),
            ),
        ];
        let (apk, moved) = split_big_apk(big, ZIP_LIMIT, &[]).unwrap();
        let names = |v: &[Entry]| v.iter().map(|e| e.0.clone()).collect::<Vec<_>>();
        // (the largest later files, until the APK fits: sharedassets1 then level2; sorted by name)
        assert_eq!(
            names(&moved),
            [
                "assets/bin/Data/level2",
                "assets/bin/Data/sharedassets1.assets"
            ]
        );
        assert_eq!(
            names(&apk),
            [
                "assets/bin/Data/Managed/x.dll",
                "assets/bin/Data/level0",
                "assets/bin/Data/level1"
            ]
        );
        let _ = fs::remove_dir_all(&d);
    }
}
