//! The check of an Android build (port.py's check_android): `pmu apk`'s dry run (what it is,
//! whether the runtime runs it, its ABIs), a Mono build's code survey (input), and the apk route.
use crate::pyx::*;
use crate::survey::{input_verdict, survey_assemblies};
use androiddata::uversion as UV;
use apk::json::Json;
use port::val::{Dict, Val};
use port::Repo;

fn val(j: &Json) -> Val {
    match j {
        Json::Null => Val::Null,
        Json::Bool(b) => Val::Bool(*b),
        Json::Int(i) => Val::Int(*i as i64),
        Json::Str(b) => Val::Str(String::from_utf8_lossy(b).into_owned()),
        Json::List(l) => Val::List(l.iter().map(val).collect()),
        Json::Obj(o) => Val::Dict(Dict(o.iter().map(|(k, v)| (k.clone(), val(v))).collect())),
    }
}

struct TempDir(String);

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn tempdir() -> port::Result<TempDir> {
    let base = std::env::temp_dir();
    for i in 0..1000u32 {
        let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.subsec_nanos());
        let p = base.join(format!("pmunity-check-{}-{t:08x}{i}", std::process::id()));
        if std::fs::create_dir(&p).is_ok() {
            return Ok(TempDir(port::ps(&p)));
        }
    }
    Err(port::exit(format!("{}: no temporary folder could be made", base.display())))
}

pub fn check_android(repo: &Repo, mut rep: Dict, inputs: &[String], log: &mut dyn FnMut(&str)) -> port::Result<Dict> {
    for (k, v) in [("build", s("android")), ("apk", s(&inputs[0])), ("apk_inputs", strs(inputs)), ("data", Val::Null),
                   ("backend", Val::Null), ("code", Val::Null), ("unity_version", Val::Null), ("unity_supported", Val::Bool(false))] {
        rep.set(k, v);
    }
    let mut route = dict(vec![("route", s("apk")), ("ok", Val::Bool(false)), ("why", Val::List(vec![])), ("needs", Val::List(vec![])),
                              ("available", Val::Bool(repo.templates().join("apk").join("route.json").is_file()))]);
    rep.set("routes", Val::List(vec![]));        // (its place among the report's keys; filled below)
    let mut lines: Vec<String> = Vec::new();
    let mut info: Option<Dict> = None;
    log("reading the Android build (pmunity apk --dry-run)");
    {
        let td = tempdir()?;
        let managed = join(&td.0, "Managed");
        // (every ARM player the build has, whatever machine checks it)
        let o = apk::Opts {
            target: "arm64,armhf".into(),
            abi: None,
            any_version: false,
            force: false,
            dry_run: true,
            il2cpp: false,
            il2cpp_opts: Default::default(),
            managed_to: Some(managed.clone()),
        };
        match apk::run(inputs, &join(&td.0, "out"), &o, &mut |m| lines.push(m.to_string())) {
            Ok(j) => info = val(&j).as_dict().cloned(),
            Err(apk::Fail::Refused(m)) => push(&mut route, "why", s(m)),
            Err(apk::Fail::Error(m) | apk::Fail::NotPorted(m) | apk::Fail::Exit(m)) => return Err(port::Error::Fail(m)),
        }
        if info.as_ref().is_some_and(|i| i.str("backend") == "mono") && isdir(&managed) {
            rep.set("code", Val::Dict(survey_assemblies(&managed)));
        }
    }
    rep.set("apk_log", strs(&lines));
    let (mut notes, mut needs): (Vec<String>, Vec<String>) = Default::default();
    if let Some(info) = &info {
        let v = UV::Version::parse(info.str("unity_version")).map_err(port::Error::Fail)?;
        let g = |k: &str| info.get(k).cloned().unwrap_or(Val::Null);
        for (k, x) in [("package", g("package")), ("version_code", g("version_code")), ("version_name", g("version_name")),
                       ("backend", g("backend")), ("unity_version", g("unity_version")), ("unity_line", s(format!("{}.{}", v.major, v.minor))),
                       ("unity_supported", Val::Bool(true)), ("abis", g("abis")), ("graphics_apis", g("graphics_apis")),
                       ("product", g("product_name")), ("size_mb", info.get("size_mb").cloned().unwrap_or(Val::Int(0))),
                       ("apk_info", Val::Dict(info.clone()))] {
            rep.set(k, x);
        }
        route.set("ok", Val::Bool(true));
        let abis = str_list(info, "abis");
        let mono = info.str("backend") == "mono";
        if abis == ["armeabi-v7a"] {
            needs.push("armeabi-v7a only: a system with a 32-bit (armhf) userland (ua_host); not 64-bit-only ones (ROCKNIX, ...)".into());
            if mono {
                notes.push(match apk::il2cpp::il2cpp_refusal(&v) {
                    Some(why) => format!("64-bit-only systems: {why}"),
                    None => "64-bit-only systems too: player.il2cpp = true has the setup compile the build's code with Unity's \
                             IL2CPP on such a handheld, for Unity's arm64 IL2CPP player (`pmunity apk --il2cpp --on-device`: one to \
                             a few hours, about 1.6 GB free, Wi-Fi; the release then ships ua_host64 too)".to_string(),
                });
            }
        } else if abis == ["arm64-v8a"] {
            needs.push("arm64-v8a only: the 64-bit ua_host (ua_host64), on any aarch64 system".into());
        } else {
            needs.push("armeabi-v7a (ua_host) where the system has a 32-bit userland, arm64-v8a (ua_host64) on 64-bit-only systems: \
                        the release ships both".into());
        }
        needs.push("nothing converted or downloaded: the setup installs the player's APK and OBBs (a few minutes; about the build's \
                    size free on the card)".into());
        notes.extend(str_list(info, "notes"));
        if info.str("data_in") != "apk" {
            notes.push(format!("the Unity data is in {}: players need it too (the .obb beside the APK, or the .xapk / .apks / .aab with \
                                everything)", if info.str("data_in") == "obb" { "the OBB" } else { "an asset pack" }));
        }
        if info.truthy("plugins") {
            notes.push(format!("native plugins: {} (the build's own ARM libraries; those that only call into Java SDKs see a phone \
                                without them)", str_list(info, "plugins").join(", ")));
        }
        if matches!(info.str("orientation"), "portrait" | "portrait upside down") {
            notes.push("made for portrait screens: Unity gets the handheld's landscape window (the game decides how it fits; \
                        setRequestedOrientation is ignored)".into());
        }
        let mut sug = rep.dict("suggest").clone();
        sug.set("player", vdict(vec![("backend", g("backend")), ("abis", strs(&abis))]));
        sug.set("game", vdict(vec![("package", g("package"))]));
        sug.table("runtime");
        if let Some(sv) = rep.get("code").and_then(Val::as_dict).cloned() {
            let verdict = input_verdict(&sv);
            rep.set("input_verdict", s(verdict));
            let lg = sv.dict("legacy");
            let touch_only = int(lg, "touch") > 0 && matches!(verdict, "none" | "pad")
                && !(lg.truthy("axes/buttons") || sv.truthy("rewired") || sv.truthy("incontrol") || sv.truthy("selects") || sv.truthy("navigation"));
            if touch_only {
                sug.table("runtime").set("UA_POINTER", s("touch"));
                notes.push("reads touches (Input.touches) and nothing of a pad: pointer mode with touch events, UA_POINTER=touch \
                            (docs/input.md)".into());
            } else if verdict == "pointer" {
                sug.table("runtime").set("UA_POINTER", s("1"));
                notes.push("menus look pointer-driven (mouse or UI pointer handlers, no UI selection for a pad): pointer mode, \
                            UA_POINTER=1".into());
            }
            if sv.truthy("rewired") || sv.truthy("incontrol") {
                if !sug.table("runtime").contains("UA_PAD_NAME") {
                    sug.table("runtime").set("UA_PAD_NAME", s("Xbox Wireless Controller"));
                }
                notes.push(format!("{} knows Android pads by name: UA_PAD_NAME names the pad as one of its Android profiles does",
                                   if sv.truthy("rewired") { "Rewired" } else { "InControl" }));
            }
            if sv.truthy("text_input") {
                notes.push(format!("text input ({}): ua_host's on-screen keyboard opens for Unity's soft keyboard; UA_OSK = \"done\" \
                                    where the text does not matter", str_list(&sv, "text_input").join(", ")));
            }
        } else {
            notes.push("IL2CPP: the code is not surveyed (no assemblies); a touch-only game needs UA_POINTER=touch, mouse-driven \
                        menus UA_POINTER=1 (docs/input.md)".into());
        }
        rep.set("suggest", Val::Dict(sug));
        let size = int(&rep, "size_mb");
        if size > 600 {
            notes.push(format!("{size} MB: the setup needs about that much free on the card (exFAT/FAT: the files are copied; ext4: linked)"));
        }
    }
    for n in needs {
        push(&mut route, "needs", s(n));
    }
    for n in notes {
        push(&mut rep, "notes", s(n));
    }
    // (the hook modules' port_check leave an Android build as it is: no data folder)
    let ok_any = route.truthy("ok");
    let why = route.list("why").first().map(Val::py_str);
    rep.set("routes", Val::List(vec![Val::Dict(route)]));
    rep.set("route", if ok_any { s("apk") } else { Val::Null });
    if !ok_any {
        rep.set("unsupported", s(why.unwrap_or_else(|| "not a build pmunity apk reads".into())));
    }
    Ok(rep)
}
