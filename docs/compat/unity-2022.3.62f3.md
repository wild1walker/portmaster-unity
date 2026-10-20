# Unity 2022.3.62f3 (latest public 2022 LTS patch)

Installer: `UnitySetup-Android-Support-for-Editor-2022.3.62f3.exe` (changeset 96770f904ca7, 477 MB).
Compared with 2021.3.45f2. GameActivity (Unity 2023.1+) is not in this package: the Java side is
still `UnityPlayer` + `UnityPlayerActivity`.

## Package
| | |
|---|---|
| ABIs | Mono: armeabi-v7a. IL2CPP: armeabi-v7a, arm64-v8a, x86, x86_64, and a new **`armv9`** variation (an arm64 libunity built for ARMv9 CPUs; same imports; never pick it for Cortex-A53/A55 handhelds) |
| Mono runtimes | as 2021.3 (`libmonobdwgc-2.0.so` = SONAME `libmonoboehm-2.0.so`, `libMonoPosixHelper.so`, `libmono-native.so`) |
| toolchain | NDK r23b, min API 22, Thumb-2; REL/RELA + PLT only; no APS2/RELR/IFUNC/PT_TLS |
| alignment | **every library, the Mono ones included, has 16 KiB-aligned segments** (Android 15 page-size support). Fine for the 4 KiB-page loader; a 16 KiB-page kernel would now be possible if the loader supported it |
| DT_NEEDED | **libunity needs `libmediandk.so`** |

## Native imports
- **`libmediandk.so` is a hard dependency now** (VideoPlayer moved from Java MediaCodec to the
  NDK): strong imports of `AMediaCodec_*` (configure, start/stop, buffers), `AMediaExtractor_*`,
  `AMediaFormat_*` and the `AMEDIAFORMAT_KEY_*` string constants (data objects); weak imports of
  `AImageReader_*`, `AImage_*`, `AMediaDataSource_*`, newer keys. Provided now by stubs
  (`android/media_ndk.c`): every call fails cleanly, the key constants hold the NDK's strings, and
  `libmediandk.so` is one of the loader's Android-only libraries (served by shims). Before, the
  DT_NEEDED entry was skipped as a system library, the functions bound to the loader's abort stub
  and the key objects to zeroed blocks.
- libandroid: weak `AHardwareBuffer_acquire/describe/release`, `ANativeWindow_toSurface` (0 if
  absent: fine). A weak C++ `thread_local` init hook (`_ZTH...`) appears; there is still no ELF TLS.
- Not provided before (as 2021.3), provided now: `__FD_SET_chk`, `__FD_ISSET_chk`, `__strlen_chk`
  (arm64), `android_set_abort_message`; Mono: `__strlen_chk`, `__strchr_chk`, `__strrchr_chk`,
  `__strlcpy_chk`, `__mmap2`, `sys_signame`.
- New `*_l` locale calls (`isdigit_l`, `islower_l`, `isupper_l`, `iswlower_l`, `isxdigit_l`,
  `strftime_l` on arm64; `strtold_l`, `strtoll_l`, `strtoull_l` since 2019.4) go to glibc. The
  locale objects come from libc++'s `newlocale`, which libhybris passed straight to glibc (so they
  were glibc `locale_t`s, but a name bionic accepts and the host lacks, such as `en_US.UTF-8`,
  failed). `newlocale` now follows bionic: "C"/"POSIX", "C.UTF-8"/"en_US.UTF-8"/"" (glibc's
  C.UTF-8, or C where the host has none), ENOENT for other names; the objects stay glibc's.
  `strtold_l` returns floating point: a softfp wrapper on 32-bit ARM.

## JNI
- RegisterNatives: 48. **`nativeInjectEvent` takes `(InputEvent, int)`** (an extra int; Java
  passes 0): runtime/jni now calls the form libunity registered (it used to call the one-argument
  form, which meant no input). `nativeRestartActivityIndicator` is gone (runtime/jni already checks), new
  `nativeGetNoWindowMode()Z` (Java asks it whether to run without an Android window).
- Java members libunity binds: 395 (+15 -107). Gone: Camera v1, the Java MediaCodec/
  MediaExtractor/MediaFormat users, ConnectivityManager/NetworkInfo, Dialog, ParcelFileDescriptor,
  WifiManager. New: `Display.getHdrCapabilities/isHdr`, `KeyEvent.getSource/isCtrlPressed/
  isShiftPressed`, `MotionEvent.getOrientation/getPressure`, `View.getLeft/getTop/getRootView`,
  `Build.BRAND`.
- New UnityPlayer methods reachable by name: `setMainSurfaceViewAspectRatio`,
  `startActivityIndicator`/`stopActivityIndicator`, `setBackButtonLeavesApp`.
- Java side (153 classes): same model (a `UnityMain` thread, message 2269, GL-thread jobs then
  `nativeRender`); back handling through `OnBackInvokedDispatcher`; `NetworkConnectivityNougat`.

## Data
- `unity default resources`: format 22, "2022.3.54f1".
- Shader programs: serializer 202012090 (as 2021.3: GL code entries with 4 statistics ints,
  separate parameter entries, per-tier player subprograms); programs add
  `m_SerializedKeywordStateMask`.
- MonoManager: flags in `m_AssemblyTypes`. 64 `UnityEngine*.dll`.
- OBB: an OBB must contain **`assets/unity_obb_guid`** matching the GUID the application expects
  ("Application OBB has mismatching GUID"); the IL2CPP player keeps
  `assets/bin/Data/unity_app_guid` to decide when to re-extract its resources. Play Asset
  Delivery: Unity data can sit in an install-time asset pack (`UnityDataAssetPack`), found through
  `getAssetPackPath`.

## IL2CPP
- 234 `il2cpp_*` functions. Resources are extracted to `<filesDir>/il2cpp` ("il2cpp resources are
  up to date, not extracting" when `unity_app_guid` matches).

## What the tools need for this version
- loader/android: done (libmediandk stubs, the bionic fortify shims, `__mmap2`, `sys_signame`,
  `newlocale`).
- runtime/jni (done): `nativeInjectEvent(InputEvent, int)` (the int is 0), `nativeGetNoWindowMode`,
  the new UnityPlayer methods and framework members, plus everything from 2019.4-2021.3.
- converter: OBB GUID file; otherwise as 2021.3.
