//! `pmu patch-obb pack <bundles dir> <game dir>` (patch_obb.py pack): an add-on's converted bundles
//! (Cuphead's DLC) as patch.<code>.<package>.obb beside an installed game, each given the Android
//! platform as android-data gives the game's own (streamed textures inlined, the CAB platform
//! set), the OBB the build-id marker. The built game is not touched.
use crate::android_data::{copyfile, Stop, ANDROID};
use crate::pyjson::{self, Json};
use pkgzip::py;
use pkgzip::zipwrite::{verify_zip, write_zip, Src};
use std::path::Path;
use std::sync::Arc;

pub const PREFIX: &str = "assets/AssetBundles/";

fn key<'a>(pk: &'a Json, k: &str) -> Result<&'a Json, Stop> {
    pk.get(k)
        .ok_or_else(|| Stop::Fail(format!("KeyError: {}", py::str_repr(k))))
}

/// The patch OBB of the bundles in `bundles` beside the game in `game`; log lines go to `log`.
pub fn pack(bundles: &str, game: &str, log: &mut dyn FnMut(&str)) -> Result<String, Stop> {
    let pjp = py::join(game, "android_package.json");
    let pk = pyjson::loads_bytes(
        &std::fs::read(&pjp).map_err(|e| Stop::Fail(format!("{e}: {}", py::str_repr(&pjp))))?,
    )
    .map_err(|e| Stop::Fail(format!("json.decoder.JSONDecodeError: {e}")))?;
    let obb = py::join(
        game,
        &format!(
            "patch.{}.{}.obb",
            key(&pk, "version_code")?.py_str(),
            key(&pk, "package")?.py_str()
        ),
    );
    if Path::new(&obb).exists() {
        return Err(Stop::Exit(format!("{obb}: there is a patch OBB already")));
    }
    let stage = py::join(bundles, ".android");
    let _ = std::fs::remove_dir_all(&stage);
    std::fs::create_dir_all(&stage)?;
    let bid = match key(&pk, "build_id")? {
        Json::Str(s) => s.clone(),
        _ => {
            return Err(Stop::Fail(
                "TypeError: can only concatenate str (not \"int\") to str".into(),
            ))
        }
    };
    let mut entries: Vec<(String, Src)> = vec![(format!("assets/{bid}"), Src::Bytes(Vec::new()))];
    let db = unityfs::ClassDb::default_db().ok().map(Arc::new);
    for n in crate::paths::listdir(bundles)? {
        let src = py::join(bundles, &n);
        if n.starts_with('.') || !Path::new(&src).is_file() || !unityfs::bundle::is_unityfs(&src) {
            continue;
        }
        let dst = py::join(&stage, &n);
        if crate::inline::has_ress_node(&src)? {
            crate::inline::inline_bundle_with(db.clone(), &src, &dst)?;
        } else {
            copyfile(&src, &dst)?;
            match unityfs::platform::patch_bundle_platform(Path::new(&dst), ANDROID) {
                Ok(_) => {}
                Err(
                    e @ (unityfs::Error::Format(_)
                    | unityfs::Error::Unsupported(_)
                    | unityfs::Error::Value(_)),
                ) => {
                    crate::streaming::repack_bundle(
                        Path::new(&dst),
                        ANDROID,
                        &e.to_string(),
                        db.clone(),
                    )?;
                }
                Err(e) => return Err(e.into()),
            }
        }
        entries.push((format!("{PREFIX}{n}"), Src::Path(dst)));
    }
    if entries.len() == 1 {
        return Err(Stop::Exit(format!("{bundles}: no asset bundles")));
    }
    write_zip(&obb, &entries, log, 4)?;
    verify_zip(&obb, &entries, log, 40)?;
    let _ = std::fs::remove_dir_all(&stage);
    Ok(obb)
}
