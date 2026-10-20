# Runtime reference

`ua_host <root>` runs a game with Unity's ARMv7 Android player. `<root>` holds:

| path | what |
|---|---|
| `lib/libmain.so`, `lib/libunity.so`, `lib/libmono.so` | the player (Release variant; `UA_LIBDIR` for another folder, e.g. the Development player) |
| `<name>.apk`, `main.<code>.<package>.obb` | the game's data, from `pmunity android-data --pack all` |
| `android_package.json` | written beside the APK by the data tool: package, build id, APK name (read by ua_host) |
| `assets/bin/Data/Managed/etc/` | Mono's config, read relative to the working directory (the data tool's zip folder has it) |
| files the game reads beside its data | e.g. a GOG build's `goggame-*.info` (`--beside`) |
| `data/` | created: `files/`, `cache/` (Unity's shader cache), `external/` (persistentDataPath: saves), `shared_prefs/` (PlayerPrefs) |

Run it with the 32-bit libraries first: `LD_LIBRARY_PATH=/lib32:/usr/lib32 ./ua_host "$PWD"`
(`tools/device/run.sh` does that, silent by default). Pads: SDL2 GameController, mapped by
`SDL_GAMECONTROLLERCONFIG` (PortMaster's launchers set it through `get_controls`).

## Display

`ua_host` picks the EGL window Unity draws into before it starts Unity (`runtime/host/display.c`):

| `UA_DISPLAY` | what |
|---|---|
| `auto` (default) | `fbdev` when the framebuffer (`ANDROID_FBDEV`, default `/dev/fb0`) exists and libEGL is an fbdev-only one: no window-system platform (`EGL_*_platform_gbm/wayland/x11/xcb/device/surfaceless`) in its client extensions, which Mesa and the Wayland/GBM/X11 flavors of vendor drivers always list. Otherwise `sdl`; if SDL cannot open a GLES window, `fbdev`. The choice and the reason are logged (`[display] auto: ...`) |
| `fbdev` | Mali's fbdev EGL, exactly the path the host always had (Knulli on the Allwinner H700): nothing is probed or opened |
| `sdl` | an SDL2 window (Wayland, KMSDRM, X11 through EGL, SDL's offscreen driver; `SDL_VIDEODRIVER` picks one) whose EGL display, config and window surface Unity is given (`runtime/android/README.md`, "Host display"); stops if SDL cannot open one |

With `UA_EGL_PBUFFER=1` (tests on Mesa's surfaceless platform) the fbdev code path is always taken.

The SDL window is fullscreen on the desktop mode, with a high-DPI drawable; Unity is told the
drawable size (`SDL_GL_GetDrawableSize`) and the display's refresh rate, and renders at that size
(`ANDROID_WINDOW_SIZE` does not apply). Its config is what Unity's Android player asks for: GLES
3.0, RGBA8888, depth 24, stencil 8 (then weaker ones, then GLES 2.0). Vsync is on (swap interval
1, EGL's default). The main loop keeps pumping SDL events (Wayland needs it); closing the window
quits like SIGTERM.

| variable | effect (sdl) |
|---|---|
| `UA_DISPLAY_WINDOW=WxH` | a WxH window instead of fullscreen (desktop tests) |
| `UA_DISPLAY_SAMPLES=<n>` | multisampled window surface, for a game whose quality settings use MSAA (Unity's Android player renders MSAA straight into the window surface) |
| `UA_DISPLAY_SRGB=1` | sRGB window surface, for a linear-color-space game (logged as a warning when Unity asks for one and this is off) |
| `UA_SWAP_INTERVAL` | also the SDL window's initial interval (SDL paces Wayland and KMSDRM swaps itself) |
| `SDL_VIDEODRIVER`, `SDL_VIDEO_EGL_DRIVER` | SDL's own; `SDL_VIDEO_EGL_DRIVER` and `ANDROID_SHIM_LIBEGL` are set to each other when only one is given (SDL and Unity must share one libEGL) |

Test without a device: `runtime/host/test/display_test.sh` (aarch64 in Docker, Mesa llvmpipe):
Unity's EGL call sequence on a render thread while the main thread pumps SDL events, drawing,
reading back and presenting, with SDL 2.0.10 (X11 on Xvfb, Wayland on a headless weston, the
fbdev fallback) and SDL 2.26 (Wayland on a headless sway at 1920x1080, X11, offscreen). The whole
game also runs this way in the emulated rig: `UA_DISPLAY=auto SDL_VIDEODRIVER=x11` against an
Xvfb instead of `UA_EGL_PBUFFER=1`.

## Host switches (environment)

| variable | effect |
|---|---|
| `UA_LIBDIR` | player libraries (default `<root>/lib`) |
| `UA_PACKAGE`, `UA_META` | package name; manifest meta-data (`unity.build-id=...` ties the APK to its OBB). Default: from `android_package.json` |
| `UA_APK_NAME` | the APK's name without `.apk` (default: from `android_package.json`, else the only `.apk` in the root) |
| `UA_OBB_DIR` | where the OBB is (default: beside the APK) |
| `UA_PAD_NAME` | name the pad reports (Rewired knows Android pads by name: `Xbox Wireless Controller`) |
| `UA_INPUT_SCRIPT` | scripted presses: `"<sec>[@<period>]:<button>[~<hold>][+...] ..."` |
| `UA_FPS` | frame stats every N s (avg/p99/worst, late >25 ms, hitch >50 ms, GC); `[hitch]` lines >100 ms |
| `touch <root>/shot.req` | screenshot of the next frame -> `<root>/shot-<n>.ppm` |
| `UA_MONO_OPTIONS` | Mono options; `--trace=T:Class,M:Class:Method` (no `<` in specs) |
| `UA_MONO_TRACE` | log Mono class/method/field lookups that fail |
| `FJ_TRACE` | log every JNI call |
| `ANDROID_SLES_OUTPUT` | `auto` (SDL, else pipe), `sdl`, `pipe`; `ANDROID_SLES_PIPE_CMD` overrides the client |
| `UA_SHADER_DUMP=<dir>` | write every GLSL source Unity compiles to `<dir>/<fnv64>.glsl` (runtime/host/glhooks.c) |
| `UA_SHADER_OVERRIDE=<dir>` | compile `<dir>/<fnv64>.glsl` instead of that source (both switches turn Unity's program binary cache off) |
| `UA_SWAP_INTERVAL` | eglSwapInterval forced (0: no vsync; Unity's own frame cap still applies) |
| `UA_DISPLAY`, `UA_DISPLAY_*` | the EGL window: Mali fbdev or SDL2 (see Display above) |
| `UA_GL_TRACE=<s>` | one whole frame every `<s>` seconds logged pass by pass: framebuffer, size, clears, draws, vertices, invalidates, uploads (runtime/host/gltrace.c) |
| `UA_GC_HEAP=<bytes>` | Mono's initial GC heap (default 64 MB, `0` = Boehm's own; `GC_INITIAL_HEAP_SIZE` wins) |
| `UA_QUIT_CLEANUP=1` | run Unity's engine cleanup (nativeDone) at quit; by default the host pauses Unity (PlayerPrefs saved) and exits |
| `UA_AUDIO_RATE`, `UA_AUDIO_FRAMES` | output rate and buffer size AudioManager reports to FMOD (default 48000, 512) |
| `UM_TRACE`, `UM_VIDEO_BUFFERS`, `UM_VPX_THREADS`, `UM_H264`, `UM_FFMPEG` | video (Unity's VideoPlayer, `runtime/media`): per-buffer log, a video decoder's output queue (default 4), libvpx threads, OpenH264 for every H.264 stream, the system's libavcodec off / from a path (see Video below) |
| `UA_HITCH_TRACE=<ms>` | in frames longer than `<ms>`, sample `UA_HITCH_THREADS` (default `UnityMain`) every `UA_HITCH_EVERY` ms (20): pc, lr, return addresses as module+offset (runtime/host/hitches.c; `tools/analysis/sym.py` names libunity offsets, `tools/analysis/hitchsum.py` sums them up) |

## Video

Unity's VideoPlayer plays clips through `runtime/media` (details, call sequences and costs:
`runtime/media/README.md`): players 2017.4 - 2021.3 through the Java classes of
`runtime/jni/up_media.c`, 2022.3 through `runtime/android/media_ndk.c`. The data needs
`Hidden/VideoDecodeAndroid`, which `pmunity android-data` adds (`docs/android-data.md`).

| clips (desktop build) | picture | sound |
|---|---|---|
| VP8 / VP9 + Vorbis / Opus WebM (Linux builds) | libvpx, built in | libvorbis / libopus, built in |
| H.264 + AAC MP4 (macOS, Windows builds) | OpenH264, built in (with B-frames: the system's libavcodec when there is one, exact; else OpenH264, close) | the system's libavcodec; without it silence |
| HEVC, MP3 | the system's libavcodec | the system's libavcodec; MP3 without it: silence |

At start the log names the decoders (`[media] decoders: vp8 (libvpx-vp8), ...`); each clip logs its
tracks and decoders. `umbench <file> [offset size]` (`runtime/media/out/umbench`, a device check)
decodes a clip as fast as it goes and prints the cost per frame.

Debugging on the device: `python3 tools/device/stacks.py <pid> [thread]` (each thread's syscall and the
libunity return addresses on its stack; name them with `tools/analysis/sym.py`), and gdb attaches to the
32-bit process (`set arm force-mode arm`, `break *<libunity base + offset>`).
