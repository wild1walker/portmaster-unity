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
| `UA_GL_TRACE=<s>` | one whole frame every `<s>` seconds logged pass by pass: framebuffer, size, clears, draws, vertices, invalidates, uploads (runtime/host/gltrace.c) |
| `UA_GC_HEAP=<bytes>` | Mono's initial GC heap (default 64 MB, `0` = Boehm's own; `GC_INITIAL_HEAP_SIZE` wins) |
| `UA_QUIT_CLEANUP=1` | run Unity's engine cleanup (nativeDone) at quit; by default the host pauses Unity (PlayerPrefs saved) and exits |
| `UA_AUDIO_RATE`, `UA_AUDIO_FRAMES` | output rate and buffer size AudioManager reports to FMOD (default 48000, 512) |
| `UA_HITCH_TRACE=<ms>` | in frames longer than `<ms>`, sample `UA_HITCH_THREADS` (default `UnityMain`) every `UA_HITCH_EVERY` ms (20): pc, lr, return addresses as module+offset (runtime/host/hitches.c; `tools/analysis/sym.py` names libunity offsets, `tools/analysis/hitchsum.py` sums them up) |

Debugging on the device: `python3 tools/device/stacks.py <pid> [thread]` (each thread's syscall and the
libunity return addresses on its stack; name them with `tools/analysis/sym.py`), and gdb attaches to the
32-bit process (`set arm force-mode arm`, `break *<libunity base + offset>`).
