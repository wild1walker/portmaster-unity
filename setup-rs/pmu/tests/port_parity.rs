//! `pmu port new` / `pmu port package` on synthetic inputs (stand-ins for the runtime, pmu, the
//! donors; no game or Unity data), frozen in tests/golden/port.txt (port_golden below). Until
//! 2026-11-27 each case also ran `python -m pmunity port new` / `package` at the same paths and every
//! output was compared byte for byte (the port_parity test, removed with convert/): the golden file
//! was written from such a run. PMU_PARITY_DIR: where the cases run (kept when set).
use std::collections::BTreeMap;
use std::fs;
use std::io::{Read, Write};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Command;

fn wt() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap()
}

fn put(p: &Path, data: impl AsRef<[u8]>) {
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    fs::write(p, data).unwrap();
}

fn chmod(p: &Path, m: u32) {
    fs::set_permissions(p, fs::Permissions::from_mode(m)).unwrap();
}

fn elf(machine: u16, bits64: bool) -> Vec<u8> {
    let mut h = vec![0u8; 64];
    h[..4].copy_from_slice(b"\x7fELF");
    h[4] = if bits64 { 2 } else { 1 };
    h[5] = 1;
    h[6] = 1;
    h[18..20].copy_from_slice(&machine.to_le_bytes());
    h.extend(std::iter::repeat_n(b'Z', 3000));
    h
}

fn zip(p: &Path, entries: &[(&str, &[u8])], deflate: bool) {
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    let mut z = port::zipw::ZipOut::create(&p.to_string_lossy()).unwrap();
    for (n, d) in entries {
        let mut i = port::zipw::Info::file(n, 0o644, 6);
        if !deflate {
            i.method = port::zipw::STORED;
        }
        z.add(i, d).unwrap();
    }
    z.finish().unwrap();
}

fn png(p: &Path, w: u32, h: u32) {
    let mut raw = Vec::new();
    for y in 0..h {
        raw.push(0);
        raw.extend((0..w * 3).map(|x| ((x + y) % 251) as u8));
    }
    let mut e = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::default());
    e.write_all(&raw).unwrap();
    let idat = e.finish().unwrap();
    let chunk = |t: &[u8], data: &[u8]| {
        let mut c = (data.len() as u32).to_be_bytes().to_vec();
        c.extend_from_slice(t);
        c.extend_from_slice(data);
        let mut hh = crc32fast::Hasher::new();
        hh.update(t);
        hh.update(data);
        c.extend_from_slice(&hh.finalize().to_be_bytes());
        c
    };
    let mut ihdr = w.to_be_bytes().to_vec();
    ihdr.extend_from_slice(&h.to_be_bytes());
    ihdr.extend_from_slice(&[8, 2, 0, 0, 0]);
    let mut out = b"\x89PNG\r\n\x1a\n".to_vec();
    out.extend(chunk(b"IHDR", &ihdr));
    out.extend(chunk(b"IDAT", &idat));
    out.extend(chunk(b"IEND", b""));
    put(p, out);
}

/// The inputs every case may use, under <w>/in: configs, the runtime's stand-ins (ua_host for both
/// architectures, the decoders' licences, libhybris's, the flip-book stand-in, the Mono bridge),
/// pmu's folder, the setup's Python, donors, Unity's Mono builds, asmpatch with its .NET runtime
/// pack, pmhlslcc, pmspirvcross, box64's build, a porter's plugin, hook, extra file and images.
fn inputs(w: &Path) {
    let i = w.join("in");
    let rt = i.join("rt");
    put(&rt.join("runtime/host/out/ua_host"), elf(40, false));
    put(&rt.join("runtime/host/out/uaflipbook.dll"), "MZ flip-book");
    put(&rt.join("runtime/host/out/arm64/ua_host"), elf(183, true));
    put(&rt.join("runtime/media/out/licenses/libvpx.copyright"), "BSD libvpx\r\nwith a CRLF line\n");
    put(&rt.join("runtime/media/out/licenses/openh264.LICENSE"), "BSD openh264\n");
    put(&rt.join("runtime/media/out/arm64/licenses/libvpx.copyright"), "BSD libvpx 64\n");
    put(&rt.join("runtime/media/out/arm64/licenses/openh264.LICENSE"), "BSD openh264\n");
    put(&rt.join("runtime/thirdparty/libhybris/LICENSE.Apache2"), "Apache 2.0 libhybris\n");
    put(&rt.join("runtime/mono64/out/arm64/libil2cpp_mono.so"), elf(183, true));
    put(&rt.join("runtime/relayout/0123abcd.txt"), "# a code layout list\n");
    put(&rt.join("runtime/relayout/README.md"), "# not shipped\n");
    put(&rt.join("tools/asmpatch/out/linux-arm64/asmpatch"), elf(183, true));
    put(&rt.join("tools/hlslcc/out/linux-arm64/pmhlslcc"), elf(183, true));
    put(&rt.join("tools/hlslcc/out/linux-arm64/pmhlslcc.LICENSE.txt"), "MIT HLSLcc\n");
    put(&rt.join("tools/spirvcross/out/linux-arm64/pmspirvcross"), elf(183, true));
    put(&rt.join("tools/spirvcross/out/linux-arm64/pmspirvcross.LICENSE.txt"), "Apache SPIRV-Cross\n");
    for n in ["box64.env", "unityprefs.sh"] {
        put(&rt.join("tools/box64").join(n), format!("# {n}\n"));
    }
    let b = i.join("box64out");
    put(&b.join("box64/box64"), elf(183, true));
    put(&b.join("box64/x64lib/libstdc++.so.6"), elf(62, true));
    put(&b.join("glxes/libGL.so.1"), elf(183, true));
    put(&b.join("gl4es/libGL.so.1"), elf(183, true));
    put(&b.join("licenses/LICENSE.box64.txt"), "MIT box64\n");
    let nuget = i.join("nuget/microsoft.netcore.app.runtime.linux-arm64");
    for v in ["8.0.2", "8.0.11", "7.0.1"] {
        put(&nuget.join(v).join("LICENSE.TXT"), format!("MIT .NET {v}\n"));
        put(&nuget.join(v).join("THIRD-PARTY-NOTICES.TXT"), format!("notices {v}\n"));
    }
    let pmu = i.join("pmu-out");
    put(&pmu.join("pmu"), elf(183, true));
    put(&pmu.join("licenses/LICENSE.musl.txt"), "MIT musl\n");
    put(&pmu.join("licenses/LICENSE.rust-crates.txt"), "MIT / Apache 2.0 crates\n");
    put(&pmu.join("licenses/._LICENSE.musl.txt"), "AppleDouble");
    let w64 = i.join("mono64");
    for line in ["2019.4", "2017.4"] {
        let br = format!("unity-{line}-mbe");
        put(&w64.join("out").join(&br).join("libmonobdwgc-2.0.so"), elf(183, true));
        put(&w64.join("out").join(&br).join("libMonoPosixHelper.so"), elf(183, true));
        put(&w64.join("src").join(&br).join("LICENSE"), "MIT mono\n");
        put(&w64.join("src").join(&br).join("PATENTS.TXT"), "patents\n");
        put(&w64.join("src").join(&br).join("external/bdwgc/README.QUICK"), "bdwgc\n");
        let cl = w64.join("out").join(&br).join("classlibs");
        for n in port::config::MONO64_CLASSLIBS.iter().chain(["System.Web.dll"].iter()) {
            put(&cl.join(n), format!("MZ {n}"));
        }
        put(&cl.join("etc/mono/config"), "<configuration/>\n");
        put(&cl.join("etc/mono/4.5/machine.config"), "<configuration/>\n");
        put(&cl.join("LICENSE.txt"), "MIT class libraries\n");
    }
    let dn = i.join("donors");
    let e32 = elf(40, false);
    let e64 = elf(183, true);
    let text: Vec<u8> = (0..20000u32).flat_map(|x| format!("line {x} of the engine\n").into_bytes()).collect();
    for v in ["2019.4.8f1", "2019.4.40f1", "2021.3.4f1", "2022.2.17f1", "2017.4.40f1"] {
        zip(
            &dn.join(format!("donor-{v}-mono-armv7.apk")),
            &[
                ("AndroidManifest.xml", b"<manifest/>"),
                ("assets/bin/Data/Managed/UnityEngine.dll", &text),
                ("assets/bin/Data/Managed/mscorlib.dll", b"MZ corlib"),
                ("lib/armeabi-v7a/libmain.so", &e32),
                ("lib/armeabi-v7a/libunity.so", &e32),
                ("lib/armeabi-v7a/libmonobdwgc-2.0.so", &e32),
            ],
            true,
        );
        zip(
            &dn.join(format!("donor-{v}-il2cpp-arm64.apk")),
            &[("lib/arm64-v8a/libmain.so", &e64), ("lib/arm64-v8a/libunity.so", &e64), ("lib/arm64-v8a/libil2cpp.so", &e64)],
            false,
        );
    }
    put(&i.join("files/animclips.py"), "# a plugin pmu has built in\n");
    put(&i.join("files/tweaks.json"), "{\"edits\": []}\n");
    put(&i.join("files/mystep.py"), "STEPS = {}\n");
    put(&i.join("files/Patch.dll"), "MZ patch");
    chmod(&i.join("files/Patch.dll"), 0o755);
    put(&i.join("files/game.sh"), "# a game hook\n");
    put(&i.join("files/notes.txt"), "extra notes\n");
    put(&i.join("player/Variations/mono/Release/Libs/armeabi-v7a/libunity.so"), "libunity bytes");
    put(&i.join("player/Variations/mono/Release/Libs/armeabi-v7a/libmain.so"), "libmain bytes");
    // one time for every input: `port package` warns of a build older than a source of its checkout, and
    // the order these files were written in must not decide it (the golden file's lines)
    let t = std::time::UNIX_EPOCH + std::time::Duration::from_secs(1_790_000_000);
    let mut stack = vec![i];
    while let Some(d) = stack.pop() {
        for e in fs::read_dir(&d).unwrap().flatten() {
            let p = e.path();
            if p.is_dir() {
                stack.push(p);
            } else if let Ok(f) = fs::File::options().write(true).open(&p) {
                let _ = f.set_modified(t);
            }
        }
    }
}

/// A case's port.toml (or .json) and the commands it runs ("{py}" is the implementation's command).
struct Case {
    name: &'static str,
    files: Vec<(&'static str, String)>,
    cmds: Vec<Vec<String>>,
    env: Vec<(&'static str, String)>,
    /// files made in the port folder before the case's later commands (as a porter adds them)
    after_new: Vec<(&'static str, Vec<u8>)>,
}

fn sv(xs: &[&str]) -> Vec<String> {
    xs.iter().map(|s| s.to_string()).collect()
}

const ANDROID: &str = r#"
[port]
name = "mygame"
title = "My Game"
porter = ["someone", "else"]
desc = "A game, café style — with a dash."
desc_md = "**A game**"
genres = ["Platformer", "visual novel"]
store = [{name = "Steam", gameurl = "https://example.com/app/1", developerurl = "https://example.com/dev"}]
reqs = ["2gb", "!lowres", "armhf"]
thanks = "Thanks to the makers."
[game]
data = "My Game_Data"
unity_versions = ["2019.4.8f1"]
size_mb = 450
developer = "Studio & Co"
publisher = "Pub <x>"
release_date = "2017-09-29"
url = "https://example.com/g"
[controls]
"D-Pad" = "Move"
"B (bottom)" = "Jump | climb"
[player]
sha256 = {"libunity.so" = "ab", "libmain.so" = "cd"}
[convert]
steps = ["shaders", "settings", "audio", "quality", "textures"]
[convert.textures]
quality = "medium"
scale = 0.375
[convert.shaders]
grab_fetch = "always"
[android_data]
asmpatch = ["auto", "hk-blur"]
streaming_assets = "both"
args = ["--rewired-joysticks", "2", "it's"]
[wwise]
banks = "StreamingAssets/Audio/GeneratedSoundBanks/Linux"
[runtime]
UA_POINTER = "1"
UA_KEYMAP = "start:escape,x:space"
UA_FLIPBOOK = ""
[runtime_low]
UA_RENDER_PIXELS = "560000"
[runtime_lowmem]
UA_X = "a b"
[files]
plugins = ["../in/files/animclips.py", "../in/files/tweaks.json"]
managed = ["../in/files/Patch.dll"]
assembly_patches = [{name = "Assembly-CSharp.dll", command = "{setup}/files/patch {managed} {out}", files = ["../in/files/notes.txt"]}]
hooks = {game = "../in/files/game.sh"}
extra = ["../in/files/notes.txt"]
[[questions]]
id = "TEXTURE_QUALITY"
question = "Texture quality? \"High\" is tested."
options = [["medium", "High (recommended)"], ["fast", "Low"]]
default = "medium"
convert_set = "textures.quality"
[[questions]]
id = "CURSOR"
question = "Cursor?"
options = [["1", "On"], ["0"]]
env = "UA_POINTER_CURSOR"
[[questions]]
id = "PICTURE"
question = "Picture?"
options = [["high", "High"]]
convert_set = "quality.profile"
"#;

fn cases(w: &Path) -> Vec<Case> {
    let i = w.join("in");
    let s = |p: &Path| p.to_string_lossy().into_owned();
    let rt = s(&i.join("rt"));
    let dn = s(&i.join("donors"));
    let pmu = s(&i.join("pmu-out"));
    let pkg = |port: &str, extra: &[&str]| -> Vec<String> {
        let mut v = sv(&["port", "package", port, "--out", &format!("{port}-rel"), "--runtime", &rt, "--donors", &dn, "--pmu", &pmu, "--no-build"]);
        v.extend(sv(extra));
        v
    };
    let new = |cfg: &str, out: &str, extra: &[&str]| -> Vec<String> {
        let mut v = sv(&["port", "new", "mygame", "--config", cfg, "--out", out]);
        v.extend(sv(extra));
        v
    };
    let mut out = vec![
        Case {
            name: "android",
            files: vec![("cfg.toml", ANDROID.to_string())],
            cmds: vec![new("cfg.toml", "port", &[]), pkg("port", &["--submission"]), new("cfg.toml", "port", &["--player", &s(&i.join("player"))])],
            env: vec![("NUGET_PACKAGES", s(&i.join("nuget")))],
            after_new: vec![],
        },
        Case {
            name: "images",
            files: vec![("cfg.toml", ANDROID.replace("thanks = \"Thanks to the makers.\"\n", "").replace("[controls]\n", "[controls_x]\n"))],
            cmds: vec![new("cfg.toml", "port", &[]), new("cfg.toml", "port", &[]), pkg("port", &["--submission"])],
            env: vec![("NUGET_PACKAGES", s(&i.join("nuget")))],
            after_new: vec![
                ("port/screenshot.png", vec![]),
                ("port/cover.png", vec![]),
                ("port/LICENSE.md", b"MIT port\n".to_vec()),
                ("port/README.md", b"## Notes\nkept\n".to_vec()),
            ],
        },
        Case {
            name: "aarch64-only",
            files: vec![(
                "cfg.toml",
                format!(
                    "[port]\ntitle = 'My Game'\nroute = 'android'\naarch64_only = true\n[game]\nunity_versions = ['2019.4.8f1']\n\
                     [player]\nbackend = 'mono'\nabi = 'armeabi-v7a'\nsha256 = {{'libunity.so' = '{}'}}\n[runtime]\nUA_FLIPBOOK = ''\n\
                     [build]\ncommands = ['printf x > \"$BUILD/x\"', 'cp arm32.so \"$BUILD/a32\"']\n\
                     [build.outputs]\n'pmhwdec/libpmhwdec.so.1' = 'arm32.so'\n'pmhwdec64/libpmhwdec.so.1' = 'arm64.so'\ntools = 'tools'\n'setup/files/x' = '{{build}}/x'\n\
                     [build.licenses]\n'LICENSE.x.txt' = '{{build}}/x'\n",
                    "00".repeat(32)
                ),
            )],
            cmds: vec![new("cfg.toml", "port", &[]), pkg("port", &["--mono64-work", &s(&i.join("mono64"))])],
            env: vec![],
            after_new: vec![
                ("port/arm32.so", elf(40, false)),
                ("port/arm64.so", elf(183, true)),
                ("port/tools/helper32", elf(40, false)),
                ("port/tools/helper64", elf(183, true)),
                ("port/tools/run.sh", b"#!/bin/sh\n".to_vec()),
            ],
        },
        Case {
            name: "il2cpp-arm64-d3d11",
            files: vec![(
                "cfg.json",
                r#"{"port": {"title": "Win Game", "route": "il2cpp-arm64", "porter": "one"}, "game": {"unity_versions": ["2022.2.17f1"], "build": "windows", "data": "Win Game_Data"},
                    "convert": {"shaders": {"from": ["d3d11"]}}, "android_data": {"asmpatch": "auto", "data_path": true}, "player": {"abi": "arm64-v8a"}}"#.to_string(),
            )],
            cmds: vec![new("cfg.json", "port", &["--title", "Other Title"]), pkg("port", &[])],
            env: vec![("NUGET_PACKAGES", s(&i.join("nuget")))],
            after_new: vec![],
        },
        Case {
            name: "windows-android",
            files: vec![("cfg.toml", "[port]\ntitle = 'Win'\n[game]\nunity_versions = ['2022.2.17f1']\nbuild = 'windows'\n[convert.shaders]\nfrom = ['d3d11']\n".to_string())],
            cmds: vec![new("cfg.toml", "port", &[]), pkg("port", &[])],
            env: vec![],
            after_new: vec![],
        },
        Case {
            name: "vulkan-own-plugin",
            files: vec![(
                "cfg.toml",
                "[port]\ntitle = 'Vk'\n[game]\nunity_versions = ['2021.3.4f1']\nsize_mb = 2000\n[convert.shaders]\nfrom = 'vulkan,gles'\n[files]\nplugins = ['../in/files/mystep.py']\n".to_string(),
            )],
            cmds: vec![new("cfg.toml", "port", &[]), pkg("port", &[])],
            env: vec![],
            after_new: vec![],
        },
        Case {
            name: "apk",
            files: vec![
                (
                    "cfg.toml",
                    "[port]\ntitle = 'My Game'\nroute = 'apk'\n[game]\ndata = '*.apk *.xapk *.apks *.aab *.zip'\nbuild = 'android'\nunity_versions = ['2021.3.4f1']\npackage = 'com.test.game'\n\
                     [player]\nbackend = 'il2cpp'\nabis = ['arm64-v8a', 'armeabi-v7a']\n".to_string(),
                ),
                (
                    "v7.toml",
                    "[port]\ntitle = 'V7'\nroute = 'apk'\n[game]\nbuild = 'android'\nunity_versions = ['2021.3.4f1']\n[player]\nbackend = 'mono'\nabis = ['armeabi-v7a']\nil2cpp = true\n".to_string(),
                ),
                (
                    "v7only.toml",
                    "[port]\ntitle = 'V7only'\nroute = 'apk'\naarch64_only = true\n[game]\nbuild = 'android'\nunity_versions = ['2021.3.4f1']\n[player]\nbackend = 'il2cpp'\nabis = ['armeabi-v7a', 'arm64-v8a']\n".to_string(),
                ),
            ],
            cmds: vec![
                new("cfg.toml", "port", &[]),
                pkg("port", &["--submission"]),
                new("v7.toml", "port2", &[]),
                pkg("port2", &[]),
                new("v7only.toml", "port3", &[]),
                pkg("port3", &[]),
            ],
            env: vec![],
            after_new: vec![],
        },
        Case {
            name: "box64",
            files: vec![("cfg.toml", "[port]\ntitle = 'Box'\nroute = 'box64'\n[game]\nunity_versions = ['2022.3.4f1']\n[runtime]\nPMU_GL = 'mesa'\n".to_string())],
            cmds: vec![new("cfg.toml", "port", &[]), pkg("port", &[])],
            env: vec![("BOX64_OUT", s(&i.join("box64out")))],
            after_new: vec![],
        },
        Case {
            name: "mono-arm64-own-toml",
            files: vec![("port/port.toml", "[port]\ntitle = 'Mono'\nroute = 'mono-arm64'\nready_marker = '.setup_done64'\n[game]\nunity_versions = ['2019.4.8f1', '2019.4.40f1']\n".to_string())],
            cmds: vec![sv(&["port", "new", "mono", "--out", "port", "--force"]), pkg("port", &["--mono64-work", &s(&i.join("mono64"))])],
            env: vec![],
            after_new: vec![],
        },
        Case {
            name: "mono-arm64-net35-own",
            files: vec![(
                "cfg.toml",
                "[port]\ntitle = 'Old'\nroute = 'mono-arm64'\n[game]\nunity_versions = ['2017.4.40f1']\n[player]\nmono64_classlibs = ['System.dll', 'Mono.Posix.dll']\n\
                 [files]\nown = ['Old.sh', 'mygame/README.md']\n".to_string(),
            )],
            cmds: vec![new("cfg.toml", "port", &[]), new("cfg.toml", "port", &[]), pkg("port", &["--mono64-work", &s(&i.join("mono64"))])],
            env: vec![],
            after_new: vec![("port/Old.sh", b"#!/bin/bash\n# hand-made\n".to_vec())],
        },
        Case {
            name: "net35-true",
            files: vec![("cfg.toml", "[port]\ntitle = 'Old'\nroute = 'mono-arm64'\n[game]\nunity_versions = ['2017.4.40f1']\n[player]\nmono64_classlibs = true\n".to_string())],
            cmds: vec![new("cfg.toml", "port", &[]), pkg("port", &["--mono64-work", &s(&i.join("mono64"))])],
            env: vec![],
            after_new: vec![],
        },
        Case {
            name: "errors",
            files: vec![
                ("bad.toml", "[port]\nname = 'Bad Name'\ngenres = ['shooter']\nstore = ['https://x']\n[game]\nunity_versions = ['x']\nrelease_date = '29/09/2017'\n[runtime]\nua-x = '1'\n[[questions]]\nid = 'q'\nquestion = '?'\noptions = [['a b', 'A']]\ndefault = 'z'\n".to_string()),
                ("noversion.toml", "[port]\ntitle = 'X'\n".to_string()),
                ("a64box.toml", "[port]\ntitle = 'X'\nroute = 'nope'\naarch64_only = true\n[game]\nunity_versions = ['2019.4.8f1']\n".to_string()),
                ("noroute.toml", "[port]\ntitle = 'X'\nroute = 'nope'\n[game]\nunity_versions = ['2019.4.8f1']\n".to_string()),
                ("ok.toml", "[port]\ntitle = 'X'\n[game]\nunity_versions = ['2019.4.8f1']\n".to_string()),
                ("mc.toml", "[port]\ntitle = 'X'\n[game]\nunity_versions = ['2019.4.8f1']\n[player]\nmono64_classlibs = true\n[files]\nown = ['../x', 'port.toml']\n".to_string()),
                ("other/notes.txt", "x".to_string()),
                ("clone/LICENSE", "x".to_string()),
                ("clone/._LICENSE", "x".to_string()),
            ],
            cmds: vec![
                new("bad.toml", "p1", &[]),
                new("noversion.toml", "p2", &[]),
                new("a64box.toml", "p3", &[]),
                new("noroute.toml", "p4", &[]),
                new("mc.toml", "p5", &[]),
                new("ok.toml", "other", &[]),
                new("ok.toml", "clone", &[]),
                sv(&["port", "package", "clone", "--out", "rel", "--runtime", &rt, "--pmu", &pmu, "--no-build"]),
                sv(&["port", "package", "clone", "--out", "rel", "--runtime", &rt, "--donors", &s(&i), "--pmu", &pmu, "--no-build"]),
                sv(&["port", "package", "other", "--out", "rel"]),
            ],
            env: vec![],
            after_new: vec![],
        },
    ];
    // (images: a real 640x480 screenshot and a cover)
    for c in out.iter_mut() {
        for (p, data) in c.after_new.iter_mut() {
            if p.ends_with(".png") && data.is_empty() {
                let t = w.join("png.tmp");
                png(&t, if p.contains("cover") { 300 } else { 640 }, if p.contains("cover") { 400 } else { 480 });
                *data = fs::read(&t).unwrap();
                let _ = fs::remove_file(&t);
            }
        }
    }
    out
}

struct Run {
    statuses: Vec<i32>,
    stdout: Vec<String>,
    stderr: Vec<String>,
}

fn run_case(w: &Path, case: &Case) -> Run {
    let work = w.join("work");
    let _ = fs::remove_dir_all(&work);
    fs::create_dir_all(&work).unwrap();
    for (p, text) in &case.files {
        put(&work.join(p), text);
    }
    let mut r = Run { statuses: vec![], stdout: vec![], stderr: vec![] };
    for (k, cmd) in case.cmds.iter().enumerate() {
        if k == 1 {
            for (p, data) in &case.after_new {
                put(&work.join(p), data);
            }
        }
        let mut c = Command::new(env!("CARGO_BIN_EXE_pmu"));
        c.args(cmd).current_dir(&work);
        for k in ["DONORS", "PMU_DIR", "PYBUNDLE", "PMUNITY_MONO64_WORK", "BOX64_OUT", "NUGET_PACKAGES", "PMUNITY_PORT_HOOKS", "PMUNITY_REPO"] {
            c.env_remove(k);
        }
        c.env("PMUNITY_REPO", wt());
        for (k, v) in &case.env {
            c.env(k, v);
        }
        let o = c.output().unwrap();
        r.statuses.push(o.status.code().unwrap_or(-1));
        r.stdout.push(String::from_utf8_lossy(&o.stdout).into_owned());
        r.stderr.push(String::from_utf8_lossy(&o.stderr).into_owned());
    }
    r
}

/// Every file under d: {relative path: (mode, bytes)}.
fn tree(d: &Path) -> BTreeMap<String, (u32, Vec<u8>)> {
    let mut out = BTreeMap::new();
    let mut stack = vec![d.to_path_buf()];
    while let Some(x) = stack.pop() {
        for e in fs::read_dir(&x).unwrap() {
            let e = e.unwrap();
            let p = e.path();
            let ft = e.file_type().unwrap();
            if ft.is_dir() {
                stack.push(p);
            } else if ft.is_file() {
                let rel = p.strip_prefix(d).unwrap().to_string_lossy().into_owned();
                let mode = fs::metadata(&p).unwrap().permissions().mode() & 0o777;
                out.insert(rel, (mode, fs::read(&p).unwrap()));
            }
        }
    }
    out
}

/// A zip's entries as (name, method, time, date, version made by, system, version needed, flags,
/// external attr, crc, size, data); data of nested zips (donor APKs) as their own entries.
fn zip_entries(data: &[u8], tmp: &Path) -> Option<Vec<String>> {
    if !data.starts_with(b"PK\x03\x04") {
        return None;
    }
    put(tmp, data);
    let a = pkgzip::zip::Archive::open(&tmp.to_string_lossy()).ok()?;
    let mut out = Vec::new();
    for e in &a.entries {
        let mut d = Vec::new();
        a.reader(e).ok()?.read_to_end(&mut d).ok()?;
        let nested = zip_entries(&d, &tmp.with_extension("inner"));
        // (a nested zip's own CRC and size follow its deflate streams: its entries are compared)
        let (crc, n) = if nested.is_some() { (0, 0) } else { (e.crc, e.file_size) };
        out.push(format!(
            "{} m{} t{} d{} cv{} cs{} ev{} f{} x{:o} crc{:08x} n{} extra{:?} comment{:?}",
            e.name, e.method, e.time, e.date, e.create_version, e.create_system, e.extract_version, e.flag_bits, e.external_attr,
            crc, n, e.extra, e.comment
        ));
        match nested {
            Some(inner) => out.extend(inner.into_iter().map(|x| format!("  {}: {x}", e.name))),
            None => out.push(format!("  data {:016x}", fnv(&d))),
        }
    }
    Some(out)
}

fn fnv(d: &[u8]) -> u64 {
    d.iter().fold(0xcbf29ce484222325u64, |h, b| (h ^ *b as u64).wrapping_mul(0x100000001b3))
}


// ---- The packager's golden outputs (no Python)
//
// `pmu port new` / `pmu port package` on the synthetic cases above, against tests/golden/port.txt: each
// command's exit status, stdout and stderr's last line, and every file left (mode, size, a digest of
// its bytes; a zip's entries, nested zips included, digested too) and the three port.toml texts
// dump_toml writes. The file was written from a run whose every output was byte for byte the
// Python packager's (the port_parity test, 2026-11-21; it and the Python left with convert/ on 2026-11-27): it is
// that bar kept without the Python. The inputs are made up here: no game or Unity data.
// A deliberate change of the templates or the packager changes it: look at the difference
// (PMU_PARITY_DIR keeps the outputs and writes port.txt.new), then write it again with
//   PMU_PORT_BLESS=1 cargo test -p pmu --test port_parity port_golden

/// The golden text of a run of every case under `base`.
fn golden_text(base: &Path) -> String {
    let (b, repo) = (base.to_string_lossy().into_owned(), wt().to_string_lossy().into_owned());
    let norm = |s: &str| s.replace(&b, "<BASE>").replace(&repo, "<REPO>");
    let mut out = String::new();
    for case in cases(base) {
        let r = run_case(base, &case);
        let dst = base.join(case.name).join("rs");
        let _ = fs::remove_dir_all(&dst);
        fs::create_dir_all(dst.parent().unwrap()).unwrap();
        fs::rename(base.join("work"), &dst).unwrap();
        out += &format!("== {}\n", case.name);
        for k in 0..case.cmds.len() {
            out += &format!("command {k}: {} -> exit {}\n", norm(&case.cmds[k].join(" ")), r.statuses[k]);
            for l in norm(&r.stdout[k]).lines() {
                out += &format!("  out: {l}\n");
            }
            if let Some(l) = norm(&r.stderr[k]).trim_end().lines().last() {
                out += &format!("  err: {l}\n");
            }
        }
        for (n, (mode, data)) in tree(&dst) {
            out += &format!("file {n} {mode:o} {} {:016x}", data.len(), fnv(&data));
            if let Some(e) = zip_entries(&data, &base.join("golden-cmp.zip")) {
                out += &format!(" zip {} {:016x}", e.len(), fnv(e.join("\n").as_bytes()));
            }
            out += "\n";
        }
    }
    let cfg_text = ANDROID.replace("[[questions]]\nid = \"PICTURE\"", "[[questions]]\nid = \"PIC\"");
    put(&base.join("dump/cfg.toml"), &cfg_text);
    for route in ["apk", "mono-arm64"] {
        let repo = port::Repo::new(wt());
        let user = port::config::load_config(&base.join("dump/cfg.toml").to_string_lossy()).unwrap();
        let mut cfg = port::config::build_config("mygame", &user);
        let r = port::routes::route(&repo, route).unwrap().unwrap();
        let donors = port::routes::port_donors(&cfg, &r);
        cfg.table("player").set("donors", port::val::Val::List(donors));
        let mine = port::config::dump_toml(&cfg, &r, &["controls"]).unwrap();
        out += &format!("== dump_toml {route}\n");
        for l in mine.lines() {
            out += &format!("  {l}\n");
        }
    }
    out
}

#[test]
fn port_golden() {
    // (a fixed place: the outputs name it; one run at a time: the lock)
    let base = PathBuf::from("/tmp/pmu-port-golden");
    let _ = fs::create_dir_all(&base);
    if base.canonicalize().ok().as_deref() != Some(base.as_path()) {
        eprintln!("port_golden: skipped (/tmp/pmu-port-golden is not a plain folder here)");
        return;
    }
    let lock = fs::File::open(&base).unwrap();
    rustix_lock(&lock);
    for e in fs::read_dir(&base).unwrap().flatten() {
        let p = e.path();
        let _ = if p.is_dir() { fs::remove_dir_all(&p) } else { fs::remove_file(&p) };
    }
    inputs(&base);
    let got = golden_text(&base);
    let file = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/golden/port.txt");
    if std::env::var_os("PMU_PORT_BLESS").is_some_and(|v| !v.is_empty()) {
        put(&file, &got);
        eprintln!("port_golden: wrote {}", file.display());
        return;
    }
    let want = fs::read_to_string(&file).unwrap_or_default();
    if got == want {
        if std::env::var_os("PMU_PARITY_DIR").is_none() {
            let _ = fs::remove_dir_all(&base);
        }
        return;
    }
    put(&base.join("port.txt.new"), &got);
    let (g, w): (Vec<&str>, Vec<&str>) = (got.lines().collect(), want.lines().collect());
    let (mut diffs, mut section) = (Vec::new(), "");
    let wset: std::collections::BTreeSet<&str> = w.iter().copied().collect();
    for l in &g {
        if l.starts_with("== ") {
            section = l;
        } else if !wset.contains(l) && diffs.len() < 40 {
            diffs.push(format!("{section}: {l}"));
        }
    }
    panic!(
        "the packager's outputs are not tests/golden/port.txt's ({} lines against {}); the new text is {}/port.txt.new, the outputs beside it; lines not in the golden file:\n{}",
        g.len(),
        w.len(),
        base.display(),
        diffs.join("\n")
    );
}

/// An exclusive lock on the golden run's folder, held while the test's handle lives.
fn rustix_lock(f: &fs::File) {
    use std::os::fd::AsRawFd;
    // SAFETY: flock on a descriptor this test owns
    unsafe {
        libc::flock(f.as_raw_fd(), libc::LOCK_EX);
    }
}
