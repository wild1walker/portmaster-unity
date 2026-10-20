# jni/: fake JavaVM/JNIEnv + Unity's Java player in C

Two layers, both built into `out/libfakejni.a` (armhf, glibc ≤ 2.36, Debian bookworm cross gcc)
or `out/arm64/libfakejni.a` (aarch64, glibc ≤ 2.31, native gcc of the `ua-build-arm64` image):

| File | What |
|---|---|
| `fakejni.h/.c` | A complete `JavaVM`/`JNIEnv`: all 229 `JNINativeInterface` slots and the 5 `JNIInvokeInterface` slots, in `include/jni.h` (AOSP libnativehelper) order. Java classes, methods and fields are implemented by C handlers. |
| `unityplayer.h/.c` | `com.unity3d.player.UnityPlayer` reimplemented in C after the behaviour of the player package's Java side (no Unity code is included): lifecycle, the `UnityMain` looper thread, the render loop and input injection. One build serves the players of Unity 2017.4 to 2022.3 (see Unity versions). |
| `up_android.c` | The Android framework objects libunity binds: Context/Activity, paths, PackageManager, Build, Display, Window/View, SharedPreferences (persisted), Bundle/Intent, Looper/Handler/Message/HandlerThread, AudioManager/AudioSystem, Vibrator, Locale, File, Uri, MimeTypeMap, AssetManager.open+Scanner, ClassLoader/System/Runtime/Thread, collections, ReflectionHelper (AndroidJavaProxy), UnityPlayer's Java methods, Play Asset Delivery wrapper. |
| `up_input.c` | KeyEvent, MotionEvent, InputDevice/MotionRange, InputManager (+ device listeners), KeyCharacterMap, Choreographer (FrameCallback), `org.fmod.FMODAudioDevice` (FMOD's AudioTrack output as a C thread). |
| `include/jni.h` | AOSP `jni.h`, patched so that every function-table slot carries `JNI_ABI` (see ABI below; empty on AArch64). |
| `test/` | Unit tests (see Tests). |
| `build.sh`, `Makefile`, `docker/Dockerfile` | Build in docker image `ua-jni-build` (bookworm + gcc-arm-linux-gnueabihf + qemu-user), or with `ARCH=arm64` in `ua-build-arm64` (`../docker/arm64`). |

```
./build.sh                    # builds out/libfakejni.a, out/test_*; runs both tests (arm/v7 container)
./build.sh out/libfakejni.a   # library only
ARCH=arm64 ./build.sh         # out/arm64/libfakejni.a + tests, built and run natively (no QEMU)
```

## ABI (important)
### 32-bit ARM
The Android libraries are armeabi-v7a, which uses **softfp**: float/double arguments and results
go in core registers (libunity's ELF flags say "soft-float ABI"). The host is armhf (VFP registers).
- Every JNI function we hand out is compiled `__attribute__((pcs("aapcs")))` (`JNI_ABI`/`FJ_ABI`).
  Because the attribute sits on the slots in `include/jni.h`, host C code that calls through
  `(*env)->CallFloatMethod(...)` also uses the right convention automatically.
- Natives registered by Unity are called with `fj_invoke_native()` (formerly `fj_invoke_softfp`, still
  an alias): arguments are laid out as base-AAPCS words (64-bit values in even register pairs /
  8-byte-aligned stack slots) and passed to a `pcs("aapcs")` function pointer; the result is read
  from r0/r0:r1. This handles any signature of up to 40 words. Use `fj_call_native*()`, never cast
  `fj_native()` to a hard-float type.
- C handlers (`fj_fn`) are only called by fakejni and use the normal host ABI.
- Varargs (`Call*Method(...)`, `NewObject(...)`) and `va_list` (`...V`) are decoded per signature with
  `va_arg` (int-promoted Z/B/C/S, `jlong`, `double` for F, `double` for D, pointers). `...A` variants
  copy the `jvalue[]`.
- `JNI_OnLoad` pointers are typed `up_onload_fn` (FJ_ABI; integer-only, so either ABI works).

### AArch64
arm64-v8a libraries and aarch64 Linux share AAPCS64 (floats and doubles in v registers on both
sides), so `JNI_ABI`/`FJ_ABI` are empty and every JNI table function is plain C.
- `fj_invoke_native()` places the decoded arguments as AAPCS64 does: integers and pointers in
  x0-x7 (env and self first; narrow types sign/zero-extended), floats in s0-s7 (single precision,
  not promoted) and doubles in d0-d7, and each argument after its register class runs out in the
  next 8-byte stack slot, in argument order. It calls through a pointer type taking 8 integers,
  8 doubles and 26 stack integers (a float rides in the low half of a double), which is as many
  stack slots as `FJ_MAXARGS` (32) arguments can need; the result is read from x0, or d0/s0 through
  a second pointer type returning double. No assembly is involved.
- Varargs and `va_list`: `va_list` is a struct on AArch64 (passed by reference to a copy, the same
  on bionic and glibc); the `...V` functions take it as `va_list` and `decode_va` reads it with
  `va_arg` per signature (int-promoted Z/B/C/S, `jlong`, `double` for F and D, pointers), exactly as
  on ARM. Host callers of `fj_call_native(...)` must pass objects as pointers (`NULL`, never `0`).
- `Build.CPU_ABI`/`CPU_ABI2` and `System.getProperty("os.arch")` follow the build: `arm64-v8a`/""/
  `aarch64` on AArch64, `armeabi-v7a`/`armeabi`/`armv7l` on ARM.

## fakejni model
- **Handles**: every `jobject` is a pointer to an internal object (plain object, class, string,
  array) with a magic word. Local and global refs are the same pointer. Bad handles are logged, not
  dereferenced.
- **Classes** are registered from C (`fj_class_define(name, super)`, `fj_class_implements`). Methods:
  `fj_method_define(cls, name, sig, is_static, handler, ud)` or `FJ_METHOD`/`FJ_STATIC`. A handler gets
  `(env, self-or-class, const jvalue *args, ud)` and returns a `jvalue` (`fj_i/fj_z/fj_f/fj_d/fj_j/fj_l`).
  Virtual dispatch: a method id taken from a base class or interface runs the receiver class's override.
  Constructors (`<init>`) are not inherited; `NewObject` allocates and then runs the `<init>` handler.
  The handler may return a different object to stand in for the new one (as `String(byte[])` does).
- **Fields**: `fj_field_define` for static values (`fj_static_int/string/...`) and instance
  defaults. Instance values live per object and are set from C with `fj_set_int/float/obj(...)`.
- **Strings**: UTF-16 internally. `NewStringUTF` accepts modified *and* standard UTF-8;
  `GetStringUTFChars` returns modified UTF-8 (malloc'd copy, freed by Release); `fj_cstr()` gives
  standard UTF-8.
- **Arrays**: primitive arrays are real (`Get*ArrayElements`/`GetPrimitiveArrayCritical` return
  the storage itself, `isCopy=false`). Regions are bounds-checked (they raise
  ArrayIndexOutOfBoundsException). Object arrays hold references. Array classes (`[I`,
  `[Ljava/lang/String;`) are created on demand.
- **References**: refcounts cover locals (per-thread stack with `Push/PopLocalFrame`), globals, weak
  refs (treated as strong), and containment in arrays and fields. An object is freed at 0
  (`FJ_NOFREE=1` turns freeing off). Objects created from C are local refs of the calling thread:
  use `fj_global()` to keep them, and `fj_push_frame()/fj_pop_frame()` around C-side calls.
- **Threads**: any thread gets state on its first JNI call. `GetEnv` always returns `JNI_OK`
  (auto-attach). `DetachCurrentThread` frees that thread's locals. Each thread gets its own
  `JNIEnv*`; the function table is shared.
- **Exceptions**: per-thread pending exception. A handler raises one with
  `fj_throw(env, "java/lang/...", msg)`. `ExceptionCheck`/`Occurred`/`Clear`/`Describe`, `Throw`,
  `ThrowNew` and `Throwable.getMessage/toString` all work.
- **Unknown names never crash**: unknown `FindClass` returns a placeholder class. Unknown
  methods and fields get placeholder ids, and calling them returns 0/null. Each such name is logged
  **once, always** (`UNKNOWN ...`, `UNIMPLEMENTED call ...`). `fj_class_absent(name)` or
  `FJ_STRICT=1` makes `FindClass` return NULL with NoClassDefFoundError instead.
- **RegisterNatives** is recorded per class. `fj_native(cls, name, sig)` returns the fnPtr,
  `fj_native_sig(cls, name)` the signature it was registered with (natives changed signature
  across Unity versions). `fj_call_native(cls, name, sig, self, ...)` calls it with the Android
  ABI. Java-side calls to a registered native (`Call*Method` on its id) are routed to it as well.
- **Proxies**: `bitter.jnibridge.JNIBridge.newInterfaceProxy` (Unity's native listeners, e.g.
  `Choreographer$FrameCallback`, `Handler$Callback`, `Runnable`) and `ReflectionHelper.newProxyInstance`
  (managed `AndroidJavaProxy`) create proxy objects. Calls on a proxy (from Unity, or from C via
  `fj_call(obj, name, sig, ...)`) go to the registered `JNIBridge.invoke(J,Class,Method,Object[])` or
  `ReflectionHelper.nativeProxyInvoke` (through the handler `unityplayer` installs with
  `fj_set_proxy_handler`, see Unity versions). Arguments are boxed and results unboxed;
  `FromReflectedMethod` on the Method object yields the interface's jmethodID, which is how Unity's
  invoker identifies it.
- Also implemented: `ToReflected*`/`FromReflected*` (`fj_reflected_sig()` gives a reflected member's
  signature), direct ByteBuffers, `IsInstanceOf`/`IsAssignableFrom` (super chain plus interfaces),
  `GetObjectRefType`, and built-in `Object`, `Class`, `String`, `Throwable`+subclasses, boxed types
  and `reflect.Method`. `fj_method_defined()` tells whether a member has an implementation (tests,
  coverage). `MonitorEnter/Exit` are no-ops and `DefineClass` returns NULL.
- **Env vars**: `FJ_TRACE=1` logs every JNI call with decoded arguments and results. `FJ_DUMP=1`
  prints a usage report at exit (each class/method/field touched, with call counts, flagged
  `[UNKNOWN]`/`[no handler]`/`[native]`; `fj_dump()` gives it on demand). `FJ_NATIVES=1` logs
  RegisterNatives. `FJ_LOG=file` redirects the log. `FJ_NOFREE=1` and `FJ_STRICT=1` as above.

## UnityPlayer startup sequence (as implemented in `up_start`)
This mirrors `UnityPlayer.java`, `UnityPlayerActivity.java` and `UnityPlayer$e` (the UnityMain looper):
1. `UnityPlayer.<clinit>`: `loadLibrary("main")`, which calls **libmain `JNI_OnLoad`** (`cfg.libmain_onload`).
   That registers `NativeLoader.load/unload`.
2. Activity and UnityPlayer objects are created; `UnityPlayer.currentActivity = activity`.
3. **`NativeLoader.load(nativeLibraryDir)`** (libmain dlopens `<dir>/libunity.so` through the loader
   and runs libunity's `JNI_OnLoad`, which registers the UnityPlayer, FMOD, JNIBridge and
   ReflectionHelper natives). If libmain is not used, `cfg.libunity_onload` is called directly.
4. **`initJni(activity)`** on the UI (calling) thread.
5. **`nativeInitWebRequest(UnityWebRequest.class)`**.
6. Start the **UnityMain** thread: a looper with Java's idle handler.
7. `onResume` → `resume()`: `paused=false`, `f()`, `nativeRestartActivityIndicator()`.
8. `surfaceCreated`: posts **`nativeRecreateGfxState(0, surface)`** to UnityMain, then SURFACE_ACQUIRED.
9. `onWindowFocusChanged(true)`: FOCUS_GAINED, so UnityMain runs **`nativeFocusChanged(true)`** once the
   surface exists. `f()` (libs loaded, focus, not paused, not yet running) posts the GL job
   **`nativeResume()`** and the RESUME message.
10. Each UnityMain loop pass: run the queued messages, then `executeGLThreadJobs()`. If resumed and a
    surface exists, run the due Choreographer `doFrame` callbacks, then **`nativeRender()`**. A false
    return means finish. Then `cfg.on_frame`, and repeat (NEXT_FRAME).

The order observed by the test: initJni → InitWebRequest → RecreateGfxState(surface) →
[SendSurfaceChangedEvent] → FocusChanged(true) → Resume → Render... Steps of later versions run
when their natives are registered: `nativeSendSurfaceChangedEvent` after every surface
(re)attach (2018.4+), `OrientationLockListener.nativeUpdateOrientationLockState(0)` (2020.3+) and
`nativeGetNoWindowMode()` (2022.3) right after initJni; `nativeInitWebRequest` (gone in 2019.4) and
`nativeRestartActivityIndicator` (gone in 2022.3) only where registered.
- `up_pause()`: soft-input cancel/close jobs, PAUSE, then **`nativePause()`** on UnityMain (true
  means quit: `nativeDone()`). Waits up to 4 s, like Java.
- `up_resume()`, `up_focus(0/1)` (`nativeFocusChanged(false)` only after a gained focus),
  `up_set_surface(win)`. NULL means SURFACE_LOST, then `nativeRecreateGfxState(0, null)`, with a 4 s wait.
- `up_low_memory()`: `nativeLowMemory`. `up_send_message()`: `nativeUnitySendMessage`.
- `up_resize(w, h)`: the window changed size: display, view and surface sizes follow and
  surfaceChanged runs (`nativeRecreateGfxState`, then `nativeSendSurfaceChangedEvent` on 2018.4+).
- `up_quit()`: finishing, pause (which runs **`nativeDone()`**), QUIT, join UnityMain (4 s),
  stop FMOD threads, then `NativeLoader.unload()`.
- When Unity itself finishes (`Activity.finish`, `UnityPlayer.kill`, `nativeRender`→false), `cfg.on_quit` is called.
  Without that callback, the UI looper runs `up_quit(); exit(0)`.

### Threads and the host loop
The thread that calls `up_start` plays Android's **UI thread**. It must call `up_poll()` regularly,
which runs `runOnUiThread` jobs and `Handler(getMainLooper())` posts. Input can be injected from it
(Android injects on the UI thread). Unity's own Handlers or HandlerThreads get real looper threads.

### Host usage
```c
#include "jni/unityplayer.h"
struct up_config c = {0};
c.apk_path = "/path/base.apk";             // zip with assets/bin/Data (Unity reads it itself)
c.native_lib_dir = "/path/lib";            // where the loader resolves libunity.so for libmain
c.files_dir = "/path/data/files";          // persistentDataPath = external_files_dir (default files_dir)
c.width = 720; c.height = 480; c.density_dpi = 160; c.refresh_rate = 60;
c.native_window = shim_window;             // ANativeWindow* (android shim)
c.libmain_onload = (up_onload_fn)so_sym(libmain, "JNI_OnLoad");
c.on_quit = my_quit; c.on_audio = NULL;    // see unityplayer.h for every field/callback
up_add_input_device(&(struct up_input_device){ .id = 1, .name = "Gamepad", .descriptor = "pad-1",
    .sources = UP_SOURCE_GAMEPAD | UP_SOURCE_JOYSTICK | UP_SOURCE_DPAD, .naxes = 4,
    .axis = { UP_AXIS_X, UP_AXIS_Y, UP_AXIS_HAT_X, UP_AXIS_HAT_Y } });
up_start(&c);
for (;;) { up_poll(); /* SDL events -> */ up_inject_key(1, 0, UP_ACTION_DOWN, UP_KEYCODE_BUTTON_A, 0, 0);
           up_inject_axes(1, 0, n, axes, values); ... }
```
- A mouse (the host's pointer mode): register a device with `UP_SOURCE_MOUSE` and send
  `up_inject_mouse(id, action, buttons, x, y, vscroll, hscroll)`: `UP_ACTION_HOVER_MOVE` while no
  button is held, `DOWN` / `MOVE` / `UP` with the `UP_BUTTON_*` state, `UP_ACTION_SCROLL` with the
  wheel; `up_inject_touch` for a finger.
- The android shim's `ANativeWindow_fromSurface(env, surface)` may use `up_surface_window(surface)`
  (== `fj_data(surface)`, the `cfg.native_window`). It currently returns its single window for any
  non-NULL Surface, which also works. `up_asset_manager(obj)` maps AssetManager objects to `cfg.asset_manager`.
- Link order: `-lfakejni` (from `out/`, or `out/arm64/`), `-lpthread`, `-lm`. Include path is the `jni/` dir.
- FMOD: `AudioSystem.getPrimaryOutputSamplingRate()/getPrimaryOutputFrameCount()` return
  `cfg.audio_sample_rate` (48000) and `cfg.audio_frames_per_buffer` (512); FMOD fails to init if either is 0.
  If FMOD chooses its Java output (`FMODAudioDevice.start()`), a C thread pumps
  `fmodGetInfo/fmodProcess` and passes 16-bit stereo PCM to `cfg.on_audio` (or sleeps if NULL).
  The OpenSL path is the android shim's.
- PlayerPrefs (SharedPreferences) persist in `<data_dir>/shared_prefs/<name>.prefs` (one
  `type\tkey\tvalue` per line, %-escaped) and are saved on commit/apply.
- `Intent.getStringExtra("unity")` returns `cfg.command_line`. `ApplicationInfo.metaData` comes from
  `cfg.meta_data` ("key=value"). The battery intent reports full; there is no network
  (`getActiveNetworkInfo` returns null), all permissions are granted and `SDK_INT` defaults to 22.

## Tests (`./build.sh` in an arm/v7 container, `ARCH=arm64 ./build.sh` natively in ua-build-arm64)
- `test/test_fakejni.c` + `test/android_side.c`: on ARM android_side is compiled **`-mfloat-abi=softfp
  -ffreestanding`**, like NDK code, and linked with `--no-warn-mismatch`; on AArch64 it is ordinary
  code (same ABI as the host). It calls every Call*
  family (plain varargs, `V` through real `va_list`, `A`, Nonvirtual, Static) with mixed
  I/J/F/D/L/Z/B/C/S arguments, float/double returns and field setters, plus NewObject with a float,
  virtual dispatch, exceptions, placeholders, absent classes, strings (UTF-16, modified UTF-8,
  4-byte UTF-8), arrays, local frames, global refs and RegisterNatives. Two natives take more
  arguments than there are registers (`wide`: 22 mixed arguments, 8 of them on the AArch64 stack
  including a float; `fsum`: 10 floats, float result), called from the Java side and from C. The
  host side calls the natives through `fj_call_native(_a)`, covers refcount freeing, direct
  buffers and boxing, and runs 4 threads.
- `test/test_unityplayer.c` + `test/fake_unity.c` (softfp on ARM): a fake libmain/libunity that
  registers the natives of one of 7 Unity versions (2017.4.9, 2017.4.40, 2018.4, 2019.4, 2020.3,
  2021.3, 2022.3: names and signatures as `tools/re/jnitables.py` lists them for each player) and
  uses the Java side as that version does. Each version runs in its own process
  (`test_unityplayer [n]` runs one). It checks the full startup order (with
  `nativeSendSurfaceChangedEvent` from 2018.4), display metrics (`getRefreshRate` float return;
  through `View.getDisplay()` from 2019.4), Build/paths/locale, SharedPreferences round trip and
  persistence, AudioSystem, Choreographer doFrame through a JNIBridge proxy, Handler/Message to a
  `Handler$Callback` proxy, the FMODAudioDevice thread, KeyEvent/MotionEvent/InputDevice reads
  (`getAxisValue(II)F`) through each `nativeInjectEvent` signature, UnitySendMessage (String and
  UTF-8 byte[] forms, from C and from the static Java method), AndroidJavaProxy with int or long
  ids and each version's `newProxyInstance` (a Runnable posted with `runOnUiThread`, a call with
  an argument and a boxed result, managed exceptions of the 2019.4 and 2021.3 kinds, calls refused
  after quit), the new UnityPlayer callbacks, Swappy's classes being absent, IL2CPP's library path
  and `<files>/il2cpp`, `up_resize`, the 2020.3/2022.3 startup natives, pause/resume/surface
  loss/focus, and quit with `nativeDone` of either signature. It also checks that every Java
  method the 2017.4-2022.3 libunity call on UnityPlayer/ReflectionHelper by name is implemented.
- Both pass (0 failures) on both architectures: `test_unityplayer` prints one line per version and
  `PASSED: 0 failure(s) in 7 Unity version shapes`. They also pass with `FJ_TRACE=1`. The tests
  assume the default FJ_* modes: `FJ_NOFREE`/`FJ_STRICT` intentionally break their
  refcount/placeholder checks. (Docker's arm/v7 emulation on a Mac has crashed in the thread part
  of the armhf tests before; the same binaries pass on the device.)

## Unity versions (2017.4 to 2022.3)
The runtime was written against 2017.4.9 (Cuphead); `docs/compat` surveys the Android players of
the last patch of every LTS line up to 2022.3. The Java side keeps the 2017.4 model throughout, so
one build serves them all, without a version switch: what differs is decided by what the loaded
libunity registers.

**Natives by name.** `up_call_native()` looks a native up by name and calls it with the signature
it was registered with, converting the caller's arguments by position (String to UTF-8 byte[],
int and long, narrower integers; missing ones are 0/null). It logs the first adaptation of each
native.

| native | 2017.4.9 | later players | runtime/jni |
|---|---|---|---|
| `nativeUnitySendMessage` (static) | `(String, String, String)V` | `(String, String, byte[])V` from 2017.4.40 | the message as UTF-8 bytes (Java's `getBytes("UTF-8")`); also for `UnityPlayer.UnitySendMessage` called from managed code |
| `nativeDone` | `()V` | `()Z` from 2019.4 (true: Java kills the process) | still only with `UA_QUIT_CLEANUP=1`; the result is logged |
| `nativeInjectEvent` | `(InputEvent)Z` | `(InputEvent, int)Z` in 2022.3 | the int is 0, as Java's `injectEvent(event)` passes |
| `ReflectionHelper.nativeProxy*` | `int` proxy id | `long` from 2019.4 | the id as the registered type |
| `nativeInitWebRequest`, `nativeRestartActivityIndicator` | yes | gone in 2019.4 / 2022.3 | called when registered |
| `nativeSendSurfaceChangedEvent` | - | 2018.4+ | after `nativeRecreateGfxState(0, surface)` (surfaceChanged), on start, surface change and `up_resize` |
| `OrientationLockListener.nativeUpdateOrientationLockState(I)` | - | 2020.3+ | 0 (auto-rotate off) after initJni |
| `nativeGetNoWindowMode()Z` | - | 2022.3 | asked after initJni and logged; a surface is provided either way |
| `nativeHidePreservedContent` | - | 2021.3+ | GL-thread job after `UnityPlayer.hidePreservedContent()` (through the UI thread; the pairing is inferred from the names) |

**AndroidJavaProxy** (`ReflectionHelper`, used by every `AndroidJavaProxy`/`AndroidJavaRunnable`
of the game's managed code): all six `newProxyInstance` forms (`(int, Class|Class[])` up to 2018.4,
`(long, ...)` in 2019.4, `(UnityPlayer, long, ...)` from 2020.3). A call on a proxy goes through
the handler `unityplayer` installs: refused for proxies of an ended launch (`endUnityLaunch`) and
after quit (Java's `beginProxyCall`), then `nativeProxyInvoke(id, name, Object[])`. A managed
exception (flagged with `setNativeExceptionOnProxy` in 2019.4/2020.3, returned as the object of
`createInvocationError` from 2021.3) is logged through `nativeProxyLogJNIInvokeException(handle)`,
its handle released with `nativeProxyJNIFreeGCHandle` (2021.3+), and the caller gets null/0 where
Java would throw. `getFieldSignature` is implemented; `nativeProxyFinalize` is never called (proxy
objects are freed without telling the managed side, which keeps its GC handle).

**UnityPlayer methods libunity added** (its `UnityPlayerJavaWrapper`, 2018.4-2022.3), answered
as for a handheld:

| method | answer |
|---|---|
| `showSoftInput` (9 and 10 arguments), `setCharacterLimit`, `setHideInputField`, `setSelection` | logged/ignored: no on-screen keyboard (as the 7-argument one) |
| `getClipboardText`/`setClipboardText` | a process-local clipboard |
| `requestUserAuthorization`, `skipPermissionsDialog` | nothing to ask (every permission is granted), false |
| `getLaunchURL`, `getNetworkConnectivity`, `getNetworkProxySettings`, `getKeyboardLayout` | null (no deep link), 0 (not reachable), null, null |
| `isUaaLUseCase`, `getUaaLLaunchProcessType` | false, 0: a game, not Unity as a Library |
| `notifyOnUnityPlayerUnloaded` (2019.4), `pauseJavaAndCallUnloadCallback` (2020.3+) | `Application.Unload`: the activity would go to the back; here the game quits |
| `startOrientationListener`, `stopOrientationListener` | false: no orientation sensor |
| `getScreenBrightness`/`setScreenBrightness` (2021.3+) | 1.0, then what the game set; the panel is not changed |
| `hidePreservedContent` (2021.3+) | acknowledged with `nativeHidePreservedContent` (inferred pairing) |
| `setMainSurfaceViewAspectRatio`, `startActivityIndicator`/`stopActivityIndicator`, `setBackButtonLeavesApp` (2022.3) | ignored |

**Frame pacing (Swappy, 2019.4+).** libunity's Swappy looks for its Java helpers
(`com.google.androidgamesdk.ChoreographerCallback`, `SwappyDisplayManager`) through the activity's
ClassLoader and then loads them from its embedded dex with `InMemoryDexClassLoader` (or, before
API 26, writes the dex to the cache dir for `PathClassLoader`). All four classes are absent here
(`ClassLoader.loadClass` throws ClassNotFoundException as on Android), so no dex is written or
run; Swappy then uses its own timer instead of a choreographer and turns itself off when EGL has no
`eglPresentationTimeANDROID` ("Swappy is disabled"; the Android Game SDK's behaviour, not yet seen
with a real player here), and Unity paces frames as 2017.4 does (vsync, Choreographer callbacks
through JNIBridge). Whether that EGL function resolves is up to the
android/EGL layer: it should not, for this to hold. The converter could also turn
`androidUseSwappy` off; that changes pacing, not the picture.

**IL2CPP players** find `libil2cpp.so` with `ClassLoader.findLibrary("il2cpp")`
(`<native_lib_dir>/libil2cpp.so` when present, else null) and `System.load` it (the host's
`on_load_library` gets the full path), then copy their data to `<files_dir>/il2cpp` with libc.
`up_register_classes` creates `files_dir` and warns when it is not writable.

**Other Java the later players use**: `View.getDisplay()` (2019.4+ reads the display through it:
the display), `Display.getRealMetrics/getSupportedModes/getMode` (one mode: the panel),
`getAppVsyncOffsetNanos` (0) and `getPresentationDeadlineNanos` (one refresh period), HDR and wide
colour gamut (none), window insets (none), `PowerManager.isSustainedPerformanceModeSupported`
(false), `Process.setThreadPriority` (ignored: thread priorities stay as on 2017.4),
`AudioManager.getDevices` (the built-in speaker), `Settings.Global` `DEVICE_NAME` (the model),
`Activity.moveTaskToBack` (ignored, false), `PackageInfo.getLongVersionCode`, more KeyEvent/
MotionEvent getters, and `PlayAssetDeliveryUnityWrapper` answering as in an app built without
the Play Core library (`playCoreApiMissing`, no asset pack paths). `cfg.sdk_int` should be at
least the player's minimum API (22 for 2021.3 and 2022.3); libunity skips newer Android paths
below their API level.

**Coverage** (`tools/re/jnicover.c` on each player's `bindings.tsv`, the Android framework
members libunity binds, and `javacalls.tsv`, the player's own Java members whose names occur in
libunity; "ok" rows of all rows, before → after this work):

| | 2017.4.9 | 2017.4.40 | 2018.4.36 | 2019.4.40 | 2020.3.48 | 2021.3.45f2 | 2022.3.62f3 |
|---|---|---|---|---|---|---|---|
| framework members (bindings) | 248 → 250 / 398 | 251 → 254 / 402 | 267 → 287 / 458 | 268 → 306 / 487 | 266 → 309 / 487 | 262 → 308 / 487 | 250 → 304 / 395 |
| missing members of known classes | 4 → 2 | 5 → 2 | 20 → 2 | 37 → 2 | 38 → 2 | 38 → 3 | 46 → 3 |
| player Java members (javacalls) | 19 / 43 | - | - | 19 → 46 / 112 | 18 → 46 / 105 | 18 → 49 / 121 | 17 → 52 / 91 |
| UnityPlayer methods libunity calls | 12 / 12 | 12 / 12 | 11 → 19 / 19 | 11 → 28 / 28 | 10 → 27 / 27 | 10 → 30 / 30 | 10 → 34 / 34 |
| natives runtime/jni calls, called with the registered signature | all | 1 ✗ → all | 1 ✗ → all | 3 ✗ → all | 3 ✗ → all | 3 ✗ → all | 4 ✗ → all |

(✗: `nativeUnitySendMessage` from 2017.4.40, `nativeDone` and `nativeProxyInvoke` from 2019.4,
`nativeInjectEvent` in 2022.3.) The remaining rows are classes with no use on a handheld (camera, VR/AR, video, web requests and
TLS, dialogs, telephony, location, Wi-Fi...: placeholders that answer 0/null as before),
`Surface(SurfaceTexture)`/`SurfaceView(Context)` (video), and in javacalls Java-only entry points
whose names libunity happens to contain (`UnityPlayer.pause/resume/quit/onKeyDown...`: the
Activity calls them, here `up_*`), Swappy's classes (absent on purpose) and obfuscated helpers.
The 2017.4.9 additions (`AudioAttributes.Builder.build`, `AudioFocusRequest.Builder.build`) are
only reached at API 26+, so Cuphead (API 19) runs as before.

**Not covered yet** (beyond the general gaps below): none of these players has been run, only
their native tables and Java bindings; `nativeOrientationChanged` (2019.4+, sent by Java's
orientation listener) and `nativeSetLaunchURL` (deep links) are never sent; `Application.Unload`
quits; Play Asset Delivery has no asset packs and split APKs are not listed
(`splitPublicSourceDirs` null); the 2019.4+ native UnityWebRequest's certificate checks meet
`TrustManagerFactory` placeholders; `nativeGetNoWindowMode` returning true is only logged; and the
2022.3 player additionally needs the libmediandk stub of the loader to load at all.

Reproduce: build jnicover as its header says, then for each player
`jnicover < notes/bindings.tsv` and `jnicover < notes/javacalls.tsv` (notes from
`tools/re/survey_player.sh` and `tools/re/javacalls.py`).

## What Unity may still call that is missing or shallow
The authoritative list of what libunity can bind comes from `tools/re/strrefs.py` + `tools/re/jnibindings.py` run on your own copy of the player. Run the real
thing with `FJ_DUMP=1` (or `FJ_TRACE=1`) and look for `UNKNOWN`/`UNIMPLEMENTED`. Known gaps:
- **Video** (`MediaCodec`, `MediaExtractor`, `MediaFormat`, `SurfaceTexture`, `Surface(SurfaceTexture)`)
  and `UnityPlayer.showVideoPlayer` (returns false). A game that plays videos through Unity's VideoPlayer on this path gets none.
- **UnityWebRequest** Java class (`com/unity3d/player/UnityWebRequest` instances: networking, up
  to 2018.4; native from 2019.4, with TLS checks through `TrustManagerFactory` placeholders), the
  soft keyboard (`showSoftInput` is logged only), AlertDialog/Dialog/ProgressBar (activity indicator)
  and Presentation/MediaRouter (second display): placeholders.
- 2019.4+: Play Asset Delivery (asset packs: none), `Camera2Wrapper`, `HFPStatus` (Bluetooth headset
  microphone), Google VR/ARCore, `java.util.zip.ZipFile` (2021.3), `PointerIcon`/`Bitmap` (cursor
  images), `DisplayCutout`/`WindowInsets` (none), `NetworkInterface`: placeholders or none.
- Camera, location, `GeomagneticField`, `Parcel`/`IBinder`/`bindService` (returns false),
  `ParcelFileDescriptor`: placeholders. Sensors go through the NDK `ASensor*` shim, not JNI.
- `SharedPreferences.getAll()` returns an empty map, which only matters for Unity's v1→v2 PlayerPrefs migration.
- `DisplayManager`/`View.OnSystemUiVisibilityChange` listeners are accepted but never called.
  InputDevice listeners get `onInputDeviceAdded/Removed` on UnityMain when the host adds or removes
  devices after start.
- `MotionEvent` history is always empty (`getHistorySize()==0`). KeyCharacterMap maps basic US keys only.
- `JNIBridge.delete(J)` is never called (proxy objects are just freed). Monitors are no-ops.
- Risk: if libunity keeps a *local* ref past its frame (a bug that ART would also fail on), freeing
  could make it dangle. Try `FJ_NOFREE=1` if you see `invalid object handle` warnings.
