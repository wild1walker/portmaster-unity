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
```

Then copy `out/` to the handheld and start `ua_host out/` with the build of the chosen ABI:
`runtime/host/out/ua_host` (armhf, armeabi-v7a; `LD_LIBRARY_PATH=/lib32:/usr/lib32`) or
`runtime/host/out/arm64/ua_host` (aarch64, arm64-v8a).

## What it writes

| path | what |
|---|---|
| `base.apk` | the APK, unchanged (an APFS clone, a hard link or a copy). Unity reads its data through the package code path (`getPackageCodePath`, `ApplicationInfo.sourceDir`). For split installs whose Unity data sits in an asset-pack split, one APK with the packs' `assets/` merged in; for an App Bundle, one APK rebuilt from the base module and the asset packs |
| `main.<versionCode>.<package>.obb`, `patch....obb` | OBBs under the names the player derives from the manifest (`getObbDir()` + `main.<versionCode>.<package>.obb`): Google Play's names are kept, Unity's split-build output (`<name>.main.obb`) is renamed |
| `lib/*.so` | `lib/<abi>/` of the APK and of its ABI split: the player, plugins (`lib_burst_generated.so`, ...) |
| `assets/bin/Data/Managed/etc/` | Mono builds: Mono's config, which the runtime reads relative to the working directory (as for converted desktop data) |
| `android_package.json` | `package`, `version_code`, `version_name`, `min_sdk`, `target_sdk`, `meta_data` (the manifest's `<application>` meta-data: `unity.build-id`, `unity.splash-mode`, ...), `apk`, the OBB names, and for the report `abi`, `backend`, `unity_version`, `il2cpp_metadata`, `plugins`, `notes`; `"source": "apk"` |

What `ua_host` takes from `android_package.json` (runtime/host/ua_host.c): the package name, the
APK name, `PackageInfo.versionCode`/`versionName` (the OBB names depend on the code),
`ApplicationInfo.metaData` (all of the manifest's meta-data; `unity.build-id` ties the APK to its
OBB: the player mounts an OBB only if it holds an entry `assets/<build-id>`), and
`Build.VERSION.SDK_INT` = at least the build's `minSdkVersion` (19 otherwise, as the ports were
made with; `UA_SDK_INT` overrides).

## Choices and checks

- **ABI**: arm64-v8a for a 64-bit userland, armeabi-v7a for a 32-bit one. `--target arm64|armhf`
  names the userland; `--target auto` (default) is the machine running the tool when that is an
  ARM Linux (a handheld running the setup), else arm64-v8a when the build has it. `--abi` takes one
  directly. A build with only armeabi-v7a (every Mono build) needs the 32-bit userland.
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
  nothing and 2021.3+ stop after their first frames (runtime/android/README.md). Swappy's own Java
  callback ticks on a looper thread of its own (runtime/jni, `SwappyChoreo`), as its
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

## Debugging

- Symbols: `python -m pmunity player fetch <version> <dir> --backend il2cpp --abi arm64-v8a
  --symbols-only` fetches `libunity.sym.so` / `libmain.sym.so` of the build's exact version (a few
  MB of range reads), for `tools/analysis/sym.py` (`UA_SYMS`) and crash reports. They fit Mono
  builds and IL2CPP builds made without engine code stripping; with stripping (the IL2CPP default)
  the build links its own `libunity.so` (another build id: compare the `NT_GNU_BUILD_ID` notes) and
  only its exported names are known.
- `kill -USR2 <pid>` dumps every thread's stack; `UA_PROF_EVERY` / `UA_HITCH_TRACE` sample them
  (runtime/host/hitches.c).
- `SO_TRACE=1` lists unresolved imports of the game's own libraries (plugins).
- `PMUNITY_APK=<file>` runs `tests/test_apk.py`'s dry import on a real build.
