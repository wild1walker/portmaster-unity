# Porting a Unity game

A PortMaster port of a Unity game made with this repository is a launcher, a setup and one or two
**donors**: small games of the port's own built with the game's exact Unity version, whose APKs
carry Unity's Android player. On the first launch the setup unpacks the player from the donors,
converts the player's own copy of the game on the handheld and packs it for the player; every
launch after that runs it with `ua_host`. Nothing of the game is in the port, and of Unity only
the donors. **The handheld never downloads anything: the porter builds the donors (section 5) and
the release ships them.** `pmunity port` writes all of that from a short settings file, so a port
needs no scripts of its own:

```sh
cd convert
python -m pmunity port check <game>                      # what the game is, what it needs, the route, the donors
python -m pmunity port new mygame --data <game>          # -> mygame/ with port.toml and the port
$EDITOR mygame/port.toml                                 # title, pointer mode, keys, questions, ...
python -m pmunity port new mygame --data <game>          # regenerate; names the donors to build
../tools/donor/get-editor.sh <version> editors/<version> # (a Mac) the game's Unity editor + Android Build Support
../tools/donor/build.sh editors/<version>/Unity.app MonoArmv7 donors   # each donor port new named (section 5)
DONORS=donors mygame/package.sh                          # -> mygame/out/mygame.zip, the release (with the donors)
PM_7ZZ=<PortMaster's 7zzs.aarch64> ../tools/port/rehearse.sh mygame/out/mygame.zip <game> work/mygame 300
python -m pmunity port package mygame --donors donors --submission   # + mygame/out/submission/mygame/ for PortMaster's repository
```

`<game>` is the folder of a desktop build as you have it (the one with the executable and
`<Game>_Data`), a macOS `<Game>.app`, or the data folder itself; or the game's own Android build
(an `.apk`, with its `.obb` files beside it, an `.xapk`, `.apks` or `.aab`, or a folder of them: the
`apk` route, section 2). Nothing in it is run or changed;
run the commands with `python -I` (and `sys.path` pointing at `convert/`) when you do not trust the
game's files. Cuphead's port ([portmaster-cuphead](https://github.com/wild1walker/portmaster-cuphead))
was written by hand before these tools; it shows what a game may need beyond them (its own code
patches and a helper assembly).

## Before you start

- **The computer:** Docker, Python 3.11+ (`pip install -r convert/requirements.txt`; `port.toml`
  is read with `tomllib`), `runtime/build.sh` once, and the .NET 8 SDK on the computer itself (no
  Docker image has it; `dotnet` on PATH, `~/.dotnet/dotnet`, or `DOTNET=<its dotnet>`):
  `port check` builds asmpatch with it when it is missing (`tools/asmpatch/build.sh`), for the
  survey of the game's code that finds its pad bindings (what a handheld without sticks needs:
  `UA_DPAD_STICK`, `UA_DPAD_SWAP`, the `[controls]` draft), InControl's pad names and the files it
  reads. Without it the check says what it left out, and the port lacks those. For the rehearsal
  (section 7), PortMaster's own 7-Zip: `PortMaster/7zzs.aarch64` inside `PortMaster.zip` from
  [PortMaster-GUI's releases](https://github.com/PortsMaster/PortMaster-GUI/releases).
- **The game as a folder:** GOG's Linux installer (`.sh`, a shell script with a zip appended) is
  never run: `7zz x -tzip <installer>.sh -o<dir>` (or `unzip -q <installer>.sh -d <dir>`, whose
  exit status 1, "extra bytes at beginning", is harmless but stops a `set -e` script), then the
  game is `<dir>/data/noarch/game`. GOG's macOS `.pkg`: `python -m pmunity macpkg <dir> <installer>.pkg`
  (the app lands under `<dir>` at the package's install location). Steam: the game's folder (the Linux depot where there is one). Keep the installer itself for
  the handheld and the rehearsal: the setup takes it as it is.
- **The port's own repository:** a GitHub repository per port holds `port new`'s output and
  nothing of the game. Clone it (a new one holds a LICENSE and a README: `port new` writes into
  such a clone without `--force`), run `port new <name> --data <game> --out <repo>`, edit
  `port.toml`, run `port new` again, commit. `port new` writes a `.gitignore` once (the build
  output `out/`, game and Unity file types, the rig's screenshots); `package.sh` there builds the
  release, with `PMUNITY` pointing at a portmaster-unity checkout (default: one beside the
  repository). A port's own code (an assembly patch, a hook) goes in the repository too, with its
  build in `[build]` (section 10).
- **The pad bindings:** a game's controls are in its input stack's defaults (Rewired's XML,
  InControl's or the Input System's actions in the assemblies); `port check`'s input survey says
  which stack. On a handheld without sticks a game that moves only with the left stick needs the
  d-pad as that stick (docs/input.md).

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
| input stacks: Rewired (source or DLL edition), InControl, the Input System package, the legacy Input calls by kind, `OnMouse*` handlers, UI pointer handlers, code that selects UI elements; the UI input modules the scenes use; the verdict (pointer-driven menus or not); InControl's default pad bindings, every one by control (the `[controls]` draft), the actions only the left stick drives (`UA_DPAD_STICK`), the d-pad's own (a `UA_DPAD_SWAP` button: one the game binds nothing to, else one whose actions other buttons also do), those only the right stick drives (a handheld without one cannot do them) | the game's assemblies' metadata (read in Python: names only, no code is loaded), the scenes' scripts, and `asmpatch --survey` (`--asmpatch` or `$PMUNITY_ASMPATCH`, else `tools/asmpatch/out`, which the check builds with the .NET 8 SDK when it is missing; without either, a warning says what was left out; `--asmpatch none`: no survey) |
| text input (InputField, TextMeshPro InputField, TouchScreenKeyboard, `Input.inputString`) | assemblies, scenes |
| video (VideoPlayers, clips and their codecs; MovieTexture) | scenes, `.resource` headers |
| native plugins, P/Invoke modules, services (Steamworks, GOG Galaxy, FMOD Studio, Wwise, ...) | `Plugins/`, assemblies |
| file access: reads of StreamingAssets that reach System.IO (then `android_data.streaming_assets`), and of `dataPath`, `persistentDataPath`, the working directory | `asmpatch --survey`/`--list` (without asmpatch: assemblies that reference both the getter and System.IO, a hint) |
| texture formats and sizes (main data); a warning above 256 MB of uncompressed ones (RGBA32, RGB24: what a 1 GB handheld must hold as they are; the textures step makes them ASTC) | the serialized files |
| StreamingAssets bundles: how many, the scene bundles (several serialized files), their shaders by the programs they have, texture formats and sizes. Warnings for what the converter will not handle: files that look like Unity bundles of another kind (UnityWeb, UnityRaw, a UnityFS header behind other bytes: a game's own offset or encryption), bundles UnityPy cannot open, shaders in bundles with no programs of the source the converter translates from (they draw magenta); a note for scene bundles (pmu converts them, the Python converter does not) and for bundles without a serialized file | UnityPy, a bundle at a time |
| the routes that fit, the recommended one, and the port config values it suggests (the textures step when the BC/DXT textures of the main data and the bundles together would decode to more than 128 MB) | all of the above |

An Android build (the `apk` route) is read with `pmunity apk`'s dry run instead: package,
versionCode and versionName, Unity version, backend, the ARM ABIs it has (and so which systems run
it), graphics APIs, OBBs, native plugins, the screen orientation, and for a Mono build the same
input survey of its assemblies (`assets/bin/Data/Managed/`): `UA_POINTER = "touch"` for a game
that reads only touches, `"1"` for pointer-driven menus. A build `pmunity apk` refuses (x86 only,
Vulkan only, VR, IL2CPP without its metadata, Unity data in an OBB not given, ...) gets the reason.

The exit status is 0 when a route fits. `--json` gives the whole report (`port new` uses the same).

## 2. Routes

| route | when | status |
|---|---|---|
| `android` | a Mono build of Unity 5.6 - 6000.6 with OpenGL Core shaders (macOS, Linux) or Direct3D 11 (Windows: `shaders.from = ["d3d11"]`, translated on the handheld by the release's `pmhlslcc`; a 2021.2+ Windows build's class libraries fetched by the setup, `pmunity winbcl`), on a handheld with a 32-bit (armhf) userland: Unity's ARMv7 Android player run natively by `ua_host` | templates in `convert/pmunity/port_template/android/` |
| `il2cpp-arm64` | a Mono build of Unity 2017.4+ on a 64-bit-only system (no armhf userland): the game's code compiled with IL2CPP into Unity's arm64 IL2CPP Android player, run natively by the 64-bit `ua_host` | templates in `convert/pmunity/port_template/il2cpp-arm64/`, hooks in `il2cpp.py`: the setup compiles the code on the handheld (`pmunity il2cpp --on-device`: one to a few hours on a Cortex-A53, about 1.6 GB free, Wi-Fi), then converts and packs as the android route (`android-data --il2cpp`); by hand on the PC: docs/il2cpp.md. What changes the game's code (asmpatch, `files.managed`, `files.assembly_patches`, the opt-in patches of `streaming_assets` and `data_path`) goes into that IL2CPP build, not into android-data |
| `mono-arm64` | a Mono build of Unity 2017.4+ with the .NET 4.x runtime on a 64-bit userland: the game's own assemblies run by Unity's Mono (built for aarch64 Linux, shipped with the release) inside Unity's arm64 IL2CPP player, whose `libil2cpp.so` is a bridge to it; no IL2CPP compile, the desktop's runtime and class libraries. .NET 3.5 games run on the line's MonoBleedingEdge with Mono's 4.x class libraries built from the same sources (`tools/mono64/build-classlibs.sh`; Cuphead, [mono-arm64.md](mono-arm64.md#a-net-35-game-on-the-4x-class-libraries-cuphead)), not yet through `port new`. Offered by `port check`, not yet recommended over the other routes | templates in `convert/pmunity/port_template/mono-arm64/`, hook `mono64.py`: the android route's setup with the IL2CPP player and its engine modules (`player donor --managed` from an IL2CPP and a Mono donor) and `android-data --mono64`; the release's `setup/mono64/<line>/` (the package part `mono64`: `runtime/mono64/build.sh` and `tools/mono64/build-mono.sh <line>`, Docker; `--mono64-work`) goes into `lib/` ([mono-arm64.md](mono-arm64.md)) |
| `box64` | a Linux x86-64 build (any backend, any version): the game's own player under box64 | templates in `convert/pmunity/port_template/box64/`, hooks in `route.py`; `tools/box64/build.sh` before `package` ([box64-route.md](box64-route.md)) |
| `apk` | the game's own Android build (an APK with its OBBs, split APKs, `.xapk`, `.apks`, `.aab`) of Unity 5.6 - 6000.6 with GLES: the player inside it, run natively by `ua_host`, its armeabi-v7a player where the handheld has a 32-bit userland, its arm64-v8a player (the 64-bit `ua_host64`) elsewhere; a Mono build (armeabi-v7a only) of 2017.4 - 2022.3 (2023.1 - 6000.2: mixed, docs/il2cpp.md) with `player.il2cpp = true` also on 64-bit-only systems, its code compiled there with IL2CPP for Unity's arm64 IL2CPP player | templates in `convert/pmunity/port_template/apk/`, hooks in `apk.py`: the setup runs `pmunity apk` on the player's copy and installs it (no download, no conversion; with `player.il2cpp` on a 64-bit-only system `pmunity apk --il2cpp --on-device`: one to a few hours, Wi-Fi; [apk.md](apk.md), "As a port") |


### Choose the route for speed first

The route sets how fast the game can ever run on a handheld: which engine build (ARMv7 or arm64)
and which code runner (Mono's JIT or IL2CPP's compiled code) run every frame. Changing it late
means redoing the port, so decide it before anything else, from measurements on a handheld in the
game's heaviest scene:

1. **Read what the game's players already know.** Community performance mods and fixes (section
   10, "Community performance mods and fixes") name the hot spots before any profile.
2. **Measure the heaviest scene, not the menu.** Fights, crowds, effects: the Breach of Enter the
   Gungeon ran 59 fps and its first fight 38 with the same settings. Get there early: a save past
   any tutorial (the porter's own, or a community save kept locally, never shipped), the game's
   own quick start, `UA_INPUT_SCRIPT`.
3. **Find what limits the frame** with the handheld's tools ([profiling.md](profiling.md)): `UA_FPS`,
   `UA_GPU_IDLE` (does the GPU wait for work?), `UA_PROF` (the main thread's self time: engine,
   managed code, Mono). Estimates from the emulated rig miss: Gungeon's GPU time was estimated at
   19 ms and measured at 34.
4. **If the GPU limits,** any route will do, and the picture is where the time goes: the game's own
   quality options (gated on RAM, with an option to restore; Gungeon's Low lighting and shaders: 32 ->
   59 fps), `UA_RENDER_SIZE` for a smaller picture scaled by the display engine at no cost (never the
   game's own scale-to-fill, which can draw its effects at the panel's size: Gungeon 48 vs 59 fps),
   ASTC textures, and exact GPU savings ([shaders.md](shaders.md), [quality.md](quality.md)).
5. **If the main thread limits,** the route is the lever. Compare the candidates on the handheld in
   the same scene with the same settings before building the port around one:
   - `android`: the ARMv7 engine and Mono's 32-bit JIT; nothing to compile at setup. With Mono's
     ahead-of-time images made by the player's own libmono (runtime.md, `UA_MONO_AOT` and
     `UA_MONO_MAIN`: Unity's runtime from the donor, GNU as/ld for ARM) the frame costs the same, and
     what the JIT compiles at first use falls: Enter the Gungeon in the emulated rig, 9,948 compiles in
     14.2 s of emulated time against 7,625 cheap ones (wrappers) in 1.6 s. Not yet a setup step.
   - `mono-arm64`: the arm64 engine and Mono's arm64 JIT; nothing to compile at setup.
   - `il2cpp-arm64` (or `apk` with `player.il2cpp`): the game's code compiled ahead of time;
     Hollow Knight's managed code took 3.4 ms a frame against 4.9 ms on Mono's arm64 JIT. Its setup
     compiles on the handheld for one to a few hours and needs about 1.6 GB free.
   A route that needs a compile at setup must say so in the port's README and setup questions.

   Measured on Enter the Gungeon (Unity 2017.4.27f1, .NET 3.5, main-thread-bound in the Keep): the
   main thread's instructions a frame in the emulated rig (QEMU with a per-thread instruction counter,
   the same 400 frames, `UA_INPUT_CLOCK=frames:43` and the data's maximum timestep at 1/43 s), then the
   RG34XX-SP (Knulli, the game's Low lighting and shaders, 480x320 scaled by the display engine):

   | route | rig, M instructions/frame | handheld, arrival room | first fight | memory on 1 GB |
   |---|---|---|---|---|
   | `android`: Mono ARMv7 | 7.93 | 35 - 37 fps | 39.1 - 39.9 fps | lowest available 80 - 92 MB |
   | `mono-arm64` | 7.47 (-6 %) | not run | not run | Mono's heap 352 MB against 248 (64-bit references) |
   | IL2CPP arm64 | 5.69 (-28 %) | 0.2 - 3 fps: swapping | not run | about 200 MB more than the handheld has |
   | IL2CPP ARMv7 (the 32-bit IL2CPP player: `pmunity il2cpp --abi armeabi-v7a`, the `Il2cppArmv7` donor) | 5.97 (-25 %) | 40.5 - 41 fps | 43.0 - 43.2 fps | as Mono's |
   | the same two, the port's own exact patches in both (gungeonpatch, asmpatch's unity-object-null), `UA_FUTEX_SPIN=50 UA_GL_REC=2` | | | Mono 43.6 - 43.9, IL2CPP ARMv7 47.0 - 47.4 fps | IL2CPP 12 MB more free |

   Instruction counts stand for time across ARMv7 and arm64 code here only because the Cortex-A53 is
   in order; the handheld confirmed the order of the routes, not the size of the gain (+9 - 12 % against
   -25 % instructions; the fight: a fixed floor seed and no damage, test-only), and IL2CPP keeps its lead
   on top of the game patches that speed up Mono (+8 %). On a 1 GB handheld the 64-bit routes lose by memory before speed counts. The
   IL2CPP compile for this game: 602 MB of C++, 787 compiles, 6 CPU-minutes on an M-series Mac in a
   1 GB container, an estimated 35 - 50 minutes on the handheld (docs/il2cpp.md, "On the handheld").
6. **Memory decides too.** A 1 GB handheld runs the game with the frontend in the background:
   measure the lowest available memory in the heaviest scene with it running, for each candidate.
7. **The firmware sets the clocks.** Knulli runs the RG34XX-SP's CPU up to 1.5 GHz, muOS up to
   1.8: measure on the firmwares players use, and never change governors or clocks for the port.

Exact runtime switches (`UA_FUTEX_SPIN`, `UA_FAST_CLOCK`, `UA_GL_SAMECLEAR`) come after the route
and gain only where a profile shows their cost (Gungeon: none of the three). Write the measurements
into the port's testing notes, with the route chosen and why.

Windows builds (Direct3D 11 shaders) take the `android` route as they come: players put the game's
folder (`<Game>.exe` beside `<Game>_Data`) or the zip it came in into `gamedata/`, and `port new`
sets the rest up from the check. The shaders: `convert.shaders.from = ["d3d11"]`, and the release
ships `setup/pmhlslcc` (HLSLcc for aarch64, built in Docker by `ARCH=arm64 tools/hlslcc/build.sh`,
with HLSLcc's licence; the `pmhlslcc?` part of route.json), which the setup gives the converter
(`PMHLSLCC`; docs/shaders.md, "Windows builds"). A Windows build of Unity 2021.2+ also needs the
Unix class libraries, because its own are Windows-only: right after the player, the setup runs
`pmunity winbcl --donor <the port's Mono donor>`, which takes the `unityjit-linux` twins from the
donor's own `assets/bin/Data/Managed/` for android-data's `--managed-file` (nothing downloaded). A
donor carries only the class libraries its own build uses: a pong donor of 2017.4 has mscorlib,
System, System.Core, System.Xml, System.Configuration, System.Security, Mono.Security and Mono.Posix;
the game's others (System.Data, System.Xml.Linq, System.Net.Http, System.Runtime.Serialization, ...)
stay its Windows ones, named in `patchlog.txt` (`not_in_donor` in `--json`): untested on the player
where their code reaches Windows' kernel32. A donor that carries them would need code of its own
that uses each (tools/donor's pong references only what Unity's engine does). The
same step gives any build the facades Unity's Android engine modules reference and the build lacks
(a Unity 6 build without `netstandard.dll`: the donor has them, it runs with the same modules), and
names, without faking them, the other assemblies they reference that are not Unity's own; when
nothing is missing it reads nothing. `android_data.winbcl = "off"` leaves the step out. By hand:
`python -m pmunity winbcl <Game>_Data <dir> [--player <pkg>] --donor <donor.apk>` prints the
`--managed-file` options for `android-data` (without `--donor` it reads Unity's Windows editor
installer instead, on the porter's computer). The il2cpp-arm64
route compiles with Unity's own class libraries and needs only `pmhlslcc` (shipped the same way;
not tried with a Windows build yet).

macOS and Linux builds take the `android` route with their OpenGL Core programs. A build made
without OpenGL Core has others: Unity's macOS builds listed OpenGL Core next to Metal up to 2020.1
and Metal alone from 2020.3 on (of the free itch.io macOS builds whose settings were read,
2026-10-22: all 14 games of 2017.3 - 2020.1 have OpenGL Core, 17 of the 18 of 2020.3 - 6000.6 Metal
only, macOS-only games among them), and every HDRP Linux build has Vulkan only. `port check` reads
the build's graphics APIs (for a compressed build too: the settings inside `data.unity3d`) and
sets `convert.shaders.from`: `["metal"]` for a Metal-only build (translated by the converter
itself, `metal.py`; nothing more ships), `["vulkan"]` for a Vulkan-only one, whose release then
ships `setup/pmspirvcross` (SPIRV-Cross for aarch64, built in Docker by `ARCH=arm64
tools/spirvcross/build.sh`, with SPIRV-Cross's and SMOL-V's licences; the `pmspirvcross?` part of
route.json), which the setup gives the converter (`PMSPIRVCROSS`; docs/shaders.md). An HDRP build
is refused for the Android route all the same: HDRP does not run on OpenGL ES.

Native plugins: `python -m pmunity plugins <Data>` lists the libraries the game's code P/Invokes
and what the port gives each: built-in stubs answering as a PC without the service (Steam,
Discord, EOS), a sound engine made of the game's own entry points (Wwise: ua_host's own player plays
the banks the setup converted, `wwise.banks`; the check also reads the version from the desktop
library and the soundbanks), or ARM libraries of the game's exact
version that the porter supplies (FMOD, Wwise for sound; [plugins.md](plugins.md)). android-data repeats it as warnings and writes `plugins.map` for the
runtime; a port config's `plugins` table changes the map.

A route is a folder `convert/pmunity/port_template/<route>/` with a `route.json`:

```json
{
  "title": "what the route is",
  "files": {"@@TITLE@@.sh": "launcher.sh", "@@PORTDIR@@/setup/patchscript": "patchscript", "...": "..."},
  "package": ["ua_host", "pmunity", "pybundle", "asmpatch?"],
  "donors": ["player", "mono/armeabi-v7a"],
  "defaults": {"player": {"backend": "il2cpp", "abi": "arm64-v8a"}}
}
```

`files` maps each output path of the port folder to a template, looked up in the route's folder
and then in `port_template/common/` (the READMEs, the gamedata note, the exit-hotkey `.ini` for gptokeyb2,
`package.sh`). Templates are text with `@@NAME@@` values (`PORTDIR`, `TITLE`, `ZIP`, `LAUNCHER`,
`SETUP_TIME`, `GAME_WHAT`, `ROUTE`, `ROUTE_TEXT`, `UNITY_VERSIONS`, `SIZE_GB`, `PORTER`,
`PLAYER_FROM` (where Unity's player comes from: the port's donors, no Wi-Fi; for il2cpp-arm64 what
its setup downloads), and for
PortMaster's README sections `THANKS`, `CONTROLS` (the controls table), `ROUTE_NEEDS` (what the
route needs of the handheld), `BUILD_STEPS` (the build commands of the route's `package` parts);
an unknown name is an error). Everything else a launcher or setup needs comes from files `port new`
writes next to them: `setup/setup.env` (shell assignments), `setup/convert.json`, `port.env`,
`setup/questions.lua`; `port.json`, `gameinfo.xml` and `testing_thread.txt` are written for every
route by `port new` itself. `package` names the parts the release zip gets (`ua_host_arm64` for the 64-bit host as `ua_host`;
`ua_host64` for it beside a 32-bit `ua_host`; a `?` part only when the port uses it: `asmpatch?`
when it runs asmpatch, `pmhlslcc?` when it translates Direct3D 11 shaders, `pmspirvcross?`
when it translates Vulkan ones, `ua_host?` /
`ua_host64?` for the ABIs in `player.abis`); `donors` the donors its setup reads (section 5):
`"player"` for the port's `player.backend`/`player.abi`, or `"<backend>/<abi>"`, each for every
Unity version of the port; `defaults` are
port.toml values the route sets where port.toml says nothing; `config` lists the port.toml
sections the route uses (port.toml and `setup.env` get only those; default all) and `config_doc`
replaces their explanations; a part the packager does not know comes from a hook module (below).
The box64 route can bring its own launcher and setup this way and share the common files.

## 3. port new

```
python -m pmunity port new <name> --data <game> [--config port.toml|.json] [--out DIR] [--route R]
                           [--title T] [--player PKG] [--force]
```

checks the game and writes `<out>` (default `./<name>`):

| file | what |
|---|---|
| `port.toml` | the port's settings (written once, with the values the check found; yours to edit). Run `port new` again to regenerate the rest from it (it is used when it is there, `--config` names another); it then lists what the check suggests that `port.toml` lacks or sets otherwise (e.g. `UA_DPAD_STICK` once asmpatch is built, `[wwise] banks`): a key it lacks is used all the same, a value it has wins |
| `port.build.json` | the resolved settings, the route and the check's findings (what `package` reads) |
| `<Title>.sh` | the launcher (PortMaster's standard first lines, then `$GPTOKEYB2`, `$ESUDO`, `pm_message`, `pm_platform_helper`, `pm_finish`, the log in `log.txt`; first launch: PortMaster's patcher runs the setup) |
| `<name>/` | the folder installed as `ports/<name>/`: `setup/patchscript`, `setup/setup.env`, `setup/convert.json`, `setup/questions.lua` and `setup.txt` (with questions), `port.env`, `port.json`, `gameinfo.xml` (the frontend's game list entry), `README.md` (the player's), `gamedata/PUT_GAME_FILES_HERE.txt`, `<name>.ini` (gptokeyb2: the exit hotkey) |
| `README.md` | the port's README in PortMaster's layout: `## Notes` (a thank-you to the game's makers, what to put where, what the route needs), `## Controls` (the `[controls]` table), `## Compile` (the commands that build the release from a portmaster-unity checkout) |
| `testing_thread.txt` | the post for PortMaster's Discord testing channel that goes with the test zip: how to install it, the game's link, the instructions, the controls, the firmware and screen checklist (not in the zip) |
| `package.sh` | the release build (`port package`) |

`port new` writes the donors the route needs for each of `game.unity_versions` into `port.toml`
(`player.donors`, as a comment: they follow the route and the versions), `setup/setup.env`
(`DONORS`) and `port.build.json`, and prints them with the command that builds them (section 5).
`--player PKG` (a folder from `pmunity player donor <version> PKG --apk <donor.apk>`) puts the
checksums of its `libmain.so` and `libunity.so` into the setup, which then checks what it unpacks.

### port.toml

| key | default | what |
|---|---|---|
| `port.name` | `<name>` | folder in `ports/`, the zip's name (lowercase) |
| `port.title` | the game's product name | menu entry; the launcher is `<title>.sh` |
| `port.route` | the check's recommendation | a route with templates |
| `port.porter`, `desc`, `inst` | `inst` from `game.what` | `port.json`: the porters' names (as on PortMaster's Discord), the description and the instructions the PortMaster app shows (plain text, one paragraph, no Markdown) |
| `port.desc_md`, `inst_md` | none | Markdown versions for portmaster.games, only where they add something (lists, links) |
| `port.genres` | none | one or more of `action`, `adventure`, `arcade`, `casino/card`, `fps`, `platformer`, `puzzle`, `racing`, `rhythm`, `rpg`, `simulation`, `sports`, `strategy`, `visual novel`, `other` (PortMaster's list; anything else is an error) |
| `port.store` | none | where to get the game: `[{name = "Steam", gameurl = "https://...", developerurl = "https://..."}]` (tables; PortMaster takes no bare links) |
| `port.availability` | `paid` with a store, else `free` | `free` / `paid` (the player provides the game's files, free or bought), `demo`, `full` (for these ports the game is never in the zip) |
| `port.reqs` | none | device capabilities the port needs besides the route's own (the android route adds `armhf`): `!lowres`, `hires`, `2gb`, `4gb`, `power`, `opengl` (PortMaster's `device_info.txt` sets them) |
| `port.exp` | false | true hides a released port unless players show experimental ports; not a "still being tested" flag |
| `port.thanks` | a thank-you to `game.developer` | the README's first sentence: thank the game's makers in your own words, with a link and one thing that makes the game worth playing |
| `port.screenshot`, `port.cover` | `screenshot.png`, `cover.png` | image files (relative to the port folder; PNG or JPEG) `package` ships as `<name>/screenshot.png` and `<name>/cover.png` |
| `port.ready_marker` | `.setup_done` | the file that marks the setup done (a port moving onto these tools keeps its old one, so installed copies are not set up again) |
| `port.setup_time` | from the size and steps | the patcher's estimate |
| `game.data` | the data folder's name; apk route: `*.apk *.xapk *.apks *.aab *.zip` | what the setup looks for in `gamedata/` (a glob; then any Unity data folder); apk route: the packages it takes (globs, any depth), with every `.obb` there |
| `game.build` | the check's | `linux`, `macos`, `windows` or `android`: what players provide |
| `game.package` | apk route: the APK's | the Android package the setup accepts (another one: it stops and says so); empty: any |
| `game.unity_versions` | the game's | the versions the setup accepts (another one: the setup stops and says so); empty: any the converter supports |
| `game.what` | from the above | how READMEs and messages name what to put in `gamedata/` |
| `game.size_mb` | the build's | the setup's free-space checks (about three times it) |
| `game.setup_space_mb` | 0 | the free space the setup asks for instead, when the game is known to need more or less |
| `game.developer`, `publisher`, `release_date` | the developer from the game's PlayerSettings (company name), else none | `gameinfo.xml`; `release_date` as `YYYY-MM-DD` |
| `game.genre`, `description` | the first of `port.genres`, `port.desc` | `gameinfo.xml`'s genre (a display name: `Platformer`, `RPG`) and description (the frontend has room for more than `port.desc`) |
| `game.url` | the first store's `gameurl` | the game's page (the testing thread) |
| `[controls]` | drafted from InControl's default pad bindings (the check's survey), else none; your own table replaces the draft | the game's controls for the READMEs and the testing thread, one row per button in order (`"D-Pad" = "Move"`, `B = "Jump"`), named as the handheld labels them; most PortMaster handhelds have the Nintendo layout (A right, B bottom), and the game sees the bottom button as its gamepad's A. Leave out buttons that do nothing; PortMaster's exit hotkey (`Select + Start`) is added |
| `player.backend`, `player.abi` | `mono`, `armeabi-v7a` | the player the setup unpacks from the port's donors; apk route: the APK's backend, and `abi = "auto"` (the APK's armeabi-v7a player where the handheld has a 32-bit userland, else its arm64-v8a one) or `"arm64-v8a"` (the 64-bit one wherever the APK has it) |
| `player.donors` | the route's for each of `game.unity_versions` (section 5) | the donor APKs the release ships in `donor/`: `port new` names the route's (a comment in port.toml) and adds any listed here; `port package --donors DIR` copies them and stops when one is missing |
| `player.abis` | apk route: the APK's ARM ABIs | the APK's players the port runs, and so the runtimes the release ships: `ua_host` for `armeabi-v7a`, `ua_host64` for `arm64-v8a` |
| `player.il2cpp` | apk route: false | true (a Mono build, armeabi-v7a only, of Unity 2017.4 - 2022.3, or 2023.1 - 6000.2 as a mixed build): where the system has no 32-bit userland, the setup compiles the build's code with Unity's IL2CPP on the handheld for Unity's arm64 IL2CPP player (`pmunity apk --il2cpp --on-device`: one to a few hours, about 1.6 GB free, Wi-Fi); the release ships `ua_host64` too and port.json no longer asks for the 32-bit userland. The check says when a Mono build can take it |
| `player.sha256` | from `--player` | `{"libmain.so" = "<sha256>", ...}`: the setup checks the player it unpacks from the donors (an older port.toml's `player.url` is ignored: the handheld never downloads Unity's player) |
| `convert` | `steps = ["shaders", "settings", "audio", "quality"]`, `settings.graphics_apis = [11]` | `pmunity convert`'s config: each step's `DEFAULTS` (`textures.py`, `settings.py`, `audio.py`, `quality.py`, `shaders/step.py`); `{setup}` in a value is the setup folder on the handheld. `quality` is the handheld quality profile ([quality.md](quality.md)): a cheaper picture where the handheld has 1 GB of RAM, the game play the same; the `PICTURE` question (below) lets the player choose. `video` (pmu only): VP8 clips taller than `max_height` (720) halved until they are not and encoded again (`mode` auto: where MemTotal is at most `ram_mb`, 1024; `cq_level` 10, `cpu_used` 4), each into its old place in the resource file: a 1080p cinematic is more than an A53 decodes in time, 540p a quarter of it and the same on a 720x480 panel ([rust-setup.md](rust-setup.md#video)) |
| `android_data.asmpatch` | `auto` when the check finds Rewired, the Input System package or PlayMaker, else `off` | `auto`: tools/asmpatch runs on the game's assemblies with its default patches, the android ones and the exact optimizations (each changes only what it finds; the package then ships asmpatch); a list: only those patches; `["auto", "<patch>", ...]`: the default ones and those (opt-in ones such as the `reduce` patches) |
| `android_data.streaming_assets` | `obb`; the check suggests `both` (`loose` above 256 MB) when reads of StreamingAssets reach System.IO | `obb`: StreamingAssets in the OBB, read by Unity's APIs; `both`: also beside the APK as `Game_Data/StreamingAssets/`, and asmpatch's streaming-assets patch gives the reads that reach System.IO that folder; `loose`: only there, every read gets it (docs/android-data.md, section 6). Not `obb` ships asmpatch (with `asmpatch = "off"` it runs only that patch); the il2cpp-arm64 route compiles the patch into the game's IL2CPP build (`pmunity il2cpp --streaming-assets`) and packs the same mode |
| `android_data.winbcl` | `auto` | `auto`: after the player, the setup runs `pmunity winbcl --donor`: a Windows 2021.2+ build gets the Unix class libraries of its version that the Mono donor carries, any build the facades the player's engine modules reference that it lacks (e.g. `netstandard.dll`), both from the port's Mono donor (nothing downloaded; nothing is read when nothing is missing); `off`: not run |
| `android_data.data_path` | false | true: the reads of `Application.dataPath` that reach System.IO get `Game_Data/` beside the APK, with the data folder's own files (asmpatch's data-path patch; docs/android-data.md, section 6); the check notes such reads |
| `android_data.video_shader`, `swappy`, `name`, `package`, `args` | on, `off`, from the title, `com.pmunity.<name>` | `pmunity android-data` options; `args` = more of them. Keep `package` from release to release: it names the PlayerPrefs file |
| `runtime` | `UA_POINTER = "1"` for pointer-driven menus; `UA_PAD_NAME` for Rewired and InControl (the name of one of its Android profiles); `UA_OSK = "done"` where a text field does not matter | ua_host's environment (docs/runtime.md, docs/input.md), written to `port.env`; a variable set before the launcher runs wins |
| `wwise.banks` | the check's: the game's Wwise soundbank folder (relative to the data folder) when its banks are of a version `pmu wwise` reads (128, Wwise 2017.2) | the setup converts those banks once into the port's `wwise/` folder (step 5 below), and the launcher points ua_host's own Wwise player at it (`UA_WWISE_DATA`): the game's sound without Wwise's engine ([wwise-player.md](wwise-player.md)); empty: no conversion, the game runs silent on ua_host's built-in Wwise |
| `runtime_lowmem` | none | more of the environment, set only on handhelds with 1 GB of RAM or less (`MemTotal` at most 1024 MB, read by `port.env` at each launch) unless already set: reductions of the picture that leave the game play as it is ([below](#small-handhelds-1-gb)) |
| `[[questions]]` | none | asked by PortMaster's patcher before the setup: `id`, `question`, `options = [[value, label], ...]`, `default`; the answer goes to `convert_set` (`pmunity convert --set <key>=<answer>`) or to `env` (the game's environment, `answers.env`). The first answer is kept for later runs; `setup.txt` (`<id>=<value>`) is used when PortMaster asks nothing |
| `files.plugins`, `files.managed`, `files.assembly_patches`, `files.hooks`, `files.extra` | none | the porter's own files (paths relative to port.toml), copied into the port: converter plugins (`--plugin`: a JSON file of field edits, which the setup's pmu runs, or a Python file: [plugins.md](plugins.md#converter-plugins-a-ports-own-conversion-steps)), assemblies shipped into the game (`--managed-file`), `{name, command, files}` assembly patches (`--assembly-patch`: a command run on the handheld, `{setup}` = the setup folder, `{managed}` = the game's original Managed folder, `{out}` = where it writes `<name>`; section 10), shell hooks sourced by the setup at `game`, `pre_convert`, `post_convert`, `post_pack`, `post_install` (a `game` hook sets `D`, the data folder), extra files for the port folder |
| `build.commands`, `build.outputs`, `build.licenses` | none | for a port with its own compiled code: shell commands `port package` runs in the port folder first (with `PMUNITY`, `PORT_SRC` and `BUILD`, a scratch folder, in the environment), the files they make put into the port folder (`{"setup/files/codepatch" = "{build}/codepatch"}`), and licences for `licenses/` |

Keep the game's look and play: options that change the picture or the game play stay off unless
a player asks for them (a question with the vanilla choice as its default). One exception, for
handhelds with 1 GB of RAM: the `quality` step's handheld profile ([quality.md](quality.md)) makes
the picture cheaper there by default (shadow maps, MSAA, SSAO, per-pixel lights, a few post
effects; never the game play, never anything that would disappear), and keeps the game's own
picture on bigger devices. `port new` adds the question that lets the player choose:

```toml
[[questions]]
id = "PICTURE"
question = "Picture: lighter on handhelds with 1 GB of RAM (faster, the same game), or always as on PC?"
options = [["auto", "By this handheld (recommended)"], ["handheld", "Lighter (faster)"], ["full", "As on PC"]]
default = "auto"
convert_set = "quality.profile"
```

(a port.toml with questions of its own keeps only those: add this one to them; without the
`quality` step in `convert.steps` it is left out). The answer applies when the setup converts the
data; changing it later means running the setup again. The `textures` step
(ASTC instead of BC/DXT: less memory, slightly different pixels, a much longer setup on the
handheld) is such an option; the check says when a game has enough BC/DXT textures for it to
matter. The shaders step's `grab_fetch` is another: blend-mode sprites (an unnamed `GrabPass`
sampled at the sprite's own pixels) read the framebuffer instead of a copy of the picture made for
each of them, which a tiler pays for (Hollow Knight's King's Pass: three copies a frame); where
sprites of one batch overlap, or with Mali's 16-bit fetched colour, the blend differs slightly
([shaders.md](shaders.md#blend-modes-without-grabpass-copies-grab_fetch-opt-in)). As a question:

```toml
[[questions]]
id = "BLEND_FETCH"
question = "Blend-mode effects (light, glow): exactly as on PC, or faster with slightly different blending?"
options = [["false", "As on PC"], ["always", "Faster (slightly different blending)"]]
default = "false"
convert_set = "shaders.grab_fetch"
```

`"always"` reads the framebuffer where the GPU driver has `GL_ARM_shader_framebuffer_fetch` (Mali's
own drivers) or `GL_EXT_shader_framebuffer_fetch`; on a driver with neither the programs do not
compile and Unity 6000.0 uses the original shader (older players: not tried). `true` leaves the
choice to Unity's own check, which in 6000.0 knows only EXT's extension: on Mali's drivers it changes
nothing. Or set it in `[convert.shaders]` (`grab_fetch = "always"`) for every install. The answer
applies when the setup converts the data; changing it later means running the setup again.

### Small handhelds (1 GB)

On handhelds with 1 GB of RAM (the RG34XX-SP and most of Anbernic's H700 family) a big game may
give up some of its picture, never its game play: what is drawn may be smaller or plainer, but
timing, physics, collisions, scripts and everything that tells the player something (an attack's
warning, a hitbox, a pickup) stay. Such a reduction is set per handheld at launch, not built in by
the setup, so the same install shows the full picture on a bigger device and a player can have it
back by editing `port.env`:

```toml
[android_data]
asmpatch = ["auto", "hk-blur"]     # the default patches and the one that reads the variable

[runtime_lowmem]
ASMPATCH_HK_BLUR = "low"       # Hollow Knight's blur as its own Shader Quality Low ("half", "off": more, tools/asmpatch)
```

`port.env` then sets `ASMPATCH_HK_BLUR=low` where `MemTotal` is at most 1024 MB and leaves it
unset elsewhere; a value set above it in `port.env` or in the environment (`ASMPATCH_HK_BLUR=full`)
wins. The reductions so far are asmpatch's `reduce` patches (tools/asmpatch/README.md,
"Reductions"); a game's own lighter settings stay the player's, in its menus. What each one gains
on the RG34XX-SP, and what it costs the picture:
[profiling.md](profiling.md#hollow-knights-blur-and-particles-on-1-gb-handhelds).

Textures are the other big share of a 1 GB handheld's memory. `port check` adds the textures step
(`convert.steps`) when BC/DXT textures would decode to more than 128 MB (the GPU cannot sample
them, so Unity unpacks them to RGBA), and warns above 256 MB of uncompressed ones (RGBA32, RGB24:
Enter the Gungeon's bundles hold 869 MB): those stay as they are, and the textures step would make
them ASTC 4x4, a quarter of RGBA32's memory, with slightly different pixels. Try such a port on a
1 GB handheld first (`log.txt`'s memory lines) before adding the step for it.

The textures step can also halve the textures only where the handheld has 1 GB of RAM or less, as
the video step does with the cinematics: `lowmem_scale` is the scale used where `MemTotal` is at most
`ram_mb` (1024), `scale` elsewhere, and the setup log says which (`textures: 973 MB of RAM (at most
1024): scale 0.5 (lowmem_scale)`). A game drawn at a small handheld's size from art made for 1080p
keeps more than a texel for each pixel; text atlases (TextMeshPro's signed distance fields) belong in
`sharp_names`, which keeps `scale_sharp`. Unlike the `runtime_lowmem` reductions it is decided at
setup: a player who wants the full textures back unsets it and runs the setup again.

```toml
[convert.textures]
lowmem_scale = 0.5
sharp_names = "SDF|tmpro|Font Texture"
```

Hollow Knight (RG34XX-SP, King's Pass): the textures 2207 -> 552 MB in the package, the Mali
driver's pages for the game ~300 -> ~142 MB, the game ~830 -> ~670 MB in all; the handheld kept
~240 MB free instead of swapping at ~40 MB, the frame rate the same (51 fps).
`tools/port/rehearse.sh`'s `REHEARSE_MEMINFO` gives the setup container a 1 GB handheld's
`/proc/meminfo`, so a rehearsal takes these paths.

## 4. The setup on the handheld

`setup/patchscript`, run by PortMaster's patcher (log: `ports/<name>/patchlog.txt`; a failure is
a `PATCH_FAIL_MSG:` line the patcher shows):

1. the setup's tools: `setup/pmu`, one static aarch64 program (setup-rs, docs/rust-setup.md:
   the same files as the converter's Python, byte for byte); a port whose setup needs what pmu
   does not do yet (a porter's Python converter plugin, `il2cpp --on-device`: `port.py`'s
   `pmu_covers`) ships the setup's Python instead
   (`python.tar.gz`: CPython with UnityPy and the image libraries, aarch64), and the setup unpacks it;
2. the game: the data folder in `gamedata/` (any depth), else one from an archive there (`.zip`,
   `.7z`, `.rar`, `.tar.*`, with PortMaster's 7-Zip), else macOS installers (`.pkg`, `pmunity
   macpkg`), else the port's `game` hook; a compressed build is unpacked (`pmunity unpack`); its
   Unity version must be one of `game.unity_versions`;
3. Unity's Android player of that version, from the port's donors (`<port>/donor/`, section 5):
   `pmunity player donor <version> <pkg> --apk donor/donor-<version>-<backend>-<armv7|arm64>.apk`
   (mono-arm64: the IL2CPP donor's player with `--managed`, the engine modules from the Mono donor);
   no donor of the game's version is a plain failure ("This port ships no donor of Unity X"), never a
   download; checksums when the port has them; then `pmunity winbcl --donor` (unless
   `android_data.winbcl = "off"`): the class libraries a Windows 2021.2+ build needs and missing
   facades, from the Mono donor (section 2). Nothing on the handheld downloads anything, except the
   il2cpp-arm64 route's IL2CPP compile and the apk route's `player.il2cpp` (Unity's IL2CPP tools and
   static libraries, Debian's compilers, .NET: no donor carries them);
4. `pmunity convert` with `setup/convert.json`, the plugins and the answers (a Windows build's
   Direct3D 11 shaders with `setup/pmhlslcc`, a Vulkan-only build's with `setup/pmspirvcross`);
5. a Wwise game (`wwise.banks`): `pmu wwise <data>/<banks> <port>/wwise --wwise-lib <plugins>` turns its soundbanks
   into the data of ua_host's Wwise player ([wwise-player.md](wwise-player.md): Ogg Vorbis and
   ADPCM in a pak, the objects in a json; the Vorbis codebooks read from the game's own Wwise library; Enter the Gungeon's 177 MB in seconds, about 25 MB of
   memory), needing about the banks' size free; banks it cannot read leave the game silent and the
   setup goes on (the step runs only where the release ships pmu);
6. `pmunity android-data --consume-src --pack all`, with asmpatch, the class libraries of step 3,
   the managed files and assembly patches, and `android_data.args`;
7. installing: the player's libraries into `lib/` with only the Mono runtime the game needs (with
   both, libunity takes the .NET 4.x one and a .NET 3.5 game aborts), the APK, OBB and Mono's
   config into the port folder (with a GOG build's `goggame-*.info`, the game's own file, beside
   the APK: GOG builds read it at `Application.dataPath + "/../"`, which is that folder; it is
   not part of the release); the game's copy in `gamedata/` (used up by step 6), the tools and
   the work folder are removed; `.setup_done` marks it done. The launcher exports
   `UA_WWISE_DATA=<port>/wwise` when that folder is there (an empty `UA_WWISE_DATA` in `port.env`
   turns the player off).

Each step leaves a marker in `gamedata/.setup/`: an interrupted setup (battery, power button)
continues where it stopped on the next launch. Free space: about three times the game while it
runs. Where the time goes on a handheld (textures and the card, mostly): [setup-speed.md](setup-speed.md). The setup needs an aarch64 system for its tools and the game a 32-bit (armhf) userland
(the launcher checks for `/lib/ld-linux-armhf.so.3` first).

The il2cpp-arm64 route's setup takes the same steps with step 3 replaced: `il2cpp --on-device`
(on pmu, as the other steps; the Python's where the release ships it) compiles the game's code on
the handheld (it also fetches the IL2CPP player), with
the port's managed files, assembly patches (run on the game's original assemblies) and asmpatch,
which gets the opt-in patches `streaming_assets` and `data_path` ask for (`--streaming-assets`,
`--data-path`); step 6 packs with `--il2cpp` and the same `--streaming-assets` / `--data-path`
(android-data checks them against the build's `il2cpp.json`), and nothing of the code goes to
android-data.

The apk route's setup is shorter: the setup's tools (pmu, or the setup's Python in a release made
without it); the players this system runs, in order (the
APK's armeabi-v7a one where `/lib/ld-linux-armhf.so.3` exists and the port has `ua_host`, its
arm64-v8a one where the port has `ua_host64`; `player.abi = "arm64-v8a"` puts that first); the
packages in `gamedata/` (`game.data`, any depth, macOS's `._` files skipped) with the `.obb` files
there, else the APK installed before (a setup again, e.g. after moving the card to a 64-bit-only
handheld), else the `game` hook (it sets `INPUTS`); `pmunity apk --target <those players> --json`,
which checks the build and lays it out in the work folder (APKs and OBBs hard-linked, or copied
on exFAT/FAT; what it unpacks from an `.xapk` or rebuilds from an `.aab` moved); the package and the
Unity version checked against `game.package` and `game.unity_versions`; then installing: `lib/`,
`base.apk`, the OBBs under the names the player looks for, Mono's config and
`android_package.json` moved into the port folder, the player's files in `gamedata/` deleted
(`keep_installers=1` keeps them). Free space: about the build's size. The launcher picks `ua_host`
or `ua_host64` by `android_package.json`'s `abi`, and keeps the build's own frame pacing (ua_host
runs imported builds with `UA_FRAME_RATE=android`). With `player.il2cpp` (a Mono build) the
players are `armhf,arm64` (`ua_host64` is in the release), and on a system without the 32-bit
loader the call is `pmunity apk --target arm64 --il2cpp --on-device --home <work>/il2cpp-home`: the
build's assemblies compiled for Unity's arm64 IL2CPP player of its version (one to a few hours,
about 1.6 GB more free space, Wi-Fi for the tools; an interrupted build continues from
`<work>/apk/.il2cpp`), its output shown as it goes.

### A game set up by an earlier release

A release brings the port's `setup/` folder again, and a setup removes it when it ends. When the
launcher finds the game set up and `setup/update` there (android route), it runs that once through
PortMaster's patcher: what this release's setup does that an earlier one did not, done on the
installed files, then `setup/` goes. Now that is asmpatch with `setup.env`'s `ASMPATCH` on the
installed APK's assemblies (`pmu apk-asmpatch`: the patches the game has already say "done", a
newer release's go in; the changed assemblies are written after the APK's end, nothing there is
overwritten, [rust-setup.md](rust-setup.md)), and convert's `video` step when the port's
`convert.json` has it (`pmu video`: the installed APK's and OBB's VP8 clips made smaller in place on
1 GB handhelds, [rust-setup.md](rust-setup.md#video)); the update logs to `patchlog.txt` like the
setup, and a run that is interrupted runs again at the next launch.

## 5. Donors: build Unity's player for the port

**Every port of the `android` and `mono-arm64` routes needs donors, and the porter builds them
before `package`.** A donor is a small game of the port's own (a one-scene game of pong,
`tools/donor/Assets`) built with exactly the game's Unity version; its APK carries that version's
Android player, which the setup unpacks (`pmunity player donor`). The release ships the donors in
`<name>/donor/` with `licenses/LICENSE.unity-donor.txt` (Made-with-Unity applications, under Unity's
terms of service), so the handheld never downloads Unity's player: nothing else of Unity is shipped.

| route | donors (for each of `game.unity_versions`) |
|---|---|
| `android` | `donor-<version>-mono-armv7.apk` (the player; and the class libraries of `winbcl`) |
| `mono-arm64` | `donor-<version>-il2cpp-arm64.apk` (the player) and `donor-<version>-mono-armv7.apk` (its engine modules, the class libraries) |
| `il2cpp-arm64`, `apk`, `box64` | none (il2cpp-arm64 downloads Unity's IL2CPP tools on the handheld: section 4) |

`port check` prints the donors of the recommended route, `port new` the port's (and writes them to
`player.donors`), and `port package` stops, naming each missing donor and the command that builds
it. The steps, on a Mac:

1. **The editor** of that version with Android Build Support, unpacked, not installed:
   `tools/donor/get-editor.sh <version> <dir>` reads the version's changeset and downloads from
   Unity's release API (`PKGS=<folder>`: installers already downloaded), and unpacks both with
   `pkgutil --expand-full` (no administrator password, no Hub, no sign-in) into `<dir>/Unity.app`
   and `<dir>/PlaybackEngines/AndroidPlayer`. An editor the Unity Hub installed works as well.
2. **A licence:** the editor builds with Unity Personal's licence file,
   `/Library/Application Support/Unity/Unity_lic.ulf`, made once by the Unity Hub or Unity's manual
   activation. The tools never sign in.
3. **The Android tools** `build.sh` points the editor at: `SDK=<an Android SDK>`, `JDK=<a JDK home>`,
   and for an IL2CPP donor `NDK=<the NDK the editor asks for>` (2017.4: SDK tools r25.2.5,
   build-tools 28.0.3, platforms;android-28, JDK 8, NDK r13b; newer editors name theirs in their
   Android Build Support).
4. **Each donor:** `tools/donor/build.sh <dir>/Unity.app MonoArmv7 <DONORS dir>` and, for
   `mono-arm64`, `tools/donor/build.sh <dir>/Unity.app Il2cppArm64 <DONORS dir>`: it writes
   `donor-<version>-mono-armv7.apk` / `donor-<version>-il2cpp-arm64.apk`, the names the setup reads.
5. **Package** with them: `DONORS=<DONORS dir> <port>/package.sh`, or `port package --donors <dir>`.

Donors are never committed (a port's repository holds no Unity files; `port new`'s `.gitignore`
leaves out `*.apk`): keep the folder beside the port's repository and build the release from it.
Donors can be built on Linux too, with the editor from Unity Hub: give `Editor/Unity` as
`<editor>` to `build.sh` and use the Android SDK, NDK and OpenJDK that the Hub installs.
`DonorBuild.cs` works with 2023.1+ editors (API level 23, no GLES2, Gradle) and `player donor`
reads 2023.2+ donors (`unity default resources` is in `Resources/`, and there is only
`libmonobdwgc-2.0.so`, no `libmono.so`).
Known gaps: a donor carries only the class libraries its own build uses (section 2, Windows
builds); the Memory Advice models 2023.1's player reads have not been tried yet.

## 6. package

```
python -m pmunity port package <port folder> [--out DIR] [--donors DIR] [--runtime DIR] [--ua-host FILE]
                               [--media-licenses DIR] [--pmu DIR] [--pybundle FILE] [--asmpatch-bin FILE]
                               [--pmhlslcc-bin FILE] [--pmspirvcross-bin FILE] [--no-build]
```

(or the port's `package.sh`, `DONORS=<dir>`) builds `<out>/<name>.zip` (default `<port>/out/`): the
launcher and the port folder, the donors (`--donors DIR`, else `$DONORS`: each of `player.donors`
into `<name>/donor/`, with `licenses/LICENSE.unity-donor.txt`; a missing one stops the build,
section 5), `ua_host` (`runtime/host/out/`, built with `runtime/build.sh` when missing), the
setup's tools: pmu where it runs the whole setup (`--pmu DIR` with `pmu` and `licenses/`, else
`setup-rs/tools/build_pmu.sh tools/port/out/pmu` builds it in setup-rs's Rust image), else the
converter (`convert/pmunity`) and the setup's Python (`--pybundle`, else
`tools/port/build_pybundle.sh` builds it: python-build-standalone's CPython 3.12 and the
converter's wheels for aarch64), asmpatch for linux-arm64 when the port uses it
(`tools/asmpatch/build.sh`), pmhlslcc for linux-arm64 when it translates Direct3D 11 shaders
(`ARCH=arm64 tools/hlslcc/build.sh`, Docker; `--pmhlslcc-bin` another build, with its
`pmhlslcc.LICENSE.txt` beside it; glibc 2.17 symbols only, libstdc++ linked in), pmspirvcross
likewise when it translates Vulkan shaders (`ARCH=arm64 tools/spirvcross/build.sh`;
`--pmspirvcross-bin`), and `licenses/`:
portmaster-unity's licence and THIRD_PARTY.md, libhybris (Apache 2.0, in ua_host), the video
decoders' licences (`runtime/media/out/licenses`), pmu's (what is compiled into it) or the setup
Python's and its packages' licences,
for asmpatch the .NET runtime's licence and third-party notices and Mono.Cecil's, for pmhlslcc
HLSLcc's (MIT, with bstrlib's BSD), and for pmspirvcross SPIRV-Cross's (Apache 2.0) and SMOL-V's (MIT). `--runtime` points at another checkout whose runtime and
asmpatch are built. Zip entries keep their Unix modes and get a fixed date. The il2cpp-arm64 route
ships the 64-bit `ua_host` (`runtime/host/out/arm64/`, `ARCH=arm64 runtime/build.sh`) as
`ua_host`; the apk route ships `ua_host` for an APK's armeabi-v7a player and that 64-bit one as
`ua_host64` for its arm64-v8a player (`player.abis`), each with its build's decoder licences (the
64-bit one's as `LICENSE.<part>.ua_host64.txt`).

The zip also gets the screenshot and the cover (`port.screenshot`, `port.cover`) as
`<name>/screenshot.png` and `<name>/cover.png`, where PortMaster's own release zips have them, and
`testing_thread.txt` goes beside the zip in `<out>/`. Then `package` lists what a PortMaster
submission still lacks or reviewers would ask about (none of it stops the build): port.toml fields
still empty (porter, description, genres, developer, publisher, release date, the thank-you, the
controls, the game's link), no screenshot or one that is not 4:3 and at least 640x480, no cover, a
`gameinfo.xml` naming an image the port does not ship, an em dash or Markdown in the plain texts,
`exp = true`, a launcher without PortMaster's standard first lines (or with `"$GPTOKEYB2"` quoted, a
fixed `SDL_VIDEODRIVER`, no `pm_finish`), files over 90 MB. `--submission` also writes
`<out>/submission/<name>/`, the port as PortMaster's repository holds it (section 12).

## 7. Rehearsal

`tools/port/rehearse.sh <zip> <game folder or archive> <work dir> [emu seconds] [input script]`
unpacks the release into `<work>/roms/ports` as PortMaster would, copies the game into its
`gamedata/` and starts the port's own launcher in an aarch64 container that looks like a handheld
(busybox first on PATH, bash, curl, a 1 GB memory limit, the network on), with a stand-in for PortMaster
(`control.txt`, `utils/patcher.txt`, PortMaster's own `7zzs.aarch64` from its release: `PM_7ZZ`).
`ANSWERS="ID=value ..."` answers the port's questions (a GOG `.sh` installer, a `.zip`, `.7z`,
`.pkg` and the other archives are copied into `gamedata/` as they are, for the setup to unpack;
a folder is copied as it is). The container's `/proc/meminfo` shows
Docker's memory, not the 1 GB limit, so a setup that sizes itself from it (the `PICTURE` question's
`auto`) takes the path of a bigger device: `ANSWERS="PICTURE=handheld"` rehearses the 1 GB one.
After the setup the launcher also tries to start the game in the container and stops with
`libSDL2-2.0.so.0` missing (exit 127): expected there, the game runs in the rig next. With `[emu seconds]` the installed game then
runs in the emulated armv7 rig (`tools/emu`, QEMU and Mesa's llvmpipe: slow, from about 20 fps on a
loading screen to under 1 fps on a busy scene, but the same frames and game logic; the screen is
`ANDROID_WINDOW_SIZE`, default 720x480, the RG34XX-SP's) with the port's `port.env` and `answers.env`, and four screenshots
go to `<work>/shots.png`; `UA_INPUT_SCRIPT`-style input (`"60:ptr=0.5/0.5 70:a~1.5"`) drives it
(buttons `a b x y l1 r1 l2 r2 start select` by position, as on an Xbox pad: `a` is the bottom one;
`up down left right` the d-pad; `lup ldown lleft lright` the left stick; `~<s>` holds; `@<s>`
repeats: [runtime.md](runtime.md)). The rig's virtual pad has sticks, so `UA_DPAD_STICK = "auto"`
does not apply there: move with `lleft`/`lright`, or set `UA_DPAD_STICK=1` for the run to press
the d-pad as a handheld without sticks would. `EMU_ONLY=1` reruns only the game; an earlier run's
`run.log`, screenshots and sound move to `<work>/earlier/<n>/` first. A run prints nothing until it
ends; the game's log is `<work>/emuroot/run.log`. To stop one early, name the containers (`REHEARSE_NAME`, `EMU_NAME`)
and `docker kill` them: killing `rehearse.sh` or the `docker run` client leaves the container
running to its timeout. An install of an arm64-v8a build (`android_package.json`'s
`abi`: the il2cpp-arm64 route, the apk route on a 64-bit-only system, `REHEARSE_64ONLY=1`) runs in the arm64 rig
(`EMU_ARCH=arm64`) with the port's `ua_host64`. `REHEARSE_NAME` and `EMU_NAME` name the containers.
The setup's space checks see the work folder's drive as the card (a `df` stand-in, and
`PMUNITY_DISK_FREE_MB` for pmunity's own checks: Docker Desktop's file sharing reports another
disk's free space).
The rig decodes ASTC in software and keeps the result, so a
game converted with the textures step may run out of the rig's memory where the handheld's GPU
would not.

**Sound.** Nothing plays aloud: the rig writes what the game's OpenSL ES output played (the game's
own sound and, for a Wwise game, ua_host's Wwise player's mix in it) to `<work>/sound.wav` (48 kHz
16-bit stereo, the whole run, about 11 MB a minute), and the rehearsal ends with its levels:
`sound: <work>/sound.wav: 532 s, peak -2.1 dBFS, RMS -23.5 dBFS, sound in 465 of 533 seconds`
(Enter the Gungeon, a 600 s run: its title music, then the Breach's; "none": the game opened no
output). `REHEARSE_SOUND=0` turns it off. As the launcher does,
the run sets `UA_WWISE_DATA` to the port's `wwise/` folder when the setup converted Wwise banks
(an empty `UA_WWISE_DATA` in `port.env` keeps the player off), so a Wwise game's music and sounds
are in the file; the Wwise player's own mix alone: `UA_WWISE_CAPTURE=/emu/wwise.wav` (for the whole
run, unless `UA_WWISE_CAPTURE_SECS`), written to `<work>/emuroot/wwise.wav`. Variables reach the rig
as `tools/emu/emu.sh` passes them: `UA_*`, `GC_*`, `ANDROID_*`, `GALLIUM_*`, `LP_*` and the prefixes in
`EMU_ENV`; `/emu` in the rig is `<work>/emuroot/`, the port folder is `/zip` (read-only). A game
reaches its title after some minutes in the rig (Enter the Gungeon: about 3), so a run of 300 - 600
s is what shows the title music.

## 8. On a device

The rehearsal does not cover what only a handheld has. Check, with the release zip unpacked into
`ports/`:

- the setup through PortMaster's real patcher: the progress screen, the questions, a failure
  message (rename the game's folder to see one), an interrupted setup (power off during the
  conversion) continuing on the next launch;
- the setup with the network off (it downloads nothing: the player comes from `donor/`); the
  il2cpp-arm64 route's download over the handheld's Wi-Fi (the system's CA certificates: the setup
  sets `SSL_CERT_FILE` when it finds a bundle);
- the time and the peak memory of the setup on the device's CPU (the textures step is the slow
  part: ASTC encoding). A rehearsal container has the PC's memory, not the handheld's: a setup
  that passes there can still be killed by a 1 GB handheld's kernel. The textures step keeps to
  a few hundred MB whatever the game (bundles over `textures.spill_mb` are unpacked to files and
  written back a chunk at a time, a `.resS` is read where a texture needs it, big single-level
  textures are converted in strips of rows, and under 2 GB of RAM a big texture is converted
  alone); to see a game's peak before a device run, run the converter in an arm64 Linux
  container with the setup's Python and read its maximum resident memory (`getrusage`; macOS's
  own figure leaves out compressed memory);
- the game: the pad (the mapping from `get_controls`; `UA_PAD_NAME` for Rewired), pointer mode and
  `UA_KEYMAP` where the menus want a mouse or keys, sound (ALSA, else `ANDROID_SLES_OUTPUT=pipe`
  when a sound server holds the card), video, saves (`ports/<name>/data/`: `external/` is
  `persistentDataPath`, `shared_prefs/` PlayerPrefs; they survive updates; moving them to and from
  a PC: docs/android-data.md, "Saves", and `pmunity prefs`), the exit hotkey, the frame rate
  (`UA_FPS=5` in `log.txt`);
- the display path (`UA_DISPLAY`: Mali fbdev or SDL; `[display]` in `log.txt`).

## 9. By hand

The same steps without the setup, on a PC:

1. **Check** the version (`python -m pmunity version <Data>`), the backend and the graphics APIs
   (`port check` says all of it). The runtime knows the Android players of 5.6 - 6000.6; for
   others `tools/re/elfimports.py` lists a player's imports (compare with
   `runtime/android/README.md`) and `FJ_DUMP=1` lists the Java classes and methods Unity asked for
   and did not find. The handheld has little RAM (1 GB with the system): the textures step can
   also scale textures down (`textures.scale`), and the audio step keeps the sound a scene holds
   decompressed within `audio.pcm_budget_mb` (16 MiB by default): the game's DecompressOnLoad
   clips are decoded once at load, as on the desktop, and only the largest long clips of a scene
   over the budget play compressed (FMOD then decodes them on the CPU while they play, which
   takes the GPU's clock in a warm handheld: [profiling.md](profiling.md#fmods-mixer-and-the-audio-step-2026-10-24)).
   A port with memory to spare raises it (the step's log names the scene that holds the most).
2. **Convert**: `python -m pmunity convert <Data> [<out>] --config game.json` with a config like
   the `convert` table above (`{"steps": ["shaders", "settings", "audio"], "settings":
   {"graphics_apis": [11]}}`); a game's own steps go in a plugin: field edits in a JSON file
   (`--plugin tweaks.json`, which the setup's pmu runs too: [plugins.md](plugins.md#converter-plugins-a-ports-own-conversion-steps)),
   else a Python file (`--plugin steps.py` with `STEPS = {"name": function(job)}`). Check the shaders on the device:
   `python -m pmunity shaders-export <converted Data> glsl/`, then `runtime/host/out/shadercheck
   glsl` there (docs/shaders.md).
3. **Package**: `python -m pmunity player donor <version> <pkg> --apk <donor.apk>` (section 5), then
   `python -m pmunity android-data --src <converted Data> --pkg <pkg> --out android --pack all
   [--asmpatch <asmpatch>] [--mono-etc <Game>.app/Contents/Mono/etc]` (docs/android-data.md).
4. **Run**: on the handheld, in one folder: `ua_host`, `lib/` with the player's libraries
   (`Variations/mono/Release/Libs/armeabi-v7a/*.so` and, from `MonoLibs/`, the Mono runtime the
   game needs) and the contents of `android/zip/`; then `tools/device/run.sh first.log 60` and
   read the log (`I/Unity`, `[host]`, `UA_FPS=5`). From the PC, `DEVICE=root@<handheld>
   tools/device/test.sh <folder> 120 30 90` does all of it over SSH (or over USB with
   `DEVICE=adb`, when the handheld's firmware runs adbd, as Knulli does: no network needed;
   tools/device/transport.sh): it copies the folder (only
   what changed) with this checkout's `ua_host`, runs the game with a memory watchdog, takes
   screenshots at 30 and 90 s, and brings back the log, a memory log and the screenshots with a
   summary (frames per second, peak memory, how the run ended). While it runs,
   `REMOTE=uatest/<folder> tools/device/drive.sh "0:a~2 6:down" 10 now.png` presses buttons (the
   game folder's `input.req`) and brings back a screenshot a few seconds later: a game driven by
   what it shows, menus and all. In the emulated rig: `tools/emu/emu.sh` (the same `input.req` and
   `shot.req` in `EMU_ROOT`).

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
| slow everywhere (a steady low frame rate) | `UA_PROF=500` + `touch prof.req` + `tools/analysis/profsum.py`; on a handheld, measured runs side by side: `tools/device/profbench.sh` (docs/profiling.md) |
| a steady 30 fps (`UA_FPS=5`) | `[host] frame rate` in `log.txt`: 2021.1+ Android players render at a fixed 30 fps unless the game sets `targetFrameRate`; converted desktop data gets the desktop's vsync pacing (`UA_FRAME_RATE=desktop`, the default when `android_package.json` names Unity 2021.1+: repack data made before that); `UA_FRAME_RATE=30` caps a game too heavy for 60 (docs/runtime.md) |

## 10. What may need a game's own code

Desktop builds sometimes assume a desktop: a store client (GOG Galaxy, Steamworks) the game waits
for, DLC found through the store, input set up for keyboards and desktop pads, a resolution or
aspect that does not fit the screen. Generic fixes are in the runtime (Wwise without its ARM library:
[plugins.md](plugins.md#wwise); a d-pad for the left stick: [input.md](input.md#handhelds-without-sticks);
the command line and `Screen.resolutions`: [runtime.md](runtime.md#what-a-desktop-game-sees)) and in
`tools/asmpatch` (`android_data.asmpatch`: e.g. Rewired on its Android platform); a game's own go in
its port: patches to its assemblies (`files.assembly_patches`) and a helper assembly
(`files.managed`), as portmaster-cuphead does with Mono.Cecil. Changes that alter the picture or
the game play belong behind an option, off by default.

### A port's own assembly patch

The setup runs it on the handheld, on the player's copy of the game: android-data's `--assembly-patch
NAME COMMAND` (port.toml's `files.assembly_patches = [{name, command, files}]`) runs `COMMAND` with
`{managed}` replaced by the game's original Managed folder and `{out}` by an empty folder, where it
must write `NAME` (e.g. `Assembly-CSharp.dll`); a non-zero exit stops the setup with its output. Its
output lines go to `patchlog.txt` (`assembly patch <NAME>: <line>`), and the lines that start with `ok`
also into `android_data_report.json`. The command is a program for the handheld's aarch64 system,
which has no .NET: a self-contained single file, built by `port package` from the port's sources:

```toml
[files]
assembly_patches = [{name = "Assembly-CSharp.dll", command = "{setup}/files/mypatch {managed} {out}", files = []}]

[build]
commands = ["patch/build.sh"]                       # dotnet publish -r linux-arm64 --self-contained
outputs = {"setup/files/mypatch" = "{build}/mypatch"}
```

`patch/build.sh` publishes a small Mono.Cecil program as tools/asmpatch/build.sh publishes asmpatch:
`dotnet publish <proj> -c Release -r linux-arm64 --self-contained -p:PublishSingleFile=true
-p:PublishTrimmed=true -p:InvariantGlobalization=true -p:DebugType=none -p:TrimmerRemoveSymbols=true`
(and a check that the binary names no folder of the build machine). The program reads
`{managed}/NAME` with a resolver on `{managed}`, changes what it must, writes `{out}/NAME`, prints an
`ok   <what>: <how>` line per change, and exits non-zero (with a `FAIL` line) when what it patches is
not there, so a game update that moved the code stops the setup instead of shipping a half-patched
game. Patch what only this game needs; what any game of its kind needs belongs in the runtime or
asmpatch.

With asmpatch on (`android_data.asmpatch`, or `streaming_assets` other than `obb`), an assembly both
change gets both: the port's patch runs first, on the original, and asmpatch then patches its output
(tools/asmpatch/README.md). The il2cpp-arm64 route runs the same command before its IL2CPP build.

### Community performance mods and fixes

Before writing a game's speedups, look for what its players have already found. Players profile
their games for years, and a performance mod's list of changes is a map of the game's hot spots,
often with what each saves. Search, with the game's name:

- GitHub (`<game> optimization`, `performance`, `fps`, `lag`, `stutter`, `memory leak`), the game's
  Thunderstore community, Nexus Mods, ModWorkshop, its modding wiki and Discord's public pages;
- PCGamingWiki's "Essential improvements" and Steam guides (config files, launch options);
- speedrun.com resources (what runners turn off for steady frame times).

Enter the Gungeon, for example: [GungeonGoVroom](https://github.com/pcrain/GungeonGoVroom) lists
about thirty CPU and memory optimizations of the vanilla game (occlusion, light culling, goop
updates, linear casts, the ammo display, beams), each marked by whether it keeps vanilla
behaviour; [Gungeon Go Brrr](https://modworkshop.net/mod/30986) speeds up the Mod the Gungeon
loader itself (mod loading, object creation with mods), not the vanilla game.

Write each candidate into the port's testing notes: the link, its licence, what it changes and
what it says it saves. Then:

1. **Its licence decides how it may be used.** An open licence (MIT, BSD, Apache): its changes may
   go into the port's own assembly patch, with the licence and the authors' credit in the port.
   GPL: only in a separate GPL program (the patch), never mixed into code under another licence. No
   licence (all rights reserved): never copy its code. Ask the author for a licence or permission (a
   message the port's owner sends, never one sent for them), or write the port's own patch from the
   game's code and your own profile, taking from the mod only its public description of what is slow.
2. **Only what keeps the game play identical goes in by default.** A mod's bug fixes change the game
   (GungeonGoVroom's Shuffle Fix changes which rooms spawn): an option at most, off by default.
   Visual changes follow the 1 GB rules ([quality.md](quality.md)).
3. **Measure each on the handheld,** as an A/B in one hold ([profiling.md](profiling.md)). A change
   that saves a lot on a desktop CPU may save little on a Cortex-A53, or the reverse.
4. **The port loads no mod loader** (BepInEx, Harmony, MelonLoader, Mod the Gungeon): the change
   goes in as a Cecil patch run at setup on the player's own copy of the game (above), never as
   shipped game assemblies.
5. **Credit the authors** in the port's README, also when only their description was used.

## 11. Extending `pmunity port`

Other parts of pmunity add to the check, the port and the release without editing `port.py`: a
module `pmunity.<name>` with a name in `port.HOOK_MODULES` (`route`, `box64`, `il2cpp`, `apk`,
`services`, `plugins`, `inputtext`, `osk`, `windows`; more in `$PMUNITY_PORT_HOOKS`) may define

| function | called with | may |
|---|---|---|
| `port_check(report)` | the check's report (a dict: `build`, `backend`, `unity_version`, `code`, `data_survey`, `plugins`, `routes`, ...) | append to `report['notes']`, change or add `report['routes']` entries (`{route, ok, why, needs, available}`), put port config values in `report['suggest']` |
| `port_new(ctx)` | the port being written: `ctx.cfg`, `ctx.out`, `ctx.route`, `ctx.write(rel, data, mode)` | add files (e.g. a plugin map, an on-screen keyboard's settings) |
| `port_package(ctx)` | the release being assembled: `ctx.cfg`, `ctx.port_dir`, `ctx.runtime`, `ctx.add(src, rel, mode)`, `ctx.license(name, path=, text=)` | add binaries and their licences (e.g. box64, an ARM plugin build) |

A new route is a template folder (section 2); `port new --route <name>` uses it.

## 12. Submitting to PortMaster

A port goes into PortMaster through a pull request to its ports repository, after a test zip has
been tried in its Discord testing channel. What PortMaster asks for, and what `pmunity port` makes:

| PortMaster wants | made by | the porter's part |
|---|---|---|
| `port.json`, version 4 (`name`, `items`, `attr`: title, porter, desc, inst, genres, `rtr`, `exp`, runtime, store, availability, reqs, arch, min_glibc) | `port new` (`<name>/port.json`; route hooks set Westonpack, `armhf`, glibc) | porter, desc, genres, store in port.toml |
| `gameinfo.xml` (path, name, desc, release date, developer, publisher, genre, image) | `port new` (`<name>/gameinfo.xml`; the image: `./<name>/cover.png` when the port has a cover, else the screenshot) | developer, publisher, release date (look them up) |
| `README.md`: Notes (a thank-you to the game's makers first), Controls (a row per bound button), Compile | `port new` (`README.md`) | `port.thanks`, `[controls]` |
| `screenshot.png`: a real capture of the game running, 4:3, at least 640x480 | `package` ships it and checks its size; `port shot` saves a frame | the capture |
| `cover.png` (optional, expected) | `package` ships it | the image |
| the launcher `<Title>.sh` and the folder `<name>/` | `port new`, `package` | explaining every line reviewers ask about |
| `testing_thread.txt`, the testing channel's post | `port new` (beside the port; `package` copies it beside the zip) | the zip's link, the testers' results |
| the repository's layout: `ports/<name>/` with the files above at the top | `package --submission` (`<out>/submission/<name>/`) | the pull request |

`package --submission` lays the port out as the repository holds it: `port.json`, `README.md`,
`gameinfo.xml`, `screenshot.png`, `cover.png` and `<Title>.sh` at the top, `<name>/` beside them
(without the four files PortMaster's build puts back into it: its release zip has them in
`<name>/`, and the README as `<name>/<name>.md`). Copy that folder to `ports/<name>/` in a checkout
of the repository; files over 90 MB are split into pieces there by the repository's own tools.

The screenshot is a frame of the game as the port runs it, never a made, scaled or cropped
picture. On a handheld, start the game with `tools/device/test.sh` and take it with
`DEVICE=root@<handheld> REMOTE=<the folder> tools/device/drive.sh "" 0 screenshot.png`; for a game
ua_host runs in a folder this machine can reach (the emulated rig's `EMU_ROOT`), `python -m pmunity
port shot <folder> --out screenshot.png` asks ua_host for the next frame (`shot.req`) and saves it
as a PNG (`port shot <shot-N.ppm>` converts one already taken). PortMaster wants 4:3: a 3:2
screen's capture (720x480) is what it is, and `package` says so; take it on a 4:3 handheld when one
is at hand.

The pull request's template asks the porter to confirm they understand the launcher and can
explain every line that differs from PortMaster's usual ones. In these launchers that is: the
first-launch setup through PortMaster's patcher (`PATCHER_FILE`, `PATCHER_GAME`, `PATCHER_TIME`,
`PATCHER_QUESTIONS`), the 32-bit userland check and `/lib32:/usr/lib32` first on `LD_LIBRARY_PATH`
(the android route's `ua_host` is an armhf program), `SDL_AUDIODRIVER=alsa` as the default there
(the 32-bit `ua_host` finds no 32-bit PipeWire or PulseAudio client libraries on the firmwares;
`port.env` can change it), `UA_FPS` (a frame-rate line in `log.txt`), `port.env` and
`answers.env` (the game's options and the setup's answers), and for box64 the Westonpack block,
box64's settings and the command written to `conf/run.sh`. Testing on the firmwares and screens
of the testing thread's checklist is the testers' and the porter's work; the rehearsal
(section 7) and `tools/device/test.sh` cover the setup and one handheld.
