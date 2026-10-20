/*
 * unityplayer: Unity's Java player side (com.unity3d.player.UnityPlayer and the Android
 * framework objects it and libunity touch) re-implemented in C on top of fakejni. Written
 * against 2017.4; the same code serves the Android players of every LTS line up to 2022.3 by
 * calling natives with the signature each libunity registered (see README "Unity versions").
 *
 * Threads (as on Android):
 *  - "UI thread": the host thread that calls up_start() and the up_* control functions.
 *    Runnables posted to it (Activity.runOnUiThread, Handler(mainLooper).post) run in up_poll().
 *  - "UnityMain": created by up_start(); a looper that runs nativeRecreateGfxState,
 *    nativeFocusChanged, nativeResume/nativePause and the nativeRender() loop.
 * Input events are injected from any thread (the UI thread on Android) through
 * UnityPlayer.nativeInjectEvent with fake KeyEvent/MotionEvent objects.
 */
#ifndef UNITYPLAYER_H
#define UNITYPLAYER_H

#include <stdint.h>
#include "fakejni.h"

#ifdef __cplusplus
extern "C" {
#endif

/* JNI_OnLoad of an Android library (FJ_ABI, softfp on ARM; integer arguments only, so either
 * ARM convention works). */
typedef jint (FJ_ABI *up_onload_fn)(JavaVM *vm, void *reserved);

struct up_config {
    /* --- application ---------------------------------------------------------------- */
    const char *package_name;       /* default "com.pmunity.game" */
    const char *apk_path;           /* ApplicationInfo.sourceDir / getPackageCodePath():
                                       the APK (zip) holding assets/bin/Data, required */
    const char *native_lib_dir;     /* ApplicationInfo.nativeLibraryDir; libmain's
                                       NativeLoader.load() dlopens "<dir>/libunity.so" */
    const char *data_dir;           /* ApplicationInfo.dataDir (default: dirname(files_dir)) */
    const char *files_dir;          /* Context.getFilesDir(), required */
    const char *cache_dir;          /* Context.getCacheDir() (default files_dir/../cache) */
    const char *external_files_dir; /* getExternalFilesDir(null) = Application.persistentDataPath
                                       (default: files_dir) */
    const char *external_cache_dir; /* default: cache_dir */
    const char *obb_dir;            /* getObbDir() (main.<ver>.<pkg>.obb is looked up here) */
    const char *external_storage;   /* Environment.getExternalStorageDirectory() */
    int version_code;               /* PackageInfo.versionCode (default 1) */
    const char *version_name;       /* default "1.0" */
    const char *command_line;       /* Intent extra "unity" (Unity command line), may be NULL */
    const char *const *meta_data;   /* ApplicationInfo.metaData: NULL-terminated "key=value"
                                       list, e.g. "unity.build-id=...", "unity.splash-mode=0" */
    /* --- device ----------------------------------------------------------------------- */
    int width, height;              /* display size in pixels (default 640x480) */
    int density_dpi;                /* default 160 */
    float refresh_rate;             /* default 60 */
    int sdk_int;                    /* Build.VERSION.SDK_INT (default 22) */
    const char *os_release;         /* Build.VERSION.RELEASE (default "5.1") */
    const char *manufacturer, *model, *brand, *device, *product, *hardware, *fingerprint;
    const char *language, *country; /* Locale.getDefault() (default "en", "US") */
    const char *android_id;         /* Settings.Secure.ANDROID_ID */
    int cpu_count;                  /* Runtime.availableProcessors() (default 4) */
    int64_t total_mem;              /* ActivityManager.MemoryInfo.totalMem (default 1 GiB) */
    int audio_sample_rate;          /* AudioSystem.getPrimaryOutputSamplingRate() and the
                                       AudioManager OUTPUT_SAMPLE_RATE property (default 48000);
                                       FMOD's audio init fails if this is 0 */
    int audio_frames_per_buffer;    /* AudioSystem.getPrimaryOutputFrameCount() and
                                       OUTPUT_FRAMES_PER_BUFFER (default 512) */
    /* --- libraries (already loaded/relocated by the host's loader) -------------------- */
    up_onload_fn libmain_onload;    /* libmain.so JNI_OnLoad: registers NativeLoader natives;
                                       then NativeLoader.load(native_lib_dir) loads libunity */
    up_onload_fn libunity_onload;   /* used when libmain_onload is NULL (or NativeLoader.load
                                       is not registered): libunity.so JNI_OnLoad, called directly */
    /* --- rendering surface -------------------------------------------------------------- */
    void *native_window;            /* ANativeWindow* behind the android.view.Surface object:
                                       ANativeWindow_fromSurface(env, s) must return
                                       up_surface_window(s) (== fj_data(s)) */
    void *asset_manager;            /* AAssetManager* for AssetManager objects (fj_data), opt. */
    /* --- callbacks (all optional; called from Unity threads unless noted) -------------- */
    void *user;
    void (*on_quit)(void *user);                    /* Activity.finish()/kill(): Unity quits */
    void (*on_frame)(void *user);                   /* UnityMain, after each nativeRender() */
    void (*on_vibrate)(void *user, int64_t ms);     /* Vibrator.vibrate; 0 = cancel */
    int  (*on_load_library)(void *user, const char *name); /* System.loadLibrary/UnityPlayer.loadLibrary:
                                                       a short name ("main"); System.load: a full
                                                       path (IL2CPP players load libil2cpp.so so);
                                                       return 1 if loaded (and JNI_OnLoad called) */
    void (*on_open_url)(void *user, const char *url);      /* startActivity(ACTION_VIEW) */
    /* FMOD Java output (org.fmod.FMODAudioDevice / AudioTrack replacement): interleaved
     * 16-bit stereo PCM; should block like AudioTrack.write. NULL: the device thread just
     * sleeps for the buffer duration (FMOD keeps running, silent). */
    void (*on_audio)(void *user, const int16_t *pcm, int frames, int rate);
    int no_focus_on_start;          /* 1: do not send onWindowFocusChanged(true) in up_start */
};

/* Run Unity's Java startup sequence (see README "Startup sequence"). Returns 0 on success.
 * After it returns, UnityMain is running and (with a surface) rendering. */
int  up_start(const struct up_config *cfg);
void up_poll(void);                    /* UI thread: run queued runOnUiThread/Handler jobs */
void up_pause(void);                   /* Activity.onPause  -> nativePause on UnityMain   */
void up_resume(void);                  /* Activity.onResume -> nativeResume               */
void up_focus(int has_focus);          /* onWindowFocusChanged -> nativeFocusChanged      */
void up_set_surface(void *native_window); /* surfaceCreated/Changed; NULL: surfaceDestroyed */
void up_resize(int width, int height);    /* the window (display, view and surface) changed size:
                                             surfaceChanged -> nativeRecreateGfxState and, 2018.4+,
                                             nativeSendSurfaceChangedEvent */
void up_low_memory(void);              /* onLowMemory -> nativeLowMemory                   */
void up_quit(void);                    /* Activity.onDestroy -> UnityPlayer.quit()        */
int  up_running(void);                 /* UnityMain alive and not finishing               */
int  up_is_finishing(void);            /* quit requested (Unity shutting down or done)    */
void up_wait(void);                    /* join UnityMain                                   */
void up_send_message(const char *obj, const char *method, const char *msg); /* UnitySendMessage */

jobject up_activity(void);             /* global refs of the fake Java world */
jobject up_player(void);
jobject up_surface(void);
void   *up_surface_window(jobject surface);   /* for the ANativeWindow_fromSurface shim */
void   *up_asset_manager(jobject am);         /* for AAssetManager_fromJava              */

/* ---- input (android.view.KeyEvent / MotionEvent / InputDevice semantics) ---------- */
enum {
    UP_SOURCE_KEYBOARD    = 0x00000101,
    UP_SOURCE_DPAD        = 0x00000201,
    UP_SOURCE_GAMEPAD     = 0x00000401,
    UP_SOURCE_TOUCHSCREEN = 0x00001002,
    UP_SOURCE_MOUSE       = 0x00002002,
    UP_SOURCE_JOYSTICK    = 0x01000010,
};
enum { UP_ACTION_DOWN = 0, UP_ACTION_UP = 1, UP_ACTION_MOVE = 2, UP_ACTION_CANCEL = 3,
       UP_ACTION_POINTER_DOWN = 5, UP_ACTION_POINTER_UP = 6, UP_ACTION_HOVER_MOVE = 7,
       UP_ACTION_SCROLL = 8, UP_ACTION_HOVER_ENTER = 9, UP_ACTION_HOVER_EXIT = 10 };
/* MotionEvent.getButtonState bits */
enum { UP_BUTTON_PRIMARY = 1, UP_BUTTON_SECONDARY = 2, UP_BUTTON_TERTIARY = 4 };
/* a few android.view.KeyEvent / MotionEvent constants */
enum {
    UP_KEYCODE_BACK = 4, UP_KEYCODE_DPAD_UP = 19, UP_KEYCODE_DPAD_DOWN = 20,
    UP_KEYCODE_DPAD_LEFT = 21, UP_KEYCODE_DPAD_RIGHT = 22, UP_KEYCODE_DPAD_CENTER = 23,
    UP_KEYCODE_ENTER = 66, UP_KEYCODE_ESCAPE = 111, UP_KEYCODE_MENU = 82,
    UP_KEYCODE_BUTTON_A = 96, UP_KEYCODE_BUTTON_B = 97, UP_KEYCODE_BUTTON_C = 98,
    UP_KEYCODE_BUTTON_X = 99, UP_KEYCODE_BUTTON_Y = 100, UP_KEYCODE_BUTTON_Z = 101,
    UP_KEYCODE_BUTTON_L1 = 102, UP_KEYCODE_BUTTON_R1 = 103, UP_KEYCODE_BUTTON_L2 = 104,
    UP_KEYCODE_BUTTON_R2 = 105, UP_KEYCODE_BUTTON_THUMBL = 106, UP_KEYCODE_BUTTON_THUMBR = 107,
    UP_KEYCODE_BUTTON_START = 108, UP_KEYCODE_BUTTON_SELECT = 109, UP_KEYCODE_BUTTON_MODE = 110,
};
enum {
    UP_AXIS_X = 0, UP_AXIS_Y = 1, UP_AXIS_VSCROLL = 9, UP_AXIS_HSCROLL = 10, UP_AXIS_Z = 11, UP_AXIS_RX = 12, UP_AXIS_RY = 13, UP_AXIS_RZ = 14,
    UP_AXIS_HAT_X = 15, UP_AXIS_HAT_Y = 16, UP_AXIS_LTRIGGER = 17, UP_AXIS_RTRIGGER = 18,
    UP_AXIS_GAS = 22, UP_AXIS_BRAKE = 23,
};

#define UP_MAX_AXES 16
struct up_input_device {
    int id;                         /* InputDevice.getId() (> 0) */
    const char *name;               /* getName(), e.g. "Anbernic RG34XXSP Gamepad" */
    const char *descriptor;         /* getDescriptor() (stable unique string) */
    int sources;                    /* UP_SOURCE_GAMEPAD | UP_SOURCE_JOYSTICK | UP_SOURCE_DPAD */
    int vendor_id, product_id;
    int naxes;                      /* motion ranges (joystick axes) */
    int axis[UP_MAX_AXES];
    float min[UP_MAX_AXES], max[UP_MAX_AXES], flat[UP_MAX_AXES], fuzz[UP_MAX_AXES];
};
/* Register a device (visible to InputDevice.getDeviceIds/getDevice, InputManager). After
 * up_start, registered InputDeviceListeners get onInputDeviceAdded. */
int  up_add_input_device(const struct up_input_device *dev);
void up_remove_input_device(int id);
/* A KeyEvent (action UP_ACTION_DOWN/UP) -> nativeInjectEvent. Returns Unity's result. */
int  up_inject_key(int device_id, int source, int action, int keycode, int repeat, int meta);
/* A joystick MotionEvent (ACTION_MOVE, 1 pointer) with the given axis values. */
int  up_inject_axes(int device_id, int source, int n, const int *axes, const float *values);
/* A touch MotionEvent; action may include the pointer index (ACTION_POINTER_DOWN | idx<<8). */
int  up_inject_touch(int action, int n, const int *ids, const float *x, const float *y);
/* A mouse MotionEvent (SOURCE_MOUSE, TOOL_TYPE_MOUSE, one pointer) at x, y in view pixels (origin
 * top left), as Android delivers a mouse: UP_ACTION_HOVER_MOVE while no button is held, DOWN when
 * the first button goes down, MOVE while one is held (and when the held set changes), UP when the
 * last one is released, SCROLL with vscroll/hscroll (AXIS_VSCROLL > 0: wheel up). buttons:
 * UP_BUTTON_* held after the event (getButtonState). */
int  up_inject_mouse(int device_id, int action, int buttons, float x, float y, float vscroll, float hscroll);

/* Register the fake Android/Unity classes without starting (done by up_start; tests). */
void up_register_classes(const struct up_config *cfg);

#ifdef __cplusplus
}
#endif
#endif
