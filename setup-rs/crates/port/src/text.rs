//! The files `port new` writes from a resolved config (setup.env, port.env, questions.lua,
//! setup.txt, port.json, gameinfo.xml, the testing thread's post, the READMEs' values) and the
//! checks `port new` and `port package` print for a PortMaster submission (port.py).
use std::path::Path;

use crate::config::{opt, opt_len, PORT_GENRES};
use crate::dict;
use crate::rx::{self, shquote as q};
use crate::val::{Dict, Val};

pub const QUIT_KEYS: &str = "Select + Start";
pub const BIG_FILE_MB: u64 = 90;
pub const TEST_CFWS: [&str; 8] = [
    "[ ] AmberELEC", "[ ] dArkOS", "[ ] MuOS", "ROCKNIX", "-> [ ] Libmali", "-> [ ] Panfrost", "-> [ ] Adreno (Optional)",
    "[ ] Knulli",
];
pub const TEST_RESOLUTIONS: [&str; 4] = [
    "[ ] 480x320 (Optional)", "[ ] 640x480", "[ ] 720x720 (RGB30) (Optional)", "[ ] Higher resolutions (e.g., 1280)",
];

pub fn route_text(route: &str) -> Option<&'static str> {
    Some(match route {
        "android" => "Unity's Android player (armv7, Mono) on the handheld's 32-bit userland: portmaster-unity's runtime",
        "il2cpp-arm64" => "the game's code compiled with IL2CPP on the handheld, on Unity's arm64 IL2CPP Android player (for 64-bit-only systems)",
        "mono-arm64" => "the game's own assemblies on Unity's Mono for aarch64 Linux (shipped with the port), in Unity's arm64 Android player (for 64-bit systems; no IL2CPP compile)",
        "box64" => "the game's own x86-64 Linux player under box64 (tools/box64, `pmunity route`)",
        "apk" => "the game's own Android build (`pmunity apk`)",
        _ => return None,
    })
}

pub fn game_forms(build: &str) -> &'static str {
    match build {
        "macos" => "the `.app`, the archive it came in, or GOG's macOS installer `.pkg`",
        "windows" => "its folder, or the archive it came in",
        _ => "its folder, the archive it came in, or GOG's Linux installer `.sh`",
    }
}

pub fn route_needs_text(route: &str) -> Option<&'static str> {
    Some(match route {
        "android" => "a handheld with a 32-bit (armhf) userland beside its 64-bit system (e.g. Knulli on Allwinner H700 devices)",
        "il2cpp-arm64" | "mono-arm64" => "a 64-bit (aarch64) handheld",
        "box64" => "a 64-bit (aarch64) handheld with OpenGL ES 3, and PortMaster's Westonpack (downloaded on the first launch)",
        "apk" => "a 64-bit (aarch64) handheld with OpenGL ES 3, and a 32-bit (armhf) userland beside its 64-bit system (e.g. Knulli) for a game whose APK has only its 32-bit player",
        _ => return None,
    })
}

pub fn player_on(route: &str) -> &'static str {
    match route {
        "mono-arm64" => "Unity's 64-bit (arm64) Android player, its own code on Unity's Mono for 64-bit ARM, run natively",
        "il2cpp-arm64" => "Unity's 64-bit (arm64) IL2CPP Android player, its code compiled with IL2CPP, run natively",
        _ => "Unity's own Android player, run natively on the handheld's 32-bit userland",
    }
}

pub fn handheld_needs(route: &str) -> &'static str {
    match route {
        "mono-arm64" | "il2cpp-arm64" => "A 64-bit (aarch64) handheld with GLES 3 (no 32-bit userland needed).",
        _ => "A handheld with a 32-bit (armhf) userland and GLES 3 (e.g. Knulli on Allwinner H700 devices),\n  and a 64-bit (aarch64) system for the setup's tools.",
    }
}

fn route_setup(key: &str) -> Option<&'static str> {
    Some(match key {
        "il2cpp-arm64" => "The first launch also compiles the game's code with Unity's IL2CPP on the handheld: one to a few hours, about 1.6 GB free, Wi-Fi for Unity's and Debian's files.",
        "apk+il2cpp" => "On a handheld without a 32-bit userland the first launch compiles the game's code with Unity's IL2CPP for Unity's 64-bit player: one to a few hours, about 1.6 GB free, Wi-Fi for Unity's and Debian's files.",
        _ => return None,
    })
}

fn build_step(c: &str) -> Option<&'static str> {
    Some(match c {
        "ua_host" => "runtime/build.sh                                            # ua_host, the 32-bit runtime (Docker)",
        "ua_host_arm64" => "ARCH=arm64 runtime/build.sh                                 # ua_host, the 64-bit runtime (Docker)",
        "ua_host64" => "ARCH=arm64 runtime/build.sh                                 # ua_host64, the 64-bit runtime (Docker)",
        "pybundle" => "tools/port/build_pybundle.sh tools/port/out/python.tar.gz   # the setup's Python, aarch64 (Docker)",
        "pmu" => "setup-rs/tools/build_pmu.sh tools/port/out/pmu               # the setup's tools, pmu, aarch64 (Docker)",
        "asmpatch" => "tools/asmpatch/build.sh                                     # asmpatch (.NET 8 SDK)",
        "pmhlslcc" => "ARCH=arm64 tools/hlslcc/build.sh                            # pmhlslcc, HLSLcc for the setup, aarch64 (Docker)",
        "pmspirvcross" => "ARCH=arm64 tools/spirvcross/build.sh                        # pmspirvcross, SPIRV-Cross for the setup, aarch64 (Docker)",
        "box64" => "tools/box64/build.sh                                        # box64, gl4es, glxes (Docker)",
        "mono64" => "runtime/mono64/build.sh; tools/mono64/build-mono.sh <line> tools/mono64/work  # the Mono bridge and Unity's Mono, aarch64 (Docker)",
        _ => return None,
    })
}

pub const STANDARD_HEADER: &str = "XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d \"/opt/system/Tools/PortMaster/\" ]; then
  controlfolder=\"/opt/system/Tools/PortMaster\"
elif [ -d \"/opt/tools/PortMaster/\" ]; then
  controlfolder=\"/opt/tools/PortMaster\"
elif [ -d \"$XDG_DATA_HOME/PortMaster/\" ]; then
  controlfolder=\"$XDG_DATA_HOME/PortMaster\"
else
  controlfolder=\"/roms/ports/PortMaster\"
fi

source $controlfolder/control.txt
[ -f \"${controlfolder}/mod_${CFW_NAME}.txt\" ] && source \"${controlfolder}/mod_${CFW_NAME}.txt\"
get_controls
";

/// q(v): shlex.quote(str(v))
fn qv(v: Option<&Val>) -> String {
    q(&v.map_or("None".to_string(), Val::py_str))
}

fn s(v: Option<&Val>) -> String {
    v.map_or("None".to_string(), Val::py_str)
}

/// "files/<basename>" of each of a list's paths, quoted.
fn files_array(l: &[Val]) -> String {
    l.iter().map(|x| q(&format!("files/{}", pkgzip::py::basename(&x.py_str())))).collect::<Vec<_>>().join(" ")
}

/// setup/setup.env: everything the setup reads, as shell assignments; sections: the port.toml
/// sections the route uses (route.json "config"; None: all).
pub fn setup_env(cfg: &Dict, extra: &[String], sections: Option<&[Val]>) -> String {
    let (p, g, pl, ad, fl) = (cfg.dict("port"), cfg.dict("game"), cfg.dict("player"), cfg.dict("android_data"), cfg.dict("files"));
    let has = |k: &str| sections.is_none_or(|s| s.iter().any(|x| x.eq(&Val::s(k))));
    let a = ad.get("asmpatch");
    let int0 = |k: &str| Val::int_or0(g.get(k)).unwrap_or(0);
    let mut lines: Vec<String> = vec![
        "# Written by `pmunity port new` from port.toml: the setup's settings (sourced by patchscript).".into(),
        format!("PORT_NAME={}", qv(p.get("name"))),
        format!("PORT_TITLE={}", qv(p.get("title"))),
        format!("PORT_ROUTE={}", qv(p.get("route"))),
        format!("GAME_DATA={}", qv(g.get("data"))),
        format!("GAME_BUILD={}", qv(g.get("build"))),
        format!("GAME_WHAT={}", qv(g.get("what"))),
        format!("GAME_SIZE_MB={}", int0("size_mb")),
        format!("SETUP_SPACE_MB={}", int0("setup_space_mb")),
    ];
    if g.truthy("package") {
        lines.push(format!("GAME_PACKAGE={}", qv(g.get("package"))));
    }
    let ready = p.get("ready_marker").filter(|v| v.truthy()).map_or(".setup_done".into(), Val::py_str);
    let join = |l: &[Val]| l.iter().map(Val::py_str).collect::<Vec<_>>().join(" ");
    let mut sha: Vec<(String, String)> = pl.dict("sha256").iter().map(|(k, v)| (k.to_string(), v.py_str())).collect();
    sha.sort();
    lines.extend([
        format!("READY={}", q(&ready)),
        format!("UNITY_VERSIONS={}", q(&join(g.list("unity_versions")))),
        format!("PLAYER_BACKEND={}", qv(pl.get("backend"))),
        format!("PLAYER_ABI={}", qv(pl.get("abi"))),
        format!("DONORS={}", q(&join(pl.list("donors")))),
        format!("PLAYER_SHA256={}", q(&sha.iter().map(|(k, v)| format!("{k}={v}")).collect::<Vec<_>>().join(" "))),
    ]);
    if pl.truthy("il2cpp") {
        lines.push("PLAYER_IL2CPP=1".into());
    }
    if has("android_data") {
        let asm = match a {
            Some(Val::List(l)) => l.iter().map(Val::py_str).collect::<Vec<_>>().join(","),
            x => s(x),
        };
        let or = |k: &str, d: &str| ad.get(k).filter(|v| v.truthy()).map_or(d.to_string(), Val::py_str);
        lines.extend([
            format!("APK_NAME={}", qv(ad.get("name"))),
            format!("PACKAGE={}", qv(ad.get("package"))),
            format!("ASMPATCH={}", q(&asm)),
            format!("VIDEO_SHADER={}", if ad.get("video_shader").is_none_or(Val::truthy) { 1 } else { 0 }),
            format!("SWAPPY={}", q(&or("swappy", "off"))),
            format!("STREAMING_ASSETS={}", q(&or("streaming_assets", "obb"))),
            format!("DATA_PATH={}", if ad.truthy("data_path") { 1 } else { 0 }),
            format!("WINBCL={}", q(&or("winbcl", "auto"))),
            format!("ANDROID_DATA_ARGS=({})", ad.list("args").iter().map(|x| q(&x.py_str())).collect::<Vec<_>>().join(" ")),
        ]);
    }
    if has("convert") {
        lines.push(format!("CONVERT_PLUGINS=({})", files_array(fl.list("plugins"))));
        let banks = |sec: &str| cfg.dict(sec).get("banks").filter(|v| v.truthy()).map_or(String::new(), Val::py_str);
        lines.push(format!("WWISE_BANKS={}", q(&banks("wwise"))));
        lines.push(format!("FMOD_BANKS={}", q(&banks("fmod"))));
    }
    if has("android_data") {
        lines.push(format!("MANAGED_FILES=({})", files_array(fl.list("managed"))));
        let aps: Vec<String> = fl
            .list("assembly_patches")
            .iter()
            .map(|ap| {
                let d = ap.as_dict().cloned().unwrap_or_default();
                format!("{} {}", qv(d.get("name")), qv(d.get("command")))
            })
            .collect();
        lines.push(format!("ASSEMBLY_PATCHES=({})", aps.join(" ")));
    }
    let qs = cfg.list("questions");
    let ids: Vec<String> = qs.iter().map(|x| s(x.as_dict().and_then(|d| d.get("id")))).collect();
    lines.push(format!("QUESTIONS={}", q(&ids.join(" "))));
    for x in qs {
        let d = x.as_dict().cloned().unwrap_or_default();
        let i = s(d.get("id"));
        let opts = d.list("options");
        let o0: Vec<String> = opts.iter().map(|o| s(opt(o, 0).as_ref())).collect();
        let default = match d.get("default").filter(|v| v.truthy()) {
            Some(v) => v.py_str(),
            None => opts.first().and_then(|o| opt(o, 0)).map_or("None".into(), |v| v.py_str()),
        };
        let or = |k: &str| d.get(k).filter(|v| v.truthy()).map_or(String::new(), Val::py_str);
        lines.extend([
            format!("Q_{i}_OPTIONS={}", q(&o0.join(" "))),
            format!("Q_{i}_DEFAULT={}", q(&default)),
            format!("Q_{i}_SET={}", q(&or("convert_set"))),
            format!("Q_{i}_ENV={}", q(&or("env"))),
        ]);
    }
    lines.extend(extra.iter().cloned());
    lines.join("\n") + "\n"
}

/// <port>/port.env: ua_host's environment, sourced by the launcher.
pub fn port_env(cfg: &Dict) -> String {
    let mut lines: Vec<String> = vec![
        "# The game's runtime options (ua_host: docs/runtime.md and docs/input.md in portmaster-unity),".into(),
        "# sourced by the launcher. Edit to taste; a variable already set in the environment wins.".into(),
    ];
    for (k, v) in cfg.dict("runtime").iter() {
        lines.push(format!("[ -n \"${{{k}+x}}\" ] || export {k}={}", q(&v.py_str())));
    }
    let pic = cfg.dict("runtime_low");
    if !pic.is_empty() {
        lines.extend([
            "# the Low picture on every handheld, the same game; UA_PICTURE=high set above keeps the picture".into(),
            "# as on PC (set a variable above, or in the environment, and that wins)".into(),
            "case \"${UA_PICTURE:-low}\" in high|full) ;; *)".into(),
        ]);
        for (k, v) in pic.iter() {
            lines.push(format!("  [ -n \"${{{k}+x}}\" ] || export {k}={}", q(&v.py_str())));
        }
        lines.push(";; esac".into());
    }
    let low = cfg.dict("runtime_lowmem");
    if !low.is_empty() {
        lines.extend([
            "# handhelds with 1 GB of RAM or less, either picture (set a variable above, or in the".into(),
            "# environment, e.g. to \"\", and that wins)".into(),
            "mem_mb=$(awk '/MemTotal/{print int($2/1024)}' /proc/meminfo 2>/dev/null)".into(),
            "if [ \"${mem_mb:-0}\" -gt 0 ] && [ \"${mem_mb:-0}\" -le 1024 ]; then".into(),
        ]);
        for (k, v) in low.iter() {
            lines.push(format!("  [ -n \"${{{k}+x}}\" ] || export {k}={}", q(&v.py_str())));
        }
        lines.push("fi".into());
    }
    lines.join("\n") + "\n"
}

pub fn lua_str(v: &str) -> String {
    format!("\"{}\"", v.replace('\\', "\\\\").replace('"', "\\\"").replace('\n', "\\n"))
}

pub fn questions_lua(qs: &[Val]) -> String {
    let mut out: Vec<String> = vec![
        "-- Asked by PortMaster's patcher before the setup starts (PATCHER_QUESTIONS in the launcher);".into(),
        "-- each answer reaches setup/patchscript as <id>=<value> in its environment.".into(),
        "return {".into(),
    ];
    for x in qs {
        let d = x.as_dict().cloned().unwrap_or_default();
        out.extend([
            "  {".into(),
            format!("    id = {},", lua_str(&s(d.get("id")))),
            format!("    question = {},", lua_str(&s(d.get("question")))),
            "    options = {".into(),
        ]);
        for o in d.list("options") {
            let a = s(opt(o, 0).as_ref());
            let b = if opt_len(o) > 1 { s(opt(o, 1).as_ref()) } else { a.clone() };
            out.push(format!("      {{ {}, {} }},", lua_str(&a), lua_str(&b)));
        }
        out.extend(["    },".into(), "  },".into()]);
    }
    out.push("}".into());
    out.join("\n") + "\n"
}

pub fn setup_txt(cfg: &Dict) -> String {
    let mut out = vec![format!(
        "# {} setup options, read once by the first-launch setup.",
        s(cfg.dict("port").get("title"))
    )];
    let qs = cfg.list("questions");
    if !qs.is_empty() {
        out.push("# PortMaster's setup screen asks the questions below itself; this file answers them when it".into());
        out.push("# does not (older PortMaster versions).".into());
    }
    for x in qs {
        let d = x.as_dict().cloned().unwrap_or_default();
        out.push("#".into());
        out.push(format!("# {}", s(d.get("question"))));
        let opts = d.list("options");
        for o in opts {
            let b = if opt_len(o) > 1 { s(opt(o, 1).as_ref()) } else { String::new() };
            out.push(format!("#   {}  {b}", s(opt(o, 0).as_ref())));
        }
        let default = match d.get("default").filter(|v| v.truthy()) {
            Some(v) => v.py_str(),
            None => opts.first().and_then(|o| opt(o, 0)).map_or("None".into(), |v| v.py_str()),
        };
        out.push(format!("{}={default}", s(d.get("id")).to_lowercase()));
    }
    out.extend([
        "#".into(),
        "# keep_installers: 1 keeps the archives or installers you put in gamedata/ after the setup;".into(),
        "#   by default the setup deletes them once the game is built (they are not needed to play)".into(),
        "keep_installers=0".into(),
    ]);
    out.join("\n") + "\n"
}

/// port.json (version 4, the fields and order of PortMaster's port.json generator).
pub fn port_json(cfg: &Dict) -> Val {
    let (p, g) = (cfg.dict("port"), cfg.dict("game"));
    let title = s(p.get("title"));
    let name = s(p.get("name"));
    let mut reqs: Vec<Val> = if p.truthy("aarch64_only") { vec![] } else { vec![Val::s("armhf")] };
    reqs.extend(p.list("reqs").iter().filter(|r| !r.eq(&Val::s("armhf"))).cloned());
    let or_none = |k: &str| p.get(k).filter(|v| v.truthy()).cloned().unwrap_or(Val::Null);
    let desc = p.get("desc").filter(|v| v.truthy()).cloned().unwrap_or_else(|| Val::Str(format!("{title} on Unity's Android player.")));
    let inst = p.get("inst").filter(|v| v.truthy()).cloned().unwrap_or_else(|| {
        Val::Str(format!(
            "Requires {}: put it in ports/{name}/gamedata. The first launch builds the game on the handheld ({}; Unity's Android player comes with the port, no Wi-Fi).",
            s(g.get("what")),
            s(p.get("setup_time"))
        ))
    });
    let store: Vec<Val> = p
        .list("store")
        .iter()
        .map(|st| {
            let d = st.as_dict().cloned().unwrap_or_default();
            let f = |k: &str| d.get(k).filter(|v| v.truthy()).cloned().unwrap_or(Val::s(""));
            Val::Dict(dict! { "name" => f("name"), "gameurl" => f("gameurl"), "developerurl" => f("developerurl") })
        })
        .collect();
    let availability = p.get("availability").filter(|v| v.truthy()).cloned().unwrap_or_else(|| {
        Val::s(if p.list("store").is_empty() { "free" } else { "paid" })
    });
    Val::Dict(dict! {
        "version" => Val::Int(4),
        "name" => Val::Str(format!("{name}.zip")),
        "items" => Val::List(vec![Val::Str(format!("{title}.sh")), p.get("name").cloned().unwrap_or(Val::Null)]),
        "items_opt" => Val::List(vec![]),
        "attr" => Val::Dict(dict! {
            "title" => p.get("title").cloned().unwrap_or(Val::Null),
            "porter" => Val::List(p.list("porter").to_vec()),
            "desc" => desc,
            "desc_md" => or_none("desc_md"),
            "inst" => inst,
            "inst_md" => or_none("inst_md"),
            "genres" => Val::List(p.list("genres").iter().map(|x| Val::Str(rx::casefold(&x.py_str()))).collect()),
            "image" => Val::Null,
            "rtr" => Val::Bool(false),
            "exp" => Val::Bool(p.truthy("exp")),
            "runtime" => Val::List(vec![]),
            "store" => Val::List(store),
            "availability" => availability,
            "reqs" => Val::List(reqs),
            "arch" => Val::strs(&["aarch64"]),
            "min_glibc" => Val::s(""),
        }),
    })
}

/// [(button, action)]: [controls] in order, then PortMaster's exit hotkey unless a row has it.
pub fn controls_rows(cfg: &Dict) -> Vec<(String, String)> {
    let mut rows: Vec<(String, String)> = cfg
        .dict("controls")
        .iter()
        .map(|(k, v)| (pkgzip::py::strip(k).to_string(), pkgzip::py::strip(&v.py_str()).to_string()))
        .collect();
    if !rows.iter().any(|(k, _)| {
        let l = k.to_lowercase();
        l.contains("select") && l.contains("start")
    }) {
        rows.push((QUIT_KEYS.into(), "Quit".into()));
    }
    rows
}

pub fn controls_markdown(cfg: &Dict) -> String {
    let cell = |s: &str| s.replace('|', "\\|");
    let mut t = vec!["| Button | Action |".to_string(), "|--|--|".to_string()];
    t.extend(controls_rows(cfg).iter().map(|(k, v)| format!("| {} | {} |", cell(k), cell(v))));
    let table = t.join("\n");
    if cfg.truthy("controls") {
        return table;
    }
    format!("The game reads the handheld's pad as a gamepad and keeps its own gamepad controls.\n\n{table}")
}

fn store_field(cfg: &Dict, k: &str) -> String {
    cfg.dict("port")
        .list("store")
        .iter()
        .filter_map(Val::as_dict)
        .find_map(|d| d.get(k).filter(|v| v.truthy()).map(Val::py_str))
        .unwrap_or_default()
}

pub fn thanks_sentence(cfg: &Dict) -> String {
    let (p, g) = (cfg.dict("port"), cfg.dict("game"));
    if p.truthy("thanks") {
        return pkgzip::py::strip(&s(p.get("thanks"))).to_string();
    }
    let dev = pkgzip::py::strip(&g.get("developer").filter(|v| v.truthy()).map_or(String::new(), Val::py_str)).to_string();
    let url = store_field(cfg, "developerurl");
    let title = s(p.get("title"));
    if dev.is_empty() {
        return format!("Thanks to the makers of {title} for creating it.");
    }
    let who = if url.is_empty() { dev } else { format!("[{dev}]({url})") };
    format!("Thanks to {who} for creating {title}.")
}

/// Whether the setup takes Unity's Unix class libraries from the port's Mono donor: a Windows
/// build of Unity 2021.2+ (or of versions not named) on the android route.
pub fn needs_winbcl(cfg: &Dict) -> bool {
    let g = cfg.dict("game");
    if g.str("build") != "windows"
        || cfg.dict("port").str("route") != "android"
        || cfg.dict("android_data").get("winbcl").is_some_and(|w| w.eq(&Val::s("off")))
    {
        return false;
    }
    let vs = g.list("unity_versions");
    vs.is_empty()
        || vs.iter().any(|v| {
            let t = v.py_str();
            let mut it = t.split('.');
            let major: u32 = it.next().and_then(|x| x.parse().ok()).unwrap_or(0);
            let minor: u32 = it.next().and_then(|x| x.parse().ok()).unwrap_or(0);
            (major, minor) >= (2021, 2)
        })
}

pub fn player_from(cfg: &Dict, route: &Dict) -> String {
    if route.truthy("donors") {
        let more = if needs_winbcl(cfg) {
            " A Windows build of Unity 2021.2+ comes with Windows-only class libraries: the setup takes the Unix ones from the same donors."
        } else {
            ""
        };
        return format!("Unity's Android player comes with the port (`donor/`: small games built with Unity for it, whose APKs carry the player); nothing is downloaded, no Wi-Fi.{more}");
    }
    if cfg.dict("port").str("route") == "il2cpp-arm64" {
        return "Wi-Fi on the first launch: the setup downloads Unity's IL2CPP tools and its arm64 player, and Debian's compilers and .NET (about 160 MB), to compile the game's code on the handheld.".into();
    }
    String::new()
}

pub fn route_needs(route: &str, cfg: &Dict) -> String {
    let mut out: Vec<String> = route_needs_text(route).map(|n| format!("It needs {n}.")).into_iter().collect();
    if route == "apk" && cfg.dict("player").truthy("il2cpp") {
        out.push(route_setup("apk+il2cpp").unwrap().into());
        return out.join(" ");
    }
    if let Some(x) = route_setup(route) {
        out.push(x.into());
    }
    out.join(" ")
}

pub fn build_steps(cfg: &Dict, route: &Dict) -> String {
    let mut out = Vec::new();
    for c in route.list("package") {
        let c = c.py_str();
        let opt = c.ends_with('?');
        let c = c.trim_end_matches('?');
        if (opt || c == "asmpatch") && !wants(c, cfg) {
            continue;
        }
        if let Some(x) = build_step(c) {
            out.push(x);
        }
    }
    out.join("\n")
}

pub fn game_url(cfg: &Dict) -> String {
    let g = cfg.dict("game");
    if g.truthy("url") {
        return s(g.get("url"));
    }
    store_field(cfg, "gameurl")
}

pub fn image_ext(rel: &str) -> Option<&'static str> {
    match rx::splitext_ext(rel).to_lowercase().as_str() {
        ".png" => Some(".png"),
        ".jpg" | ".jpeg" => Some(".jpg"),
        _ => None,
    }
}

/// {'screenshot': (file, shipped name) or None, 'cover': ...}
pub fn port_images(cfg: &Dict, port_src: &str) -> [(&'static str, Option<(String, String)>); 2] {
    let p = cfg.dict("port");
    let one = |k: &'static str| -> Option<(String, String)> {
        let rel = match p.get(k) {
            None => format!("{k}.png"),
            Some(v) if !v.truthy() => String::new(),
            Some(v) => v.py_str(),
        };
        if rel.is_empty() {
            return None;
        }
        let path = pkgzip::py::join(port_src, &rel);
        let ext = image_ext(&rel)?;
        Path::new(&path).is_file().then(|| (path, format!("{k}{ext}")))
    };
    [("screenshot", one("screenshot")), ("cover", one("cover"))]
}

pub fn gameinfo_image(cfg: &Dict, port_src: &str) -> String {
    let imgs = port_images(cfg, port_src);
    if let Some((_, n)) = &imgs[1].1 {
        return n.clone();
    }
    let shot = cfg.dict("port").get("screenshot").filter(|v| v.truthy()).map_or(".png".into(), Val::py_str);
    format!("screenshot{}", image_ext(&shot).unwrap_or(".png"))
}

fn xml_escape(v: &str) -> String {
    v.replace('&', "&amp;").replace('>', "&gt;").replace('<', "&lt;")
}

pub fn gameinfo_xml(cfg: &Dict, image: &str, desc: Option<&Val>) -> String {
    let (p, g) = (cfg.dict("port"), cfg.dict("game"));
    let genres = p.list("genres");
    let genre = if g.truthy("genre") {
        s(g.get("genre"))
    } else if let Some(first) = genres.first() {
        let f = first.py_str();
        match rx::casefold(&f).as_str() {
            "casino/card" => "Casino/Card".into(),
            "fps" => "FPS".into(),
            "rpg" => "RPG".into(),
            "visual novel" => "Visual Novel".into(),
            _ => rx::title(&f),
        }
    } else {
        String::new()
    };
    let date = g.get("release_date").filter(|v| v.truthy()).map(Val::py_str).unwrap_or_default();
    let date = rx::date(&date).map_or(String::new(), |(y, m, d)| format!("{y}{m}{d}T000000"));
    let dsc = [g.get("description"), p.get("desc"), desc]
        .into_iter()
        .flatten()
        .find(|v| v.truthy())
        .map_or(String::new(), Val::py_str);
    let or = |d: &Dict, k: &str| d.get(k).filter(|v| v.truthy()).map_or(String::new(), Val::py_str);
    let fields = [
        ("path", format!("./{}.sh", s(p.get("title")))),
        ("name", s(p.get("title"))),
        ("desc", pkgzip::py::strip(&dsc).to_string()),
        ("releasedate", date),
        ("developer", or(g, "developer")),
        ("publisher", or(g, "publisher")),
        ("genre", genre),
        ("image", format!("./{}/{image}", s(p.get("name")))),
    ];
    let mut out = vec!["<?xml version=\"1.0\" encoding=\"utf-8\"?>".to_string(), "<gameList>".into(), "  <game>".into()];
    for (k, v) in fields {
        if !v.is_empty() {
            out.push(format!("    <{k}>{}</{k}>", xml_escape(&v)));
        }
    }
    out.extend(["  </game>".into(), "</gameList>".into()]);
    out.join("\n") + "\n"
}

pub fn testing_thread_txt(cfg: &Dict, inst: &str) -> String {
    let p = cfg.dict("port");
    let url = game_url(cfg);
    let mut out: Vec<String> = vec![
        "How to install the test zip:".into(),
        format!("Put {}.zip into PortMaster's autoinstall folder, then start PortMaster.", s(p.get("name"))),
        "Or over SSH: harbourmaster install \"<the link to the zip on Discord>\"".into(),
        String::new(),
        "Game information".into(),
        format!("Title: {}", s(p.get("title"))),
        format!("URL: {}", if url.is_empty() { "(port.toml: game.url)".to_string() } else { url }),
        String::new(),
        "Instructions:".into(),
        pkgzip::py::strip(inst).to_string(),
    ];
    if let Some(n) = route_needs_text(&s(p.get("route"))) {
        out.push(format!("Needs {n}."));
    }
    out.extend([String::new(), "| Button | Action |".into(), "|--|--|".into()]);
    out.extend(controls_rows(cfg).iter().map(|(k, v)| format!("| {k} | {v} |")));
    out.extend([String::new(), "CFW Tests:".into()]);
    out.extend(TEST_CFWS.iter().map(|x| x.to_string()));
    out.extend([String::new(), "Resolutions:".into()]);
    out.extend(TEST_RESOLUTIONS.iter().map(|x| x.to_string()));
    out.join("\n") + "\n"
}

/// (width, height) of a PNG or JPEG file.
pub fn image_size(path: &str) -> Option<(u32, u32)> {
    use std::io::Read;
    let mut d = Vec::new();
    std::fs::File::open(path).ok()?.take(1 << 16).read_to_end(&mut d).ok()?;
    if d.len() >= 24 && d.starts_with(b"\x89PNG\r\n\x1a\n") && &d[12..16] == b"IHDR" {
        let be = |i: usize| u32::from_be_bytes(d[i..i + 4].try_into().unwrap());
        return Some((be(16), be(20)));
    }
    if d.starts_with(b"\xff\xd8") {
        let mut i = 2;
        while i + 9 < d.len() && d[i] == 0xFF {
            let m = d[i + 1];
            let ln = u16::from_be_bytes([d[i + 2], d[i + 3]]) as usize;
            if matches!(m, 0xC0 | 0xC1 | 0xC2 | 0xC3 | 0xC5 | 0xC6 | 0xC7 | 0xC9 | 0xCA | 0xCB | 0xCD | 0xCE | 0xCF) {
                let h = u16::from_be_bytes([d[i + 5], d[i + 6]]) as u32;
                let w = u16::from_be_bytes([d[i + 7], d[i + 8]]) as u32;
                return Some((w, h));
            }
            i += 2 + ln;
        }
    }
    None
}

pub fn launcher_warnings(text: &str) -> Vec<String> {
    let mut w = Vec::new();
    let lines: Vec<&str> = text.split('\n').collect();
    let mut body = &lines[1..];
    if body.first().is_some_and(|l| l.starts_with("# PORTMASTER:")) {
        body = &body[1..];
    }
    if lines[0] != "#!/bin/bash" || !format!("{}", body.join("\n")).starts_with(&format!("\n{STANDARD_HEADER}")) {
        w.push("the launcher does not start with PortMaster's standard lines (#!/bin/bash, the PORTMASTER line, then the controlfolder block, source control.txt, the firmware's mod_${CFW_NAME}.txt, get_controls)".into());
    }
    if rx::quoted_gptokeyb(text) {
        w.push("\"$GPTOKEYB2\" is quoted: it is a command line with arguments, use it unquoted".into());
    }
    if rx::sets_videodriver(text) {
        w.push("the launcher sets SDL_VIDEODRIVER: leave the video driver to the firmware".into());
    }
    if !text.contains("pm_finish") {
        w.push("the launcher never calls pm_finish".into());
    }
    w
}

/// What PortMaster's reviewers would ask for that the port does not have yet.
pub fn submission_warnings(cfg: &Dict, port_src: &str, pj: Option<&Val>, launcher: Option<&str>, gameinfo: Option<&str>) -> Vec<String> {
    let (p, g) = (cfg.dict("port"), cfg.dict("game"));
    let mut w: Vec<String> = Vec::new();
    let empty = Dict::new();
    let a = pj.and_then(Val::as_dict).map(|d| d.dict("attr")).unwrap_or(&empty);
    let at = |k: &str| a.get(k).filter(|v| v.truthy()).map_or(String::new(), Val::py_str);
    if !p.truthy("porter") {
        w.push("port.porter is empty: port.json names the porters (as on PortMaster's Discord)".into());
    }
    if !p.truthy("desc") {
        let said = if a.truthy("desc") { format!(" (port.json says \"{}\")", at("desc")) } else { String::new() };
        w.push(format!("port.desc is empty{said}: describe the game in a sentence or two"));
    }
    for k in ["title", "desc", "inst"] {
        let t = at(k);
        if t.contains('\n') || rx::markdown(&t) {
            w.push(format!(
                "port.json {k}: plain text in one paragraph (the PortMaster app shows it as it is; Markdown goes into {k}_md)"
            ));
        }
    }
    let mut texts: Vec<Option<&Val>> = ["title", "desc", "inst", "desc_md", "inst_md"].iter().map(|k| a.get(k)).collect();
    texts.push(p.get("thanks"));
    texts.push(g.get("description"));
    let ctl: Vec<Val> = cfg.dict("controls").iter().map(|(_, v)| v.clone()).collect();
    texts.extend(ctl.iter().map(Some));
    if texts.iter().flatten().any(|t| matches!(t, Val::Str(s) if s.contains('\u{2014}'))) {
        w.push("an em dash in the port's texts: PortMaster's reviewers ask for a comma, colon or parentheses".into());
    }
    if !p.truthy("genres") {
        w.push(format!("port.genres is empty: one or more of {}", PORT_GENRES.join(", ")));
    }
    if p.truthy("exp") {
        w.push("port.exp = true hides the released port unless players show experimental ports (not a \"being tested\" flag)".into());
    }
    let name = s(p.get("name"));
    if name.contains(['_', '-']) {
        w.push(format!(
            "port.name {}: PortMaster's ports use a squashed lowercase name (no '-' or '_'); renaming an installed port moves its saves, so decide before the first release",
            p.get("name").map_or("None".into(), Val::repr)
        ));
    }
    for (k, what) in [("developer", "developer"), ("publisher", "publisher"), ("release_date", "release date (YYYY-MM-DD)")] {
        if !g.truthy(k) {
            w.push(format!("game.{k} is empty: gameinfo.xml gives the frontend the game's {what}"));
        }
    }
    if !p.truthy("thanks") {
        w.push(format!(
            "port.thanks is empty: the README starts with \"{}\"; thank the game's makers in your own sentence (a link, one thing that makes the game worth playing)",
            thanks_sentence(cfg)
        ));
    }
    if !cfg.truthy("controls") {
        w.push("[controls] is empty: the README and the testing thread list only the exit hotkey; list the game's controls as the handheld labels its buttons".into());
    }
    if game_url(cfg).is_empty() {
        w.push("game.url and port.store are empty: no link to the game's page (the testing thread's URL)".into());
    } else if !p.truthy("store") {
        w.push("port.store is empty: where to get the game ([{name, gameurl, developerurl}])".into());
    }
    let imgs = port_images(cfg, port_src);
    match &imgs[0].1 {
        None => {
            let shot = p.get("screenshot").filter(|v| v.truthy()).map_or("screenshot.png".into(), Val::py_str);
            w.push(format!(
                "no screenshot ({}): PortMaster requires one, a real capture of the game on a handheld (4:3, at least 640x480): tools/device/drive.sh or `pmunity port shot`",
                pkgzip::py::join(port_src, &shot)
            ));
        }
        Some((path, _)) => match image_size(path) {
            None => w.push(format!("{path}: not a PNG or JPEG file")),
            Some((x, y)) if x < 640 || y < 480 || x as u64 * 3 != y as u64 * 4 => w.push(format!(
                "the screenshot is {x}x{y}: PortMaster asks for 4:3, at least 640x480 (capture it on a 640x480 or larger 4:3 screen; never scale or crop one)"
            )),
            _ => {}
        },
    }
    if imgs[1].1.is_none() {
        let cover = p.get("cover").filter(|v| v.truthy()).map_or("cover.png".into(), Val::py_str);
        w.push(format!("no cover ({cover}): optional, but expected; gameinfo.xml then shows the screenshot"));
    }
    if let Some(gi) = gameinfo.filter(|g| !g.is_empty()) {
        if let Some(m) = rx::gameinfo_image(gi) {
            let shipped: Vec<&str> = imgs.iter().filter_map(|(_, v)| v.as_ref().map(|x| x.1.as_str())).collect();
            if !shipped.contains(&m) && imgs[0].1.is_some() {
                w.push(format!("gameinfo.xml names {m}, which the port does not ship: run `port new` again"));
            }
        }
    }
    if let Some(l) = launcher {
        w.extend(launcher_warnings(l));
    }
    w
}

// ---- what a release takes

pub const PMU_PLUGINS: [&str; 1] = ["animclips"];
pub const PMU_SHADER_SOURCES: [&str; 4] = ["glcore", "d3d11", "vulkan", "metal"];

fn shader_from(cfg: &Dict) -> Vec<String> {
    match cfg.dict("convert").dict("shaders").get("from") {
        None => vec!["glcore".into()],
        Some(Val::Str(s)) => s.split(',').map(String::from).collect(),
        Some(Val::List(l)) => l.iter().map(Val::py_str).collect(),
        Some(_) => vec![],
    }
}

/// Whether the setup's commands all run on pmu with this port's options, and if not why.
pub fn pmu_covers(cfg: &Dict) -> (bool, Vec<String>) {
    let mut why = Vec::new();
    let other: Vec<String> = shader_from(cfg).into_iter().filter(|x| !PMU_SHADER_SOURCES.contains(&x.as_str())).collect();
    if !other.is_empty() {
        why.push(format!("shaders from {}", other.join(", ")));
    }
    let own: Vec<String> = cfg
        .dict("files")
        .list("plugins")
        .iter()
        .map(Val::py_str)
        .filter(|p| {
            let base = pkgzip::py::basename(p);
            let stem = match base.rfind('.') {
                Some(i) if base[..i].chars().any(|c| c != '.') => &base[..i],
                _ => base,
            };
            !p.to_lowercase().ends_with(".json") && !PMU_PLUGINS.contains(&stem)
        })
        .collect();
    if !own.is_empty() {
        let names: Vec<&str> = own.iter().map(|p| pkgzip::py::basename(p)).collect();
        why.push(format!("the port's converter plugins ({})", names.join(", ")));
    }
    (why.is_empty(), why)
}

/// shaders.from has `what` (a list or a string's characters: `in`).
fn from_has(cfg: &Dict, what: &str) -> bool {
    match cfg.dict("convert").dict("shaders").get("from") {
        Some(Val::List(l)) => l.iter().any(|x| x.eq(&Val::s(what))),
        Some(Val::Str(s)) => s.contains(what),
        _ => false,
    }
}

pub fn needs_pmhlslcc(cfg: &Dict) -> bool {
    from_has(cfg, "d3d11")
}

pub fn needs_pmspirvcross(cfg: &Dict) -> bool {
    from_has(cfg, "vulkan")
}

pub fn needs_asmpatch(cfg: &Dict) -> bool {
    let ad = cfg.dict("android_data");
    !ad.get("asmpatch").is_none_or(|v| v.eq(&Val::s("off")))
        || !ad.get("streaming_assets").is_none_or(|v| v.eq(&Val::s("obb")))
        || ad.truthy("data_path")
}

pub fn uses_flipbook(cfg: &Dict) -> bool {
    ["runtime", "runtime_low", "runtime_lowmem"].iter().any(|k| cfg.dict(k).contains("UA_FLIPBOOK"))
        || cfg.list("questions").iter().any(|x| x.as_dict().is_some_and(|d| d.get("env").is_some_and(|e| e.eq(&Val::s("UA_FLIPBOOK")))))
}

/// Whether a release needs a part route.json names with '?'.
pub fn wants(part: &str, cfg: &Dict) -> bool {
    let pl = cfg.dict("player");
    let abis = pl.list("abis");
    let has = |a: &str| abis.iter().any(|x| x.eq(&Val::s(a)));
    let pmu = pmu_covers(cfg).0;
    match part {
        "asmpatch" => needs_asmpatch(cfg),
        "pmhlslcc" => needs_pmhlslcc(cfg),
        "pmspirvcross" => needs_pmspirvcross(cfg),
        "pmu" => pmu,
        "pmunity" | "pybundle" => !pmu,
        "ua_host" => (abis.is_empty() || has("armeabi-v7a")) && !cfg.dict("port").truthy("aarch64_only"),
        "ua_host64" => abis.is_empty() || has("arm64-v8a") || pl.truthy("il2cpp"),
        _ => true,
    }
}

/// The Unity lines ("2019.4") whose Mono runtime a mono-arm64 release ships.
pub fn mono64_lines(cfg: &Dict) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for v in cfg.dict("game").list("unity_versions") {
        if let Some(l) = rx::line(&v.py_str()) {
            if !out.iter().any(|x| x == l) {
                out.push(l.to_string());
            }
        }
    }
    out
}
