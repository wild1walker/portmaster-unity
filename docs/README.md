# Documentation

The [main README](../README.md) is the place to start: what the tools are, what they can port and
the five-step quick start. This page lists every document and the repository's layout.

## Pages

| Page | What is in it |
|---|---|
| [porting.md](porting.md) | the porter's guide: `port check`, `port.toml`, the routes, the setup on the handheld, packaging, the rehearsal, testing on a device, the steps by hand, typical first problems, a game's own patches |
| [compat/README.md](compat/README.md) | Unity version coverage: the matrix per version, results on a handheld and in the emulator, the gap list; one page per Unity line |
| [roadmap.md](roadmap.md) | the ways a game can get in, and the work items |
| [building.md](building.md) | what building needs, and converting and running a game by hand |
| [runtime.md](runtime.md) | `ua_host`: its folder layout and every `UA_*` switch |
| [input.md](input.md) | pads, Rewired, InControl, the Input System package, pointer mode, keys from the pad, the on-screen keyboard |
| [plugins.md](plugins.md) | native plugins, the plugin map, FMOD, Steam and other service stubs, HTTPS |
| [shaders.md](shaders.md) | the shader converter (OpenGL Core and Direct3D 11 to GLSL ES), checks, what costs what on a Mali-G31 |
| [android-data.md](android-data.md) | the APK and OBB the converter writes, per Unity version; StreamingAssets; saves |
| [il2cpp.md](il2cpp.md) | Mono desktop games compiled with IL2CPP for the 64-bit ARM player, on a PC or on the handheld (`--on-device`) |
| [apk.md](apk.md) | games owned as Android builds (`pmunity apk`) |
| [arm64.md](arm64.md) | the 64-bit ARM loader |
| [box64-route.md](box64-route.md) | the game's own Linux x86-64 build under box64 |
| [design.md](design.md) | the design and the interfaces between the parts |

Component notes: [runtime/android](../runtime/android/README.md),
[runtime/jni](../runtime/jni/README.md), [runtime/media](../runtime/media/README.md),
[tools/asmpatch](../tools/asmpatch/README.md), [tools/box64](../tools/box64/README.md).

## Repository layout

| folder | what |
|---|---|
| `runtime/loader/` | ELF loader for Android ARMv7 libraries on glibc: relocation, symbol resolution, bionic -> glibc through libhybris's hooks plus our own fixes (futex semaphores, mutex/cond init, clocks, stat/sigaction/setjmp/ctype translations, bionic's fortify functions and the flag/layout translations newer players need), softfp thunks for float-passing GL and libc calls |
| `runtime/jni/` | a JavaVM/JNIEnv in C and Unity's `UnityPlayer` activity: lifecycle, the UnityMain looper, surface, input events, FMOD's AudioTrack, PackageManager, PlayerPrefs, android.media's video classes |
| `runtime/android/` | liblog, libandroid (ANativeWindow on Mali fbdev, looper, sensors), OpenSL ES (to SDL2, or PCM piped into `pw-cat`/`pacat` when a sound server holds the card), libmediandk (the NDK's media API over `runtime/media`), EGL glue |
| `runtime/media/` | video for Unity's VideoPlayer: WebM and MP4 demuxers, decoders (libvpx VP8/VP9, libvorbis, libopus, OpenH264; the system's libavcodec when there is one), the SurfaceTexture whose frames reach Unity's external texture through an EGLImage |
| `runtime/host/` | `ua_host` (ties it together; SDL2 pads -> Android gamepads, pointer mode (a pad-driven mouse and cursor for mouse-only menus) and pad -> keyboard keys; frame statistics, shader dump/override, GL frame tracer, hitch sampler) and device checks (`egltest`, `sdltest`, `audiocheck`, `shadercheck`) |
| `convert/pmunity/` | the data converter: `shaders` (OpenGL Core -> GLSL ES 3.00), `textures` (BC/DXT -> ASTC, optional downscale), `settings`, `audio`, `android-data` (APK + OBB for the player), `macpkg` (unpacks macOS installers such as GOG's), `player fetch` (Unity's Android player of a version, about 10 MB of its installer), `apk` (a game's own Android build, APK/XAPK/AAB + OBB, made ready for `ua_host`: [apk.md](apk.md)), `port` (`check`: what a game is and needs and which route fits; `new`: a complete PortMaster port from a short port.toml; `package`: its release zip), `prefs` (PlayerPrefs between a PC and the handheld). Runs on a PC or on the handheld itself (the ports' first-launch setup does) |
| `tools/` | `asmpatch/` (generic patches to a game's assemblies for the Android player, e.g. Rewired on Android or StreamingAssets read as files, and a survey of input stacks and file access; C#, Mono.Cecil), `port/` (the setup's Python bundle for ports, `rehearse.sh`: a port's first launch in a handheld-like container, then the game in the emulated rig), device scripts (`device/`), log analysis (`analysis/`), an emulated test rig with Mesa, armv7 under QEMU or native aarch64 (`emu/`, `EMU_ARCH`), the box64 route's builds and rig (`box64/`), HLSLcc for Windows builds' shaders (`hlslcc/`), IL2CPP toolchain helpers (`il2cpp/`), reverse-engineering helpers for new player versions (`re/`) |
| `docs/` | these pages |

## Shaders in short

`pmunity shaders` gives every OpenGL Core program a GLSL ES 3.00 twin with the same numbers:
32-bit floats and full-precision samplers as on a desktop GPU, plus two rewrites that keep every
operation (constant tables as `const` arrays, fixed-count loops unrolled). A Windows build's
Direct3D 11 programs are translated with Unity's HLSLcc (`--from d3d11`). `runtime/host/shadercheck`
compiles and links all of a game's programs with the handheld's own driver and times their
fragment shaders. [shaders.md](shaders.md) has the details and what costs what on a Mali-G31.
