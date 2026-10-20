# Unity 2021.3.45f2 (latest public 2021 LTS patch)

Installer: `UnitySetup-Android-Support-for-Editor-2021.3.45f2.exe` (changeset 88f88f591b2e, 417 MB).
Compared with 2020.3.48f1. 2021.3.45f2 (2025-10) is a re-release of 2021.3.45f1 (2024-10), the last regular patch.

## Package
| | |
|---|---|
| ABIs | Mono: armeabi-v7a. IL2CPP: armeabi-v7a, arm64-v8a, x86, x86_64 |
| Mono runtimes | `libmonobdwgc-2.0.so` (its SONAME is **`libmonoboehm-2.0.so`**), `libMonoPosixHelper.so`, new **`libmono-native.so`** (System.Native for the newer BCL; `DT_NEEDED libmonoboehm-2.0.so`, imports `mono_*` from it). The loader matches DT_NEEDED against loaded modules' SONAMEs, so this resolves once libmonobdwgc is loaded |
| toolchain | NDK r21d, min API 22 on every ABI, Thumb-2; REL/RELA + PLT only; 4 KiB alignment again on arm64 |

## Native imports
- `libunity` (both ABIs) now also imports fortify functions bionic has and glibc lacks:
  **`__FD_SET_chk`, `__FD_ISSET_chk`** (armeabi-v7a too now), **`__strlen_chk`** (arm64);
  plus `android_set_abort_message` (arm64). Glibc-provided fortify calls (`__memcpy_chk`,
  `__vsnprintf_chk`, ...) resolve to the host.
- Mono libraries (armeabi-v7a), not provided before: **`__strlen_chk`, `__strchr_chk`,
  `__strrchr_chk`, `__strlcpy_chk`, `__mmap2`, `sys_signame`** (bionic's signal-name table, a data
  import). All provided now (`loader/hybris_compat.c`, bionic's checks and messages).
- New host-resolved calls worth a check: `wcstod`/`wcstof` and `strtold_l` in armeabi-v7a libunity
  return floating point: softfp wrappers in `loader/softfp.c` now (they returned garbage on
  armhf); `statvfs`/`fstatvfs` in libMonoPosixHelper: bionic's 32-bit `struct statvfs` is eleven
  `unsigned long`s (44 bytes; `fsblkcnt_t`/`fsfilcnt_t` are `unsigned long` there), glibc's 72
  bytes with an int after `f_fsid`: translated now; `mbsrtowcs`/`mbsnrtowcs`/`wcsnrtombs` (4-byte
  `mbstate_t`): translated; `sincos(f)` and the libm additions are wrapped.
- Strings: `ANativeWindow_setFrameRate`, `ATrace_*`, the `AMediaCodec/AMediaExtractor/AMediaFormat`
  set (dlsym'd from libmediandk in this version), `AChoreographer_*`, ARCore.

## JNI
- RegisterNatives: 48. New: `SoftInputProvider.nativeGetSoftInputType()I`,
  `nativeHidePreservedContent`, `ReflectionHelper.nativeProxyJNIFreeGCHandle(J)`;
  `JNIBridge.delete` is gone. Signatures otherwise as in 2020.3 (`nativeDone()Z`, byte[]
  `nativeUnitySendMessage`, long proxy handles).
- Java members libunity binds: 487 (+12 -12): `java.util.zip.ZipFile` (`getEntry`,
  `getInputStream`), `NetworkInterface.getByName/isUp`, `Settings.Global.DEVICE_NAME`,
  `PackageInfo.getLongVersionCode`, `MotionEvent.getSource`, `Activity.moveTaskToBack`.
- New UnityPlayer methods reachable by name: `getScreenBrightness`/`setScreenBrightness`,
  `hidePreservedContent`.
- Java side (128 classes, now obfuscated to `A`..`z0`): same model. The `UnityMain` thread's
  handler (message 2269) runs GL-thread jobs and `nativeRender`; pause, surface detach and
  destroy wait on the engine with timeouts (`UnityPlayer.SynchronizationTimeout`); the boolean
  from `nativeDone` decides whether the process is killed.

## Data
- `unity default resources`: format 22, "2021.3.43f1".
- Shader programs: serializer **202012090**. GL code entries still have 4 statistics ints, but
  **(2021.2+) the blob also holds parameter entries**: a program's subprograms are in
  `m_PlayerSubPrograms` (lists per hardware tier) with only blob index, GPU program type,
  keyword indices and requirements; their parameters are separate blob entries named by
  `m_ParameterBlobIndices`, plus `m_CommonParameters`. The Shader has a new `stageCounts`.
  A GLCore->GLES3 conversion must produce and index both kinds of entry.
- MonoManager: flags in `m_AssemblyTypes`. 64 `UnityEngine*.dll`.
- OBB: `main.<versionCode>.<package>.obb` names as before, but the `unity.build-id` meta-data
  string that 2017.4-2020.3 used to match an OBB is no longer in libunity (how 2021.3 checks an
  OBB was not traced).

## IL2CPP
- 230 `il2cpp_*` functions (new: `class_is_inited`, fixed GC allocations, liveness API changes,
  `unity_set_android_network_up_state_func`).

## What the tools need for this version
- bionic shims: done (the fortify set above, `sys_signame`, `__mmap2`, softfp `wcstod`/`wcstof`/
  `strtold_l`, `statvfs`).
- runtime/jni: as 2020.3 (+ `nativeGetSoftInputType` is only called by Java).
- converter: serializer 202012090 with separate parameter entries and per-tier subprograms.
