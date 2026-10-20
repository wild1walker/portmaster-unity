# portmaster-unity

Run Unity games on ARM Linux handhelds with **Unity's own ARMv7 Android player**, natively: no
Android, no emulator, no x86 translation. A small runtime loads the player's libraries
(`libmain.so`, `libunity.so`, `libmono.so`) on the handheld's 32-bit Linux userland, plays the
Java side of Unity's Android activity in C, and stands in for the handful of Android APIs the
player uses. A converter turns the game's desktop data (macOS or Linux build) into the APK + OBB
that player reads: GLES3 shaders, ASTC textures, Android platform settings.

First game: Cuphead (Unity 2017.4.9f1, Mono) on an Anbernic RG34XX-SP (Knulli), in
[portmaster-cuphead](https://github.com/wild1walker/portmaster-cuphead). Most of the game runs at
55-60 fps there, against roughly half that as the x86-64 Linux player under box64.

**Nothing from Unity or from any game is in this repository.** You provide Unity's Android player
(from Unity's editor installers) and your own copy of the game; the tools convert them on your
computer.

## Layout

| folder | what |
|---|---|
| `runtime/loader/` | ELF loader for Android ARMv7 libraries on glibc: relocation, symbol resolution, bionic -> glibc through libhybris's hooks plus our own fixes (futex semaphores, mutex/cond init, clocks, stat/sigaction/setjmp/ctype translations, bionic's fortify functions and the flag/layout translations newer players need), softfp thunks for float-passing GL and libc calls |
| `runtime/jni/` | a JavaVM/JNIEnv in C and Unity's `UnityPlayer` activity: lifecycle, the UnityMain looper, surface, input events, FMOD's AudioTrack, PackageManager, PlayerPrefs, android.media's video classes |
| `runtime/android/` | liblog, libandroid (ANativeWindow on Mali fbdev, looper, sensors), OpenSL ES (to SDL2, or PCM piped into `pw-cat`/`pacat` when a sound server holds the card), libmediandk (the NDK's media API over `runtime/media`), EGL glue |
| `runtime/media/` | video for Unity's VideoPlayer: WebM and MP4 demuxers, decoders (libvpx VP8/VP9, libvorbis, libopus, OpenH264; the system's libavcodec when there is one), the SurfaceTexture whose frames reach Unity's external texture through an EGLImage |
| `runtime/host/` | `ua_host` (ties it together; SDL2 pads -> Android gamepads, pointer mode (a pad-driven mouse and cursor for mouse-only menus) and pad -> keyboard keys; frame statistics, shader dump/override, GL frame tracer, hitch sampler) and device checks (`egltest`, `sdltest`, `audiocheck`, `shadercheck`) |
| `convert/pmunity/` | the data converter: `shaders` (OpenGL Core -> GLSL ES 3.00), `textures` (BC/DXT -> ASTC, optional downscale), `settings`, `audio`, `android-data` (APK + OBB for the player), `macpkg` (unpacks macOS installers such as GOG's), `apk` (a game's own Android build, APK/XAPK/AAB + OBB, made ready for `ua_host`: docs/apk.md). Runs on a PC or on the handheld itself (portmaster-cuphead's PortMaster setup does) |
| `tools/` | `asmpatch/` (generic patches to a game's assemblies for the Android player, e.g. Rewired on Android, and an input survey; C#, Mono.Cecil), device scripts (`device/`), log analysis (`analysis/`), an emulated test rig with Mesa, armv7 under QEMU or native aarch64 (`emu/`, `EMU_ARCH`), reverse-engineering helpers for new player versions (`re/`) |
| `docs/` | design, runtime reference (every `UA_*` switch), input (pads, pointer mode, input stacks), the data format notes, shader notes, porting a new game |

## Requirements

- A handheld with an AArch64 kernel that runs 32-bit programs and has a 32-bit userland with
  GLES 3 / EGL and SDL2 (e.g. Knulli on Allwinner H700 devices: `/lib32`, `/usr/lib32`).
- To build: Docker (the Dockerfiles hold Debian bookworm armhf cross toolchains), Python 3.10+
  with `convert/requirements.txt`, 7-Zip (to unpack Unity's installer), .NET SDK if a game's port
  patches its assemblies.
- Unity's Android player for **the game's exact Unity version**: install, or just download, the
  Unity editor's "Android Build Support" for that version from Unity's download archive, and unpack
  it (`7z x UnitySetup-Android-Support-for-Editor-<version>.exe`). The folder with `Variations/`
  and `Data/` is the "player package" below. Unity's licence terms apply to it; it is not
  redistributed here.
- The game: a desktop build you own. The macOS build is the usual choice (its OpenGL Core shaders
  convert to GLES 3); Windows builds carry only Direct3D shaders.

## Making a port

```sh
git clone --recurse-submodules https://github.com/wild1walker/portmaster-unity
cd portmaster-unity
runtime/build.sh                              # -> runtime/host/out/ua_host (+ device checks)
pip install -r convert/requirements.txt
cd convert
python -m pmunity convert "<Game>.app/Contents/Resources/Data" work/data --config game.json
python -m pmunity android-data --src work/data --pkg <player package> --out work/android --pack all
```

Then copy to the handheld: `ua_host`, the player's three libraries into `lib/`, and the contents
of `work/android/zip/` (APK, OBB, `android_package.json`, `assets/bin/Data/Managed/etc`). Start it
with `LD_LIBRARY_PATH=/lib32:/usr/lib32 ./ua_host "$PWD"` (see `tools/device/run.sh` and
docs/runtime.md). [docs/porting.md](docs/porting.md) goes through a new game step by step: what
the converter's config holds, what to check first, and what a game may need beyond it (assembly
patches, a helper DLL).

## Shaders

`pmunity shaders` gives every OpenGL Core program a GLSL ES 3.00 twin with the same numbers:
32-bit floats and full-precision samplers as on a desktop GPU, plus two rewrites that keep every
operation (constant tables as `const` arrays, fixed-count loops unrolled). `runtime/host/shadercheck`
compiles and links all of a game's programs with the handheld's own driver and times their
fragment shaders. [docs/shaders.md](docs/shaders.md) has the details and what costs what on a
Mali-G31.

## Licence

MIT (see [LICENSE](LICENSE)) for everything in this repository except the third-party parts listed
in [THIRD_PARTY.md](THIRD_PARTY.md): libhybris (a git submodule, Apache 2.0) and AOSP's `jni.h`
(Apache 2.0). `ua_host` also links the video decoders of Debian's packages (libvpx, libvorbis,
libogg, libopus: BSD-3-Clause; OpenH264: BSD-2-Clause), whose licence texts go with it.

Unity is a trademark of Unity Technologies. This project is not affiliated with or endorsed by
Unity Technologies or by any game's publisher.
