//! A port's settings (port.py's DEFAULTS, merge, build_config, aarch64_only_route, validate,
//! validate_metadata, dump_toml): port.toml (or .json) over the defaults, resolved; port.toml
//! written back with each section explained.
use crate::dict;
use crate::routes::route_donors;
use crate::rx;
use crate::val::{Dict, Val};
use crate::{exit, pyjson};
use pkgzip::{io_at, Result};

/// port.aarch64_only's default (docs/arm64.md, "Shipping aarch64 only"): false ships the armhf
/// route where a port takes it; true makes every port's release aarch64 only.
pub const AARCH64_ONLY_DEFAULT: bool = false;
/// The routes whose releases carry nothing for armhf.
/// The Unity lines whose .NET 3.5 games take the mono-arm64 route (Mono's 4.x class libraries).
pub const MONO64_NET35_LINES: [&str; 1] = ["2017.4"];
/// The class libraries such a release ships (player.mono64_classlibs = true).
pub const MONO64_CLASSLIBS: [&str; 8] = [
    "mscorlib.dll", "System.dll", "System.Core.dll", "System.Xml.dll", "Mono.Security.dll", "System.Configuration.dll",
    "System.Security.dll", "Mono.Posix.dll",
];
pub const AARCH64_ROUTES: [&str; 4] = ["mono-arm64", "il2cpp-arm64", "box64", "apk"];
pub const DEFAULT_STEPS: [&str; 4] = ["shaders", "settings", "audio", "quality"];
pub const HOOK_POINTS: [&str; 5] = ["game", "pre_convert", "post_convert", "post_pack", "post_install"];
pub const PORT_GENRES: [&str; 15] = [
    "action", "adventure", "arcade", "casino/card", "fps", "platformer", "puzzle", "racing", "rhythm", "rpg",
    "simulation", "sports", "strategy", "visual novel", "other",
];
pub const AVAILABILITY: [&str; 4] = ["free", "paid", "demo", "full"];
/// The game.data of an apk port: what the setup takes (and *.obb).
pub const APK_GLOBS: &str = "*.apk *.xapk *.apks *.aab *.zip";

fn empty() -> Val {
    Val::Dict(Dict::new())
}

fn list() -> Val {
    Val::List(Vec::new())
}

/// port.py's DEFAULTS.
pub fn defaults() -> Dict {
    let s = Val::s;
    dict! {
        "port" => Val::Dict(dict! {
            "name" => s(""), "title" => s(""), "route" => s("android"), "porter" => list(), "desc" => s(""),
            "desc_md" => s(""), "inst" => s(""), "inst_md" => s(""), "genres" => list(), "availability" => s(""),
            "store" => list(), "reqs" => list(), "setup_time" => s(""), "exp" => Val::Bool(false),
            "ready_marker" => s(".setup_done"), "thanks" => s(""), "screenshot" => s("screenshot.png"),
            "cover" => s("cover.png"), "aarch64_only" => Val::Bool(AARCH64_ONLY_DEFAULT),
        }),
        "game" => Val::Dict(dict! {
            "data" => s("*_Data"), "build" => s("linux"), "unity_versions" => list(), "what" => s(""),
            "size_mb" => Val::Int(0), "setup_space_mb" => Val::Int(0), "developer" => s(""), "publisher" => s(""),
            "release_date" => s(""), "genre" => s(""), "url" => s(""), "description" => s(""),
        }),
        "controls" => empty(),
        "player" => Val::Dict(dict! {
            "backend" => s("mono"), "abi" => s("armeabi-v7a"), "sha256" => empty(), "donors" => list(),
        }),
        "convert" => Val::Dict(dict! {
            "steps" => Val::strs(&DEFAULT_STEPS),
            "settings" => Val::Dict(dict! { "graphics_apis" => Val::List(vec![Val::Int(11)]) }),
        }),
        "android_data" => Val::Dict(dict! {
            "asmpatch" => s("off"), "video_shader" => Val::Bool(true), "swappy" => s("off"),
            "streaming_assets" => s("obb"), "data_path" => Val::Bool(false), "winbcl" => s("auto"),
            "name" => s(""), "package" => s(""), "args" => list(),
        }),
        "wwise" => Val::Dict(dict! { "banks" => s("") }),
        "fmod" => Val::Dict(dict! { "banks" => s("") }),
        "runtime" => empty(),
        "runtime_low" => empty(),
        "runtime_lowmem" => empty(),
        "questions" => list(),
        "files" => Val::Dict(dict! {
            "plugins" => list(), "managed" => list(), "assembly_patches" => list(), "hooks" => empty(), "extra" => list(), "own" => list(),
        }),
        "build" => Val::Dict(dict! { "commands" => list(), "outputs" => empty(), "licenses" => empty() }),
    }
}

/// Deep merge of config dicts: tables merge, everything else is replaced.
pub fn merge(base: &Dict, over: &Dict) -> Dict {
    let mut out = base.clone();
    for (k, v) in over.iter() {
        match (v, out.get(k)) {
            (Val::Dict(vd), Some(Val::Dict(bd))) => {
                let m = merge(bd, vd);
                out.set(k, Val::Dict(m));
            }
            _ => out.set(k, v.clone()),
        }
    }
    out
}

/// load_config: port.toml, or a .json file.
pub fn load_config(path: &str) -> Result<Dict> {
    let raw = std::fs::read(path).map_err(io_at(path))?;
    let v = if path.ends_with(".json") {
        pyjson::loads(&String::from_utf8_lossy(&raw))?
    } else {
        let t = String::from_utf8(raw).map_err(|_| pkgzip::Error::Fail(format!("{path}: not UTF-8")))?;
        crate::toml::loads(&t)?
    };
    match v {
        Val::Dict(d) => Ok(d),
        _ => Err(pkgzip::Error::Fail(format!("{path}: not a table"))),
    }
}

/// port.aarch64_only: the android route moves to mono-arm64 (the player's settings become the
/// route's), an apk-route port keeps only its arm64-v8a player. Changes cfg (and user's player
/// table) in place.
pub fn aarch64_only_route(cfg: &mut Dict, user: &mut Dict, log: &mut dyn FnMut(&str)) -> Result<()> {
    aarch64_only_route_rep(cfg, user, None, log)
}

/// aarch64_only_route with the check's report (`port new --data`): a game whose check says it
/// cannot take the mono-arm64 route stops here.
pub fn aarch64_only_route_rep(cfg: &mut Dict, user: &mut Dict, rep: Option<&Dict>, log: &mut dyn FnMut(&str)) -> Result<()> {
    if !cfg.dict("port").truthy("aarch64_only") {
        return Ok(());
    }
    if cfg.dict("port").str("route") == "android" {
        let m = rep.and_then(|r| r.list("routes").iter().filter_map(Val::as_dict).find(|r| r.str("route") == "mono-arm64"));
        if let Some(m) = m.filter(|m| !m.truthy("ok")) {
            return Err(exit(format!(
                "port.aarch64_only: this game cannot take the mono-arm64 route: {}",
                m.list("why").iter().map(Val::py_str).collect::<Vec<_>>().join("; ")
            )));
        }
    }
    let route = cfg.dict("port").get("route").cloned().unwrap_or(Val::Null);
    match route.as_str() {
        Some("android") => {
            log("port.aarch64_only: the mono-arm64 route (Unity's arm64 player, the game's code on Unity's Mono for \
                 aarch64) instead of the android route's armv7 player");
            cfg.table("port").set("route", Val::s("mono-arm64"));
            let own = user.dict("player").clone();
            let abi = own.get("abi").cloned().unwrap_or(Val::s("armeabi-v7a"));
            let backend = own.get("backend").cloned().unwrap_or(Val::s("mono"));
            if !abi.eq(&Val::s("arm64-v8a")) || !backend.eq(&Val::s("il2cpp")) {
                let pl = cfg.table("player");
                for k in ["backend", "abi"] {
                    pl.pop(k);
                }
                if let Some(Val::Dict(up)) = user.get_mut("player") {
                    for k in ["backend", "abi"] {
                        up.pop(k);
                    }
                }
                let pl = cfg.table("player");
                pl.set("sha256", Val::Dict(Dict::new()));
                pl.set("backend", Val::s("il2cpp"));
                pl.set("abi", Val::s("arm64-v8a"));
            }
        }
        Some("apk") => {
            let pl = cfg.table("player");
            let has64 = pl.list("abis").iter().any(|a| a.eq(&Val::s("arm64-v8a")));
            if has64 {
                pl.set("abis", Val::strs(&["arm64-v8a"]));
            } else if !pl.truthy("il2cpp") {
                return Err(exit(
                    "port.aarch64_only: the build has no arm64-v8a player (player.abis); a Mono build takes \
                     player.il2cpp = true (its code compiled for the arm64 IL2CPP player)",
                ));
            }
        }
        _ => {
            let r = route.py_str();
            if !AARCH64_ROUTES.contains(&r.as_str()) {
                return Err(exit(format!(
                    "port.aarch64_only: the {r} route has no aarch64-only release (routes: {})",
                    AARCH64_ROUTES.join(", ")
                )));
            }
        }
    }
    Ok(())
}

/// build_config(name, None, user): DEFAULTS <- the porter's config, resolved (the check's report is
/// not read: `pmu port new` takes no --data).
pub fn build_config(name: &str, user: &Dict) -> Dict {
    build_config_rep(name, None, user)
}

/// build_config(name, rep, user): DEFAULTS <- what the check found <- the porter's config.
pub fn build_config_rep(name: &str, rep: Option<&Dict>, user: &Dict) -> Dict {
    let mut cfg = merge(&defaults(), &Dict::new());
    if let Some(rep) = rep.filter(|r| !r.is_empty()) {
        let android = rep.str("build") == "android";
        let versions = if rep.truthy("unity_version") { vec![rep.get("unity_version").cloned().unwrap()] } else { Vec::new() };
        let size = rep.get("size_mb").cloned().unwrap_or(Val::Int(0));
        let or = |k: &str, d: &str| if rep.truthy(k) { rep.get(k).map_or(String::new(), Val::py_str) } else { d.to_string() };
        let mut found = Dict::new();
        if android {
            found.table("port").set("title", Val::Str(rx::title_case(&or("product", name))));
            found.table("port").set("route", Val::Str(or("route", "apk")));
            let g = found.table("game");
            g.set("data", Val::s(APK_GLOBS));
            g.set("build", Val::s("android"));
            g.set("unity_versions", Val::List(versions));
            g.set("size_mb", size);
        } else {
            let b = rep.str("build");
            let data = rep.str("data");
            let base = pkgzip::py::basename(data);
            let product = or("product", data.strip_suffix("_Data").map_or(name, |_| &base[..base.len() - 5]));
            let company = pkgzip::py::strip(rep.dict("data_survey").str("company")).to_string();
            found.table("port").set("title", Val::Str(rx::title_case(&product)));
            found.table("port").set("route", Val::Str(or("route", "android")));
            let g = found.table("game");
            g.set("data", Val::s(if b == "macos" { "Data" } else { base }));
            g.set("build", Val::s(if ["linux", "macos", "windows"].contains(&b) { b } else { "linux" }));
            g.set("unity_versions", Val::List(versions));
            g.set("size_mb", size);
            // (PlayerSettings' company: the developer, unless Unity's placeholder)
            g.set("developer", Val::Str(if company == "DefaultCompany" || company == "Default Company" { String::new() } else { company }));
        }
        cfg = merge(&cfg, &found);
        cfg = merge(&cfg, rep.dict("suggest"));
    }
    cfg = merge(&cfg, user);
    cfg.table("player").pop("url");
    if user.truthy("controls") {
        let c = user.get("controls").cloned().unwrap();
        cfg.set("controls", c);
    }
    let qs: Vec<Val> = cfg
        .list("questions")
        .iter()
        .filter(|q| {
            let d = q.as_dict();
            !(d.is_some_and(|d| d.get("id").is_some_and(|x| x.eq(&Val::s("PICTURE")))
                && d.get("convert_set").is_some_and(|x| x.eq(&Val::s("quality.profile")))))
        })
        .cloned()
        .collect();
    cfg.set("questions", Val::List(qs));
    {
        let p = cfg.table("port");
        if !p.truthy("name") {
            p.set("name", Val::s(name));
        }
        if !p.truthy("title") {
            p.set("title", Val::Str(rx::title_case(name)));
        }
        for k in ["porter", "genres", "reqs"] {
            if let Some(Val::Str(s)) = p.get(k) {
                let v = if s.is_empty() { Val::List(vec![]) } else { Val::List(vec![Val::s(s)]) };
                p.set(k, v);
            }
        }
        if let Some(Val::Dict(d)) = p.get("store") {
            let d = d.clone();
            p.set("store", Val::List(vec![Val::Dict(d)]));
        }
        let store: Vec<Val> = p
            .list("store")
            .iter()
            .map(|s| match s {
                Val::Dict(d) => {
                    let mut out = dict! { "name" => Val::s(""), "gameurl" => Val::s(""), "developerurl" => Val::s("") };
                    for (k, v) in d.iter() {
                        out.set(k, v.clone());
                    }
                    Val::Dict(out)
                }
                x => x.clone(),
            })
            .collect();
        let sold = !store.is_empty();
        p.set("store", Val::List(store));
        if !p.truthy("availability") {
            p.set("availability", Val::s(if sold { "paid" } else { "free" }));
        }
    }
    let title = cfg.dict("port").get("title").map(Val::py_str).unwrap_or_default();
    let pname = cfg.dict("port").get("name").map(Val::py_str).unwrap_or_default();
    {
        let g = cfg.table("game");
        let vs: Vec<String> = g.list("unity_versions").iter().map(Val::py_str).collect();
        let vs = vs.join(", ");
        let build = g.get("build").map(Val::py_str).unwrap_or_default();
        if !g.truthy("what") && build == "android" {
            g.set(
                "what",
                Val::Str(format!(
                    "the Android build of {title}{}: its APK with its .obb files if it has any, or the .xapk, .apks or .aab it came as",
                    if vs.is_empty() { String::new() } else { format!(" (Unity {vs})") }
                )),
            );
        }
        if !g.truthy("what") {
            let kind = match build.as_str() {
                "linux" => "Linux",
                "macos" => "macOS",
                "windows" => "Windows",
                b => b,
            };
            let tail = if build == "macos" {
                ": the installed .app or the macOS installers (.pkg)".to_string()
            } else {
                format!(
                    ": its folder, the archive it came in (.zip, .7z, .tar.gz){}",
                    if build == "linux" { " or GOG's installer (.sh)" } else { "" }
                )
            };
            g.set(
                "what",
                Val::Str(format!(
                    "the {kind} build of {title}{}{tail}",
                    if vs.is_empty() { String::new() } else { format!(" (Unity {vs})") }
                )),
            );
        }
    }
    {
        let ad = cfg.table("android_data");
        if !ad.truthy("name") {
            let n: String = title.chars().filter(char::is_ascii_alphanumeric).collect();
            ad.set("name", Val::Str(if n.is_empty() { "Game".into() } else { n }));
        }
        if !ad.truthy("package") {
            let n: String = pname.to_lowercase().chars().filter(|c| c.is_ascii_lowercase() || c.is_ascii_digit()).collect();
            ad.set("package", Val::Str(format!("com.pmunity.{n}")));
        }
    }
    if !cfg.dict("port").truthy("setup_time") {
        let mb = match cfg.dict("game").get("size_mb") {
            Some(Val::Int(i)) => *i as f64,
            Some(Val::Float(f)) => *f,
            Some(Val::Bool(b)) => *b as i64 as f64,
            _ => 0.0,
        };
        let slow = cfg.dict("convert").list("steps").iter().any(|x| x.eq(&Val::s("textures")));
        let t = if slow {
            if mb > 300.0 { "about an hour" } else { "a quarter of an hour" }
        } else if mb > 1000.0 {
            "about a quarter of an hour"
        } else {
            "a few minutes"
        };
        cfg.table("port").set("setup_time", Val::s(t));
    }
    cfg
}

/// The first item of a question's option ([value, label]: value; a string: its first character).
pub fn opt(o: &Val, i: usize) -> Option<Val> {
    match o {
        Val::List(l) => l.get(i).cloned(),
        Val::Str(s) => s.chars().nth(i).map(|c| Val::Str(c.to_string())),
        _ => None,
    }
}

pub fn opt_len(o: &Val) -> usize {
    match o {
        Val::List(l) => l.len(),
        Val::Str(s) => s.chars().count(),
        _ => 0,
    }
}

/// validate(cfg): stops with every error found.
pub fn validate(cfg: &Dict) -> Result<()> {
    let p = cfg.dict("port");
    let mut errs: Vec<String> = Vec::new();
    let pname = p.get("name").cloned().unwrap_or(Val::Null);
    if !pname.as_str().is_some_and(rx::name) {
        errs.push(format!(
            "port.name {}: lowercase letters, digits, '.', '_', '-' (it is the folder in ports/)",
            pname.repr()
        ));
    }
    let title = p.get("title").cloned().unwrap_or(Val::Null);
    let t = title.py_str();
    if !title.truthy() || t.contains('/') || t.starts_with('.') {
        errs.push(format!("port.title {}: the launcher is '<title>.sh'", title.repr()));
    }
    for sec in ["runtime", "runtime_low", "runtime_lowmem"] {
        for k in cfg.dict(sec).keys() {
            if !rx::env(k) {
                errs.push(format!("{sec}.{k}: not an environment variable name"));
            }
        }
    }
    for q in cfg.list("questions") {
        let empty = Dict::new();
        let qd = q.as_dict().unwrap_or(&empty);
        let id = qd.get("id").cloned().unwrap_or(Val::s(""));
        let idr = qd.get("id").map_or("None".to_string(), Val::py_str);
        if !id.as_str().is_some_and(rx::id) {
            errs.push(format!(
                "questions: id {} must be like TEXTURE_QUALITY",
                qd.get("id").map_or("None".into(), Val::repr)
            ));
        }
        let opts: Vec<Val> = match qd.get("options") {
            Some(Val::List(l)) => l.clone(),
            _ => vec![],
        };
        if opts.is_empty() || opts.iter().any(|o| !rx::value(&opt(o, 0).map(|v| v.py_str()).unwrap_or_default())) {
            errs.push(format!("questions.{idr}: options [[value, label], ...] with plain values"));
        }
        if let Some(d) = qd.get("default").filter(|d| d.truthy()) {
            if !opts.iter().any(|o| opt(o, 0).is_some_and(|x| x.eq(d))) {
                errs.push(format!("questions.{idr}: default {} is not an option", d.repr()));
            }
        }
        if let Some(e) = qd.get("env").filter(|e| e.truthy()) {
            if !e.as_str().is_some_and(rx::env) {
                errs.push(format!("questions.{idr}: env {} is not a variable name", e.repr()));
            }
        }
    }
    let ad = cfg.dict("android_data");
    let a = ad.get("asmpatch").cloned().unwrap_or(Val::Null);
    let ok = match &a {
        Val::Str(s) => s == "auto" || s == "off",
        Val::List(l) => {
            l.iter().all(|x| x.as_str().is_some_and(rx::value)) && !l.iter().skip(1).any(|x| x.eq(&Val::s("auto")))
        }
        _ => false,
    };
    if !ok {
        errs.push(format!(
            "android_data.asmpatch {}: auto, off or a list of patch names ([\"auto\", ...]: the default ones and those)",
            a.repr()
        ));
    }
    for (sec, what) in [
        ("wwise", "the soundbanks' folder, relative to the game's data folder (e.g. StreamingAssets/Audio/GeneratedSoundBanks/Linux)"),
        ("fmod", "the FMOD banks' folder, relative to the game's data folder (e.g. StreamingAssets)"),
    ] {
        let b = cfg.dict(sec).get("banks").filter(|b| b.truthy()).cloned();
        if let Some(b) = b {
            let bad = match &b {
                Val::Str(s) => s.starts_with('/') || s.split('/').any(|x| x == "..") || !rx::banks(s),
                _ => true,
            };
            if bad {
                errs.push(format!("{sec}.banks {}: {what}", b.repr()));
            }
        }
    }
    let pl = cfg.dict("player");
    let mc = pl.get("mono64_classlibs");
    if mc.is_some_and(Val::truthy) && p.str("route") != "mono-arm64" {
        errs.push("player.mono64_classlibs: for the mono-arm64 route (a .NET 3.5 game on Mono's 4.x class libraries)".into());
    } else if let Some(m) = mc.filter(|m| !matches!(m, Val::Null)) {
        let ok = match m {
            Val::Bool(_) => true,
            Val::List(l) => !l.is_empty() && l.iter().all(|x| x.as_str().is_some_and(rx::dll_name)),
            _ => false,
        };
        if !ok {
            errs.push(format!(
                "player.mono64_classlibs {}: true, false or a list of assembly file names (e.g. \"System.Xml.dll\")",
                m.repr()
            ));
        }
    }
    if pl.truthy("il2cpp")
        && (p.str("route") != "apk" || !pl.get("backend").is_some_and(|b| b.eq(&Val::s("mono"))))
    {
        errs.push(
            "player.il2cpp: for the apk route with a Mono build (its code compiled for the arm64 IL2CPP player where \
             the system has no 32-bit userland); the il2cpp-arm64 route does this for desktop builds"
                .into(),
        );
    }
    let winbcl = ad.get("winbcl").cloned().unwrap_or(Val::s("auto"));
    if !matches!(winbcl.as_str(), Some("auto" | "off")) {
        errs.push(format!("android_data.winbcl {}: auto or off", winbcl.repr()));
    }
    let sa = ad.get("streaming_assets").cloned().unwrap_or(Val::s("obb"));
    if !matches!(sa.as_str(), Some("obb" | "both" | "loose")) {
        errs.push(format!("android_data.streaming_assets {}: obb, both or loose", sa.repr()));
    }
    for rel in cfg.dict("files").list("own") {
        let bad = match rel {
            Val::Str(s) => s.starts_with('/') || s.split('/').any(|x| x == "..") || s == "port.toml" || s == "port.build.json",
            _ => true,
        };
        if bad {
            errs.push(format!(
                "files.own: {} must be a path in the port's source folder (not port.toml or port.build.json)",
                rel.repr()
            ));
        }
    }
    for k in cfg.dict("files").dict("hooks").keys() {
        if !HOOK_POINTS.contains(&k) {
            errs.push(format!("files.hooks.{k}: one of {}", HOOK_POINTS.join(", ")));
        }
    }
    let rm = p.get("ready_marker").cloned().unwrap_or(Val::s(""));
    if !rm.as_str().is_some_and(rx::ready_marker) {
        errs.push(format!("port.ready_marker {}: a dot file name, e.g. .setup_done", rm.repr()));
    }
    for dst in cfg.dict("build").dict("outputs").keys() {
        if dst.starts_with('/') || dst.split('/').any(|x| x == "..") {
            errs.push(format!("build.outputs: {} must be a path inside the port folder", Val::s(dst).repr()));
        }
    }
    for v in cfg.dict("game").list("unity_versions") {
        if !v.as_str().is_some_and(rx::unity_version) {
            errs.push(format!("game.unity_versions: {} is not a Unity version", v.repr()));
        }
    }
    for n in pl.list("donors") {
        if rx::donor(&n.py_str()).is_none() {
            errs.push(format!(
                "player.donors: {} is not a donor name (donor-<version>-mono|il2cpp-armv7|arm64.apk)",
                n.repr()
            ));
        }
    }
    errs.extend(validate_metadata(cfg));
    if !errs.is_empty() {
        return Err(exit(format!("port config:\n  {}", errs.join("\n  "))));
    }
    Ok(())
}

/// port.toml's PortMaster fields: errors.
pub fn validate_metadata(cfg: &Dict) -> Vec<String> {
    let (p, g) = (cfg.dict("port"), cfg.dict("game"));
    let mut errs = Vec::new();
    for x in p.list("genres") {
        if !x.as_str().is_some_and(|s| PORT_GENRES.contains(&rx::casefold(s).as_str())) {
            errs.push(format!("port.genres: {} is not one of PortMaster's: {}", x.repr(), PORT_GENRES.join(", ")));
        }
    }
    let av = p.get("availability").cloned().unwrap_or(Val::Null);
    if !av.as_str().is_some_and(|a| AVAILABILITY.contains(&a)) {
        errs.push(format!("port.availability {}: {}", av.repr(), AVAILABILITY.join(" | ")));
    }
    for s in p.list("store") {
        let ok = s.as_dict().is_some_and(|d| {
            d.get("name").is_some_and(|n| n.as_str().is_some_and(|n| !n.is_empty()))
                && d.get("gameurl").is_some_and(|n| n.as_str().is_some_and(|n| !n.is_empty()))
        });
        if !ok {
            errs.push(format!(
                "port.store: {} must be a table {{name = \"Steam\", gameurl = \"https://...\", developerurl = \"https://...\"}} (PortMaster takes no bare links)",
                s.repr()
            ));
        }
    }
    if !p.list("porter").iter().all(|x| x.as_str().is_some_and(|s| !s.is_empty())) {
        errs.push(format!("port.porter {}: a list of names", p.get("porter").map_or("None".into(), Val::repr)));
    }
    for r in p.list("reqs") {
        if !r.as_str().is_some_and(rx::req) {
            errs.push(format!(
                "port.reqs: {} is not a device capability (e.g. \"!lowres\", \"hires\", \"2gb\", \"power\", \"opengl\")",
                r.repr()
            ));
        }
    }
    let d = g.get("release_date").filter(|d| d.truthy()).map(Val::py_str).unwrap_or_default();
    if !d.is_empty() {
        let ok = rx::date(&d).is_some_and(|(_, m, dd)| {
            let (m, dd): (u32, u32) = (m.parse().unwrap(), dd.parse().unwrap());
            (1..=12).contains(&m) && (1..=31).contains(&dd)
        });
        if !ok {
            errs.push(format!("game.release_date {}: YYYY-MM-DD", Val::s(&d).repr()));
        }
    }
    for k in ["screenshot", "cover"] {
        if let Some(v) = p.get(k).filter(|v| v.truthy()) {
            let ext = rx::splitext_ext(&v.py_str()).to_lowercase();
            if !matches!(ext.as_str(), ".png" | ".jpg" | ".jpeg") {
                errs.push(format!("port.{k} {}: a .png or .jpg file", v.repr()));
            }
        }
    }
    for (k, v) in cfg.dict("controls").iter() {
        let ok = v.as_str().is_some_and(|s| !pkgzip::py::strip(s).is_empty()) && !pkgzip::py::strip(k).is_empty();
        if !ok {
            errs.push(format!("controls.{k}: a button name and what it does (text)"));
        }
    }
    errs
}

/// _toml_value
pub fn toml_value(v: &Val) -> Result<String> {
    Ok(match v {
        Val::Bool(b) => (if *b { "true" } else { "false" }).into(),
        Val::Int(_) | Val::Float(_) => v.repr(),
        Val::Str(s) => pyjson::dumps(&Val::s(s), None, false),
        Val::List(l) => format!("[{}]", l.iter().map(toml_value).collect::<Result<Vec<_>>>()?.join(", ")),
        Val::Dict(d) => format!(
            "{{{}}}",
            d.iter()
                .map(|(k, x)| Ok(format!("{} = {}", toml_key(k), toml_value(x)?)))
                .collect::<Result<Vec<_>>>()?
                .join(", ")
        ),
        Val::Null => return Err(pkgzip::Error::Fail("TypeError: <class 'NoneType'>".into())),
    })
}

/// _toml_key
pub fn toml_key(k: &str) -> String {
    if rx::bare_key(k) {
        k.to_string()
    } else {
        pyjson::dumps(&Val::s(k), None, true)
    }
}

pub const CONFIG_SECTIONS: [&str; 13] = [
    "port", "game", "controls", "player", "convert", "android_data", "wwise", "fmod", "runtime", "runtime_low",
    "runtime_lowmem", "files", "build",
];

/// CONFIG_DOC: each section's explanation in port.toml.
pub fn config_doc(sec: &str) -> Option<&'static str> {
    Some(match sec {
        "port" => "# the port: its folder in ports/ (name), the menu entry and launcher \"<title>.sh\" (title), the\n\
# route (port check recommends one), the patcher's time estimate, and PortMaster's listing\n\
# (port.json): porter, desc and inst (plain text; desc_md, inst_md: Markdown for the website),\n\
# genres (action adventure arcade casino/card fps platformer puzzle racing rhythm rpg simulation\n\
# sports strategy \"visual novel\" other), availability (free | paid | demo | full), store =\n\
# [{name, gameurl, developerurl}], reqs (more device capabilities: \"!lowres\", \"2gb\", \"power\"),\n\
# exp (true only to hide a released port); thanks = the README's first sentence; screenshot and\n\
# cover = image files (relative to the port folder) the release takes; aarch64_only = true: a\n\
# release with nothing for armhf (a Mono game on the mono-arm64 route: Unity's arm64 player and\n\
# Unity's Mono for aarch64; no 32-bit ua_host, libraries or player in the zip or on the handheld)",
        "game" => "# what the player provides: the data folder the setup looks for (data, a glob), the build\n\
# kind (linux | macos | windows), the Unity versions the setup accepts (empty: any the\n\
# converter supports), how READMEs and messages describe it (what), its size in MB; and for\n\
# gameinfo.xml (the frontend's game list): developer, publisher, release_date (YYYY-MM-DD),\n\
# genre (default: the first of port.genres), description (default: port.desc), url (its page)",
        "controls" => "# the game's controls for the README and the testing thread, as the handheld labels its buttons\n\
# (most have A right, B bottom), one row each, in order: e.g. \"D-Pad\" = \"Move\", B = \"Jump\".\n\
# PortMaster's exit hotkey (Select + Start) is added. Leave out buttons that do nothing",
        "player" => "# Unity's Android player: backend/abi; sha256 = {\"<file>\" = \"<hash>\"} checked after the setup unpacks\n\
# it from the port's donors; donors = the donor APKs the release ships in donor/ (written by `port new`\n\
# from the route and game.unity_versions; names added here are shipped too). The handheld never\n\
# downloads Unity's player: `port package --donors DIR` takes the donors from DIR (tools/donor)",
        "convert" => "# pmu convert's config (each step's DEFAULTS); \"textures\" re-encodes BC/DXT as ASTC\n\
# (less memory, a longer setup, slightly different pixels: off unless you add it)",
        "android_data" => "# pmu android-data: asmpatch = auto (run tools/asmpatch; it changes only what it\n\
# finds: Rewired, the Input System package; PlayMaker's exact optimizations) | off |\n\
# [\"patch\", ...]; streaming_assets = obb |\n\
# both | loose (code that reads StreamingAssets as files: also/only beside the APK,\n\
# docs/android-data.md); data_path = true (files read or written at Application.dataPath);\n\
# winbcl = auto (the setup takes Unity's class libraries a build lacks for the player from the\n\
# port's Mono donor: a Windows 2021.2+ build's Unix ones, facades such as netstandard.dll) | off;\n\
# name = the APK's, package (keep it: it names the PlayerPrefs file); args = more options",
        "wwise" => "# a Wwise game's soundbanks (banks: their folder, relative to the game's data folder), which the setup\n\
# converts once for ua_host's own Wwise player (pmu wwise; docs/wwise-player.md); empty: silent",
        "fmod" => "# an FMOD Studio game's banks (banks: their folder, relative to the game's data folder), which the setup\n\
# converts once for ua_host's own FMOD player (pmu fmod; docs/fmod-player.md); empty: silent",
        "runtime" => "# ua_host's environment (docs/runtime.md, docs/input.md), written to port.env:\n\
# e.g. UA_POINTER = \"1\", UA_POINTER_CURSOR = \"0\", UA_KEYMAP = \"start:escape\", UA_PAD_NAME,\n\
# UA_OSK = \"done\" (the on-screen keyboard answers every soft keyboard at once), UA_KEYBOARD = \"1\",\n\
# UA_DPAD_STICK = \"auto\" (the d-pad drives the left stick on handhelds without sticks)",
        "runtime_low" => "# the Low picture: more of the environment on every handheld (no setup question offers another;\n\
# UA_PICTURE=high set by hand at the top of port.env skips it): reductions of the picture,\n\
# never of the game play (docs/porting.md, \"The Low picture at launch\"), e.g.\n\
# ASMPATCH_HK_BLUR = \"off\", UA_RENDER_PIXELS = \"560000\", UA_PREFS_DEFAULTS (the game's own options)",
        "runtime_lowmem" => "# more of the environment, only on handhelds with 1 GB of RAM or less (port.env sets it there\n\
# unless set already), with either picture: what spares memory at a price a bigger handheld\n\
# need not pay; speedups go in [runtime], for every handheld (docs/porting.md, \"Small handhelds\")",
        "build" => "# commands `port package` runs in this folder first (env PMUNITY, PORT_SRC, BUILD), the files\n\
# they make put into the port folder (outputs = {\"setup/files/x\" = \"{build}/x\"}), and their licences\n\
# (licenses = {\"LICENSE.x.txt\" = \"path\" or \"{build}/path\"})",
        "files" => "# the porter's own files (paths relative to this file), copied into setup/: converter plugins\n\
# (--plugin), managed = assemblies shipped into the game (--managed-file), assembly_patches =\n\
# [{name, command, files}] ({setup} = the setup folder), hooks = {game|pre_convert|post_convert|\n\
# post_pack|post_install = \"x.sh\"} sourced by the setup, extra = files for the port folder,\n\
# own = files of the port's source folder `port new` leaves as they are (e.g. \"<title>.sh\", a\n\
# hand-made launcher or \"<name>/setup/patchscript\"; the template's version is not written)",
        _ => return None,
    })
}

pub const QUESTIONS_DOC: &str = "# Questions PortMaster's patcher asks before the setup (each answer is kept for later runs), e.g.:
# [[questions]]
# id = \"TEXTURE_QUALITY\"
# question = \"Texture quality? High is what this port is tested with.\"
# options = [[\"medium\", \"High (recommended)\"], [\"fast\", \"Low (shorter setup)\"]]
# default = \"medium\"
# convert_set = \"textures.quality\"        # the answer -> pmu convert --set textures.quality=<answer>
# # env = \"UA_POINTER\"                    # or: the answer -> the game's environment
";

pub const CONTROLS_DRAFT_DOC: &str = "# (drafted by `port new` from the game's default pad bindings, asmpatch --survey: reword them\n\
# and check them on a handheld)";

/// dump_toml(cfg, route, drafted): port.toml text for a resolved config.
pub fn dump_toml(cfg: &Dict, route: &Dict, drafted: &[&str]) -> Result<String> {
    let mut out: Vec<String> = vec![
        "# port.toml: a PortMaster port made with `pmu port new` (docs/porting.md).".into(),
        "# Edit, then run `port new <name> --data <game>` again to regenerate the port folder.".into(),
        String::new(),
    ];
    let own_docs = route.dict("config_doc");
    let secs: Vec<String> = match route.get("config") {
        Some(c) if c.truthy() => c.as_list().unwrap_or(&[]).iter().map(Val::py_str).collect(),
        _ => CONFIG_SECTIONS.iter().map(|s| s.to_string()).collect(),
    };
    let empty = Dict::new();
    for sec in &secs {
        let val = cfg.get(sec).and_then(Val::as_dict).unwrap_or(&empty);
        if (sec == "wwise" || sec == "fmod") && !val.truthy("banks") {
            continue;
        }
        let doc = match own_docs.get(sec) {
            Some(d) => d.py_str(),
            None => config_doc(sec)
                .ok_or_else(|| pkgzip::Error::Fail(format!("KeyError: {}", Val::s(sec).repr())))?
                .to_string(),
        };
        out.push(doc);
        if drafted.contains(&sec.as_str()) && !val.is_empty() {
            out.push(CONTROLS_DRAFT_DOC.into());
        }
        out.push(format!("[{sec}]"));
        let mut subs = Vec::new();
        for (k, v) in val.iter() {
            if sec == "convert" {
                if let Val::Dict(d) = v {
                    if !d.is_empty() {
                        subs.push((k, d));
                        continue;
                    }
                }
            }
            let mut v = v.clone();
            if sec == "player" && k == "donors" {
                let pl = cfg.dict("player");
                let opt_str = |k: &str| pl.get(k).map_or("None".to_string(), Val::py_str);
                let made = route_donors(route, cfg.dict("game").list("unity_versions"), &opt_str("backend"), &opt_str("abi"));
                if !made.is_empty() {
                    out.push(format!("# donors port new ships for the route and game.unity_versions: {}", made.join(", ")));
                }
                if let Val::List(l) = &v {
                    v = Val::List(l.iter().filter(|n| !made.iter().any(|m| n.eq(&Val::s(m)))).cloned().collect());
                }
            }
            out.push(format!("{} = {}", toml_key(k), toml_value(&v)?));
        }
        for (k, v) in subs {
            out.push(format!("[{sec}.{}]", toml_key(k)));
            for (kk, vv) in v.iter() {
                out.push(format!("{} = {}", toml_key(kk), toml_value(vv)?));
            }
        }
        out.push(String::new());
    }
    let qs = cfg.list("questions");
    if !qs.is_empty() {
        for q in qs {
            out.push("[[questions]]".into());
            if let Some(d) = q.as_dict() {
                for (k, v) in d.iter() {
                    out.push(format!("{} = {}", toml_key(k), toml_value(v)?));
                }
            }
            out.push(String::new());
        }
    } else {
        out.push(QUESTIONS_DOC.into());
    }
    Ok(out.join("\n"))
}
