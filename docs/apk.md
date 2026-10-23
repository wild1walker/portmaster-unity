# Games owned as Android builds (`pmunity apk`)

A game's own Android build already holds everything Unity's Android player reads: the player
libraries (`libmain.so`, `libunity.so`, `libil2cpp.so` or Mono), the data with GLES shaders and
mobile texture formats, and the manifest. Nothing has to be converted: `pmunity apk` picks the
libraries for the handheld, keeps the APK as it is and writes what `ua_host` needs to start it.
This is also the way to the 64-bit route: Unity ships arm64-v8a players only with IL2CPP, and
IL2CPP Android builds carry them, so a 64-bit-only firmware (ROCKNIX, ...) runs these games with
the aarch64 `ua_host` and its own GLES/SDL2 (docs/arm64.md).

```sh
cd convert
python -m pmunity apk <game>.apk out/                       # an APK (+ <game>.main.obb beside it)
python -m pmunity apk base.apk split_config.arm64_v8a.apk main.12.com.x.obb out/
python -m pmunity apk game.xapk out/                        # .xapk / .apks / a zip of APKs and OBBs
python -m pmunity apk game.aab out/                         # an App Bundle
python -m pmunity apk <game>.apk out/ --dry-run             # the report only
python -m pmunity apk <game>.apk out/ --target armhf,arm64 --json   # the first player the build has; JSON on stdout
```

Then copy `out/` to the handheld and start `ua_host out/` with the build of the chosen ABI:
`runtime/host/out/ua_host` (armhf, armeabi-v7a; `LD_LIBRARY_PATH=/lib32:/usr/lib32`) or
`runtime/host/out/arm64/ua_host` (aarch64, arm64-v8a).

## Mono builds on 64-bit-only systems (`--il2cpp`)

Unity's Android players with Mono are armeabi-v7a only, so a Mono build needs a 32-bit userland.
With `--il2cpp` it runs on a 64-bit one too: its assemblies (`assets/bin/Data/Managed/*.dll`) are
compiled with Unity's IL2CPP (`pmunity il2cpp`, docs/il2cpp.md) for Unity's arm64 IL2CPP player
of the build's version, and `out/` gets that player, the new `libil2cpp.so` and an APK with
IL2CPP's metadata in place of the assemblies:

```sh
python -m pmunity apk <game>.apk out/ --target arm64 --il2cpp --home <IL2CPP tools> [--jobs 3]        # PC (Docker)
python -m pmunity apk <game>.apk out/ --target arm64 --il2cpp --on-device --home <dir>                # an aarch64 handheld
```

It takes what `pmunity il2cpp` takes (`--home`, `--pkg`, `--jobs`, `--on-device`, `--toolchain`,
`--link-xml`, `--strip`, `--keep-cpp`) and acts only when the chosen player is arm64-v8a and the
build has none (a target list that starts with `armhf` on a system with a 32-bit userland keeps
the build's own player). Unity 2017.4.16f1 - 6000.2 (the IL2CPP route's range, 2023.1 - 6000.2 as a
mixed build with 2022.3's il2cpp, not tried with an APK yet; docs/il2cpp.md, "Android builds with
Mono", lists what changes in the layout). The build's own native plugins are
32-bit ARM and left out. The same AOT limits as for desktop games apply (docs/il2cpp.md, "AOT
limits"), but the code is the Android build's own, so its Android code paths are the ones compiled.

## As a port (`pmunity port new --route apk`)

```sh
cd convert
python -m pmunity port check <game>.apk                  # what it is, its ABIs, the apk route
python -m pmunity port new mygame --data <game>.apk      # -> mygame/ with port.toml and the port
mygame/package.sh                                        # -> mygame/out/mygame.zip
REHEARSE_64ONLY=1 PM_7ZZ=<PortMaster's 7zzs.aarch64> ../tools/port/rehearse.sh mygame/out/mygame.zip <game>.apk work/mygame 240
```

`--data` takes what `pmunity apk` takes: an `.apk` (with its `.obb` files beside it), an `.xapk`,
`.apks` or `.aab`, or a folder of split APKs and OBBs. The check reads the build with the dry run
and suggests `game.package` (the setup takes only that package), `game.unity_versions`,
`player.abis` (the APK's ARM players) and, for a Mono build, the pointer mode its code wants
(docs/porting.md, section 1). The port (`port_template/apk/`):

- the release ships `ua_host` (armhf) when the APK has an armeabi-v7a player and `ua_host64` (the
  aarch64 build) when it has an arm64-v8a one: both for a build with both, about 4 MB of the zip;
- the setup (no download, no conversion: a few minutes) takes the player's copy from `gamedata/`
  and runs `pmunity apk --target <the players this system runs>`: the armeabi-v7a one where the
  system has a 32-bit userland (`/lib/ld-linux-armhf.so.3`; the tested route on the RG34XX-SP's
  Mali driver), else the arm64-v8a one; `player.abi = "arm64-v8a"` takes the 64-bit one wherever
  the APK has it. It installs `lib/`, `base.apk`, the OBBs and `android_package.json` into the port
  folder and deletes the player's files from `gamedata/` (docs/porting.md, section 4);
- the launcher runs `ua_host` or `ua_host64` by `android_package.json`'s `abi`, with gptokeyb2 for
  the exit hotkey, SDL's own audio driver and the build's own frame pacing (`UA_FRAME_RATE=android`);
- port.json asks for the 32-bit userland (`reqs = ["armhf"]`) only when the port runs nothing but
  armeabi-v7a; a card set up with the 32-bit player and moved to a 64-bit-only handheld is set up
  again from the installed APK (the launcher says how);
- a Mono build (armeabi-v7a only) with `player.il2cpp = true` in port.toml (the check says when a
  build can take it): the release ships `ua_host64` too, port.json drops the 32-bit userland, and
  on a system without one the setup runs `pmunity apk --il2cpp --on-device`, which compiles the
  build's code on the handheld (one to a few hours on a Cortex-A53, about 1.6 GB free, Wi-Fi for
  Unity's, Debian's and Microsoft's files; an interrupted build continues on the next launch). A
  system with a 32-bit userland still takes the build's own player.

## What it writes

| path | what |
|---|---|
| `base.apk` | the APK, unchanged (an APFS clone, a hard link or a copy; moved when the tool unpacked or rebuilt it itself). Unity reads its data through the package code path (`getPackageCodePath`, `ApplicationInfo.sourceDir`). For split installs whose Unity data sits in an asset-pack split, one APK with the packs' `assets/` merged in; for an App Bundle, one APK rebuilt from the base module and the asset packs |
| `main.<versionCode>.<package>.obb`, `patch....obb` | OBBs under the names the player derives from the manifest (`getObbDir()` + `main.<versionCode>.<package>.obb`): Google Play's names are kept, Unity's split-build output (`<name>.main.obb`) is renamed |
| `lib/*.so` | `lib/<abi>/` of the APK and of its ABI split: the player, plugins (`lib_burst_generated.so`, ...) |
| `assets/bin/Data/Managed/etc/` | Mono builds: Mono's config, which the runtime reads relative to the working directory (as for converted desktop data) |
| `android_package.json` | `package`, `version_code`, `version_name`, `min_sdk`, `target_sdk`, `meta_data` (the manifest's `<application>` meta-data: `unity.build-id`, `unity.splash-mode`, ...), `apk`, the OBB names, and for the report `abi`, `backend`, `unity_version`, `il2cpp_metadata`, `plugins`, `notes`; `"source": "apk"` |

What `ua_host` takes from `android_package.json` (runtime/host/ua_host.c): the package name, the
APK name, `PackageInfo.versionCode`/`versionName` (the OBB names depend on the code),
`ApplicationInfo.metaData` (all of the manifest's meta-data; `unity.build-id` ties the APK to its
OBB: the player mounts an OBB only if it holds an entry `assets/<build-id>`), and the API level.
Frame pacing stays the Android build's own (`UA_FRAME_RATE=android`: 2021.1+ players render a game
that never sets `targetFrameRate` at 30 fps, as on a phone); `UA_FRAME_RATE=desktop` or a number in
the launcher changes that ([runtime.md](runtime.md)).

**API level**: an imported build is told its `targetSdkVersion` (`Build.VERSION.SDK_INT`, the
`ro.build.version.sdk` property, `ApplicationInfo.targetSdkVersion`, AConfiguration): the Android its
makers built and tested it for, at most 33 (Android 13) and at least its `minSdkVersion` (a phone it
installs on has that), its `minSdkVersion` when the manifest names no target. The libandroid shims
then offer what that level has (runtime/android/README.md: AChoreographer from 24, refresh rate
callbacks and `ANativeWindow_setFrameRate` from 30, ...; newer functions read NULL, as on a phone of
that level). `UA_SDK_INT` overrides. Converted desktop data stays at API 19 on armhf (the level the
32-bit ports were made with, Cuphead's among them) and 22 on arm64 (64-bit Android starts at 21),
or the `min_sdk` android-data writes into `android_package.json`: 22 for a game with compute
kernels, because Unity's players only create OpenGL ES 3.1+ contexts from Android 5.0 (API 21)
([android-data.md](android-data.md#2-platform-differences)).
Until AChoreographer existed here, imported builds were held at 23 and below: from 24 Swappy takes
the NDK's choreographer.

## Choices and checks

- **ABI**: arm64-v8a for a 64-bit userland, armeabi-v7a for a 32-bit one. `--target arm64|armhf`
  names the userland, a list (`armhf,arm64`) the userlands the system has in order of preference
  (the first the build has a player for; the port's setup passes it); `--target auto` (default) is
  the machine running the tool when that is an ARM Linux, else arm64-v8a when the build has it.
  `--abi` takes one directly. A build with only armeabi-v7a (every Mono build) needs the 32-bit
  userland.
- **Backend**: `libil2cpp.so` = IL2CPP, `libmono.so` / `libmonobdwgc-2.0.so` = Mono.
- **Unity version**: from the data's header (`globalgamemanagers`, `data.unity3d` or `mainData`,
  in the APK, an asset pack or the OBB); the player's own build line is logged at startup too.
- **IL2CPP metadata**: `assets/bin/Data/Managed/Metadata/global-metadata.dat` must be there
  (libunity extracts it to `<external files>/il2cpp/` at startup); its header gives the metadata
  version (24 for 2018.4/2019.4, 27 for 2020.x, 29 for 2021.x/2022.x, 31 for late 2022.3). A
  file without the usual header is reported: some games encrypt it and their own libil2cpp
  decrypts it, which runs here as on a phone.
- **Graphics APIs** (`BuildSettings.m_GraphicsAPIs`, read with UnityPy): the runtime draws with
  GLES; Vulkan first (the 2021.3+ default) is fine, the player falls back to GLES3 because
  `libvulkan.so` does not load. A Vulkan-only build is refused.
- **Frame pacing**: Optimized Frame Pacing (`androidUseSwappy`, on in many 2020+ projects) is
  reported. ua_host lets Swappy run for imported builds (it clears the loader's `swappy.disable`
  property, `"source": "apk"`): with the setting on and Swappy off, the 2019.4 player presents
  nothing and 2021.3+ stop after their first frames (runtime/android/README.md). At the builds'
  target levels (API 24+) Swappy ticks on the NDK's AChoreographer, on a looper thread it makes;
  below 24 on its Java callback, on a looper thread of its own (runtime/jni, `SwappyChoreo`), as its
  `LooperThread` does on Android. `SO_PROP_swappy_disable=1` turns it off again.

Refused, each with the reason: no `libmain.so`/`libunity.so` for an ARM ABI (not a Unity build, or
the libraries are in an ABI split that was not given), x86-only builds, an ABI the target cannot
run, Vulkan-only builds, IL2CPP without its metadata, Unity data that is not in the inputs (the
OBB or the asset packs a store delivers), Unity versions outside 5.6 - 6000.6
(`--any-unity-version`), VR builds (Oculus/OpenXR/Pico runtimes, a VR launcher category or a
required `android.hardware.vr.*` feature; `--force`), OBBs of another build or package
(`--force` for the build id).

## Inputs

| input | how |
|---|---|
| `.apk` | the base APK; OBBs given too, or found beside it (`<stem>.main.obb`, `*.<package>.obb`) |
| split APKs | the base plus `config.<abi>` (libraries), `config.<density/lang>` (ignored), asset packs (`assets/` merged) |
| `.xapk`, `.apks`, a zip | the APKs and `*.obb` inside (`Android/obb/<package>/...`), unpacked to a temporary folder |
| `.aab` | modules `base` (manifest in protobuf, `assets/`, `lib/`, `root/`) and the asset packs (install-time, fast-follow and on-demand alike: all of them are local here); dex is left out |
| `.obb` | main or patch by name (`main.`/`patch.` or `*.patch.obb`) |

Manifests: binary AXML (APKs; string pools in UTF-8 or UTF-16, attribute names resolved through
the resource map, so stripped or obfuscated names work) and aapt2's protobuf XML (App Bundles).

## What these games need that converted desktop data does not

- Their Java code does not run: `runtime/jni` plays Unity's own `UnityPlayer` side, so plugins
  made of Java (Google Play services, billing, ads, analytics SDKs, Firebase's Java half) see a
  phone without them: calls through `AndroidJavaObject` to their classes fail (logged as
  `UNKNOWN class ...`), which most games survive the way they survive a phone without Play
  services.
- Touch-first games: pads arrive as Android gamepads (Unity's Input Manager / Input System see
  them), touch only through the pointer mode (`UA_POINTER`, docs/input.md).
- Portrait games: Unity is given the handheld's landscape window; `setRequestedOrientation` is
  logged and ignored.
- HTTPS fails certificate checks (as for every game here, docs/compat/README.md).

## Tested (emulated rig)

Free itch.io Android builds, run with `tools/emu` (`EMU_ARCH=arm64`: a native aarch64 container
with Mesa llvmpipe; `EMU_ARCH=armhf`: QEMU armv7); the list and results are in
docs/compat/README.md ("Android builds run"): Unity 2019.4 - 2022.3, Mono (armeabi-v7a) and IL2CPP
(arm64-v8a, and one build's armeabi-v7a libraries on the 32-bit route), Burst, FMOD, GLES2-only,
Vulkan-first and Swappy-on builds all reach their title screens. No free build with an OBB was
found; one was made locally by moving an IL2CPP 2021.3 build's `data.unity3d` into a main OBB with
the 2021.1+ GUID files, as Unity's "Split Application Binary" does: `pmunity apk` found it beside
the APK, named it `main.1.<package>.obb`, and the player mounted it and reached the title screen.

Mono builds through `--il2cpp` (arm64 rig): a 2022.3 card battler (title, then into its first
battle with pointer taps) and a 2019.4 portrait game (title, a run started with touch taps), each
compiled in Docker in 15 - 31 minutes; docs/il2cpp.md, "Android builds with Mono", has the figures.
The 2022.3 one as a port with `player.il2cpp = true`, rehearsed on a 64-bit-only stand-in
(`REHEARSE_64ONLY=1`, an arm64 container with 1 GB + 1 GB swap): the setup compiled it with
`--on-device` in 38 minutes (peak 989 MB, the conversion), installed 106 MB, and the installed game
reached its first battle in the arm64 rig.

## Debugging

- Symbols: `python -m pmunity player fetch <version> <dir> --backend il2cpp --abi arm64-v8a
  --symbols-only` fetches `libunity.sym.so` / `libmain.sym.so` of the build's exact version (a few
  MB of range reads), for `tools/analysis/sym.py` (`UA_SYMS`) and crash reports. They fit Mono
  builds and IL2CPP builds made without engine code stripping; with stripping (the IL2CPP default)
  the build links its own `libunity.so` (another build id: compare the `NT_GNU_BUILD_ID` notes) and
  only its exported names are known.
- `kill -USR2 <pid>` dumps every thread's stack (Mono 4.x games, every Mono build of 2019.4+: `kill -s
  RTMIN+7 <pid>`, 41 with glibc: their Mono takes SIGUSR1/SIGUSR2 for its own thread suspension, and a
  SIGUSR2 on a thread it does not know aborts the game); `UA_PROF_EVERY` / `UA_HITCH_TRACE` sample them
  (runtime/host/hitches.c).
- `SO_TRACE=1` lists unresolved imports of the game's own libraries (plugins).
- `PMUNITY_APK=<file>` runs `tests/test_apk.py`'s dry import on a real build.
