//! The result checked (android_data.py verify): every plain serialized file loads (object count,
//! platform, version), every settings object reads, every bundle's CAB headers say Android,
//! --verify-bundles bundles read whole, and the Managed set covers what MonoManager loads.
use crate::android_data::{list_repr, Ctx, Item, Stop, ANDROID};
use crate::ggm;
use crate::pyjson::{self, Json};
use crate::typetrees;
use pkgzip::py;
use std::path::Path;
use unityfs::bundle::Bundle;
use unityfs::{DataFile, SerializedFile};

fn problems_repr(bad: &[(String, String)]) -> String {
    format!(
        "[{}]",
        bad.iter()
            .take(20)
            .map(|(a, b)| format!("({}, {})", py::str_repr(a), py::str_repr(b)))
            .collect::<Vec<_>>()
            .join(", ")
    )
}

pub fn verify(ctx: &mut Ctx, items: &[Item]) -> Result<Json, Stop> {
    let want = (ctx.args.platform_patch == "data").then_some(ANDROID as i32);
    let out = ctx.args.out.clone();
    let mut bad: Vec<(String, String)> = Vec::new();
    let mut n_sf = 0;
    for it in items {
        if !matches!(
            it.act.as_str(),
            "sfpatch" | "sfinline" | "ggm" | "sfvideo" | "sfvideo709"
        ) {
            continue;
        }
        let dst = py::join(&out, &it.dst);
        let f = DataFile::open(&dst, &ctx.opts())?;
        if f.is_bundle {
            bad.push((it.dst.clone(), "not a serialized file".into()));
            continue;
        }
        let sf = &f.sf;
        let added = if it.act.starts_with("sfvideo") { 1 } else { 0 };
        if Path::new(&it.src).exists() {
            let f0 = DataFile::open(&it.src, &ctx.opts())?;
            if sf.objects.len() != f0.sf.objects.len() + added {
                bad.push((
                    it.dst.clone(),
                    format!(
                        "{} objects, source {}",
                        sf.objects.len(),
                        f0.sf.objects.len()
                    ),
                ));
            }
        }
        if let Some(w) = want {
            if sf.target_platform != w {
                bad.push((it.dst.clone(), format!("platform {}", sf.target_platform)));
            }
        }
        if let Some(rt) = &ctx.retarget {
            if !sf.enable_type_tree && sf.unity_version != rt.as_bytes() {
                bad.push((
                    it.dst.clone(),
                    format!(
                        "version {}, the player is {rt}",
                        String::from_utf8_lossy(&sf.unity_version)
                    ),
                ));
            }
        }
        if it.act == "ggm" {
            for o in &sf.objects {
                typetrees::read(sf, o.path_id, &[])?;
            }
        }
        n_sf += 1;
    }
    ctx.log(&format!("verify: {n_sf} serialized files load in UnityPy"));
    let bundles: Vec<&Item> = items.iter().filter(|i| i.act == "bundle").collect();
    for it in &bundles {
        for (name, plat) in
            unityfs::platform::bundle_platforms(Path::new(&py::join(&out, &it.dst)))?
        {
            if let Some(w) = want {
                if plat as i32 != w {
                    bad.push((it.dst.clone(), format!("{name} platform {plat}")));
                }
            }
        }
    }
    ctx.log(&format!(
        "verify: {} bundles: CAB headers checked",
        bundles.len()
    ));
    let nb = if ctx.args.verify_bundles < 0 {
        bundles.len() as i64
    } else {
        ctx.args.verify_bundles
    };
    if nb > 0 && !bundles.is_empty() {
        let mut by_size: Vec<(u64, &Item)> = Vec::new();
        for it in &bundles {
            by_size.push((std::fs::metadata(py::join(&out, &it.dst))?.len(), it));
        }
        by_size.sort_by_key(|x| x.0);
        let l = by_size.len();
        let pick: Vec<&Item> = if nb as usize >= l {
            by_size.iter().map(|x| x.1).collect()
        } else {
            (0..nb as usize)
                .map(|i| by_size[(i as f64 * (l - 1) as f64 / (1.max(nb - 1)) as f64) as usize].1)
                .collect()
        };
        for it in pick {
            let path = py::join(&out, &it.dst);
            let b = Bundle::open(&path)?;
            let big = b.header.nodes_size() > 64 << 20;
            let idx: Vec<usize> = if big {
                (0..b.header.nodes.len())
                    .filter(|&i| b.header.nodes[i].flags & 4 != 0)
                    .collect()
            } else {
                unitypy_files(&b)?
            };
            let mut cnt = 0;
            for i in idx {
                let sf = SerializedFile::open(
                    &b.header.nodes[i].name,
                    b.node(i),
                    Some(ctx.db.clone()),
                    Some(&b.header.engine),
                )?;
                if let Some(w) = want {
                    if sf.target_platform != w {
                        bad.push((
                            it.dst.clone(),
                            format!("UnityPy platform {}", sf.target_platform),
                        ));
                    }
                }
                for o in &sf.objects {
                    sf.read_typetree(o.path_id)?;
                    cnt += 1;
                }
            }
            ctx.log(&format!("verify: {}: {cnt} objects parsed", it.dst));
        }
    }
    let data = format!("{}/{}/assets/bin/Data", out, ctx.name);
    let mut lists: Vec<(&str, Vec<String>)> =
        ggm::assembly_lists(Path::new(&py::join(&data, "globalgamemanagers")), ctx)?
            .into_iter()
            .map(|l| ("MonoManager", l))
            .collect();
    let sa = py::join(&data, "ScriptingAssemblies.json");
    if Path::new(&sa).exists() {
        let j = pyjson::loads_bytes(&std::fs::read(&sa)?).map_err(Stop::Fail)?;
        lists.push(("ScriptingAssemblies.json", ggm::json_names(&j)));
    }
    if lists.is_empty() {
        bad.push((
            "Managed".into(),
            "no assembly list (MonoManager.m_AssemblyNames, ScriptingAssemblies.json)".into(),
        ));
    }
    let mdir = py::join(&data, "Managed");
    for (what, names) in lists {
        let have = |a: &str| Path::new(&py::join(&mdir, a)).exists();
        let (game, engine) = ggm::split_assemblies(&names, Some(&ctx.unity));
        let miss: Vec<String> = game.iter().filter(|a| !have(a)).cloned().collect();
        if !miss.is_empty() {
            bad.push(("Managed".into(), format!("missing {}", list_repr(&miss))));
        }
        let skip: Vec<String> = engine.iter().filter(|a| !have(a)).cloned().collect();
        let tail = if miss.is_empty() {
            ", all present".to_string()
        } else {
            format!(", MISSING {}", list_repr(&miss))
        };
        ctx.log(&format!(
            "verify: {what}: {} engine modules (skipped, not in Managed: {}), {} game assemblies{tail}",
            engine.len() - skip.len(),
            list_repr(&skip),
            game.len()
        ));
    }
    if bad.is_empty() {
        ctx.log("verify: OK");
    } else {
        ctx.log(&format!(
            "verify: {} PROBLEMS: {}",
            bad.len(),
            problems_repr(&bad)
        ));
    }
    let mut v = Json::obj();
    v.set(
        "problems",
        Json::List(
            bad.into_iter()
                .map(|(a, b)| Json::List(vec![a.into(), b.into()]))
                .collect(),
        ),
    );
    Ok(v)
}

/// The nodes UnityPy opens as serialized files (not a stream by name, a serialized file's header).
fn unitypy_files(b: &Bundle) -> Result<Vec<usize>, Stop> {
    let mut out = Vec::new();
    for (i, n) in b.header.nodes.iter().enumerate() {
        if [".resS", ".resource", ".config", ".xml", ".dat"]
            .iter()
            .any(|e| n.name.ends_with(e))
        {
            continue;
        }
        let src = b.node(i);
        let first = src.read_clipped(0, 48)?;
        if unityfs::serialized::looks_serialized(&first, src.len()) {
            out.push(i);
        }
    }
    Ok(out)
}
