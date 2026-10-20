# Unity 2020.3.48f1 (last 2020 LTS)

Installer: `UnitySetup-Android-Support-for-Editor-2020.3.48f1.exe` (changeset b805b124c6b7, 374 MB).
Compared with 2019.4.40f1: a small step for the libraries, a format step for the data.

## Package
| | |
|---|---|
| ABIs | Mono: armeabi-v7a. IL2CPP: armeabi-v7a, arm64-v8a, x86, x86_64 |
| Mono runtimes | `libmonobdwgc-2.0.so` + `libMonoPosixHelper.so` (NDK r16b builds, imports as in 2019.4) |
| toolchain | NDK r19, min API 19 / 21, Thumb-2 on armeabi-v7a; REL/RELA + PLT only; arm64 64 KiB aligned |

## Native imports
- Same as 2019.4 plus `eglSurfaceAttrib`, `fchmod`, `fdopen`. Not provided: the same four
  (`__FD_SET_chk`, `__FD_ISSET_chk`, `android_set_abort_message` on arm64; `__mmap2` in the
  Mono libraries).
- `eglSurfaceAttrib` goes to the host libEGL. With the SDL host display, Unity's surface is the
  glue's fixed handle, so this call needs mapping to the real surface in `egl_host_shims`
  (otherwise EGL_BAD_SURFACE).

## JNI
- RegisterNatives: 45. New `OrientationLockListener.nativeUpdateOrientationLockState(I)`; the
  Google VR natives are gone. `nativeDone()Z`, byte[] `nativeUnitySendMessage` as in 2019.4.
- `ReflectionHelper.newProxyInstance` is now `(UnityPlayer, long, Class)` / `(UnityPlayer, long,
  Class[])`, with `beginProxyCall`/`endProxyCall` around Java-side proxy calls.
- Java members libunity binds: 487 (+21 -21): `Display.getSupportedModes` and `Display.Mode`
  (physical size, refresh rate), `View.setOnTouchListener`, `SensorManager.getDefaultSensor`,
  `PackageInfo.reqFeatures`; Parcel/IBinder/bindService users are gone.
- UnityPlayer: `loadNative` now does `System.load(<nativeLibraryDir>/libmain.so)` (falls back to
  `loadLibrary("main")`) and then NativeLoader; new `pauseJavaAndCallUnloadCallback`,
  `UnityPermissions`. Same `UnityMain` looper model.

## Data
- `unity default resources`: **format 22**, "2020.3.48f1". Format 22 has a 48-byte header
  (metadata size, 64-bit file size and data offset), so the version string starts at 48 and the
  platform word sits at 60 here. `android_data.sf_platform_offset` accepts formats 9..21 only.
- Shader programs: serializer 201806140 (as 2019.4), segmented blobs, 12-byte entries.
  SubPrograms group their parameters in one `m_Parameters` record (vector/matrix/texture/buffer
  params, constant buffers and bindings no longer separate fields).
- MonoManager: flags in `m_AssemblyTypes`.
- 63 `UnityEngine*.dll`.

## IL2CPP
- 225 `il2cpp_*` functions.

## What the tools need for this version
- everything listed for 2019.4, plus: SerializedFile format 22 in the converter; the new
  `newProxyInstance` signatures and `OrientationLockListener.nativeUpdateOrientationLockState`
  (done in runtime/jni); `eglSurfaceAttrib` mapping for the SDL display path.
