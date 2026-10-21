# Unity 6 (6000.0 - 6000.6), Mono armeabi-v7a, vs 2022.3.62f3

Installers surveyed: `UnitySetup-Android-Support-for-Editor-6000.0.85f1.exe` (changeset
ee7790f1426a, 505 MB; the last 6000.0 LTS patch when surveyed), and the `unity default resources`
and Mono players of 6000.1.17f1, 6000.2.15f1, 6000.3.26f1, 6000.4.12f1, 6000.5.11f1, 6000.6.2f1,
6000.6.3f1 and 6000.6.6f1. Run in the emulated rig: free Linux games of 6000.0.58f2, 6000.3.11f1,
6000.5.7f1, 6000.6.2f1 and 6000.6.3f1 (README, "Players run"). 6000.7 (betas) is not covered.

## Installers and `pmunity player fetch`
Every Unity 6 Android support installer still carries the Mono armeabi-v7a player
(`Variations/mono/Release/Libs/armeabi-v7a`, `MonoLibs/armeabi-v7a`, `Symbols/armeabi-v7a`, with
`libunity.dbg.so` from 6000.6). The installers are built with an NSIS of 2025 (first-header flag
0x40) that cuts LZMA data into chunks of 2 MiB (uncompressed), each its own LZMA stream behind a
3-byte compressed length:

| installers | layout | what a fetch costs |
|---|---|---|
| 6000.0.58f2, 6000.1.17f1, 6000.2.15f1, 6000.3.11f1 (and older patches) | per-file blocks (8-byte block words, the chunks of a block end with a zero length) | one range request per file, as before: ~150 requests, 13-14 MB |
| 6000.0.85f1, 6000.3.26f1, 6000.4.12f1, 6000.5.x, 6000.6.x | solid: one stream of all files (each: 8-byte length + data, at its offset in the stream), the header a chunk of its own | the chunk lengths are walked with a 3-byte range request per 2 MiB chunk (kept-alive connection) up to the chunks a file spans: about 1,000 - 2,700 requests, 13-18 MB read, 40-100 s from the Mac |

`convert/pmunity/nsis.py` reads all four layouts (`tests/test_nsis.py`: hand-made installers of
each). The package keeps `unity default resources` in `Data/Resources/` (from 2023.2).

## Package
| | 6000.0.85f1 |
|---|---|
| ABIs | Mono: armeabi-v7a. IL2CPP: armeabi-v7a, arm64-v8a, armv9, x86_64 (no x86) |
| Mono runtime | `libmonobdwgc-2.0.so` (SONAME libmonoboehm-2.0.so), `libMonoPosixHelper.so`, `libmono-native.so`; the same NDK r19 build as 2021.3 - 2022.3 |
| toolchain | NDK r27c, min API 23 (libmain, libunity); REL + PLT only, no APS2/RELR/IFUNC/PT_TLS; 16 KiB segments (Mono libraries 4 KiB) |
| DT_NEEDED | as 2022.3 (libmediandk among them) |
| GameActivity | `Source/GameActivity/*.cpp` and `UnityPlayerGameActivity.java`: the GameActivity variant's `libgame.so` is compiled by a Gradle build, so the package has no prebuilt one; the runtime plays the Activity variant (`UnityPlayerActivity`), which every package carries |

## Native imports
- Mono armeabi-v7a libunity: 438 imports, all provided (shims or glibc). New since 2022.3: weak
  `AChoreographer_getInstance/postFrameCallback/postFrameCallback64` (absent: libunity's vsync
  clock stays on Java's Choreographer, as in 2021.3+), `ALooper_addFd/removeFd`,
  `sem_timedwait/sem_trywait`, `process_vm_readv`, `strtof`, `round` (softfp), `syslog`.
- **mimalloc** (6000.6: "Mimalloc is enabled and will override the Dynamic Heap and Dual Thread
  allocators"): libunity's mimalloc takes bionic's thread id slot as its thread id
  (`_mi_thread_id`: `mrc p15, 0, r0, c13, c0, 3; ldr r0, [r0, #4]`, inlined at 9 places). That
  word is the unused second word of glibc's TCB, zero in every thread: mimalloc's "no owner", so
  frees from other threads were taken as local ones and heaps broke (SIGSEGV in
  `_mi_theap_malloc_zero`, Mono asserting in `mono_thread_info_current`). The loader now gives the
  word a unique value per thread (the thread pointer): for the thread that loads the first library
  and, through its `pthread_create`, for every thread (runtime/loader/so_loader.h, "THREAD ID
  SLOT"; both architectures). 6000.0 - 6000.5 do not enable mimalloc.

## JNI
- **UnityPlayer is abstract from 2023.1** (`UnityPlayerForActivityOrService` for an Activity or
  Service, `UnityPlayerForGameActivity`). libunity registers the per-instance natives
  (`nativeRender`, `nativePause`, `nativeResume`, `nativeDone`, `nativeFocusChanged`,
  `nativeRecreateGfxState`, `nativeSendSurfaceChangedEvent`, soft input, `nativeGetNoWindowMode`,
  `nativeMemoryUsageChanged`, ...) on `UnityPlayerForActivityOrService`, the rest (`initJni`,
  `nativeInjectEvent`, `nativeUnitySendMessage`, `nativeHidePreservedContent`, ...) on
  `UnityPlayer`, and a few on `UnityPlayerForGameActivity`. runtime/jni looks a UnityPlayer native up
  on both classes; the player object stays a UnityPlayer (older players never ask for the
  subclass).
- `initJni(Context, int)` (2023.1 - 6000.5; the int is the context type: 0 ActivityOrService, 1
  GameActivity), `initJni(Context, int, String)` in 6000.6 (`UnityPlayer.initialize`'s string, ""
  for the Activity): called as (activity, 0, "").
- `nativeInjectEvent(InputEvent)Z` again in 2023.1 - 6000.5, `(InputEvent, int)Z` in 6000.6.
- `nativeLowMemory` is gone: `onTrimMemory(MemoryUsage)` sends `nativeMemoryUsageChanged(value)`
  (Low 1, Medium 2, High 3, Critical 4); `up_low_memory()` sends Critical.
- New in 6000.6: `clearMicrophonePermissionCache`, `nativeViewPaddingChanged`,
  `nativeGetMainWindowIndex`, `nativeCrashNow`, `nativeOnMultiWindowModeChanged`,
  `AudioDeviceObserver` natives; accessibility natives (`UnityAccessibilityDelegate`) from 2023.2.
- **UnityPlayerJavaWrapper** binds UnityPlayer members earlier players did not:
  `runningOnMainThread` (Java: the UnityMain thread; answered for the calling thread),
  `executeMainThreadJobs` (the jobs queued for UnityMain), `invokeOnMainThread`,
  `requestUserAuthorization(String, long)` (granted, answered with `permissionResponseToNative`),
  `shouldSetGameState`, `shouldReportFullyDrawn`/`reportFullyDrawn`, `setRunWithoutFocus`,
  `requestPointerCapture`/`releasePointerCapture`, `shutdown` (runtime/jni/up_unity6.c). Helpers the
  engine asks for device facts: `DeviceInfo` (model, OS version, language, locales, total space,
  boot time), `ApplicationExitInfoProvider.getANRRecordsJson` ("[]"), the advertising / app set /
  Firebase id helpers (no answer), `UnityGameState`, `UnityPlayerUtilities` (6000.6).
- **The UI thread's native looper.** libunity's `InitializeUILooper` (JNI_OnLoad, on Android's UI
  thread) takes `ALooper_forThread()` for its `NdkLooper` handlers (surface callbacks, vibrator,
  cursors); without one it logs "Couldn't retrieve native ALooper for UI thread." and crashes
  later. runtime/jni prepares the android shims' ALooper on the UI thread before loading the
  libraries and services it in `up_poll()`.
- Java side (194 classes in 6000.0): the UnityMain looper thread (`com.unity3d.player.U`), message
  2269 for the next frame, `executeMainThreadJobs` then `nativeRender`: the same model as before.
  `UnityPlayerActivity.mUnityPlayer` (read by libunity) is the player.

## Data
- `unity default resources`: format 22 up to 6000.4, **format 23** from 6000.5 (the same 48-byte
  header; type tree entries carry a hash and their size); 6000.7 betas: format 25 (refused).
- Shader programs: serializer 202012090 up to 6000.5, **202605290** in 6000.6, both with the 2022.3
  layout (checked: a 6000.6 game's built-ins convert to Unity's own GLES3 programs, test_glcore_corpus).
- `PlayerSettings` of the releases is newer than UnityPy's trees (4 bytes; `captureStartupLogsBaked`
  in 6000.6.3f1): `pmunity/typetrees.py` (android-data.md, section 4).
- The player reads `assets/bin/Data/Resources/unity default resources`.
- Assemblies: `ScriptingAssemblies.json` (80 engine modules in a 6000.6 game; `Unity.Scripting.dll`
  among the Android ones), OBB GUID as 2022.3.

## What the tools need for this version
- converter: format 23, the 6000.6 serializer, the default resources path, the PlayerSettings
  tree: done.
- runtime: natives on both player classes, the 2023.1+ UnityPlayer members and helpers, the UI
  ALooper, mimalloc's thread id: done. Not done: the GameActivity variant (not needed: the Activity
  one is in every package), accessibility, Vulkan (the converter keeps GLES3 only).
