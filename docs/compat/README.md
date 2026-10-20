# Unity version coverage: what each LTS Android player needs

The runtime and converter were built for Unity 2017.4.9f1 (Mono, armeabi-v7a; Cuphead). This
survey reads the Android players of the last patch of every LTS line, 2017.4 to 2022.3, and lists
what each one needs that the tools do not have yet. It started as static analysis of Unity's own
files (imports, JNI tables and string references, the Java side, the built-in resources); every
line has since run a free Linux game in the emulated rig ("Players run" below). Per-version details:
[2017.4.40f1](unity-2017.4.40f1.md) · [2018.4.36f1](unity-2018.4.36f1.md) ·
[2019.4.40f1](unity-2019.4.40f1.md) · [2020.3.48f1](unity-2020.3.48f1.md) ·
[2021.3.45f2](unity-2021.3.45f2.md) · [2022.3.62f3](unity-2022.3.62f3.md).

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
| of which runtime/jni defines | 250 | 254 | 287 | 306 | 309 | 308 | 304 |
| UnityPlayer Java model | ✅ | ✅ | ✅ | ✅ (callbacks 28/28) | ✅ (27/27) | ✅ (30/30) | ✅ (34/34; GameActivity only from 2023.1) |
| frame pacing (Swappy) | - | - | - | ✅ off (`swappy.disable`); `androidUseSwappy` must be off (converter default) or nothing is presented | ✅ (Unity: off below API 23) | ✅ off; with the setting on: stops after 2-4 frames | ✅ same |
| vsync clock | UnityMain | UnityMain | UnityMain | UnityMain | UnityMain | ✅ own HandlerThread (`FrameTimeTracker`): per-looper Choreographer | ✅ same |
| SerializedFile format | 17 ✅ | 17 ✅ | 17 ✅ | 21 ✅ | 22 ✅ | 22 ✅ | 22 ✅ |
| shader serializer (converter) | 201708220 ✅ | 201708220 ✅ | 201802150 ✅ | 201806140 ✅ segments | 201806140 ✅ | 202012090 ✅ param entries | 202012090 ✅ |
| GLCore41/43 programs (converter) | - (output as before) | ⚠️ off by default (`shaders.glcore4x`) | ✅ GLES3 / GLES 3.1 | ✅ | ✅ | ✅ + stripped shaders' empty blobs | ✅ |
| D3D11 programs (Windows builds) | ✅ | ✅ | ✅ | ❌ GL cbuffer layouts | ❌ + common params | ❌ | ❌ |
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
  0/null. Video's members (MediaExtractor, MediaFormat, MediaCodec, SurfaceTexture, ...) are
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
  blob, as in Unity's builds. D3D11: 2019.1+ GL programs carry constant buffer layouts the D3D11
  data does not give (refused with that reason).
- IL2CPP metadata: the header's integer version (`global-metadata.dat` starts with 0xFAB11BAF
  and the version). These values come from public reverse-engineering tools, not from these
  packages (`libil2cpp.so` and the metadata are built per game), and tools also use sub-versions
  (24.1 for 2018.3/2018.4, 24.2-24.5 across 2019.x, 27.1 for 2020.2.4+, 29.1 for 2022.x; late
  2022.3 patches are reported with 31). Read the header of the game's own file.

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

What these runs still show: Unity's optional Java classes (`GoogleARCoreApi`, `Camera2Wrapper`,
`HFPStatus`, `AudioVolumeHandler`, `SoftInputProvider`) are placeholders that only get natives
registered; cursor images (`Bitmap`/`PointerIcon`) and video decoding are other work; HTTPS from
2019.4+ fails certificate checks (`TrustManagerFactory` placeholders; libunity's unitytls then
reports "Cert verify failed"); a game's native plugins (FMOD, Burst) need ARM builds.

## Android builds run (emulated rig)

Free itch.io Android builds, each made ready with `pmunity apk` (docs/apk.md) and run as they
are: the game's own player libraries and data, nothing converted. The 64-bit route runs in an
aarch64 container (`EMU_ARCH=arm64`, native on an Apple-silicon Mac, Mesa llvmpipe), the 32-bit
one under QEMU armv7.

| build | ABI run | result |
|---|---|---|
| 2019.4.18, Mono (bdwgc), armeabi-v7a, Vulkan + GLES3, portrait | armeabi-v7a | title screen ("PLAY"), laid out for the landscape window |
| 2020.3.3, IL2CPP, arm64-v8a + armeabi-v7a, GLES3 + GLES2, Optimized Frame Pacing on | arm64-v8a | title menu, 60-70 fps in the rig (2020.3 turns Swappy off itself below API 23) |
| 2021.3.4, IL2CPP + Burst, Vulkan + GLES3, Optimized Frame Pacing on | arm64-v8a | title menu, 25-29 fps; before: stopped after 3 frames (Swappy kept off), then slowed to 2 fps (Swappy's tick on UnityMain) |
| same build | armeabi-v7a (IL2CPP, 32-bit) | intro, then the title menu |
| 2022.3.14, IL2CPP + Burst, FMOD, 3D, Vulkan + GLES3, Optimized Frame Pacing on, minSdk 26 | arm64-v8a | title menu over its 3D scene (0.2-12 fps: llvmpipe-bound); before: stopped after 8 frames when told API 26 (Swappy on the NDK choreographer) |
| 2022.3.23, Mono (bdwgc), armeabi-v7a, GLES2 only | armeabi-v7a | title screen ("tap anywhere to start") |

In all of them Unity picks GLES (`libvulkan.so` does not load), finds no OBB (none shipped) and
reads its data from the APK as on a phone. The Mono ones stalled for minutes at times under QEMU
with UnityMain stopped in the Boehm collector's suspend handler (a stop-the-world that does not
end); not seen with the converted desktop games' longer runs nor reproducible natively here (no
32-bit ARM on Apple silicon): needs a run on a handheld. Not found among free builds: one with an
OBB, an IL2CPP arm64 build of 2017.4 - 2019.4 (the one 2018.4 build was a VR title, refused).

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
   its root, the APK's `assets/unity_obb_guid`). Left: D3D11 programs of 2019.1+ Windows builds
   (GL constant buffer layouts).
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
   `androidUseSwappy` (on in many 2020+ projects) stays as built: ua_host runs Swappy for them
   (its Java tick on a thread of its own) and reports at most API 23, below Swappy's NDK path.
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
