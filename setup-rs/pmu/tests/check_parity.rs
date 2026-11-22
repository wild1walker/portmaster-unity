//! `pmu port check`, `pmu port new --data`, `pmu port routes` and `pmu route` against the Python
//! they were ported from (convert/pmunity: port.py's check with route.py's, plugins.py's and
//! streaming.py's parts of it), on made-up games: executables' headers, assemblies' metadata,
//! plugins, soundbanks and (with the class database, $PMU_TPK) serialized files and bundles
//! written here. No game or Unity data.
//!
//! check_parity: with PMU_PARITY_PYTHON (a Python 3.11+ with convert/requirements.txt) each case
//! runs the Python and pmu at the same paths and compares the exit status, stdout, stderr and
//! every file left. check_golden: the same cases against tests/golden/check.txt, written from a
//! run whose every output was the Python's byte for byte; PMU_CHECK_BLESS=1 writes it again.
//! Without PMU_TPK the cases that read serialized objects are left out (both tests say so).
#[path = "../../crates/convert/tests/madeup/mod.rs"]
mod madeup;
mod made;

use made::{dll, elf, pe, put, sf_empty, Asm};
use madeup::Obj;
use std::collections::BTreeMap;
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use unityfs::Value;

fn wt() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap()
}

const SURVEY: &str = "\
input survey
  InControl's Android profiles:
    -> UA_PAD_NAME=\"Xbox 360 Wired Controller\"
  pad bindings (the game's defaults):
    the left stick alone (no d-pad binding): GameActions.MoveUp, GameActions.MoveDown
      -> UA_DPAD_STICK=1,2
    the d-pad: its own bindings GameActions.SelectUp,
      GameActions.SelectDown need a UA_DPAD_SWAP
      -> UA_DPAD_SWAP=select (the game binds nothing
         to it)
    the right stick alone (no other binding): GameActions.AimUp, GameActions.AimDown
    every binding, by control:
      LeftStickUp: GameActions.MoveUp
      LeftStickDown: GameActions.MoveDown
      LeftStickLeft: GameActions.MoveLeft
      LeftStickRight: GameActions.MoveRight
      DPadUp: GameActions.SelectUp
      DPadDown: GameActions.SelectDown
      RightStickUp: GameActions.AimUp
      RightStickDown: GameActions.AimDown
      Action1: GameActions.JumpAction, GameActions.UISubmit
      Action2: GameActions.DodgeRollAction
      Start: GameActions.PauseAction
      Back: GameActions.MapAction
      Select: GameActions.UseHPPotion2
  Mouse.current: used with no null check in 3 methods (Foo.Update, Bar.LateUpdate)
verdict: mixed: the pad and the mouse
";

/// The game's own code of the Linux game: every stack the check looks for.
fn assembly_csharp() -> Vec<u8> {
    dll(&Asm {
        assembly: Some(("Assembly-CSharp", 0)),
        typerefs: &[
            ("UnityEngine", "Input"),                               // 1
            ("UnityEngine", "Application"),                         // 2
            ("System.IO", "File"),                                  // 3
            ("UnityEngine.UI", "InputField"),                       // 4
            ("UnityEngine.EventSystems", "IPointerClickHandler"),   // 5
            ("UnityEngine.EventSystems", "ISubmitHandler"),         // 6
            ("UnityEngine.Video", "VideoPlayer"),                   // 7
            ("Steamworks", "SteamAPI"),                             // 8
            ("UnityEngine", "Cursor"),                              // 9
            ("UnityEngine", "Screen"),                              // 10
            ("UnityEngine.EventSystems", "EventSystem"),            // 11
            ("InControl", "InputDevice"),                           // 12
            ("", "AkSoundEngine"),                                  // 13
        ],
        typedefs: &[("", "<Module>"), ("Game", "Player"), ("HutongGames.PlayMaker", "Fsm"), ("InControl", "InputManager")],
        methods: &["Update", "OnMouseDown", "OnMouseEnter", ".ctor"],
        memberrefs: &[
            (1, "GetAxis"), (1, "GetButtonDown"), (1, "GetKey"), (1, "GetKeyDown"), (1, "GetMouseButton"), (1, "get_inputString"),
            (1, "get_touchCount"), (2, "get_streamingAssetsPath"), (2, "get_dataPath"), (3, "ReadAllText"), (9, "set_visible"),
            (9, "set_lockState"), (10, "SetResolution"), (11, "SetSelectedGameObject"),
        ],
        modulerefs: &["AkSoundEngine", "steam_api", "libc", "mylib"],
        pinvokes: &[(1, "CSharp_Init"), (2, "SteamAPI_Init"), (3, "getpid"), (4, "my_call")],
    })
}

fn bank(version: u32) -> Vec<u8> {
    let mut b = b"BKHD".to_vec();
    b.extend(24u32.to_le_bytes());
    b.extend(version.to_le_bytes());
    b.extend([0u8; 20]);
    b
}

/// The made-up games (under <base>/in), and the stand-in for asmpatch.
fn inputs(base: &Path, db: Option<&unityfs::ClassDb>) {
    let i = base.join("in");
    let interp = Some("/lib64/ld-linux-x86-64.so.2");
    // ---- linux: a Mono game for Linux (Unity 2019.4), with everything the check reports
    let g = i.join("linux/My Game");
    let d = g.join("My Game_Data");
    put(&g.join("My Game.x86_64"), elf(62, 2, interp, &["libdl.so.2", "libc.so.6", "UnityPlayer.so", "libgcc_s.so.1"], &["GLIBC_2.2.5", "GLIBC_2.17", "GLIBCXX_3.4.9"], b""));
    put(&g.join("UnityPlayer.so"), elf(62, 3, None, &["libm.so.6", "libstdc++.so.6", "libfoo.so.1"], &["GLIBC_2.27"],
        b"\0force-glcore\0force-glcore45\0force-vulkan\0force-opengl\0force-gfx-direct\0libGL.so.1\0libX11.so.6\0libasound.so.2\0libnotused.so\0eglCreateWindowSurface\0GLX_EXT_create_context_es2_profile\0"));
    put(&g.join("My Game_Data.txt"), "notes");
    put(&d.join("globalgamemanagers"), sf_empty("2019.4.8f1", 21, 24));
    put(&d.join("Managed/Assembly-CSharp.dll"), assembly_csharp());
    put(&d.join("Managed/UnityEngine.dll"), dll(&Asm { assembly: Some(("UnityEngine", 0)), typedefs: &[("UnityEngine", "Object")], ..Default::default() }));
    put(&d.join("Managed/Unity.InputSystem.dll"), dll(&Asm { typedefs: &[("UnityEngine.InputSystem", "Mouse")], ..Default::default() }));
    put(&d.join("Managed/Broken.dll"), b"MZ not an assembly");
    put(&d.join("Managed/notes.txt"), "x");
    put(&d.join("Plugins/x86_64/libsteam_api.so"), elf(62, 3, None, &[], &[], b""));
    put(&d.join("Plugins/x86_64/libAkSoundEngine.so"), elf(62, 3, None, &[], &[], b"\0Wwise SDK v2017.2.9 Build 6726\0"));
    put(&d.join("Plugins/x86_64/ScreenSelector.so"), elf(62, 3, None, &[], &[], b""));
    put(&d.join("Plugins/x86_64/libmylib.so"), elf(62, 3, None, &["libz.so.1"], &[], b""));
    put(&d.join("Plugins/x86_64/readme.txt"), "x");
    put(&d.join("StreamingAssets/Audio/GeneratedSoundBanks/Linux/Init.bnk"), bank(128));
    put(&d.join("StreamingAssets/Audio/GeneratedSoundBanks/Linux/Music.bnk"), bank(128));
    put(&d.join("StreamingAssets/Audio/GeneratedSoundBanks/Linux/SoundbanksInfo.xml"), "<SoundBanksInfo SchemaVersion=\"10\" SoundbankVersion=\"128\">");
    put(&d.join("StreamingAssets/web.unity3d"), b"UnityRaw\0\x03\x003.x.x\0");
    put(&d.join("StreamingAssets/hidden.dat"), [&[9u8; 40][..], b"UnityFS\0\0\0\0\x06"].concat());
    put(&d.join("StreamingAssets/table.json"), vec![b'{'; 3000]);
    // ---- net35: Unity 2017.4 with the 2.0 class libraries (Mono/ beside the data), a pad-only game
    let g = i.join("net35");
    let d = g.join("Old_Data");
    put(&g.join("Old.x86"), elf(3, 2, interp, &["libc.so.6"], &[], b"\0force-opengl\0"));
    put(&d.join("globalgamemanagers"), sf_empty("2017.4.30f1", 17, 25));
    put(&d.join("Managed/mscorlib.dll"), dll(&Asm { assembly: Some(("mscorlib", 2)), typedefs: &[("System", "Object")], ..Default::default() }));
    put(&d.join("Managed/Assembly-CSharp.dll"), dll(&Asm {
        assembly: Some(("Assembly-CSharp", 0)),
        typerefs: &[("UnityEngine", "Input"), ("Rewired", "Player")],
        typedefs: &[("", "<Module>"), ("Game", "Main")],
        methods: &["Update"],
        memberrefs: &[(1, "GetAxis")],
        ..Default::default()
    }));
    put(&d.join("Managed/Rewired_Core.dll"), dll(&Asm { typedefs: &[("Rewired", "InputManager_Base")], ..Default::default() }));
    put(&d.join("Mono/etc/mono/config"), "<configuration/>");
    // ---- windows: an IL2CPP build for Windows (no route)
    let g = i.join("windows");
    put(&g.join("Win.exe"), pe(0x8664));
    put(&g.join("GameAssembly.dll"), pe(0x8664));
    put(&g.join("UnityPlayer.dll"), pe(0x8664));
    put(&g.join("Win_Data/globalgamemanagers"), sf_empty("2021.3.5f1", 21, 19));
    put(&g.join("Win_Data/il2cpp_data/Metadata/global-metadata.dat"), "x");
    put(&g.join("Win_Data/Plugins/x86_64/discord_game_sdk.dll"), pe(0x8664));
    // ---- winmono: a Mono build for Windows of an old Unity (outside the converter's range), in a folder with the Linux one
    let g = i.join("two/win");
    put(&g.join("Thing.exe"), pe(0x14c));
    put(&g.join("Thing_Data/globalgamemanagers"), sf_empty("5.4.1f1", 17, 5));
    put(&g.join("Thing_Data/Managed/Assembly-CSharp.dll"), dll(&Asm { typedefs: &[("", "<Module>")], ..Default::default() }));
    let g = i.join("two/linux");
    put(&g.join("Thing.x86_64"), elf(62, 2, interp, &["libc.so.6"], &[], b"\0force-opengl\0"));
    put(&g.join("Thing_Data/globalgamemanagers"), sf_empty("5.4.1f1", 17, 24));
    put(&g.join("Thing_Data/Managed/Assembly-CSharp.dll"), dll(&Asm {
        typerefs: &[("UnityEngine", "Input"), ("UnityEngine", "Handheld"), ("UnityEngine", "MovieTexture")],
        typedefs: &[("", "<Module>")],
        memberrefs: &[(1, "GetMouseButtonDown"), (1, "get_mousePosition"), (2, "PlayFullScreenMovie")],
        methods: &["OnMouseDown"],
        ..Default::default()
    }));
    // ---- mac: a macOS app (Mono, universal executable, a plugin bundle)
    let app = i.join("mac/Mac Game.app/Contents");
    put(&app.join("MacOS/Mac Game"), [&b"\xca\xfe\xba\xbe\0\0\0\x02"[..], &[1, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0], &[1, 0, 0, 12], &[0u8; 64]].concat());
    put(&app.join("Frameworks/MonoBleedingEdge/x"), "x");
    put(&app.join("Resources/Data/globalgamemanagers"), sf_empty("2020.3.1f1", 21, 2));
    put(&app.join("Resources/Data/Managed/Assembly-CSharp.dll"), dll(&Asm { typedefs: &[("", "<Module>"), ("Game", "Main")], ..Default::default() }));
    put(&app.join("PlugIns/fmodstudio.bundle/Contents/MacOS/fmodstudio"), b"\xcf\xfa\xed\xfe\x07\0\0\x01 and more bytes here");
    // ---- none: no game
    put(&i.join("none/readme.txt"), "nothing here");
    // ---- the stand-in for asmpatch: a survey and the patches that would apply
    put(&i.join("survey.txt"), SURVEY);
    let sh = i.join("asmpatch.sh");
    put(&sh, "#!/bin/sh\ncase \"$1\" in\n--survey) cat \"$(dirname \"$0\")/survey.txt\";;\n--list) echo 'would streaming-assets: Assembly-CSharp.dll'; echo 'would data-path: Assembly-CSharp.dll'; echo 'is rewired-android: none';;\nesac\n");
    fs::set_permissions(&sh, fs::Permissions::from_mode(0o755)).unwrap();
    put(&i.join("fail.sh"), "#!/bin/sh\nexit 3\n");
    fs::set_permissions(i.join("fail.sh"), fs::Permissions::from_mode(0o755)).unwrap();
    let Some(db) = db else { return };
    // ---- data: serialized files with objects (Unity 2017.4, the class database's trees), and
    // ---- packed: the same settings in a compressed build's data.unity3d
    let o = |class_id, path_id, value| Obj { class_id, path_id, value };
    let set = madeup::set;
    let mut ps = madeup::object(db, 129);
    set(&mut ps, &["productName"], Value::str("the made up game"));
    set(&mut ps, &["companyName"], Value::str(" Made Up Games "));
    set(&mut ps, &["defaultScreenWidth"], Value::I32(1280));
    set(&mut ps, &["defaultScreenHeight"], Value::I32(720));
    set(&mut ps, &["m_ActiveColorSpace"], Value::I32(1));
    let mut bs = madeup::build_settings(db);
    set(&mut bs, &["m_GraphicsAPIs"], Value::Array(vec![Value::I32(2), Value::I32(21)]));
    let ggm = madeup::serialized(db, &[o(141, 1, bs), o(129, 2, ps)], &[], false);
    let script = |ns: &str, class: &str| {
        let mut v = madeup::object(db, 115);
        set(&mut v, &["m_Namespace"], Value::str(ns));
        set(&mut v, &["m_ClassName"], Value::str(class));
        set(&mut v, &["m_AssemblyName"], Value::str("Assembly-CSharp.dll"));
        v
    };
    let behaviour = |file: i32, pid: i64| {
        let mut v = madeup::object(db, 114);
        set(&mut v, &["m_Script"], madeup::pptr(file, pid));
        v
    };
    let tex = |name: &str, (w, h): (i32, i32), format: i32, mips: i32, size: i32| {
        let mut v = madeup::texture(db, name, (4, 4), format, &[0u8; 16], None);
        set(&mut v, &["m_Width"], Value::I32(w));
        set(&mut v, &["m_Height"], Value::I32(h));
        set(&mut v, &["m_MipCount"], Value::I32(mips));
        set(&mut v, &["m_CompleteImageSize"], Value::I32(size));
        v
    };
    let scripts = madeup::serialized(db, &[
        o(115, 1, script("UnityEngine.EventSystems", "StandaloneInputModule")),
        o(115, 2, script("UnityEngine.UI", "InputField")),
        o(115, 3, script("", "Player")),
    ], &[], false);
    let level = madeup::serialized(db, &[o(114, 1, behaviour(1, 1)), o(114, 2, behaviour(1, 3)), o(114, 3, behaviour(1, 3)), o(114, 4, behaviour(1, 2)),
                                         o(328, 5, madeup::object(db, 328))], &["globalgamemanagers.assets"], false);
    let shared = madeup::serialized(db, &[
        o(28, 1, tex("a", (4096, 4096), 12, 13, 50 << 20)),
        o(28, 2, tex("b", (8192, 4096), 10, 1, 40 << 20)),
        o(28, 3, tex("c", (8192, 8192), 4, 1, 256 << 20)),
        o(28, 4, tex("d", (8192, 8192), 5, 1, 200 << 20)),
        o(28, 5, tex("e", (64, 64), 29, 1, 1000)),
    ], &[], false);
    let shader = |name: &str, platforms: &[u32]| {
        let mut v = madeup::object(db, 48);
        set(&mut v, &["m_ParsedForm", "m_Name"], Value::str(name));
        set(&mut v, &["platforms"], Value::Array(platforms.iter().map(|p| Value::U32(*p)).collect()));
        v
    };
    let in_bundle = madeup::serialized(db, &[o(48, 1, shader("One", &[15, 4])), o(48, 2, shader("Two", &[4])), o(48, 3, shader("Three", &[4])),
                                             o(28, 4, tex("f", (2048, 2048), 12, 1, 4 << 20))], &[], true);
    let two = madeup::serialized(db, &[o(28, 1, tex("g", (16, 16), 4, 1, 1024))], &[], true);
    let g = i.join("data");
    let d = g.join("Data Game_Data");
    put(&g.join("Data Game.exe"), pe(0x8664));
    put(&d.join("globalgamemanagers"), &ggm);
    put(&d.join("globalgamemanagers.assets"), &scripts);
    put(&d.join("level0"), &level);
    put(&d.join("sharedassets0.assets"), &shared);
    put(&d.join("Managed/Assembly-CSharp.dll"), dll(&Asm { typedefs: &[("", "<Module>"), ("", "Player")], ..Default::default() }));
    put(&d.join("StreamingAssets/bundles/one.bundle"), madeup::bundle(&[("CAB-one", 4, in_bundle.clone())], 1 << 17));
    put(&d.join("StreamingAssets/bundles/scene.bundle"), madeup::bundle(&[("BuildPlayer-s", 4, two.clone()), ("BuildPlayer-s.sharedAssets", 4, in_bundle)], 1 << 17));
    put(&d.join("StreamingAssets/bundles/res.bundle"), madeup::bundle(&[("CAB-res.resS", 0, vec![7u8; 100])], 1 << 17));
    let g = i.join("packed");
    put(&g.join("Packed.x86_64"), elf(62, 2, interp, &["libc.so.6"], &[], b"\0force-glcore\0"));
    put(&g.join("Packed_Data/data.unity3d"), madeup::bundle(&[("globalgamemanagers", 4, ggm), ("level0", 4, two)], 1 << 17));
    put(&g.join("Packed_Data/Managed/Assembly-CSharp.dll"), dll(&Asm { typedefs: &[("", "<Module>")], ..Default::default() }));
}

struct Case {
    name: &'static str,
    cmds: Vec<Vec<String>>,
}

fn cases(base: &Path, with_db: bool) -> Vec<Case> {
    let i = base.join("in");
    let p = |x: &str| i.join(x).to_string_lossy().into_owned();
    let c = |name: &'static str, cmds: &[&[&str]]| Case { name, cmds: cmds.iter().map(|c| c.iter().map(|x| x.to_string()).collect()).collect() };
    let (linux, fake) = (p("linux"), p("asmpatch.sh"));
    let mut v = vec![
        c("check-linux", &[
            &["port", "check", &linux, "--asmpatch", "none"],
            &["port", "check", &linux, "--asmpatch", "none", "--json"],
            &["port", "check", &p("linux/My Game/My Game_Data"), "--asmpatch", "none", "--no-scan"],
        ]),
        c("check-survey", &[
            &["port", "check", &linux, "--asmpatch", &fake],
            &["port", "check", &linux, "--asmpatch", &fake, "--json"],
            &["port", "check", &linux, "--asmpatch", &p("fail.sh")],
        ]),
        c("check-net35", &[&["port", "check", &p("net35"), "--asmpatch", "none"], &["port", "check", &p("net35"), "--asmpatch", "none", "--json"]]),
        c("check-windows", &[&["port", "check", &p("windows"), "--asmpatch", "none"], &["port", "check", &p("windows"), "--json"]]),
        c("check-two", &[&["port", "check", &p("two"), "--asmpatch", "none"], &["port", "check", &p("two/win"), "--asmpatch", "none"]]),
        c("check-mac", &[
            &["port", "check", &p("mac/Mac Game.app"), "--asmpatch", "none"],
            &["port", "check", &p("mac"), "--asmpatch", "none", "--json"],
        ]),
        c("check-none", &[&["port", "check", &p("none")], &["port", "check", &p("missing")], &["port", "check"]]),
        c("routes", &[&["port", "routes"]]),
        c("route", &[
            &["route", &linux],
            &["route", &linux, "--json"],
            &["route", &linux, "--env"],
            &["route", &p("linux/My Game/My Game.x86_64"), "--env"],
            &["route", &p("net35")],
            &["route", &p("net35"), "--env"],
            &["route", &p("windows")],
            &["route", &p("windows"), "--json"],
            &["route", &p("windows"), "--env"],
            &["route", &p("two")],
            &["route", &p("two"), "--json"],
            &["route", &p("two"), "--env"],
            &["route", &p("mac")],
            &["route", &p("mac"), "--json"],
            &["route", &p("none")],
            &["route", &linux, "--launcher", "launcher.sh", "--title", "My Game!"],
            &["route", &p("two"), "--launcher", "two.sh", "--port", "thing"],
            &["route", &p("windows"), "--launcher", "no.sh"],
            &["route"],
        ]),
        c("new-linux", &[
            &["port", "new", "mygame", "--data", &linux, "--asmpatch", "none", "--out", "port"],
            &["port", "new", "mygame", "--data", &linux, "--asmpatch", &fake, "--out", "port"],
            &["port", "new", "fresh", "--data", &linux, "--asmpatch", &fake],
        ]),
        c("new-net35", &[
            &["port", "new", "old", "--data", &p("net35"), "--asmpatch", "none", "--route", "mono-arm64", "--title", "The Old Game"],
            &["port", "new", "old2", "--data", &p("net35"), "--asmpatch", "none", "--route", "box64"],
        ]),
        c("new-refused", &[
            &["port", "new", "win", "--data", &p("windows"), "--asmpatch", "none"],
            &["port", "new", "nothing", "--data", &p("none")],
            &["port", "new", "nothing"],
        ]),
    ];
    if with_db {
        let (data, packed) = (p("data"), p("packed"));
        v.extend([
            c("check-data", &[&["port", "check", &data, "--asmpatch", "none"], &["port", "check", &data, "--asmpatch", "none", "--json"],
                              &["port", "check", &data, "--asmpatch", "none", "--no-scan"]]),
            c("check-packed", &[&["port", "check", &packed, "--asmpatch", "none"], &["port", "check", &packed, "--asmpatch", "none", "--json"]]),
            c("new-data", &[&["port", "new", "datagame", "--data", &data, "--asmpatch", "none"], &["route", &data], &["route", &packed, "--json"],
                            &["route", &packed, "--env"]]),
        ]);
    }
    v
}

struct Run {
    statuses: Vec<i32>,
    stdout: Vec<String>,
    stderr: Vec<String>,
}

fn run_case(base: &Path, case: &Case, python: Option<&str>) -> Run {
    let work = base.join("work");
    let _ = fs::remove_dir_all(&work);
    fs::create_dir_all(&work).unwrap();
    let mut r = Run { statuses: vec![], stdout: vec![], stderr: vec![] };
    for cmd in &case.cmds {
        let mut c = match python {
            Some(py) => {
                let mut c = Command::new(py);
                c.arg("-m").arg("pmunity").env("PYTHONPATH", wt().join("convert")).env("PYTHONDONTWRITEBYTECODE", "1").env_remove("PMUNITY_REPO");
                c
            }
            None => {
                let mut c = Command::new(env!("CARGO_BIN_EXE_pmu"));
                c.env("PMUNITY_REPO", wt());
                c
            }
        };
        c.args(cmd).current_dir(&work);
        for k in ["PMUNITY_ASMPATCH", "PMUNITY_PORT_HOOKS", "DOTNET", "DONORS"] {
            c.env_remove(k);
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
    fn walk(root: &Path, d: &Path, out: &mut BTreeMap<String, (u32, Vec<u8>)>) {
        for e in fs::read_dir(d).unwrap().flatten() {
            let p = e.path();
            if p.is_dir() {
                walk(root, &p, out);
            } else {
                let mode = fs::metadata(&p).unwrap().permissions().mode() & 0o777;
                out.insert(p.strip_prefix(root).unwrap().to_string_lossy().into_owned(), (mode, fs::read(&p).unwrap()));
            }
        }
    }
    let mut out = BTreeMap::new();
    walk(d, d, &mut out);
    out
}

fn fnv(d: &[u8]) -> u64 {
    d.iter().fold(0xcbf29ce484222325u64, |h, b| (h ^ *b as u64).wrapping_mul(0x100000001b3))
}

/// An error's last line (a traceback's text is Python's own; argparse's usage lines are compared
/// whole).
fn err_text(s: &str) -> String {
    if s.contains("Traceback (most recent call last)") {
        s.trim_end().lines().last().unwrap_or("").to_string()
    } else {
        s.to_string()
    }
}

#[test]
fn check_parity() {
    let Some(py) = std::env::var("PMU_PARITY_PYTHON").ok().filter(|p| !p.is_empty()) else {
        eprintln!("check_parity: skipped (PMU_PARITY_PYTHON: a Python 3.11+ with convert/requirements.txt)");
        return;
    };
    let db = madeup::db();
    if db.is_none() {
        eprintln!("check_parity: without PMU_TPK the cases with serialized objects are left out");
    }
    let keep = std::env::var("PMU_PARITY_DIR").ok().filter(|p| !p.is_empty());
    let base = keep.clone().map(PathBuf::from).unwrap_or_else(|| std::env::temp_dir().join(format!("pmu-check-parity-{}", std::process::id())));
    let _ = fs::remove_dir_all(&base);
    fs::create_dir_all(&base).unwrap();
    let base = base.canonicalize().unwrap();
    inputs(&base, db.as_deref());
    let mut failures: Vec<String> = Vec::new();
    for case in cases(&base, db.is_some()) {
        let mut outs = Vec::new();
        for (impl_, python) in [("py", Some(py.as_str())), ("rs", None)] {
            let r = run_case(&base, &case, python);
            let dst = base.join(case.name).join(impl_);
            let _ = fs::remove_dir_all(&dst);
            fs::create_dir_all(dst.parent().unwrap()).unwrap();
            fs::rename(base.join("work"), &dst).unwrap();
            outs.push((r, tree(&dst)));
        }
        let ((rp, tp), (rr, tr)) = (&outs[0], &outs[1]);
        let mut fail = |m: String| failures.push(format!("{}: {m}", case.name));
        for k in 0..case.cmds.len() {
            if rp.statuses[k] != rr.statuses[k] {
                fail(format!("command {k} exit {} (Python) != {} (pmu): py stderr {:?} / pmu stderr {:?}", rp.statuses[k], rr.statuses[k], rp.stderr[k], rr.stderr[k]));
            }
            if rp.stdout[k] != rr.stdout[k] {
                let (a, b): (Vec<&str>, Vec<&str>) = (rp.stdout[k].lines().collect(), rr.stdout[k].lines().collect());
                let at = a.iter().zip(&b).position(|(x, y)| x != y).unwrap_or(a.len().min(b.len()));
                fail(format!("command {k} stdout differs at line {} ({} lines against {}):\n--- py\n{}\n--- pmu\n{}", at + 1, a.len(), b.len(),
                             a.get(at).unwrap_or(&"(the end)"), b.get(at).unwrap_or(&"(the end)")));
            }
            if err_text(&rp.stderr[k]) != err_text(&rr.stderr[k]) {
                fail(format!("command {k} stderr differs:\n--- py\n{}\n--- pmu\n{}", rp.stderr[k], rr.stderr[k]));
            }
        }
        let names: std::collections::BTreeSet<&String> = tp.keys().chain(tr.keys()).collect();
        for n in names {
            match (tp.get(n), tr.get(n)) {
                (Some(a), Some(b)) if a == b => {}
                (Some(a), Some(b)) => fail(format!("file {n} differs (mode {:o} / {:o}, {} / {} bytes)", a.0, b.0, a.1.len(), b.1.len())),
                (Some(_), None) => fail(format!("file {n}: the Python's only")),
                _ => fail(format!("file {n}: pmu's only")),
            }
        }
    }
    if keep.is_none() {
        let _ = fs::remove_dir_all(&base);
    }
    assert!(failures.is_empty(), "{} difference(s) from the Python:\n{}", failures.len(), failures.join("\n"));
}

fn golden_text(base: &Path, with_db: bool) -> String {
    let (b, repo) = (base.to_string_lossy().into_owned(), wt().to_string_lossy().into_owned());
    let norm = |s: &str| s.replace(&b, "<BASE>").replace(&repo, "<REPO>");
    let mut out = String::new();
    for case in cases(base, with_db) {
        let r = run_case(base, &case, None);
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
            for l in norm(&r.stderr[k]).lines() {
                out += &format!("  err: {l}\n");
            }
        }
        for (n, (mode, data)) in tree(&dst) {
            // (a port's files name the folder they were made in only in port.build.json's notes)
            let text = String::from_utf8_lossy(&data);
            let data = if text.contains(&b) { norm(&text).into_bytes() } else { data };
            out += &format!("file {n} {mode:o} {} {:016x}\n", data.len(), fnv(&data));
        }
    }
    out
}

#[test]
fn check_golden() {
    // (a fixed place: the outputs name it; one run at a time: the lock)
    let base = PathBuf::from("/tmp/pmu-check-golden");
    let _ = fs::create_dir_all(&base);
    if base.canonicalize().ok().as_deref() != Some(base.as_path()) {
        eprintln!("check_golden: skipped (/tmp/pmu-check-golden is not a plain folder here)");
        return;
    }
    let lock = fs::File::open(&base).unwrap();
    {
        use std::os::fd::AsRawFd;
        // SAFETY: flock on a descriptor this test owns
        unsafe {
            libc::flock(lock.as_raw_fd(), libc::LOCK_EX);
        }
    }
    for e in fs::read_dir(&base).unwrap().flatten() {
        let p = e.path();
        let _ = if p.is_dir() { fs::remove_dir_all(&p) } else { fs::remove_file(&p) };
    }
    let db = madeup::db();
    inputs(&base, db.as_deref());
    let got = golden_text(&base, db.is_some());
    let file = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/golden/check.txt");
    if std::env::var_os("PMU_CHECK_BLESS").is_some_and(|v| !v.is_empty()) {
        assert!(db.is_some(), "PMU_CHECK_BLESS needs PMU_TPK (the golden file holds every case)");
        put(&file, &got);
        eprintln!("check_golden: wrote {}", file.display());
        return;
    }
    let want = fs::read_to_string(&file).unwrap_or_default();
    // (without the class database: the cases it has, which come first)
    let want = if db.is_some() {
        want
    } else {
        eprintln!("check_golden: without PMU_TPK the cases with serialized objects are left out");
        want.split("== check-data\n").next().unwrap_or("").to_string()
    };
    if got == want {
        if std::env::var_os("PMU_PARITY_DIR").is_none() {
            let _ = fs::remove_dir_all(&base);
        }
        return;
    }
    put(&base.join("check.txt.new"), &got);
    let (g, w): (Vec<&str>, Vec<&str>) = (got.lines().collect(), want.lines().collect());
    let wset: std::collections::BTreeSet<&str> = w.iter().copied().collect();
    let (mut diffs, mut section) = (Vec::new(), "");
    for l in &g {
        if l.starts_with("== ") {
            section = l;
        } else if !wset.contains(l) && diffs.len() < 40 {
            diffs.push(format!("{section}: {l}"));
        }
    }
    panic!(
        "the check's outputs are not tests/golden/check.txt's ({} lines against {}); the new text is {}/check.txt.new, the outputs beside it; lines not in the golden file:\n{}",
        g.len(),
        w.len(),
        base.display(),
        diffs.join("\n")
    );
}
