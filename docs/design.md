# Design

Unity games built for desktop run with Unity's own ARMv7 Android player instead of an x86 player
under an emulator. The player libraries are loaded on the handheld's 32-bit Linux userland by our
own loader, with a fake Java layer and small Android API shims. No Android OS, no emulation. The
first game was Cuphead (Unity 2017.4.9f1, Mono); before this route it ran as the x86-64 Linux
player under box64, at roughly half the frame rate.

## Target devices
- 64-bit ARM Linux handhelds with a 32-bit (armhf) userland: an AArch64 kernel with
  `CONFIG_COMPAT=y`, `/lib/ld-linux-armhf.so.3`, 32-bit GL ES / EGL / SDL2 libraries. Developed on
  an Anbernic RG34XX-SP with Knulli (Allwinner H700, 4x Cortex-A53, Mali-G31 MP2, 1 GB RAM,
  720x480): glibc 2.40 in `/lib32`, libmali fbdev flavor (no GBM/DRM/Wayland), SDL2 2.32.
- Build for **armhf (ARMv7, hard float), glibc <= 2.36** (the Debian bookworm cross toolchain in
  the Dockerfiles).

## Inputs (you provide them; nothing of them is in this repository)
- Unity's Android player for the game's exact Unity version, unpacked from the editor's "Android
  Build Support" installer (Unity's download archive; the .exe unpacks with 7-Zip):
  - `Variations/mono/Release/Libs/armeabi-v7a/libunity.so`, `libmain.so`
  - `Variations/mono/Release/MonoLibs/armeabi-v7a/libmono.so`
  - `Variations/mono/Release/Symbols/armeabi-v7a/libunity.sym.so` (symbols, for the debugging tools)
  - `Variations/mono/Managed/UnityEngine*.dll` (the Android build of Unity's managed assemblies)
  - `Data/unity default resources` (the Android player's built-in resources)
  - the Development variants (`Variations/mono/Development/...`) for debugging
- The game, a desktop build you own: macOS (its OpenGL Core shaders are converted for GLES) or
  Linux. Windows builds carry Direct3D shaders only.

## Layout
- `runtime/loader/`   ELF loader for Android .so files on glibc + bionic compatibility wrappers
                      (armeabi-v7a on armhf; arm64-v8a on aarch64: [arm64.md](arm64.md))
- `runtime/jni/`      fake JavaVM/JNIEnv and the UnityPlayer Java side written in C
- `runtime/android/`  shims: liblog, libandroid (ANativeWindow, AAssetManager, ALooper,
                      AConfiguration), OpenSL ES (audio, to SDL2 or a pipe), EGL glue
- `runtime/host/`     `ua_host`, the program tying it together, and device checks
- `convert/pmunity/`  the game's desktop data -> what the Android player reads (APK + OBB)
- `tools/`            device scripts, analysis, the emulated test rig, reverse-engineering helpers

## Interfaces between parts (C, armhf)
- loader (`loader/so_loader.h`):
  - `so_module *so_load(const char *path, const so_shim *shims_null_terminated...)` maps, relocates,
    resolves; `void *so_sym(so_module *, const char *name)`; `void so_run_init(so_module *)`.
  - Symbol resolution order: override tables registered with `so_add_shims(const so_shim *)`
    (`typedef struct { const char *name; void *fn; } so_shim;`), then other loaded Android modules,
    then the host (`dlsym` on glibc / host libs), else a stub that prints the symbol name and aborts.
  - `dlopen`/`dlsym`/`dlclose` called *by* the Android libraries are routed through the loader:
    Android libs we load ourselves, host libs (libEGL/libGLESv2/libGLESv3/libm/libz) via real dlopen,
    Android-only libs (liblog, libandroid, libOpenSLES, ...) to our shims.
- jni (`jni/fakejni.h`): `JavaVM *fj_vm(void); JNIEnv *fj_env(void);` objects are opaque handles;
  `fj_register_class/method` let C code implement Java methods by (class, name, signature);
  `RegisterNatives` calls are recorded: `void *fj_native(const char *cls, const char *name, const char *sig)`.
  Every JNI call is logged (env `FJ_TRACE=1`) so we learn what Unity uses.
- android (`android/android_shims.h`): exports `so_shim android_shims[]`; ANativeWindow is backed by
  an SDL2 window (or a Mali fbdev window), AAssetManager by a directory.

## Testing
- Locally: Docker (QEMU armv7) runs the armhf binaries without a GPU: loading, relocation,
  JNI_OnLoad, data reading, unit tests. `tools/emu/` runs a whole game there on Mesa's software GL
  (slow, but frame contents and GL work counts match the device; deterministic replays compare
  pictures pixel for pixel).
- On the device: `tools/device/run.sh` and friends over SSH.
- Ideas came from earlier work (all code here is our own): the PS Vita so_loader ports
  (`so_util.c`), FalsoJNI (fake JNI for Vita ports), PortMaster's gmloader (GameMaker's Android
  runner on Linux), and libhybris (whose bionic hooks the loader compiles in).
