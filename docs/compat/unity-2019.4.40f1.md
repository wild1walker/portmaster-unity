# Unity 2019.4.40f1 (last 2019 LTS)

Installer: `UnitySetup-Android-Support-for-Editor-2019.4.40f1.exe` (changeset ffc62b691db5, 352 MB).
Compared with 2018.4.36f1. The biggest step of the LTS line.

## Package
| | |
|---|---|
| ABIs | Mono: armeabi-v7a only (x86 Mono dropped). IL2CPP: armeabi-v7a, arm64-v8a, x86, x86_64 |
| Mono runtimes | **`libmono.so` (.NET 3.5) is gone**: only `libmonobdwgc-2.0.so` (Unity's Mono 5/6 with Boehm GC) + `libMonoPosixHelper.so` |
| toolchain | NDK r19; min API 19 (armeabi-v7a), 21 (arm64-v8a); **armeabi-v7a code is Thumb-2** |
| relocations | REL / RELA + PLT only; no APS2, RELR, IFUNC or PT_TLS; arm64-v8a 64 KiB aligned |
| DT_NEEDED | libunity no longer needs `libGLESv2.so` or `libstdc++.so` (GL is dlopened, C++ runtime linked in) |
| new files | `gamesdk_classes_dex.o` next to each IL2CPP libunity (Android Game SDK Java classes as a dex) |

## Native imports
- armeabi-v7a `libunity` 373 imports, all provided. New ones going to glibc that need an ABI
  look: `getpwuid_r` (bionic's 32-bit `struct passwd` has no `pw_gecos`: glibc writes a
  different layout), `nftw` (the callback gets a glibc `struct stat`, not bionic's),
  `sched_get/setaffinity` (fine: size passed), `capset`, `strtoimax`, many `isw*`.
- arm64-v8a `libunity` 353 imports; **not provided**: `__FD_SET_chk`, `__FD_ISSET_chk` (bionic
  fortify for `select()`), `android_set_abort_message`. Called, they hit the loader's abort stub.
- `libmonobdwgc-2.0.so` and `libMonoPosixHelper.so` (armeabi-v7a): **`__mmap2`** not provided
  (bionic's 32-bit mmap with a page offset); `__end__` is a weak data import (0 is fine).
- Strings show dlsym'd NDK APIs: `AChoreographer_getInstance/postFrameCallback(Delayed)/
  registerRefreshRateCallback` (Swappy, see below), `libmediandk.so` (video, falls back to JNI),
  `libvulkan.so`, `eglPresentationTimeANDROID`.

## JNI
- RegisterNatives: 47. Changes that break runtime/jni:
  - **`nativeDone` returns boolean** (`()Z`, was `()V`): the quit path's lookup fails.
  - `ReflectionHelper.nativeProxyInvoke/nativeProxyFinalize/nativeProxyLogJNIInvokeException`
    take a **`long`** proxy handle (was `int`); Java's `newProxyInstance` is `(long, Class)` /
    `(long, Class[])`. runtime/jni defines the int versions and passes the handle as int:
    AndroidJavaProxy (managed callbacks from Java) breaks.
  - `nativeInitWebRequest` and the `UnityWebRequest` Java class are gone (web requests are native
    now); runtime/jni already skips the call when it is not registered.
  - New: `nativeApplicationUnload`, `nativeOrientationChanged(II)`, `nativeSetLaunchURL`,
    `nativeSetInputArea(IIII)`, `nativeSetKeyboardIsVisible`, `nativeReportKeyboardConfigChanged`,
    `onAudioVolumeChanged`, HFP (Bluetooth headset) and asset-pack status natives.
- Java members libunity binds: 487. Display info now comes from `View.getDisplay()` and
  `Display.getRealMetrics/getWidth/getHeight/getRefreshRate` (no more `WindowManager.
  getDefaultDisplay`, `Display.getMetrics/getRawWidth`), plus `Display.getAppVsyncOffsetNanos/
  getPresentationDeadlineNanos/isWideColorGamut`, `PointerIcon`, `KeyCharacterMap.get`,
  `AudioManager.getDevices`, `WifiManager` multicast lock, `PackageManager.queryIntentActivities`.
- UnityPlayer methods libunity reaches by name (not defined in runtime/jni, so they read 0/null):
  `isUaaLUseCase`, `getUaaLLaunchProcessType`, `getLaunchURL`, `getNetworkConnectivity`,
  `getKeyboardLayout`, `startOrientationListener`, `notifyOnUnityPlayerUnloaded`, the new
  10-argument `showSoftInput`, clipboard and soft-input setters, `skipPermissionsDialog`.
- Java side (128 classes): `UnityPlayerActivity` moved out of `classes.jar` into `Source/` (the
  gradle project compiles it); new `com.google.androidgamesdk.ChoreographerCallback` and
  `SwappyDisplayManager` (Swappy's Java side; their natives are exported `Java_` symbols, not
  RegisterNatives), Play Asset Delivery wrapper, HFP, network connectivity, multi-window.
  Startup is unchanged: `loadNative` (NativeLoader) -> `initJni(Context)` -> `UnityMain` looper
  thread (message 2269) running GL-thread jobs, then `nativeRender` per frame.

## Frame pacing (Swappy)
`libunity` contains Android Frame Pacing (Swappy, player setting `androidUseSwappy`). It looks up
`AChoreographer_*` in libandroid (our shim has none: NULL), then falls back to Java
(`com.google.androidgamesdk.ChoreographerCallback`, loaded from its embedded dex through
`dalvik.system.InMemoryDexClassLoader` if the class is missing) and needs the
`EGL_ANDROID_get_frame_timestamps`/`eglPresentationTimeANDROID` extensions. With none of these,
Swappy should switch itself off ("Swappy is disabled") and Unity presents as before; that path was
not run.

## Data
- `unity default resources`: **format 21**, "2019.4.38f1", platform 13 (format 21 is still the
  20-byte header the converter reads).
- Shader programs: serializer **201806140**, GL entries 4 statistics ints (write back unchanged).
  **Layout change (2019.3+)**: `offsets`/`compressedLengths`/`decompressedLengths` are lists per
  platform (several LZ4 segments), and the entry table holds 12-byte entries
  (offset, length, segment) instead of 8. SubPrograms now have `m_GlobalKeywordIndices` and
  `m_LocalKeywordIndices`. `blob.py`/`ShaderBlobs` read neither.
- MonoManager: flags in `m_AssemblyTypes` (as 2018.4).
- 58 `UnityEngine*.dll` in the Android Managed set.

## IL2CPP
- 222 `il2cpp_*` functions (new: incremental GC, write barriers, debugger agent, unitytls).

## What the tools need for this version
- runtime loader/bionic: `__FD_SET_chk`, `__FD_ISSET_chk`, `android_set_abort_message`, `__mmap2`;
  layout translation for `getpwuid_r` and `nftw` on 32-bit ARM.
- runtime/jni: `nativeDone()Z`; long proxy handles and the new `newProxyInstance` signatures;
  defaults for the new UnityPlayer methods; a decided Swappy behaviour (classes absent, or
  `AChoreographer` shims).
- Mono: every Mono game of this version runs on `libmonobdwgc-2.0.so`, which the runtime has
  never run (Cuphead uses `libmono.so`).
- converter: serializer 201806140, segmented blobs and 12-byte entries; MonoManager flags.
