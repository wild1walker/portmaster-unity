# android/: Android API shims (liblog, libandroid, OpenSL ES, EGL glue)

C shims for the Android-only libraries that Unity 2017.4.9f1's ARMv7 player
(`libunity.so`, `libmain.so`, `libmono.so`) imports or dlopens, built for armhf glibc
(Debian bookworm cross toolchain, glibc symbols up to 2.34).

```
./build.sh        # -> out/libandroidshims.a (+ out/obj/*.o), out/test/*
./build.sh test   # also runs the unit tests under QEMU (no GPU), twice: SDL "disk" and "dummy" audio
```
Link the host with `out/libandroidshims.a -lSDL2 -ldl -lpthread -lm`. Do not link a real libEGL
for the shims' sake: `egl_glue.c` dlopens it.

## What the libraries need (from `readelf`; `tools/re/elfimports.py` lists them for any player)

The DT_NEEDED entries and undefined dynamic symbols of the 2017.4 player; the Android-only ones are:

| Library | Needed by | Symbols |
|---|---|---|
| liblog.so | libunity, libmain, libmono | `__android_log_print`, `__android_log_vprint`, `__android_log_write` (libmain only lists liblog, it imports nothing from it) |
| libandroid.so | libunity, libmain | `ANativeWindow_{fromSurface,acquire,release,getWidth,getHeight,setBuffersGeometry}`, `ALooper_{forThread,prepare}`, `ASensorManager_{getInstance,getSensorList,getDefaultSensor,createEventQueue,destroyEventQueue}`, `ASensorEventQueue_{enableSensor,disableSensor,setEventRate,hasEvents,getEvents}`, `ASensor_{getName,getVendor,getType,getResolution,getMinDelay}`, `AInputEvent_{getType,getDeviceId}`, `AKeyEvent_{getAction,getKeyCode,getMetaState}` |
| libOpenSLES.so | libunity (FMOD) via dlopen | `slCreateEngine`, `SL_IID_ENGINE`, `SL_IID_ANDROIDSIMPLEBUFFERQUEUE`, `SL_IID_PLAY`, `SL_IID_RECORD`, `SL_IID_ANDROIDCONFIGURATION` (data symbols) |
| libEGL.so | libunity | 21 `egl*` functions (host libEGL; 6 are wrapped, see below) |
| libGLESv2.so | libunity | nothing imported: GL is loaded with dlopen/dlsym and eglGetProcAddress |

Notes from the disassembly (with the symbols in `Symbols/armeabi-v7a/libunity.sym.so`):
- The `AInputEvent_*`/`AKeyEvent_*` imports have no relocations: Unity never calls them (input
  arrives through the Java side). AAssetManager, AConfiguration, ANativeActivity and AStorageManager
  are not imported at all: Unity reads the APK zip itself.
- The sensor calls come from `InitNativeSensors()` etc.: Unity handles an empty sensor list and a
  NULL default sensor.
- libunity also dlopens `libmediandk.so` (AMediaCodec/AMediaExtractor for VideoPlayer),
  `libvulkan.so`, `libSEC_OMX_Core.so`, `libsomxcore.so`. We have no shims for them:
  `android_shims_for_library()` returns NULL and the loader's dlopen should fail, which makes
  Unity fall back (GLES instead of Vulkan, no hardware video decoding).

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
so_map_library("libmediandk.so", NULL);     // no shims: let these dlopens fail
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

Environment variables: `ANDROID_LOG_LEVEL`, `ANDROID_ASSET_DIR`, `ANDROID_WINDOW_SIZE=WxH`,
`ANDROID_FBDEV` (default /dev/fb0), `ANDROID_SHIM_LIBEGL` (real libEGL path, default libEGL.so.1
then libEGL.so), `ANDROID_SLES_SAMPLES` (SDL buffer in frames, default 512), `ANDROID_SLES_DEVICE`
(SDL output device name), `ANDROID_SHIM_TRACE=1` (trace shim calls), plus SDL's own
(`SDL_AUDIODRIVER`, ...).

## EGL and the window

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
  wrapped names.

All other `egl*` imports can resolve to the host libEGL directly (`egl_glue_real(name)` returns
the real symbol). The real libEGL is dlopened on first use, so EGL calls can come from any of
Unity's threads.

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
`ASensor_getResolution` and `AMotionEvent_getX/getY/getAxisValue`. EGL, OpenSL ES and liblog have
no floating-point parameters (varargs always use the base convention). GLES does: Unity gets
GL functions through dlsym/eglGetProcAddress, and the loader's `softfp_gl.c`/`so_softfp_wrap()`
provides the thunks; our `eglGetProcAddress` wrapper routes results through it.

## Tests

`test/test_shims.c` (163 checks) covers: log formatting and filtering; assets (read/seek bounds,
getBuffer, openFileDescriptor, directory listing, `..` refusal); ALooper callbacks, idents, wake
and timeout; sensors and input; the window and egl_glue against `test/fake_egl.c` (fbdev size
follows the buffer geometry, attribute filtering and retries, swap hook, eglGetProcAddress);
OpenSL ES driven like FMOD (resolved through `android_sles_shims` like dlsym, 4 buffers, 40
callbacks), checking SDL's disk-driver output sample by sample (sequence order, no gaps, -602 mB
gives half amplitude). The test binary runs in the arm64 build image with the armhf multiarch
runtime, under Docker Desktop's QEMU.

OpenSL ES layouts, constants and interface IDs follow the NDK's `OpenSLES.h`,
`OpenSLES_Android.h` and AOSP's `OpenSLES_IID.cpp`; `sles_min.h` declares only what we
implement.
