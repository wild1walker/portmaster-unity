//! convert/tests/test_port.py's tests of the config, `port new` and `port package` (and
//! test_route.py's of the box64 hook), on synthetic ports: a port.toml naming the game's Unity
//! version (pmu's `port new` takes no --data), stand-ins for the runtime, pmu, the donors and the
//! setup's Python. Nothing of a real game or of Unity.
use std::collections::BTreeMap;
use std::fs;
use std::io::{Read, Write};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Command;

use port::config::{self, build_config, dump_toml, merge, validate};
use port::new::{new, NewArgs};
use port::package::{self, package, PackOpts};
use port::text;
use port::val::{Dict, Val};
use port::{pyjson, toml, Repo};

fn repo() -> Repo {
    Repo::new(Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..").canonicalize().unwrap())
}

struct Tmp(PathBuf);
impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn tmp(tag: &str) -> Tmp {
    static N: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    let n = N.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    let d = std::env::temp_dir().join(format!("pmu-port-{tag}-{}-{n}", std::process::id()));
    let _ = fs::remove_dir_all(&d);
    fs::create_dir_all(&d).unwrap();
    Tmp(d)
}

fn put(p: &Path, data: impl AsRef<[u8]>) {
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    fs::write(p, data).unwrap();
}

fn read(p: &Path) -> String {
    String::from_utf8(fs::read(p).unwrap()).unwrap()
}

fn d(toml_text: &str) -> Dict {
    match toml::loads(toml_text).unwrap() {
        Val::Dict(d) => d,
        _ => unreachable!(),
    }
}

fn cfg_of(name: &str, toml_text: &str) -> Dict {
    build_config(name, &d(toml_text))
}

fn quiet() -> impl FnMut(&str) {
    |_: &str| {}
}

fn err_text(r: port::Result<impl std::fmt::Debug>) -> String {
    match r {
        Err(e) => e.to_string(),
        Ok(x) => panic!("no error: {x:?}"),
    }
}

/// The test port's settings (test_port.py's make(): title, porter, two questions), over a game of
/// Unity 2019.4.8f1 whose data folder is "My Game_Data".
const BASE: &str = r#"
[port]
title = "My Game"
porter = ["someone"]
[game]
unity_versions = ["2019.4.8f1"]
data = "My Game_Data"
[runtime]
UA_POINTER = "1"
[[questions]]
id = "TEXTURE_QUALITY"
question = "Texture quality?"
options = [["medium", "Medium"], ["fast", "Fast"]]
default = "medium"
convert_set = "textures.quality"
[[questions]]
id = "CURSOR"
question = "Cursor?"
options = [["1", "On"], ["0", "Off"]]
env = "UA_POINTER_CURSOR"
"#;

/// port new from BASE merged with `extra` (a config's tables as TOML), into <t>/port.
fn make(t: &Path, extra: &str) -> PathBuf {
    let cfg = merge(&d(BASE), &d(extra));
    let cp = t.join("cfg.json");
    put(&cp, pyjson::dumps(&Val::Dict(cfg), None, true));
    let a = NewArgs { name: "mygame".into(), config: Some(port::ps(&cp)), out: Some(port::ps(&t.join("port"))), ..Default::default() };
    let out = new(&repo(), &a, &mut quiet()).unwrap();
    fake_donors(&t.join("donors"), &out);
    out
}

fn build_cfg(out: &Path) -> Dict {
    pyjson::loads(&read(&out.join("port.build.json"))).unwrap().as_dict().unwrap().dict("config").clone()
}

/// Stand-ins for the donor APKs port new names (port.build.json's player.donors).
fn fake_donors(dir: &Path, out: &Path) {
    fs::create_dir_all(dir).unwrap();
    for n in build_cfg(out).dict("player").list("donors") {
        let n = n.py_str();
        write_zip(&dir.join(&n), &[("lib/armeabi-v7a/libunity.so", n.as_bytes())], false);
    }
}

fn write_zip(p: &Path, entries: &[(&str, &[u8])], deflate: bool) {
    let mut z = port::zipw::ZipOut::create(&port::ps(p)).unwrap();
    for (n, data) in entries {
        let mut i = port::zipw::Info::file(n, 0o644, 6);
        if !deflate {
            i.method = port::zipw::STORED;
        }
        z.add(i, data).unwrap();
    }
    z.finish().unwrap();
}

fn stub_pmu(t: &Path) -> String {
    let d = t.join("pmu-out");
    put(&d.join("pmu"), "#!/bin/sh\nexit 2\n");
    fs::set_permissions(d.join("pmu"), fs::Permissions::from_mode(0o755)).unwrap();
    put(&d.join("licenses/LICENSE.musl.txt"), "MIT musl");
    put(&d.join("licenses/LICENSE.rust-crates.txt"), "MIT / Apache 2.0 crates");
    port::ps(&d)
}

/// The runtime's stand-ins (ua_host with the flip-book stand-in beside it, the decoders' and
/// libhybris's licences) and package()'s options.
fn release_parts(t: &Path) -> (String, PackOpts) {
    let rt = t.join("rt");
    put(&rt.join("runtime/host/out/ua_host"), "host");
    put(&rt.join("runtime/host/out/uaflipbook.dll"), "MZ flip-book");
    put(&rt.join("runtime/media/out/licenses/libvpx.copyright"), "BSD");
    put(&rt.join("runtime/thirdparty/libhybris/LICENSE.Apache2"), "Apache 2.0");
    let opts = PackOpts {
        out: Some(port::ps(&t.join("rel"))),
        runtime: Some(port::ps(&rt)),
        donors: Some(port::ps(&t.join("donors"))),
        no_build: true,
        pmu: Some(stub_pmu(t)),
        ..Default::default()
    };
    (port::ps(&rt), opts)
}

struct Zip {
    names: Vec<String>,
    modes: BTreeMap<String, u32>,
    a: pkgzip::zip::Archive,
}

impl Zip {
    fn open(p: &Path) -> Zip {
        let a = pkgzip::zip::Archive::open(&port::ps(p)).unwrap();
        let names = a.entries.iter().map(|e| e.name.clone()).collect();
        let modes = a.entries.iter().map(|e| (e.name.clone(), e.external_attr >> 16 & 0o777)).collect();
        Zip { names, modes, a }
    }
    fn read(&self, n: &str) -> Vec<u8> {
        self.a.read(n).unwrap()
    }
    fn has(&self, n: &str) -> bool {
        self.names.iter().any(|x| x == n)
    }
}

fn bash_n(p: &Path) {
    let st = Command::new("bash").arg("-n").arg(p).status().unwrap();
    assert!(st.success(), "bash -n {}", p.display());
}

fn exec(p: &Path) -> bool {
    fs::metadata(p).unwrap().permissions().mode() & 0o111 != 0
}

// ------------------------------------------------------------------ ConfigTest

#[test]
fn toml_round_trip() {
    let cfg = cfg_of(
        "mygame",
        r#"[game]
unity_versions = ["2020.3.1f1"]
[runtime]
UA_POINTER = "1"
[convert.shaders]
from = ["d3d11"]
[[questions]]
id = "TEXTURE_QUALITY"
question = 'Q "x"?'
options = [["medium", "Medium"], ["fast", "Fast"]]
default = "medium"
convert_set = "textures.quality"
"#,
    );
    validate(&cfg).unwrap();
    let back = d(&dump_toml(&cfg, &Dict::new(), &[]).unwrap());
    assert!(Val::Dict(merge(&config::defaults(), &back)).eq(&Val::Dict(cfg.clone())));
    assert_eq!(cfg.dict("android_data").str("package"), "com.pmunity.mygame");
    assert_eq!(cfg.dict("port").str("title"), "mygame");
}

#[test]
fn validate_errors() {
    let bad = cfg_of(
        "Bad Name",
        "[game]\nunity_versions = ['x']\n[runtime]\nua-x = '1'\n[[questions]]\nid = 'q'\nquestion = '?'\noptions = [['a b', 'A']]\n",
    );
    let msg = err_text(validate(&bad));
    for part in ["port.name", "runtime.ua-x", "id", "plain values", "unity_versions"] {
        assert!(msg.contains(part), "{part}: {msg}");
    }
}

#[test]
fn shell_files() {
    let cfg = cfg_of(
        "g",
        "[game]\nunity_versions = ['2020.3.1f1']\ndata = 'My Game_Data'\n[runtime]\nUA_KEYMAP = 'start:escape,x:space'\nUA_POINTER = '1'\n\
         [android_data]\nargs = ['--rewired-joysticks', '2']\n",
    );
    let t = tmp("shell");
    put(&t.0.join("setup.env"), text::setup_env(&cfg, &[], None));
    put(&t.0.join("port.env"), text::port_env(&cfg));
    let out = Command::new("bash")
        .arg("-c")
        .arg(format!(
            ". {0}/setup.env; UA_POINTER=0; . {0}/port.env; echo \"$GAME_DATA|${{ANDROID_DATA_ARGS[1]}}|$UA_KEYMAP|$UA_POINTER\"",
            t.0.display()
        ))
        .output()
        .unwrap();
    assert_eq!(String::from_utf8_lossy(&out.stdout).trim(), "My Game_Data|2|start:escape,x:space|0");
}

#[test]
fn lowmem_env() {
    let mut cfg = cfg_of(
        "g",
        "[game]\nunity_versions = ['2020.3.1f1']\n[runtime]\nUA_POINTER = '1'\n[runtime_lowmem]\nASMPATCH_HK_BLUR = 'half'\nUA_X = 'a b'\n",
    );
    validate(&cfg).unwrap();
    let back = d(&dump_toml(&cfg, &Dict::new(), &[]).unwrap());
    assert!(Val::Dict(merge(&config::defaults(), &back)).eq(&Val::Dict(cfg.clone())));
    let t = tmp("lowmem");
    let run = |cfg: &Dict, kb: u64, pre: &str, vars: &str| -> String {
        put(&t.0.join("meminfo"), format!("MemTotal:     {kb} kB\nMemFree: 1 kB\n"));
        put(&t.0.join("port.env"), text::port_env(cfg).replace("/proc/meminfo", &port::ps(&t.0.join("meminfo"))));
        let o = Command::new("bash").arg("-c").arg(format!("{pre}. {}/port.env; echo \"{vars}\"", t.0.display())).output().unwrap();
        String::from_utf8_lossy(&o.stdout).trim().to_string()
    };
    for (kb, pre, want) in [(973 * 1024, "", "half|a b"), (1990 * 1024, "", "|"), (973 * 1024, "ASMPATCH_HK_BLUR=full; ", "full|a b")] {
        assert_eq!(run(&cfg, kb, pre, "$ASMPATCH_HK_BLUR|$UA_X"), want, "{kb} {pre}");
    }
    assert!(!text::port_env(&cfg_of("g", "[game]\nunity_versions = ['2020.3.1f1']\n")).contains("MemTotal"));
    let cfg2 = cfg_of(
        "g",
        "[game]\nunity_versions = ['2020.3.1f1']\n[runtime_low]\nASMPATCH_HK_BLUR = 'off'\nUA_RENDER_PIXELS = '560000'\n[runtime_lowmem]\nUA_X = 'a b'\n",
    );
    validate(&cfg2).unwrap();
    for (kb, pre, want) in [
        (973 * 1024, "", "off|560000|a b"),
        (3900 * 1024, "", "off|560000|"),
        (3900 * 1024, "UA_PICTURE=high; ", "||"),
        (973 * 1024, "UA_PICTURE=high; ", "||a b"),
        (973 * 1024, "UA_PICTURE=full; ", "||a b"),
        (973 * 1024, "UA_PICTURE=low; ", "off|560000|a b"),
        (973 * 1024, "ASMPATCH_HK_BLUR=low; ", "low|560000|a b"),
    ] {
        assert_eq!(run(&cfg2, kb, pre, "$ASMPATCH_HK_BLUR|$UA_RENDER_PIXELS|$UA_X"), want, "{kb} {pre}");
    }
    assert!(!text::port_env(&cfg_of("g", "[game]\nunity_versions = ['2020.3.1f1']\n")).contains("UA_PICTURE"));
    // asmpatch = ["auto", <opt-in>]: the default patches and those; "auto" only first
    cfg.table("android_data").set("asmpatch", Val::strs(&["auto", "hk-blur"]));
    validate(&cfg).unwrap();
    assert!(text::setup_env(&cfg, &[], None).replace('\'', "").contains("ASMPATCH=auto,hk-blur"));
    assert!(text::setup_env(&cfg, &[], None).replace('\'', "").contains("WWISE_BANKS=\n"));
    cfg.table("wwise").set("banks", Val::s("StreamingAssets/Audio/GeneratedSoundBanks/Linux"));
    validate(&cfg).unwrap();
    assert!(text::setup_env(&cfg, &[], None).contains("WWISE_BANKS=StreamingAssets/Audio/GeneratedSoundBanks/Linux\n"));
    cfg.table("android_data").set("asmpatch", Val::strs(&["hk-blur", "auto"]));
    assert!(validate(&cfg).is_err());
}

#[test]
fn questions_lua() {
    let qs = d("[[q]]\nid = 'A'\nquestion = 'say \"hi\"'\noptions = [['x', 'X'], ['y', 'Y']]\n");
    let lua = text::questions_lua(qs.list("q"));
    assert!(lua.contains("id = \"A\""));
    assert!(lua.contains("question = \"say \\\"hi\\\"\""));
    assert!(lua.contains("{ \"x\", \"X\" },"));
}

// ------------------------------------------------------------------ NewAndSetupTest

#[test]
fn new_into_repo_clone() {
    let t = tmp("clone");
    make(&t.0, "");
    let rp = t.0.join("repo");
    for f in ["LICENSE", "README.md", ".git/HEAD"] {
        put(&rp.join(f), "x");
    }
    for f in ["._LICENSE", ".DS_Store"] {
        put(&rp.join(f), "x"); // (macOS's own files beside them are not "other files")
    }
    let cp = port::ps(&t.0.join("cfg.json"));
    let a = NewArgs { name: "mygame".into(), config: Some(cp.clone()), out: Some(port::ps(&rp)), ..Default::default() };
    let out = new(&repo(), &a, &mut quiet()).unwrap();
    let gi = out.join(".gitignore");
    assert!(read(&gi).split('\n').any(|l| l == "out/"));
    assert!(exec(&out.join("package.sh")));
    put(&gi, "mine\n");
    new(&repo(), &a, &mut quiet()).unwrap();
    assert_eq!(read(&gi), "mine\n");
    let other = t.0.join("other");
    put(&other.join("notes.txt"), "x");
    let a2 = NewArgs { out: Some(port::ps(&other)), ..a };
    assert!(err_text(new(&repo(), &a2, &mut quiet())).contains("is not a port made by `port new`"));
}

#[test]
fn new_files() {
    let t = tmp("files");
    let out = make(&t.0, "");
    let files: Vec<String> = read(&out.join(".port_files")).split('\n').filter(|x| !x.is_empty()).map(String::from).collect();
    for f in [
        "My Game.sh", "mygame/setup/patchscript", "mygame/setup/update", "mygame/setup/setup.env", "mygame/setup/convert.json",
        "mygame/setup/questions.lua", "mygame/setup.txt", "mygame/port.json", "mygame/port.env", "mygame/README.md",
        "mygame/gamedata/PUT_GAME_FILES_HERE.txt", "mygame/mygame.ini", "mygame/gameinfo.xml", "README.md", "package.sh",
        "port.build.json", "testing_thread.txt",
    ] {
        assert!(files.iter().any(|x| x == f), "{f}");
    }
    for f in &files {
        let p = out.join(f);
        let text = String::from_utf8_lossy(&fs::read(&p).unwrap()).into_owned();
        if f != "port.build.json" {
            assert!(!text.contains("@@"), "{f}");
        }
        if f.ends_with(".sh") || f.ends_with("patchscript") || f.ends_with("setup/update") {
            bash_n(&p);
            assert_eq!(exec(&p), f == "package.sh", "{f}");
        }
    }
    let pj = pyjson::loads(&read(&out.join("mygame/port.json"))).unwrap();
    let pj = pj.as_dict().unwrap();
    assert!(pj.get("items").unwrap().eq(&Val::strs(&["My Game.sh", "mygame"])));
    assert_eq!(pj.str("name"), "mygame.zip");
    let launcher = read(&out.join("My Game.sh"));
    assert!(launcher.contains("GAMEDIR=\"/$directory/ports/mygame\""));
    assert!(launcher.contains("PATCHER_QUESTIONS"));
    assert!(text::launcher_warnings(&launcher).is_empty());
    let readme = read(&out.join("README.md"));
    assert!(readme.starts_with("## Notes\n\nThanks to the makers of My Game"), "{}", &readme[..80]);
    let heads: Vec<&str> = readme.lines().filter(|l| l.starts_with('#')).collect();
    assert_eq!(heads, ["## Notes", "## Controls", "## Compile"]);
    assert!(readme.contains("| Select + Start | Quit |"));
    assert!(readme.contains("runtime/build.sh"));
    assert!(readme.contains("32-bit (armhf) userland"));
    assert!(!readme.contains("tools/asmpatch/build.sh"));
    assert!(!readme.contains('\u{2014}'));
    let pr = read(&out.join("mygame/README.md"));
    assert!(pr.contains("| Select + Start | Quit |"));
    assert!(pr.contains("or GOG's Linux installer `.sh`"));
    assert!(pr.contains("runs on Unity's own Android player, run natively on the handheld's 32-bit userland by\n"));
    assert!(pr.contains(
        "- A handheld with a 32-bit (armhf) userland and GLES 3 (e.g. Knulli on Allwinner H700 devices),\n  and a 64-bit (aarch64) system for the setup's tools."
    ));
    let gi = read(&out.join("mygame/gameinfo.xml"));
    assert!(gi.contains("<path>./My Game.sh</path>"));
    assert!(gi.contains("<image>./mygame/screenshot.png</image>"));
    let tt = read(&out.join("testing_thread.txt"));
    assert!(tt.contains("Title: My Game"));
    assert!(tt.contains(pj.dict("attr").str("inst")));
    let cfg = build_cfg(&out);
    assert_eq!(cfg.dict("game").str("data"), "My Game_Data");
    assert!(cfg.get("runtime").unwrap().eq(&Val::Dict(d("UA_POINTER = '1'"))));
    // again, without questions: their files go
    let cfg = merge(&d(BASE), &Dict::new());
    let mut cfg = cfg;
    cfg.set("questions", Val::List(vec![]));
    put(&t.0.join("cfg.json"), pyjson::dumps(&Val::Dict(cfg), None, true));
    let a = NewArgs { name: "mygame".into(), config: Some(port::ps(&t.0.join("cfg.json"))), out: Some(port::ps(&out)), ..Default::default() };
    new(&repo(), &a, &mut quiet()).unwrap();
    assert!(!out.join("mygame/setup/questions.lua").exists());
}

#[test]
fn pmu_covers() {
    let base = d("[convert]\nsteps = ['shaders', 'settings', 'audio', 'quality']\n[convert.settings]\ngraphics_apis = [11]\n[player]\n[files]\nplugins = []\n[android_data]\n");
    assert_eq!(text::pmu_covers(&base), (true, vec![]));
    assert!(text::wants("pmu", &base));
    let over = |o: &str| merge(&base, &d(o));
    assert_eq!(text::pmu_covers(&over("[files]\nplugins = ['convert/animclips.py']")), (true, vec![]));
    let win = over("[convert.shaders]\nfrom = ['d3d11']");
    assert_eq!(text::pmu_covers(&win), (true, vec![]));
    assert!(text::wants("pmu", &win) && text::wants("pmhlslcc", &win));
    let vk = over("[convert.shaders]\nfrom = ['vulkan']");
    assert!(text::wants("pmu", &vk) && text::wants("pmspirvcross", &vk));
    for o in [
        "[convert.shaders]\nfrom = ['metal']",
        "[convert.shaders]\nfrom = ['glcore', 'metal']",
        "[files]\nplugins = ['steps/tweaks.json', 'convert/animclips.py']",
        "[port]\nroute = 'apk'\n[player]\nil2cpp = true",
    ] {
        assert_eq!(text::pmu_covers(&over(o)), (true, vec![]), "{o}");
    }
    for (o, why) in [
        ("[convert.shaders]\nfrom = ['glcore', 'gles']", "shaders from gles"),
        ("[files]\nplugins = ['steps/mine.py', 'x.json']", "the port's converter plugins (mine.py)"),
    ] {
        assert_eq!(text::pmu_covers(&over(o)), (false, vec![why.to_string()]));
    }
    for route in ["android", "mono-arm64", "box64", "apk", "il2cpp-arm64"] {
        let r = port::routes::route(&repo(), route).unwrap().unwrap();
        assert!(r.list("package").iter().any(|x| x.eq(&Val::s("pmu"))), "{route}");
        for p in ["pmu?", "pmunity?", "pybundle?"] {
            assert!(!r.list("package").iter().any(|x| x.eq(&Val::s(p))), "{route} {p}");
        }
    }
}

#[test]
fn windows_and_vulkan_ports() {
    for (tool, from, lic, built) in [
        ("pmhlslcc", "d3d11", "LICENSE.pmhlslcc-HLSLcc.txt", "ARCH=arm64 tools/hlslcc/build.sh"),
        ("pmspirvcross", "vulkan", "LICENSE.pmspirvcross-SPIRV-Cross-SMOL-V.txt", "ARCH=arm64 tools/spirvcross/build.sh"),
    ] {
        let t = tmp(tool);
        let out = make(&t.0, &format!("[game]\nunity_versions = ['2022.2.17f1']\nbuild = 'windows'\n[convert.shaders]\nfrom = ['{from}']\n"));
        let cfg = build_cfg(&out);
        if tool == "pmhlslcc" {
            assert!(text::needs_pmhlslcc(&cfg) && text::needs_winbcl(&cfg) && !text::needs_pmspirvcross(&cfg));
            assert!(read(&out.join("mygame/setup/setup.env")).contains("WINBCL=auto"));
            assert!(dump_toml(&cfg, &Dict::new(), &[]).unwrap().contains("winbcl = \"auto\""));
            let readme = read(&out.join("README.md"));
            assert!(readme.contains(built) && readme.contains("Windows-only class libraries"));
            let pr = read(&out.join("mygame/README.md"));
            assert!(pr.contains("the setup takes the Unix ones from the same donors"));
            assert!(pr.contains("Copy the game (its folder, or the archive it came in)"));
            assert!(!pr.contains("GOG's Linux installer") && !pr.contains("Wi-Fi on"));
            let cfg19 = merge(&cfg, &d("[game]\nunity_versions = ['2019.4.8f1']"));
            assert!(text::needs_pmhlslcc(&cfg19) && !text::needs_winbcl(&cfg19));
        } else {
            assert!(text::needs_pmspirvcross(&cfg) && text::wants("pmspirvcross", &cfg) && !text::needs_pmhlslcc(&cfg));
        }
        let (rt, mut opts) = release_parts(&t.0);
        let bin = Path::new(&rt).join("tl").join(tool);
        put(&bin, "elf");
        opts.pmhlslcc_bin = Some(port::ps(&bin));
        opts.pmspirvcross_bin = Some(port::ps(&bin));
        assert!(err_text(package(&repo(), &port::ps(&out), &opts, &mut quiet())).contains(&format!("{tool}.LICENSE.txt")));
        put(&bin.with_file_name(format!("{tool}.LICENSE.txt")), "the tool's licence");
        let z = Zip::open(&package(&repo(), &port::ps(&out), &opts, &mut quiet()).unwrap());
        assert_eq!(z.read(&format!("mygame/licenses/{lic}")), b"the tool's licence");
        assert_eq!(z.read("mygame/licenses/LICENSE.musl.txt"), b"MIT musl");
        assert_eq!(z.modes[&format!("mygame/setup/{tool}")], 0o755);
        assert_eq!(z.modes["mygame/setup/pmu"], 0o755);
        assert!(!z.has("mygame/setup/python.tar.gz"));
        opts.pmhlslcc_bin = None;
        opts.pmspirvcross_bin = None;
        assert!(err_text(package(&repo(), &port::ps(&out), &opts, &mut quiet())).contains(built));
    }
}

#[test]
fn il2cpp_and_mono_arm64_routes() {
    let t = tmp("arm64");
    let out = make(&t.0, "[port]\nroute = 'il2cpp-arm64'\n");
    let env = read(&out.join("mygame/setup/setup.env"));
    for x in ["PORT_ROUTE=il2cpp-arm64", "PLAYER_BACKEND=il2cpp", "PLAYER_ABI=arm64-v8a"] {
        assert!(env.contains(x), "{x}");
    }
    let launcher = read(&out.join("My Game.sh"));
    assert!(!launcher.contains("lib32") && launcher.contains("uname -m"));
    bash_n(&out.join("My Game.sh"));
    bash_n(&out.join("mygame/setup/patchscript"));
    let pj = pyjson::loads(&read(&out.join("mygame/port.json"))).unwrap();
    let a = pj.as_dict().unwrap().dict("attr");
    assert!(!a.list("reqs").iter().any(|r| r.eq(&Val::s("armhf"))));
    assert!(a.str("inst").contains("IL2CPP"));
    let readme = read(&out.join("README.md"));
    assert!(readme.contains("64-bit (aarch64) handheld") && readme.contains("Unity's IL2CPP on the handheld"));
    assert!(readme.contains("ARCH=arm64 runtime/build.sh") && !readme.contains("armhf"));
    assert!(read(&out.join("testing_thread.txt")).contains("Needs a 64-bit (aarch64) handheld."));
    assert!(read(&out.join("mygame/gameinfo.xml")).contains("<desc>My Game on Unity's 64-bit Android player"));
    // a player set in port.toml wins over the route's default
    let out = make(&t.0, "[port]\nroute = 'il2cpp-arm64'\n[player]\nabi = 'armeabi-v7a'\n");
    assert!(read(&out.join("mygame/setup/setup.env")).contains("PLAYER_ABI=armeabi-v7a"));
    // mono-arm64
    let out = make(&t.0, "[port]\nroute = 'mono-arm64'\n");
    let env = read(&out.join("mygame/setup/setup.env"));
    for x in ["PORT_ROUTE=mono-arm64", "PLAYER_BACKEND=il2cpp", "PLAYER_ABI=arm64-v8a"] {
        assert!(env.contains(x), "{x}");
    }
    let pj = pyjson::loads(&read(&out.join("mygame/port.json"))).unwrap();
    let a = pj.as_dict().unwrap().dict("attr");
    assert!(!a.list("reqs").iter().any(|r| r.eq(&Val::s("armhf"))) && a.str("desc").contains("Unity's Mono"));
    let pr = read(&out.join("mygame/README.md"));
    assert!(pr.contains("runs on Unity's 64-bit (arm64) Android player") && pr.contains("no 32-bit userland needed"));
    let lines = text::mono64_lines(&d("[game]\nunity_versions = ['2019.4.8f1', '2019.4.40f1', '6000.0.61f1']"));
    assert_eq!(lines, ["2019.4", "6000.0"]);
}

#[test]
fn package_build_outputs() {
    // a port with its own build steps; a port whose setup would need what pmu does not do (a converter
    // plugin of the porter's own, which the retired setup's Python ran) is refused
    let t = tmp("pkgpy");
    put(&t.0.join("mystep.py"), "STEPS = {}\n");
    let own = make(&t.0, "[files]\nplugins = [\"mystep.py\"]\n");
    let cfg = build_cfg(&own);
    assert_eq!(text::pmu_covers(&cfg), (false, vec!["the port's converter plugins (mystep.py)".to_string()]));
    let (_, mut opts) = release_parts(&t.0);
    let e = err_text(package(&repo(), &port::ps(&own), &opts, &mut quiet()));
    assert!(e.contains("do not do what this port needs: the port's converter plugins (mystep.py)") && e.contains("retired"), "{e}");
    let t = tmp("pkgbuild");
    let out = make(
        &t.0,
        r##"[build]
commands = ['printf "#!/bin/sh\\n" > "$BUILD/tool"', 'echo data > "$BUILD/table" && chmod +x "$BUILD/table"']
[build.outputs]
"setup/files/tool" = "{build}/tool"
"setup/files/table" = "{build}/table"
"##,
    );
    let (_, o2) = release_parts(&t.0);
    opts = o2;
    opts.submission = true;
    write_png(&out.join("screenshot.png"), 640, 480);
    let mut msgs = Vec::new();
    let z = Zip::open(&package(&repo(), &port::ps(&out), &opts, &mut |m: &str| msgs.push(m.to_string())).unwrap());
    assert_eq!(z.read("mygame/screenshot.png"), fs::read(out.join("screenshot.png")).unwrap());
    assert!(z.has("mygame/gameinfo.xml") && !z.has("mygame/cover.png"));
    assert!(!z.names.iter().any(|n| n.contains("testing_thread")));
    assert!(t.0.join("rel/testing_thread.txt").is_file());
    let text = msgs.join("\n");
    assert!(text.contains("no cover") && !text.contains("no screenshot") && text.contains("[controls] is empty"));
    assert!(!text.contains("standard lines"));
    let sub = t.0.join("rel/submission/mygame");
    let mut top: Vec<String> = fs::read_dir(&sub).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect();
    top.sort();
    assert_eq!(top, ["My Game.sh", "README.md", "gameinfo.xml", "mygame", "port.json", "screenshot.png"]);
    assert!(read(&sub.join("README.md")).starts_with("## Notes"));
    assert!(!exec(&sub.join("My Game.sh")));
    let inner: Vec<String> = fs::read_dir(sub.join("mygame")).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect();
    assert!(!inner.iter().any(|n| ["port.json", "gameinfo.xml", "screenshot.png"].contains(&n.as_str())));
    for n in ["ua_host", "setup", "licenses", "README.md"] {
        assert!(inner.iter().any(|x| x == n), "{n}");
    }
    assert!(exec(&sub.join("mygame/ua_host")));
    for n in [
        "My Game.sh", "mygame/ua_host", "mygame/setup/pmu", "mygame/licenses/LICENSE.libvpx.txt",
        "mygame/licenses/LICENSE.portmaster-unity.txt", "mygame/licenses/LICENSE.libhybris.txt", "mygame/setup/patchscript",
    ] {
        assert!(z.has(n), "{n}");
    }
    assert!(!z.names.iter().any(|n| n.contains("pmunity/") || n.contains("python") || n.contains("__pycache__")));
    for n in ["mygame/setup/asmpatch", "mygame/setup/pmhlslcc"] {
        assert!(!z.has(n), "{n}");
    }
    assert_eq!(z.modes["My Game.sh"], 0o644);
    assert_eq!(z.modes["mygame/ua_host"], 0o755);
    assert_eq!(z.modes["mygame/setup/patchscript"], 0o644);
    assert_eq!(z.modes["mygame/setup/files/tool"], 0o755);
    assert_eq!(z.modes["mygame/setup/files/table"], 0o644);
    assert!(z.names.iter().filter(|n| n.starts_with("mygame/licenses/")).all(|n| {
        let f = &n["mygame/licenses/".len()..];
        f.starts_with("LICENSE.") && f.ends_with(".txt") && !f.contains('/')
    }));
    assert!(!z.names.iter().any(|n| n.contains("port.toml")));
}

fn write_png(p: &Path, w: u32, h: u32) {
    let mut raw = Vec::new();
    for _ in 0..h {
        raw.push(0);
        raw.extend(std::iter::repeat_n(0u8, (w * 3) as usize));
    }
    let mut e = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::default());
    e.write_all(&raw).unwrap();
    let idat = e.finish().unwrap();
    let chunk = |t: &[u8], data: &[u8]| {
        let mut c = (data.len() as u32).to_be_bytes().to_vec();
        c.extend_from_slice(t);
        c.extend_from_slice(data);
        let mut h = crc32fast::Hasher::new();
        h.update(t);
        h.update(data);
        c.extend_from_slice(&h.finalize().to_be_bytes());
        c
    };
    let mut ihdr = w.to_be_bytes().to_vec();
    ihdr.extend_from_slice(&h.to_be_bytes());
    ihdr.extend_from_slice(&[8, 2, 0, 0, 0]);
    let mut png = b"\x89PNG\r\n\x1a\n".to_vec();
    png.extend(chunk(b"IHDR", &ihdr));
    png.extend(chunk(b"IDAT", &idat));
    png.extend(chunk(b"IEND", b""));
    put(p, png);
}

#[test]
fn package_pmu() {
    let t = tmp("pkgpmu");
    let out = make(&t.0, "");
    let cfg = build_cfg(&out);
    assert_eq!(text::pmu_covers(&cfg), (true, vec![]));
    assert!(cfg.dict("player").get("donors").unwrap().eq(&Val::strs(&["donor-2019.4.8f1-mono-armv7.apk"])));
    let env = read(&out.join("mygame/setup/setup.env"));
    assert!(env.contains("DONORS=donor-2019.4.8f1-mono-armv7.apk") && !env.contains("PLAYER_URL"));
    let (_, opts) = release_parts(&t.0);
    for (donors, says) in [
        (None, "no donor folder was given: --donors DIR (or $DONORS) with donor-2019.4.8f1-mono-armv7.apk"),
        (Some(port::ps(&t.0.join("none"))), "has no donor-2019.4.8f1-mono-armv7.apk"),
    ] {
        if std::env::var_os("DONORS").is_some() && donors.is_none() {
            continue;
        }
        let o = PackOpts { donors, ..opts.clone() };
        let e = err_text(package(&repo(), &port::ps(&out), &o, &mut quiet()));
        assert!(e.contains(says), "{e}");
        assert!(e.contains("tools/donor/build.sh <Unity 2019.4.8f1 editor> MonoArmv7"), "{e}");
    }
    let o = PackOpts { pmu: Some(port::ps(&t.0.join("none"))), ..opts.clone() };
    assert!(err_text(package(&repo(), &port::ps(&out), &o, &mut quiet())).contains("no pmu and licenses/"));
    let z = Zip::open(&package(&repo(), &port::ps(&out), &opts, &mut quiet()).unwrap());
    assert_eq!(z.read("mygame/setup/pmu"), b"#!/bin/sh\nexit 2\n");
    assert_eq!(z.read("mygame/licenses/LICENSE.musl.txt"), b"MIT musl");
    assert_eq!(z.read("mygame/licenses/LICENSE.rust-crates.txt"), b"MIT / Apache 2.0 crates");
    assert!(String::from_utf8(z.read("mygame/licenses/LICENSE.unity-donor.txt")).unwrap().contains("Made-with-Unity"));
    assert_eq!(z.modes["mygame/setup/pmu"], 0o755);
    assert_eq!(z.modes["mygame/donor/donor-2019.4.8f1-mono-armv7.apk"], 0o644);
    for n in ["My Game.sh", "mygame/ua_host", "mygame/setup/patchscript", "mygame/licenses/LICENSE.libvpx.txt", "mygame/licenses/LICENSE.libhybris.txt"] {
        assert!(z.has(n), "{n}");
    }
    for n in [
        "mygame/setup/python.tar.gz", "mygame/setup/pmunity/__main__.py", "mygame/setup/tools/il2cpp/ndk-clang.sh",
        "mygame/licenses/LICENSE.setup-python.txt", "mygame/setup/pmhlslcc", "mygame/setup/pmspirvcross", "mygame/setup/asmpatch",
    ] {
        assert!(!z.has(n), "{n}");
    }
}

fn elf(machine: u16) -> Vec<u8> {
    let mut h = vec![0u8; 64];
    h[..4].copy_from_slice(b"\x7fELF");
    h[4] = if machine == package::EM_AARCH64 { 2 } else { 1 };
    h[5] = 1;
    h[6] = 1;
    h[18..20].copy_from_slice(&machine.to_le_bytes());
    h.extend(std::iter::repeat_n(b'Z', 100));
    h
}

#[test]
fn aarch64_only() {
    let t = tmp("a64off");
    let out = make(&t.0, "");
    assert!(read(&out.join("mygame/setup/setup.env")).contains("PORT_ROUTE=android"));
    assert!(build_cfg(&out).dict("port").get("aarch64_only").unwrap().eq(&Val::Bool(false)));
    assert!(read(&out.join("mygame/port.json")).contains("\"armhf\""));
    drop(t);
    let t = tmp("a64on");
    let out = make(
        &t.0,
        &format!(
            r#"[port]
route = "android"
aarch64_only = true
[player]
backend = "mono"
abi = "armeabi-v7a"
sha256 = {{"libunity.so" = "{}"}}
[runtime]
UA_FLIPBOOK = ""
[build]
commands = ['printf x > "$BUILD/x"']
[build.outputs]
"pmhwdec/libpmhwdec.so.1" = "arm32.so"
"pmhwdec64/libpmhwdec.so.1" = "arm64.so"
tools = "tools"
"#,
            "00".repeat(32)
        ),
    );
    put(&out.join("arm32.so"), elf(package::EM_ARM));
    put(&out.join("arm64.so"), elf(package::EM_AARCH64));
    put(&out.join("tools/helper32"), elf(package::EM_ARM));
    put(&out.join("tools/helper64"), elf(package::EM_AARCH64));
    let env = read(&out.join("mygame/setup/setup.env"));
    for x in ["PORT_ROUTE=mono-arm64", "PLAYER_BACKEND=il2cpp", "PLAYER_ABI=arm64-v8a", "DONORS='donor-2019.4.8f1-il2cpp-arm64.apk donor-2019.4.8f1-mono-armv7.apk'"] {
        assert!(env.contains(x), "{x}");
    }
    assert!(!env.contains("PLAYER_SHA256=libunity"));
    let cfg = build_cfg(&out);
    assert_eq!(cfg.dict("port").str("route"), "mono-arm64");
    assert!(cfg.dict("player").dict("sha256").is_empty());
    assert!(!read(&out.join("mygame/port.json")).contains("\"armhf\""));
    assert!(read(&out.join("My Game.sh")).contains("uname -m"));
    // the donors as tools/donor builds them: Unity's 32-bit Mono player in the Mono one
    let dn = t.0.join("donors");
    let e32 = elf(package::EM_ARM);
    let e64 = elf(package::EM_AARCH64);
    let e86 = elf(3);
    write_zip(
        &dn.join("donor-2019.4.8f1-mono-armv7.apk"),
        &[
            ("assets/bin/Data/Managed/UnityEngine.dll", b"MZ engine"),
            ("assets/bin/Data/Managed/mscorlib.dll", b"MZ corlib"),
            ("lib/armeabi-v7a/libmain.so", &e32),
            ("lib/armeabi-v7a/libunity.so", &e32),
            ("lib/armeabi-v7a/libmonobdwgc-2.0.so", &e32),
            ("lib/x86/libunity.so", &e86),
        ],
        true,
    );
    write_zip(
        &dn.join("donor-2019.4.8f1-il2cpp-arm64.apk"),
        &[("lib/arm64-v8a/libmain.so", &e64), ("lib/arm64-v8a/libunity.so", &e64), ("lib/arm64-v8a/libil2cpp.so", &e64)],
        false,
    );
    let (rt, mut opts) = release_parts(&t.0);
    let rt = PathBuf::from(rt);
    put(&rt.join("runtime/host/out/arm64/ua_host"), elf(package::EM_AARCH64));
    put(&rt.join("runtime/media/out/arm64/licenses/libvpx.copyright"), "BSD");
    put(&rt.join("runtime/host/out/ua_host"), elf(package::EM_ARM));
    put(&rt.join("runtime/relayout/0123abcd.txt"), "# a code layout list\n");
    put(&rt.join("runtime/relayout/README.md"), "# not shipped\n");
    let w = t.0.join("mono64");
    for n in ["out/unity-2019.4-mbe/libmonobdwgc-2.0.so", "out/unity-2019.4-mbe/libMonoPosixHelper.so"] {
        put(&w.join(n), elf(package::EM_AARCH64));
    }
    for n in ["LICENSE", "PATENTS.TXT", "external/bdwgc/README.QUICK"] {
        put(&w.join("src/unity-2019.4-mbe").join(n), "MIT");
    }
    put(&t.0.join("bridge.so"), elf(package::EM_AARCH64));
    opts.mono64_work = Some(port::ps(&w));
    opts.mono64_bridge = Some(port::ps(&t.0.join("bridge.so")));
    // (arch-check wants setup/pmu for aarch64: the stand-in as an aarch64 ELF file)
    put(&Path::new(opts.pmu.as_ref().unwrap()).join("pmu"), elf(package::EM_AARCH64));
    let mut msgs = Vec::new();
    let zp = package(&repo(), &port::ps(&out), &opts, &mut |m: &str| msgs.push(m.to_string())).unwrap();
    let text = msgs.join("\n");
    assert!(text.contains("build.outputs: pmhwdec/libpmhwdec.so.1 not shipped: armhf"));
    assert!(text.contains("tools/helper32 not shipped: armhf"));
    assert!(text.contains("aarch64 only: no armhf file in the release"));
    let z = Zip::open(&zp);
    for n in [
        "mygame/ua_host", "mygame/uaflipbook.dll", "mygame/setup/mono64/2019.4/libil2cpp.so", "mygame/setup/mono64/2019.4/libmonobdwgc-2.0.so",
        "mygame/pmhwdec64/libpmhwdec.so.1", "mygame/tools/helper64", "mygame/donor/donor-2019.4.8f1-il2cpp-arm64.apk",
    ] {
        assert!(z.has(n), "{n}");
    }
    assert!(z.has("mygame/relayout/0123abcd.txt"));
    for n in ["mygame/ua_host64", "mygame/pmhwdec/libpmhwdec.so.1", "mygame/tools/helper32", "mygame/relayout/README.md"] {
        assert!(!z.has(n), "{n}");
    }
    assert_eq!(package::elf_machine(&z.read("mygame/ua_host")[..20]), Some(package::EM_AARCH64));
    // (pmu arch-check's full check of the release zip passes)
    let rep = pkgzip::arch::check_release_zip(&port::ps(&zp), &t.0.join("archcheck")).unwrap();
    assert!(rep.ok(), "arch-check: {:?} {:?}", rep.missing, rep.armhf);
    let x = t.0.join("x");
    pkgzip::zip::extract_all(&port::ps(&zp), &port::ps(&x)).unwrap();
    assert!(package::armhf_files(&x, &x).unwrap().is_empty());
    let dz = pkgzip::zip::Archive::open(&port::ps(&x.join("mygame/donor/donor-2019.4.8f1-mono-armv7.apk"))).unwrap();
    let mut names: Vec<&str> = dz.names().collect();
    names.sort();
    assert_eq!(names, ["assets/bin/Data/Managed/UnityEngine.dll", "assets/bin/Data/Managed/mscorlib.dll"]);
    assert_eq!(dz.read("assets/bin/Data/Managed/UnityEngine.dll").unwrap(), b"MZ engine");
    assert_eq!(dz.info("assets/bin/Data/Managed/mscorlib.dll").unwrap().method, pkgzip::zip::DEFLATED);
    // a port folder made for the android route, then switched by hand: made again first
    let bj = out.join("port.build.json");
    let mut b = pyjson::loads(&read(&bj)).unwrap();
    b.as_dict_mut().unwrap().table("config").table("port").set("route", Val::s("android"));
    put(&bj, pyjson::dumps(&b, None, true));
    assert!(err_text(package(&repo(), &port::ps(&out), &opts, &mut quiet())).contains("run `port new` again"));
    let mut bad = package::armhf_files(&dn, &t.0).unwrap();
    bad.sort();
    assert_eq!(
        bad,
        [
            "donors/donor-2019.4.8f1-mono-armv7.apk!lib/armeabi-v7a/libmain.so",
            "donors/donor-2019.4.8f1-mono-armv7.apk!lib/armeabi-v7a/libmonobdwgc-2.0.so",
            "donors/donor-2019.4.8f1-mono-armv7.apk!lib/armeabi-v7a/libunity.so",
        ]
    );
}

#[test]
fn package_flipbook() {
    let t = tmp("flip");
    let (rt, opts) = release_parts(&t.0);
    let out = make(&t.0, "");
    assert!(!Zip::open(&package(&repo(), &port::ps(&out), &opts, &mut quiet()).unwrap()).has("mygame/uaflipbook.dll"));
    let out = make(&t.0, "[runtime_lowmem]\nUA_FLIPBOOK = '@flipbooks.txt'\n");
    assert_eq!(Zip::open(&package(&repo(), &port::ps(&out), &opts, &mut quiet()).unwrap()).read("mygame/uaflipbook.dll"), b"MZ flip-book");
    fs::remove_file(Path::new(&rt).join("runtime/host/out/uaflipbook.dll")).unwrap();
    assert!(err_text(package(&repo(), &port::ps(&out), &opts, &mut quiet()))
        .contains("the port sets UA_FLIPBOOK: build it (runtime/host/managed/build.sh <donor APK>)"));
    assert!(text::uses_flipbook(&d("[runtime]\nUA_FLIPBOOK = ''")));
    assert!(text::uses_flipbook(&d("[[questions]]\nid = 'BOOKS'\nenv = 'UA_FLIPBOOK'")));
    assert!(!text::uses_flipbook(&d("[runtime]\nUA_POINTER_CURSOR = '0'\n[runtime_low]\n[[questions]]\nid = 'CURSOR'\nenv = 'UA_POINTER_CURSOR'")));
}

/// Folders as macOS makes them on exFAT: `._<name>` beside every file, .DS_Store in every
/// folder, every file executable.
fn as_on_exfat(top: &Path) {
    let mut dirs = vec![top.to_path_buf()];
    while let Some(d) = dirs.pop() {
        let ents: Vec<PathBuf> = fs::read_dir(&d).unwrap().map(|e| e.unwrap().path()).collect();
        for p in ents {
            if p.is_dir() {
                dirs.push(p);
            } else {
                let n = p.file_name().unwrap().to_string_lossy().into_owned();
                put(&d.join(format!("._{n}")), b"\x00\x05\x16\x07\x00\x02\x00\x00Mac OS X        ");
                fs::set_permissions(&p, fs::Permissions::from_mode(0o777)).unwrap();
            }
        }
        put(&d.join(".DS_Store"), b"\x00\x00\x00\x01Bud1");
    }
}

#[test]
fn package_zip() {
    let t = tmp("zip");
    let (rt, opts) = release_parts(&t.0);
    let out = make(&t.0, "[runtime]\nUA_FLIPBOOK = '@flipbooks.txt'\n");
    let text_file = fs::read(repo().root.join("docs/porting.md")).unwrap();
    for (n, data) in [("libthing.so", [b"\x7fELF".to_vec(), vec![0; 60]].concat()), ("table.bin", vec![0; 64]), ("notes.txt", text_file.clone())] {
        put(&out.join("mygame").join(n), data);
    }
    let zp = package(&repo(), &port::ps(&out), &opts, &mut quiet()).unwrap();
    let z = Zip::open(&zp);
    for e in &z.a.entries {
        assert_eq!(e.method, pkgzip::zip::DEFLATED, "{}", e.name);
        let data = z.read(&e.name);
        assert_eq!(e.compress_size, port::zipw::deflate(&data, 9).unwrap().len() as u64, "{}", e.name);
        assert_eq!((e.date, e.time, e.create_system), port::zipw::dos((2020, 1, 1, 0, 0, 0)).into_tuple3(3), "{}", e.name);
    }
    assert!(port::zipw::deflate(&text_file, 9).unwrap().len() < port::zipw::deflate(&text_file, 6).unwrap().len());
    assert_eq!(z.names[0], "My Game.sh");
    let key = |n: &String| {
        let mut p: Vec<String> = n.split('/').map(String::from).collect();
        p.pop();
        (p, n.clone())
    };
    let mut sorted = z.names[1..].to_vec();
    sorted.sort_by_key(key);
    assert_eq!(z.names[1..], sorted[..]);
    let want: BTreeMap<&str, u32> = [
        ("My Game.sh", 0o644), ("mygame/ua_host", 0o755), ("mygame/setup/pmu", 0o755), ("mygame/setup/patchscript", 0o644),
        ("mygame/setup/update", 0o644), ("mygame/libthing.so", 0o755), ("mygame/table.bin", 0o644), ("mygame/notes.txt", 0o644),
        ("mygame/port.json", 0o644), ("mygame/uaflipbook.dll", 0o644), ("mygame/donor/donor-2019.4.8f1-mono-armv7.apk", 0o644),
        ("mygame/licenses/LICENSE.musl.txt", 0o644),
    ]
    .into_iter()
    .collect();
    for (n, m) in &want {
        assert_eq!(z.modes[*n], *m, "{n}");
    }
    as_on_exfat(&out);
    as_on_exfat(Path::new(&rt));
    as_on_exfat(&t.0.join("donors"));
    as_on_exfat(Path::new(opts.pmu.as_ref().unwrap()));
    let o2 = PackOpts { out: Some(port::ps(&t.0.join("rel2"))), submission: true, ..opts.clone() };
    let z2 = package(&repo(), &port::ps(&out), &o2, &mut quiet()).unwrap();
    assert!(fs::read(&zp).unwrap() == fs::read(&z2).unwrap(), "another zip from the same port on exFAT");
    let sub = t.0.join("rel2/submission/mygame");
    let mut junk = Vec::new();
    let mut dirs = vec![sub.parent().unwrap().to_path_buf()];
    while let Some(d) = dirs.pop() {
        for e in fs::read_dir(&d).unwrap() {
            let p = e.unwrap().path();
            let n = p.file_name().unwrap().to_string_lossy().into_owned();
            if port::mac_junk(&n) {
                junk.push(p.clone());
            }
            if p.is_dir() {
                dirs.push(p);
            }
        }
    }
    assert!(junk.is_empty(), "{junk:?}");
    for (n, m) in &want {
        let f = if *n == "mygame/port.json" { sub.join("port.json") } else { sub.join(n) };
        assert_eq!(exec(&f), *m == 0o755, "{}", f.display());
    }
}

trait Tuple3 {
    fn into_tuple3(self, sys: u8) -> (u16, u16, u8);
}
impl Tuple3 for (u16, u16) {
    fn into_tuple3(self, sys: u8) -> (u16, u16, u8) {
        (self.0, self.1, sys)
    }
}

#[test]
fn mono_arm64_net35() {
    // player.mono64_classlibs (set by hand: pmu port new runs no check): setup.env, the release's
    // classlibs and Mono's config for them, the validation
    let t = tmp("net35");
    let out = make(&t.0, "[port]\nroute = 'mono-arm64'\n[game]\nunity_versions = ['2017.4.40f1']\n[player]\nmono64_classlibs = true\n");
    assert!(read(&out.join("mygame/setup/setup.env")).contains("MONO64_CLASSLIBS=1"));
    let cfg = build_cfg(&out);
    assert!(matches!(cfg.dict("player").get("mono64_classlibs"), Some(Val::Bool(true))));
    let w = t.0.join("mono64");
    let cl = w.join("out/unity-2017.4-mbe/classlibs");
    for n in config::MONO64_CLASSLIBS.iter().chain(["System.Web.dll"].iter()) {
        put(&cl.join(n), format!("MZ {n}"));
    }
    put(&cl.join("etc/mono/config"), "<configuration/>");
    put(&cl.join("etc/mono/4.5/machine.config"), "<configuration/>");
    put(&cl.join("LICENSE.txt"), "MIT");
    let r = repo();
    let mut log = |_: &str| {};
    let zipdir = t.0.join("zip");
    let mut ctx = package::PackCtx {
        repo: &r,
        cfg: cfg.clone(),
        port_src: out.clone(),
        zipdir: zipdir.clone(),
        runtime: t.0.clone(),
        name: "mygame".into(),
        port_dir: zipdir.join("mygame"),
        licenses: zipdir.join("mygame/licenses"),
        modes: BTreeMap::new(),
        stale: vec![],
        log: &mut log,
    };
    package::mono64_classlibs(&mut ctx, &PackOpts::default(), "2017.4", &w, cl.parent().unwrap()).unwrap();
    let m = zipdir.join("mygame/setup/mono64/2017.4");
    let mut got: Vec<String> = fs::read_dir(m.join("classlibs")).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect();
    got.sort();
    let mut want: Vec<String> = config::MONO64_CLASSLIBS.iter().map(|s| s.to_string()).collect();
    want.sort();
    assert_eq!(got, want);
    assert!(m.join("etc/mono/4.5/machine.config").is_file());
    assert!(zipdir.join("mygame/licenses/LICENSE.mono-class-libraries-2017.4.txt").is_file());
    fs::remove_file(cl.join("Mono.Posix.dll")).unwrap();
    let e = err_text(package::mono64_classlibs(&mut ctx, &PackOpts { no_build: true, ..Default::default() }, "2017.4", &w, cl.parent().unwrap()));
    assert!(e.contains("Mono.Posix.dll"), "{e}");
    let mut bad = cfg.clone();
    bad.table("port").set("route", Val::s("android"));
    assert!(err_text(validate(&bad)).contains("player.mono64_classlibs"));
    bad = cfg.clone();
    bad.table("player").set("mono64_classlibs", Val::strs(&["x y"]));
    assert!(err_text(validate(&bad)).contains("a list of assembly file names"));
}

#[test]
fn files_own() {
    let t = tmp("own");
    let out = make(&t.0, "");
    let launcher = out.join("My Game.sh");
    let ps = out.join("mygame/setup/patchscript");
    put(&launcher, "#!/bin/bash\n# hand-made\n");
    put(&ps, "#!/bin/bash\n# hand-made setup\n");
    put(&out.join("mygame/setup.txt"), "changed\n");
    let mut cfg = merge(&d(BASE), &d("[files]\nown = ['My Game.sh', 'mygame/setup/patchscript']"));
    let cp = t.0.join("cfg.json");
    put(&cp, pyjson::dumps(&Val::Dict(cfg.clone()), None, true));
    let a = NewArgs { name: "mygame".into(), config: Some(port::ps(&cp)), out: Some(port::ps(&out)), ..Default::default() };
    let mut msgs = Vec::new();
    new(&repo(), &a, &mut |m: &str| msgs.push(m.to_string())).unwrap();
    assert_eq!(read(&launcher), "#!/bin/bash\n# hand-made\n");
    assert_eq!(read(&ps), "#!/bin/bash\n# hand-made setup\n");
    assert_ne!(read(&out.join("mygame/setup.txt")), "changed\n");
    assert!(msgs.join("\n").contains("My Game.sh: the port's own (files.own), left as it is"));
    let kept = read(&out.join(".port_files"));
    assert!(kept.split('\n').any(|l| l == "My Game.sh") && kept.split('\n').any(|l| l == "mygame/setup/patchscript"));
    cfg.table("files").set("own", Val::strs(&["../x"]));
    put(&cp, pyjson::dumps(&Val::Dict(cfg), None, true));
    assert!(err_text(new(&repo(), &a, &mut quiet())).contains("files.own"));
}

// ------------------------------------------------------------------ ApkRouteTest (from port.toml)

#[test]
fn apk_new_and_package() {
    let t = tmp("apk");
    let cp = t.0.join("apk.toml");
    put(
        &cp,
        r#"[port]
title = "My Game"
route = "apk"
[game]
data = "*.apk *.xapk *.apks *.aab *.zip"
build = "android"
unity_versions = ["2021.3.4f1"]
package = "com.test.game"
[player]
backend = "il2cpp"
abis = ["arm64-v8a", "armeabi-v7a"]
"#,
    );
    let a = NewArgs { name: "mygame".into(), config: Some(port::ps(&cp)), out: Some(port::ps(&t.0.join("port"))), ..Default::default() };
    let out = new(&repo(), &a, &mut quiet()).unwrap();
    let files = read(&out.join(".port_files"));
    for f in ["My Game.sh", "mygame/setup/patchscript", "mygame/setup/setup.env", "mygame/port.json", "mygame/README.md", "README.md", "mygame/gameinfo.xml", "testing_thread.txt"] {
        assert!(files.split('\n').any(|x| x == f), "{f}");
    }
    assert!(!files.contains("mygame/setup/convert.json"));
    bash_n(&out.join("My Game.sh"));
    bash_n(&out.join("mygame/setup/patchscript"));
    let env = read(&out.join("mygame/setup/setup.env"));
    for x in ["PORT_ROUTE=apk", "GAME_PACKAGE=com.test.game", "PLAYER_ABI=auto", "GAME_BUILD=android", "GAME_DATA='*.apk *.xapk *.apks *.aab *.zip'", "UNITY_VERSIONS=2021.3.4f1"] {
        assert!(env.contains(x), "{x}");
    }
    assert!(!env.contains("PACKAGE=com.pmunity"));
    let launcher = read(&out.join("My Game.sh"));
    assert!(text::launcher_warnings(&launcher).is_empty());
    for x in ["./ua_host64 \"$GAMEDIR\"", "/lib/ld-linux-armhf.so.3", "\"abi\""] {
        assert!(launcher.contains(x), "{x}");
    }
    let pj = pyjson::loads(&read(&out.join("mygame/port.json"))).unwrap();
    let at = pj.as_dict().unwrap().dict("attr").clone();
    assert!(at.list("reqs").is_empty() && at.str("inst").contains("nothing is downloaded"));
    assert!(read(&out.join("README.md")).contains("ARCH=arm64 runtime/build.sh"));
    // the release: both runtimes, each with its licences; the setup's pmu
    let rt = t.0.join("rt");
    for arch in ["", "arm64/"] {
        put(&rt.join(format!("runtime/host/out/{arch}ua_host")), format!("host {arch}"));
        put(&rt.join(format!("runtime/media/out/{arch}licenses/libvpx.copyright")), format!("BSD {arch}"));
    }
    put(&rt.join("runtime/thirdparty/libhybris/LICENSE.Apache2"), "Apache 2.0");
    let opts = PackOpts {
        out: Some(port::ps(&t.0.join("rel"))),
        runtime: Some(port::ps(&rt)),
        no_build: true,
        pmu: Some(stub_pmu(&t.0)),
        ..Default::default()
    };
    let z = Zip::open(&package(&repo(), &port::ps(&out), &opts, &mut quiet()).unwrap());
    assert_eq!(z.read("mygame/ua_host64"), b"host arm64/");
    assert!(z.read("mygame/licenses/LICENSE.libvpx.txt").ends_with(b"BSD arm64/"));
    assert!(!z.has("mygame/licenses/LICENSE.libvpx.ua_host64.txt"));
    for n in ["mygame/ua_host", "mygame/setup/pmu"] {
        assert!(z.has(n));
    }
    assert!(!z.has("mygame/setup/python.tar.gz"));
    // port new again from port.toml alone: the same setup
    let mut env_before = env;
    let a2 = NewArgs { name: "mygame".into(), out: Some(port::ps(&out)), ..Default::default() };
    // (made with --config: no port.toml in the folder; write one as the first `port new` would)
    let cfg = build_cfg(&out);
    put(&out.join("port.toml"), dump_toml(&cfg, &port::routes::route(&repo(), "apk").unwrap().unwrap(), &[]).unwrap());
    new(&repo(), &a2, &mut quiet()).unwrap();
    assert_eq!(read(&out.join("mygame/setup/setup.env")), env_before);
    env_before.clear();
    // an armeabi-v7a-only build: a 32-bit userland (port.json), ua_host alone
    put(&cp, read(&cp).replace("abis = [\"arm64-v8a\", \"armeabi-v7a\"]", "abis = [\"armeabi-v7a\"]"));
    let a3 = NewArgs { name: "v7game".into(), config: Some(port::ps(&cp)), out: Some(port::ps(&t.0.join("port2"))), ..Default::default() };
    let out2 = new(&repo(), &a3, &mut quiet()).unwrap();
    let pj = pyjson::loads(&read(&out2.join("v7game/port.json"))).unwrap();
    assert!(pj.as_dict().unwrap().dict("attr").get("reqs").unwrap().eq(&Val::strs(&["armhf"])));
    assert!(!read(&out2.join("README.md")).contains("ARCH=arm64"));
    let z = Zip::open(&package(&repo(), &port::ps(&out2), &PackOpts { out: Some(port::ps(&t.0.join("rel2"))), ..opts }, &mut quiet()).unwrap());
    assert!(z.has("v7game/ua_host") && !z.has("v7game/ua_host64"));
    // player.il2cpp is for the apk route with a Mono build
    put(&cp, read(&cp) + "il2cpp = true\n");
    assert!(err_text(new(&repo(), &a3, &mut quiet())).contains("player.il2cpp"));
}

// ------------------------------------------------------------------ routes, stale builds, metadata

#[test]
fn templates() {
    let rts = port::routes::routes(&repo()).unwrap();
    assert!(rts.iter().any(|(n, _)| n == "android"));
    for (_, r) in &rts {
        for (_, spec) in r.dict("files").iter() {
            let n = match spec {
                Val::Str(s) => s.clone(),
                Val::Dict(d) => d.str("template").to_string(),
                _ => panic!(),
            };
            port::routes::template_path(&repo(), r, &n).unwrap();
        }
    }
    assert!(port::routes::render("@@NOPE@@", &[], "t").is_err());
    assert_eq!(port::routes::render("a@@@X@@b@", &[("X", "1".into())], "t").unwrap(), "a@1b@");
}

#[test]
fn stale_build() {
    let t = tmp("stale");
    let root = &t.0;
    let built = root.join("tools/port/out/pmu/pmu");
    let src = root.join("setup-rs/crates/a/src/lib.rs");
    let lock = root.join("setup-rs/Cargo.lock");
    let skipped: Vec<PathBuf> = ["out", "tests", ".git"].iter().map(|d| root.join("setup-rs/crates/a").join(d).join("x.rs")).collect();
    for f in [&built, &src, &lock].into_iter().chain(skipped.iter()) {
        put(f, "x");
    }
    let at = |f: &Path, s: u64| {
        let t = std::time::UNIX_EPOCH + std::time::Duration::from_secs(s);
        fs::File::options().write(true).open(f).unwrap().set_modified(t).unwrap();
    };
    at(&built, 2000);
    at(&src, 1000);
    at(&lock, 1000);
    for f in &skipped {
        at(f, 3000);
    }
    let mut st = Vec::new();
    package::stale_build(&mut st, "pmu", &built, root, "setup-rs/tools/build_pmu.sh tools/port/out/pmu");
    assert!(st.is_empty());
    at(&src, 2500);
    at(&lock, 2600);
    assert_eq!(package::newer_sources(&built, root, package::build_sources("pmu")), ["setup-rs/Cargo.lock", "setup-rs/crates/a/src/lib.rs"]);
    package::stale_build(&mut st, "pmu", &built, root, "setup-rs/tools/build_pmu.sh tools/port/out/pmu");
    assert_eq!(st.len(), 1);
    assert!(st[0].contains("tools/port/out/pmu/pmu is older than setup-rs/Cargo.lock and 1 more"));
    assert!(st[0].contains("setup-rs/tools/build_pmu.sh tools/port/out/pmu"));
}

fn mcfg(over: &str) -> Dict {
    let c = build_config("mygame", &merge(&d("[game]\nunity_versions = ['2020.3.1f1']"), &d(over)));
    validate(&c).unwrap();
    c
}

#[test]
fn port_json() {
    let cfg = mcfg(
        "[port]\ntitle = 'My Game'\nporter = 'someone'\ngenres = ['Platformer', 'visual novel']\ndesc_md = '**bold**'\nreqs = ['2gb', '!lowres']\n\
         store = [{name = 'Steam', gameurl = 'https://example.com/app/1'}]\n",
    );
    let pj = text::port_json(&cfg);
    let a = pj.as_dict().unwrap().dict("attr");
    let keys: Vec<&str> = a.keys().collect();
    assert_eq!(keys, ["title", "porter", "desc", "desc_md", "inst", "inst_md", "genres", "image", "rtr", "exp", "runtime", "store", "availability", "reqs", "arch", "min_glibc"]);
    assert!(a.get("porter").unwrap().eq(&Val::strs(&["someone"])));
    assert!(a.get("genres").unwrap().eq(&Val::strs(&["platformer", "visual novel"])));
    assert!(a.get("inst_md").unwrap().eq(&Val::Null) && a.str("desc_md") == "**bold**");
    assert_eq!(pyjson::dumps(a.get("store").unwrap(), None, true), r#"[{"name": "Steam", "gameurl": "https://example.com/app/1", "developerurl": ""}]"#);
    assert_eq!(a.str("availability"), "paid");
    assert!(a.get("reqs").unwrap().eq(&Val::strs(&["armhf", "2gb", "!lowres"])));
    assert!(matches!(a.get("exp"), Some(Val::Bool(false))) && matches!(a.get("rtr"), Some(Val::Bool(false))));
    assert_eq!(mcfg("").dict("port").str("availability"), "free");
    assert_eq!(mcfg("[port]\navailability = 'demo'").dict("port").str("availability"), "demo");
}

#[test]
fn validate_metadata() {
    let bad = build_config(
        "g",
        &d("[game]\nunity_versions = ['2020.3.1f1']\nrelease_date = '29/09/2017'\n[port]\ngenres = ['shooter']\navailability = 'maybe'\n\
            store = ['https://example.com']\nreqs = ['Not Lowres']\nscreenshot = 'shot.bmp'\n[controls]\nA = ''\n"),
    );
    let msg = err_text(validate(&bad));
    for part in ["'shooter'", "port.availability", "port.store", "'Not Lowres'", "release_date", "port.screenshot", "controls.A"] {
        assert!(msg.contains(part), "{part}");
    }
}

#[test]
fn gameinfo() {
    let cfg = mcfg("[port]\ntitle = 'My Game'\ngenres = ['rpg']\ndesc = 'Short.'\n[game]\ndeveloper = 'Studio & Co'\nrelease_date = '2017-09-29'\ndescription = 'A <long> one.'\n");
    let xml = text::gameinfo_xml(&cfg, "cover.png", None);
    assert_eq!(
        xml,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<gameList>\n  <game>\n    <path>./My Game.sh</path>\n    <name>My Game</name>\n    \
         <desc>A &lt;long&gt; one.</desc>\n    <releasedate>20170929T000000</releasedate>\n    <developer>Studio &amp; Co</developer>\n    \
         <genre>RPG</genre>\n    <image>./mygame/cover.png</image>\n  </game>\n</gameList>\n"
    );
    let t = tmp("gi");
    assert_eq!(text::gameinfo_image(&cfg, &port::ps(&t.0)), "screenshot.png");
    write_png(&t.0.join("cover.png"), 4, 3);
    assert_eq!(text::gameinfo_image(&cfg, &port::ps(&t.0)), "cover.png");
}

#[test]
fn controls_and_thanks() {
    let mut cfg = mcfg(
        "[port]\ntitle = 'G'\nstore = [{name = 'itch.io', gameurl = 'https://x.example/g', developerurl = 'https://x.example'}]\n\
         [game]\ndeveloper = 'X'\n[controls]\nD-Pad = 'Move'\nB = 'Jump | climb'\n",
    );
    let rows = text::controls_rows(&cfg);
    assert_eq!(rows, [("D-Pad".into(), "Move".into()), ("B".into(), "Jump | climb".into()), ("Select + Start".to_string(), "Quit".to_string())]);
    let md = text::controls_markdown(&cfg);
    assert!(md.starts_with("| Button | Action |\n|--|--|\n| D-Pad | Move |") && md.contains("| B | Jump \\| climb |"));
    assert_eq!(text::thanks_sentence(&cfg), "Thanks to [X](https://x.example) for creating G.");
    cfg.table("controls").set("Select + Start", Val::s("Back to the menu"));
    assert_eq!(text::controls_rows(&cfg).last().unwrap().1, "Back to the menu");
    assert!(text::controls_markdown(&mcfg("")).contains("keeps its own gamepad controls"));
    assert_eq!(text::thanks_sentence(&mcfg("[port]\nthanks = 'Thanks!'")), "Thanks!");
}

#[test]
fn testing_thread() {
    let cfg = mcfg("[port]\ntitle = 'My Game'\n[game]\nurl = 'https://example.com/g'\n[controls]\nA = 'Jump'\n");
    let tt = text::testing_thread_txt(&cfg, "Requires the game.");
    for part in [
        "mygame.zip", "harbourmaster install", "Title: My Game", "URL: https://example.com/g",
        "Instructions:\nRequires the game.\nNeeds a handheld with a 32-bit (armhf) userland", "| A | Jump |", "| Select + Start | Quit |",
        "ROCKNIX\n-> [ ] Libmali", "[ ] Knulli", "[ ] 640x480",
    ] {
        assert!(tt.contains(part), "{part}");
    }
}

#[test]
fn images() {
    let t = tmp("img");
    let png = t.0.join("a.png");
    write_png(&png, 3, 2);
    assert_eq!(text::image_size(&port::ps(&png)), Some((3, 2)));
    let jpg = t.0.join("b.jpg");
    put(&jpg, [b"\xff\xd8\xff\xe0\x00\x04xx\xff\xc0\x00\x11\x08\x01\xe0\x02\x80\x03".to_vec(), vec![0; 20]].concat());
    assert_eq!(text::image_size(&port::ps(&jpg)), Some((640, 480)));
    assert_eq!(text::image_size(&port::ps(&t.0.join("none.png"))), None);
    let cfg = mcfg("[port]\nscreenshot = 'b.jpg'");
    let imgs = text::port_images(&cfg, &port::ps(&t.0));
    assert_eq!(imgs[0].1, Some((port::ps(&jpg), "screenshot.jpg".to_string())));
    assert_eq!(imgs[1].1, None);
    let w = text::submission_warnings(&cfg, &port::ps(&t.0), None, None, None).join("\n");
    assert!(!w.contains("no screenshot") && !w.contains("the screenshot is") && w.contains("no cover"));
    write_png(&t.0.join("screenshot.png"), 720, 480);
    assert!(text::submission_warnings(&mcfg(""), &port::ps(&t.0), None, None, None).join("\n").contains("the screenshot is 720x480"));
}

#[test]
fn submission_warnings() {
    let cfg = build_config("my_game", &d("[game]\nunity_versions = ['2020.3.1f1']\n[port]\ndesc = 'Fast \u{2014} and **bold**.'\nexp = true\n"));
    validate(&cfg).unwrap();
    let t = tmp("subw");
    write_png(&t.0.join("screenshot.png"), 640, 480);
    let gi = text::gameinfo_xml(&cfg, "cover.png", None);
    let w = text::submission_warnings(&cfg, &port::ps(&t.0), Some(&text::port_json(&cfg)), None, Some(&gi)).join("\n");
    for part in ["port.porter is empty", "port.json desc: plain text", "an em dash", "port.exp = true", "port.name 'my_game'", "game.publisher is empty", "gameinfo.xml names cover.png"] {
        assert!(w.contains(part), "{part}");
    }
    assert!(!w.contains("port.desc is empty"));
    assert!(!w.replace("shows the screenshot", "").contains("screenshot"));
}

#[test]
fn launchers() {
    let rts = port::routes::routes(&repo()).unwrap();
    for (name, r) in &rts {
        if let Some(spec) = r.dict("files").get("@@TITLE@@.sh") {
            let n = match spec {
                Val::Str(s) => s.clone(),
                Val::Dict(d) => d.str("template").to_string(),
                _ => panic!(),
            };
            let t = port::routes::read_text(&port::routes::template_path(&repo(), r, &n).unwrap()).unwrap();
            assert!(text::launcher_warnings(&t).is_empty(), "{name}");
        }
    }
    let android = &rts.iter().find(|(n, _)| n == "android").unwrap().1;
    let good = port::routes::read_text(&port::routes::template_path(&repo(), android, "launcher.sh").unwrap()).unwrap();
    let bad = good.replace("\nget_controls\n", "\n").replace("$GPTOKEYB2 \"ua_host\"", "\"$GPTOKEYB2\" \"ua_host\"").replace("pm_finish", "true")
        + "export SDL_VIDEODRIVER=kmsdrm\n";
    let w = text::launcher_warnings(&bad).join("\n");
    for part in ["standard lines", "quoted", "SDL_VIDEODRIVER", "pm_finish"] {
        assert!(w.contains(part), "{part}");
    }
    let m = "[ -f \"${controlfolder}/mod_${CFW_NAME}.txt\" ] && source \"${controlfolder}/mod_${CFW_NAME}.txt\"\n";
    assert!(good.contains(&format!("\nsource $controlfolder/control.txt\n{m}get_controls\n")));
    assert!(text::launcher_warnings(&good.replace(m, "")).join("\n").contains("mod_${CFW_NAME}.txt"));
}

#[test]
fn metadata_toml_round_trip() {
    let cfg = mcfg("[port]\nstore = [{name = 'GOG', gameurl = 'https://example.com', developerurl = ''}]\n[controls]\nD-Pad = 'Move'\n'L1 + R1' = 'Map'\n");
    let back = d(&dump_toml(&cfg, &Dict::new(), &[]).unwrap());
    assert!(Val::Dict(merge(&config::defaults(), &back)).eq(&Val::Dict(cfg)));
    let keys: Vec<&str> = back.dict("controls").keys().collect();
    assert_eq!(keys, ["D-Pad", "L1 + R1"]);
}

#[test]
fn elf_debug_bytes() {
    fn elf(bits: u32, sections: &[(&str, usize)]) -> Vec<u8> {
        let mut names = vec![0u8];
        let hdr = if bits == 64 { 64 } else { 52 };
        let mut body = Vec::new();
        let mut at = Vec::new();
        let mut pos = 1u32;
        for (n, size) in sections {
            at.push((pos, (hdr + body.len()) as u64, *size as u64));
            body.extend(std::iter::repeat_n(0u8, *size));
            names.extend_from_slice(n.as_bytes());
            names.push(0);
            pos += n.len() as u32 + 1;
        }
        names.extend_from_slice(b".shstrtab\0");
        at.push((pos, (hdr + body.len()) as u64, names.len() as u64));
        body.extend_from_slice(&names);
        let mut heads = Vec::new();
        for (name, off, size) in std::iter::once((0, 0, 0)).chain(at.iter().copied()) {
            if bits == 64 {
                for (v, w) in [(name as u64, 4), (1, 4), (0, 8), (0, 8), (off, 8), (size, 8), (0, 4), (0, 4), (1, 8), (0, 8)] {
                    heads.extend_from_slice(&v.to_le_bytes()[..w]);
                }
            } else {
                for v in [name, 1, 0, 0, off as u32, size as u32, 0, 0, 1, 0] {
                    heads.extend_from_slice(&v.to_le_bytes());
                }
            }
        }
        let num = (at.len() + 1) as u16;
        let mut h = b"\x7fELF".to_vec();
        h.extend_from_slice(&[if bits == 64 { 2 } else { 1 }, 1, 1]);
        h.extend_from_slice(&[0; 9]);
        let shoff = (hdr + body.len()) as u64;
        h.extend_from_slice(&2u16.to_le_bytes());
        h.extend_from_slice(&(if bits == 64 { 183u16 } else { 40 }).to_le_bytes());
        h.extend_from_slice(&1u32.to_le_bytes());
        if bits == 64 {
            h.extend_from_slice(&0u64.to_le_bytes());
            h.extend_from_slice(&0u64.to_le_bytes());
            h.extend_from_slice(&shoff.to_le_bytes());
        } else {
            h.extend_from_slice(&0u32.to_le_bytes());
            h.extend_from_slice(&0u32.to_le_bytes());
            h.extend_from_slice(&(shoff as u32).to_le_bytes());
        }
        h.extend_from_slice(&0u32.to_le_bytes());
        for v in [hdr as u16, 0, 0, if bits == 64 { 64 } else { 40 }, num, num - 1] {
            h.extend_from_slice(&v.to_le_bytes());
        }
        assert_eq!(h.len(), hdr);
        [h, body, heads].concat()
    }
    let t = tmp("elf");
    for bits in [32, 64] {
        let a = t.0.join(format!("with{bits}"));
        put(&a, elf(bits, &[(".text", 100), (".debug_info", 3000), (".symtab", 64), (".debug_line", 500)]));
        assert_eq!(package::elf_debug_bytes(&a), 3500);
        let b = t.0.join(format!("stripped{bits}"));
        put(&b, elf(bits, &[(".text", 100), (".dynsym", 64)]));
        assert_eq!(package::elf_debug_bytes(&b), 0);
    }
    put(&t.0.join("text"), "#!/bin/sh\necho\n");
    put(&t.0.join("cut"), &elf(32, &[(".debug_info", 3000)])[..60]);
    assert_eq!(package::elf_debug_bytes(&t.0.join("text")), 0);
    assert_eq!(package::elf_debug_bytes(&t.0.join("cut")), 0);
    assert_eq!(package::elf_debug_bytes(&t.0.join("missing")), 0);
}

// ------------------------------------------------------------------ test_route.py: the box64 hook

#[test]
fn box64_port_new_hook() {
    let t = tmp("box64");
    let out = make(&t.0, "[port]\nroute = 'box64'\n[runtime]\nPMU_GL = 'mesa'\n");
    let gles = read(&out.join("mygame/setup/gles.json"));
    assert!(gles.contains("\"graphics_apis\": [11, 17]"), "{gles}");
    let pj = pyjson::loads(&read(&out.join("mygame/port.json"))).unwrap();
    let a = pj.as_dict().unwrap().dict("attr");
    assert!(a.get("runtime").unwrap().eq(&Val::strs(&["weston_pkg_0.2.squashfs"])));
    assert!(a.list("reqs").is_empty());
    assert_eq!(a.str("min_glibc"), "2.29");
    let env = read(&out.join("mygame/port.env"));
    assert!(env.contains("export PMU_GL=mesa") && env.contains("#PMU_BOX64=default"));
    // the release takes box64's build ($BOX64_OUT, else <runtime>/tools/box64/out)
    let (rt, opts) = release_parts(&t.0);
    let rt = PathBuf::from(rt);
    let b = rt.join("tools/box64/out");
    for n in ["box64/box64", "box64/x64lib/libx.so", "glxes/libGL.so.1", "gl4es/libGL.so.1", "licenses/LICENSE.box64.txt"] {
        put(&b.join(n), n);
    }
    for n in ["box64.env", "unityprefs.sh"] {
        put(&rt.join("tools/box64").join(n), n);
    }
    if std::env::var_os("BOX64_OUT").is_none() {
        let z = Zip::open(&package(&repo(), &port::ps(&out), &opts, &mut quiet()).unwrap());
        for n in ["mygame/box64/box64", "mygame/box64/x64lib/libx.so", "mygame/glxes/libGL.so.1", "mygame/box64/box64.env", "mygame/licenses/LICENSE.box64.txt"] {
            assert!(z.has(n), "{n}");
        }
        assert_eq!(z.modes["mygame/box64/box64"], 0o755);
        assert!(!z.has("mygame/ua_host"));
    }
}

#[test]
fn route_and_pmunity_repo_found() {
    let r = Repo::find(&[PathBuf::from(env!("CARGO_MANIFEST_DIR"))]).unwrap();
    assert!(r.templates().join("android/route.json").is_file());
    let _ = Read::bytes(&b""[..]);
}
