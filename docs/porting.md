# Porting a Unity game

A PortMaster port of a Unity game made with this repository is a launcher, a setup and one or two
**donors**: small games of the port's own built with the game's exact Unity version, whose APKs
carry Unity's Android player. On the first launch the setup unpacks the player from the donors,
converts the player's own copy of the game on the handheld and packs it for the player; every
launch after that runs it with `ua_host`. Nothing of the game is in the port, and of Unity only
the donors. **The handheld never downloads anything: the porter builds the donors (section 5) and
the release ships them.** `pmu port` writes all of that from a short settings file, so a port
needs no scripts of its own:

```sh
alias pmu="$PWD/setup-rs/tools/pmu.sh"                   # (in the checkout) pmu in Docker
pmu port check <game>                      # what the game is, what it needs, the route, the donors
pmu port new mygame --data <game>          # -> mygame/ with port.toml and the port
$EDITOR mygame/port.toml                                 # title, pointer mode, keys, questions, ...
pmu port new mygame --data <game>          # regenerate; names the donors to build
tools/donor/get-editor.sh <version> editors/<version> # (a Mac) the game's Unity editor + Android Build Support
tools/donor/build.sh editors/<version>/Unity.app MonoArmv7 donors   # each donor port new named (section 5)
DONORS=donors mygame/package.sh                          # -> mygame/out/mygame.zip, the release (with the donors)
PM_7ZZ=<PortMaster's 7zzs.aarch64> tools/port/rehearse.sh mygame/out/mygame.zip <game> work/mygame 300
pmu port package mygame --donors donors --submission   # + mygame/out/submission/mygame/ for PortMaster's repository
```

`pmu` is one static Rust program (setup-rs: the setup's tool on the handheld and the porter's on the
computer); `setup-rs/tools/pmu.sh` runs it in Docker with the folders the command names, and runs
what it builds (the runtime, asmpatch, a port's own `build.commands`) on the computer
(docs/rust-setup.md, "The port packager").

`<game>` is the folder of a desktop build as you have it (the one with the executable and
`<Game>_Data`), a macOS `<Game>.app`, or the data folder itself; or the game's own Android build
(an `.apk`, with its `.obb` files beside it, an `.xapk`, `.apks` or `.aab`, or a folder of them: the
`apk` route, section 2). Nothing in it is run or changed (pmu only reads it; through pmu.sh it
runs in a container that sees only the folders the command names). Cuphead's port ([portmaster-cuphead](https://github.com/wild1walker/portmaster-cuphead))
was written by hand before these tools; it shows what a game may need beyond them (its own code
patches and a helper assembly).

## Rules for every port

These hold for every port made with these tools, whichever game and whichever handheld.

- **64-bit only.** A release ships aarch64 binaries only, no armhf (`[port] aarch64_only = true`;
  [arm64.md](arm64.md), "Shipping aarch64 only"). A port moves once its 64-bit build is measured on
  the handhelds at the 32-bit build's frame rate and memory, with the same picture and game play.
  Tested so far: Hollow Knight passes, upgrade from the 32-bit release included (2026-11-23).
  Gungeon does not yet: on a 1 GB handheld its dungeon floors take 120-170 MB more than the 32-bit
  build (the managed heap, 8-byte references) and it runs out of memory on the G350, so it keeps its
  armhf build until that is closed. New ports start 64-bit only.
- **Every speedup on every handheld.** Whatever makes the game faster is on by default on every
  device, not only on those with 1 GB of RAM: `[runtime]`, and `[runtime_low]` for what changes the
  picture ([The Low picture at launch](#the-low-picture-at-launch-runtime_low)). `[runtime_lowmem]`
  is only for what trades something away to fit in memory (a smaller sound budget, half textures)
  and gains nothing where memory is not short. A new gain is never gated on RAM, screen or device
  name without a measurement that shows it costs something elsewhere.
- **Game play is never changed**, and the setup offers no heavier picture
  ([quality.md](quality.md)).

## Before you start

- **The computer:** Docker (pmu runs in it: `setup-rs/tools/pmu.sh`), `runtime/build.sh` once, and the .NET 8 SDK on the computer itself (no
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
  game is `<dir>/data/noarch/game`. GOG's macOS `.pkg`: `pmu macpkg <dir> <installer>.pkg`
  (the app lands under `<dir>` at the package's install location). Steam: the game's folder (the Linux depot where there is one).
  A Windows-only Steam game on a Mac: type `download_depot <app> <depot>` for each of its Windows
  depots in Steam's console (`steam://open/console`; the depot ids are in SteamDB or in the app's
  `depots` section of Steam's `appcache/appinfo.vdf`); the console prints the folder when a depot
  is done. Only someone at the Mac can do this: the console has no command-line or file input, and
  a platform override (`userdata/<id>/config/compat.vdf`) changes nothing on macOS, because every
  compatibility tool Steam lists targets Linux (Bomb Rush Cyberfunk, 2026-11-09: Steam "installed"
  it with no depots). Keep the installer itself for
  the handheld and the rehearsal: the setup takes it as it is.
- **The port's own repository:** a GitHub repository per port holds `port new`'s output and
  nothing of the game. Clone it (a new one holds a LICENSE and a README: `port new` writes into
  such a clone without `--force`), run `port new <name> --data <game> --out <repo>`, edit
  `port.toml`, run `port new` again, commit. `port new` writes a `.gitignore` once (the build
  output `out/`, game and Unity file types, the rig's screenshots); `package.sh` there builds the
  release, with `PMUNITY` pointing at a portmaster-unity checkout (default: one beside the
  repository). A port's own code (an assembly patch, a hook) goes in the repository too, with its
  build in `[build]` (section 10). Issue forms help players report what a fix needs:
  [Hollow Knight's](https://github.com/wild1walker/portmaster-hollowknight/tree/main/.github/ISSUE_TEMPLATE)
  (a problem, performance, a feature; the handheld, firmware, release, the setup's answers in the
  labels the setup showed, the log) are a starting point to copy and rename.
- **The pad bindings:** a game's controls are in its input stack's defaults (Rewired's XML,
  InControl's or the Input System's actions in the assemblies); `port check`'s input survey says
  which stack. On a handheld without sticks a game that moves only with the left stick needs the
  d-pad as that stick (docs/input.md).

## 1. port check

```
pmu port check <game> [--json] [--asmpatch CMD] [--no-scan]
```

reads the game and prints:

| what | from |
|---|---|
| Unity version, inside the converter's range (5.6 - 6000.6) or not | the data's serialized files |
| scripting backend (Mono: `Managed/*.dll`; IL2CPP: `GameAssembly`, `il2cpp_data`), .NET 3.5 or 4.x runtime | the build's files |
| platform build (Linux, macOS, Windows; the executable's architecture), compressed (`data.unity3d`) or not | the executable's header, the data folder |
| graphics APIs, and so the shaders the converter can translate (OpenGL Core, Direct3D 11) | `BuildSettings` |
| input stacks: Rewired (source or DLL edition), InControl, the Input System package, the legacy Input calls by kind, `OnMouse*` handlers, UI pointer handlers, code that selects UI elements; the UI input modules the scenes use; the verdict (pointer-driven menus or not); InControl's default pad bindings, every one by control (the `[controls]` draft), the actions only the left stick drives (`UA_DPAD_STICK`), the d-pad's own (a `UA_DPAD_SWAP` button: one the game binds nothing to, else one whose actions other buttons also do), those only the right stick drives (a handheld without one cannot do them) | the game's assemblies' metadata (read by pmu: names only, no code is loaded), the scenes' scripts, and `asmpatch --survey` (`--asmpatch` or `$PMUNITY_ASMPATCH`, else `tools/asmpatch/out`, which the check builds with the .NET 8 SDK when it is missing; without either, a warning says what was left out; `--asmpatch none`: no survey) |
| text input (InputField, TextMeshPro InputField, TouchScreenKeyboard, `Input.inputString`) | assemblies, scenes |
| video (VideoPlayers, clips and their codecs; MovieTexture) | scenes, `.resource` headers |
| native plugins, P/Invoke modules, services (Steamworks, GOG Galaxy, FMOD Studio, Wwise, ...) | `Plugins/`, assemblies |
| file access: reads of StreamingAssets that reach System.IO (then `android_data.streaming_assets`), and of `dataPath`, `persistentDataPath`, the working directory | `asmpatch --survey`/`--list` (without asmpatch: assemblies that reference both the getter and System.IO, a hint) |
| texture formats and sizes (main data); a warning above 256 MB of uncompressed ones (RGBA32, RGB24: what a 1 GB handheld must hold as they are; the textures step makes them ASTC) | the serialized files |
| StreamingAssets bundles: how many, the scene bundles (several serialized files), their shaders by the programs they have, texture formats and sizes. Warnings for what the converter will not handle: files that look like Unity bundles of another kind (UnityWeb, UnityRaw, a UnityFS header behind other bytes: a game's own offset or encryption), bundles UnityPy cannot open, shaders in bundles with no programs of the source the converter translates from (they draw magenta); a note for scene bundles and for bundles without a serialized file | UnityPy, a bundle at a time |
| the routes that fit, the recommended one, and the port config values it suggests (the textures step when the BC/DXT textures of the main data and the bundles together would decode to more than 128 MB) | all of the above |

An Android build (the `apk` route) is read with `pmu apk`'s dry run instead: package,
versionCode and versionName, Unity version, backend, the ARM ABIs it has (and so which systems run
it), graphics APIs, OBBs, native plugins, the screen orientation, and for a Mono build the same
input survey of its assemblies (`assets/bin/Data/Managed/`): `UA_POINTER = "touch"` for a game
that reads only touches, `"1"` for pointer-driven menus. A build `pmu apk` refuses (x86 only,
Vulkan only, VR, IL2CPP without its metadata, Unity data in an OBB not given, ...) gets the reason.

The exit status is 0 when a route fits. `--json` gives the whole report (`port new` uses the same).

## 2. Routes

| route | when | status |
|---|---|---|
| `android` | a Mono build of Unity 5.6 - 6000.6 with OpenGL Core shaders (macOS, Linux) or Direct3D 11 (Windows: `shaders.from = ["d3d11"]`, translated on the handheld by the release's `pmhlslcc`; a 2021.2+ Windows build's class libraries fetched by the setup, `pmu winbcl`), on a handheld with a 32-bit (armhf) userland: Unity's ARMv7 Android player run natively by `ua_host` | templates in `setup-rs/templates/android/` |
| `il2cpp-arm64` | a Mono build of Unity 2017.4+ on a 64-bit-only system (no armhf userland): the game's code compiled with IL2CPP into Unity's arm64 IL2CPP Android player, run natively by the 64-bit `ua_host` | templates in `setup-rs/templates/il2cpp-arm64/`, hooks in `il2cpp.py`: the setup compiles the code on the handheld (`pmu il2cpp --on-device`: one to a few hours on a Cortex-A53, about 1.6 GB free, Wi-Fi), then converts and packs as the android route (`android-data --il2cpp`); by hand on the PC: docs/il2cpp.md. What changes the game's code (asmpatch, `files.managed`, `files.assembly_patches`, the opt-in patches of `streaming_assets` and `data_path`) goes into that IL2CPP build, not into android-data |
| `mono-arm64` | a Mono build of Unity 2017.4+ with the .NET 4.x runtime on a 64-bit userland: the game's own assemblies run by Unity's Mono (built for aarch64 Linux, shipped with the release) inside Unity's arm64 IL2CPP player, whose `libil2cpp.so` is a bridge to it; no IL2CPP compile, the desktop's runtime and class libraries. .NET 3.5 games run on the line's MonoBleedingEdge with Mono's 4.x class libraries built from the same sources (`tools/mono64/build-classlibs.sh`; Cuphead, [mono-arm64.md](mono-arm64.md#a-net-35-game-on-the-4x-class-libraries-cuphead)), not yet through `port new`. Offered by `port check`, not yet recommended over the other routes | templates in `setup-rs/templates/mono-arm64/`, hook `mono64.py`: the android route's setup with the IL2CPP player and its engine modules (`player donor --managed` from an IL2CPP and a Mono donor) and `android-data --mono64`; the release's `setup/mono64/<line>/` (the package part `mono64`: `runtime/mono64/build.sh` and `tools/mono64/build-mono.sh <line>`, Docker; `--mono64-work`) goes into `lib/` ([mono-arm64.md](mono-arm64.md)) |
| `box64` | a Linux x86-64 build (any backend, any version): the game's own player under box64 | templates in `setup-rs/templates/box64/`, hooks in `route.py`; `tools/box64/build.sh` before `package` ([box64-route.md](box64-route.md)) |
| `apk` | the game's own Android build (an APK with its OBBs, split APKs, `.xapk`, `.apks`, `.aab`) of Unity 5.6 - 6000.6 with GLES: the player inside it, run natively by `ua_host`, its armeabi-v7a player where the handheld has a 32-bit userland, its arm64-v8a player (the 64-bit `ua_host64`) elsewhere; a Mono build (armeabi-v7a only) of 2017.4 - 2022.3 (2023.1 - 6000.2: mixed, docs/il2cpp.md) with `player.il2cpp = true` also on 64-bit-only systems, its code compiled there with IL2CPP for Unity's arm64 IL2CPP player | templates in `setup-rs/templates/apk/`, hooks in `apk.py`: the setup runs `pmu apk` on the player's copy and installs it (no download, no conversion; with `player.il2cpp` on a 64-bit-only system `pmu apk --il2cpp --on-device`: one to a few hours, Wi-Fi; [apk.md](apk.md), "As a port") |


### Both builds in one release: 32-bit where it can, 64-bit where it must

A Mono game whose 32-bit build is the one to run (Hollow Knight: as fast as IL2CPP on the RG34XX-SP, less
memory, nothing compiled at setup) still needs a 64-bit build for firmwares without a 32-bit userland
(TrimUI's, and muOS on them), or PortMaster hides the port there: `armhf` in port.json's `reqs` is a
device capability PortMaster checks (`device_info.txt` adds it only where the system has the 32-bit
loader). Hollow Knight's release (portmaster-hollowknight, 2026-11-09) carries both, the way Cuphead's does:

- **Two setups.** `setup/patchscript` is the android route's; `setup/patchscript64` is the mono-arm64
  template's patchscript (`port new --route mono-arm64` into a scratch folder) with its own work folder
  (`gamedata/.setup64`), its install in `arm64/`, its own ready file (`.setup_done64`), and after
  `setup.env` the 64-bit player (`PLAYER_BACKEND=il2cpp PLAYER_ABI=arm64-v8a`, the hashes of Unity's
  arm64 `libmain.so` and `libunity.so`). The conversion and its questions are the same.
- **The launcher picks** as Cuphead's does: 32-bit where `/lib/ld-linux-armhf.so.3` is there, else
  64-bit; `HK_ROUTE=64` at the top of port.env forces it; a built game or a setup under way keeps its
  route (muOS's 32-bit libraries flipped between launches for Cuphead). The 64-bit build runs
  `./ua_host64 <port>/arm64` from the port folder, so port.env's relative paths, `flipbooks.txt` and
  `uaflipbook.dll` beside the binary serve both, with `UM_PMHWDEC=pmhwdec64/libpmhwdec.so.1` and SDL's
  own audio driver. The 32-bit build's `setup/update` is not run on the 64-bit one.
- **The 32-bit libraries' glibc.** port.json's `min_glibc` is the 64-bit system's (Hollow Knight: 2.30,
  `ua_host64` and Unity's arm64 Mono; PortMaster's pmcheck reads the aarch64 binaries) and says nothing of
  the 32-bit userland, which `ua_host` (built on Debian bookworm armhf) needs at glibc 2.35. Hollow Knight's
  launcher reads it from `/lib/ld-linux-armhf.so.3 --version` (`... stable release version 2.40.`, busybox
  `sed` too) and takes the 64-bit build where it is older; a version it can not read counts as new enough.
  A 32-bit build set up before that check, on such a system, gets a message naming `HK_ROUTE=64` instead
  of a game that closes.
- **The 32-bit GL driver.** The 32-bit libraries can be there without a 32-bit GLES 3 driver: muOS on
  the TrimUI Smart Pro has `/lib/ld-linux-armhf.so.3` (PortMaster's `DEVICE_HAS_ARMHF=Y`, glibc new
  enough) and a 32-bit fbdev libEGL, but no GLES 3 functions in its 32-bit libGLESv2. A tester's log
  of Hollow Knight 2026.11.19 there (2026-11-26): `32-bit build (32-bit libraries: yes)`, `[present]
  GLES 3 functions missing`, `[glrec] no GLES 3 driver`, then Unity's "Your device does not match the
  hardware requirements of this application." dialog, which ua_host does not show: a black screen
  until the exit hotkey. `HK_ROUTE=64` and a new setup are the way out on that release. The 64-bit-only
  releases ("Rules for every port" above) end it; a launcher that keeps a 32-bit route has to check the
  32-bit driver for GLES 3 too, not only the loader. (The 2026.11.19 README thought TrimUI handhelds
  had no 32-bit libraries.)
- **Which build, in order:** `HK_ROUTE` in port.env (with or without `export`), then a finished build's
  mark (`.setup_done`, then `.setup_done64`), only then a setup under way (`gamedata/.setup`, `.setup64`),
  then the libraries. With the work folders first, a stopped 32-bit setup's folder took a finished 64-bit
  build back to the 32-bit setup once a release's new port.env dropped the `HK_ROUTE` line (a review of
  the launcher, 2026-11-14). Once a build is ready, both work folders go.
- **The release adds** the IL2CPP arm64 donor (`player.donors`; `tools/donor/build.sh <editor>
  Il2cppArm64`), `ua_host64`, Unity's Mono of the game's line with the bridge in `setup/mono64/<line>/`
  and libpmhwdec's arm64 build in `pmhwdec64/`, through `[build]` (Hollow Knight's `build64.sh`): 36.4 ->
  59.5 MB zipped. No Rockchip MPP for the 64-bit build: the vendor-kernel Rockchip firmwares (dArkOS,
  ArkOS) have 32-bit libraries, and Knifethrower's MPP patch scripts need a newer Python than the arm64
  build image has.
- **Rehearse** the 64-bit path with `REHEARSE_64ONLY=1 REHEARSE_DONE=.setup_done64 REHEARSE_NET=none`
  and a 1 GB `REHEARSE_MEMINFO`: Hollow Knight's whole setup from GOG's game folder in 14 minutes in the
  arm64 container (2.3 GB installed; the rehearsal container has no 64-bit SDL, so the game itself runs
  on a handheld, or without a screen in the emulator: tools/emu). On an RG34XX-SP with muOS, Greenpath walking and fighting: 45.6 fps, then 49.7 with the arm64
  player's present-sync point (`UA_PRESENT_SYNC`, runtime/host/unityhooks.c), against the 32-bit build's
  50.0; at least 189 MB available. Those runs were without `UA_GLES_POOL`: its arm64 row came after
  (2026-11-12; the game runs with it in the arm64 emulator, `EMU_ARCH=arm64 tools/emu/emu.sh` on the
  install's `arm64` folder, 99.9 % of Greenpath's buffers from the pool), a handheld run with it is
  still to do.

What shipping only the 64-bit build would give up, measured on Hollow Knight ([mono-arm64.md](mono-arm64.md),
"32-bit or 64-bit only", 2026-11-19, muOS, Knulli and the G350): 3.5 - 6 % of the frame rate in a
main-thread-bound scene (RG34XX-SP 52.5 -> 50.9 fps under muOS, 53.9 -> 50.7 under Knulli, BattleXP G350
39.9 -> 37.8) and 45 MB (RG34XX-SP) to 110 MB (G350) of available memory on a 1 GB handheld. Little of it
is Mono's (the 64-bit runtime is faster than Unity's 32-bit libmono): it is Unity's arm64 engine, the
64-bit Mali driver and Mono's heap at twice the pointer size (a census of the live heap finds the same
objects on both builds, each larger), which an IL2CPP 64-bit build pays as well. The one part that was
the route's own, 197,566 empty strings the bridge made new where both real players answer String.Empty
(6 MB of heap), is fixed, and so is the collector's 16-byte granule (8 bytes for 6000.0's runtime, 7 MB of live heap). Compared with the same assets held by both builds the memory gap on the RG34XX-SP was 75 - 80 MB; with 8-byte granules and Unity's allocators at 1 MB blocks (`UA_BOOT_CONFIG`, a port setting, which helps the 32-bit build too) it is about 30 MB ([mono-arm64.md](mono-arm64.md#a-fourth-look-8-byte-granules-the-engine-symbol-by-symbol-the-gpu-clock-thread-placement-2026-11-19)). Where the margin is that thin (a game near its frame rate or its memory limit), keep both
builds; a game with headroom can ship the 64-bit build alone (`reqs` without `armhf`).

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
   | IL2CPP ARMv7 (the 32-bit IL2CPP player: `pmu il2cpp --abi armeabi-v7a`, the `Il2cppArmv7` donor) | 5.97 (-25 %) | 40.5 - 41 fps | 43.0 - 43.2 fps | as Mono's |
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

For a Unity 2017.4 Mono game (32-bit, the `android` route), what Enter the Gungeon's port showed is worth
turning on or looking for (profiling.md has the numbers):
- nothing to turn on for Mono's runtime: ua_host's `UA_MONO_INVOKE` and `UA_MONO_TLS` apply by themselves
  where libmono is the checked 2017.4 armeabi-v7a build (`[mono] invoke cache: on` in the log), +1 - 1.5
  fps in a fight of ~1,200 script callbacks a frame;
- `UA_FUTEX_SPIN=50` in the port's `[runtime]` (exact, +0.5 fps there);
- the frame recorder `UA_GL_REC=2` only where there is swap (it costs 20 - 30 MB; the port's
  post_install hook checks `SwapTotal`); the GL thread (`UA_GL_REC=thread`) did not pay on the H700;
- asmpatch's `unity-object-null` (Unity's null tests inline), and a look at the game's UI: a UI library
  that rebuilds its whole mesh on any change (dfGUI, NGUI) can be rebuilding it every frame because a
  control, often a hidden one, moves by float noise (Gungeon: 2 ms a frame; the port's `df-render-skip`);
  a test build that prints a stack at the UI's invalidation finds the cause in one run;
- asmpatch's `inline` with a list of the game's hottest callers from a profile (`UA_PROF`, pmu-dev profsum's
  managed methods): `List<T>[i]`, Unity's null tests, Mathf and Vector leaves that Mono 2.6's JIT keeps as
  calls are copied into them, exactly (Gungeon: +2.6 % in its busiest fight). The list goes to
  `setup/files/inline.txt` through the port's `[files]` (an assembly patch's `files`), the patch into
  `android_data.asmpatch`; check a new list with `inline-check` on the handheld first
  ([tools/asmpatch/README.md](../tools/asmpatch/README.md#optimizations)). Native translations of hot methods, the
  .NET 4.x runtime and an optimised libmono were measured and do not pay
  ([profiling.md](profiling.md#managed-code-on-unity-2017xs-mono-26-inlining-mono-5-and-native-code-enter-the-gungeon-2026-11-09));
- the game's own lowest graphics options on every handheld, with no setup question
  ([below](#the-games-own-options-lowest-and-the-games-rules-kept); Gungeon's realtime reflections
  alone were a whole extra camera pass a frame).

Four of ua_host's layers are on in every launcher the templates make (port.env or the environment wins;
an empty value turns one off): `UA_JIT_WARM=1`, `UA_GL_ARENA=1`, `UA_GLES_POOL=1` and
`UA_PRESENT_SYNC=end` (runtime.md): each leaves the picture and the play as they are, paid in Hollow
Knight on the RG34XX-SP and the G350, and the emulated rig's corpus (Unity 2017.4 - 2022.3) runs with
all four. Exact runtime switches (`UA_FUTEX_SPIN`, `UA_FAST_CLOCK`, `UA_GL_SAMECLEAR`, `UA_FAST_VERTS`,
`UA_GL_MERGE`) come after the route and gain only where a profile shows their cost (Gungeon: none of
the first three; Hollow Knight: none of the last two). Write the measurements
into the port's testing notes, with the route chosen and why.

Windows builds (Direct3D 11 shaders) take the `android` route as they come: players put the game's
folder (`<Game>.exe` beside `<Game>_Data`) or the zip it came in into `gamedata/`, and `port new`
sets the rest up from the check. The shaders: `convert.shaders.from = ["d3d11"]`, and the release
ships `setup/pmhlslcc` (HLSLcc for aarch64, built in Docker by `ARCH=arm64 tools/hlslcc/build.sh`,
with HLSLcc's licence; the `pmhlslcc?` part of route.json), which the setup gives the converter
(`PMHLSLCC`; docs/shaders.md, "Windows builds"). A Windows build of Unity 2021.2+ also needs the
Unix class libraries, because its own are Windows-only: right after the player, the setup runs
`pmu winbcl --donor <the port's Mono donor>`, which takes the `unityjit-linux` twins from the
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
`pmu winbcl <Game>_Data <dir> [--player <pkg>] --donor <donor.apk>` prints the
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

Native plugins: `pmu port check <game>` lists the libraries the game's code P/Invokes
and what the port gives each: built-in stubs answering as a PC without the service (Steam,
Discord, EOS), a sound engine made of the game's own entry points (Wwise: ua_host's own player plays
the banks the setup converted, `wwise.banks`; the check also reads the version from the desktop
library and the soundbanks), or ARM libraries of the game's exact
version that the porter supplies (FMOD, Wwise for sound; [plugins.md](plugins.md)). android-data repeats it as warnings and writes `plugins.map` for the
runtime; a port config's `plugins` table changes the map.

A route is a folder `setup-rs/templates/<route>/` with a `route.json`:

```json
{
  "title": "what the route is",
  "files": {"@@TITLE@@.sh": "launcher.sh", "@@PORTDIR@@/setup/patchscript": "patchscript", "...": "..."},
  "package": ["ua_host", "pmu", "asmpatch?"],
  "donors": ["player", "mono/armeabi-v7a"],
  "defaults": {"player": {"backend": "il2cpp", "abi": "arm64-v8a"}}
}
```

`files` maps each output path of the port folder to a template, looked up in the route's folder
and then in `setup-rs/templates/common/` (the READMEs, the gamedata note, the exit-hotkey `.ini` for gptokeyb2,
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
pmu port new <name> --data <game> [--config port.toml|.json] [--out DIR] [--route R]
                           [--title T] [--player PKG] [--force]
```

checks the game and writes `<out>` (default `./<name>`):

| file | what |
|---|---|
| `port.toml` | the port's settings (written once, with the values the check found; yours to edit). Run `port new` again to regenerate the rest from it (it is used when it is there, `--config` names another); it then lists what the check suggests that `port.toml` lacks or sets otherwise (e.g. `UA_DPAD_STICK` once asmpatch is built, `[wwise] banks`): a key it lacks is used all the same, a value it has wins Everything else is made again, so edit the sources, never the generated copies: a step written into `<port>/setup/hooks/post_install` is lost at the next `port new` (put it in the hook `[files] hooks` names, e.g. `hooks/options.sh`; Enter the Gungeon's JIT seed step was lost this way once), and the top-level `README.md` is written again from the template (keep a port's own sections where `port.toml` carries them, or restore them by hand after the run) |
| `port.build.json` | the resolved settings, the route and the check's findings (what `package` reads) |
| `<Title>.sh` | the launcher (PortMaster's standard first lines: the `controlfolder` block, `control.txt`, the firmware's `mod_${CFW_NAME}.txt` and `get_controls`; then `$GPTOKEYB2`, `$ESUDO`, `pm_message`, `pm_platform_helper`, `pm_finish`, the log in `log.txt`; first launch: PortMaster's patcher runs the setup). The mod file is the firmware's own part, and comes before `get_controls`: its `pm_platform_helper` (muOS's names the game's process in its `foreground_process` file, ROCKNIX's makes the window full screen under sway), its own `get_controls` (EmuELEC's keeps the player's own button mappings, nuubOS's its pad) and its environment (ROCKNIX: its 32-bit Mesa drivers in `LIBGL_DRIVERS_PATH`; nuubOS: SDL's Wayland video driver); a launcher without it gets only funcs.txt's plain functions |
| `<name>/` | the folder installed as `ports/<name>/`: `setup/patchscript`, `setup/setup.env`, `setup/convert.json`, `setup/questions.lua` and `setup.txt` (with questions), `port.env`, `port.json`, `gameinfo.xml` (the frontend's game list entry), `README.md` (the player's), `gamedata/PUT_GAME_FILES_HERE.txt`, `<name>.ini` (gptokeyb2: the exit hotkey) |
| `README.md` | the port's README in PortMaster's layout: `## Notes` (a thank-you to the game's makers, what to put where, what the route needs), `## Controls` (the `[controls]` table), `## Compile` (the commands that build the release from a portmaster-unity checkout) |
| `testing_thread.txt` | the post for PortMaster's Discord testing channel that goes with the test zip: how to install it, the game's link, the instructions, the controls, the firmware and screen checklist (not in the zip) |
| `package.sh` | the release build (`port package`) |

`pmu port new <name> [--data <game>] [--config port.toml|.json] [--out DIR] [--route R] [--title T]
[--player PKG] [--asmpatch CMD] [--force]` (setup-rs: crates/port, with crates/check's check of the
game for `--data`) writes the folder.

`port new` writes the donors the route needs for each of `game.unity_versions` into `port.toml`
(`player.donors`, as a comment: they follow the route and the versions), `setup/setup.env`
(`DONORS`) and `port.build.json`, and prints them with the command that builds them (section 5).
`--player PKG` (a folder from `pmu player donor <version> PKG --apk <donor.apk>`) puts the
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
| `port.aarch64_only` | false (`AARCH64_ONLY_DEFAULT` in port.py) | true: a release with nothing for armhf. A Mono game made for the android route takes the mono-arm64 route (Unity's arm64 IL2CPP player from the IL2CPP donor, the game's assemblies on Unity's Mono for aarch64; port.toml's armv7 `player` settings and checksums give way); `package` ships the Mono donor without its `lib/` (its engine assemblies kept), leaves out the porter's armhf `build.outputs`, and stops if any EM_ARM file is left; port.json asks for no `armhf`. The apk route keeps only `arm64-v8a` (or `player.il2cpp`). docs/arm64.md, "Shipping aarch64 only" |
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
| `[controls]` | drafted from InControl's default pad bindings (the check's survey), else none; your own table replaces the draft | the game's controls for the READMEs and the testing thread, one row per button in order (`"D-Pad" = "Move"`, `B = "Jump"`), named as the handheld labels them; most PortMaster handhelds have the Nintendo layout (A right, B bottom), and the game sees the bottom button as its gamepad's A. The buttons always work by position (the bottom one jumps and confirms); only the prompts may show the handheld's letters ([input.md](input.md#buttons-by-position): never name the pad as a Nintendo one). Leave out buttons that do nothing; PortMaster's exit hotkey (`Select + Start`) is added |
| `player.backend`, `player.abi` | `mono`, `armeabi-v7a` | the player the setup unpacks from the port's donors; apk route: the APK's backend, and `abi = "auto"` (the APK's armeabi-v7a player where the handheld has a 32-bit userland, else its arm64-v8a one) or `"arm64-v8a"` (the 64-bit one wherever the APK has it) |
| `player.donors` | the route's for each of `game.unity_versions` (section 5) | the donor APKs the release ships in `donor/`: `port new` names the route's (a comment in port.toml) and adds any listed here; `port package --donors DIR` copies them and stops when one is missing |
| `player.abis` | apk route: the APK's ARM ABIs | the APK's players the port runs, and so the runtimes the release ships: `ua_host` for `armeabi-v7a`, `ua_host64` for `arm64-v8a` |
| `player.il2cpp` | apk route: false | true (a Mono build, armeabi-v7a only, of Unity 2017.4 - 2022.3, or 2023.1 - 6000.2 as a mixed build): where the system has no 32-bit userland, the setup compiles the build's code with Unity's IL2CPP on the handheld for Unity's arm64 IL2CPP player (`pmu apk --il2cpp --on-device`: one to a few hours, about 1.6 GB free, Wi-Fi); the release ships `ua_host64` too and port.json no longer asks for the 32-bit userland. The check says when a Mono build can take it |
| `player.sha256` | from `--player` | `{"libmain.so" = "<sha256>", ...}`: the setup checks the player it unpacks from the donors (an older port.toml's `player.url` is ignored: the handheld never downloads Unity's player) |
| `convert` | `steps = ["shaders", "settings", "audio", "quality"]`, `settings.graphics_apis = [11]` | `pmu convert`'s config: each step's `DEFAULTS` (`textures.py`, `settings.py`, `audio.py`, `quality.py`, `shaders/step.py`); `{setup}` in a value is the setup folder on the handheld. `quality` is the handheld quality profile ([quality.md](quality.md)): a cheaper picture on every handheld, the game play the same, with no setup question (`quality.ram_mb` limits it to handhelds with at most that much RAM, for a port measured to keep its frame rate with the game's own picture on bigger ones). `video` (pmu only): VP8 clips taller than `max_height` (720) halved until they are not and encoded again (`mode` auto: where MemTotal is at most `ram_mb`, 1024; `cq_level` 10, `cpu_used` 4), each into its old place in the resource file: a 1080p cinematic is more than an A53 decodes in time, 540p a quarter of it and the same on a 720x480 panel ([rust-setup.md](rust-setup.md#video)) |
| `android_data.asmpatch` | `auto` when the check finds Rewired, the Input System package or PlayMaker, else `off` | `auto`: tools/asmpatch runs on the game's assemblies with its default patches, the android ones and the exact optimizations (each changes only what it finds; the package then ships asmpatch); a list: only those patches; `["auto", "<patch>", ...]`: the default ones and those (opt-in ones such as the `reduce` patches) |
| `android_data.streaming_assets` | `obb`; the check suggests `both` (`loose` above 256 MB) when reads of StreamingAssets reach System.IO | `obb`: StreamingAssets in the OBB, read by Unity's APIs; `both`: also beside the APK as `Game_Data/StreamingAssets/`, and asmpatch's streaming-assets patch gives the reads that reach System.IO that folder; `loose`: only there, every read gets it (docs/android-data.md, section 6). Not `obb` ships asmpatch (with `asmpatch = "off"` it runs only that patch); the il2cpp-arm64 route compiles the patch into the game's IL2CPP build (`pmu il2cpp --streaming-assets`) and packs the same mode |
| `android_data.winbcl` | `auto` | `auto`: after the player, the setup runs `pmu winbcl --donor`: a Windows 2021.2+ build gets the Unix class libraries of its version that the Mono donor carries, any build the facades the player's engine modules reference that it lacks (e.g. `netstandard.dll`), both from the port's Mono donor (nothing downloaded; nothing is read when nothing is missing); `off`: not run |
| `android_data.data_path` | false | true: the reads of `Application.dataPath` that reach System.IO get `Game_Data/` beside the APK, with the data folder's own files (asmpatch's data-path patch; docs/android-data.md, section 6); the check notes such reads |
| `android_data.video_shader`, `swappy`, `name`, `package`, `args` | on, `off`, from the title, `com.pmunity.<name>` | `pmu android-data` options; `args` = more of them. Keep `package` from release to release: it names the PlayerPrefs file |
| `runtime` | `UA_POINTER = "1"` for pointer-driven menus; `UA_PAD_NAME` for Rewired and InControl (the name of one of its Android profiles); `UA_OSK = "done"` where a text field does not matter | ua_host's environment (docs/runtime.md, docs/input.md), written to `port.env`; a variable set before the launcher runs wins |
| `wwise.banks` | the check's: the game's Wwise soundbank folder (relative to the data folder) when its banks are of a version `pmu wwise` reads (128, Wwise 2017.2) | the setup converts those banks once into the port's `wwise/` folder (step 5 below), and the launcher points ua_host's own Wwise player at it (`UA_WWISE_DATA`): the game's sound without Wwise's engine ([wwise-player.md](wwise-player.md)); empty: no conversion, the game runs silent on ua_host's built-in Wwise |
| `fmod.banks` | empty (set it by hand: `StreamingAssets` for a game whose FMOD Studio banks are of format 0x67, FMOD 1.10) | the setup converts those banks once into the port's `fmod/` folder (step 5b), and the launcher points ua_host's FMOD player at it (`UA_FMOD_DATA`): the game's sound without FMOD ([fmod-player.md](fmod-player.md)); empty: the game runs silent on ua_host's built-in FMOD ([plugins.md](plugins.md#fmod)) |
| `runtime_low` | none | the Low picture: more of the environment, set on every handheld unless already set (no setup question offers another picture; `UA_PICTURE=high` set by hand in `port.env` skips it): the game's own options at their lowest and reductions of the picture that leave the game play as it is ([below](#the-low-picture-at-launch-runtime_low)) |
| `runtime_lowmem` | none | more of the environment, set only on handhelds with 1 GB of RAM or less (`MemTotal` at most 1024 MB, read by `port.env` at each launch) unless already set, with either picture: what spares memory there at a price a bigger handheld need not pay; speedups go in `runtime` or `runtime_low`, for every handheld ([below](#small-handhelds-1-gb)) |
| `[[questions]]` | none | asked by PortMaster's patcher before the setup: `id`, `question`, `options = [[value, label], ...]`, `default`; the answer goes to `convert_set` (`pmu convert --set <key>=<answer>`) or to `env` (the game's environment, `answers.env`). The first answer is kept for later runs; `setup.txt` (`<id>=<value>`) is used when PortMaster asks nothing |
| `files.plugins`, `files.managed`, `files.assembly_patches`, `files.hooks`, `files.extra` | none | the porter's own files (paths relative to port.toml), copied into the port: converter plugins (`--plugin`: a JSON file of field edits, which the setup's pmu runs: [plugins.md](plugins.md#converter-plugins-a-ports-own-conversion-steps); a Python file is refused since 2026-11-27), assemblies shipped into the game (`--managed-file`), `{name, command, files}` assembly patches (`--assembly-patch`: a command run on the handheld, `{setup}` = the setup folder, `{managed}` = the game's original Managed folder, `{out}` = where it writes `<name>`; section 10), shell hooks sourced by the setup at `game`, `pre_convert`, `post_convert`, `post_pack`, `post_install` (a `game` hook sets `D`, the data folder; they run in the port folder with `GAMEDIR=.`, so keep the paths they print and pass relative: on some kernels, e.g. Knulli's for the Allwinner A133P, a freed page that starts with an absolute card path can deadlock the card's FUSE driver), extra files for the port folder |
| `files.own` | none | files of the port's source folder that `port new` leaves as they are (paths relative to it, e.g. `"<title>.sh"`, `"<name>/setup/patchscript"`, `"README.md"`): a launcher, setup or README made or trimmed by hand keeps its edits when `port new` runs again (for a new route, `port.aarch64_only`, or a template fix in the files it still writes); the log names each one it left, and `.port_files` keeps them. Hollow Knight's and Blasphemous's launchers, Hollow Knight's setup |
| `build.commands`, `build.outputs`, `build.licenses` | none | for a port with its own compiled code: shell commands `port package` runs in the port folder first (with `PMUNITY`, `PORT_SRC` and `BUILD`, a scratch folder, in the environment), the files they make put into the port folder (`{"setup/files/codepatch" = "{build}/codepatch"}`; one that is a program, an ELF file or a `#!` script, is packed executable, whatever its mode in the build folder), and licences for `licenses/` |

Name a question's choices so they make sense together. A choice between a better and a cheaper
version of something is **High** and **Low** (in brackets what each costs or gives: "High (as on
PC)", "Low (shorter setup)"), never a tier without its pair (Medium and Low with no High) or two
words that do not answer each other (Fast and Sharper). A third choice that picks by the handheld
is "By this handheld". The values stay whatever the setup passes on (`medium`, `fast`); only the
labels are what the player reads. The recommended choice, if there is one, comes first and says so.

The converter's half of the Low picture is the `quality` step's handheld profile
([quality.md](quality.md)): on every handheld, with no question (shadow maps, MSAA, SSAO, per-pixel
lights, a few post effects; never the game play, never anything that would disappear). A porter who
measured that a bigger handheld keeps its frame rate with the game's own picture can set
`quality.ram_mb` in `[convert]`; the setup offers the player no PC picture
([The Low picture at launch](#the-low-picture-at-launch-runtime_low)).

A conversion the setup makes can still be a question (High and Low), since it is built once at
setup: the `textures` step (ASTC instead of BC/DXT: less memory, slightly different pixels, a much
longer setup on the handheld) is such an option; the check says when a game has enough BC/DXT
textures for it to matter. The shaders step's `grab_fetch` is another: blend-mode sprites (an
unnamed `GrabPass` sampled at the sprite's own pixels) read the framebuffer instead of a copy of the
picture made for each of them, which a tiler pays for (Hollow Knight's King's Pass: three copies a
frame); where sprites of one batch overlap, or with Mali's 16-bit fetched colour, the blend differs
slightly ([shaders.md](shaders.md#blend-modes-without-grabpass-copies-grab_fetch-opt-in)). As a question:

```toml
[[questions]]
id = "BLEND_FETCH"
question = "Blend-mode effects (light, glow): High (exactly as on PC), or Low (faster, slightly different blending)?"
options = [["false", "High (as on PC)"], ["always", "Low (faster, slightly different blending)"]]
default = "false"
convert_set = "shaders.grab_fetch"
```

`"always"` reads the framebuffer where the GPU driver has `GL_ARM_shader_framebuffer_fetch` (Mali's
own drivers) or `GL_EXT_shader_framebuffer_fetch`; on a driver with neither the programs do not
compile and Unity 6000.0 uses the original shader (older players: not tried). `true` leaves the
choice to Unity's own check, which in 6000.0 knows only EXT's extension: on Mali's drivers it changes
nothing. Or set it in `[convert.shaders]` (`grab_fetch = "always"`) for every install. The answer
applies when the setup converts the data; changing it later means running the setup again.

### The game's own options: lowest, and the game's rules kept

Every port's setup or launcher writes the game's own lowest graphics and performance options (its
options file, a PlayerPrefs key through `UA_PREFS_DEFAULTS`, its config: wherever the game keeps
them; a file only when the player has none yet), with no setup question: the player can change them
afterwards in the game's own menus, and a hand edit (`UA_PICTURE=high` in `port.env`, or the
game's options file) is the only way the port itself offers back. The rules for choosing them:

- only graphics and performance options: never game play, controls, difficulty or accessibility
  options (aim assist, bindings, text speed, screen shake, subtitles and the like);
- a lower option may change how many random numbers the game draws (fewer effects spawned), so the
  same seed rolls differently afterwards; that is allowed. An option that changes what the game does
  (an object that stops bullets left out, a body removed, a timer, a rule) is not: stop one level
  above it, and say why in the port;
- read the game's code for each option before adopting it (what it switches, what it spawns or
  leaves out, whether it draws random numbers), and probe what it removes on a real floor or level
  where that depends on the game's data;
- raise a default above the lowest only after measuring the frame rate on the handhelds, and say so
  in the port's notes.

The worked example, Enter the Gungeon (`hooks/options.sh` in its port; profiling.md has the
numbers): its options are Lighting (High, Low), Shaders (High, Medium, Low, Very Low), Realtime
Reflections (on, off) and Debris (High, Medium, Low, Very Low). The port writes Lighting Low, Shaders
Very Low, Reflections off and Debris Low. Debris stops at Low: Low only spawns no shell casings and no
wall chips from enemy shots (fewer random rolls) and keeps at most 50 debris pieces lying around,
while Very Low also leaves out the objects the game puts on tables when it builds a floor, some of
which (book piles) stop bullets, and its cap of 0 removes the pushable debris bodies (a stool, a
knife) at once: a change of the game's rules. The game's own Options menu agrees with the rest: its
reset to defaults on a machine with at most 1536 MB of RAM picks Low lighting, Low shaders, Low debris
and reflections off, and it turns reflections off whenever shaders go to Low.

`port check` could list the quality options it finds in a game's code (a proposal, not built: the
options class's enum fields and a settings menu's strings); until then the porter reads them.

### The Low picture at launch (`runtime_low`)

A port has one picture, the Low one, on every handheld whatever its size: the game's own graphics
options at their lowest and the reductions below, never anything of the game play (timing,
physics, collisions, scripts and everything that tells the player something, such as an attack's
warning, a hitbox or a pickup, stay). The setup offers no other picture: players who picked a
heavier one would get the port's worst frame rates and report them as the port's. (The setup's
High / Low questions are only for what the setup builds, such as the texture conversion.) Hollow
Knight showed why it cannot be only for 1 GB handhelds: an RG552 (4 GB, a 1920x1152 screen) got
none of the reductions and drew King's Pass at about 11 fps. It showed it again on 2026-11-12: with
the render size kept to 1 GB handhelds (`UA_RENDER_RAM = "1024"`) and its CPU savers in
`runtime_lowmem`, testers' handhelds with more RAM, often with the same kind of four small cores, ran
slower than 1 GB ones (on KMSDRM drivers also without ua_host's present thread, which needs a render
size); the port now sets them on every handheld. The reductions are set at launch, not built in by
the setup:

```toml
[android_data]
asmpatch = ["auto", "hk-blur"]     # the default patches and the one that reads ASMPATCH_HK_BLUR

[runtime_low]
ASMPATCH_HK_BLUR = "off"           # no background blur (tools/asmpatch/README.md, "Reductions")
UA_RENDER_SIZE = "540x360"         # Unity draws at 540x360 on every handheld ...
UA_RENDER_PIXELS = "560000"        # ... and at most about 960x576 where a player empties it (half a 1080p screen)
UA_PREFS_DEFAULTS = "ShaderQuality=0 VidParticles=0 VidDithering=0 VidNoise=0"   # its own options, lowest
```

`port.env` sets the `runtime_low` variables unless already set; a value set above them in
`port.env` or in the environment wins (`ASMPATCH_HK_BLUR=full`), and `UA_PICTURE=high` there skips
them all. That is for someone who edits `port.env` by hand; the setup and the README's first steps
never offer it. The resolution is such an edit too: `export UA_RENDER_SIZE=1280x768 UA_RENDER_RAM=
UA_RENDER_PIXELS=` at the top of `port.env` draws at that size on any device (the panel's shape kept);
the port's README shows the line. The game's own options go in through `UA_PREFS_DEFAULTS` (docs/runtime.md): they
are the game's defaults until the player changes them in its menus, and what the game stores from
then on is the player's. The render size is the largest single lever on a GPU-bound game;
`UA_RENDER_PIXELS` keeps a big screen from costing many times a small one's GPU time
(docs/runtime.md). What each of Hollow Knight's reductions gains on the RG34XX-SP, and what it costs
the picture: [profiling.md](profiling.md#hollow-knights-blur-and-particles-on-1-gb-handhelds).

### Small handhelds (1 GB)

On handhelds with 1 GB of RAM (the RG34XX-SP and most of Anbernic's H700 family) the four A53 cores
and the memory are short as well. `[runtime_lowmem]` is set only where `MemTotal` is at most 1024 MB,
with either picture. Keep it for what spares memory at a price a bigger handheld need not pay: what
makes the game faster goes in `[runtime]` (in `[runtime_low]` when it changes the picture), for every
handheld. The RAM says little about the cores, and a handheld with 2 GB may have the same four small
ones (an RK3566, say). Hollow Knight kept its CPU savers here until 2026-11-12 (one Unity job worker,
`UA_UNITY_ARGS = "-job-worker-count 1"`; Unity's legacy graphics jobs, `UA_GFX_JOBS = "legacy"`; FMOD
mixing at 24 kHz, `UA_AUDIO_RATE = "24000"`), and testers' bigger handhelds ran slower than 1 GB ones;
they are in `[runtime]` now. Anything that changes the picture belongs in `runtime_low`.

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

`lowmem_block` does the same for the ASTC block footprint: `block` elsewhere, `lowmem_block` on a
1 GB handheld (the setup log names both: `textures: 973 MB of RAM (at most 1024): scale 0.375
(lowmem_scale), block [6, 6] (lowmem_block)`). A 6x6 block is 3.56 bits a texel against 4x4's 8,
so the textures' memory and the GPU's texture reads fall by more than half; the price is a little
detail and a longer texture step: the setup's encoder fits blocks directly only for 4x4 (other
footprints go to astc-encoder, tiered by how far a block's texels are from one line, the bounds
scaled to its texel count): Hollow Knight's texture step 189 s with 4x4, 226 s with 6x6 on four
cores of a Mac (304 s before the tiers; over 284 of its textures the tiers cost 0.02 dB).
Text atlases stay at `sharp_block` (4x4). 8x8 (2 bits a texel) is
visibly blocky in Hollow Knight; testers on Discord found 6x6 good enough, and side by side on the
handheld the two are hard to tell apart.

Where the texture step's time matters at 5x5 or 6x6, `tiers` shortens the search of the blocks left
to astc-encoder: a fifth of the encoder's time at 5x5 `fast` with no lower PSNR on the game it was
measured on ([setup-speed.md](setup-speed.md#search-settings-by-guided-tier-2026-11-25)).

A small soft picture drawn many times its size is the exception: its blocks show. Hollow Knight's light
around the Knight (`light_effect_v02`, RGBA32 128x121, on screen in almost every scene) went to 48x45 in
6x6 blocks, and testers on Discord saw staircase lines and, on a 4:3 3.5" screen, "giant macro blocks"
at its faint edge (2026-11-19). Kept at full size it is within 3 levels of the original on a dark
background in 6x6 as in 4x4; in `sharp_names` (`light_effect`) it keeps its size in 4x4 for 15 KB.
To find such a texture: the name from a tester or a frame, then the original through UnityPy, and
the setup's own `pmu convert <data> <out> --config <a textures-only config> --only <file>` on a copy of
the one file that holds it (24 s for Hollow Knight's `resources.assets` in the arm64 image). The
banding a tester still saw is in the original's 8-bit gradient; whether the game's Dithering or Noise
options (off in the port) hide it is not checked yet.

```toml
[convert.textures]
lowmem_scale = 0.375
lowmem_block = [6, 6]
sharp_names = "SDF|tmpro|Font Texture"
```

Hollow Knight on the RG34XX-SP (Knulli, release settings, 2026-11-12), 4x4 against 6x6 at 0.375:
the textures 311 -> 138 MB in the package; Greenpath 52.3 / 52.4 -> 52.8 / 53.0 fps walking
(steady 53.6 -> 53.5), the game 585 -> 558 MB, Mali's pages 130 -> 114 MB; City of Tears 54.2 ->
53.9 fps walking (51.7 -> 51.3 steady: the same within the runs' spread), the game 623 -> 588 MB,
Mali's pages 132 -> 110 MB, the lowest MemAvailable 277 -> 318 MB. The frame rate does not move:
on the H700 these scenes are limited by the CPU threads, not by texture reads.

Hollow Knight uses neither switch since 2026-11-12: `scale = 0.375` and `block = [6, 6]` on every
handheld, since it draws its world at 540x360 everywhere and the full textures bought a bigger
handheld nothing but a longer setup. Its sound budget stays a 1 GB switch (`lowmem_pcm_budget_mb`): a
memory cut that costs the CPU a little, which a handheld with more RAM need not pay. Its cinematics are
re-encoded wherever no hardware decoder plays them (`[convert.video] mode = "on"`; the default `auto`
re-encodes only on 1 GB handhelds).

Readable textures cost twice: Unity keeps a copy of a texture marked readable (`m_IsReadable`, Read/Write
in the editor) in system memory beside the GPU's, and the textures step leaves such textures as they are
(scripts may read their pixels). Many games mark big atlases readable without ever reading them. When
none of the game's scripts reads or writes a texture's pixels, `readable = "release"` clears the flag at
setup and keeps the pixels as they are (the same picture, no copy in system memory); `"convert"` also
converts them as the others. `readable_keep` (a regular expression on texture names) leaves the ones a
script does use readable. Check first: every call of `Texture2D.GetPixel(s)`, `GetPixels32`,
`GetPixelBilinear`, `GetRawTextureData`, `SetPixel(s)`, `Apply`, `Resize`, `Compress`, `LoadImage`,
`EncodeTo*`, `PackTextures`, `Cursor.SetCursor` in the game's assemblies, and where its texture comes
from: one the game makes (`new Texture2D`) is fine, one loaded from the data (a serialized field, a
bundle) goes into `readable_keep`; a texture used wrongly throws `UnityException: Texture '<name>' is not
readable` in log.txt.

```toml
[convert.textures]
readable = "release"
readable_keep = "^occlusion_layer_source$"   # Enter the Gungeon: OcclusionLayer resizes and fills it
```

Enter the Gungeon: 157 readable textures (180 MB of RGBA32), one written by the game; on the BattleXP
G350 (895 MB) the Keep's load had 43 MB available and was stopped by the memory watchdog, about 105 MB
with release and 182 MB with convert (ASTC 4x4 like the game's other textures: the HUD's pixels the same
by eye, the same frame rate). With 110 MB held by another process, as EmulationStation holds on
dArkOS while a port runs, release reached the watchdog's 50 MB and convert kept 75 MB; the port uses
convert.

Blasphemous: 395 readable textures (361 MB of RGBA32, all but one RGBA32), none whose pixels its code
reads (the calls are on textures it makes itself, in editor-only buttons such as
`ScreenColorPaletteData.Generate`, or in `MapManager`'s reveal masks, whose dictionary nothing fills; Unity
2017.4's particle shape module has no texture yet). Most are in the scene its first level loads before
itself and keeps for the whole session, `Preload_<platform>` (every enemy's execution and death
animations: 402 textures, 487 MB as the data has them, 248 readable, 189 MB): held twice, that load took
the BattleXP G350 from 372 MB available at the menu to the kernel's OOM kill in about 14 s. With `readable =
"convert"` (ASTC 4x4 like the game's other textures, 90 MB): the menu 478 -> 405 MB resident (Unity's own
allocations 92 -> 50 MB), the first level loads and keeps 213 MB available at 647 MB resident. A scene that
a game loads by name to warm a cache is worth reading first: its PreloadData (`sharedassets<n>.assets`,
UnityPy) lists every object it pulls in, by file, with the textures' sizes and readable flags.

Textures a shader reads as data, not as colours, must stay as they are. Blasphemous draws the Penitent,
several bosses and some props with `Maikel/MaikelSpriteMasterShader`, which takes a sprite texel's blue
channel as the x of a palette texture (`_PaletteTex`, the skins) and outputs that palette colour: any
change of a blue value picks another palette entry, and ASTC turned parts of the Penitent magenta and
cyan. Look for `texture(<palette>, <a channel of _MainTex>)` in the game's shaders (UnityPy's
`Shader.export()` of the data's GLSL) and for the materials that use them.

`exact_shaders` (a regular expression on shader names) with `exact_keywords` (keywords the material must
have, all of them) finds the textures those programs sample in the player's data at setup and leaves them as
the game has them; `exact_index = "blue"` (step `exactindex`) then stores those it can prove are drawn only
through them in one byte a texel and rewrites the programs to read it (rust-setup.md, "exact_shaders and
exactindex", has the rules and the render check; the walk's result is kept beside the journal, so a setup
that is stopped and goes on does not walk the data again: setup-speed.md):

```toml
[convert]
steps = ["shaders", "settings", "audio", "quality", "textures", "exactindex", "fmodcatalogs"]
[convert.textures]
exact_shaders = "^Maikel/MaikelSpriteMasterShader"
exact_keywords = "PALETTE_ON"
exact_index = "blue"
exact_index_skip = "^ReplicateSprite$"
```

Blasphemous: 109 textures the palette programs sample (547 MB as RGBA32), 24 of them as R8 (399 -> 100 MB:
the executions as the Penitent draws them, his respawn, lever and chest animations). ASTC 4x4 cannot hold such
textures, whatever the effort: encoded with the blue channel and alpha alone weighted, at astcenc's exhaustive
preset, 9 - 17 % of the opaque texels still change. The Penitent's own animations stay RGBA32: a script
(`ReplicateSprite`) copies his sprite every frame to two renderers whose shaders compare its colour with a
target colour, which one byte cannot give back. On the BattleXP G350 (EmulationStation stopped, perfmax) the
first level: 720 MB resident and 134 MB available (all ASTC, with the magenta: 647 and 213; all 137 of the
first walk as RGBA32: stopped for low memory in the load at 773 MB), 56.6 fps (the same as before), the
Penitent's colours right. With EmulationStation's 100 MB held, as on dArkOS, that is too close: what is left
is the Penitent's RGBA32 sprites (about 75 MB in that level). (They cannot go exactly: below. What went instead, 2026-11-23: the
executions and prayers, "Things a game instantiates at the start", further down.)

Why the Penitent's sprites stay RGBA32 (tried 2026-11-17; do not retry without a proof for every GPU the port
runs on): the sword's stencil shader (`Maikel/StencilMaskSword`) discards a texel when its sampled RGB equals
`_TargetColor` (168, 192, 175)/255, and those bytes are on the sword in 50 of the 52 textures, opaque and
transparent. Whether the original discards them depends on how the GPU turns a byte into a float: ua_host's
`UA_GL_PROBE=unorm8` (runtime.md), run inside a game on the BattleXP G350, found Mali-G31 (libmali r13p0) exact
for all 256 bytes, so there the key matches and the texels are discarded; Mesa llvmpipe is exact for 130 bytes
only (192 among those that are not), so there the key never matches. An R8 texture keeps only the palette
index, so a rewritten shader would have to know "this texel is the key" without its bytes: a decision made at
setup is right on one kind of GPU and wrong on the other, and a byte-identical render on Mesa cannot be had. A
form exact on every GPU needs the key bytes sampled on that GPU (another texture bound to that program) or a
per-GPU choice at run time; neither exists. (2026-11-24: the question does not arise. `ReplicateSprite` copies his
sprite only to `SwordMask` and `SwordEffectRenderer`, inactive in his prefab, and nothing in the data or the code
turns them on: the sword's programs never run. With `exact_index_skip_dormant = true` (rust-setup.md) his sheets go
as R8 like the others: 81 textures instead of 24, 476 -> 119 MB, the render check byte for byte the same.)

Text for every language, loaded in every language: a game may keep the fonts of all its languages
referenced from its text objects (a component that swaps the font by language, a fallback list), and Unity
then loads every one of them with the first scene and again with each scene that has text. Hollow Knight's
first scene's shared assets hold the TextMeshPro atlases of every language, uncompressed (Alpha8; 4096x4096
for both Chinese ones, 2048x2048 for Japanese, Korean and the bold Chinese and Japanese, 55.5 MB with the
Russian ones), and the CJK font files (32 MB): about 87 MB of a 1 GB handheld for text an English player never
sees. The textures step
leaves them as they are (`sharp_names`: text atlases keep their size, and Alpha8 is already a byte a texel).
The census (`UA_DUMP_TEXTURES`, runtime.md) finds them among a scene's biggest textures, and the G350's Mali
driver among its biggest allocations ([profiling.md](profiling.md#where-a-games-memory-goes-on-a-1-gb-handheld-hollow-knight-2026-11-12)).
Two parts keep them out of memory with the same picture
in every language: the setup's `lazy` step leaves them out of every scene's preload list (Unity loads an
asset that is not listed when something uses it), and `UA_UNLOAD_ASSETS` (the same names, runtime.md) gives
back what the first scenes loaded; a game in Chinese loads its atlas again at once and keeps it (one load at
the title). Hollow Knight's City of Tears on the RG34XX-SP: the game 593 -> 480 MB, the Mali driver's pages
109 -> 52 MB, the lowest MemAvailable 308 -> 421 MB; on the G350 617 -> 492 MB and 231 -> 353 MB
([profiling.md](profiling.md#where-a-games-memory-goes-on-a-1-gb-handheld-hollow-knight-2026-11-12)). The unload
alone is undone by the next room's preload, and the lists alone save nothing (the first scene still loads them):
set both, and only for a game whose setup ran the step (Hollow Knight's setup notes `convert=lazy` in its mark and
the launcher reads it). A game set up before gets the step in place from the port's update step (`pmu lazy`,
[rust-setup.md](rust-setup.md#lazy)).
How it was checked before it went on by default (screenshots of the same screens side by side, A the release's
data, B both parts): English, Chinese, Traditional Chinese, Japanese, Korean and Russian saved as the game's
language, each launched straight into it; the title, profile list, inventory, pause and options text the same
(mean difference under 1/255; where it was larger, the two runs had caught the screen at different moments).
Each language's atlas came back once at the title and stayed; the long frames matched. In English, a room
change and a death with its respawn (the shade in the next room) brought nothing back and the slowest frame was
the same load in A and B (3.34 s, 3.37 s). Inputs on the G350 are sometimes lost: its walk and menu shots are
not to be trusted, so the language and death checks are best done on a handheld that keeps them (the RG34XX-SP).
On a test copy whose prefs had lost `GameLangSet` a scripted run waited at the title in A and B alike (the game
is likely asking for a language, as at its first launch): check the prefs before blaming a change.
Missed by that check (2026-11-17, a tester's Korean game on an R36S): the start screen's autosave notice, shown
before the title. Its atlas was given back while the notice used it, and Unity did not load it again: the
notice's letters were white blocks (a texture given back is not always loaded again for a text that already
draws it, whatever the documentation says). Hollow Knight's launcher now keeps every atlas when the game's saved
language (`M2H_lastLanguage` in its PlayerPrefs) is Japanese, Korean, Chinese or Russian, or none is saved yet
(the first launch asks for one): the saving is for the players of the other languages. A player who switches to
one of those languages in the game's options may see blocks until the next launch. Shoot a language check from
the game's first frame, not from the title.

```toml
[convert]
steps = ["shaders", "settings", "audio", "textures", "video", "lazy"]
[convert.lazy]
names = "chinese_body Atlas|chinese_trad_body Atlas|...|NotoSerifCJKsc-Regular|NotoSerifTC-Medium"
[runtime]
UA_UNLOAD_ASSETS = "chinese_body Atlas|chinese_trad_body Atlas|...|NotoSerifCJKsc-Regular|NotoSerifTC-Medium"
```

A startup load that pulls in whole enemies: `Resources.LoadAll` loads everything under a path, and with each
object everything its fields reference. Blasphemous's `FMODAudioManager.Initialize` loads every enemy's sound
catalog (`Resources.LoadAll<FMODAudioCatalog>("FMODCatalogs/Enemies")`: 66 small prefabs), and 59 of them have an
`Owner` field that references the enemy or boss component it belongs to, so the load brings every enemy and boss
with all their sprite sheets (resources.assets holds 972 textures, 2.4 GB as RGBA32, about 600 MB as the port's
ASTC): on the BattleXP G350 (895 MB) the game climbed to 778 MB during startup and ran out of memory before the
title. How it showed: `mem.log` climbing while nothing was on screen, and a `UA_PROF` profile with 90 % of the
stall under that one `LoadAll`; then the decompiled code, for who reads what the load returns. Here the only
reader is each level's `AudioLoader.AddCatalogItems`, which takes, for each enemy type in the level, the first
catalog whose `Owner` has that type. The fix keeps that exactly and changes only when things load: the setup's
`fmodcatalogs` step ([rust-setup.md](rust-setup.md#fmodcatalogs)) writes what the load returns from the player's own
data (the paths in the ResourceManager container's order, which is `LoadAll`'s, each with its owner's class), and
asmpatch's `blasphemous-catalogs` ([tools/asmpatch](../tools/asmpatch/README.md)) makes the startup load empty and
has `AddCatalogItems` walk only the listed catalogs whose owner class is one of the level's types, loading each by
its path then: the same catalogs in the same order. The order matters: three catalogs own the `Fool` enemy
(`dmkfool`, `fool`, `quirce`), and the game takes the first. `ASMPATCH_BLASPHEMOUS_CATALOGS=check` does the
original load as well and compares it with the list entry by entry. How much an enemy holds, estimated from the data
(2026-11-17: each catalog's objects in resources.assets reached from it by references, a raw scan of every
object for pointers into the file; textures counted at one byte a pixel, as the port's ASTC 4x4 and R8 store
them): most enemies 15 - 70 MB, the bosses 140 - 340 MB (Amanecidas, the Pontiff's husk, the snake, Isidora), all
66 catalogs 629 MB with what they share counted once. The RG34XX-SP plays the first level with about 220 MB
available, so a few areas' worth of enemies would use it up if they stayed. The game keeps them: the catalogs a
level registers stay in `FMODAudioManager.RegisterCatalog`'s array for the session, and each keeps its `Owner`.
`Owner` is only a type tag (`AddCatalogItems` reads it as `== null` and `.GetType()`; nothing else reads it, by
IL, by reflection or in the data: the type's name is in no scene or PlayMaker file), so the patch routes those two
reads through a helper that remembers each catalog's owner type when it first sees it, and after the walk sets
`Owner` to null on the catalogs walked: `Level`'s `Resources.UnloadUnusedAssets` can then free an enemy once the
level that had it is gone, and a later level with the same type finds the same catalog by the type remembered.
The game's own `GetType() ==` comparison stays (the list's class names only choose what to load), and the patch
refuses nested or generic enemy classes, two of one full name, and `Owner` named next to a reflection call. The
asmpatch tests run a stand-in of the catalog, manager and loader over seven levels (a revisit after the owners
went, a level whose own catalog is the second of its type, two classes of one name in two namespaces, a respawn,
a level without enemies) as built and as patched: the same catalogs walked and registered, level by level.

```toml
[convert]
steps = ["shaders", "settings", "audio", "quality", "textures", "fmodcatalogs"]
[android_data]
asmpatch = ["auto", "blasphemous-catalogs"]
data_path = true          # the list in Game_Data/ beside the APK, where the patched game reads it
```

Checked on Unity 2017.4's armv7 player (the emulated rig, which has the memory for the original load):
`check` found the game's load and the list the same in all 66 entries (order, the object `Resources.Load` gives
for each path, the owner's runtime type), and the first level (D17Z01S01) walked its one enemy type's two
catalogs in the array's order (`DMKPatrollingFlyingEnemy`, then `PatrollingFlyingEnemy`). On the BattleXP G350
(2026-11-16, release settings, silent): unpatched (and with `check`, which does the original load) the game
reached 778-791 MB during startup and was stopped for low memory before the title; patched, the title at 233 MB
and 60 fps, the main menu 490 MB. The first level's load still runs out (781 MB at its loading screen): a
second memory problem, not this one, still to be found (a census during that load).

The owners let go, on the RG34XX-SP (Knulli, 2026-11-17, 300 s from the title through D17Z01S01 and S02 twice,
silent): `check` found the 66 entries the same and walked the same two catalogs in S01 each time (the second time
after their owners were let go), no owner type different; memory the same as the build that keeps the owners
(rss 687 against 684 MB, 220 against 223 MB available, 58.7 fps both): the first rooms have one enemy type, so
there is nothing there to free yet; the saving shows only after areas with other enemies, which a test reaches
only by playing there (the console is a debug build's, and a save made for it would be made-up game state).

Where the rest of a level's memory goes, and what can go exactly (Blasphemous's first level on the RG34XX-SP,
Knulli, 2026-11-17; the armv7 player's memory behaves as on the G350). The game at about 740 MB resident: the GPU
driver's pages about 445 MB (Mali's `gpu_memory`: textures 370 MB, render textures 21 MB), Mono's heap about 52 MB,
and 179 MB of malloc. Naming the malloc took two tools: `UA_MEM_LEDGER=2` (runtime.md) for the Android libraries'
blocks, and for everything else a measurement-only `LD_PRELOAD` ([tools/analysis/mtrack](../tools/analysis/mtrack/mtrack.c)) that wraps glibc's malloc family (through
`__libc_malloc` and friends) and files each block under the library its return address is in (`/proc/self/maps`
read with raw syscalls; no `dladdr` inside malloc), with the callers inside ua_host symbolized from
`ua_host.debug`. Result: Unity 96.9 MB, Mono 39.0, ua_host 14 - 27 (the video decoder's frames 4.5, the FMOD
player's bank trees 4.5, its Vorbis decoders 2), the Mali driver 13.9, libstdc++/SDL/libc about 1. The ledger's
block-size split and biggest blocks then showed what in them is not the game's own: 24 of Mono's and Unity's
blocks begin with `MZ`, whole copies of the assemblies (15.8 MB) the APK holds unchanged. What went, each exact:

- the assemblies read in place from the APK (`UA_MONO_IMAGE_MAP=1`, runtime.md): anonymous memory 238 -> 222 MB,
  Mono's malloc 39.0 -> 28.3, Unity's 97.8 -> 92.7; the same bytes now file pages the kernel can drop;
- the other languages' fonts (pmu's `lazy` step + `UA_UNLOAD_ASSETS`, as for Hollow Knight above): the Traditional
  Chinese font (`MSJhengHei-cut`, an 8 MB RGBAHalf atlas) and the Chinese, Korean and Japanese TextMeshPro atlases:
  textures 370.3 -> 360.3 MB, Mali's pages -13.9 MB, the lowest MemAvailable 170 -> 185 MB. In English the Korean
  atlas came back in the main menu (its text uses it: the census logs `loaded again after it was given back: in
  use, kept`), as the unload is meant to; the others stayed out through the first level. In Chinese (the
  options file's `textLanguageIndex` 5, saved before launch) the Chinese font and atlas and the Korean atlas came
  back at the title and stayed, the Japanese one stayed out, and the title and save-slot screens were the same
  bytes as without the step;
- the video's render texture without a depth buffer (asmpatch `blasphemous-video-depth`): render textures
  21.1 -> 13.2 MB, Mali's pages -7.1 MB;
- the FMOD player's parsed bank trees freed after reading (ua_host): about 4.5 MB.

What stays and why: Unity's 97 MB is its objects (234,000 blocks of 65 - 256 bytes: 11,950 sprites, 8,991 game
objects, 4,584 text assets) and its allocators' blocks; Mono's 28 MB its metadata (498,000 blocks of 64 bytes or
less) and its memory pools (1,700 blocks of 8 - 16 KB); neither has a copy of anything to drop. The census's
`[dump] unity memory: allocated 76.8 MB` is Unity's own count of the same allocations, less its allocators' slack.
All four together on the release install's data (no video-depth patch in it): the game 747 -> 705 MB resident,
the lowest MemAvailable in the run 186 -> 214 MB, 59.6 fps either way. Mali's `gpu_memory` page count moves by
several MB from run to run with nothing changed: compare the census's texture totals for a texture cut.
The two tools are measurements: the ledger's lock around every allocation and the preload's table cost frame rate
and 16 MB each.

Things a game instantiates at the start that a player sees only now and then (Blasphemous, 2026-11-23, the 64-bit
build on muOS's RG34XX-SP). The census (`UA_DUMP_TEXTURES=req`) of the first level named 360 MB of textures; reading
who holds the big ones (the census's holder lines, then the prefabs in the data) found two that load everything at
once: the Penitent's prefab references every enemy's execution animation (the finishing move on a stunned enemy: 30
textures, 40 MB), and `InventoryManager` instantiates every prayer's prefab when the game starts, so every prayer's
effect sheets load whether or not the player has the prayer (27 textures, 46 MB). Both are drawn only when that
execution or prayer plays. pmu's `lazy` step with those 57 names (port.toml's `[convert.lazy] names`, beside the
fonts) leaves them out of the preload lists, and Unity loads each the first time it is drawn: the same picture, a
load in the frame that first needs it. `pmu lazy` rewrites only the APK's PreloadData and ResourceManager lists (the
OBB stays byte for byte), so an installed game takes it in place. The first level: textures 360 -> 285 MB, the lowest
MemAvailable 146 -> 250 MB; Crisanta's arena, killed before, loads (83 MB at the lowest); the Amanecidas' still runs
out (50 MB). The same prayer script on both builds (four prayers cast, through the console, below): 173 -> 244 MB
available at the lowest, 746 -> 692 MB resident at the most; the prayers' textures loaded at the first cast
(`PrayerMiriamPortal`, `RegresoAPuerto-Prayer`, `penitent_pr12PrayerV3`, `threeAnguishBigBeam 1`,
`lightningBolt_Tileable_fxs`) and PR12's effect drew as before; the longest frame after a cast 120 - 235 ms against
141 - 178 ms (the console's commands and the screenshots alone cost 100 - 300 ms frames, so that is within the
noise). Six of the names still load from other holders (`MiriamPrayerSequence`, `threeAnguishBigBeamBlue`,
`threeAnguishSpearEffect`, both `pope_fireBallVortex`, `flagellant_execution_awareness 1`). Not yet seen on screen: an
execution (a scripted pad did not bring an enemy to the stun; the textures load the same way as the prayers'). On the
BattleXP G350, launched by EmulationStation (which keeps 60 - 110 MB): the first level 59 -> about 110 MB available,
45 - 55 fps; Crisanta's arena is still killed by the kernel (the G350 has about 140 MB less free than muOS in the
same scene, and her arena needs about 170 MB over the first level).

```toml
[convert]
steps = ["shaders", "settings", "audio", "quality", "textures", "exactindex", "fmodcatalogs", "video", "lazy"]
[convert.lazy]
names = "MSJhengHei-cut|...|Bishop_executionTPORecolor|...|RegresoAPuerto-Prayer|...|miriam-fragments-spritesheet"
```

The same step for a boss that comes in variants (2026-11-24): the Amanecidas' arena (`D21Z01S01`-`S04`) preloads
every Amanecida's sprite sheets, four colours and Laudes (`AmanecidasAnimatorInyector.bodyAnimatorsByType`, one
animator controller each; `SetAmanecidaColor` binds the fight's one), 160 textures, 166 MB at a byte a texel. With
them in the lazy list only the bound controller's sheets load (about 25 of the 160 at the fight's start): muOS, the
arena loaded straight from a save, 313 MB of textures and 157 - 171 MB free through a fight with the Bejeweled Arrow
(drawn as before, 55 - 60 fps), a death and its respawn; before, the arena ran out (50 MB). A controller bound to an
active Animator loads every sprite its clips name when it is bound, whatever the lists say: Crisanta's ending (her
execution, 92 MB of sheets in her arena's own shared assets) loads with her arena although the step names it (also
with its override controllers and clips named, tried with a pmu that names those classes): the lists decide what a
scene preloads, not what its live objects load.

Prefabs a scene's script names (2026-11-24): Crisanta's boss script (`CrisantaBehaviour`, twice in her arena's scene)
holds her two ending prefabs in `executionPrefab` and `trueEndingExecutionPrefab`, and Unity loads a scene's live
references with it, the prefab and everything its animators reach. pmu's `lazyref` step (after `lazy`) takes
`<file>:<GameObject name>@<script class>` entries: it clears, in every MonoBehaviour of that script class, the 12-byte
PPtr to the prefab (the script read from `m_Script`, its class from the MonoScript), drops the prefab's closure from
the PreloadData and ResourceManager dependency lists of the files that could name it, and adds a resource-table entry
`pmu-lazy/<name, lower case>` for it (sorted, as Unity reads the table; the prefab's file named through
globalgamemanagers' externals). It plans everything first and leaves a prefab as it is when a check fails (another
object naming it, a reference it cannot read). The game's code then has to load the prefab by that name where it
reads the field: asmpatch's `blasphemous-crisanta-lazy` (tools/asmpatch/README.md). The ResourceManager's own
container, NetworkManager's prefab list (names every prefab, loads none) and PreloadData are not uses. A step that
grows globalgamemanagers fits the setup's convert, not `pmu lazy` in place (that keeps every file's length). muOS:
the arena 346 -> 251 MB of textures; at the ending `ACT_CrisantaExecution(Clone)` spawns, active (its sheets load
when its animation plays).

```toml
[convert]
steps = [..., "lazy", "lazyref"]
[convert.lazyref]
objects = "sharedassets1061.assets:ACT_CrisantaExecution@CrisantaBehaviour|sharedassets1061.assets:ACT_TrueEndingCrisantaExecution@CrisantaBehaviour"
```

A zone kept loaded (2026-11-24): Blasphemous's `LevelManager.StreamingLevels` loads, after each room change, every
room of the current zone and keeps them (up to 26 rooms of 4 scenes: 107 scenes in Crisanta's arena). On the BattleXP
G350 her arena itself loaded and ran (161 MB free) and the game was killed seconds later, while the rest of the zone
streamed in. asmpatch's `blasphemous-near-rooms` keeps only the near rooms (the doors' rooms, the last, the
prie-dieu's): muOS, the arena 64,188 -> 50,929 objects, 133 -> 157 MB free at the lowest, 60 fps. Textures stay
about the same (250 MB): the zone's rooms share most of their art; what they held alone was objects. A room the
player has not walked next to loads when entered (about 6 s on the G350, as entering another zone does).

Art a game keeps referenced everywhere but draws in one place: Blasphemous's persistent UI holds the main menu's
animations, its DLC screens and the two logos in every level (133 textures, 51 MB). The lists cannot help (the menu
draws it before every game), `UA_UNLOAD_ASSETS` neither (given back while the menu still shows it, it comes back at
once and is kept); ua_host's `UA_UNLOAD_OUTSIDE` (runtime.md) gives it back on the way out of the menu scenes and lets
the menu load it again: muOS's first room 263 -> 217 MB of textures, Crisanta's arena 400 -> 349 MB; the menu after a
return from a level the same picture. Unity's active scene names are not the game's level names
(`MainMenu_LOGIC`, `D06Z01S25_LOGIC`): a first run with any list logs the names it sees.

On the BattleXP G350 (EmulationStation launch, 2026-11-24) with both: the first level 145 - 163 MB free (59 before the
lazy list, about 110 with the executions and prayers), Crisanta's zone about 155; her arena's load is still killed by
the kernel (both rooms and her ending's sheets during the load).

What this is not: `UA_UNLOAD_ASSETS` for these names. The fonts are given back because the game never draws them in
another language; an execution or a prayer may come back any time, and a texture given back is not always loaded
again for a renderer that already draws it (Hollow Knight's Korean notice, above). The one profile goes to every
handheld: a cut that is exact on every device, never a lower picture for the 1 GB ones.

Scans of every loaded object at a level's start (Blasphemous, RG34XX-SP, 2026-11-17): a level's first seconds ran at
31 - 40 fps, and each room change's fade-in started with a 200 ms frame. `UA_PROF` over them, the long frames
named by their samples (profsum's parser, frame by frame): the level's start is I2 Localization rescanning at every
scene load (`Resources.FindObjectsOfTypeAll(typeof(LanguageSource))`, 28 ms, while `LevelManager.StreamingLevels`
loads the neighbouring rooms' scenes one after another); the fade-in frame is the game's level-loaded handlers'
`FindObjectsOfType<T>` (each walks every loaded MonoBehaviour, 25 - 45 ms), four of them `MapManager`'s. The other
long frames of a room change are behind the black screen the game holds until its frame rate settles (the new
room's Awake/Start, its enemy pools, and `Resources.UnloadUnusedAssets`, whose walk of the managed heap is a 500 ms
frame), and the `UA_JIT_WARM=2` warm-up takes 10 - 15 ms off the fade-in frame and 110 ms off the first room
change. asmpatch's `blasphemous-i2-sources` skips I2's scan when no `LanguageSource` was made since (every one runs
its constructor; the scan can only add one not in `Sources`) and `blasphemous-map-scan` makes `MapManager`'s four
scans one ([tools/asmpatch](../tools/asmpatch/README.md)), both with a check mode that does the game's scans too and
compares. With `check` on Knulli (title, the first level, S01 - S02 - S01 - S02, and again after switching the text
language to French in Options): 73 I2 scans each run, one of which added a source (the patch would have scanned it too), the other 72
skippable, no difference; `MapManager`'s lists the same as the game's at every level. Patched, the level's start
50 then 59 - 60 fps (31 - 40 for 4 - 5 s before), the fade-in frame 109 - 125 ms (183 - 206 before; what is left is
the camera's and the persistence manager's scans). In play, `blasphemous-skin-idle` stops the hidden Extras menu's
skin previews from rebuilding their meshes at every frame of their idle animation (3.7 UI rebuilds a frame, unseen):
the BattleXP G350's first level 58.4 -> 59.7 fps ([profiling.md](profiling.md#blasphemouss-first-level-on-the-battlexp-g350-2026-11-18)).

```toml
[android_data]
asmpatch = ["auto", "blasphemous-catalogs", "rewired-no-keyboard", "blasphemous-i2-sources", "blasphemous-map-scan", "blasphemous-skin-idle"]
```

### RK3326 handhelds (dArkOS, ArkOS: the R36S family, the BattleXP G350)

The R36S class is PortMaster's most common handheld and the slowest these tools aim at: four Cortex-A35
at up to 1.5 GHz (slower per clock than the H700's A53s), a Mali-G31 through libmali's GBM driver
(SDL's KMSDRM, not fbdev), 640x480, 1 GB of which the kernel reports about 895 MB, and no swap. What a
port needs there, measured on the BattleXP G350 (dArkOS) with Hollow Knight and Enter the Gungeon:

- **The present thread and the GL thread:** `UA_PRESENT_THREAD = "1"` in `[runtime]` (libmali's GBM
  driver waits in each swap for the GPU, so the render thread and the GPU took turns: Hollow Knight's
  Greenpath 17.5 -> 25.1 fps) and `UA_GL_REC = "auto"` (the driver calls on a thread of their own
  wherever the present thread runs: 25 -> 30-33; Enter the Gungeon's fight 29.8 -> 31.1). Both do
  nothing on the fbdev handhelds, so a port can set them everywhere (ROCKNIX's sway too, SDL's Wayland
  driver, since 2026-11-24); `UA_GL_REC=2` (the frame recorder) is for handhelds with swap.
- **Filling the screen:** there is no display-engine scaler (sunxi's, which the H700 handhelds use for
  free). `UA_RENDER_SIZE` still works: on the SDL path the present thread scales Unity's smaller window
  to the panel in one linear blit (without it, two GPU blits before each swap). A 16:9 game shown at
  480x360 fills the 640x480 panel's width (640x360); a game whose own scaling mode draws its effects at
  the output size (Enter the Gungeon's Uniform Scaling) costs far more.
- **Memory:** about 700 MB available with EmulationStation running, about 800 with it stopped. dArkOS's
  EmulationStation stays resident (about 100 MB) while it runs a port (`es_systems.cfg`: `sudo perfmax
  %GOVERNOR% %ROM%; nice -n -19 %ROM%; sudo perfnorm`), so a game that needs more than ~650 MB at its
  peak is stopped by the kernel there. The test harnesses stop EmulationStation (`sudo systemctl stop
  emulationstation`); give the game's loads a margin of 100 MB in those tests, or hold that much in
  another process during the run (a stand-in for EmulationStation). Readable textures (above) and the
  textures step are the big levers. Unity 2017.x and older (Mono 2.6): `UA_GC_GROW = "250"` (runtime.md)
  collects when Mono's heap grows in a load with under 250 MB available (Enter the Gungeon's Forge: heap
  264 -> 224 MB, lowest available 82 -> 140 MB).
- **The setup:** it runs in about 20 minutes (Enter the Gungeon: 1157 s with EmulationStation running,
  the tools' peak 331 MB, at least 424 MB available; the textures step's ASTC encoding is half of it,
  and the SoC reaches 80 C there, the CPU at 1.2 GHz).
- **Programs and the JIT at first use** (Unity 2017.4): `UA_GL_PROGCACHE = "1"` (Unity's own program
  binary cache links about half its programs again at every launch: 10 s of Enter the Gungeon's launch)
  and `UA_JIT_WARM = "2"` (the methods a run compiled, compiled ahead at the next launch: a room's first
  entry 16 -> 6 frames over 50 ms on the G350; static constructors stay where the game runs them:
  runtime.md, `UA_JIT_WARM`).
- **Slow loads expose races:** the A35s make a game's loading thread and its main thread overlap far
  more than on a PC. Enter the Gungeon hung at its first load in 3 of 5 launches until asmpatch's
  `playmaker-loadlock` (any game with PlayMaker: add it to `asmpatch`); look for UnityMain at 100 % with
  no frames (`UA_PROF` with `UA_PROF_START`) when a launch stalls.
- **PortMaster:** dArkOS ships it as `/opt/system/Tools/Install.PortMaster.sh` (an offline installer the
  player runs once); `control.txt` gives `DISPLAY_WIDTH`/`HEIGHT` 640x480 and the pad's mapping. The
  G350's pad has two sticks.
- **Measure the average fps** of a walk or fight (the bench's `steady` step never settles here), with
  the SoC cool at the start (`devlock.py cool`): the CPU throttles from 1.5 to 1.0-1.2 GHz near 80 C.
- **Bench at the clocks a player gets:** EmulationStation launches every port through dArkOS's own
  `sudo perfmax %GOVERNOR% %ROM%` and `sudo perfnorm` after it (`es_systems.cfg`; ES-fcamod fills
  `%GOVERNOR%` with its `GlobalPerformanceGovernor` setting, `performance` unless the player changed it):
  CPU, GPU and memory controller on their performance governors (1.5 GHz, 520 MHz, DDR 786 MHz). From a
  shell the device stays on its idle governors, where the memory controller never left 528 MHz and the
  GPU 400 MHz in a fight; Enter the Gungeon's heavy fights ran 5 - 30 % slower there (the Keep's boss
  27.7 -> 36.1 fps, the High Dragun 32.7 -> 34.5, the Forge's busiest room 23.7 -> 27.4, Bullet Hell's
  22.9 -> 25.9: idle governors -> perfmax, EmulationStation stopped). Blasphemous's first level: 50.5 fps
  -> 56.5 - 57.7 (three runs), its intro narration 45.7 -> 47 - 49. A test harness should run the same two
  commands around the game, as ES does: with no terminal (ES's stdin is `/dev/null`). From a terminal
  (`ssh -t`, `script`) both scripts take their non-RK3326 branch and perfnorm leaves the GPU on
  `performance`. A port itself never touches clocks.
- **Probe the GPU only from inside a game run.** A standalone GBM/EGL program on dArkOS's Mali (libmali
  r13p0, `/dev/dri/renderD128`, EmulationStation running) oopsed the kernel (a NULL pointer dereference in
  the driver): the program hung in uninterruptible sleep, every ua_host started after it hung at display
  setup, EmulationStation did not come back, and only a reboot helps. A question about what the GPU computes
  (how it converts a texel, what a shader decides) is asked through ua_host's own display path in a game
  run; tools/analysis/glcmp renders on the PC with Mesa only. Mesa's answer is not the handheld's: for
  Blasphemous's colour key (`Maikel/StencilMaskSword` discards a texel equal to `_TargetColor`, which the
  material stores as 168/255, 192/255, 175/255) llvmpipe discards no texel of exactly those bytes, so its
  sampled 8-bit values are not the nearest floats.
- **Memory hogs in tests take the device down:** a process standing in for EmulationStation's memory must
  give it back before the game's own watchdog stops the game (the harness's `hog.py` exits under 60 MB
  available; the watchdog stops the game under 50), and never in tmpfs (`/dev/shm` is not freed when the
  test ends): a G350 that runs out of memory with no swap needs its power button.
- **EmulationStation stays in memory** while a port runs on dArkOS (it is the process that launched it):
  64 - 126 MB resident on the G350 (895 MB, no swap), more after browsing. PortMaster's dArkOS module does
  not stop it, and a launcher cannot (stopping its service stops the port too). A test over SSH stops ES
  for the display, so it overstates free memory by that much: hold ES's size in RAM for the run instead
  (a file of that size in `/dev/shm`, removed after). Hollow Knight, release settings, so measured
  (2026-11-09): Greenpath at least 84 MB available, City of Tears down to 34 MB at 148 s (the bench's
  40 MB floor stopped it), with libunity's code pages already evicted (19 -> 5 MB resident); without the
  hold the same scenes kept 202 MB. Testers saw 839 of 892 MB used on an RGB10. A 1 GB dArkOS port needs that much more headroom than the stopped-ES numbers show. Hollow Knight's
  fix (2026-11-09): textures at 0.375 instead of 0.5 on 1 GB handhelds (`textures.lowmem_scale`; about a
  texel for each pixel of its 540x360 picture, the same picture side by side) and 16 MiB of a scene's
  sound decoded instead of 36 (`audio.lowmem_pcm_budget_mb`): City of Tears on muOS 663 -> 597 MB, the
  lowest available 266 -> 332 MB, 56.0 -> 56.3 fps. The G350 with 126 MB held had hung in that scene. The
  64-bit only release went back to 36 MiB on 1 GB handhelds (2026-11-21): on the G350 it leaves what the 32-bit
  build left with 16 ([arm64-routes.md](arm64-routes.md), "The sound budget on a 1 GB handheld").

### A pixel-perfect picture on another panel

A game that draws a fixed picture and shows it at the largest whole scale that fits (Blasphemous: 640x360,
its Options' "Pixel perfect", the default; Enter the Gungeon: 480x270) shows it at 1x in the middle of a
720x480 panel, black on all four sides. Fill the width without stretching, as the Gungeon port does: Unity's
window the smallest size of the panel's aspect that holds the picture (`UA_RENDER_SIZE`, computed at each
launch from `DISPLAY_WIDTH`/`DISPLAY_HEIGHT` by a `post_install` hook that writes it into `answers.env`), so
the game keeps its 1x and the display engine (or the GPU in one blit) scales the window to the panel. Mind
ua_host's sizes: whole steps of the panel's aspect, even (720x480: m x 3x2 with m even), so 640x426 becomes
636x424, which no longer holds 640 and makes such a game fall back to its own uneven scaling or, below 1x,
to nothing sensible; take the first step that holds the picture (642x428: the picture 718x404 of 720x480).
Blasphemous on the RG34XX-SP (fbdev, the display engine scales): the menu 59.6 fps (Knulli) and 61.4 (muOS) with it and without. A panel the
picture fills at a whole scale (640x480: 640x360 at 1x) gets none. The game's own scaling option ("Scale",
Options, Video) fills too, by scaling the picture unevenly: a choice for the player, not the port.

### Videos

`port check` lists the game's clips and their codecs; what the port does with them depends on the
codec (the whole table: [runtime/media/README.md](../runtime/media/README.md)). Decoding a video
costs only while it plays: game play frames decode nothing.

- **VP8** (WebM, what Unity's desktop builds usually carry; Hollow Knight's 14 cinematics): `ua_host`
  decodes it on the handheld's video decoder where the port ships libpmhwdec (below; VP8 from its
  commit 27e1bf1), else in software with libvpx. Hollow Knight's 1080p intro on the RG34XX-SP
  (2026-11-08): both play it in real time, the game at 59.5 fps, but on the hardware the four cores
  are 54 % busy instead of 73 %, the lowest second 33 fps instead of 20 and the worst frame 185 ms
  instead of 243. Where libvpx cannot keep up (an RG552 played that 30 s clip in about 45 s), the
  `video` step re-encodes the clips at half size on 1 GB handhelds (`[convert.video]`,
  [rust-setup.md](rust-setup.md#video)); Hollow Knight's then play at their own 30 pictures a second
  on the RG34XX-SP instead of in slow motion.
- **H.264, HEVC and VP8 on the hardware**: ship Knifethrower's
  [libpmhwdec](https://github.com/Knifethrower/pmhwdec) with the port and `ua_host` takes the
  pictures from the handheld's video decoder: the Allwinner H616 / H618 / H700 / A133 engine on
  vendor kernels (Knulli, muOS), Rockchip MPP (ArkOS, dArkOS, AmberELEC), V4L2 on mainline (ROCKNIX).
  `tools/pmhwdec/build.sh <out>` builds it with this repository's images: the library for `ua_host`'s
  architecture (armhf for the android route, arm64 for the arm64 routes) and `pmhwdec-cedard`, the
  Allwinner decoder the library starts from its own folder (aarch64 for both: those handhelds run
  64-bit kernels). Ship them with the port's `[build]` (Hollow Knight's port.toml: the script as a
  command, the two files as outputs in `pmhwdec/`, the three licences) and set `UM_PMHWDEC =
  "pmhwdec/libpmhwdec.so.1"` in `[runtime]`: a path in the port folder, `ua_host`'s working folder
  (`port.env` quotes values, so `$GAMEDIR` would not expand there; the
  launcher's library path holds only the system's folders). The log says what it chose
  (`[media] decoders: ... vp8 (pmhwdec)`, `[media] pmhwdec: VP8 1920x1080 on cedar`). Where its
  start-up self-test fails, or a stream is one the decoder refuses, the software decoders play it as
  before. Rockchip handhelds on vendor kernels (dArkOS, ArkOS) need two more things, both from
  Knifethrower: Rockchip MPP beside the library (`tools/pmhwdec/build.sh` builds
  `armhf/librockchip_mpp.so` with `LICENSE.mpp.txt`, trimmed as his `rkmpp/build_mpp.sh` does), and
  the decoder's nodes, root-only there while ports run as `ark`: the launcher templates open
  `/dev/vpu_service` and `/dev/hevc_service` to the video group (`$ESUDO chgrp video`, `chmod 660`)
  only where the port ships `pmhwdec/` and they are not usable already, and put back the owner, group
  and mode they found when the launcher ends (PortMaster's Moonlight opens the same nodes; the user
  agreed on 2026-11-08, on condition that everything is returned as it was found). G350 (dArkOS,
  RK3326), as `ark`: the self-test passes for H.264, HEVC and VP8 on `rkmpp`, Hollow Knight's intro
  plays on it in the game with the game at 60 fps, and the nodes are `crw------- root root` again
  after. Without the nodes or MPP it reports no hardware in milliseconds and libvpx plays the clips. Ship `pmhwdec-test` too (`pmhwdec/pmhwdec-test`, the same
  build): the setup asks it whether VP8 plays on this handheld's video engine and, where it does,
  leaves the `video` step's clips at full size (`--set video.mode=off`; the update script skips
  `pmu video` the same way): the hardware plays them in time, which is what the smaller clips are
  for, and the setup is about ten minutes shorter on an RG34XX-SP (Knifethrower's idea). Measured by its author with GRIS's 1080p H.264
  intro: on a TrimUI Smart Pro (A133P) 21.1 -> 23.8 pictures a second and the game 42 -> 48 fps; on
  an RG Cube XX (H700) the CPU during the intro 83 -> 56 %. The library is LGPL-2.1-or-later and `pmhwdec-cedard` GPL-2.0-or-later: the port ships their licences and says
  where their source is; this repository never ships them.
- **Clips of 60 pictures a second** (Blasphemous: 15 VP8 clips, 1920x1080 at 60, 201 MB, and one
  H.264 clip in an MP4 that shares a `.resource` file with a VP8 one): too many pictures for the path
  Unity's player takes them by, whatever decodes them. RG34XX-SP, `umbench` alone (Knulli and muOS
  alike): libvpx 0.6 - 0.9 times the clips' speed at full size, 2.1 - 2.3 at 960x540; libpmhwdec
  on the video engine 1.3 - 1.4 at full size, 4.2 at 960x540. In the game (muOS, the 7.5 s boot clip,
  pictures shown counted from `UM_TRACE=1`'s releases): libvpx at full size 113 of 451 (the player
  seeks ahead again and again), the video engine at full size 401 (the render thread 36 % busy taking
  1080p pictures), libvpx at 960x540 430 with the four cores 28 - 42 % busy, the video engine at
  960x540 421 with them 31 - 34 % busy. Such a port sets
  `on_hardware = "smaller"` in `[convert.video]`: the setup and the update make the clips smaller
  where libpmhwdec decodes VP8 too (the default, `"full"`, leaves them as they are there), and the
  video engine then decodes the smaller ones. The step leaves a clip that is not WebM as it is (before
  2026-11-17 Blasphemous's MP4 stopped it with "webm: bad element id"). In the 1 GB arm64 container
  pmu made Blasphemous's 15 clips smaller (24,800 pictures, 201 -> 128 MB) in 81 s on four of the Mac's
  cores; an H700 takes about 63 ms a picture (Hollow Knight's), about 26 minutes of setup.
- **VP9, Opus, others**: the table in runtime/media/README.md says which decoder takes each.

## 4. The setup on the handheld

`setup/patchscript`, run by PortMaster's patcher (log: `ports/<name>/patchlog.txt`; a failure is
a `PATCH_FAIL_MSG:` line the patcher shows):

On muOS the setup first claims muOS's own idle inhibit: it writes its PID to
`/run/muos/idle_game_inhibit`, as muOS's video player and RetroArch launcher do, and removes it at the
end (only if it still holds that PID). Without it, a setup with nobody at the pad is idle time to muOS:
after `idle_display` (120 s) muOS dims the screen and switches to its idle CPU governor (`gov_idle`:
powersave, 288 MHz), and a 26-minute setup took 92 minutes. muOS 2606's `muhotkey` resets its display
and sleep timers every 250 ms while that file names a live process; checked on an RG34XX-SP with
muOS 2606 (2026-11-19): schedutil at 1.2 - 1.32 GHz again within 10 s, held past the timeout, and
powersave 120 s after the file was gone. Nothing of muOS's settings, governors or clocks is changed;
the screen stays at full brightness during the setup. A file naming a dead process (a killed setup) is
taken over, another app's live one left alone; systems without `/run/muos` are not affected. Not
legitimate, and not done: writing `config/system/idle_inhibit` (muOS's `idle.sh` rewrites it every
5 s), faking input activity, changing `idle_display` or `gov_idle`. Ports made before this
(2026-11-19) can copy the block from the templates' patchscripts.

1. the setup's tools: `setup/pmu`, one static aarch64 program (setup-rs, docs/rust-setup.md; until
   2026-11-27 a port whose setup needed what pmu did not do shipped the setup's Python instead:
   `port package` now refuses such a port, `pmu_covers` in crates/port's text.rs);
2. the game: the data folder in `gamedata/` (any depth), else one from an archive there (`.zip`,
   `.7z`, `.rar`, `.tar.*`, with PortMaster's 7-Zip), else macOS installers (`.pkg`, `pmunity
   macpkg`), else the port's `game` hook; a compressed build is unpacked (`pmu unpack`, whenever
   `data.unity3d` is still there: also beside files an unpack that was cut off left, as the player
   would read the archive first; pmu syncs each file under a `.tmp` name before its rename, and the
   folder before the archive goes); its
   Unity version must be one of `game.unity_versions`; an archive that does not unpack whole (a
   damaged `.tar.gz` too: both halves of the unpacking pipe are checked) stops the setup with
   "Could not unpack <name> (see patchlog.txt)", 7-Zip's own error in the log;
3. Unity's Android player of that version, from the port's donors (`<port>/donor/`, section 5):
   `pmu player donor <version> <pkg> --apk donor/donor-<version>-<backend>-<armv7|arm64>.apk`
   (mono-arm64: the IL2CPP donor's player with `--managed`, the engine modules from the Mono donor);
   no donor of the game's version is a plain failure ("This port ships no donor of Unity X"), never a
   download; checksums when the port has them; then `pmu winbcl --donor` (unless
   `android_data.winbcl = "off"`): the class libraries a Windows 2021.2+ build needs and missing
   facades, from the Mono donor (section 2). Nothing on the handheld downloads anything, except the
   il2cpp-arm64 route's IL2CPP compile and the apk route's `player.il2cpp` (Unity's IL2CPP tools and
   static libraries, Debian's compilers, .NET: no donor carries them);
4. `pmu convert` with `setup/convert.json`, the plugins and the answers (a Windows build's
   Direct3D 11 shaders with `setup/pmhlslcc`, a Vulkan-only build's with `setup/pmspirvcross`);
5. a Wwise game (`wwise.banks`): `pmu wwise <data>/<banks> <port>/wwise --wwise-lib <plugins>` turns its soundbanks
   into the data of ua_host's Wwise player ([wwise-player.md](wwise-player.md): Ogg Vorbis and
   ADPCM in a pak, the objects in a json; the Vorbis codebooks read from the game's own Wwise library; Enter the Gungeon's 177 MB in seconds, about 25 MB of
   memory), needing about the banks' size free; banks it cannot read leave the game silent and the
   setup goes on (the step runs only where the release ships pmu);
5b. an FMOD Studio game (`fmod.banks`): `pmu fmod <data>/<banks> <port>/fmod --fmod-lib <plugins>` turns its banks
    into the data of ua_host's FMOD player ([fmod-player.md](fmod-player.md): the samples as Ogg Vorbis, with
    the Vorbis setup headers read from the game's own `libfmod.so`); the launcher sets `UA_FMOD_DATA=<port>/fmod`
    when that folder is there.
6. `pmu android-data --consume-src --pack all`, with asmpatch, the class libraries of step 3,
   the managed files and assembly patches, and `android_data.args` (pmu syncs a file's new copy to
   the card before the file it was made from goes: docs/rust-setup.md, "the power cut");
7. installing: the player's libraries into `lib/` with only the Mono runtime the game needs (with
   both, libunity takes the .NET 4.x one and a .NET 3.5 game aborts), the APK, OBB and Mono's
   config into the port folder (with a GOG build's `goggame-*.info`, the game's own file, beside
   the APK: GOG builds read it at `Application.dataPath + "/../"`, which is that folder; it is
   not part of the release). The end, in this order: the game's copy in `gamedata/` (used up by
   step 6) is removed, `sync`, `.setup_done` marks the game set up, `sync`, and only then the
   tools (`setup/`), the work folder (`gamedata/.setup/`) and the donors (`donor/`) go: a setup
   cut off there leaves a game that starts, never one that is neither set up nor able to set
   itself up, and the launcher removes what was left. The launcher exports
   `UA_WWISE_DATA=<port>/wwise` when that folder is there (an empty `UA_WWISE_DATA` in `port.env`
   turns the player off).

Each step leaves a marker in `gamedata/.setup/` (`mark <step>`: a `sync`, then the marker, so a
marker on the card means the step's files are there too): an interrupted setup (battery, power
button) continues where it stopped on the next launch. Every write of the setup's own state (the
markers, the answers, the notes of where the game is, `answers.env`, box64's `game.env`) is
checked: one that fails stops the setup with "Could not write ... to the card: is the card
full?", and notes that read back empty, or name a folder that is gone, send the setup back to
finding the game. A conversion the kernel stops (out of memory on a 1 GB handheld: status 137)
runs again by itself as long as each run gets further (its journal, `<data>/.pmunity.json`, counted by `pmu journal-files`: the files done), and stops the setup when one gets no further. A card too full to convert or to pack (convert's
status 3, android-data's 4) stops it with "The card is too full to convert / pack the game", and a
journal damaged by a restart (status 5) with how to start again from the game's files. A launcher that finds the game not set up and no `setup/patchscript` (the
folder copied without it, or deleted) says so: "Install the port again (unzip it over your ports
folder)". To set a game up again: unzip the release over the port (a launch of a set-up game has
removed `setup/` and `donor/`), delete `.setup_done`, put the game in `gamedata/`, launch.
Free space: about three times the game while it
runs. Where the time goes on a handheld (textures and the card, mostly): [setup-speed.md](setup-speed.md). The setup needs an aarch64 system for its tools and the game a 32-bit (armhf) userland
(the launcher checks for `/lib/ld-linux-armhf.so.3` first).

The il2cpp-arm64 route's setup takes the same steps with step 3 replaced: `il2cpp --on-device`
(on pmu, as the other steps) compiles the game's code on
the handheld (it also fetches the IL2CPP player), with
the port's managed files, assembly patches (run on the game's original assemblies) and asmpatch,
which gets the opt-in patches `streaming_assets` and `data_path` ask for (`--streaming-assets`,
`--data-path`); step 6 packs with `--il2cpp` and the same `--streaming-assets` / `--data-path`
(android-data checks them against the build's `il2cpp.json`), and nothing of the code goes to
android-data.

The apk route's setup is shorter: the setup's tool (pmu); the players this system runs, in order (the
APK's armeabi-v7a one where `/lib/ld-linux-armhf.so.3` exists and the port has `ua_host`, its
arm64-v8a one where the port has `ua_host64`; `player.abi = "arm64-v8a"` puts that first); the
packages in `gamedata/` (`game.data`, any depth, macOS's `._` files skipped) with the `.obb` files
there, else the APK installed before (a setup again, e.g. after moving the card to a 64-bit-only
handheld), else the `game` hook (it sets `INPUTS`); `pmu apk --target <those players> --json`,
which checks the build and lays it out in the work folder (APKs and OBBs hard-linked, or copied
on exFAT/FAT; what it unpacks from an `.xapk` or rebuilds from an `.aab` moved); the package and the
Unity version checked against `game.package` and `game.unity_versions`; then installing: `lib/`,
`base.apk`, the OBBs under the names the player looks for, Mono's config and
`android_package.json` moved into the port folder, the player's files in `gamedata/` deleted
(`keep_installers=1` keeps them). Free space: about the build's size. The launcher picks `ua_host`
or `ua_host64` by `android_package.json`'s `abi`, and keeps the build's own frame pacing (ua_host
runs imported builds with `UA_FRAME_RATE=android`). With `player.il2cpp` (a Mono build) the
players are `armhf,arm64` (`ua_host64` is in the release), and on a system without the 32-bit
loader the call is `pmu apk --target arm64 --il2cpp --on-device --home <work>/il2cpp-home`: the
build's assemblies compiled for Unity's arm64 IL2CPP player of its version (one to a few hours,
about 1.6 GB more free space, Wi-Fi for the tools; an interrupted build continues from
`<work>/apk/.il2cpp`), its output shown as it goes.

### A game set up by an earlier release

A release brings the port's `setup/` folder (and `donor/`) again, and a setup removes them when it
ends. On every route the launcher of a set-up game removes `setup/`, `donor/` and
`gamedata/.setup/` when it finds them (logged: "removed setup: the setup's, not needed to play"):
a newer release unzipped over an installed game starts the game, and never asks for the setup
again. When the
launcher finds the game set up and `setup/update` there (android route), it runs that once through
PortMaster's patcher first: what this release's setup does that an earlier one did not, done on the
installed files, then `setup/`, the work folder and `donor/` go (the launcher leaves them alone
while `setup/update` is there: an update that did not finish runs again). Now that is asmpatch with `setup.env`'s `ASMPATCH` on the
installed APK's assemblies (`pmu apk-asmpatch`: the patches the game has already say "done", a
newer release's go in; the changed assemblies are written after the APK's end, nothing there is
overwritten, [rust-setup.md](rust-setup.md)), and convert's `video` step when the port's
`convert.json` has it (`pmu video`: the installed APK's and OBB's VP8 clips made smaller in place on
1 GB handhelds, [rust-setup.md](rust-setup.md#video)); the update logs to `patchlog.txt` like the
setup, and a run that is interrupted runs again at the next launch.

A port submitted to PortMaster leaves the update step out: PortMaster-New's instructions for
submissions (its `AGENTS.md`, 2026-11) reject code that migrates installs of earlier test releases.
The Hollow Knight port dropped `setup/update` and its launcher block on 2026-11-18 (portmaster-hollowknight
a1501a3): a newer release unzipped over a set-up game runs it as it was set up, and what a newer setup
does reaches it when it is set up again (the game back in `gamedata/`, `.setup_done` deleted).

## 5. Donors: build Unity's player for the port

**Every port of the `android` and `mono-arm64` routes needs donors, and the porter builds them
before `package`.** A donor is a small game of the port's own (a one-scene game of pong,
`tools/donor/Assets`) built with exactly the game's Unity version; its APK carries that version's
Android player, which the setup unpacks (`pmu player donor`). The release ships the donors in
`<name>/donor/` with `licenses/LICENSE.unity-donor.txt` (Made-with-Unity applications, under Unity's
terms of service), so the handheld never downloads Unity's player: nothing else of Unity is shipped.
Only the setup reads `donor/` (nothing at run time), so the setup removes it from the handheld when
it ends (13 - 23 MB a donor); a later setup needs the release unzipped again.

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
   build-tools 28.0.3, platforms;android-28, JDK 8, NDK r13b; newer editors list theirs as the
   Android module's sub-modules in Unity's release API, e.g. 2021.3.27f1: SDK tools 26.1.1,
   platform-tools 30.0.4, build-tools 30.0.2, platforms android-29 and android-30, OpenJDK 8u172
   (x86-64, also for the Apple-silicon editor), NDK r21d). 2021.3 and Unity 6 want the NDK for a
   Mono donor too ("Android NDK not found").
4. **Each donor:** `tools/donor/build.sh <dir>/Unity.app MonoArmv7 <DONORS dir>` and, for
   `mono-arm64`, `tools/donor/build.sh <dir>/Unity.app Il2cppArm64 <DONORS dir>`: it writes
   `donor-<version>-mono-armv7.apk` / `donor-<version>-il2cpp-arm64.apk`, the names the setup reads.
   **adb:** once the build gives the editor the SDK, Unity scans for devices and kills every
   running adb whose file is not `<SDK>/platform-tools/adb` ("terminated due to being run from
   another SDK"; 2021.3, Unity 6), then starts its own server, so every handheld on adb drops its
   connection mid-run. `build.sh` therefore stops while such an adb runs: build when nobody uses
   adb, or give `SDK` the SDK the running server comes from (`ADB_KILL_OK=1` builds anyway).
   An editor's first launch after `get-editor.sh` can sit for minutes with no CPU and no log while
   macOS verifies the unpacked bundle: let it run (killing and relaunching it starts the check again).
   The editor's project is a temporary folder (`$TMPDIR`; 0.8 - 1.4 GB with its Library and build
   files), removed when the build ends and kept when the editor fails (`build.sh` prints its path;
   its `build.log` is the whole log). Before 2026-11-12 it was never removed: look for
   `$TMPDIR/tmp.*/donor` folders an older `build.sh` left.
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

### A code layout list for a 64-bit player (`UA_RELAYOUT`)

A 64-bit port can lay out its player's hot engine code together at load (runtime.md, `UA_RELAYOUT`;
why: [arm64-routes.md](arm64-routes.md), route 2). The list is per libunity build (its GNU build id;
one per Unity version and ABI) and holds offsets, sizes and weights only, nothing of Unity's: it can
ship with the port or in portmaster-unity beside the runtime. To make one:

1. **Record** the game on a handheld with the 64-bit player, a few scenes of ordinary play, each a
   `UA_PROF` recording of UnityMain and the render thread, plus one of instruction-fetch stalls:
   ```
   DEVICE=... ARCH=arm64 tools/device/profbench.sh <game> pA UA_PROF=500 'UA_PROF_THREADS=UnityMain,UnityGfxDeviceW*'
   DEVICE=... ARCH=arm64 tools/device/profbench.sh <game> eA UA_PROF=2000 UA_PROF_EVENT=raw:0xe1 \
       UA_PROF_EVENT_PERIOD=400000 'UA_PROF_THREADS=UnityMain,UnityGfxDeviceW*'
   ```
   (`raw:0xe1` is the Cortex-A53's `ic_dep_stall`; another scene, another `pB`). Keep one scene out of
   the recordings to test the list on.
2. **Make the list** from the donor's own `libunity.so` (the file the handheld loads; `--as` names it
   as the recordings do when the file is called otherwise):
   ```
   runtime/host/build.sh (ARCH=arm64) builds runtime/host/out/arm64/relayprof (static)
   docker run --rm -v $PWD:/w ua-build-arm64 /w/relayprof make /w/libunity.so /w/<build id>.txt \
       /w/pA/prof-1.txt /w/pB/prof-1.txt /w/eA/prof-1.txt
   ```
   It weighs each recording's samples alike (scaled to a million per thread), counts the functions on
   the sampled stacks too (`--no-callers`: only where samples fell), clusters callers with their
   callees up to a page (`--cluster`), and writes the clusters densest first. `--split` also gives each
   function the 64-byte lines its samples fell in as hot ranges (gaps of up to `--split-gap` lines, 1,
   filled): the loader puts those first and the rest of the function after all hot code. `relayprof check
   <lib>` says how many of the library's functions can move at all (6000.0.61f1 arm64: all 76,082),
   `relayprof plan <lib> <list>` what the loader will build (sizes, islands, unwind data), `relayprof
   refs <lib> <list>` how many of them are reached through vtables (`UA_RELAYOUT_DATA`).
3. **Use it**: put it in `runtime/relayout/` (ua_host finds it in `relayout/` beside itself; `port package`
   ships the folder's lists beside the 64-bit ua_host, a port of its own copies them), or for a test copy it to the handheld and set `UA_RELAYOUT=<file or directory>` (a directory holds
   lists by build id: a list for another libunity is refused, logged, and the game runs as without).
   Check the log for `[relayout] libunity.so: N functions ... laid out`, then A/B the frame rate and
   `HWCOUNT=1`'s i-cache stall cycles on the scene the list was not made from.
4. **ua_host's own hot code** is grouped at build time, not at load: `runtime/host/hot-arm64.txt` lists the functions
   of ua_host (and of the loader, shims and fake JNI linked into it) that the recordings sampled on UnityMain, the render
   thread and ua_host's GL thread; `runtime/host/build.sh` (ARCH=arm64) gives each its own `.text.hot.<name>` section so
   the linker puts them together. To remake it from new recordings of the same ua_host build, with `tools/hotlist`
   (a small Rust crate, no dependencies; it reads the names from ua_host.debug's symbol table):
   `docker run --rm -v $PWD:/w ua-rust sh -c 'cp -r /w/tools/hotlist /tmp/h && cd /tmp/h && cargo build --release -q &&
   ./target/release/hotlist /w/runtime/host/out/arm64/ua_host.debug /w/pA/prof-1.txt /w/eA/prof-1.txt ...' >
   runtime/host/hot-arm64.txt` (one list for every game: the GL layers are the same code; names that no longer exist
   are skipped).

## 6. package

```
pmu port package <port folder> [--out DIR] [--donors DIR] [--runtime DIR] [--ua-host FILE]
                               [--media-licenses DIR] [--pmu DIR] [--asmpatch-bin FILE]
                               [--pmhlslcc-bin FILE] [--pmspirvcross-bin FILE] [--no-build]
```

(or the port's `package.sh`, `DONORS=<dir>`; or `pmu port package` with the same options, the same
zip: docs/rust-setup.md, "The port packager") builds `<out>/<name>.zip` (default `<port>/out/`): the
launcher and the port folder, the donors (`--donors DIR`, else `$DONORS`: each of `player.donors`
into `<name>/donor/`, with `licenses/LICENSE.unity-donor.txt`; a missing one stops the build,
section 5), `ua_host` (`runtime/host/out/`, built with `runtime/build.sh` when missing), the
setup's tool, pmu (`--pmu DIR` with `pmu` and `licenses/`, else
`setup-rs/tools/build_pmu.sh tools/port/out/pmu` builds it in setup-rs's Rust image), asmpatch for linux-arm64 when the port uses it
(`tools/asmpatch/build.sh`), pmhlslcc for linux-arm64 when it translates Direct3D 11 shaders
(`ARCH=arm64 tools/hlslcc/build.sh`, Docker; `--pmhlslcc-bin` another build, with its
`pmhlslcc.LICENSE.txt` beside it; glibc 2.17 symbols only, libstdc++ linked in), pmspirvcross
likewise when it translates Vulkan shaders (`ARCH=arm64 tools/spirvcross/build.sh`;
`--pmspirvcross-bin`), and `licenses/`:
portmaster-unity's licence and THIRD_PARTY.md, libhybris (Apache 2.0, in ua_host), the video
decoders' licences (`runtime/media/out/licenses`), pmu's (what is compiled into it),
for asmpatch the .NET runtime's licence and third-party notices and Mono.Cecil's, for pmhlslcc
HLSLcc's (MIT, with bstrlib's BSD), and for pmspirvcross SPIRV-Cross's (Apache 2.0) and SMOL-V's (MIT). `--runtime` points at another checkout whose runtime and
asmpatch are built. Zip entries are deflated at level 9 and get a fixed date; an entry's Unix mode
goes by rule, never by the file's mode in the folder (on exFAT, where a port on a USB drive lives,
every file is executable): 755 for the programs the packaging names (`ua_host`, `pmu`, asmpatch,
pmhlslcc, pmspirvcross, ...), for ELF files and for a `build.outputs` file that is a program (an
ELF file or a `#!` script); 644 for the rest, the scripts too (the launcher and the setup set the
bits of what they run; PortMaster wants `.sh` files 644). macOS's `._<name>` and `.DS_Store`
files are left out of the zip and of the submission folder, so the same port gives the same zip
from any filesystem. `uaflipbook.dll` goes beside `ua_host` only when the port's settings name
`UA_FLIPBOOK` (`runtime`, `runtime_low`, `runtime_lowmem` or a question's `env`); `package` stops
when such a port's DLL is not built. The il2cpp-arm64 route
ships the 64-bit `ua_host` (`runtime/host/out/arm64/`, `ARCH=arm64 runtime/build.sh`) as
`ua_host`; the apk route ships `ua_host` for an APK's armeabi-v7a player and that 64-bit one as
`ua_host64` for its arm64-v8a player (`player.abis`), each with its build's decoder licences (the
64-bit one's as `LICENSE.<part>.ua_host64.txt`).

The zip also gets the screenshot and the cover (`port.screenshot`, `port.cover`) as
`<name>/screenshot.png` and `<name>/cover.png`, where PortMaster's own release zips have them, and
`testing_thread.txt` goes beside the zip in `<out>/`. A build `package` takes from the checkout
(`setup/pmu` from `tools/port/out/pmu`, `ua_host`, asmpatch, the Mono bridge) that is older than
a file it is made from (a pull after it was built) gets a `warning:` line after the zip's: the
release has it as it was built, so build it again (the line names the command) and package
again. Then `package` lists what a PortMaster
submission still lacks or reviewers would ask about (none of it stops the build): port.toml fields
still empty (porter, description, genres, developer, publisher, release date, the thank-you, the
controls, the game's link), no screenshot or one that is not 4:3 and at least 640x480, no cover, a
`gameinfo.xml` naming an image the port does not ship, an em dash or Markdown in the plain texts,
`exp = true`, a launcher without PortMaster's standard first lines (the firmware's
`mod_${CFW_NAME}.txt` among them; or with `"$GPTOKEYB2"` quoted, a fixed `SDL_VIDEODRIVER`, no
`pm_finish`), files over 90 MB. `--submission` also writes
`<out>/submission/<name>/`, the port as PortMaster's repository holds it (section 12).

## 7. Rehearsal

`tools/port/rehearse.sh <zip> <game folder or archive> <work dir> [emu seconds] [input script]`
unpacks the release into `<work>/roms/ports` as PortMaster would, copies the game into its
`gamedata/` and starts the port's own launcher in an aarch64 container that looks like a handheld
(busybox first on PATH, bash, curl, a 1 GB memory limit, the network on), with a stand-in for PortMaster
(`control.txt`, `utils/patcher.txt`, PortMaster's own `7zzs.aarch64` from its release: `PM_7ZZ`).
`REHEARSE_OVER=<an earlier rehearsal's work dir>` rehearses an update instead: that install's `roms/` is
copied and the zip unpacked over it, as PortMaster updates a port (nothing removed first; pass `-` for the
game), and the launcher runs what it runs on an updated install: its `setup/update` step, a marker
it reads, files a newer launcher expects from an older setup. Rehearse it for every release that changes
the setup: Hollow Knight's `hk-render-size` patch reaches an installed game only through
`setup/update`, and the launcher turns the matching host switch on only when the setup's marker
names the patch (half a minute in the container, against the setup's 20 minutes there).
`ANSWERS="ID=value ..."` answers the port's questions (a GOG `.sh` installer, a `.zip`, `.7z`,
`.pkg` and the other archives are copied into `gamedata/` as they are, for the setup to unpack;
a folder is copied as it is). The container's `/proc/meminfo` shows
Docker's memory, not the 1 GB limit, so a setup that sizes itself from it (`textures` or `video`
`auto`, a `quality.ram_mb`) takes the path of a bigger device: answer such a port's own questions
for the 1 GB one (`ANSWERS="TEXTURES=astc"`).
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
  alone); to see a game's peak before a device run, run the setup's pmu in an arm64 Linux container (tools/port/rehearse.sh, REHEARSE_MEMLOG=1) and read its maximum resident memory (`getrusage`; macOS's
  own figure leaves out compressed memory);
- the game: the pad (the mapping from `get_controls`; `UA_PAD_NAME` for Rewired), pointer mode and
  `UA_KEYMAP` where the menus want a mouse or keys, sound (ALSA, else `ANDROID_SLES_OUTPUT=pipe`
  when a sound server holds the card), video, saves (`ports/<name>/data/`: `external/` is
  `persistentDataPath`, `shared_prefs/` PlayerPrefs; they survive updates; moving them to and from
  a PC: docs/android-data.md, "Saves", and `pmu-dev prefs`), the exit hotkey, the frame rate
  (`UA_FPS=5` in `log.txt`);
- the display path (`UA_DISPLAY`: Mali fbdev or SDL; `[display]` in `log.txt`).

Detaching a run over adb (Knulli): adbd ends the shell's session when its command line ends, and a
`setsid -f` job that is the command line's last command can be killed before it has left the session (a
script whose first line never ran; 2026-11-17, twice). Put another command after it (`setsid -f ./run.sh
> run.out 2>&1 < /dev/null; sleep 1; echo started`) and check that the run started (its log exists)
before waiting for it to end: test.sh and profbench.sh run theirs through `dev_sh`, which appends its
marker line.

## 9. By hand

The same steps without the setup, on a PC:

1. **Check** the version (`pmu version <Data>`), the backend and the graphics APIs
   (`port check` says all of it). The runtime knows the Android players of 5.6 - 6000.6; for
   others `readelf --dyn-syms -W` lists a player's imports (compare with
   `runtime/android/README.md`) and `FJ_DUMP=1` lists the Java classes and methods Unity asked for
   and did not find. The handheld has little RAM (1 GB with the system): the textures step can
   also scale textures down (`textures.scale`), and the audio step keeps the sound a scene holds
   decompressed within `audio.pcm_budget_mb` (16 MiB by default): the game's DecompressOnLoad
   clips are decoded once at load, as on the desktop, and only the largest long clips of a scene
   over the budget play compressed (FMOD then decodes them on the CPU while they play, which
   takes the GPU's clock in a warm handheld: [profiling.md](profiling.md#fmods-mixer-and-the-audio-step-2026-10-24)).
   A port with memory to spare raises it (the step's log names the scene that holds the most);
   `audio.lowmem_pcm_budget_mb` is the budget on handhelds with at most `audio.ram_mb` (1024) of RAM, as
   `textures.lowmem_scale` is the scale there (Hollow Knight: 36 MiB on every handheld since its 64-bit only
   release; its 32-bit build had 16 on 1 GB handhelds).
2. **Convert**: `pmu convert <Data> [<out>] --config game.json` with a config like
   the `convert` table above (`{"steps": ["shaders", "settings", "audio"], "settings":
   {"graphics_apis": [11]}}`); a game's own steps go in a plugin: field edits in a JSON file
   (`--plugin tweaks.json`, which the setup's pmu runs too: [plugins.md](plugins.md#converter-plugins-a-ports-own-conversion-steps));
   a step that needs code goes into pmu (crates/convert's steps, as Cuphead's `animclips`). Check the
   shaders on the device: `pmu-dev shaders-export <converted Data> glsl/`, then
   `runtime/host/out/shadercheck glsl` there (docs/shaders.md).
3. **Package**: `pmu player donor <version> <pkg> --apk <donor.apk>` (section 5), then
   `pmu android-data --src <converted Data> --pkg <pkg> --out android --pack all
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
| hitches | `UA_HITCH_TRACE=50` + `pmu-dev hitchsum`; `UA_GC_HEAP` for Mono's collector |
| slow everywhere (a steady low frame rate) | `UA_PROF=500` + `touch prof.req` + `pmu-dev profsum`; on a handheld, measured runs side by side: `tools/device/profbench.sh` (docs/profiling.md) |
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

A game may also name its own content by the platform it runs on, and the Android player is none of the
platforms the desktop build was made for. Blasphemous loads the scene `"Preload_" + GetPlatformName()`
before its first level, its build holds `Preload_Windows`, `Preload_Linux` and `Preload_OSX`, and on the
Android player it asked for `Preload_Android`: `LevelScene.Load` threw a NullReferenceException and the
new game stayed on its loading screen. asmpatch's `blasphemous-preload` makes that method read the data's
own platform (globalgamemanagers' build target) where it reads Android. Look for such names before the
first level: every read of `Application.platform` and `RuntimePlatform` in the game's assemblies (an
IL scan; the decompiled code shows the switches), and what each picks. Blasphemous's others: the
disclaimer's version line prints `Steam - Android` (left as it is), the rest is demo and library code
the game does not run. The patch reads `globalgamemanagers` beside `Managed`, so it fails (and the
setup stops) when asmpatch runs on a staged copy of the Managed folder, which it does when the port
also ships its own patch of `Assembly-CSharp.dll` (below).

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

## 11. Extending `pmu port`

`pmu port` (setup-rs's crates/port and crates/check) has the route hooks built in (the box64 route's,
il2cpp-arm64's, mono-arm64's and apk's: what route.py, il2cpp.py, mono64.py and apk.py added to the
Python's check, port and release) and runs no outside module: a new hook goes into crates/check
(`port check`'s report), crates/port's `new.rs` (files of the port) or `package.rs` (binaries of the
release and their licences), and a new route is a template folder (section 2; `port new --route
<name>` uses it). A port's own compiled pieces need no hook: port.toml's `[build]` (`commands`,
`outputs`, `licenses`, run by `port package` on the computer). The Python's hook modules
(`$PMUNITY_PORT_HOOKS`) went with it on 2026-11-27; pmu stops when that variable names one.

## 12. Submitting to PortMaster

A port goes into PortMaster through a pull request to its ports repository, after a test zip has
been tried in its Discord testing channel. What PortMaster asks for, and what `pmu port` makes:

| PortMaster wants | made by | the porter's part |
|---|---|---|
| `port.json`, version 4 (`name`, `items`, `attr`: title, porter, desc, inst, genres, `rtr`, `exp`, runtime, store, availability, reqs, arch, min_glibc) | `port new` (`<name>/port.json`; route hooks set Westonpack, `armhf`, glibc) | porter, desc, genres, store in port.toml |
| `gameinfo.xml` (path, name, desc, release date, developer, publisher, genre, image) | `port new` (`<name>/gameinfo.xml`; the image: `./<name>/cover.png` when the port has a cover, else the screenshot) | developer, publisher, release date (look them up) |
| `README.md`: Notes (a thank-you to the game's makers first), Controls (a row per bound button), Compile | `port new` (`README.md`) | `port.thanks`, `[controls]` |
| `screenshot.png`: a real capture of the game running, 4:3, at least 640x480 | `package` ships it and checks its size; `tools/device/drive.sh` takes one | the capture |
| `cover.png` (optional, expected) | `package` ships it | the image |
| the launcher `<Title>.sh` and the folder `<name>/` | `port new`, `package` | explaining every line reviewers ask about |
| `testing_thread.txt`, the testing channel's post | `port new` (beside the port; `package` copies it beside the zip) | the zip's link, the testers' results |
| the repository's layout: `ports/<name>/` with the files above at the top | `package --submission` (`<out>/submission/<name>/`) | the pull request |

`package --submission` lays the port out as the repository holds it: `port.json`, `README.md`,
`gameinfo.xml`, `screenshot.png`, `cover.png` and `<Title>.sh` at the top, `<name>/` beside them
(without the four files PortMaster's build puts back into it: its release zip has them in
`<name>/`, and the README as `<name>/<name>.md`). Copy that folder to `ports/<name>/` in a checkout
of the repository; files over 90 MB are split into pieces there by the repository's own tools.

`package` ends with what is left to do for a submission. One of its lines is about debug info: a
binary in the zip with more than 1 MB of `.debug_*` sections (PortMaster's port check reports the
same as `bin.debug`: "Binaries ship with debug info; strip would save ..."). The runtime's builds
strip what a port ships (`ua_host`: 7.9 -> 2.3 MB for armhf, 10.2 -> 3.1 MB for arm64;
`libil2cpp_mono.so`) and keep the file as linked beside it as `<file>.debug` for the analysis tools,
so the line means an `out/` folder built before that (run `runtime/build.sh` again) or a binary
the port added itself (strip it). Nothing at run time needs the debug info: ua_host names
functions in its logs and profiles from the exported symbols.

The screenshot is a frame of the game as the port runs it, never a made, scaled or cropped
picture. On a handheld, start the game with `tools/device/test.sh` and take it with
`DEVICE=root@<handheld> REMOTE=<the folder> tools/device/drive.sh "" 0 screenshot.png`; for a game
ua_host runs in a folder this machine can reach (the emulated rig's `EMU_ROOT`), `touch
<folder>/shot.req` asks ua_host for the next frame, which it writes there as `shot-<n>.ppm` (any
image tool turns it into a PNG). PortMaster wants 4:3 (its `AGENTS.md`:
640x480; a 640x480 handheld's capture, the BattleXP G350's, needs no crop or scaling): a 3:2
screen's capture (720x480) is what it is, and `package` says so; take it on a 4:3 handheld when one
is at hand.

The pull request's template asks the porter to confirm they understand the launcher and can
explain every line that differs from PortMaster's usual ones. In these launchers that is: the
first-launch setup through PortMaster's patcher (`PATCHER_FILE`, `PATCHER_GAME`, `PATCHER_TIME`,
`PATCHER_QUESTIONS`), the 32-bit userland check and `/lib32:/usr/lib32` first on `LD_LIBRARY_PATH`
(the android route's `ua_host` is an armhf program), `UA_FPS` (a frame-rate line in `log.txt`), `port.env` and
`answers.env` (the game's options and the setup's answers), and for box64 the Westonpack block,
box64's settings and the command written to `conf/run.sh`. Testing on the firmwares and screens
of the testing thread's checklist is the testers' and the porter's work; the rehearsal
(section 7) and `tools/device/test.sh` cover the setup and one handheld.

Read PortMaster-New's `AGENTS.md` before a pull request: reviewers reject AI-made ports that are
more verbose than the ports around them without discussion. What it asked for in 2026-11, and what the
Hollow Knight submission (PortsMaster/PortMaster-New#2880) did about it:

- A launcher about the size of recently merged ports of the same kind (the new ports merged in
  2026-11: 63 - 143 lines): Hollow Knight's went from 206 to 109 lines, short comments, one
  message where there were six.
- No migration code for earlier test releases: the update step (section 4) and the launcher's branches
  for installs of older releases left out.
- `.sh` files at mode 644, one flat `licenses/`, the screenshot at 640x480, one branch per port, fixes
  pushed to the same pull request (never closed and opened again).
- `trap` only for a reason (Hollow Knight's puts the Rockchip video nodes back on exit and still ends
  with `pm_message_end`, which `funcs.txt` set as the EXIT trap).
- No SDL driver defaults (PortMaster's `pmcheck` warns on `SDL_AUDIODRIVER` and the like) unless a
  named device needs one. The android template's `SDL_AUDIODRIVER=alsa` (`setup-rs/templates/android/launcher.sh`) changes nothing
  on the firmwares checked: with it unset, the 32-bit SDL picks ALSA by itself on dArkOS (G350: it plays;
  32-bit libpipewire and libpulse are there, no sound server runs) and on muOS 2606 (32-bit libpipewire
  and its ALSA plugin are there, SDL's PipeWire driver is not; ALSA's open fails either way, and
  the OpenSL shim pipes into `pw-cat`), and on Knulli the same as on muOS (32-bit libpipewire and libpulse
  there, SDL 2.32's drivers for them not available). Hollow Knight's submission dropped it (2026-11-19); a silent
  check is `runtime/host/audiocheck` (one second of zeros) with and without the variable.
- The same question for `/lib32:/usr/lib32` on `LD_LIBRARY_PATH` (pmcheck asks which device needs it):
  Knulli, muOS 2606 (32-bit libraries in `/lib32`, `/usr/lib32`) and dArkOS (Debian multiarch, no `/lib32`)
  find them without it (`/lib/ld-linux-armhf.so.3 --list` on `audiocheck`, 2026-11-19). Hollow Knight's
  launcher keeps it for `ua_host`: 197 merged launchers put `/usr/lib32` first the same way (gmloader
  ports such as Fran Bow), so it is the usual line, and ROCKNIX and AmberELEC were not checked.
  (PortMaster's `control.txt` itself appends `/usr/lib32` on JELOS, UnofficialOS and firmwares with
  `/storage/.config/.OS_ARCH` or no `$ESUDO`, which takes in ROCKNIX and AmberELEC.)
- `PORT_32BIT="Y"` before the mod file, on the 32-bit build (pmcheck 0.2.31 warns without it):
  ROCKNIX's, Knulli's, JELOS's, UnofficialOS's and Batocera's `mod_*.txt` then point PipeWire
  (`SPA_PLUGIN_DIR`, `PIPEWIRE_MODULE_DIR`) at `/usr/lib32`, for the game's own 32-bit client. So a
  launcher with two builds picks its route between `control.txt` and the mod line (Cuphead's has
  since its submission, Hollow Knight's since 2026-11-21). The OpenSL shim's pipe output starts its
  client (the system's 64-bit `pw-cat`, `pacat` or `aplay`) without those two variables (8a94435),
  so the pipe still plays with `PORT_32BIT` set.
- `$DEVICE_HAS_ARMHF` (`device_info.txt`) for the 32-bit userland, with the test for
  `/lib/ld-linux-armhf.so.3` kept: `ua_host` names that loader, and the flag is also `Y` for a
  `/lib32` or `/usr/lib32` folder alone. PortMaster has set it since 2025.
- No gptokeyb2 config when it maps no button: `$GPTOKEYB2 "$HOST" &`. Without `-c` gptokeyb2 makes
  no virtual keyboard and maps nothing, its exit hotkey (Start + hotkey, `state.c`) works the same,
  and its Start + D-pad text entry only comes with `TEXTINPUTINTERACTIVE` (`config_finalise` in
  `config.c`). The templates' `<name>.ini` (`common/hotkey.ini`: an empty `[controls]` against that
  text entry) is not needed.
- No `# PORTMASTER: <zip>, <script>` on line 2: harbourmaster writes it on the handheld at install,
  so it marks a launcher copied back from one. `cd $GAMEDIR`, as in the templates, not
  `cd "$GAMEDIR" || exit 1`. The templates (`setup-rs/templates/*/launcher.sh`) still write the line,
  pass `-c` and leave out `PORT_32BIT`: fix them there before the next submission.
- pmcheck also notes two setup scripts that are 91 % alike (`patchscript`, `patchscript64`); Hollow
  Knight's pull request says why they stay two (different donors, players, folders and flags).
- A message that ends the launch is PortMaster's error card, which stays until it is closed:
  `pm_show_error "<what to do>" --title "Setup needed"`, then `pm_finish` and `exit 1` (merged
  launchers do so, FTL and Rez among them), not `pm_message` with `sleep 8`, which goes by itself.
  `funcs.txt` has taken portmsg's options (`--title`, `--type warning`) since `PM_FUNCS_VERSION` 4;
  older PortMaster shows the text alone.
- Keep the setup's tools and donors (`setup/`, `donor/`) when the game is set up; clear only the
  work folders (`gamedata/.setup*`). A port with two builds sets the other one up later
  (`HK_ROUTE=64` after a 32-bit setup), and the README's way to get a newer setup's changes (the
  game back in `gamedata/`, `.setup_done` deleted) needs them too. With them removed, as Hollow
  Knight's setup and launcher did until 2026-11-22, both ended at "not set up yet" (a reviewer's
  check of the pull request). Hollow Knight keeps 68 MB for it (`setup/` 28, `donor/` 40). A launcher whose setup files are missing (an install from an
  older release) says to install the port again.
- Still to do in the templates (`setup-rs/templates/*/launcher.sh`, `*/patchscript`; `port_golden`
  covers them): `pm_show_error` for the not-set-up and wrong-system messages, the setup's tools and
  donors kept (the launchers' `for d in setup donor gamedata/.setup` and the patchscripts'
  `rm -rf "$W" "$S" "$DONOR_DIR"`), and the game's own letters on muOS's Retro layout (input.md, "Buttons by position").

After `source "$controlfolder/utils/patcher.txt"` the launcher goes back to `$GAMEDIR` before
anything relative (`./ua_host`): the patcher runs in the launcher's own shell, and a working directory
it left elsewhere would keep the game from starting right after the first setup.

To check a cut launcher without a handheld, run it and the released one against stub PortMaster files
(`control.txt` whose `pm_*` functions log, a `utils/patcher.txt` that runs `PATCHER_FILE`, stub
`ua_host`/`ua_host64` that write their arguments, folder and `UA_*`/`ASMPATCH_*`/`SDL_*` environment)
in each state a handheld can be in, and compare the logs: everything that starts the game should match,
line for line.
