# Unity 2017.4.40f1 (last 2017 LTS) vs the 2017.4.9f1 the runtime implements

Installer: `UnitySetup-Android-Support-for-Editor-2017.4.40f1.exe` (changeset 6e14067f8a9a, 285 MB).
Analysed: Release players of both backends, `classes.jar`, `unity default resources`, templates.

## Package
| | |
|---|---|
| ABIs | Mono: armeabi-v7a, x86. IL2CPP: armeabi-v7a, **arm64-v8a**, x86 (2017.4.9 had no arm64) |
| Mono runtimes | `libmono.so` (.NET 3.5 profile, what Cuphead uses) and `libmonobdwgc-2.0.so` (.NET 4.x) |
| toolchain | same old one as 2017.4.9: clang 3.8 / GCC 4.9, no `.note.android.ident`; armeabi-v7a code is ARM mode |
| relocations | REL (armv7) / RELA (arm64) + PLT only. No packed (APS2) or RELR tables, no IFUNC, no PT_TLS; segments 4 KiB aligned |
| DT_NEEDED | unchanged: libmain, libc, libm, libandroid, libdl, liblog, libGLESv2, libstdc++, libz, libEGL |

## Native imports
- armeabi-v7a `libunity`: 360 imports, all provided (runtime shims or glibc). New since 2017.4.9:
  `dladdr` (loader), `exp2`, `lroundf`, `rintf` (softfp wrappers exist).
- arm64-v8a `libunity`: 329 imports, all provided; the two `__google_potentially_blocking_region_*`
  are weak and resolve to 0, as on ARMv7. The stack protector reads `tpidr_el0+0x28` (the loader's
  guard slot handles it). `libmain` arm64: 18 imports, all provided.
- `libmono.so`, `libmonobdwgc-2.0.so`: import sets identical to 2017.4.9.

## JNI
- RegisterNatives: 45 natives, as in 2017.4.9, but **`UnityPlayer.nativeUnitySendMessage` now takes
  `(String, String, byte[])`** (the message as UTF-8 bytes). `runtime/jni` used to call it with
  three Strings; it now passes the UTF-8 bytes when that is what libunity registered.
- Java members libunity binds: 402 (2017.4.9: 398). New: `ApplicationInfo.splitPublicSourceDirs`
  (split APKs; not defined in runtime/jni, reads as null), `Context.getApplicationInfo`,
  `AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE`, `MotionEvent.getPressure(int)`,
  `ServiceConnection.onNullBinding` (proxy).
- Java side: same classes and startup as 2017.4.9 (`ReflectionHelper.endUnityLaunch` added).

## Data
- `unity default resources`: SerializedFile format 17, version string "2017.4.3f1", platform 13.
- Shader programs: serializer 201708220 (already in `blob.py` LAYOUTS); GLES2/GLES3 entries parse
  and write back unchanged.
- MonoManager: `GetAvailableDllAssemblyMask` is the same code: slots 0..46 are engine modules,
  47 is skipped, the game starts at 48 (`BUILTIN_ASSEMBLY_SLOTS = 48` holds).
- 45 `UnityEngine*.dll` in the Android Managed set (as 2017.4.9).

## IL2CPP
- `libunity` asks Java for the library (`ClassLoader.findLibrary("il2cpp")`, then `System.load`;
  both exist in runtime/jni), `dlopen`s it and looks up 175 `il2cpp_*` functions.
- `UnityInitApplication` then copies IL2CPP's data from the APK's `assets/bin/Data/Managed` to
  `<filesDir>/il2cpp` (a writable files dir is required; failure is fatal) and initializes IL2CPP
  with `<filesDir>/il2cpp/etc` as its config dir. The game ships `global-metadata.dat` under
  `assets/bin/Data/Managed/Metadata/` and `libil2cpp.so` under `lib/<abi>/`; which of the two
  copies IL2CPP reads the metadata from was not traced.

## What the tools need for this version
- runtime/jni: the byte[] `nativeUnitySendMessage` (done: natives are called with the signature
  libunity registered).
- Everything else that works for 2017.4.9 should work: same libraries' interfaces, same data
  formats. arm64-v8a players exist here, so 2017.4.16+ IL2CPP Android builds can use the arm64 runtime.
