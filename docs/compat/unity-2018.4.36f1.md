# Unity 2018.4.36f1 (last 2018 LTS)

Installer: `UnitySetup-Android-Support-for-Editor-2018.4.36f1.exe` (changeset 6cd387d23174, 471 MB).
Compared with 2017.4.40f1 (and the 2017.4.9f1 baseline).

## Package
| | |
|---|---|
| ABIs | Mono: armeabi-v7a, x86. IL2CPP: armeabi-v7a, arm64-v8a, x86 |
| Mono runtimes | `libmono.so` (.NET 3.5, deprecated) + `libmonobdwgc-2.0.so` (.NET 4.x) + new `libMonoPosixHelper.so` (Mono.Posix P/Invoke target) |
| toolchain | NDK r16b; min API 16 (armeabi-v7a), 21 (arm64-v8a); ARM mode code on armeabi-v7a |
| relocations | REL / RELA + PLT only; no APS2, RELR, IFUNC or PT_TLS |
| alignment | arm64-v8a segments 64 KiB aligned (fine for a 4 KiB-page loader), armeabi-v7a 4 KiB |
| DT_NEEDED | unchanged |

## Native imports
- All imports of `libmain`, `libunity` (both ABIs), `libmono`, `libmonobdwgc-2.0` and
  `libMonoPosixHelper` are provided by the runtime or glibc.
- libunity stopped importing the unwinder (`_Unwind_*`, `__gnu_unwind_frame`, `__aeabi_unwind_cpp_pr*`):
  it carries its own. arm64-v8a libunity/libmain no longer import `__stack_chk_guard` (TLS slot only).
- New in `libMonoPosixHelper` and going straight to glibc: file/xattr calls (`creat`, `mknod`,
  `pathconf`, `readv`, `utimes`, `ttyname_r`, `tmpfile`...). Only reached through Mono.Posix.

## JNI
- RegisterNatives: 44. New: `nativeSendSurfaceChangedEvent` (Java calls it after a surface size
  change), `nativeSetInputSelection`, `validateCertificateCallback` (UnityWebRequest TLS),
  Camera2 natives (`initCamera2Jni`, `nativeFrameReady`, `nativeSurfaceTextureReady`),
  `ReflectionHelper.nativeProxyLogJNIInvokeException`. Tango natives are gone.
- `nativeUnitySendMessage(String, String, byte[])`, as in 2017.4.40.
- Java members libunity binds: 458 (+60). New groups: window insets and display cutouts
  (`View.getRootWindowInsets`, `WindowInsets`, `DisplayCutout`), input device ids
  (`InputManager.getInputDeviceIds/getInputDevice`, `InputDevice.getVendorId/getProductId`),
  more `MotionEvent` getters (touch major/minor, historical values), `Process.setThreadPriority`,
  sustained performance mode, `TrustManagerFactory`/`X509TrustManager` (certificate checks),
  `Resources.getIdentifier`, `Activity.findViewById`. runtime/jni now defines the members of known
  classes (insets: none; input device ids; `findViewById`: null; sustained performance: not
  supported); the other classes answer 0/null, which Unity treats as "not available".
- Java side (`classes.jar`, 95 classes): the same UnityPlayer model as 2017.4 (`initJni`,
  `nativeInitWebRequest`, a `UnityMain` looper thread posting frames to `nativeRender`).
  `showSoftInput` gained parameters; `UnityPlayerNativeActivity`/`ProxyActivity` are gone.

## Data
- `unity default resources`: format 17, "2018.4.24f1", platform 13.
- Shader programs: serializer **201802150**. GLES2/GLES3 entries have 4 statistics ints and
  write back unchanged with that layout (`blob.py` reads them only as "guessed"; add to LAYOUTS).
- **MonoManager changed**: `GetAvailableDllAssemblyMask` no longer reserves slots 0..46 for
  engine modules; it walks all `m_AssemblyNames` and filters by the per-assembly flags in
  `m_AssemblyTypes`. `BUILTIN_ASSEMBLY_SLOTS = 48` does not apply from this version on.
- 59 `UnityEngine*.dll` in the Android Managed set.

## IL2CPP
- 194 `il2cpp_*` functions looked up; same loading and resource-copy path as 2017.4.

## What the tools need for this version
- runtime/jni: byte[] `nativeUnitySendMessage`, `nativeSendSurfaceChangedEvent` after a surface
  change, the 9-argument `showSoftInput` and the new framework members (done).
- converter: shader serializer 201802150 in LAYOUTS; MonoManager handling by `m_AssemblyTypes`.
