//! The setup templates (setup-rs/templates/<route>/patchscript and launcher.sh) run as a handheld
//! runs them (convert/tests/test_port.py's NewAndSetupTest and ApkRouteTest, written here): a port
//! made by `pmu port new` from a made-up game, installed as ports/mygame, its patchscript run by
//! bash with stand-ins for the setup's tools (setup/pmu: a script that logs each command and makes
//! what the setup reads next; PortMaster's 7-Zip; `sync`, `df`), and the launcher on a stand-in
//! PortMaster. The apk route's setup runs the real pmu on made-up APKs. No game or Unity data.
#[path = "../../crates/apk/tests/made/mod.rs"]
mod apkmade;
mod made;

use made::{dll, put, sf_empty, Asm};
use std::fs;
use std::io::Write;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Command;

fn wt() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap()
}

fn templates() -> PathBuf {
    wt().join("setup-rs/templates")
}

/// The routes with a setup (templates/<route>/patchscript).
fn routes() -> Vec<String> {
    let mut r: Vec<String> = fs::read_dir(templates())
        .unwrap()
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| templates().join(n).join("patchscript").is_file())
        .collect();
    r.sort();
    assert!(r.len() >= 5, "{r:?}");
    r
}

fn read(p: &Path) -> String {
    fs::read_to_string(p).unwrap_or_else(|e| panic!("{}: {e}", p.display()))
}

fn template(route: &str, name: &str) -> String {
    read(&templates().join(route).join(name))
}

fn exe(p: &Path, text: &str) {
    put(p, text);
    fs::set_permissions(p, fs::Permissions::from_mode(0o755)).unwrap();
}

fn copytree(src: &Path, dst: &Path) {
    fs::create_dir_all(dst).unwrap();
    for e in fs::read_dir(src).unwrap().flatten() {
        let (s, d) = (e.path(), dst.join(e.file_name()));
        if s.is_dir() {
            copytree(&s, &d);
        } else {
            fs::copy(&s, &d).unwrap();
        }
    }
}

fn listdir(p: &Path) -> Vec<String> {
    let mut v: Vec<String> =
        fs::read_dir(p).map(|rd| rd.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect()).unwrap_or_default();
    v.sort();
    v
}

fn files_under(d: &Path, out: &mut Vec<PathBuf>) {
    for e in fs::read_dir(d).unwrap().flatten() {
        let p = e.path();
        if p.is_dir() {
            files_under(&p, out);
        } else {
            out.push(p);
        }
    }
}

fn zip(p: &Path, entries: &[(String, Vec<u8>)]) {
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    let mut z = port::zipw::ZipOut::create(&p.to_string_lossy()).unwrap();
    for (n, d) in entries {
        z.add(port::zipw::Info::file(n, 0o644, 6), d).unwrap();
    }
    z.finish().unwrap();
}

fn tar_gz(files: &[(String, Vec<u8>)]) -> Vec<u8> {
    let mut tar = Vec::new();
    for (n, data) in files {
        let mut h = [0u8; 512];
        h[..n.len()].copy_from_slice(n.as_bytes());
        h[100..107].copy_from_slice(b"0000644");
        h[108..115].copy_from_slice(b"0000000");
        h[116..123].copy_from_slice(b"0000000");
        h[124..135].copy_from_slice(format!("{:011o}", data.len()).as_bytes());
        h[136..147].copy_from_slice(b"00000000000");
        h[156] = b'0';
        h[257..263].copy_from_slice(b"ustar\0");
        h[263..265].copy_from_slice(b"00");
        h[148..156].copy_from_slice(b"        ");
        let sum: u32 = h.iter().map(|b| *b as u32).sum();
        h[148..155].copy_from_slice(format!("{sum:06o}\0").as_bytes());
        tar.extend_from_slice(&h);
        tar.extend_from_slice(data);
        tar.resize(tar.len().div_ceil(512) * 512, 0);
    }
    tar.resize(tar.len() + 1024, 0);
    let mut e = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::default());
    e.write_all(&tar).unwrap();
    e.finish().unwrap()
}

/// <root>/<name>.x86_64 (an ELF header) and <name>_Data with globalgamemanagers and Managed/: a
/// Mono game for Linux whose menus read the mouse.
fn fake_game(root: &Path, version: &str, name: &str) -> PathBuf {
    let d = root.join(format!("{name}_Data"));
    put(&root.join(format!("{name}.x86_64")), made::elf(62, 3, None, &[], &[], b""));
    put(&d.join("globalgamemanagers"), sf_empty(version, 17, 24));
    put(
        &d.join("Managed/Assembly-CSharp.dll"),
        dll(&Asm {
            assembly: Some(("Assembly-CSharp", 0)),
            typerefs: &[("UnityEngine", "Input"), ("UnityEngine", "MonoBehaviour"), ("UnityEngine.UI", "InputField")],
            typedefs: &[("", "Menu")],
            methods: &["Update", "OnMouseDown", "Start"],
            memberrefs: &[(1, "GetMouseButtonDown"), (1, "get_mousePosition"), (1, "GetKeyDown")],
            modulerefs: &["steam_api"],
            pinvokes: &[(1, "SteamAPI_Init")],
        }),
    );
    put(&d.join("Managed/UnityEngine.dll"), b"MZ");
    d
}

/// The stand-in for setup/pmu: each command goes to $STUB_LOG ("pm <arguments>"), fails when it is
/// $STUB_FAIL (status $STUB_STATUS), and makes what the setup reads next.
const STUB_PMU: &str = r#"#!/bin/bash
[ $# = 0 ] && exit 2                    # (the setup's check that its tools run here)
echo "pm $*" >> "$STUB_LOG"
[ "$1" = "$STUB_FAIL" ] && exit "${STUB_STATUS:-1}"
case "$1" in
  journal-files) n=0; [ -f "$2" ] && n=$(wc -l < "$2"); echo $((n));;   # (as pmu's: the files done)
  version) echo "$STUB_VERSION (supported)";;
  player) d=$4; b=mono; abi=armeabi-v7a; a=("$@")
          for ((i = 0; i < $#; i++)); do case "${a[i]}" in --backend) b=${a[i+1]};; --abi) abi=${a[i+1]};; esac; done
          L=$d/Variations/$b/Release; mkdir -p "$L/Libs/$abi"
          for f in libmain.so libunity.so; do echo x > "$L/Libs/$abi/$f"; done
          if [ "$b" = mono ]; then
            mkdir -p "$L/MonoLibs/$abi"
            for f in libmono.so libmonobdwgc-2.0.so libMonoPosixHelper.so; do echo x > "$L/MonoLibs/$abi/$f"; done
          else echo x > "$L/Libs/$abi/libil2cpp.so"; fi;;
  macpkg) out=$2; shift 2; for f in "$@"; do echo "pkg <${f##*/}>" >> "$STUB_LOG"; done
          mkdir -p "$out/My Game.app/Contents/Resources/Data"; touch "$out/My Game.app/Contents/Resources/Data/globalgamemanagers";;
  route) [ -f "$2/My Game/My Game.x86_64" ] || exit 1
         printf "GAME_DIR='My Game'\nEXE='My Game.x86_64'\nDATA='My Game_Data'\nUNITY=%s\nBACKEND=mono\nGL_MODE=%s\n" "$STUB_VERSION" "${STUB_GL:-gles}";;
  unpack) rm "$2/data.unity3d"; touch "$2/globalgamemanagers";;
  winbcl) if [ -n "$STUB_WINBCL" ]; then mkdir -p "$3"; for n in $STUB_WINBCL; do echo x > "$3/$n"; done; fi;;
  convert) if [ -n "$PMHLSLCC" ]; then echo "env PMHLSLCC=$PMHLSLCC" >> "$STUB_LOG"; fi
           # (STUB_KILLS runs stopped as the kernel stops one that is out of memory, each after a file
           # it journaled; with STUB_KILL_STUCK before any)
           k=$(cat "$STUB_LOG.kills" 2>/dev/null || echo 0)
           if [ "$k" -lt "${STUB_KILLS:-0}" ]; then
             echo $((k + 1)) > "$STUB_LOG.kills"; [ -n "$STUB_KILL_STUCK" ] || echo done >> "$2/.pmunity.json"; kill -9 $$
           fi;;
  il2cpp) out=$3; while [ $# -gt 0 ]; do case "$1" in --pkg) pkg=$2;; esac; shift; done
          L=$pkg/Variations/il2cpp/Release/Libs/arm64-v8a; mkdir -p "$out/lib/arm64-v8a" "$L"
          echo x > "$out/lib/arm64-v8a/libil2cpp.so"; for f in libmain.so libunity.so; do echo x > "$L/$f"; done;;
  android-data) while [ $# -gt 0 ]; do case "$1" in --out) out=$2;; --name) name=$2;; --src) src=$2;; esac; shift; done
          mkdir -p "$out/zip/assets/bin/Data/Managed/etc"; echo apk > "$out/zip/$name.apk"
          # (the report as android-data writes it: the setup reads the scripting runtime in its "unity" section)
          printf '{\n "stats": {},\n "unity": {\n  "data": "%s",\n  "player": null,\n  "scripting_runtime": "%s",\n  "warnings": []\n },\n "skipped": []\n}' \
            "$STUB_VERSION" "$STUB_RUNTIME" > "$out/android_data_report.json"
          find "${src:?}" -mindepth 1 -maxdepth 1 ! -name '.*' -exec rm -r {} +;;   # (the game's copy is used up)
esac
"#;

/// PortMaster's 7-Zip as the setups call it: x -y -o<dir> <archive> (a zip); x -so <archive> (a
/// .tar.gz's tar to stdout: with 7-Zip's words and status when the archive's checksum says its
/// data is damaged); x -si -ttar -y -o<dir> (that tar, from stdin).
const STUB_7Z: &str = r#"#!/bin/bash
echo "7zz $*" >> "$STUB_LOG"
case "$2" in
  -so) gunzip -c "$3" 2>/dev/null || { echo "ERROR: CRC Failed : $(basename "$3" .gz)" >&2; exit 2; };;
  -si) o=${5#-o}; tar -xf - -C "$o";;
  *) o=${3#-o}; "$REAL_PMU" zip-extract "$4" "$o";;
esac
"#;

/// A `sync` that notes in $STUB_LOG what is on the card at that moment ("ready": the game is
/// marked set up; "setup", "work", "donor": the setup's tools, its work folder, the donors;
/// "game": the player's copy in gamedata/), and stops the setup dead at the state $STUB_CUT names
/// (a power cut).
const SYNC_SPY: &str = r#"#!/bin/sh
s=
[ -f "$GAMEDIR/.setup_done" ] && s="$s ready"
[ -d "$GAMEDIR/setup" ] && s="$s setup"
[ -d "$GAMEDIR/gamedata/.setup" ] && s="$s work"
[ -d "$GAMEDIR/donor" ] && s="$s donor"
[ -d "$GAMEDIR/gamedata/My Game" ] && s="$s game"
echo "sync:$s" >> "$STUB_LOG"
if [ -n "$STUB_CUT" ] && [ "$s" = " $STUB_CUT" ]; then kill -9 $PPID; fi
exit 0
"#;

struct Run {
    code: i32,
    stdout: String,
    stderr: String,
}

impl Run {
    fn all(&self) -> String {
        format!("{}{}", self.stdout, self.stderr)
    }
}

fn run(mut c: Command) -> Run {
    let o = c.output().unwrap();
    Run {
        code: o.status.code().unwrap_or(-1),
        stdout: String::from_utf8_lossy(&o.stdout).into_owned(),
        stderr: String::from_utf8_lossy(&o.stderr).into_owned(),
    }
}

fn path_with(first: &Path, rest: Option<&str>) -> String {
    format!("{}:{}", first.display(), rest.map(String::from).unwrap_or_else(|| std::env::var("PATH").unwrap_or_default()))
}

/// A test's folder (removed at its end).
struct Fx {
    t: PathBuf,
}

impl Drop for Fx {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.t);
    }
}

impl Fx {
    fn new(name: &str) -> Fx {
        let t = std::env::temp_dir().join(format!("pmu-setup-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&t);
        fs::create_dir_all(&t).unwrap();
        Fx { t: t.canonicalize().unwrap() }
    }

    fn p(&self, rel: &str) -> PathBuf {
        self.t.join(rel)
    }

    fn pmu(&self, args: &[&str]) -> Run {
        let mut c = Command::new(env!("CARGO_BIN_EXE_pmu"));
        c.args(args).current_dir(&self.t).env("PMUNITY_REPO", wt());
        for k in ["PMUNITY_ASMPATCH", "PMUNITY_PORT_HOOKS", "DOTNET"] {
            c.env_remove(k);
        }
        run(c)
    }

    /// A port made by `port new` from the made-up game (src/My Game) with the config (JSON) over
    /// the tests' own; returns (the game, the port's folder).
    fn make(&self, extra: &str, version: &str) -> (PathBuf, PathBuf) {
        self.make_as(extra, version, "linux")
    }

    /// make, with the game laid out as "linux", as "windows" (My Game.exe beside the data, the
    /// unityjit-win32 class libraries) or as "net35" (Unity's 2.0 Mono beside the data: a .NET 3.5
    /// build).
    fn make_as(&self, extra: &str, version: &str, layout: &str) -> (PathBuf, PathBuf) {
        let game = self.p("src/My Game");
        let d = fake_game(&game, version, "My Game");
        if layout == "windows" {
            fs::remove_file(game.join("My Game.x86_64")).unwrap();
            put(&game.join("My Game.exe"), made::pe(0x8664));
            put(&d.join("Managed/mscorlib.dll"), b"MZ Interop+Kernel32 SetThreadErrorMode");
        }
        if layout == "net35" {
            put(&d.join("Mono/etc/mono/config"), "<configuration/>");
        }
        let base = r#"{"port": {"title": "My Game", "porter": ["someone"]},
            "questions": [{"id": "TEXTURE_QUALITY", "question": "Texture quality?", "options": [["medium", "Medium"], ["fast", "Fast"]],
                           "default": "medium", "convert_set": "textures.quality"},
                          {"id": "CURSOR", "question": "Cursor?", "options": [["1", "On"], ["0", "Off"]], "env": "UA_POINTER_CURSOR"}]}"#;
        let d = |t: &str| port::pyjson::loads(t).unwrap().as_dict().cloned().unwrap();
        let cfg = port::config::merge(&d(base), &d(if extra.is_empty() { "{}" } else { extra }));
        put(&self.p("cfg.json"), port::pyjson::dumps(&port::val::Val::Dict(cfg), None, true));
        let r = self.pmu(&["port", "new", "mygame", "--data", &game.to_string_lossy(), "--config", "cfg.json", "--out", "port", "--asmpatch", "none"]);
        assert_eq!(r.code, 0, "{}", r.all());
        (game, self.p("port"))
    }

    /// A port made by port new, unzipped as ports/mygame, the player's copy of the game (its
    /// folder) in gamedata/; returns (the game, the port's folder, ports/mygame).
    fn installed(&self, extra: &str, game_folder: bool) -> (PathBuf, PathBuf, PathBuf) {
        let (game, out) = self.make(extra, "2019.4.8f1");
        let g = self.p("ports/mygame");
        copytree(&out.join("mygame"), &g);
        if game_folder {
            copytree(&game, &g.join("gamedata/My Game"));
        }
        if extra.contains("\"mono-arm64\"") {
            // (the release's Mono runtime of the Unity line)
            for n in ["libmonobdwgc-2.0.so", "libil2cpp.so"] {
                put(&g.join("setup/mono64/2019.4").join(n), "elf");
            }
        }
        (game, out, g)
    }

    /// PATH with a `df` that reports a card with 64 GB free first (the IL2CPP setups' space checks
    /// then do not depend on this machine's own free space).
    fn roomy_path(&self) -> String {
        exe(
            &self.p("roomy/df"),
            "#!/bin/sh\necho \"Filesystem 1048576-blocks Used Available Capacity Mounted on\"\necho \"card 65536 0 65536 0% /card\"\n",
        );
        path_with(&self.p("roomy"), None)
    }

    /// PATH with the sync spy first.
    fn sync_spy(&self, path: Option<&str>) -> String {
        exe(&self.p("spy/sync"), SYNC_SPY);
        path_with(&self.p("spy"), path)
    }

    /// The generated launcher's setup part: patchscript with GAMEDIR etc., the stand-in tools;
    /// returns (ports/mygame, the run, what the stand-ins noted).
    fn run_setup(&self, out: &Path, env: &[(&str, &str)]) -> (PathBuf, Run, String) {
        exe(&self.p("pm/7zzs.aarch64"), STUB_7Z);
        let g = self.p("ports/mygame");
        if !g.is_dir() {
            copytree(&out.join("mygame"), &g);
        }
        if g.join("setup").is_dir() {
            exe(&g.join("setup/pmu"), STUB_PMU);
        }
        let get = |k: &str| env.iter().find(|e| e.0 == k).map(|e| e.1);
        if get("NO_DONORS").is_none() {
            // (the release's donor/: package copies them in)
            for kind in ["mono-armv7", "il2cpp-arm64"] {
                put(&g.join("donor").join(format!("donor-{}-{kind}.apk", get("STUB_VERSION").unwrap_or("2019.4.8f1"))), "apk");
            }
        }
        let log = self.p("pm.log");
        let mut c = Command::new("bash");
        c.arg(g.join("setup/patchscript")).current_dir(&self.t);
        // (not the caller's PMHLSLCC: the setup sets it only for a port that ships pmhlslcc)
        c.env_remove("PMHLSLCC");
        for (k, v) in [
            ("DEVICE_ARCH", "aarch64"),
            ("STUB_RUNTIME", ".NET 4.x (libmonobdwgc-2.0)"),
            ("STUB_VERSION", "2019.4.8f1"),
            ("STUB_FAIL", ""),
            ("REAL_PMU", env!("CARGO_BIN_EXE_pmu")),
        ] {
            c.env(k, v);
        }
        c.env("GAMEDIR", &g).env("controlfolder", self.p("pm")).env("STUB_LOG", &log);
        for (k, v) in env {
            c.env(k, v);
        }
        let r = run(c);
        // paths relative to the port folder, in what the setup prints and in what it gives its tools: a
        // freed page that starts with an absolute card path can deadlock a FUSE card driver (Knulli A133P)
        assert!(!r.all().contains(&format!("{}/", g.display())), "an absolute path of the port folder:\n{}", r.all());
        let noted = fs::read_to_string(&log).unwrap_or_default();
        (g, r, noted)
    }

    /// The port's launcher as port new made it, on a stand-in PortMaster where a launcher looks for
    /// it ($XDG_DATA_HOME/PortMaster: control.txt with a handheld's values and PortMaster's
    /// functions, what pm_message shows going to $STUB_LOG, no patcher; a `uname` that says
    /// aarch64); the ports folder is <t>/ports. None where a real PortMaster is installed.
    fn run_launcher(&self, out: &Path) -> Option<(Run, String)> {
        if ["/opt/system/Tools/PortMaster", "/opt/tools/PortMaster"].iter().any(|d| Path::new(d).is_dir()) {
            eprintln!("a real PortMaster here: the launcher takes it (not run)");
            return None;
        }
        let x = self.p("xdg");
        let dir = self.t.to_string_lossy().trim_start_matches('/').to_string();
        put(
            &x.join("PortMaster/control.txt"),
            format!(
                "directory='{dir}'\nCFW_NAME=test; DEVICE_NAME=test; DEVICE_ARCH=aarch64; DISPLAY_WIDTH=640; DISPLAY_HEIGHT=480; ANALOG_STICKS=2\n\
                 ESUDO=; GPTOKEYB2=true; sdl_controllerconfig=\nget_controls() {{ :; }}\npm_message() {{ echo \"message: $*\" >> \"$STUB_LOG\"; }}\n\
                 pm_finish() {{ :; }}\npm_message_end() {{ :; }}\npm_platform_helper() {{ :; }}\nsleep() {{ :; }}\n"
            ),
        );
        // the firmware's mod file (PortMaster's mod_<CFW_NAME>.txt) with its own pm_platform_helper, as
        // muOS's and ROCKNIX's have: the launcher sources it after control.txt
        put(&x.join("PortMaster/mod_test.txt"), "pm_platform_helper() { echo \"mod helper: ${1##*/}\" >> \"$STUB_LOG\"; }\n");
        exe(&x.join("bin/uname"), "#!/bin/sh\necho aarch64\n");
        let log = self.p("pm.log");
        let _ = fs::remove_file(&log);
        let mut c = Command::new("bash");
        c.arg(out.join("My Game.sh")).current_dir(&self.t).env("XDG_DATA_HOME", &x).env("STUB_LOG", &log).env("PATH", path_with(&x.join("bin"), None));
        let r = run(c);
        Some((r, fs::read_to_string(&log).unwrap_or_default()))
    }
}

/// What the sync spy noted, in order.
fn syncs(log: &str) -> Vec<String> {
    log.lines().filter_map(|l| l.strip_prefix("sync:")).map(|l| l.trim().to_string()).collect()
}

fn count(hay: &str, needle: &str) -> usize {
    hay.matches(needle).count()
}

/// files.hooks: each lands as setup/hooks/<point> without .sh (harbourmaster warns on install
/// about .sh files inside the port folder), and every template's hook() sources it from there.
#[test]
fn new_hooks_without_sh() {
    let f = Fx::new("hooks");
    put(&f.p("hooks/pre_convert.sh"), "echo \"pre_convert ran\"\n");
    let (_, out) = f.make(r#"{"files": {"hooks": {"pre_convert": "hooks/pre_convert.sh"}}}"#, "2019.4.8f1");
    let setup = out.join("mygame/setup");
    assert!(setup.join("hooks/pre_convert").is_file());
    let mut all = Vec::new();
    files_under(&out.join("mygame"), &mut all);
    let sh: Vec<&PathBuf> = all.iter().filter(|p| p.to_string_lossy().ends_with(".sh")).collect();
    assert!(sh.is_empty(), "{sh:?}");
    for route in routes() {
        let text = template(&route, "patchscript");
        let fun = &text[text.find("\nhook() {").unwrap() + 1..];
        let fun = &fun[..fun.find("\n}\n").unwrap() + 3];
        let mut c = Command::new("bash");
        c.arg("-c")
            .arg(format!("fail() {{ echo \"FAIL $1\"; exit 1; }}; step() {{ echo \"step $*\"; }}\n{fun}hook pre_convert; hook post_install; echo done"))
            .env("S", &setup)
            .env("PORT_TITLE", "My Game");
        let r = run(c);
        assert_eq!(r.stdout.lines().collect::<Vec<_>>(), ["step My Game: pre_convert", "pre_convert ran", "done"], "{route}: {}", r.stderr);
    }
}

/// Every template started by a relative name with GAMEDIR=. (by hand, from the port folder) runs
/// on: its line that runs the script again by its relative name is for an absolute start only (it
/// ran itself again for ever). Started by its absolute path it runs itself again once, relative.
#[test]
fn setup_relative_start() {
    for route in routes() {
        let f = Fx::new(&format!("rel-{route}"));
        put(&f.p("setup/patchscript"), template(&route, "patchscript"));
        put(&f.p("setup/setup.env"), "echo \"setup.env read\"; exit 7\n");
        let abs = f.p("setup/patchscript").to_string_lossy().into_owned();
        let t = f.t.to_string_lossy().into_owned();
        for (start, gamedir) in [("./setup/patchscript", "."), (abs.as_str(), t.as_str())] {
            let mut c = Command::new("timeout");
            c.args(["30", "bash", start]).current_dir(&f.t).env("GAMEDIR", gamedir);
            let r = run(c);
            assert_eq!((r.code, r.stdout.as_str()), (7, "setup.env read\n"), "{route} {start}: {}", r.stderr);
        }
    }
}

/// A release without the donor of the game's version: the setup says so, and downloads nothing.
#[test]
fn setup_no_donor() {
    let f = Fx::new("nodonor");
    let (_, out, _) = f.installed("", true);
    let (_, r, log) = f.run_setup(&out, &[("NO_DONORS", "1")]);
    assert!(
        r.stdout.contains(
            "PATCH_FAIL_MSG:This port ships no donor of Unity 2019.4.8f1 (mono, armeabi-v7a): donor/donor-2019.4.8f1-mono-armv7.apk is missing"
        ),
        "{}",
        r.all()
    );
    assert!(!log.contains("pm player"));
    for p in ["android", "mono-arm64"] {
        let text = template(p, "patchscript");
        for net in ["curl", "wget", "player fetch", "player url", "SSL_CERT_FILE", "--installer"] {
            assert!(!text.contains(net), "{p}: {net}");
        }
    }
}

#[test]
fn setup_folder() {
    let f = Fx::new("folder");
    let (_, out, g) = f.installed("", true);
    let mut st = fs::OpenOptions::new().append(true).open(g.join("setup.txt")).unwrap();
    st.write_all(b"cursor=0\n").unwrap();
    drop(st);
    // first run: the conversion fails (interrupted), the second continues from there
    let (_, r, _) = f.run_setup(&out, &[("TEXTURE_QUALITY", "fast"), ("STUB_FAIL", "convert")]);
    assert!(r.stdout.contains("PATCH_FAIL_MSG:Converting"), "{}", r.all());
    // (as a setup begun by an older release left it: the data folder's absolute path; the setup goes
    // on with it, relative, and still removes the game's copy at the end)
    let data = g.join("gamedata/.setup/.data");
    assert_eq!(read(&data).trim(), "./gamedata/My Game/My Game_Data");
    put(&data, format!("{}\n", g.join("gamedata/My Game/My Game_Data").display()));
    let (_, r, log) = f.run_setup(&out, &[("TEXTURE_QUALITY", "medium")]);
    assert_eq!(r.code, 0, "{}", r.all());
    assert_eq!(count(&log, "pm player donor 2019.4.8f1"), 1);
    assert!(log.contains("--apk ./donor/donor-2019.4.8f1-mono-armv7.apk"), "{log}");
    assert!(log.contains("--set textures.quality=fast")); // (the first answer is kept)
    assert!(log.contains("--name MyGame --package com.pmunity.mygame"));
    assert!(!log.contains("--asmpatch"));
    assert!(g.join(".setup_done").is_file());
    // (the Mono of the game's scripting runtime: .NET 4.x, libmonobdwgc-2.0)
    assert_eq!(listdir(&g.join("lib")), ["libMonoPosixHelper.so", "libmain.so", "libmonobdwgc-2.0.so", "libunity.so"]);
    assert!(g.join("MyGame.apk").is_file());
    assert_eq!(read(&g.join("answers.env")).trim(), "export UA_POINTER_CURSOR='0'");
    assert_eq!(listdir(&g.join("gamedata")), ["PUT_GAME_FILES_HERE.txt"]);
    assert!(!g.join("setup").exists());
    assert!(read(&g.join("patchlog.txt")).contains("setup finished"));
}

/// A compressed build's data.unity3d is unpacked whenever it is still there: also when an unpack
/// that was stopped left some of its files (globalgamemanagers) beside it, as the player would
/// read the archive first.
#[test]
fn setup_compressed_build() {
    for left in [false, true] {
        let f = Fx::new(&format!("packed-{left}"));
        let (_, out, g) = f.installed("", true);
        let data = g.join("gamedata/My Game/My Game_Data");
        put(&data.join("data.unity3d"), "UnityFS");
        if !left {
            fs::remove_file(data.join("globalgamemanagers")).unwrap();
        }
        let (_, r, log) = f.run_setup(&out, &[]);
        assert_eq!(r.code, 0, "{}", r.all());
        assert_eq!(count(&log, "pm unpack ./gamedata/My Game/My Game_Data --remove"), 1, "{left}: {log}");
    }
    for p in ["android", "mono-arm64", "il2cpp-arm64"] {
        assert!(template(p, "patchscript").contains("\n  if [ -f \"$D/data.unity3d\" ]; then\n"), "{p}");
    }
}

/// convert stopped by the kernel (out of memory on a 1 GB handheld: status 137) runs again by
/// itself while its journal grows, and stops the setup when a run gets no further; a card too
/// full (status 3) and a damaged journal (5) say what to do; a card too full to pack (4) too.
#[test]
fn setup_convert_stopped() {
    type Case<'a> = (&'a [(&'a str, &'a str)], i32, Option<&'a str>, usize);
    let cases: [Case; 5] = [
        (&[("STUB_KILLS", "2")], 0, None, 3),
        (&[("STUB_KILLS", "1"), ("STUB_KILL_STUCK", "1")], 1, Some("PATCH_FAIL_MSG:Converting the game's files failed"), 1),
        (&[("STUB_FAIL", "convert"), ("STUB_STATUS", "3")], 1, Some("PATCH_FAIL_MSG:The card is too full to convert the game"), 1),
        (
            &[("STUB_FAIL", "convert"), ("STUB_STATUS", "5")],
            1,
            Some("PATCH_FAIL_MSG:The handheld restarted while the conversion wrote its journal"),
            1,
        ),
        (&[("STUB_FAIL", "android-data"), ("STUB_STATUS", "4")], 1, Some("PATCH_FAIL_MSG:The card is too full to pack the game"), 1),
    ];
    for (i, (env, code, msg, runs)) in cases.iter().enumerate() {
        let f = Fx::new(&format!("stopped-{i}"));
        let (_, out, _) = f.installed("", true);
        let (_, r, log) = f.run_setup(&out, env);
        assert_eq!(r.code, *code, "{env:?}: {}", r.all());
        assert_eq!(count(&log, "pm convert "), *runs, "{env:?}: {log}");
        if let Some(m) = msg {
            assert!(r.stdout.contains(m), "{env:?}: {}", r.stdout);
        }
        assert_eq!(count(&r.stdout, "it starts again where it stopped"), if *code == 0 { 2 } else { 0 }, "{env:?}");
    }
    for p in ["android", "mono-arm64", "il2cpp-arm64", "box64"] {
        assert!(template(p, "patchscript").contains("pm_convert \"$"), "{p}");
    }
}

/// The game as a zip: unpacked by PortMaster's 7-Zip; a copy built with another Unity version is
/// refused; asmpatch when the port uses it; the archive is removed once the game is built.
#[test]
fn setup_archive_and_checks() {
    let f = Fx::new("archive");
    let (game, out, g) = f.installed(r#"{"android_data": {"asmpatch": "auto"}}"#, false);
    exe(&g.join("setup/asmpatch"), "#!/bin/sh\n");
    let z = g.join("gamedata/game-linux.zip");
    let mut all = Vec::new();
    files_under(&game, &mut all);
    all.sort();
    let entries: Vec<(String, Vec<u8>)> =
        all.iter().map(|p| (p.strip_prefix(game.parent().unwrap()).unwrap().to_string_lossy().into_owned(), fs::read(p).unwrap())).collect();
    zip(&z, &entries);
    // another Unity version than the port's: refused
    let (_, r, log) = f.run_setup(&out, &[("STUB_VERSION", "2019.4.9f1")]);
    assert!(
        r.stdout.contains("PATCH_FAIL_MSG:This port is made for My Game built with Unity 2019.4.8f1; your copy was built with Unity 2019.4.9f1"),
        "{}",
        r.all()
    );
    assert!(log.contains("7zz x -y -o"), "{log}");
    fs::remove_dir_all(g.join("gamedata/.setup")).unwrap();
    let (_, r, log) = f.run_setup(&out, &[("STUB_RUNTIME", ".NET 3.5 (libmono)")]);
    assert_eq!(r.code, 0, "{}", r.all());
    assert!(log.contains("--asmpatch './setup/asmpatch'"), "{log}");
    assert!(log.contains("--set textures.quality=medium")); // (the default)
    assert!(!listdir(&g.join("lib")).contains(&"libmonobdwgc-2.0.so".to_string()), "{:?}", listdir(&g.join("lib")));
    assert!(!z.is_file()); // (the archive is removed once built)
    assert!(read(&g.join("patchlog.txt")).contains("removing game-linux.zip: not needed to play"));
    assert!(read(&g.join("setup.txt")).contains("keep_installers=0"));
}

/// The il2cpp-arm64 setup: what changes the game's code (the port's assembly patches, asmpatch
/// with the opt-in patches for StreamingAssets and dataPath read with System.IO) goes to the
/// IL2CPP build, and android-data packs the same StreamingAssets and dataPath modes.
#[test]
fn setup_il2cpp_arm64() {
    let f = Fx::new("il2cpp");
    put(&f.p("mypatch.sh"), "#!/bin/sh\n");
    let (_, out, g) = f.installed(
        r#"{"port": {"route": "il2cpp-arm64"}, "android_data": {"streaming_assets": "both", "data_path": true},
            "files": {"assembly_patches": [{"name": "Assembly-CSharp.dll", "files": ["mypatch.sh"], "command": "sh {setup}/files/mypatch.sh {managed} {out}"}]}}"#,
        true,
    );
    assert!(template("il2cpp-arm64", "route.json").contains("\"asmpatch?\""));
    exe(&g.join("setup/asmpatch"), "#!/bin/sh\n");
    let path = f.roomy_path();
    let (_, r, log) = f.run_setup(&out, &[("PATH", &path)]);
    assert_eq!(r.code, 0, "{}", r.all());
    let il = log.lines().find(|l| l.starts_with("pm il2cpp ")).unwrap();
    assert!(il.contains("--assembly-patch Assembly-CSharp.dll sh ./setup/files/mypatch.sh {managed} {out}"), "{il}");
    assert!(il.contains("--streaming-assets both --data-path --asmpatch './setup/asmpatch' --only="), "{il}");
    let ad = log.lines().find(|l| l.starts_with("pm android-data ")).unwrap();
    assert!(ad.contains("--streaming-assets both --data-path"), "{ad}");
    for o in ["--asmpatch", "--managed-file", "--assembly-patch"] {
        assert!(!ad.contains(o), "{o}: {ad}");
    }
    assert_eq!(listdir(&g.join("lib")), ["libil2cpp.so", "libmain.so", "libunity.so"]);
    assert!(g.join(".setup_done").is_file());
    // asmpatch = "auto" and the modes left at obb: asmpatch's own choice of patches, nothing opt-in
    fs::remove_dir_all(&g).unwrap();
    let (_, out, g) = f.installed(r#"{"port": {"route": "il2cpp-arm64"}, "android_data": {"asmpatch": "auto"}}"#, true);
    exe(&g.join("setup/asmpatch"), "#!/bin/sh\n");
    fs::remove_file(f.p("pm.log")).unwrap();
    let (_, r, log) = f.run_setup(&out, &[("PATH", &path)]);
    assert_eq!(r.code, 0, "{}", r.all());
    let il = log.lines().find(|l| l.starts_with("pm il2cpp ")).unwrap();
    assert!(il.ends_with("--streaming-assets obb --asmpatch './setup/asmpatch'"), "{il}");
}

/// A Windows build of Unity 2021.2+ (Direct3D 11 shaders, the unityjit-win32 class libraries): the
/// setup runs winbcl with the player after it has it, PMHLSLCC for the conversion, and ships
/// winbcl's files with --managed-file; android_data.winbcl = "off": not run.
#[test]
fn setup_windows_port() {
    let f = Fx::new("windows");
    let (game, out) = f.make_as("", "2022.2.17f1", "windows");
    let env = read(&out.join("mygame/setup/setup.env"));
    assert!(env.contains("WINBCL=auto"), "{env}");
    let g = f.p("ports/mygame");
    copytree(&out.join("mygame"), &g);
    copytree(&game, &g.join("gamedata/My Game"));
    exe(&g.join("setup/pmhlslcc"), "#!/bin/sh\n");
    let (_, r, log) = f.run_setup(&out, &[("STUB_VERSION", "2022.2.17f1"), ("STUB_WINBCL", "System.dll netstandard.dll")]);
    assert_eq!(r.code, 0, "{}", r.all());
    let wb = log.lines().find(|l| l.starts_with("pm winbcl ")).unwrap();
    assert_eq!(
        wb,
        "pm winbcl ./gamedata/My Game/My Game_Data ./gamedata/.setup/bcl --version 2022.2.17f1 --player ./gamedata/.setup/player --donor ./donor/donor-2022.2.17f1-mono-armv7.apk"
    );
    assert!(log.contains("env PMHLSLCC=./setup/pmhlslcc"), "{log}");
    let ad = log.lines().find(|l| l.starts_with("pm android-data ")).unwrap();
    assert!(ad.contains("--managed-file ./gamedata/.setup/bcl/System.dll --managed-file ./gamedata/.setup/bcl/netstandard.dll"), "{ad}");
    assert!(log.find("pm player donor").unwrap() < log.find("pm winbcl").unwrap());
    assert!(g.join(".setup_done").is_file());
    // winbcl = "off": not run
    fs::remove_dir_all(&g).unwrap();
    let (game, out) = f.make_as(r#"{"android_data": {"winbcl": "off"}}"#, "2022.2.17f1", "windows");
    copytree(&out.join("mygame"), &g);
    copytree(&game, &g.join("gamedata/My Game"));
    fs::remove_file(f.p("pm.log")).unwrap();
    let (_, r, log) = f.run_setup(&out, &[("STUB_VERSION", "2022.2.17f1"), ("STUB_WINBCL", "System.dll")]);
    assert_eq!(r.code, 0, "{}", r.all());
    assert!(!log.contains("pm winbcl"));
    assert!(!log.contains("--managed-file"));
    assert!(!log.contains("PMHLSLCC")); // (no pmhlslcc in this copy of the port)
}

/// A .NET 3.5 game of Unity 2017.4 on the mono-arm64 route (docs/mono-arm64.md, Cuphead): port new
/// sets player.mono64_classlibs from the check, and the setup packs the release's 4.x class
/// libraries after the game's, with their config; the game's own class library without a 4.x one
/// is named in the log.
#[test]
fn setup_mono_arm64_net35() {
    let f = Fx::new("net35");
    let (game, out) = f.make_as(r#"{"port": {"route": "mono-arm64"}}"#, "2017.4.40f1", "net35");
    assert!(read(&out.join("mygame/setup/setup.env")).contains("MONO64_CLASSLIBS=1"));
    assert!(read(&out.join("port.build.json")).contains("\"mono64_classlibs\": true"));
    put(&game.join("My Game_Data/Managed/System.Threading.dll"), "MZ");
    let g = f.p("ports/mygame");
    copytree(&out.join("mygame"), &g);
    copytree(&game, &g.join("gamedata/My Game"));
    let gm = g.join("setup/mono64/2017.4");
    for n in port::config::MONO64_CLASSLIBS {
        put(&gm.join("classlibs").join(n), format!("MZ {n}"));
    }
    put(&gm.join("etc/mono/config"), "<configuration/>");
    put(&gm.join("etc/mono/4.5/machine.config"), "<configuration/>");
    for n in ["libmonobdwgc-2.0.so", "libil2cpp.so"] {
        put(&gm.join(n), "elf");
    }
    let path = f.roomy_path();
    let (_, r, log) = f.run_setup(&out, &[("PATH", &path), ("STUB_VERSION", "2017.4.40f1"), ("STUB_RUNTIME", ".NET 3.5 (libmono)")]);
    assert_eq!(r.code, 0, "{}", r.all());
    let ad = log.lines().find(|l| l.starts_with("pm android-data ")).unwrap();
    assert!(ad.contains("--managed-file ./setup/mono64/2017.4/classlibs/mscorlib.dll"), "{ad}");
    assert!(ad.contains("--mono-etc ./setup/mono64/2017.4/etc"), "{ad}");
    assert!(r.all().contains("System.Threading.dll: no 4.x class library of that name"), "{}", r.all());
}

/// The end of a setup: the used-up copy of the game goes, the card is synced, the game is marked
/// set up, the card is synced, and only then the setup's tools, its work folder and the donors go
/// (the sync spy notes what is on the card at each sync). A power cut right after the mark: the
/// game is whole, and the launcher starts it and removes what the setup left.
#[test]
fn setup_end_order() {
    for route in ["android", "mono-arm64", "il2cpp-arm64"] {
        let cfg = format!(r#"{{"port": {{"route": "{route}"}}}}"#);
        {
            let f = Fx::new(&format!("end-{route}"));
            let (_, out, g) = f.installed(&cfg, true);
            let path = f.sync_spy(Some(&f.roomy_path()));
            let (_, r, log) = f.run_setup(&out, &[("PATH", &path)]);
            assert_eq!(r.code, 0, "{route}: {}", r.all());
            let seen = syncs(&log);
            let i = seen.iter().position(|s| s == "ready setup work donor").unwrap_or_else(|| panic!("{route}: {seen:?}"));
            assert_eq!(seen[i - 1], "setup work donor", "{route}: {seen:?}"); // (before the mark: the copy gone, synced)
            assert_eq!(seen.last().unwrap(), "ready", "{route}: {seen:?}");
            assert!(seen.iter().all(|x| x.split(' ').any(|w| w == "setup") || x == "ready"), "{route}: {seen:?}");
            assert_eq!(listdir(&g.join("gamedata")), ["PUT_GAME_FILES_HERE.txt"]);
            for d in ["setup", "donor"] {
                assert!(!g.join(d).exists(), "{route}: {d}");
            }
        }
        let f = Fx::new(&format!("cut-{route}"));
        let (_, out, g) = f.installed(&cfg, true);
        let path = f.sync_spy(Some(&f.roomy_path()));
        let (_, r, _) = f.run_setup(&out, &[("PATH", &path), ("STUB_CUT", "ready setup work donor")]);
        assert_ne!(r.code, 0, "{route}");
        assert!(!read(&g.join("patchlog.txt")).contains("setup finished"));
        for x in [".setup_done", "MyGame.apk", "lib/libunity.so", "answers.env", "setup/patchscript"] {
            assert!(g.join(x).is_file(), "{route}: {x}");
        }
        assert!(g.join("gamedata/.setup").is_dir());
        if route == "android" {
            continue; // (its launcher wants a 32-bit userland: not run here)
        }
        exe(&g.join("ua_host"), "#!/bin/sh\necho \"ua_host $1\" >> \"$STUB_LOG\"\n");
        let Some((r, log)) = f.run_launcher(&out) else { continue };
        assert_eq!(r.code, 0, "{route}: {}", r.all());
        assert!(log.contains(&format!("ua_host {}", g.display())), "{route}: {log}");
        assert!(log.contains("mod helper: ua_host"), "{route}: {log}"); // (the firmware's mod file, sourced)
        assert!(!log.contains("message:"), "{route}: {log}");
        for d in ["setup", "donor", "gamedata/.setup"] {
            assert!(!g.join(d).exists(), "{route}: {d}");
            assert!(read(&g.join("log.txt")).contains(&format!("removed {d}: the setup's, not needed to play")), "{route}: {d}");
        }
        assert_eq!(listdir(&g.join("gamedata")), ["PUT_GAME_FILES_HERE.txt"]);
    }
}

/// The setup's progress files: one the card does not take stops the setup with the reason in plain
/// words (a full card) and nothing is marked done; notes that came back empty, or a game folder
/// that went before it was packed, redo the step; the game is not marked set up when the mark
/// cannot be written, and the tools stay to finish it.
#[test]
fn setup_progress_files() {
    let f = Fx::new("progress");
    let (_, out, g) = f.installed("", true);
    let w = g.join("gamedata/.setup");
    let full = " to the card: is the card full? Free some space on it, then launch again.";
    // the note of the game's data folder cannot be written
    fs::create_dir_all(w.join(".data")).unwrap();
    let (_, r, log) = f.run_setup(&out, &[]);
    assert!(r.stdout.contains(&format!("PATCH_FAIL_MSG:Could not write the setup's progress{full}")), "{}", r.all());
    assert!(!w.join(".step_game").exists());
    assert!(!log.contains("pm player"));
    fs::remove_dir(w.join(".data")).unwrap();
    // a step's marker cannot be written (a link to nowhere stands in for the full card)
    std::os::unix::fs::symlink(f.p("nowhere/x"), w.join(".step_player")).unwrap();
    let (_, r, log) = f.run_setup(&out, &[]);
    assert!(r.stdout.contains(&format!("PATCH_FAIL_MSG:Could not write the setup's progress{full}")), "{}", r.all());
    assert!(w.join(".step_game").is_file());
    assert!(log.contains("pm player"));
    assert!(!log.contains("pm winbcl")); // (the next step did not start)
    fs::remove_file(w.join(".step_player")).unwrap();
    // the answers cannot be written
    fs::create_dir_all(w.join("answers.env.new")).unwrap();
    let (_, r, log) = f.run_setup(&out, &[]);
    assert!(r.stdout.contains(&format!("PATCH_FAIL_MSG:Could not write the setup's answers{full}")), "{}", r.all());
    fs::remove_dir(w.join("answers.env.new")).unwrap();
    // the note of the data folder came back empty (a power cut): step 1 again
    put(&w.join(".data"), "");
    let n = count(&log, "pm version");
    let (_, r, log) = f.run_setup(&out, &[("STUB_FAIL", "convert")]);
    assert!(r.stdout.contains("the setup's notes of the game are incomplete (or the game's folder is gone): starting again"), "{}", r.all());
    assert_eq!(count(&log, "pm version"), n + 1);
    assert!(r.stdout.contains("PATCH_FAIL_MSG:Converting"));
    assert_eq!(read(&w.join(".data")).trim(), "./gamedata/My Game/My Game_Data");
    // the game's folder went before it was packed: the player is asked for the game again
    fs::rename(g.join("gamedata/My Game"), f.p("away")).unwrap();
    let (_, r, _) = f.run_setup(&out, &[]);
    assert!(r.stdout.contains("starting again from the game"), "{}", r.all());
    assert!(r.stdout.contains("PATCH_FAIL_MSG:Put "));
    fs::rename(f.p("away"), g.join("gamedata/My Game")).unwrap();
    // the last mark cannot be written: the game is not marked set up, the tools stay; the next
    // launch (the copy of the game is gone by then: its files are packed) finishes
    std::os::unix::fs::symlink(f.p("nowhere/x"), g.join(".setup_done")).unwrap();
    let (_, r, log) = f.run_setup(&out, &[]);
    assert!(r.stdout.contains(&format!("PATCH_FAIL_MSG:Could not write the setup's last mark{full}")), "{}", r.all());
    assert!(g.join("setup/patchscript").is_file());
    assert!(w.join(".step_package").is_file());
    assert!(!g.join("gamedata/My Game").exists());
    fs::remove_file(g.join(".setup_done")).unwrap();
    let n = count(&log, "pm android-data");
    let (_, r, log) = f.run_setup(&out, &[]);
    assert_eq!(r.code, 0, "{}", r.all());
    assert!(!r.stdout.rsplit("=== ").next().unwrap().contains("starting again from the game"));
    assert_eq!(count(&log, "pm android-data"), n); // (nothing is packed twice)
    assert!(g.join(".setup_done").is_file());
    assert!(!g.join("setup").exists());
    // the same helpers on every route; the mark, then the cleanup: never the other way round
    let (mark, step, ready, cleanup) =
        ("mark() { sync; touch \"$W/.step_$1\"", "touch \"$W/.step_", "touch \"$GAMEDIR/$READY\" || nowrite", ["rm", "-rf", "\"$W\"", "\"$S\""].join(" "));
    for route in ["android", "mono-arm64", "il2cpp-arm64", "apk", "box64"] {
        let text = template(route, "patchscript");
        assert!(text.contains("nowrite() { fail \"Could not write $1 to the card: is the card full?"), "{route}");
        assert!(text.contains(&format!("{mark} || nowrite")), "{route}");
        assert!(!text.replace(mark, "").contains(step), "{route}");
        assert!(!text.contains("eval "), "{route}");
        assert!(text.find(ready).unwrap() < text.find(&cleanup).unwrap(), "{route}");
    }
}

/// macOS installers (.pkg) in gamedata/: all of them go to macpkg in one call, the largest first,
/// a name with spaces as one argument.
#[test]
fn setup_pkg_names() {
    let f = Fx::new("pkg");
    let (_, out, g) = f.installed("", false);
    for (n, size) in [("My Game Part 2.pkg", 10), ("My Game.pkg", 5000), ("extras.pkg", 300)] {
        put(&g.join("gamedata").join(n), vec![b'x'; size]);
    }
    let (_, r, log) = f.run_setup(&out, &[]);
    assert_eq!(r.code, 0, "{}", r.all());
    assert_eq!(count(&log, "pm macpkg"), 1);
    assert_eq!(
        log.lines().filter(|l| l.starts_with("pkg <")).collect::<Vec<_>>(),
        ["pkg <My Game.pkg>", "pkg <extras.pkg>", "pkg <My Game Part 2.pkg>"]
    );
    assert_eq!(listdir(&g.join("gamedata")), ["PUT_GAME_FILES_HERE.txt"]);
    assert!(read(&g.join("patchlog.txt")).contains("removing My Game Part 2.pkg: not needed to play"));
}

/// The game as a .tar.gz: unpacked through 7-Zip's pipe (decompress | untar). An archive whose
/// checksum says its data is damaged still gives a whole tar that unpacks: the decompressor's
/// status stops the setup, with its words in patchlog.txt.
#[test]
fn setup_tar_gz() {
    let f = Fx::new("targz");
    let (game, out, g) = f.installed("", false);
    let a = g.join("gamedata/my game.tar.gz");
    let mut all = Vec::new();
    files_under(&game, &mut all);
    all.sort();
    let entries: Vec<(String, Vec<u8>)> =
        all.iter().map(|p| (format!("My Game/{}", p.strip_prefix(&game).unwrap().display()), fs::read(p).unwrap())).collect();
    let good = tar_gz(&entries);
    let mut bad = good.clone();
    let n = bad.len();
    bad[n - 8] ^= 0xff; // (the data's CRC-32, in the gzip trailer)
    put(&a, &bad);
    let (_, r, log) = f.run_setup(&out, &[]);
    assert!(r.stdout.contains("PATCH_FAIL_MSG:Could not unpack my game.tar.gz (see patchlog.txt)."), "{}", r.all());
    assert!(read(&g.join("patchlog.txt")).contains("ERROR: CRC Failed : my game.tar"));
    // (the untar part had nothing to say)
    assert!(g.join("gamedata/.setup/unpacked/My Game/My Game_Data/globalgamemanagers").is_file());
    assert!(!log.contains("pm version"));
    put(&a, &good);
    let (_, r, log) = f.run_setup(&out, &[]);
    assert_eq!(r.code, 0, "{}", r.all());
    assert!(log.contains("7zz x -so ./gamedata/my game.tar.gz"), "{log}");
    assert!(log.contains("7zz x -si -ttar -y -o"));
    assert!(!a.is_file());
    assert!(g.join(".setup_done").is_file());
    for route in ["android", "mono-arm64", "il2cpp-arm64", "box64"] {
        let text = template(route, "patchscript");
        assert!(text.contains("( set -o pipefail; \"$SEVENZ\" x -so \"$a\" | \"$SEVENZ\" x -si -ttar -y -o"), "{route}");
        assert!(!text.contains("x -so \"$a\" 2>/dev/null"), "{route}");
    }
}

/// The box64 setup: the game `route` finds, the GLES shaders, game.env for the launcher; the game
/// is marked set up before the setup's files go; notes of the game that came back empty redo the
/// step. The launcher of a set-up game whose game.env or game folder is gone says so.
#[test]
fn setup_box64() {
    let f = Fx::new("box64");
    let (_, out, g) = f.installed(r#"{"port": {"route": "box64"}}"#, true);
    let w = g.join("gamedata/.setup");
    let path = f.sync_spy(None);
    let (_, r, _) = f.run_setup(&out, &[("PATH", &path), ("STUB_FAIL", "convert")]);
    assert!(r.stdout.contains("PATCH_FAIL_MSG:Converting the game's shaders failed"), "{}", r.all());
    assert!(read(&w.join("game.env")).contains("EXE='My Game.x86_64'"));
    put(&w.join("game.env"), ""); // (the card did not keep it)
    let (_, r, log) = f.run_setup(&out, &[("PATH", &path)]);
    assert_eq!(r.code, 0, "{}", r.all());
    assert!(r.stdout.contains("starting again from the game"));
    assert_eq!(count(&log, "pm route"), 2);
    assert!(read(&g.join("game.env")).contains("GAME_DIR='My Game'"));
    let seen = syncs(&log);
    let i = seen.iter().position(|s| s == "ready setup work donor game").unwrap_or_else(|| panic!("{seen:?}"));
    assert_eq!(seen[i - 1], "setup work donor game", "{seen:?}");
    assert_eq!(seen.last().unwrap(), "ready game", "{seen:?}");
    assert!(seen.iter().all(|x| x.split(' ').any(|w| w == "setup") || x == "ready game"), "{seen:?}");
    assert!(g.join("gamedata/My Game/My Game.x86_64").is_file()); // (the game stays: box64 runs it)
    for d in ["setup", "donor", "gamedata/.setup"] {
        assert!(!g.join(d).exists(), "{d}");
    }
    // the launcher: game.env gone, then the game's folder gone
    fs::rename(g.join("game.env"), f.p("game.env")).unwrap();
    let Some((r, log)) = f.run_launcher(&out) else { return };
    assert_eq!(r.code, 1, "{}", r.all());
    assert!(
        log.contains(
            "message: My Game's files are not where the setup left them (ports/mygame/gamedata). To set it up again: install the port again, put "
        ),
        "{log}"
    );
    fs::rename(f.p("game.env"), g.join("game.env")).unwrap();
    fs::remove_dir_all(g.join("gamedata/My Game")).unwrap();
    let (r, log) = f.run_launcher(&out).unwrap();
    assert_eq!(r.code, 1);
    assert!(log.contains("message: My Game's files are not where the setup left them"), "{log}");
}

/// A launch of a game that is not set up (here PortMaster has no patcher: no setup runs): with the
/// setup's files the player is asked for the game, without them (setup/ deleted, or the folder
/// copied without it) to install the port again. A set-up game with the setup's files of a release
/// unzipped over it, or of a setup cut off after its mark: they are removed, the game starts.
#[test]
fn launcher_not_set_up() {
    for route in ["mono-arm64", "il2cpp-arm64", "box64"] {
        let f = Fx::new(&format!("launch-{route}"));
        let (_, out, g) = f.installed(&format!(r#"{{"port": {{"route": "{route}"}}}}"#), true);
        let Some((r, log)) = f.run_launcher(&out) else { break };
        assert_eq!(r.code, 1, "{route}: {}", r.all());
        assert!(log.contains("message: My Game is not set up yet: put "), "{route}: {log}");
        assert!(!log.contains("Install the port again"));
        fs::remove_dir_all(g.join("setup")).unwrap();
        let (r, log) = f.run_launcher(&out).unwrap();
        assert_eq!(r.code, 1);
        assert!(
            log.contains("message: My Game is not set up and its setup files are missing. Install the port again (unzip it over your ports folder), put "),
            "{route}: {log}"
        );
        assert!(g.join("gamedata/My Game").is_dir()); // (nothing of the player's is touched)
        if route == "box64" {
            continue; // (its launcher goes on to box64 and Westonpack: setup_box64)
        }
        put(&g.join(".setup_done"), "");
        for d in ["setup", "donor", "gamedata/.setup/player"] {
            fs::create_dir_all(g.join(d)).unwrap();
        }
        put(&g.join("setup/patchscript"), "exit 1\n");
        exe(&g.join("ua_host"), "#!/bin/sh\necho \"ua_host $1\" >> \"$STUB_LOG\"\n");
        let (r, log) = f.run_launcher(&out).unwrap();
        assert_eq!(r.code, 0, "{route}: {}", r.all());
        assert!(log.contains(&format!("ua_host {}", g.display())), "{route}: {log}");
        assert!(log.contains("mod helper: ua_host")); // (the firmware's mod file, sourced)
        assert!(!log.contains("message:"));
        for d in ["setup", "donor", "gamedata/.setup"] {
            assert!(!g.join(d).exists(), "{route}: {d}");
        }
        assert!(g.join("gamedata/My Game").is_dir());
    }
    // every route's launcher: both messages, and the leftovers of a set-up game removed (the android
    // one after its update, which needs setup/ until it has run)
    let leftovers = "for d in setup donor gamedata/.setup; do";
    let remove = ["rm", "-rf", "\"${GAMEDIR:?}/$d\""].join(" ");
    for route in ["android", "apk", "box64", "il2cpp-arm64", "mono-arm64"] {
        let text = template(route, "launcher.sh");
        assert!(text.contains("is not set up and its setup files are missing. Install the port again (unzip it over your ports folder)"), "{route}");
        assert!(text.contains("if [ -f \"$GAMEDIR/setup/patchscript\" ]; then\n      pm_message \"@@TITLE@@ is not set up yet"), "{route}");
        assert!(text.contains(&format!("{leftovers}\n")), "{route}");
        assert!(text.contains(&remove), "{route}");
        assert!(text.find("pm_message \"@@TITLE@@ is not set up yet").unwrap() < text.find(leftovers).unwrap(), "{route}");
    }
    assert!(template("android", "launcher.sh").contains(&format!("if [ ! -f \"$GAMEDIR/setup/update\" ]; then\n  {leftovers}")));
    // (both of the update's ends)
    let ends = ["rm", "-rf", "\"$S\"", "\"$GAMEDIR/gamedata/.setup\"", "\"$GAMEDIR/donor\""].join(" ");
    assert_eq!(count(&template("android", "update"), &ends), 2);
}

// ---------------------------------------------------------------------------------------------
// The apk route: its setup with the real pmu (`pmu apk`) on made-up Android builds.

const BID: &str = "0123abcd-0000-1111-2222-333344445555";

fn made_apk(dir: &Path, name: &str, pkg: &str, version: &str) -> PathBuf {
    fs::create_dir_all(dir).unwrap();
    let mf = apkmade::manifest(pkg, 7, None, &[("unity.build-id", apkmade::s(BID))]);
    apkmade::make_apk(&dir.join(name), &mf, &["arm64-v8a", "armeabi-v7a"], Some("il2cpp"), version, true)
}

/// A port made from the build `data`, installed as ports/mygame (over the installed one: as
/// PortMaster installs it again) with `files` in gamedata/ and the given runtimes; setup/pmu is
/// the real pmu.
fn apk_port(f: &Fx, data: &Path, files: &[&Path], hosts: &[&str], cfg: &str, reinstall: bool) -> PathBuf {
    put(&f.p("cfg.json"), if cfg.is_empty() { r#"{"port": {"title": "My Game"}}"# } else { cfg });
    let r = f.pmu(&["port", "new", "mygame", "--data", &data.to_string_lossy(), "--config", "cfg.json", "--out", "port"]);
    assert_eq!(r.code, 0, "{}", r.all());
    let g = f.p("ports/mygame");
    if !reinstall {
        let _ = fs::remove_dir_all(&g);
    }
    copytree(&f.p("port/mygame"), &g);
    for h in hosts {
        put(&g.join(h), "#!/bin/sh\n");
    }
    fs::copy(env!("CARGO_BIN_EXE_pmu"), g.join("setup/pmu")).unwrap();
    for x in files {
        fs::copy(x, g.join("gamedata").join(x.file_name().unwrap())).unwrap();
    }
    g
}

fn apk_setup(f: &Fx, g: &Path, env: &[(&str, &str)]) -> Run {
    let mut c = Command::new("bash");
    c.arg(g.join("setup/patchscript")).current_dir(&f.t).env("GAMEDIR", g).env("controlfolder", f.p("ports/PortMaster")).env("DEVICE_ARCH", "aarch64");
    for (k, v) in env {
        c.env(k, v);
    }
    let r = run(c);
    assert!(!r.all().contains(&format!("{}/", g.display())), "an absolute path of the port folder:\n{}", r.all());
    r
}

fn armhf_userland() -> bool {
    Path::new("/lib/ld-linux-armhf.so.3").exists()
}

fn aarch64_linux() -> bool {
    cfg!(target_arch = "aarch64") && cfg!(target_os = "linux")
}

#[test]
fn apk_setup_installs() {
    if !aarch64_linux() {
        eprintln!("apk_setup_installs: skipped (setup/pmu is this build of pmu: an aarch64 Linux is tested)");
        return;
    }
    // (the 64-bit player is what is tested: where the system also has a 32-bit userland, the port
    // as a release without the 32-bit ua_host, which the setup would take first)
    let hosts: &[&str] = if armhf_userland() { &["ua_host64"] } else { &["ua_host", "ua_host64"] };
    let f = Fx::new("apk");
    let a = made_apk(&f.p("src"), "game.apk", "com.test.game", "2021.3.4f1");
    let o = apkmade::obb(&f.p("src/game.main.obb"), BID);
    let g = apk_port(&f, &a, &[&a, &o], hosts, "", false);
    put(&g.join("gamedata/._game.apk"), "AppleDouble"); // (macOS's, on exFAT: ignored)
    fs::create_dir_all(g.join("gamedata/more")).unwrap();
    fs::rename(g.join("gamedata/game.main.obb"), g.join("gamedata/more/game.main.obb")).unwrap();
    let log = f.p("pm.log");
    let path = f.sync_spy(None);
    let r = apk_setup(&f, &g, &[("PATH", &path), ("STUB_LOG", &log.to_string_lossy())]);
    assert_eq!(r.code, 0, "{}", r.all());
    // the game is marked set up, the card synced before and after, before the setup's files go
    let seen = syncs(&read(&log));
    let i = seen.iter().position(|s| s == "ready setup work").unwrap_or_else(|| panic!("{seen:?}"));
    assert_eq!(seen[i - 1], "setup work", "{seen:?}");
    assert_eq!(seen.last().unwrap(), "ready", "{seen:?}");
    assert!(seen.iter().all(|x| x.split(' ').any(|w| w == "setup") || x == "ready"), "{seen:?}");
    assert!(r.stdout.contains("the players this system runs, in this order: arm64"), "{}", r.stdout);
    assert_eq!(listdir(&g.join("lib")), ["libil2cpp.so", "libmain.so", "libunity.so"]);
    assert_eq!(fs::read(g.join("lib/libunity.so")).unwrap(), b"\x7fELFarm64-v8a");
    for x in ["base.apk", "main.7.com.test.game.obb", ".setup_done", "answers.env"] {
        assert!(g.join(x).is_file(), "{x}");
    }
    let info = read(&g.join("android_package.json"));
    for x in ["\"abi\": \"arm64-v8a\"", "\"source\": \"apk\"", "\"main_obb\": \"main.7.com.test.game.obb\""] {
        assert!(info.contains(x), "{x}: {info}");
    }
    assert_eq!(listdir(&g.join("gamedata")), ["._game.apk", "PUT_GAME_FILES_HERE.txt"]);
    assert!(!g.join("setup").exists());
    assert!(read(&g.join("patchlog.txt")).contains("setup finished"));
    // the setup again (the port installed anew, the marker deleted): the installed APK and OBB
    fs::remove_file(g.join(".setup_done")).unwrap();
    apk_port(&f, &a, &[], hosts, "", true);
    let r = apk_setup(&f, &g, &[]);
    assert_eq!(r.code, 0, "{}", r.all());
    assert!(r.stdout.contains("no package in gamedata/: the installed one again"), "{}", r.stdout);
    assert!(g.join("main.7.com.test.game.obb").is_file());
    assert!(g.join("base.apk").is_file());
}

#[test]
fn apk_setup_checks() {
    if !aarch64_linux() {
        eprintln!("apk_setup_checks: skipped (setup/pmu is this build of pmu: an aarch64 Linux is tested)");
        return;
    }
    let f = Fx::new("apkchecks");
    let a = made_apk(&f.p("src"), "game.apk", "com.test.game", "2021.3.4f1");
    // another game's package; another Unity version; nothing in gamedata/
    let other = made_apk(&f.p("other"), "other.apk", "com.other.game", "2021.3.4f1");
    let both = ["ua_host", "ua_host64"];
    let g = apk_port(&f, &a, &[&other], &both, "", false);
    let r = apk_setup(&f, &g, &[]);
    assert!(r.stdout.contains("PATCH_FAIL_MSG:This port is made for My Game (com.test.game); your files are com.other.game"), "{}", r.all());
    let g = apk_port(&f, &a, &[&a], &both, r#"{"port": {"title": "My Game"}, "game": {"unity_versions": ["2021.3.5f1"]}}"#, false);
    let r = apk_setup(&f, &g, &[]);
    assert!(r.stdout.contains("your copy was built with Unity 2021.3.4f1"), "{}", r.all());
    let g = apk_port(&f, &a, &[], &both, "", false);
    let r = apk_setup(&f, &g, &[]);
    assert!(r.stdout.contains("PATCH_FAIL_MSG:Put the Android build of My Game"), "{}", r.all());
    // refused by pmu apk, with its reason
    let g = apk_port(&f, &a, &[], &both, "", false);
    put(&g.join("gamedata/game.apk"), "not a zip");
    let r = apk_setup(&f, &g, &[]);
    assert!(r.stdout.contains("PATCH_FAIL_MSG:The game's files cannot run here:"), "{}", r.all());
    assert!(r.stdout.contains("not a zip archive"), "{}", r.all());
    // the 32-bit player only, on a system without a 32-bit userland: said before anything else
    if !armhf_userland() {
        let g = apk_port(&f, &a, &[&a], &["ua_host"], "", false);
        let r = apk_setup(&f, &g, &[]);
        assert!(r.stdout.contains("needs a 32-bit (armhf) userland"), "{}", r.all());
        assert!(!r.stdout.contains("unpacking the setup tools"));
    }
}
