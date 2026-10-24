# Runtime reference

`ua_host <root>` runs a game with Unity's ARMv7 Android player. `<root>` holds:

| path | what |
|---|---|
| `lib/libmain.so`, `lib/libunity.so`, `lib/libmono.so` | the player (Release variant; `UA_LIBDIR` for another folder, e.g. the Development player) |
| `<name>.apk`, `main.<code>.<package>.obb` | the game's data, from `pmunity android-data --pack all` |
| `android_package.json` | written beside the APK by the data tool: package, build id, APK name, Unity version (read by ua_host) |
| `assets/bin/Data/Managed/etc/` | Mono's config, read relative to the working directory (the data tool's zip folder has it) |
| files the game reads beside its data | e.g. a GOG build's `goggame-*.info` (`--beside`) |
| `plugins.map` | written beside the APK by the data tool: what each native library the game P/Invokes becomes (ARM build in `lib/`, system library, built-in stub, refused) and the float signatures of glibc plugins ([plugins.md](plugins.md)) |
| `lib/<plugin>.so` | ARM builds of the game's plugins (FMOD's Android libraries, ...) beside the player |
| `assets/*.bank` | FMOD banks, kept out of the OBB (FMOD opens them as files) |
| `Game_Data/StreamingAssets/` | StreamingAssets as plain files, for code that reads them with System.IO (`android-data --streaming-assets both\|loose` with asmpatch's streaming-assets patch; android-data.md section 6) |
| `data/` | created: `files/`, `cache/` (Unity's shader cache, temporaryCachePath), `external/` (persistentDataPath: saves), `shared_prefs/` (PlayerPrefs, `<package>.v2.playerprefs.prefs`; `pmunity prefs` converts it to and from a Linux player's `prefs`) |

What the host answers for itself (besides the Android shims): `System.loadLibrary(name)` /
`UnityPlayer.loadLibrary(name)` load `lib<name>.so` from the library folder and `System.load(path)`
that file, with the library's `JNI_OnLoad` called the first time (runtime/host/ua_host.c); and
`libsteam_api.so`, unless the library folder has a real ARM one, is a stub of the Steamworks flat
API that answers as a PC without a running Steam client (Init fails, the game goes on without
Steam; runtime/host/steamstub.c). The game's other native plugins follow `plugins.map`
([plugins.md](plugins.md)): ARM builds in `lib/`, the system's libraries, built-in stubs of
platform SDKs (Discord, EOS, old Steamworks.NET), or refused. Android system properties come from the loader
(`SO_PROP_<name, '.' as '_'>` overrides them; `swappy.disable=1` keeps Android Frame Pacing off).

Run it with the 32-bit libraries first: `LD_LIBRARY_PATH=/lib32:/usr/lib32 ./ua_host "$PWD"`
(`tools/device/run.sh` does that, silent by default). Pads: SDL2 GameController, mapped by
`SDL_GAMECONTROLLERCONFIG` (PortMaster's launchers set it through `get_controls`).

## Display

`ua_host` picks the EGL window Unity draws into before it starts Unity (`runtime/host/display.c`):

| `UA_DISPLAY` | what |
|---|---|
| `auto` (default) | `fbdev` when the framebuffer (`ANDROID_FBDEV`, default `/dev/fb0`) exists and libEGL is an fbdev-only one: no window-system platform (`EGL_*_platform_gbm/wayland/x11/xcb/device/surfaceless`) in its client extensions, which Mesa and the Wayland/GBM/X11 flavors of vendor drivers always list. Otherwise `sdl`; if SDL cannot open a GLES window, `fbdev`. The choice and the reason are logged (`[display] auto: ...`) |
| `fbdev` | Mali's fbdev EGL (Knulli on the Allwinner H700): no window system is probed; the framebuffer's buffers and the flips are the host's (below) |
| `sdl` | an SDL2 window (Wayland, KMSDRM, X11 through EGL, SDL's offscreen driver; `SDL_VIDEODRIVER` picks one) whose EGL display, config and window surface Unity is given (`runtime/android/README.md`, "Host display"); stops if SDL cannot open one |

With `UA_EGL_PBUFFER=1` (tests on Mesa's surfaceless platform) the fbdev code path is always taken.

### The fbdev path's flips

Mali's fbdev EGL draws the window into the framebuffer itself and shows a finished frame by panning
to it (`FBIOPAN_DISPLAY`). On the RG34XX-SP (sunxi's fbdev, Mali-G31 r20p0) the pan returns at the
vblank that shows the frame, whatever `eglSwapInterval` says, and the driver makes it on the thread
that also feeds the GPU (`mali-cmar-backend`): while it waits, the GPU gets no work. A frame of 18 ms
of GPU work was shown every second refresh (30 fps), the GPU idle a third of the time and clocked
down for it (simple_ondemand: 420 - 504 MHz, at times 336 MHz and 20 fps); a third buffer alone
changed nothing (Cell Machine 29.7 fps with 2 or 3). So ua_host makes the flips
(`runtime/host/fbdev.c`): it exports `ioctl`, the driver's pans come to it and are queued to a thread
of the host (`ua_flip`) that makes them in order, and the driver's call returns at once; the screen
info the driver reads before each pan is answered from the host's copy (the kernel would answer only
once a pending pan is done: its fb lock). The driver takes the return as "shown" and hands the buffer
shown before to the next frame, so the framebuffer gets four buffers (its virtual height; the
RG34XX-SP's framebuffer memory holds five 720x480 screens; the old geometry is put back when ua_host
ends, by a small watcher process, `ua_fbrestore`, if ua_host is killed) and Unity's render thread
waits after each `eglSwapBuffers` until the buffer its next frame draws into has really left the
screen (the driver uses its buffers in turn). Nothing the GPU draws into is on the screen, every
frame is shown at a vblank, in order: no tearing (`runtime/host/test/fbdev_test.sh`: a fake driver
and display; without the wait the same scenarios tear).

On the RG34XX-SP (fps, then the time from a frame's swap to its flip):

| game | driver's flips (before) | host flips | without vsync (`mailbox`, tears) |
|---|---|---|---|
| Cell Machine (2019.3, URP), level 1 | 29.7, 31 ms | **53.6**, 25 ms | 53.4 |
| Chapéu de Papel (2022.3 macOS, URP), title | 20.2 - 29.6 (GPU at 336 or 600 MHz), 90 ms | **36.4**, 73 ms | 36.3 |
| Hollow Knight 64-bit (6000.0), main menu | 38.2, 23 ms | **42.6**, 28 ms | |
| The Third Eye (2022.2, URP 3D), first scene | 29.6, 26 ms | 31.9, 27 ms | |
| White Mirror (2021.2, URP), in game | 43.0 (197 frames over 25 ms in 25 s), 35 ms | 43.1 (12), 36 ms | |
| Chapéu de Papel APK (IL2CPP arm64), first scene | 32.9, 31 ms | 33.4, 31 ms | |
| Cuphead 64-bit (2017.4 IL2CPP), title | 59.4, 45 ms | 59.4, 28 ms | |
| Tom City (2020.3), title | 59.3, 47 ms | 59.4, 30.5 ms | |

Games the GPU limits gain what the wait for the vblank cost (the GPU now at 696 MHz throughout;
those already at it gain little). Frames that keep up with the refresh are shown a refresh sooner
than with the driver's own flips (it queued swapped frames on its thread); frames the GPU is late
with keep two queued, so the GPU always has the next one (a few ms more from swap to screen than
the driver's flips in Hollow Knight's menu, at 11 % more frames).

| variable | effect (fbdev) |
|---|---|
| `UA_FB_FLIP=host\|driver\|mailbox` | `host` (default): the flips above; falls back to `driver` with fewer than three buffers or when the driver does not use its buffers in turn. `driver`: the driver's own pans (the old behaviour). `mailbox`: a measurement, tears: the driver's pan returns at once and only the newest frame is flipped to, so the frame rate is what the frames cost (above the refresh rate when they cost less); `UA_SWAP_INTERVAL=0` changes nothing on this driver |
| `UA_FB_BUFFERS=<n>` | the framebuffer's virtual height as `n` screens, when its memory holds them. Default: 4 with host flips (3 if the memory holds no more), else the firmware's (2 on Knulli) |
| `UA_FB_QUEUE=<n>` | frames the render thread may be ahead of the screen, at most buffers - 2 (and 2). Default: automatic, 2 while frames miss refreshes (the GPU always has the next frame), 1 once 30 frames in a row were done while the one before still waited for its flip (the game keeps up with the refresh, and a second queued frame would only be latency); back to 2 at a missed refresh, for 2 s (doubling up to a minute at each fall-back, so a game at the edge does not go back and forth) |
| `UA_FB_TRACE=<n>`, `UA_FB_TRACE_START=<s>` | `[fbtrace]` lines: the times of `n` swaps, the driver's pans and the flips, from `s` (40) seconds after the first swap |

`UA_FPS`'s lines end with, on the fbdev path, the time from each swap to the flip that shows it
(`shown <avg> / <worst> ms after the swap`), the buffers, the queue depth allowed and the render
thread's wait for its next buffer (`gate`).

## Host switches (environment)

| variable | effect |
|---|---|
| `UA_LIBDIR` | player libraries (default `<root>/lib`) |
| `UA_SDK_INT` | the Android API level reported (`Build.VERSION.SDK_INT`, `ro.build.version.sdk`). Default: converted data 19 on armhf, 22 on arm64, or `android_package.json`'s `min_sdk` (22 for a game with compute kernels: Unity uses OpenGL ES 3.1+ only from API 21, [android-data.md](android-data.md#2-platform-differences)); an imported APK its targetSdkVersion ([apk.md](apk.md)) |
| `UA_PACKAGE`, `UA_META` | package name; manifest meta-data (`unity.build-id=...` ties the APK to its OBB). Default: from `android_package.json` |
| `UA_APK_NAME` | the APK's name without `.apk` (default: from `android_package.json`, else the only `.apk` in the root) |
| `UA_OBB_DIR` | where the OBB is (default: beside the APK) |
| `UA_PAD_NAME` | name the pad reports (Rewired knows Android pads by name: `Xbox Wireless Controller`) |
| `UA_INPUT_SCRIPT` | scripted presses: `"<sec>[@<period>]:<button>[~<hold>][+...] ..."`; `ptr=<x>/<y>` puts pointer mode's cursor there (fractions with a `.`, else pixels); `text=<chars>` types into an open soft keyboard (`_` = space). Without a real pad they press a virtual one, registered at the first press (`UA_VIRTUAL_PAD=1`: at start). A release waits until 2 frames were presented since the press, and the first press until 3 frames after the pad is taken, so a slow game (the emulated rig, a long load) still sees every press |
| `UA_OSK=0\|done\|cancel`, `UA_OSK_SCALE`, `UA_OSK_SHOTS=0` | the on-screen keyboard for the game's soft keyboard (on by default, shown only when the game opens it; see [input.md](input.md)): off; answer every keyboard at once (OK / Back); glyph scale; keep it out of `shot.req` screenshots |
| `UA_KEYBOARD=1` | an SDL window's key events also reach the game as an Android hardware keyboard (they always type into an open soft keyboard) |
| `UA_POINTER=1\|mouse\|touch` | pointer mode, off by default: the pad moves a cursor ua_host draws over the frame and clicks with it, as an Android mouse (`1` = `mouse`: hover, buttons, wheel) or a finger (`touch`); see [input.md](input.md) |
| `UA_POINTER_MOVE`, `UA_POINTER_BUTTONS`, `UA_POINTER_SCROLL` | what moves it (`lstick,dpad`; also `rstick`), pad button : mouse button (`a:left,b:right`), `rstick` = wheel; the controls it uses no longer reach the game as the pad's |
| `UA_POINTER_SPEED`, `UA_POINTER_START`, `UA_POINTER_TOGGLE` | full speed in screen heights/s (1.2); start position `<x>/<y>` (fractions, `0.5/0.5`); a button that turns pointer mode off and on |
| `UA_POINTER_CURSOR=0`, `UA_POINTER_SIZE`, `UA_POINTER_HIDE`, `UA_POINTER_SHOTS=1` | no cursor drawn (the game draws its own); its scale; hide it after `<s>` idle; draw it into `shot.req` screenshots too (by default they show only the game) |
| `UA_KEYMAP=<button>:<key>,...` | pad buttons (and `up/down/left/right`, the d-pad) that press keyboard keys instead, e.g. `start:escape,x:space` |
| `UA_FPS` | frame stats every N s (avg/p99/worst, late >25 ms, hitch >50 ms, GC; on the fbdev path the time from the swap to the flip: Display above); `[hitch]` lines >100 ms |
| `touch <root>/shot.req` | screenshot of the next frame -> `<root>/shot-<n>.ppm` |
| `<root>/input.req` | live input: presses in `UA_INPUT_SCRIPT`'s syntax, times counted from when it is read (every 0.25 s; then deleted). Write it elsewhere and `mv` it into place. With `shot.req`, a test drives a game by what its screenshots show: `echo "0:b~2 6:down" > x && mv x <root>/input.req` |
| `UA_MONO_OPTIONS` | Mono options; `--trace=T:Class,M:Class:Method` (no `<` in specs) |
| `UA_MONO_TRACE` | log Mono class/method/field lookups that fail |
| `FJ_TRACE` | log every JNI call |
| `ANDROID_SLES_OUTPUT` | `auto` (SDL, else pipe), `sdl`, `pipe`; `ANDROID_SLES_PIPE_CMD` overrides the client. The pipe goes to `pw-cat` (PipeWire), else `pacat` (PulseAudio), else the system's `aplay` (ALSA alone, e.g. when the 32-bit alsa-lib cannot drive an older kernel) |
| `ANDROID_SLES_APLAY_DEVICES` | the ALSA devices the `aplay` client tries in order (default `plughw:0,0 default`: the card directly, without dmix's buffer, else the system's default device when the card is busy) |
| `UA_SHADER_DUMP=<dir>` | write every GLSL source Unity compiles to `<dir>/<fnv64>.glsl` (runtime/host/glhooks.c) |
| `UA_SHADER_OVERRIDE=<dir>` | compile `<dir>/<fnv64>.glsl` instead of that source (both switches turn Unity's program binary cache off) |
| `UA_SWAP_INTERVAL` | eglSwapInterval forced (0: no vsync; Unity's own frame cap still applies). Mali's fbdev driver on the RG34XX-SP flips at the vblank whatever the interval: `UA_FB_FLIP=mailbox` there (Display above) |
| `UA_FRAME_RATE=desktop\|android\|<n>` | frame pacing. From 2021.1 Unity's Android players ignore `QualitySettings.vSyncCount` and render at `Application.targetFrameRate`, whose default (-1) is a fixed 30 fps; a desktop game that leaves its frame rate to vsync drops from 60 to 30. `desktop`: paced as the desktop player does (vSyncCount > 0: the display's rate / vSyncCount; 0: targetFrameRate, -1 = the display's rate; the game reads back what it set); `android`: Unity's Android pacing; `<n>`: n fps whatever the game asks. Before 2021.1 the players follow vSyncCount, but a game with vsync off and no targetFrameRate gets the same 30 fps default (Forsaken Castle, Liberal Crime Squad: 30 -> 59 fps with `desktop`). Default: `desktop` for converted desktop data of any Unity version (`unity_version` in `android_package.json`; games with vsync on pace as before), else `android` (imported APKs, older packages without the field) (runtime/host/framerate.c) |
| `UA_DISPLAY`, `UA_DISPLAY_*`, `UA_FB_*` | the EGL window: Mali fbdev or SDL2, and the fbdev path's flips (see Display above) |
| `UA_MSAA_FIX=0\|1` | Unity 2017.1 - 2017.3 with a quality level that uses MSAA: those players always present through their blit path then (a multisampled framebuffer of Unity's own, resolved into a texture drawn over the window), but the window's first setup leaves Unity drawing every frame into the window itself, and the present then draws the untouched framebuffer over it: every frame black (Shiver, 2017.2). With the fix the present's resolve reads the window instead (the frame shows, without MSAA), and the window's EGL config is single-sampled (`UA_DISPLAY_SAMPLES` is ignored); runtime/host/msaafix.c. Default: on for those players (`unity_version` in `android_package.json`), off for all others and for packages that do not name their version |
| `UA_GL_TRACE=<s>` | one whole frame every `<s>` seconds logged pass by pass: framebuffer, size, clears, draws, vertices (index counts), invalidates, blits, copies (`glCopyTex(Sub)Image`), compute dispatches, uploads, and which draw entry points the frame used (`calls:`). Every draw entry point of GLES 3.2 and its extensions is counted (Instanced, Range, BaseVertex in its core, OES and EXT forms, BaseInstance, MultiDraw, Indirect), also when Unity takes them through `eglGetProcAddress`. `UA_GL_TRACE_START=<s>`: the first traced frame `<s>` seconds after the first frame (past the menus, so the traced frames' stalls do not disturb a bench plan); runtime/host/gltrace.c |
| `UA_GL_TRACE_DRAWS=<n>` | with `UA_GL_TRACE`: the traced frame's first `<n>` clears, draws and blits one by one, as they happen: each pass's colour buffer (texture, size, format), each draw's program with its shaders' source hashes (the names `UA_SHADER_DUMP` gives their files: run with it, which also keeps Unity's program binary cache off), its samplers (`name=unit:texture`), the pixel at the centre and at (1/4, 1/4) read back after the call, and its fixed-function state (`| depth <func>[ write] stencil <func> ref <n> <fail>/<zfail>/<zpass>[ write] blend <src>,<dst>|off [mask <rgba>] [scissor]`: what decides whether a pass's depth/stencil or old colour matter); a clear's colour; each pass's depth/stencil attachment (`depth tex<n> D24S8`). What draws a wrong picture, and from what: e.g. `#76 draw fb51 prog81 vs <hash> fs <hash> 3 _BlitTexture=0:tex916 _InternalLut=1:tex922 -> (0.00392,0.984,0.00762,1)`: a full-screen pass that turned a dark picture green through its colour-grading LUT (each read waits for the GPU: a traced frame takes much longer; the bindings Unity had are put back after each read: runtime/host/test/pointer_test.sh runs test_gltrace on a fake libGLESv2) |
| `UA_GL_TRACE_GPU=3\|1\|2\|0` | with `UA_GL_TRACE` (not with `UA_GL_TRACE_DRAWS`, whose reads split passes): the GPU time of each pass of a traced frame (`gpu <ms> ms`, the frame's sum `GPU <ms> ms`). `3` (default): the host waits for the GPU (`glFinish`) at each pass's end and times that wait on the CPU's clock, so the passes run one at a time; each includes the driver's submission and the GPU's start-up (about 0.3 ms on the RG34XX-SP: the 180x112 passes read 0.3), the frame's first pass also the wait for the previous frame (vsync); works on any driver. `1`: `EXT_disjoint_timer_query` (`GL_TIME_ELAPSED_EXT` per pass, `glFlush` at each pass's end, the frame logged a frame later, "(disjoint)" when the driver lost time); `2` the same without the flush; `0` off. The Mali-G31's r20p0 driver has the extension, but its answers cannot be right ("disjoint" nearly every frame, 0.00 ms for most of a scene's passes, once 18446744073735 ms) |
| `UA_GL_DSCLEAR=1\|0` | (default 1) a depth/stencil buffer that still holds only the values of its last whole clear (no draw since wrote depth or stencil) is cleared to them again at the start of the next pass on its framebuffer instead of being loaded from memory: a tiler (the Mali) otherwise loads it each time a camera comes back to its framebuffer (after a grab pass's copy, an image effect drawn into it). Exact: the buffer gets the values it holds, and the game's GL state is put back. Hollow Knight (64-bit) on the RG34XX-SP: King's Pass 30.6 -> 32.4 fps, its main menu 36.1 -> 38.7; runtime/host/glopt.c ([profiling.md](profiling.md#gpu-pass-by-pass)) |
| `UA_GPU_IDLE=<s>` | a measurement, off by default: every `<s>` seconds the time the GPU had nothing to do while the frames were drawn (`[gpuidle] ... GPU idle <ms> ms a frame (<%>): before the frame's first span ..., inside frames ...`, and the spans, i.e. framebuffers, it waited before). Each span of commands (up to a change of the draw framebuffer, or the swap) ends with an EGL fence and a glFlush on the render thread; a thread of the host waits for the fences in order and counts the gaps in which everything flushed was done and the next span not flushed yet. Nothing is serialized (unlike `UA_GL_TRACE_GPU`); the fences, flushes and the waiting thread (`ua_gpuidle`, about 4 % of a core in Hollow Knight) are its own cost. Needs `EGL_KHR_fence_sync`; runtime/host/gpuidle.c ([profiling.md](profiling.md#the-handheld-around-the-game-2026-10-23)) |
| `UA_GC_BUDGET=<MB>` | games that switch Unity's garbage collector off (`GarbageCollector.GCMode = Disabled`, Unity 2018.3+; Hollow Knight does, collecting only at scene changes) let the heap (Mono's or IL2CPP's) grow for as long as a scene lasts, which a 1 GB handheld cannot hold. While the game has it off, ua_host collects once whenever the used heap has grown by this much since the last collection (default 32; 0 = off), as the game's own forced collections do; each is one pause, logged as `[host] gc budget`. Games that keep the collector on are not affected (runtime/host/gcbudget.c) |
| `UA_GC_HEAP=<bytes>` | Mono's initial GC heap (default 64 MB, `0` = Boehm's own; `GC_INITIAL_HEAP_SIZE` wins) |
| `UA_FAST_CLOCK=1\|force` | a 32-bit player on a 64-bit ARM kernel older than Linux 5.3 (the RG34XX-SP's 4.9) gets no vDSO, so each `clock_gettime` and `gettimeofday` of the player's libraries is a system call: about 1.1 us on the RG34XX-SP's Cortex-A53 (`getpid` 0.3 us); Unity, Mono and FMOD make about 6,000 a second in Hollow Knight (`CLOCK_MONOTONIC` 3,600, `CLOCK_BOOTTIME` 1,900, `gettimeofday` 700). Android's kernels give 32-bit apps a vDSO; `1` gives the Android libraries the same: the ARM generic timer's virtual counter (`CNTVCT`, readable from user space) scaled to each clock, the slope resynchronised with the kernel's clock every 0.25 s (continuous and monotonic, within microseconds of the kernel's; it follows NTP's slewing; a step of `CLOCK_REALTIME`, or `CLOCK_BOOTTIME` after a suspend, is taken over at the next resync), 0.13 us a call. Used only where there is no vDSO, the counter can be read (a probe at start) and keeps pace with `CLOCK_MONOTONIC_RAW` (`[clock] ...` in the log); CPU-time clocks stay system calls; `force` also with a vDSO (tests). Off by default: in Hollow Knight it took UnityMain's kernel time down by a third with no frame rate change that could be measured (49.1 / 49.2 fps against 49.2 / 49.1 with the GPU's work left out: [profiling.md](profiling.md)). runtime/loader/fastclock.c, runtime/loader/test/test_clock.c |
| `UA_CLOCK_STATS=<s>` | log the Android libraries' clock calls per second and clock every `<s>` s (`[clock] ...`) |
| `UA_THREAD_PRIORITY=1` | apply Unity's `Process.setThreadPriority(tid, priority)` calls (Android's nice values) with `setpriority`, as Android does. Unity 6000.0 asks for 0 for UnityMain and its job threads, -2 for the render thread and the choreographer, 10 for its background job threads; FMOD sets its mixer's itself (-19). Off by default: ignored and logged (the first 32); Hollow Knight measured no difference ([profiling.md](profiling.md#what-limits-kings-pass-32-bit-2026-10-23)) |
| `UA_AFFINITY='<name>:<cores> ...'`, `UA_NICE='<name>:<nice> ...'` | cores and nice values for the game's threads by name (runtime/host/threads.c): a name is exact or a prefix ending in `*` (names are cut to 15 characters: `UnityGfxDeviceW`, `Job.Worker*`, `FMOD*`, Mali's `mali-cmar-backe`), `*` alone every other thread, the first rule of each variable that matches wins; cores as `0-2,3`; e.g. `UA_AFFINITY='FMOD*:0 *:1-3' UA_NICE='mali-cmar-backe:-10'`. A thread `ua_threads` applies them to new and renamed threads (every 0.5 s for two minutes, then every 5 s) and logs each (`[threads] ...`). Measuring tools, off by default. On the RG34XX-SP FMOD's mixer (nice -19, about half a core) preempts whatever shares its core: `UA_AFFINITY='FMOD*:0 SDLAudio*:0 *:1-3'` gave Hollow Knight's main thread 3 % more frames when the GPU's work was left out, but nothing in the game, which the GPU holds there ([profiling.md](profiling.md#what-limits-kings-pass-32-bit-2026-10-23)) |
| `UA_QUIT_CLEANUP=1` | run Unity's engine cleanup (nativeDone) at quit; by default the host pauses Unity (PlayerPrefs saved) and exits |
| `UA_PLUGIN_MAP=<file>` | another plugins.map (default `<root>/plugins.map`; [plugins.md](plugins.md)) |
| `UA_CA_BUNDLE=<pem>` (`SSL_CERT_FILE`, `SSL_CERT_DIR`) | the CA certificates libunity's TLS trusts (Unity 2018.4+; default: the system bundle, `/etc/ssl/certs/ca-certificates.crt` and the other usual places; [plugins.md](plugins.md#https)) |
| `UA_HTTP=1` | Unity 2017.4 - 2018.4: UnityWebRequest/WWW make real requests through the system's libcurl (default: they fail at once, as without a network; [plugins.md](plugins.md#https)) |
| `UA_AUDIO_RATE`, `UA_AUDIO_FRAMES` | output rate and buffer size AudioManager reports to FMOD (default 48000, 512) |
| `UM_TRACE`, `UM_VIDEO_BUFFERS`, `UM_VPX_THREADS`, `UM_H264`, `UM_FFMPEG` | video (Unity's VideoPlayer, `runtime/media`): per-buffer log, a video decoder's output queue (default 4), libvpx threads, OpenH264 for every H.264 stream, the system's libavcodec off / from a path (see Video below) |
| `UA_HITCH_TRACE=<ms>` | in frames longer than `<ms>`, sample `UA_HITCH_THREADS` (default `UnityMain`) every `UA_HITCH_EVERY` ms (20): pc, lr, return addresses as module+offset (runtime/host/hitches.c; `tools/analysis/sym.py` names libunity offsets, `tools/analysis/hitchsum.py` sums them up). `UA_PROF_EVERY=<ms>` logs such a sample of those threads every `<ms>` all the time (`hitchsum.py --prof`; heavy: `UA_PROF` replaces it) |
| `UA_PROF=<hz>` | the sampling profiler, off by default: recordings of `UA_PROF_THREADS` (default `UnityMain,UnityGfxDeviceW`; `Job.Worker*` style prefixes, `@swap` the presenting thread) at `<hz>` samples a second, `UA_PROF_SECS` long (20), started by `touch <root>/prof.req` (a number in it: the length) or once `UA_PROF_START` s after the first frame; `UA_PROF_DEPTH` (32) return addresses a sample from `UA_PROF_SCAN` bytes of stack (16384). Each one is written to `<root>/prof-<n>.txt`: states (on a CPU, runnable, blocked in which call), pc, lr and callers (unwound through code with unwind tables, scanned above Mono's JIT frames), Mono's JIT methods by name, per-thread CPU time, frame times; `tools/analysis/profsum.py` sums it up by module, function and managed method (runtime/host/prof.c, [profiling.md](profiling.md)) |

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

Profiling on the device (`UA_PROF`, the bench `tools/device/profbench.sh`): [profiling.md](profiling.md).
Debugging on the device: `python3 tools/device/stacks.py <pid> [thread]` (each thread's syscall and the
libunity return addresses on its stack; name them with `tools/analysis/sym.py`), and gdb attaches to the
32-bit process (`set arm force-mode arm`, `break *<libunity base + offset>`).
