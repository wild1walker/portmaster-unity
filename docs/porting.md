# Porting a Unity game

A PortMaster port of a Unity game made with this repository is a launcher and a setup: on the
first launch the setup fetches Unity's Android player for the game's exact version, converts the
player's own copy of the game on the handheld and packs it for the player; every launch after
that runs it with `ua_host`. Nothing of the game or of Unity is in the port. `pmunity port` writes
all of that from a short settings file, so a port needs no scripts of its own:

```sh
cd convert
python -m pmunity port check <game>                      # what the game is, what it needs, the route
python -m pmunity port new mygame --data <game>          # -> mygame/ with port.toml and the port
$EDITOR mygame/port.toml                                 # title, pointer mode, keys, questions, ...
python -m pmunity port new mygame --data <game>          # regenerate from the edited port.toml
mygame/package.sh                                        # -> mygame/out/mygame.zip, the release
PM_7ZZ=<PortMaster's 7zzs.aarch64> ../tools/port/rehearse.sh mygame/out/mygame.zip <game> work/mygame 300
```

`<game>` is the folder of a desktop build as you have it (the one with the executable and
`<Game>_Data`), a macOS `<Game>.app`, or the data folder itself. Nothing in it is run or changed;
run the commands with `python -I` (and `sys.path` pointing at `convert/`) when you do not trust the
game's files. Cuphead's port ([portmaster-cuphead](https://github.com/wild1walker/portmaster-cuphead))
was written by hand before these tools; it shows what a game may need beyond them (its own code
patches and a helper assembly).

## 1. port check

```
python -m pmunity port check <game> [--json] [--asmpatch CMD] [--no-scan]
```

reads the game and prints:

| what | from |
|---|---|
| Unity version, inside the converter's range (5.6 - 6000.6) or not | the data's serialized files |
| scripting backend (Mono: `Managed/*.dll`; IL2CPP: `GameAssembly`, `il2cpp_data`), .NET 3.5 or 4.x runtime | the build's files |
| platform build (Linux, macOS, Windows; the executable's architecture), compressed (`data.unity3d`) or not | the executable's header, the data folder |
| graphics APIs, and so the shaders the converter can translate (OpenGL Core, Direct3D 11) | `BuildSettings` |
| input stacks: Rewired (source or DLL edition), InControl, the Input System package, the legacy Input calls by kind, `OnMouse*` handlers, UI pointer handlers, code that selects UI elements; the UI input modules the scenes use; the verdict (pointer-driven menus or not) | the game's assemblies' metadata (read in Python: names only, no code is loaded), the scenes' scripts, and `asmpatch --survey` when asmpatch is built (`--asmpatch` or `$PMUNITY_ASMPATCH`, else `tools/asmpatch/out`) |
| text input (InputField, TextMeshPro InputField, TouchScreenKeyboard, `Input.inputString`) | assemblies, scenes |
| video (VideoPlayers, clips and their codecs; MovieTexture) | scenes, `.resource` headers |
| native plugins, P/Invoke modules, services (Steamworks, GOG Galaxy, FMOD Studio, Wwise, ...) | `Plugins/`, assemblies |
| file access: reads of StreamingAssets that reach System.IO (then `android_data.streaming_assets`), and of `dataPath`, `persistentDataPath`, the working directory | `asmpatch --survey`/`--list` (without asmpatch: assemblies that reference both the getter and System.IO, a hint) |
| texture formats and sizes (main data) | the serialized files |
| the routes that fit, the recommended one, and the port config values it suggests | all of the above |

The exit status is 0 when a route fits. `--json` gives the whole report (`port new` uses the same).

## 2. Routes

| route | when | status |
|---|---|---|
| `android` | a Mono build of Unity 5.6 - 6000.6 with OpenGL Core shaders (macOS, Linux) or Direct3D 11 (Windows: `shaders.from = ["d3d11"]`, needs `pmhlslcc`), on a handheld with a 32-bit (armhf) userland: Unity's ARMv7 Android player run natively by `ua_host` | templates in `convert/pmunity/port_template/android/` |
| `il2cpp-arm64` | a Mono build on a 64-bit-only system: the game compiled with IL2CPP into an arm64 Android player | `pmunity il2cpp` + `android-data --il2cpp` on the porter's PC/Mac (docs/il2cpp.md; 2018.4-2020.3 checked); no port template yet |
| `box64` | a Linux x86-64 build (any backend, any version): the game's own player under box64 | templates in `convert/pmunity/port_template/box64/`, hooks in `route.py`; `tools/box64/build.sh` before `package` ([box64-route.md](box64-route.md)) |
| `apk` | an Android build of the game | coming (`pmunity apk`) |

Windows builds (Direct3D 11 shaders) take the `android` route with `shaders.from = ["d3d11"]` and
`pmhlslcc` (`tools/hlslcc/build.sh`; docs/shaders.md, "Windows builds"). A Windows build of Unity
2021.2+ also needs the Unix class libraries, because its own are Windows-only: `python -m
pmunity.winbcl <Game>_Data <dir>` fetches them from Unity (about 5 MB) and prints the
`--managed-file` options for `android-data` (in a port: `android_data.args`).

Native plugins: `python -m pmunity plugins <Data>` lists the libraries the game's code P/Invokes
and what the port gives each: built-in stubs answering as a PC without the service (Steam,
Discord, EOS), or ARM libraries of the game's exact version that the porter supplies (FMOD, Wwise;
[plugins.md](plugins.md)). android-data repeats it as warnings and writes `plugins.map` for the
runtime; a port config's `plugins` table changes the map.

A route is a folder `convert/pmunity/port_template/<route>/` with a `route.json`:

```json
{
  "title": "what the route is",
  "files": {"@@TITLE@@.sh": "launcher.sh", "@@PORTDIR@@/setup/patchscript": "patchscript", "...": "..."},
  "package": ["ua_host", "pmunity", "pybundle", "asmpatch?"]
}
```

`files` maps each output path of the port folder to a template, looked up in the route's folder
and then in `port_template/common/` (the READMEs, the gamedata note, the exit-hotkey `.gptk`,
`package.sh`). Templates are text with `@@NAME@@` values (`PORTDIR`, `TITLE`, `ZIP`, `LAUNCHER`,
`SETUP_TIME`, `GAME_WHAT`, `ROUTE`, `ROUTE_TEXT`, `UNITY_VERSIONS`, `SIZE_GB`, `PORTER`; an unknown
name is an error). Everything else a launcher or setup needs comes from files `port new` writes
next to them: `setup/setup.env` (shell assignments), `setup/convert.json`, `port.env`,
`setup/questions.lua`. `package` names the parts the release zip gets (`asmpatch?`: only when the
port uses it); a part the packager does not know comes from a hook module (below). The box64
route can bring its own launcher and setup this way and share the common files.

## 3. port new

```
python -m pmunity port new <name> --data <game> [--config port.toml|.json] [--out DIR] [--route R]
                           [--title T] [--player PKG] [--force]
```

checks the game and writes `<out>` (default `./<name>`):

| file | what |
|---|---|
| `port.toml` | the port's settings (written once, with the values the check found; yours to edit). Run `port new` again to regenerate the rest from it (it is used when it is there, `--config` names another) |
| `port.build.json` | the resolved settings, the route and the check's findings (what `package` reads) |
| `<Title>.sh` | the launcher (PortMaster conventions: `control.txt`, `get_controls`, `$GPTOKEYB`, `$ESUDO`, `pm_message`, `pm_finish`; first launch: PortMaster's patcher runs the setup) |
| `<name>/` | the folder installed as `ports/<name>/`: `setup/patchscript`, `setup/setup.env`, `setup/convert.json`, `setup/questions.lua` and `setup.txt` (with questions), `port.env`, `port.json`, `README.md`, `gamedata/PUT_GAME_FILES_HERE.txt`, `<name>.gptk` |
| `README.md`, `package.sh` | the port's own README (PortMaster's layout: Notes, Controls) and its release build |

`--player PKG` (a folder from `pmunity player fetch <version> PKG`) puts the installer's URL and
the checksums of `libmain.so` and `libunity.so` into the setup, which then checks what it fetches.

### port.toml

| key | default | what |
|---|---|---|
| `port.name` | `<name>` | folder in `ports/`, the zip's name (lowercase) |
| `port.title` | the game's product name | menu entry; the launcher is `<title>.sh` |
| `port.route` | the check's recommendation | a route with templates |
| `port.porter`, `desc`, `inst`, `genres`, `availability`, `store`, `exp` | `inst` from `game.what` | `port.json` |
| `port.ready_marker` | `.setup_done` | the file that marks the setup done (a port moving onto these tools keeps its old one, so installed copies are not set up again) |
| `port.setup_time` | from the size and steps | the patcher's estimate |
| `game.data` | the data folder's name | what the setup looks for in `gamedata/` (a glob; then any Unity data folder) |
| `game.build` | the check's | `linux`, `macos` or `windows`: what players provide |
| `game.unity_versions` | the game's | the versions the setup accepts (another one: the setup stops and says so); empty: any the converter supports |
| `game.what` | from the above | how READMEs and messages name what to put in `gamedata/` |
| `game.size_mb` | the build's | the setup's free-space checks (about three times it) |
| `game.setup_space_mb` | 0 | the free space the setup asks for instead, when the game is known to need more or less |
| `player.backend`, `player.abi` | `mono`, `armeabi-v7a` | the player package `pmunity player fetch` gets |
| `player.url`, `player.sha256` | from `--player` | the installer (else looked up on the handheld through Unity's release API), checksums |
| `convert` | `steps = ["shaders", "settings", "audio"]`, `settings.graphics_apis = [11]` | `pmunity convert`'s config: each step's `DEFAULTS` (`textures.py`, `settings.py`, `audio.py`, `shaders/step.py`); `{setup}` in a value is the setup folder on the handheld |
| `android_data.asmpatch` | `auto` when the check finds Rewired, else `off` | `auto`: tools/asmpatch runs on the game's assemblies (each patch changes only what it finds; the package then ships asmpatch); a list: only those patches |
| `android_data.streaming_assets` | `obb`; the check suggests `both` (`loose` above 256 MB) when reads of StreamingAssets reach System.IO | `obb`: StreamingAssets in the OBB, read by Unity's APIs; `both`: also beside the APK as `Game_Data/StreamingAssets/`, and asmpatch's streaming-assets patch gives the reads that reach System.IO that folder; `loose`: only there, every read gets it (docs/android-data.md, section 6). Not `obb` ships asmpatch (with `asmpatch = "off"` it runs only that patch) |
| `android_data.data_path` | false | true: the reads of `Application.dataPath` that reach System.IO get `Game_Data/` beside the APK, with the data folder's own files (asmpatch's data-path patch; docs/android-data.md, section 6); the check notes such reads |
| `android_data.video_shader`, `swappy`, `name`, `package`, `args` | on, `off`, from the title, `com.pmunity.<name>` | `pmunity android-data` options; `args` = more of them. Keep `package` from release to release: it names the PlayerPrefs file |
| `runtime` | `UA_POINTER = "1"` for pointer-driven menus; `UA_PAD_NAME` for Rewired and InControl (the name of one of its Android profiles); `UA_OSK = "done"` where a text field does not matter | ua_host's environment (docs/runtime.md, docs/input.md), written to `port.env`; a variable set before the launcher runs wins |
| `[[questions]]` | none | asked by PortMaster's patcher before the setup: `id`, `question`, `options = [[value, label], ...]`, `default`; the answer goes to `convert_set` (`pmunity convert --set <key>=<answer>`) or to `env` (the game's environment, `answers.env`). The first answer is kept for later runs; `setup.txt` (`<id>=<value>`) is used when PortMaster asks nothing |
| `files.plugins`, `files.managed`, `files.assembly_patches`, `files.hooks`, `files.extra` | none | the porter's own files (paths relative to port.toml), copied into the port: converter plugins (`--plugin`), assemblies shipped into the game (`--managed-file`), `{name, command, files}` assembly patches (`--assembly-patch`, `{setup}` = the setup folder), shell hooks sourced by the setup at `game`, `pre_convert`, `post_convert`, `post_pack`, `post_install` (a `game` hook sets `D`, the data folder), extra files for the port folder |
| `build.commands`, `build.outputs`, `build.licenses` | none | for a port with its own compiled code: shell commands `port package` runs in the port folder first (with `PMUNITY`, `PORT_SRC` and `BUILD`, a scratch folder, in the environment), the files they make put into the port folder (`{"setup/files/codepatch" = "{build}/codepatch"}`), and licences for `licenses/` |

Keep the game's look and play: options that change the picture or the game play stay off unless
a player asks for them (a question with the vanilla choice as its default). The `textures` step
(ASTC instead of BC/DXT: less memory, slightly different pixels, a much longer setup on the
handheld) is such an option; the check says when a game has enough BC/DXT textures for it to
matter.

## 4. The setup on the handheld

`setup/patchscript`, run by PortMaster's patcher (log: `ports/<name>/patchlog.txt`; a failure is
a `PATCH_FAIL_MSG:` line the patcher shows):

1. the setup's Python (`python.tar.gz`: CPython with UnityPy and the image libraries, aarch64);
2. the game: the data folder in `gamedata/` (any depth), else one from an archive there (`.zip`,
   `.7z`, `.rar`, `.tar.*`, with PortMaster's 7-Zip), else macOS installers (`.pkg`, `pmunity
   macpkg`), else the port's `game` hook; a compressed build is unpacked (`pmunity unpack`); its
   Unity version must be one of `game.unity_versions`;
3. Unity's Android player of that version: `pmunity player fetch` (about 10 MB of the installer
   with HTTP range requests); from `UnitySetup-Android-Support-for-Editor-<version>.exe` when the
   player put it in `gamedata/`; if Unity's servers cannot be read that way, the whole installer
   with curl or wget; checksums when the port has them;
4. `pmunity convert` with `setup/convert.json`, the plugins and the answers;
5. `pmunity android-data --consume-src --pack all`, with asmpatch, the managed files and assembly
   patches, and `android_data.args`;
6. installing: the player's libraries into `lib/` with only the Mono runtime the game needs (with
   both, libunity takes the .NET 4.x one and a .NET 3.5 game aborts), the APK, OBB and Mono's
   config into the port folder; the game's copy in `gamedata/` (used up by step 5), the tools and
   the work folder are removed; `.setup_done` marks it done.

Each step leaves a marker in `gamedata/.setup/`: an interrupted setup (battery, power button)
continues where it stopped on the next launch. Free space: about three times the game while it
runs. The setup needs an aarch64 system for its tools and the game a 32-bit (armhf) userland
(the launcher checks for `/lib/ld-linux-armhf.so.3` first).

## 5. package

```
python -m pmunity port package <port folder> [--out DIR] [--runtime DIR] [--ua-host FILE]
                               [--media-licenses DIR] [--pybundle FILE] [--asmpatch-bin FILE] [--no-build]
```

(or the port's `package.sh`) builds `<out>/<name>.zip` (default `<port>/out/`): the launcher and
the port folder, `ua_host` (`runtime/host/out/`, built with `runtime/build.sh` when missing), the
converter (`convert/pmunity`), the setup's Python (`--pybundle`, else
`tools/port/build_pybundle.sh` builds it: python-build-standalone's CPython 3.12 and the
converter's wheels for aarch64), asmpatch for linux-arm64 when the port uses it
(`tools/asmpatch/build.sh`), and `licenses/`: portmaster-unity's licence and THIRD_PARTY.md,
libhybris (Apache 2.0, in ua_host), the video decoders' licences (`runtime/media/out/licenses`),
the setup Python's and its packages' licences, and for asmpatch the .NET runtime's licence and
third-party notices and Mono.Cecil's. `--runtime` points at another checkout whose runtime and
asmpatch are built. Zip entries keep their Unix modes and get a fixed date.

## 6. Rehearsal

`tools/port/rehearse.sh <zip> <game folder or archive> <work dir> [emu seconds] [input script]`
unpacks the release into `<work>/roms/ports` as PortMaster would, copies the game into its
`gamedata/` and starts the port's own launcher in an aarch64 container that looks like a handheld
(busybox first on PATH, bash, curl, 1 GB of RAM, the network on), with a stand-in for PortMaster
(`control.txt`, `utils/patcher.txt`, PortMaster's own `7zzs.aarch64` from its release: `PM_7ZZ`).
`ANSWERS="ID=value ..."` answers the port's questions. With `[emu seconds]` the installed game then
runs in the emulated armv7 rig (`tools/emu`, QEMU and Mesa's llvmpipe: slow, about 1 fps, but the
same frames and game logic) with the port's `port.env` and `answers.env`, and four screenshots
go to `<work>/shots.png`; `UA_INPUT_SCRIPT`-style input (`"60:ptr=0.5/0.5 70:a~1.5"`) drives it.
`EMU_ONLY=1` reruns only the game. The rig decodes ASTC in software and keeps the result, so a
game converted with the textures step may run out of the rig's memory where the handheld's GPU
would not.

## 7. On a device

The rehearsal does not cover what only a handheld has. Check, with the release zip unpacked into
`ports/`:

- the setup through PortMaster's real patcher: the progress screen, the questions, a failure
  message (rename the game's folder to see one), an interrupted setup (power off during the
  conversion) continuing on the next launch;
- the download over the handheld's Wi-Fi (Python's TLS and the system's CA certificates; the
  setup sets `SSL_CERT_FILE` when it finds a bundle);
- the time and the peak memory of the setup on the device's CPU (the textures step is the slow
  part: ASTC encoding);
- the game: the pad (the mapping from `get_controls`; `UA_PAD_NAME` for Rewired), pointer mode and
  `UA_KEYMAP` where the menus want a mouse or keys, sound (ALSA, else `ANDROID_SLES_OUTPUT=pipe`
  when a sound server holds the card), video, saves (`ports/<name>/data/`: `external/` is
  `persistentDataPath`, `shared_prefs/` PlayerPrefs; they survive updates; moving them to and from
  a PC: docs/android-data.md, "Saves", and `pmunity prefs`), the exit hotkey, the frame rate
  (`UA_FPS=5` in `log.txt`);
- the display path (`UA_DISPLAY`: Mali fbdev or SDL; `[display]` in `log.txt`).

## 8. By hand

The same steps without the setup, on a PC:

1. **Check** the version (`python -m pmunity version <Data>`), the backend and the graphics APIs
   (`port check` says all of it). The runtime knows the Android players of 5.6 - 6000.6; for
   others `tools/re/elfimports.py` lists a player's imports (compare with
   `runtime/android/README.md`) and `FJ_DUMP=1` lists the Java classes and methods Unity asked for
   and did not find. The handheld has little RAM (1 GB with the system): the textures step can
   also scale textures down (`textures.scale`).
2. **Convert**: `python -m pmunity convert <Data> [<out>] --config game.json` with a config like
   the `convert` table above (`{"steps": ["shaders", "settings", "audio"], "settings":
   {"graphics_apis": [11]}}`); a game's own steps go in a plugin (`--plugin steps.py` with
   `STEPS = {"name": function(job)}`). Check the shaders on the device:
   `python -m pmunity shaders-export <converted Data> glsl/`, then `runtime/host/out/shadercheck
   glsl` there (docs/shaders.md).
3. **Package**: `python -m pmunity player fetch <version> <pkg>`, then
   `python -m pmunity android-data --src <converted Data> --pkg <pkg> --out android --pack all
   [--asmpatch <asmpatch>] [--mono-etc <Game>.app/Contents/Mono/etc]` (docs/android-data.md).
4. **Run**: on the handheld, in one folder: `ua_host`, `lib/` with the player's libraries
   (`Variations/mono/Release/Libs/armeabi-v7a/*.so` and, from `MonoLibs/`, the Mono runtime the
   game needs) and the contents of `android/zip/`; then `tools/device/run.sh first.log 60` and
   read the log (`I/Unity`, `[host]`, `UA_FPS=5`). In the emulated rig: `tools/emu/emu.sh`.

Typical first problems:

| symptom | look at |
|---|---|
| exits during load with an unresolved symbol | the loader's `[so]` lines; add the function to `runtime/loader/hybris_compat.c` or an android shim |
| Java class or method missing | `FJ_DUMP=1` / `FJ_TRACE=1`; implement it in `runtime/jni/up_android.c` |
| black screen, shaders failing | `shadercheck`, `UA_SHADER_DUMP`, Unity's `GLSL` errors in the log |
| no pad | `SDL_GAMECONTROLLERCONFIG` (PortMaster's `get_controls`), `UA_PAD_NAME` (Rewired and InControl match Android pads by name; the survey lists InControl's names); `asmpatch --survey <Managed>` names the game's input stacks, `--asmpatch` (Rewired: `rewired-android`; the Input System package: `inputsystem-android`) and docs/input.md say what each needs |
| a name or text field waits for typing | nothing: the on-screen keyboard opens when the game asks for one; `UA_OSK=done` answers it at once (docs/input.md) |
| menus need a mouse, controls need keys | pointer mode (`UA_POINTER=1`) and `UA_KEYMAP` (docs/input.md) |
| no sound | `ANDROID_SLES_OUTPUT=pipe` when a sound server holds the card |
| hitches | `UA_HITCH_TRACE=50` + `tools/analysis/hitchsum.py`; `UA_GC_HEAP` for Mono's collector |

## 9. What may need a game's own code

Desktop builds sometimes assume a desktop: a store client (GOG Galaxy, Steamworks) the game waits
for, DLC found through the store, input set up for keyboards and desktop pads, a resolution or
aspect that does not fit the screen. Generic fixes are in `tools/asmpatch` (`android_data.asmpatch`:
e.g. Rewired on its Android platform); a game's own go in its port: patches to its assemblies
(`files.assembly_patches`, run on the original assembly at packaging time) and a helper assembly
(`files.managed`), as portmaster-cuphead does with Mono.Cecil. Changes that alter the picture or
the game play belong behind an option, off by default.

## 10. Extending `pmunity port`

Other parts of pmunity add to the check, the port and the release without editing `port.py`: a
module `pmunity.<name>` with a name in `port.HOOK_MODULES` (`route`, `box64`, `il2cpp`, `apk`,
`services`, `plugins`, `inputtext`, `osk`, `windows`; more in `$PMUNITY_PORT_HOOKS`) may define

| function | called with | may |
|---|---|---|
| `port_check(report)` | the check's report (a dict: `build`, `backend`, `unity_version`, `code`, `data_survey`, `plugins`, `routes`, ...) | append to `report['notes']`, change or add `report['routes']` entries (`{route, ok, why, needs, available}`), put port config values in `report['suggest']` |
| `port_new(ctx)` | the port being written: `ctx.cfg`, `ctx.out`, `ctx.route`, `ctx.write(rel, data, mode)` | add files (e.g. a plugin map, an on-screen keyboard's settings) |
| `port_package(ctx)` | the release being assembled: `ctx.cfg`, `ctx.port_dir`, `ctx.runtime`, `ctx.add(src, rel, mode)`, `ctx.license(name, path=, text=)` | add binaries and their licences (e.g. box64, pmhlslcc, an ARM plugin build) |

A new route is a template folder (section 2); `port new --route <name>` uses it.
