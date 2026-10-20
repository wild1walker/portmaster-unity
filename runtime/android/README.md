# android/: Android API shims (liblog, libandroid, OpenSL ES, libmediandk, EGL glue)

C shims for the Android-only libraries that Unity 2017.4.9f1's ARMv7 player
(`libunity.so`, `libmain.so`, `libmono.so`) imports or dlopens, built for armhf glibc
(Debian bookworm cross toolchain, glibc symbols up to 2.34), or for aarch64 glibc (arm64-v8a
players; native gcc of the `ua-build-arm64` image, glibc symbols up to 2.28).

```
./build.sh                   # -> out/libandroidshims.a (+ out/obj/*.o), out/test/*
./build.sh test              # also runs the unit tests under QEMU (no GPU), twice: SDL "disk" and "dummy" audio
ARCH=arm64 ./build.sh test   # the same under out/arm64/, built and run natively in ua-build-arm64
```
Link the host with `out/libandroidshims.a` (or `out/arm64/libandroidshims.a`) `-lSDL2 -ldl -lpthread -lm`.
Do not link a real libEGL for the shims' sake: `egl_glue.c` dlopens it.

## What the libraries need (from `readelf`; `tools/re/elfimports.py` lists them for any player)

The DT_NEEDED entries and undefined dynamic symbols of the 2017.4 player; the Android-only ones are:

| Library | Needed by | Symbols |
|---|---|---|
| liblog.so | libunity, libmain, libmono | `__android_log_print`, `__android_log_vprint`, `__android_log_write` (libmain only lists liblog, it imports nothing from it) |
| libandroid.so | libunity, libmain | `ANativeWindow_{fromSurface,acquire,release,getWidth,getHeight,setBuffersGeometry}`, `ALooper_{forThread,prepare}`, `ASensorManager_{getInstance,getSensorList,getDefaultSensor,createEventQueue,destroyEventQueue}`, `ASensorEventQueue_{enableSensor,disableSensor,setEventRate,hasEvents,getEvents}`, `ASensor_{getName,getVendor,getType,getResolution,getMinDelay}`, `AInputEvent_{getType,getDeviceId}`, `AKeyEvent_{getAction,getKeyCode,getMetaState}` |
| libOpenSLES.so | libunity (FMOD) via dlopen | `slCreateEngine`, `SL_IID_ENGINE`, `SL_IID_ANDROIDSIMPLEBUFFERQUEUE`, `SL_IID_PLAY`, `SL_IID_RECORD`, `SL_IID_ANDROIDCONFIGURATION` (data symbols) |
| libEGL.so | libunity | 21 `egl*` functions (host libEGL; 6 are wrapped, see below) |
| libGLESv2.so | libunity | nothing imported: GL is loaded with dlopen/dlsym and eglGetProcAddress |
| libmediandk.so | libunity 2022.3 (DT_NEEDED); 2017.4-2021.3 dlopen it | `AMediaCodec_*`, `AMediaExtractor_*`, `AMediaFormat_*`, the `AMEDIAFORMAT_KEY_*` string constants (data objects); weak `AImageReader_*`, `AImage_*`, `AMediaDataSource_*`, more keys |

Notes from the disassembly (with the symbols in `Symbols/armeabi-v7a/libunity.sym.so`):
- The `AInputEvent_*`/`AKeyEvent_*` imports have no relocations: Unity never calls them (input
  arrives through the Java side). AAssetManager, AConfiguration, ANativeActivity and AStorageManager
  are not imported at all: Unity reads the APK zip itself.
- The sensor calls come from `InitNativeSensors()` etc.: Unity handles an empty sensor list and a
  NULL default sensor.
- libunity also dlopens `libvulkan.so`, `libSEC_OMX_Core.so`, `libsomxcore.so`. We have no shims
  for them: `android_shims_for_library()` returns NULL and the loader's dlopen fails, which makes
  Unity fall back (GLES instead of Vulkan). `libmediandk.so` (VideoPlayer; dlopened up to 2021.3,
  linked by 2022.3) is served by stubs that fail cleanly, below.

## Implemented vs stubbed

| Area | State |
|---|---|
| liblog | Implemented: `__android_log_{write,print,vprint,buf_write,buf_print,assert,is_loggable,set/get_minimum_priority}` print `I/Unity(tid): msg` to stderr with one write() per line, trailing newlines trimmed. `ANDROID_LOG_LEVEL=V/D/I/W/E/F` (default D). |
| ANativeWindow | Implemented: one window, its size is the `/dev/fb0` visible size (`FBIOGET_VSCREENINFO`), or `ANDROID_WINDOW_SIZE=WxH`, or 720x480. `setBuffersGeometry` sets the buffer size reported by `getWidth/getHeight` and used for the EGL surface (0,0 resets). `fromSurface` returns the window for any non-NULL Surface. Stubbed: `lock`/`unlockAndPost` return -EINVAL (no CPU rendering). |
| AAssetManager | Implemented over a directory: `fromJava`, `open`, `openDir`, `AAssetDir_{getNextFileName,rewind,close}` (regular files only, as on Android), `AAsset_{read,seek,seek64,getLength(64),getRemainingLength(64),getBuffer (mmap),openFileDescriptor(64),isAllocated,close}`. Names with `..` are refused. |
| ALooper | Implemented with poll() and an eventfd: `forThread`, `prepare`, `acquire/release`, `addFd/removeFd` (callback or ident), `pollOnce/pollAll`, `wake`. |
| Sensors | Implemented as "no sensors": a manager with an empty list, NULL default sensors, event queues that never have events. |
| Input | Minimal: `AInputEvent_*`, `AKeyEvent_*`, `AMotionEvent_*` getters over our own event struct (`android_input_event_new_key()` builds one). `AInputQueue_*` are inert stubs. |
| AConfiguration | Minimal fixed config: landscape, 160 dpi, SDK 26, no touch, D-pad, en_US. |
| OpenSL ES | Implemented on SDL2: engine, output mix, audio player with a PCM buffer queue (`SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE` or `_BUFFERQUEUE`, 8/16/32-bit int and float PCM). Interfaces: Object, Engine, OutputMix, Play, BufferQueue/AndroidSimpleBufferQueue, Volume, AndroidConfiguration. Stubbed with `SL_RESULT_FEATURE_UNSUPPORTED`: audio recorder (`SL_IID_RECORD` exists but recording does not), MIDI, LED/vibra, listener/3D, metadata extractor, URI/fd players, effects. |
| libmediandk | `media_ndk.c`: AMediaExtractor (fd ranges, paths, `AMediaDataSource`), AMediaFormat, AMediaCodec over the media core (`../media`, referenced weakly) when the program links it, as ua_host does: Unity 2022.3's VideoPlayer path, with the video decoder's output the window of a Java `Surface(SurfaceTexture)` (`ANativeWindow_fromSurface` gives such a Surface a window of its own, `android_window_media_surface()`). Without the core every call fails the way the NDK reports failure (constructors NULL, status calls `AMEDIA_ERROR_UNSUPPORTED` (-10002), getters false / -1 / 0, out-pointers cleared, NULL objects accepted), which this folder's tests check. `AImageReader`/`AImage` (the Vulkan video path) are failing stubs either way; `AImage_deleteAsync` closes its fence fd. The `AMEDIAFORMAT_KEY_*` constants are `const char *` variables with the NDK's strings ("mime", "durationUs", "rotation-degrees", ...). `AMediaFormat_setFloat` is `SOFTFP`. In `android_shims[]` (DT_NEEDED imports resolve through the global tables) and `android_mediandk_shims[]` (dlopen). |
| Frame pacing | No `AChoreographer_*` (dlsym answers NULL, as on API < 24), no Android frame timestamps, a no-op `eglPresentationTimeANDROID`; Swappy is kept off by its own switch (below). |
| EGL | `egl_glue.c`, separate table `egl_shims[]` (below). |

## Using it from the host

```c
#include "android/android_shims.h"
#include "android/egl_glue.h"

android_set_asset_dir("/path/to/assets");   // or env ANDROID_ASSET_DIR (default ./assets)
android_window_set_size(w, h);              // optional: default is /dev/fb0's size
so_add_shims(egl_shims);                    // wrapped egl* first (wins over the host libEGL)
so_add_shims(android_shims);                // liblog + libandroid + libOpenSLES
so_register_fake_lib("liblog.so", android_log_shims);       // dlopen()+dlsym() by Android code;
so_register_fake_lib("libandroid.so", android_ndk_shims);   // or loop over names with
so_register_fake_lib("libOpenSLES.so", android_sles_shims); // android_shims_for_library(name)
so_map_library("libmediandk.so", NULL);     // dlopen refused (2017.4-2021.3 take their JNI video path);
                                            // 2022.3's DT_NEEDED and imports still get media_ndk.c
so_map_library("libvulkan.so", NULL);
```
- For `SL_IID_*` the table entry is the address of the `SLInterfaceID` variable, which is what
  dlsym returns for a data symbol (FMOD dereferences it).
- The fake JNI must hand `ANativeWindow_fromSurface` a non-NULL Surface object, and should
  implement `android.media.AudioSystem.getPrimaryOutputSamplingRate()I` and
  `getPrimaryOutputFrameCount()I` (static): FMOD's OpenSL output calls them in
  `getDeviceNativeParams()` and fails to start (FMOD_ERR_OUTPUT_INIT) if either returns 0.
  Suggested values: 48000 and 512.
- `android_shims.h` takes `so_shim` from `../loader/so_loader.h` when that file exists (checked
  with `__has_include`), so both headers can be included in any order.
- Our `eglGetProcAddress` wrapper returns the EGL wrappers for their names, otherwise the real
  result passed through the loader's `so_softfp_wrap()` (weak reference: used when linked with
  the loader). It overlaps with the loader's `so_egl_get_proc_address`; whichever table is
  registered first wins, and both give the same result.
- `egl_glue_set_swap_hook(fn, ctx)` runs `fn` on Unity's render thread before every
  `eglSwapBuffers` (a place to pump input or measure frames).
- `libmediandk.so` is one of the loader's Android-only libraries: without the `so_map_library(...,
  NULL)` line above, dlopen of it returns a handle on the shims too (or on
  `android_mediandk_shims` once registered with `so_register_fake_lib`), and players up to 2021.3
  take their NDK video path (untested: they were written against the Java one here). `media_ndk.c`
  includes `../media/um.h` (build.sh mounts `runtime/`).

Environment variables: `ANDROID_LOG_LEVEL`, `ANDROID_ASSET_DIR`, `ANDROID_WINDOW_SIZE=WxH`,
`ANDROID_FBDEV` (default /dev/fb0), `ANDROID_SHIM_LIBEGL` (real libEGL path, default libEGL.so.1
then libEGL.so), `ANDROID_SLES_SAMPLES` (SDL buffer in frames, default 512), `ANDROID_SLES_DEVICE`
(SDL output device name), `ANDROID_SHIM_TRACE=1` (trace shim calls), plus SDL's own
(`SDL_AUDIODRIVER`, ...).

## EGL and the window

Two ways to the screen, picked by the host (`runtime/host/display.c`, `UA_DISPLAY`, see
`docs/runtime.md`): Mali's fbdev EGL (below, the default on Knulli/H700), or a window the host
created with SDL2 (next section).

### Mali fbdev

On the device EGL is Mali's fbdev libEGL, whose `EGLNativeWindowType` is
`struct fbdev_window { unsigned short width, height; }`. Our `struct ANativeWindow` starts with
such a struct, so the pointer Unity passes is already a valid fbdev window. `egl_shims[]` wraps:

- `eglCreateWindowSurface`: translates our ANativeWindow into its fbdev_window (sized to the
  current buffer geometry), or into a host-provided handle set with `android_window_set_native()`
  (e.g. an X11 window for a desktop test); retried without attributes if the driver rejects
  Android-specific ones (e.g. `EGL_GL_COLORSPACE_KHR`).
- `eglChooseConfig`: drops Android-only attributes (0x3140-0x314F: `EGL_RECORDABLE_ANDROID`,
  `EGL_FRAMEBUFFER_TARGET_ANDROID`, ...). `eglGetConfigAttrib` answers them with 0.
- `eglCreateContext`: on `EGL_BAD_ATTRIBUTE` retries with only `EGL_CONTEXT_CLIENT_VERSION`.
- `eglSwapBuffers`: host hook, then the real call. `eglGetProcAddress`: returns the wrappers for
  wrapped names, NULL for Android's frame-timestamp queries, our no-op for
  `eglPresentationTimeANDROID` (see Frame pacing).
- `eglSurfaceAttrib` (Unity 2020.3+) is not wrapped: Unity's surface is the driver's.

All other `egl*` imports can resolve to the host libEGL directly (`egl_glue_real(name)` returns
the real symbol). The real libEGL is dlopened on first use, so EGL calls can come from any of
Unity's threads.

### Host display (SDL2: Wayland, KMSDRM, X11, offscreen)

Mesa and vendor GLES drivers on Wayland, KMS/DRM or X11 have no fbdev window type, and on KMSDRM
presenting is more than `eglSwapBuffers` (GBM buffers, page flips). So the host lets SDL2 create
the window, the EGL display, config and window surface (`runtime/host/display_sdl.c`), and hands
them to the glue with `egl_glue_set_host_display()`:

```c
egl_host_display d = { display, config, surface, make_current, swap, swap_interval, user };
egl_glue_set_host_display(&d);          // before Unity's first EGL call
so_add_shims(egl_host_shims);           // before egl_shims (after the host's own GL/EGL hooks)
android_window_set_size(w, h);          // the drawable size: what Unity is told and renders at
```

`egl_host_shims[]` maps Unity's calls (libunity 2017.4's `ContextGLES`, `ConfigEGL`,
`ContextEGL`, `WindowContextEGL`) onto those objects:

| Unity calls | becomes |
|---|---|
| `eglGetDisplay(EGL_DEFAULT_DISPLAY)` | the host display; `eglInitialize` only reports its version |
| `eglTerminate` (ContextGLES::Destroy) | kept (no-op): the host's window lives on that display |
| `eglChooseConfig` | the host config only. The context's config must be the surface's (Mesa checks equality); Unity checks it with `eglGetConfigAttrib` (depth, stencil, samples, ES3 bit, exact RGBA) and takes its closest match if it differs |
| `eglCreateContext` (ConfigEGL::Match's probe, the window context, contexts linked to it) | a real new context with the host config; the share context is passed on |
| `eglCreateWindowSurface(ANativeWindow*)` | a fixed handle standing for the host surface: the window system may replace the real one (SDL's KMSDRM driver recreates it inside `SDL_GL_SwapWindow` after a fullscreen or VT switch, window creation included). An sRGB request on a non-sRGB host surface is logged (`UA_DISPLAY_SRGB=1`) |
| `eglGetCurrentSurface`, `eglQuerySurface` | that handle when the real current surface is the one the host made current on this thread (Unity's `IsCurrentInternal` compares it before every present); queries go to the real surface |
| `eglDestroySurface` of it | no-op; the next `eglCreateWindowSurface` returns it again (pause/resume) |
| `eglMakeCurrent(host surface, ctx)` | `make_current(ctx)` (`SDL_GL_MakeCurrent(window, ctx)`, which keeps SDL's per-thread current window right); releases and other surfaces go to the real call after `make_current(NULL)` |
| `eglSwapBuffers(host surface)` | the swap hook, then `swap()` (`SDL_GL_SwapWindow`: KMSDRM flips there, newer SDLs pace Wayland there); the real call if this thread did not make the surface current through `make_current` |
| `eglSwapInterval` | `swap_interval()` (`SDL_GL_SetSwapInterval`; SDL keeps its own copy for Wayland and KMSDRM) |
| `eglSurfaceAttrib(host surface)` (2020.3+) | the real surface the host has current; attributes the driver accepted are set again on a surface the window system puts in its place (KMSDRM) |
| `eglGetProcAddress` | what an import of that name resolves to (the loader's tables in order, these wrappers first without the loader) |

Display, config and contexts are the real EGL objects, so `eglGetCurrentContext`,
`eglGetConfigAttrib` and `eglGetError` need no wrapping. Not combined with `UA_EGL_PBUFFER` (the
host never sets a display then).

## Frame pacing (Swappy)

Unity 2019.3+ contains Android Frame Pacing (Swappy, player setting "Optimized Frame Pacing",
`androidUseSwappy`). Decision: Swappy stays off, and Unity presents with `eglSwapBuffers` at its
swap interval, which the host's display paces (vsync on Mali fbdev, `SDL_GL_SetSwapInterval` on SDL):
- Swappy's own off switch, the `swappy.disable` system property, is set (`loader/hybris_compat.c`;
  `SO_PROP_swappy_disable=0` would let it run). It is read once Swappy's EGL table and its
  display query (`NativeActivity.getWindowManager()`, runtime/jni) work.
- Swappy's EGL table needs `eglPresentationTimeANDROID` (and `eglCreateSyncKHR`,
  `eglDestroySyncKHR`, `eglGetSyncAttribKHR`, `eglSurfaceAttrib`, ...). `eglGetProcAddress` answers
  a no-op that succeeds: a desired present time is a hint to SurfaceFlinger's queue, and no host
  here queues frames. Before, it answered NULL so that Swappy switched itself off; but the Swappy of
  2021.3+ keeps its disabled instance, Unity's present still calls `SwappyGL_swap`, and that falls
  back to the table: a NULL dereference on `UnityGfxDeviceW` after two frames (`swappy::SwappyGL::swap`).
  `eglGetNextFrameIdANDROID`, `eglGetFrameTimestampsANDROID`, `eglGetCompositorTimingANDROID` and
  their `Supported` queries stay NULL (optional for Swappy too), also when the driver has them.
- `AChoreographer_*` are absent from the libandroid shims (dlsym of them answers NULL, as on
  Android before API 24): Swappy's NDK choreographer cannot start; its Java one
  (`ChoreographerCallback`, runtime/jni) is driven by UnityMain's frame loop.
- `ANativeWindow_setFrameRate` (2021.3+, dlsym) is absent too: Unity keeps its frame rate.

Unity itself must not use Swappy then: with `androidUseSwappy` on and Swappy off, the 2019.4 player
presents nothing (Swappy deletes a disabled instance, `SwappyGL_swap` returns false without
swapping: "Failed to get SwappyGL instance in swap" for every frame), and 2021.3+ stop after their
first frames (the disabled Swappy reports a swap duration of -1, Unity's target frame rate comes out
near 0 and `AndroidVSync` waits for INT_MAX vsyncs). 2020.3 turns Swappy off itself below API 23.
`pmunity android-data` therefore turns `androidUseSwappy` off by default (`--swappy`), which is
also what a desktop build ran with; runtime/jni logs a line when Swappy starts anyway.

A game's own Android build (`pmunity apk`, docs/apk.md) keeps its setting, so ua_host clears
`swappy.disable` for it (`SO_PROP_swappy_disable=0` unless set): Swappy then paces the frames, with
its Java `ChoreographerCallback` ticking on a looper thread of its own (`SwappyChoreo`,
runtime/jni/up_input.c) as Swappy's `LooperThread` does on Android. Driven from UnityMain's frame
loop instead, Swappy's swap waited for ticks the blocked loop could not give, and each frame took
longer than the last (2021.3: 130 ms to 480 ms within 90 s in the emulated rig; with the thread,
a steady 25-29 fps). The reported API level stays below 24 for these builds: from 24 Swappy takes
the NDK choreographer, which these shims do not have.

## Audio

Each OpenSL audio player opens its own SDL audio device with the exact PCM format (SDL converts
and mixes), started at Realize. The SDL callback copies from the enqueued buffers in order
(queue depth = the locator's `numBuffers`; a full queue gives `SL_RESULT_BUFFER_INSUFFICIENT`)
and outputs silence unless the play state is PLAYING. When a buffer is used up it calls the
registered buffer-queue callback with our lock released, so the callback can `Enqueue`, like
Android's AudioTrack thread. `SetPlayState` never calls into SDL, so it cannot deadlock against a
running callback. STOPPED resets the position but keeps the queue, as Android does; `Clear`
empties it. Volume and mute are applied in the callback. This covers FMOD's exact call sequence
(checked against `FMOD::OutputOpenSL::init/updateOutput/start/stop/close` in libunity).

## ABI: softfp vs hardfp

Unity's armeabi-v7a libraries use the base AAPCS (no `Tag_ABI_VFP_args`): float and double
arguments and return values go in core registers. This code is armhf (VFP registers). Every shim
with floating-point arguments or results is marked `SOFTFP` (`__attribute__((pcs("aapcs")))`):
`ASensor_getResolution`, `AMotionEvent_getX/getY/getAxisValue` and `AMediaFormat_setFloat`. EGL, OpenSL ES and liblog have
no floating-point parameters (varargs always use the base convention). GLES does: Unity gets
GL functions through dlsym/eglGetProcAddress, and the loader's `softfp_gl.c`/`so_softfp_wrap()`
provides the thunks; our `eglGetProcAddress` wrapper routes results through it.

## AArch64 (arm64-v8a players)

arm64-v8a code and aarch64 Linux share AAPCS64, so `SOFTFP` is empty and no thunks are needed:
an arm64 loader need not define `so_softfp_wrap` (`egl_glue.c` references it weakly), and if it
does, it must return the function unchanged. What else changes with the word
size was checked against the NDK headers: every declaration here uses pointer-sized fields where
the NDK does (`SLDataSource`, `SLDataLocator_OutputMix`, the interface vtables and handles,
`ANativeWindow_Buffer.bits`), fixed-width integers elsewhere; `AAsset_seek/getLength/...` use
`a_off_t` = `long`, which is bionic's `off_t` on both (32-bit on ARMv7, 64-bit on AArch64);
`va_list` (`__android_log_vprint`) is the AAPCS64 struct passed by reference on bionic and glibc
alike; EGL's native window/display types are one pointer-sized word on fbdev, X11 and Android, so
`void *` still matches; Mali's `fbdev_window` is the same two `unsigned short`s on 64-bit drivers.
The unit tests pass unchanged on both architectures.

## Tests

`test/test_shims.c` (249 checks) covers: log formatting and filtering; assets (read/seek bounds,
getBuffer, openFileDescriptor, directory listing, `..` refusal); ALooper callbacks, idents, wake
and timeout; sensors and input; the window and egl_glue against `test/fake_egl.c` (fbdev size
follows the buffer geometry, attribute filtering and retries, swap hook, eglGetProcAddress, its
hidden frame-timestamp functions and the no-op presentation time; with a host display: every mapping above, `eglSurfaceAttrib` and
its re-application after a surface replacement, pass-through for other displays and surfaces,
no-op destroy/terminate); the libmediandk stubs (tables, key strings, a VideoPlayer's call
sequence failing cleanly); the same EGL against Mesa with real SDL windows (X11, Wayland) is
`runtime/host/test/display_test.sh`;
OpenSL ES driven like FMOD (resolved through `android_sles_shims` like dlsym, 4 buffers, 40
callbacks), checking SDL's disk-driver output sample by sample (sequence order, no gaps, -602 mB
gives half amplitude). The armhf test binary runs in the `ua-android-shims` image (which carries
the armhf multiarch runtime) under Docker Desktop's QEMU; the aarch64 one runs natively in
`ua-build-arm64`.

OpenSL ES layouts, constants and interface IDs follow the NDK's `OpenSLES.h`,
`OpenSLES_Android.h` and AOSP's `OpenSLES_IID.cpp`; `sles_min.h` declares only what we
implement.
