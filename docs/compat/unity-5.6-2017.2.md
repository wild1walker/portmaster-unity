# Unity 5.6, 2017.1 and 2017.2, Mono armeabi-v7a, vs 2017.4.9f1

Installers: `UnitySetup-Android-Support-for-Editor-5.6.7f1.exe`, `...-2017.1.5f1.exe`,
`...-2017.2.5f1.exe` (the last patches; classic non-solid NSIS, 28-byte first header: one range
request per file). Run in the emulated rig: free Linux games of 5.6.0f3 and 2017.2.0f3 (README,
"Players run"). Unity 5.0 - 5.5 are not covered (5.0 - 5.4 keep shader programs in another layout;
5.5 is unchecked).

## Package
| | 5.6.7f1 | 2017.1.5f1 | 2017.2.5f1 |
|---|---|---|---|
| Mono player | `Libs/armeabi-v7a/{libmain,libmono,libunity}.so` (no `MonoLibs/`) | `MonoLibs/`: `libmono.so`, `libmonobdwgc-2.0.so` (.NET 4.6, experimental) | same as 2017.1 |
| libunity DT_NEEDED | **`libmono.so`** (linked against Mono, not dlopened), libmain, libGLESv2, libstdc++, ... | as 2017.4 | as 2017.4 |
| relocations, alignment | REL + PLT, 4 KiB, ARM mode (old NDK, no `.note.android.ident`) | same | same |
| managed engine | one `UnityEngine.dll` | one `UnityEngine.dll` | `UnityEngine.dll` facade + 41 modules (42 files) |
| `unity default resources` | format 17, "5.6.4p1" | format 17, "2017.1.1p5" | format 17, "2017.2.0f1" |
| shader serializer | 201609010 (the 201708220 layout) | 201609010 | 201609010 |

## Native imports
- libunity 2017.1 and 5.6 import **`SHA1Init`, `SHA1Update`, `SHA1Final`** from libc (bionic's
  libc exported a NetBSD-style SHA-1 then): now loader shims with bionic's `SHA1_CTX` layout
  (`loader/hybris_compat.c`, tested with the FIPS 180-1 vectors). Everything else is provided.
- 5.6's libunity takes its `mono_*` imports from `libmono.so` (symbol version VER_1; the loader
  loads it from the library folder as a dependency, as Android's linker does).

## JNI and Java side
- RegisterNatives entries (all classes): 37 (5.6), 37 (2017.1), 47 (2017.2; Tango and GoogleVR
  ones among them). `initJni(Context)`, `nativeUnitySendMessage(String, String, String)`,
  `nativeDone()V`, as 2017.4.9.
- **5.x: `nativeFile(String)`**: Java mounts the APK (`getPackageCodePath()`) right after
  `initJni`, then each OBB whose signature the APK's `assets/bin/Data/settings.xml` lists (an MD5
  of the OBB's last 64 KiB): runtime/jni/up_legacy.c calls it for the APK and every main/patch OBB
  of the package. 2017.1+ mount the APK and OBBs natively (`unity.build-id`, as 2017.4).
- **To 2017.2: `nativeInitWWW(WWW.class)`** and the Java `com.unity3d.player.WWW` (a Thread
  created with id, URL, post data and headers; natives `headerCallback`, `readCallback`,
  `progressCallback`, `errorCallback`, `doneCallback`): up_legacy.c answers as a phone without a
  network (`errorCallback(id, "java.net.UnknownHostException: ...")`, from the request's own
  thread); file: URLs are read. `UnityWebRequest` (Java, 5.6+, `nativeInitWebRequest`) is
  runtime/jni's 2017.4 one.
- 5.6 / 2017.1: `nativeSetInputCanceled(Z)` in place of `nativeSoftInputCanceled()` (soft input
  only); 5.6: `nativeActivityIndicatorStyle()I`, `isQuiting()Z`, no `nativeIsAutorotationOn`.
- Java members libunity binds: 363 (5.6), 379 (2017.1), 384 (2017.2); 2017.4.9: 398.

## Data
- MonoManager (`GetAvailableDllAssemblyMask` disassembled): 5.6 and 2017.1 mark slot 0
  (`UnityEngine.dll`) loaded, reserve slot 1 and check 2+; 2017.2 marks slots 0 - 43 loaded
  without looking for the files, skips 44, checks 45+ (2017.3/2017.4: 0 - 46 checked, 47 skipped).
  android-data's checks use these counts.
- GraphicsSettings: 5.x has no `m_ShaderDefinesPerShaderCompiler` (nothing to add).
- 2017.2 games built against the engine modules may reference standalone-only types
  (`UnityEngine.MovieTexture` in `UnityEngine.AudioModule`): the Android module lacks them and the
  player stops at a `TypeLoadException`; android-data ships the game's desktop copy of such a
  module (android-data.md, section 4).

## What the tools need for this version
- converter: the 201609010 serializer, the assembly slots, 5.x GraphicsSettings, desktop engine
  modules where needed: done.
- runtime: `nativeFile`, `nativeInitWWW` + WWW, the SHA1 shims: done. Not done: the 5.x
  `settings.xml` itself (the Java side's splash and OBB flags; nothing in libunity reads it).
