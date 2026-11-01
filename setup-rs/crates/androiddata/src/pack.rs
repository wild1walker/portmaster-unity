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
/// resources move to the OBBs until the APK fits. Returns (apk entries, moved entries).
fn split_big_apk(apk: Vec<Entry>) -> Result<(Vec<Entry>, Vec<Entry>), Stop> {
    let limit = ZIP_LIMIT - (64 << 20);
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
        if a.starts_with(pre) && matches!(s, Src::Path(_)) && !apk_first(&a[pre.len()..]) {
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
    let mut pack_what = ctx.args.pack.clone();
    if ctx.args.mono64 && pack_what == "all" {
        let mut total = 0;
        for (_, s) in apk_entries.iter().chain(obb_entries.iter()) {
            total += size_of(s)?;
        }
        if total < ZIP_LIMIT - (64 << 20) {
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
    if pack_what == "all" {
        let (a, moved) = split_big_apk(apk_entries)?;
        apk_entries = a;
        if !moved.is_empty() {
            let mut sz = 0;
            for (_, s) in &moved {
                sz += size_of(s)?;
            }
            let m = format!(
                "pack: the data passes 2 GiB: {} files of later scenes and resources ({:.2} GiB) go into the OBBs, as Unity's \
                 \"split application binary\" does (the player looks bin/Data up in every mounted archive)",
                moved.len(),
                gib(sz as f64)
            );
            ctx.log(&m);
            let mut v = moved;
            v.append(&mut obb_entries);
            obb_entries = v;
        }
    }
    let both_apk = matches!(pack_what.as_str(), "apk" | "all");
    let both_obb = matches!(pack_what.as_str(), "obb" | "all");
    let mut space: Vec<&Entry> = Vec::new();
    if both_apk {
        space.extend(apk_entries.iter());
    }
    if both_obb {
        space.extend(obb_entries.iter());
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
        for (_, s) in &obb_entries {
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
        let mut main: Vec<Entry> = marker
            .iter()
            .map(|(a, s)| (a.clone(), src_clone(s)))
            .collect();
        let mut rest: Vec<Entry> = Vec::new();
        let mut size = 0;
        for (a, s) in obb_entries {
            let sz = room(&a, &s)?;
            if size + sz < ZIP_LIMIT - (16 << 20) && rest.is_empty() {
                main.push((a, s));
                size += sz;
            } else {
                rest.push((a, s));
            }
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
        let (apk, moved) = split_big_apk(small).unwrap();
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
        let (apk, moved) = split_big_apk(big).unwrap();
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
