# Unity version coverage: what each LTS Android player needs

The runtime and converter were built for Unity 2017.4.9f1 (Mono, armeabi-v7a; Cuphead). This
survey reads the Android players of the last patch of every LTS line, 2017.4 to 2022.3, and lists
what each one needs that the tools do not have yet. It started as static analysis of Unity's own
files (imports, JNI tables and string references, the Java side, the built-in resources); every
line has since run a free Linux game in the emulated rig ("Players run" below). Per-version details:
[2017.4.40f1](unity-2017.4.40f1.md) · [2018.4.36f1](unity-2018.4.36f1.md) ·
[2019.4.40f1](unity-2019.4.40f1.md) · [2020.3.48f1](unity-2020.3.48f1.md) ·
[2021.3.45f2](unity-2021.3.45f2.md) · [2022.3.62f3](unity-2022.3.62f3.md). The lines around them
(Mono builds of 5.6 - 2017.2 and 2023.1 - 6000.6, "Older and newer lines" below):
[5.6 - 2017.2](unity-5.6-2017.2.md) · [2023.1 - 2023.2](unity-2023.md) · [6000.0 - 6000.6](unity-6000.md).
Every game tested, by name, and how far each got: [games.md](games.md).

## Matrix

✅ works as for 2017.4.9 / provided · ⚠️ works with a caveat or untested path · ❌ missing (breaks
loading, startup, input or data conversion) · "-" not applicable.

| | 2017.4.9 (done) | 2017.4.40 | 2018.4.36 | 2019.4.40 | 2020.3.48 | 2021.3.45f2 | 2022.3.62f3 |
|---|---|---|---|---|---|---|---|
| Mono ABIs | v7a, x86 | v7a, x86 | v7a, x86 | v7a | v7a | v7a | v7a |
| IL2CPP ABIs | v7a, x86 | v7a, **arm64**, x86 | v7a, arm64, x86 | v7a, arm64, x86, x86_64 | same | same | same + armv9 variant |
| Mono runtime | libmono ✅ (+bdwgc) | same ✅ | same ✅ (+PosixHelper) | **bdwgc only** ✅ | ✅ | ✅ +mono-native | ✅ |
| NDK / min API | old / - | old / - | r16b / 16, 21 | r19 / 19, 21 | r19 / 19, 21 | r21d / 22 | r23b / 22 |
| armeabi-v7a code | ARM | ARM | ARM | Thumb-2 | Thumb-2 | Thumb-2 | Thumb-2 |
| relocations, TLS, IFUNC | REL/RELA ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| segment alignment | 4K ✅ | 4K ✅ | 4K, arm64 64K ✅ | 4K, arm64 64K ✅ | 4K, arm64 64K ✅ | 4K ✅ | **16K all** ✅ (4K kernels) |
| DT_NEEDED | ✅ | ✅ | ✅ | ✅ (no GLES/stdc++) | ✅ | ✅ (mono-native → SONAME) | ✅ libmediandk (stubs) |
| imports nobody provides | 0 | 0 | 0 | ✅ 0 (was 4) | ✅ 0 (was 4) | ✅ 0 (was 9) | ✅ 0 (was 9 + 41 libmediandk) |
| imports with ABI risk | - | - | - | ✅ getpwuid_r, nftw, fnmatch, pathconf translated | ✅ + eglSurfaceAttrib mapped | ✅ + wcstod/wcstof/strtold_l (softfp), statvfs | ✅ + `*_l` (glibc locale objects) |
| RegisterNatives (count) | 45 ✅ | 45 | 44 | 47 | 45 | 48 | 48 |
| natives runtime/jni calls | ✅ | ✅ SendMessage(byte[]) | ✅ | ✅ nativeDone()Z | ✅ | ✅ | ✅ nativeInjectEvent(·,int) |
| AndroidJavaProxy | ✅ (int handle) | ✅ | ✅ | ✅ long handle | ✅ newProxyInstance(UnityPlayer,·) | ✅ invocation errors | ✅ |
| Java members bound (count) | 398 | 402 | 458 | 487 | 487 | 487 | 395 |
| of which runtime/jni defines | 250 | 254 | 293 | 312 | 315 | 317 | 310 |
| UnityPlayer Java model | ✅ | ✅ | ✅ | ✅ (callbacks 28/28) | ✅ (27/27) | ✅ (30/30) | ✅ (34/34; GameActivity only from 2023.1) |
| frame pacing (Swappy) | - | - | - | ✅ converted data: off (`swappy.disable`); `androidUseSwappy` must be off (converter default) or nothing is presented; imported builds: on | ✅ (Unity: off below API 23) | ✅ converted: off; with the setting on: stops after 2-4 frames; imported: on | ✅ same |
| Swappy's choreographer | - | - | - | ✅ Java (`ChoreographerCallback`, own looper) below API 24, NDK `AChoreographer` from 24, `SwappyDisplayManager` modes from 28 | ✅ same | ✅ same + refresh rate callback (30+) | ✅ same |
| vsync clock | UnityMain | UnityMain | UnityMain | UnityMain | UnityMain | ✅ own HandlerThread (`FrameTimeTracker`): per-looper Choreographer | ✅ same |
| API level reported | converted data: 19 (arm64: 22) | same | same | same; imported builds: their targetSdk (≤ 33) | same | same (19 is below this player's minimum, 22: works) | same |
| SerializedFile format | 17 ✅ | 17 ✅ | 17 ✅ | 21 ✅ | 22 ✅ | 22 ✅ | 22 ✅ |
| shader serializer (converter) | 201708220 ✅ | 201708220 ✅ | 201802150 ✅ | 201806140 ✅ segments | 201806140 ✅ | 202012090 ✅ param entries | 202012090 ✅ |
| GLCore41/43 programs (converter) | - (output as before) | ⚠️ off by default (`shaders.glcore4x`) | ✅ GLES3 / GLES 3.1 | ✅ | ✅ | ✅ + stripped shaders' empty blobs | ✅ |
| D3D11 programs (Windows builds) | ✅ | ✅ | ✅ | ✅ GL binding info (Unity's 2019 layout) | ✅ + common params | ✅ + param entries | ✅ merged `$Globals` |
| class libraries (Windows builds) | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ unityjit-win32: `pmunity.winbcl` (2021.2+) | ⚠️ same |
| assembly list (converter) | fixed 48 slots ✅ | fixed 48 ✅ | flags (`m_AssemblyTypes`) ✅ | flags ✅ | `ScriptingAssemblies.json` ✅ (2020.2+) | json ✅ | json ✅ |
| texture formats (converter) | ASTC RGB/RGBA ✅ | ✅ | ✅ | `ASTC_nxn` ✅ | ✅ | ✅ | ✅ |
| Android engine modules | 45 | 45 | 59 | 58 | 63 | 64 | 64 |
| OBB matching | build-id ✅ | ✅ | ✅ | ✅ | ✅ | ✅ root `unity_obb_guid` via `ZipFile` | ✅ root `unity_obb_guid` (own zip reader) |
| IL2CPP API looked up | - | 175 | 194 | 222 | 225 | 230 | 234 |
| VideoPlayer (`runtime/media`) | ⚠️ Java path, same code as 2017.4.40 (not run) | ✅ Java path: a game's VP8 WebM intro plays and moves on (emulated rig) | ⚠️ Java path (same bindings; not run) | ⚠️ Java path (+ `IntBuffer`, `KEY_STRIDE`/`KEY_COLOR_FORMAT` bound; not run) | ⚠️ same | ⚠️ same | ⚠️ NDK path (libmediandk over the media core; unit tests only) |
| IL2CPP metadata (header, unverified) | - | 24 | 24 | 24 | 27 | 29 | 29 or 31 |

Notes on the rows:
- "imports nobody provides": undefined dynamic symbols of `libmain`, `libunity` (Mono v7a, IL2CPP
  v7a and arm64) and the Mono libraries that are neither in the runtime's shim tables nor in glibc
  (calls land in the loader's abort stub), counted with `tools/re/shimnames.py` as `playerdiff.py
  --all-missing` does (weak ones left out). Before the bionic additions: 2019.4/2020.3:
  `__FD_SET_chk`, `__FD_ISSET_chk`, `android_set_abort_message`, `__mmap2`; 2021.3 added
  `__strlen_chk`, `__strchr_chk`, `__strrchr_chk`, `__strlcpy_chk`, `sys_signame` (and the FD ones
  on v7a too); 2022.3 the same 9 plus 41 strong `libmediandk` imports (31 functions, 10
  `AMEDIAFORMAT_KEY_*` objects). All are provided now (`loader/hybris_compat.c`,
  `android/media_ndk.c`). Libraries the survey does not cover (a game's `libil2cpp.so`, plugins) can
  still import others: the loader lists them with `SO_TRACE=1`.
- "imports with ABI risk": host-resolved calls whose bionic flag values or 32-bit structures differ
  from glibc's, or that return floating point. Translated in `loader/hybris_compat.c`: `nftw`/`ftw`
  (BSD `FTW_*` flags and callback types, both ABIs), `fnmatch` (`FNM_NOESCAPE`/`FNM_PATHNAME`
  swapped), `pathconf`/`fpathconf` (`_PC_*` numbering), `newlocale` (bionic's locale names; glibc
  objects for the `*_l` calls), `sem_open` (fails with ENOSYS as on Android), and on 32-bit ARM
  `getpw*` (`struct passwd` without `pw_gecos`), `statvfs`/`fstatvfs` (44-byte struct, 32-bit
  counts), `mknod`/`mknodat` (32-bit `dev_t`), the `mbstate_t` string conversions (4-byte state);
  softfp wrappers (`loader/softfp.c`) for `wcstod`, `wcstof`, `strtold_l` and their siblings.
- "natives runtime/jni calls": the natives `runtime/jni` calls, checked against what each
  libunity/libmain registers. runtime/jni looks them up by name and calls them with the registered
  signature (`runtime/jni/README.md`, "Unity versions"); `runtime/jni/test` runs each version's
  native table.
- "of which runtime/jni defines": `tools/re/jnicover.c` on the bound members; the rest are classes
  of no use on a handheld (camera, VR/AR, web/TLS, dialogs, telephony, location) that answer
  0/null (2018.4+: the six TLS members, TrustManagerFactory, X509TrustManager and Certificate, are
  defined since runtime/jni/up_tls.c). Video's members (MediaExtractor, MediaFormat, MediaCodec, SurfaceTexture, ...) are
  defined by `runtime/jni/up_media.c` in a program that links the media core: then 302, 306, 339,
  360, 364, 364, 311 (`runtime/jni/README.md`).
- Converter rows (shaders, assembly list, textures, OBB): what `convert/` does for each version,
  checked on Unity's built-in resources of each line (`tests/test_glcore_corpus.py`: conversion
  equal to Unity's own GLES3 data but for the program text) and on six free Linux games, one per
  line, taken through every step to packed APK/OBB ([shaders](../shaders.md#unity-versions),
  [android-data](../android-data.md#4-unity-versions-20173---20223)). Nothing of it has run in
  a player of these versions. Serializers: 2019.1 - 2021.1 entries have a second (local) keyword
  list; 2019.3+ blobs are segmented (12-byte entry table); 2021.2+ keep parameters as entries of
  their own and subprograms in per-tier lists. GLCore41/43: the GLSL 4.x programs of `#pragma
  target 4.5`+ shaders (URP's error shaders and FSR upscaler, the 4.5 SubShaders of its
  post-processing, particles' procedural instancing, `unity_builtin_extra`'s `BlitToDepth` in
  every game) become GLES3 or GLES 3.1 programs (on by default; `shaders.glcore4x: false` keeps the
  earlier output, GLCore32 programs only, as Cuphead's port does). A shader
  whose variants were all stripped (URP's `StencilDeferred`, `BlitHDROverlay`) gets an empty GLES3
  blob, as in Unity's builds. D3D11 (Windows builds): translated with Unity's HLSLcc for every
  line, the GL programs' own binding info (2019.1+) written as Unity's GL compiler does
  (docs/shaders.md, "Windows builds"); a 2021.2+ Windows build's class libraries are the Windows
  profile's (`unityjit-win32`, kernel32 P/Invokes) and are replaced by the Unix profile's from
  Unity's editor installer (`python -m pmunity.winbcl`). Six free Windows-only games (2018.3 -
  2022.3) reach their first screens in the emulated rig ("Players run" below).
- IL2CPP metadata: the header's integer version (`global-metadata.dat` starts with 0xFAB11BAF
  and the version). These values come from public reverse-engineering tools, not from these
  packages (`libil2cpp.so` and the metadata are built per game), and tools also use sub-versions
  (24.1 for 2018.3/2018.4, 24.2-24.5 across 2019.x, 27.1 for 2020.2.4+, 29.1 for 2022.x; late
  2022.3 patches are reported with 31). Read the header of the game's own file.

## On a handheld (RG34XX-SP: Allwinner H700, Mali-G31, 1 GB, Knulli; 2026-10-20)

The first runs outside the emulated rig, with free games of each line and Cuphead (the games, by
name: [games.md](games.md)). Every game reached its title or first screen and drew it correctly.

| route | Unity, build | result |
|---|---|---|
| 32-bit (armhf `ua_host`, desktop data converted) | Cuphead 2017.4.9 | title 59.6 fps; eight boss fights 59.0 - 59.5 fps average (the 2026-10-18 baseline: 58.5 - 59.5), fewer long frames |
| | 2017.4.40 (its intro video plays), 2018.4, 2019.4, 2020.3, 2021.3 URP | title 59.6 fps |
| | 2022.3 URP | title, a steady 30 fps: from 2021.1 Unity's Android players ignore vSyncCount and render at a fixed 30 fps unless the game sets `targetFrameRate`. ua_host now paces 2021.1+ converted data as the desktop did (`UA_FRAME_RATE`, docs/runtime.md); not measured on the handheld yet |
| 64-bit (arm64 `ua_host`, no 32-bit userland used) | 2018.4, 2019.4, 2020.3 desktop Mono games compiled with `pmunity il2cpp` | title 59.4 - 59.6 fps, `Scripting Backend 'il2cpp', CPU 'arm64-v8a'` |
| | Android IL2CPP builds (`pmunity apk`) of 2020.3, 2021.3, 2022.3 | title; 59.6 fps, and 30 fps for the two that leave Unity's Android default frame rate |

What only the handheld showed:
- **32-bit players on a 64-bit kernel** read the AArch64 format of `/proc/cpuinfo` unless the process
  has the PER_LINUX32 personality; Unity 2019.4+ then takes itself for ARM64 and its FMOD crashes in
  `PluginFactory::registerOutput` before the first frame. ua_host (armhf) now sets PER_LINUX32 on
  an aarch64 kernel, as Android does for its 32-bit apps (`UA_LINUX32=0`: off). QEMU's rig never
  showed it: QEMU supplies its own cpuinfo.
- **BC/DXT textures**: the Mali cannot sample them, so Unity decodes them to RGBA at load. A game's
  57 MB of crunched DXT grew to 830 MB resident in 12 s and the 1 GB handheld froze in swap; with
  the textures step (ASTC) it ran at 60 fps in 385 MB. `pmunity port check` now estimates the decoded
  size and suggests the textures step above 128 MB. The rig's llvmpipe samples BC, so it never showed
  this either.
- Writing hundreds of MB to the SD card during a run makes the game stutter (seen while copying test
  data): keep a device idle during benchmarks.

## Players run (emulated rig)

One free Linux (Mono) game per LTS line, converted with `pmunity convert` + `android-data --pack
all` and run on the runtime in the emulated rig (`tools/emu`: QEMU armv7, Mesa llvmpipe), with
that version's player from `pmunity player fetch`. Frame times there mean nothing; startup, the
Java side, data loading and the picture do.

| player | game | result |
|---|---|---|
| 2017.4.9 | Cuphead (the port) | title screen as before |
| 2017.4.40 | Mono 2.x, Rewired, intro videos | runs; black while its intro VideoPlayer waits (video decoding is not provided); its web requests now fail at once instead of never finishing |
| 2018.4.36 | .NET 4.x (bdwgc) | title screen; 0 unresolved imports (`__isfinite` was one) |
| 2019.4.8 | bdwgc | Unity splash, title screen (with `androidUseSwappy` on: nothing presented, hence the converter default) |
| 2020.3.49 | bdwgc | splash, title screen |
| 2021.3.16 | URP, bdwgc, Burst | first screen (name entry), then the main menu; was a SIGSEGV after 2 frames (Swappy's NULL EGL table), then a hang in `WaitVSync` (vsync clock on UnityMain; Swappy's -1 swap duration) |
| 2022.3.36 | URP, bdwgc, FMOD Studio, Steamworks.NET, Burst | title screen; was the same SIGSEGV, then `Application.Quit` (no `libsteam_api.so`); FMOD stays silent without ARM FMOD libraries |

Windows builds (Direct3D 11 shaders translated with HLSLcc, `--set shaders.from=d3d11`; six free
Windows-only Mono games, docs/shaders.md "Windows builds"): a 2018.3.7f1 3D game starts in its
first level; 2019.2.5f1: title screen; 2019.3.14f1 (URP): Unity splash, title menu; 2020.3.44f1:
splash, first screen; 2021.3.28f1: title menu once its class libraries are the Unix ones
(`pmunity.winbcl`; with the Windows ones Mono asserts in the Input System's start, then SIGSEGV);
2022.3.62f3 (URP): title menu (its Windows class libraries had failed only System.Console).

What these runs still show: Unity's optional Java classes (`GoogleARCoreApi`, `Camera2Wrapper`,
`HFPStatus`, `AudioVolumeHandler`, `SoftInputProvider`) are placeholders that only get natives
registered; cursor images (`Bitmap`/`PointerIcon`) and video decoding are other work; HTTPS from
2018.4+ now checks certificates against the handheld's CA bundle (runtime/jni/up_tls.c, below); a
game's native plugins follow `plugins.map` (FMOD needs FMOD's ARM libraries; Burst falls back).

## Plugins and services (emulated rig)

Native plugins, platform SDKs and HTTPS ([plugins.md](../plugins.md)), on free Linux games of the
corpus, each converted with `android-data` (plugin report, `plugins.map`, FMOD banks beside the APK)
and run in the rig. FMOD's real libraries need an FMOD account and were not used: an ABI-compatible
test double (`runtime/host/test/fmod_double.c`) stood in for them as `lib/libfmodstudio.so`.

| player | game | plugins | result |
|---|---|---|---|
| 2022.3.36 | URP, Burst | FMOD 2.02.23 (`fmodstudio` only), Steamworks, Resonance | the integration starts FMOD (create, output, software format, initialize), loads its 5 banks from beside the APK (`file:///android_asset/*.bank` -> `assets/`), starts its music event, sets 3 VCA volumes; the same 34 calls with the double built as an Android and as a glibc library (15 softfp thunks); `org.fmod.FMOD` answers (initialized, 48000 Hz / 512, assets, one output). Steam stub: `SteamAPI_Init` fails, the game goes on. Without ARM FMOD libraries: `DllNotFoundException`, silent, as before |
| 2021.1.28 | Rewired | FMOD 2.02.04, SQLite, Steamworks | FMOD as above (5 banks, events, 3D attributes); `sqlite3` -> the system's `libsqlite3.so.0` (4 thunks); Unity's own analytics over HTTPS (below) |
| 2022.3.62f2 | Burst, Addressables | Discord Game SDK, StandaloneFileBrowser | Discord stub: the game logs "Failed to initialize Discord: NotRunning" and reaches its language screen. (Its Addressables bundles fail: their provider checks `jar:file://` StreamingAssets paths with file I/O, not a plugin matter) |
| 2022.1.10 | Burst | SQLite (GameAnalytics) | `sqlite3` -> the system's library; GameAnalytics turns itself off on Android ("Unsupported platform") |
| 2018.1.0 | Photon | Steamworks, Discord Game SDK | not reached: a game script's `Awake` faults right after "The script behaviour ... could not be instantiated" (2018.1 is outside the LTS lines; before any plugin call). Stubs registered (steam, discord_game_sdk), Photon's optional native socket plugins refused |
| 2017.4.40 | Rewired, intro videos | Steamworks | requests of Unity's analytics and the game's news feed fail at once as before; with `UA_HTTP=1` (and the system's libcurl) they are made: below |
| 2017.4.9 | Cuphead | CSteamworks, GOG Galaxy (macOS build) | `plugins.map` from the survey: CSteamworks stub (never called: the GOG build's online interface is GOG's), Galaxy refused as before; title screen as before |

HTTPS, Unity 2018.4+ (the 2021.1 game's analytics at startup: `config.uca.cloud.unity3d.com`,
`cdp.cloud.unity3d.com`, `perf-events.cloud.unity3d.com`), on an internal Docker network whose DNS
sends every name to a local HTTPS server that signs a certificate per name with a self-made CA:
with that CA as the bundle (`UA_CA_BUNDLE`), libunity's unitytls verifies all of them and the six
POSTs arrive; with another CA, all six handshakes fail (`Curl error 60: Cert verify failed:
UNITYTLS_X509VERIFY_FLAG_NOT_TRUSTED`) and nothing is sent. Before up_tls.c every certificate
check failed the same way.

HTTP(S), Unity 2017.4 (Java UnityWebRequest, `UA_HTTP=1`, the rig image with Debian's libcurl4
added): the 2017.4.40 game's five HTTPS POSTs (Unity analytics, config) and its plain-HTTP news
GET reach the local server and get their 200 back; with another CA the four HTTPS requests fail
with TLS error 16 (`SSL certificate problem: unable to get local issuer certificate`) and the HTTP
one still works.

## Android builds run (emulated rig)

Free itch.io Android builds, each made ready with `pmunity apk` (docs/apk.md) and run as they
are: the game's own player libraries and data, nothing converted, told their own `targetSdkVersion`
as the API level (docs/apk.md). The 64-bit route runs in an aarch64 container (`EMU_ARCH=arm64`,
native on an Apple-silicon Mac, Mesa llvmpipe), the 32-bit one under QEMU armv7.

| build | minSdk / target (API run) | ABI run | result |
|---|---|---|---|
| 2019.4.18, Mono (bdwgc), armeabi-v7a, Vulkan + GLES3, portrait | 19 / 29 (29) | armeabi-v7a | title screen ("PLAY"), laid out for the landscape window; at times a stall under QEMU (below), at API 19 as at 29 |
| 2020.3.3, IL2CPP, arm64-v8a + armeabi-v7a, GLES3 + GLES2, Optimized Frame Pacing on | 19 / 29 (29) | arm64-v8a | title menu, 35-47 fps in the rig with Swappy pacing on the NDK choreographer and `SwappyDisplayManager` (at API 19/22: 60-70 fps, 2020.3 turns Swappy off itself below 23) |
| 2021.3.4, IL2CPP + Burst, Vulkan + GLES3, Optimized Frame Pacing on | 22 / 31 (31; 33) | arm64-v8a | title menu, 22-29 fps (Swappy on the NDK choreographer, its refresh rate callback); before: stopped after 3 frames (Swappy kept off), then slowed to 2 fps (Swappy's tick on UnityMain) |
| same build | 22 / 31 (31) | armeabi-v7a (IL2CPP, 32-bit) | intro, then the title menu |
| 2022.3.14, IL2CPP + Burst, FMOD, 3D, Vulkan + GLES3, Optimized Frame Pacing on | 26 / 32 (32) | arm64-v8a | title menu over its 3D scene, a steady 30 fps (Swappy: every second vsync); with the data on a USB drive through Docker's file sharing 6-19 fps and some early stops; before AChoreographer: stopped after 8 frames when told API 26, hence the old cap of 23 |
| 2022.3.23, Mono (bdwgc), armeabi-v7a, GLES2 only | 22 / 32 (32) | armeabi-v7a | title screen ("tap anywhere to start") |

The 2021.3 build also ran at API 33 (the cap): the same title menu. At these levels the players
read more of the framework (display cutout mode, window insets, audio devices): runtime/jni answers
those with placeholders, which they survive. In all of them Unity picks GLES (`libvulkan.so` does
not load), finds no OBB (none shipped) and reads its data from the APK as on a phone. A converted
desktop game of 2021.3 (Mono) behaves the same at API 30 as at its default 19.

**The stalls under QEMU** (the Mono builds; the early stops of the 2022.3 IL2CPP build in some arm64
runs were file I/O instead: a thread waiting on Docker's file sharing of a USB drive; with the data
on the internal disk it ran steadily): in 4 runs of 300 s, the 2019.4 Mono
build stopped presenting in 3, at API 19 and 29 alike. Caught with kernel thread states and stack
dumps during the stall, it is not a stop-the-world: no thread sits in the suspend handler
(`rt_sigsuspend`); UnityMain waits in PhysX's `NpScene::fetchResults` (`SyncImpl::wait`, a
condition variable) for simulation tasks that Unity's job workers never take: all of them sleep in
`CappedSemaphore::WaitForSignalImpl` (Baselib's own futex calls, which reach the kernel, here QEMU,
untranslated). A lost wakeup between the job queue and its workers. What the runtime puts on that
path held under stress in the same QEMU, through the bionic translations and through glibc
directly alike: Boehm's stop-the-world protocol as Unity's libmonobdwgc does it (SIGPWR suspends,
SIGXCPU restarts, sem_post from the handler, sem_timedwait-based resends; 20000 rounds x 16
threads: no lost signal, only deliveries up to ~1 s late) and PhysX's Sync handoff (mutex,
condition variable, timed wait on CLOCK_REALTIME; 100000 rounds x 8 pairs: no lost wakeup). The
signal masks, `sigaction` layouts and semaphores match bionic's for the signals Unity's Mono uses
(Boehm: SIGPWR/SIGXCPU; Mono's own thread suspension: SIGUSR1, SIGUSR2, SIGTTIN; all below 32, as
its asserts require). One runtime-side hazard did turn up: ua_host's stack dump on SIGUSR2 collides
with Mono's restart signal, so `kill -USR2` (how the earlier stalls were inspected) aborts a Mono
4.x game ("may have been prematurely finalized") or resumes a suspended thread; the dump also
answers `kill -s RTMIN+7` now (docs/apk.md). Not reproducible natively here (no 32-bit ARM on Apple
silicon): needs a run on a handheld. Not found among free builds: one with an
OBB (a build re-split locally into APK + main OBB, as Unity's "Split Application Binary" makes
them, mounted its OBB and reached its title: docs/apk.md), an IL2CPP arm64 build of 2017.4 - 2019.4
(the one 2018.4 build was a VR title, refused).

## Older and newer lines (5.6 - 2017.2, 2023.1 - 6000.6)

Surveyed the same way (`tools/re`), then converted and run with a free Linux Mono game where one
was found. Everything in the matrix above that is not listed here holds for these lines too (the
REL/RELA-only libraries, the UnityMain looper model, Swappy off, the OBB GUID of 2021.3+).

| | 5.6.7 | 2017.1.5 | 2017.2.5 | 2023.1.22 | 2023.2.22 | 6000.0.85 | 6000.1 - 6000.4 | 6000.5 | 6000.6 |
|---|---|---|---|---|---|---|---|---|---|
| installer (player fetch) | NSIS, per file | same | same | large-installer NSIS | same | **chunked LZMA**: per file (6000.0.58) or solid (6000.0.85) | per file (6000.1.17, 6000.2.15, 6000.3.11), solid (6000.3.26, 6000.4.12) | solid | solid |
| Mono runtime | libmono (in `Libs/`, DT_NEEDED of libunity) | libmono, libmonobdwgc | same | libmonobdwgc | same | same | same | same | same |
| imports nobody provided | `SHA1*` (now shims) | `SHA1*` (now shims) | 0 | 0 | 0 | 0 | - | - | - |
| Java side | UnityPlayer; **`nativeFile`** mounts APK/OBBs; WWW | UnityPlayer; WWW | same | **UnityPlayer abstract**: natives on `UnityPlayerForActivityOrService` | same | same; UI-thread `ALooper` | same | same | + `initJni(Context, I, String)`, `nativeInjectEvent(·, I)` |
| allocator | - | - | - | - | - | - | - | - | **mimalloc**: bionic thread id slot (loader) |
| SerializedFile format | 17 | 17 | 17 | 22 | 22 | 22 | 22 | **23** | 23 |
| shader serializer | 201609010 | 201609010 | 201609010 | 202012090 | 202012090 | 202012090 | 202012090 | 202012090 | **202605290** |
| `unity default resources` read from | `bin/Data/` | same | same | same | **`bin/Data/Resources/`** | same | same | same | same |
| engine assembly slots | 2 | 2 | 45 | by name (json) | same | same | same | same | same |
| PlayerSettings type tree | UnityPy's | UnityPy's | UnityPy's | UnityPy's | 2023.2.20: +1 byte (typetrees.py) | +4 bytes | (6000.3.11: +4) | +4 | +4 |

Not covered: 5.0 - 5.5 (5.0 - 5.4: another Shader layout; 5.5 unchecked), 6000.7 (betas: format
25), the GameActivity variant (a Gradle-built `libgame.so`; every package has the Activity one,
which the runtime plays), Vulkan (the converter keeps GLES3 only).

### Players run (emulated rig)

| player | game | result |
|---|---|---|
| 5.6.0f3 | .NET 3.5, UI, Unity Analytics (WWW) | main menu; its analytics requests fail at once (WWW without a network) |
| 2017.2.0f3 | .NET 3.5, Adventure Creator, CSteamworks, 860 MB | the `TypeLoadException` (`MovieTexture`, desktop-only) is gone with the desktop AudioModule; then black: `EntryPointNotFoundException: SteamClient_` (an old Steamworks.NET through `CSteamworks`, which the Steam stub's flat API lacks) and 47 - 60 frames in 300 s |
| 2023.2.20f1 | bdwgc | main menu (Start, Settings, Exit) |
| 6000.0.58f2 | bdwgc | studio logo, "sound required" card, its opening screen (frames take ~2 s in the rig) |
| 6000.3.11f1 | bdwgc, VideoPlayer (H.264) | main menu; its background clip seeks with "Codec appears stalled" |
| 6000.5.7f1 | format 23 | title screen (Start, Quit) |
| 6000.6.2f1 | mimalloc, 3D | its first scene (in game: sky, crosshair, control hints), 23 frames in 300 s |
| 6000.6.3f1 | mimalloc, generated world | Unity splash, then minutes of world generation (a GC every ~1 s); before the thread id slot: SIGSEGV in mimalloc or Mono assertions; still, in some runs: Mono's restart-signal handler on the UI thread asserting (`mono_thread_info_current`) |

## Linux players under box64 (the other route)

Games the Android-player route cannot take (Unity before 5.6 or after 6000.6, IL2CPP builds
the IL2CPP route does not take, x86-only plugins) run as their own Linux x86_64 build under box64: [box64-route.md](../box64-route.md).
`pmunity route <game>` says which route fits. In the box64 rig (`tools/box64/rig.sh`: arm64
container, Mesa), with box64 0.4.4:

| Linux player | build | desktop GL | GLES mode (`-force-gles`, converted shaders, glxes; no desktop GL) |
|---|---|---|---|
| 5.5.1 | Mono | menu | menu (ES 3.1 AEP device) |
| 2017.4.40 | Mono | title menu | title menu (ES 3.1 AEP) |
| 2018.4.36 | Mono (bdwgc) | title menu | title menu (ES 3.1 AEP) |
| 2019.4.8 | Mono | title | title (ES 3.2) |
| 2020.3.49 / 2020.3.28 | Mono / IL2CPP | title menu / main menu | title menu / not converted (settings step error on that build) |
| 2021.3.16 | Mono, URP | main menu | first-start name entry |
| 2022.3.36 / 2022.3.4 | Mono, URP, FMOD / IL2CPP | title menu / opening scene | title menu / opening scene |
| 6000.5.9 / 6000.6.4 | IL2CPP / Mono | sign-in screen / in game | not converted (SerializedFile format 23) |
| 5.0.0, 5.3.2 | Mono, 32-bit x86 only | not run (box86 or box64's BOX32) | |

The Linux players' own GLES device: `-force-gles` works when OpenGL ES 3 (11) is in the build's
graphics API list (otherwise ignored; the player logs `Unknown renderer 11` and goes on), and it
uses only GLES 3 shader programs (platform 9; `pmunity convert` adds them, `tools/box64/gles.json`).
Its context comes through GLX with the ES profile, after a desktop core-profile probe whose version
sets the ES device level.

## Gaps, most impact for the most games first

1. ~~**JNI signatures that drifted**~~ (done in runtime/jni): natives are called with the signature
   libunity registered: `nativeUnitySendMessage(String, String, byte[])` (2017.4.40+),
   `nativeDone()Z` (2019.4+), `nativeInjectEvent(InputEvent, int)` (2022.3), `long` proxy ids
   (2019.4+); all `newProxyInstance` forms and the 2019.4/2021.3 exception paths of
   AndroidJavaProxy are implemented. Tested with each version's native table, not with the real
   players.
2. **Missing bionic symbols** (done): bionic's own fortify functions (`__FD_SET_chk`,
   `__FD_CLR_chk`, `__FD_ISSET_chk`, `__strlen_chk`, `__strchr_chk`, `__strrchr_chk`,
   `__strlcpy_chk`, `__strlcat_chk`, `__memchr_chk`, `__write_chk`, ... with bionic's checks and
   "FORTIFY:" messages, then abort), `android_set_abort_message` (printed), `__mmap2`,
   `sys_signame`/`sys_siglist`; the ABI translations listed under "imports with ABI risk" above.
   `nftw` mattered beyond its stat layout: glibc's flag and type values would make a recursive
   delete visit directories first and see them as symlinks.
3. **libmediandk and video** (done): `android/media_ndk.c` is the NDK's media API over the media
   core (`runtime/media`), and its failing stubs where the core is not linked (the shims' own
   tests); the `AMEDIAFORMAT_KEY_*` objects hold the NDK's strings. The host's
   `so_map_library("libmediandk.so", NULL)` keeps dlopen of it failing, so 2017.4-2021.3 decode
   through Java (`runtime/jni/up_media.c`), while 2022.3's DT_NEEDED entry and imports bind to
   the NDK implementation. Both draw through a SurfaceTexture into Unity's external texture with
   the converter's `Hidden/VideoDecodeAndroid`. Decoders: libvpx, libvorbis, libopus, OpenH264
   built in, the system's libavcodec when present (AAC, MP3, HEVC, H.264 with B-frames);
   `runtime/media/README.md`. Left: 2022.3's Vulkan path (`AImageReader`), a run of the
   2018.4-2022.3 players with video, device measurements.
4. ~~**Converter data formats**~~ (done in the converter): SerializedFile format 22, the four shader
   serializers, segmented blobs, parameter entries and per-tier subprograms, assembly lists by
   flags and `ScriptingAssemblies.json`, 2019+ ASTC numbers, compressed builds via `pmunity
   unpack`, GLCore41/43 programs (2018.1+), the 2021.3+ OBB GUID (the OBB's `unity_obb_guid` at
   its root, the APK's `assets/unity_obb_guid`), D3D11 programs of Windows builds of every line
   (2019.1+: the GL binding info Unity's GL compiler writes).
5. **Mono 5/6 (`libmonobdwgc-2.0.so`)**: the only Mono runtime from 2019.x on (and of any
   .NET 4.x game of 2017.4/2018.4), never run on this runtime. Bring-up test: Boehm GC thread
   suspend/restart signals through the loader's signal translation, `sigaltstack`,
   `libmono-native.so` (2021.3+, needs its SONAME dependency `libmonoboehm-2.0.so` loaded first).
6. ~~**UnityPlayer callbacks added since 2018.4**~~ (done in runtime/jni, with handheld answers):
   `isUaaLUseCase`, `getUaaLLaunchProcessType`, `getLaunchURL`, `getNetworkConnectivity`,
   `getKeyboardLayout`, `startOrientationListener`, the 9- and 10-argument `showSoftInput`,
   clipboard, `getScreenBrightness`/`setScreenBrightness` (2021.3), `hidePreservedContent`,
   `setMainSurfaceViewAspectRatio`, activity indicator (2022.3), and the framework members the
   later players read (`View.getDisplay`, display modes, ...).
7. ~~**Swappy**~~ (2019.3+), done: off. Swappy's own switch (`swappy.disable`, the loader's
   system properties) keeps it off once its EGL table loads (`eglPresentationTimeANDROID` is a
   no-op; with NULL, the 2021.3+ Swappy kept a disabled instance whose fallback swap went through
   a NULL table: the SIGSEGV on `UnityGfxDeviceW` after two frames) and its display query works
   (`NativeActivity.getWindowManager()`). Unity's own setting must be off as well, which the
   converter does by default (`--swappy`): with `androidUseSwappy` on and Swappy off, 2019.4
   presents nothing and 2021.3+ wait for INT_MAX vsyncs after their first frames
   (`runtime/android/README.md`, "Frame pacing").
8. **EGL** (done): `eglSurfaceAttrib` (2020.3+) on the SDL display path goes to the real surface
   (and is set again on a surface KMSDRM puts in its place); on fbdev it is the driver's own.
9. **Packaging of Android builds** (games owned as APK/AAB; `pmunity apk`, docs/apk.md: APKs,
   splits, XAPK/APKS, AAB and OBBs handled, asset-pack splits merged into the one APK the player
   reads; run as above): split APKs
   (`ApplicationInfo.splitPublicSourceDirs`, 2017.4.40+), Play Asset Delivery
   (`PlayAssetDeliveryUnityWrapper.getAssetPackPath`, 2019.4+; Unity data in `UnityDataAssetPack`;
   runtime/jni answers as an app without Play Core: no asset packs); their PlayerSettings'
   `androidUseSwappy` (on in many 2020+ projects) stays as built: ua_host runs Swappy for them and
   reports their `targetSdkVersion` (at most 33): Swappy ticks on the NDK's AChoreographer from 24
   (runtime/android/choreographer.c), on its Java callback (a thread of its own) below.
10. ~~**IL2CPP Android games**~~ (run, 32- and 64-bit): `libil2cpp.so` comes up with libunity
    through libmain's `NativeLoader.load` (its `JNI_OnLoad` right after libmain's; `ClassLoader.findLibrary`
    + `System.load` serve Burst's `lib_burst_generated.so`); libunity copies IL2CPP data to
    `<external files>/il2cpp` (2022.3 re-extracts when `assets/bin/Data/unity_app_guid` changes)
    and binds 175-234 `il2cpp_*` functions from the game's `libil2cpp.so`.
11. Small: ~~`nativeSendSurfaceChangedEvent` after a resize (2018.4+)~~ (done); the 2022.3 `armv9` libunity
    must not be used on ARMv8 CPUs; 16 KiB-page kernels stay unsupported (the 2022.3.62 libraries
    would allow them).

What needs nothing: no player uses packed (APS2) or RELR relocations, IFUNC or ELF TLS (all
REL/RELA + PLT; no PT_TLS segment or TLS relocation anywhere), no new DT_NEEDED before 2022.3, and the
Java side keeps the 2017.4 model throughout (`UnityPlayer` FrameLayout + SurfaceView, NativeLoader,
`initJni(Context)`, a `UnityMain` looper thread posting "next frame" messages that run GL-thread
jobs and `nativeRender`).

## How this was surveyed (repeatable)

Downloads: the Windows "Android Build Support" installer of each version from Unity's archive
(Unity's release API gives each version's changeset and URL). The installers are non-solid NSIS
(from 2020.3 a large-installer variant: 36-byte first header, 64-bit file offsets),
so `tools/re/nsisrange.py` fetches only the needed files with HTTP range requests (20-30 MB per
version); the installers were never stored. Per version:

```
python3 tools/re/nsisrange.py list    <installer URL>
python3 tools/re/nsisrange.py extract <installer URL> pkg '$_OUTDIR/Variations/mono/Release/*' ...
tools/re/survey_player.sh pkg/_OUTDIR notes      # docker: ua-spec (pyelftools, capstone), ua-jni-build (javap)
python3 tools/re/unitydata.py "pkg/_OUTDIR/Data/unity default resources"   # UnityPy + lz4
python3 tools/re/javacalls.py notes/javap_s.txt <libunity.so> > notes/javacalls.tsv
gcc ... tools/re/jnicover.c runtime/jni/*.c && jnicover < notes/bindings.tsv   # see its header
python3 tools/re/playerdiff.py <base notes> notes runtime --all-missing
```

| tool | does |
|---|---|
| `nsisrange.py` | list/extract NSIS installers over HTTP ranges (both NSIS layouts Unity uses) |
| `elfimage.py` | relocations of every format (REL/RELA, APS2, RELR), relocated pointer reads, ELF facts (NDK note, alignment, TLS, IFUNC) for both ABIs |
| `elfimports.py` | undefined symbols with provider and ABI notes (now both ABIs, packed relocations) |
| `strrefs.py`, `jnibindings.py` | Java members libunity binds (now also Thumb-2 code and literal-pool strings) |
| `jnitables.py` | RegisterNatives tables |
| `dis.py` | disassemble functions by name, or find functions by the strings they reference (AArch64) |
| `javacalls.py` | player Java methods/fields libunity can reach by name |
| `jnicover.c` | which bound classes/members runtime/jni defines |
| `shimnames.py` | names the runtime provides (loader, libhybris hooks, Android shims) |
| `unitydata.py` | SerializedFile header, shader blob layout and serializer versions of the built-in resources |
| `playerdiff.py` | the comparisons above between two players, against the runtime |
