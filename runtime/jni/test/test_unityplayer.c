/* End-to-end test of the C UnityPlayer against a fake libmain/libunity (softfp on ARM), once per
 * Unity version shape (fake_unity.h): each runs in its own process (the player is a singleton).
 *   test_unityplayer            all shapes
 *   test_unityplayer <n>        shape n only (0 = 2017.4.9 ... 6 = 2022.3) */
#include "../unityplayer.h"
#include "fake_unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int fake_unity_get(int i);
void fake_unity_set_shape(int shape);
jobject fake_unity_proxy(void);
jint FJ_ABI fake_libmain_onload(JavaVM *vm, void *reserved);

static const char *const shape_name[SHAPE_N] = { "2017.4.9", "2017.4.40", "2018.4", "2019.4", "2020.3", "2021.3", "2022.3" };

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define R(i) fake_unity_get(i)

static int frames_cb, audio_frames, quit_cb;
static void on_frame(void *u) { frames_cb++; }
static void on_audio(void *u, const int16_t *pcm, int frames, int rate) { audio_frames += frames; usleep(1000); }
static void on_quit(void *u) { quit_cb++; }

static int wait_for(int idx, int min, int ms)
{
    for (int t = 0; t < ms; t += 10) {
        up_poll();
        if (R(idx) >= min) return 1;
        usleep(10000);
    }
    return 0;
}

/* Java methods the libunity of 2017.4-2022.3 call on UnityPlayer / ReflectionHelper by name */
static const char *const java_side[][3] = {
    { "com/unity3d/player/UnityPlayer", "executeGLThreadJobs", "()V" },
    { "com/unity3d/player/UnityPlayer", "disableLogger", "()V" },
    { "com/unity3d/player/UnityPlayer", "kill", "()V" },
    { "com/unity3d/player/UnityPlayer", "addPhoneCallListener", "()V" },
    { "com/unity3d/player/UnityPlayer", "toggleGyroscopeSensor", "(Z)V" },
    { "com/unity3d/player/UnityPlayer", "initializeGoogleAr", "()Z" },
    { "com/unity3d/player/UnityPlayer", "showVideoPlayer", "(Ljava/lang/String;IIIZII)Z" },
    { "com/unity3d/player/UnityPlayer", "loadLibrary", "(Ljava/lang/String;)Z" },
    { "com/unity3d/player/UnityPlayer", "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;)V" },
    { "com/unity3d/player/UnityPlayer", "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;IZ)V" },
    { "com/unity3d/player/UnityPlayer", "showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;IZZ)V" },
    { "com/unity3d/player/UnityPlayer", "setSoftInputStr", "(Ljava/lang/String;)V" },
    { "com/unity3d/player/UnityPlayer", "hideSoftInput", "()V" },
    { "com/unity3d/player/UnityPlayer", "setCharacterLimit", "(I)V" },
    { "com/unity3d/player/UnityPlayer", "setHideInputField", "(Z)V" },
    { "com/unity3d/player/UnityPlayer", "setSelection", "(II)V" },
    { "com/unity3d/player/UnityPlayer", "getKeyboardLayout", "()Ljava/lang/String;" },
    { "com/unity3d/player/UnityPlayer", "setClipboardText", "(Ljava/lang/String;)V" },
    { "com/unity3d/player/UnityPlayer", "getClipboardText", "()Ljava/lang/String;" },
    { "com/unity3d/player/UnityPlayer", "requestUserAuthorization", "(Ljava/lang/String;)V" },
    { "com/unity3d/player/UnityPlayer", "skipPermissionsDialog", "()Z" },
    { "com/unity3d/player/UnityPlayer", "getLaunchURL", "()Ljava/lang/String;" },
    { "com/unity3d/player/UnityPlayer", "getNetworkConnectivity", "()I" },
    { "com/unity3d/player/UnityPlayer", "getNetworkProxySettings", "(Ljava/lang/String;)Ljava/lang/String;" },
    { "com/unity3d/player/UnityPlayer", "notifyOnUnityPlayerUnloaded", "()V" },
    { "com/unity3d/player/UnityPlayer", "pauseJavaAndCallUnloadCallback", "()V" },
    { "com/unity3d/player/UnityPlayer", "isUaaLUseCase", "()Z" },
    { "com/unity3d/player/UnityPlayer", "getUaaLLaunchProcessType", "()I" },
    { "com/unity3d/player/UnityPlayer", "startOrientationListener", "(I)Z" },
    { "com/unity3d/player/UnityPlayer", "stopOrientationListener", "()Z" },
    { "com/unity3d/player/UnityPlayer", "getScreenBrightness", "()F" },
    { "com/unity3d/player/UnityPlayer", "setScreenBrightness", "(F)V" },
    { "com/unity3d/player/UnityPlayer", "hidePreservedContent", "()V" },
    { "com/unity3d/player/UnityPlayer", "setMainSurfaceViewAspectRatio", "(F)V" },
    { "com/unity3d/player/UnityPlayer", "startActivityIndicator", "(I)V" },
    { "com/unity3d/player/UnityPlayer", "stopActivityIndicator", "()V" },
    { "com/unity3d/player/UnityPlayer", "setBackButtonLeavesApp", "(Z)V" },
    { "com/unity3d/player/ReflectionHelper", "endUnityLaunch", "()V" },
    { "com/unity3d/player/ReflectionHelper", "newProxyInstance", "(I[Ljava/lang/Class;)Ljava/lang/Object;" },
    { "com/unity3d/player/ReflectionHelper", "newProxyInstance", "(J[Ljava/lang/Class;)Ljava/lang/Object;" },
    { "com/unity3d/player/ReflectionHelper", "newProxyInstance", "(Lcom/unity3d/player/UnityPlayer;J[Ljava/lang/Class;)Ljava/lang/Object;" },
    { "com/unity3d/player/ReflectionHelper", "newProxyInstance", "(Lcom/unity3d/player/UnityPlayer;JLjava/lang/Class;)Ljava/lang/Object;" },
    { "com/unity3d/player/ReflectionHelper", "setNativeExceptionOnProxy", "(Ljava/lang/Object;JZ)V" },
    { "com/unity3d/player/ReflectionHelper", "createInvocationError", "(JZ)Ljava/lang/Object;" },
    { "com/unity3d/player/ReflectionHelper", "getFieldSignature", "(Ljava/lang/reflect/Field;)Ljava/lang/String;" },
    { "android/view/View", "getDisplay", "()Landroid/view/Display;" },
    { "android/view/Display", "getRealMetrics", "(Landroid/util/DisplayMetrics;)V" },
};

static int run_shape(int shape)
{
    printf("== Unity %s shape\n", shape_name[shape]);
    fake_unity_set_shape(shape);
    if (system("rm -rf /tmp/uptest && mkdir -p /tmp/uptest/lib && touch /tmp/uptest/lib/libil2cpp.so") != 0)
        printf("  (could not reset /tmp/uptest)\n");
    static const char *meta[] = { "unity.splash-mode=0", "unity.build-id=1234-test", NULL };
    struct up_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.package_name = "com.test.app";
    cfg.apk_path = "/tmp/uptest/base.apk";
    cfg.native_lib_dir = "/tmp/uptest/lib";
    cfg.files_dir = "/tmp/uptest/data/files";
    cfg.width = 720;
    cfg.height = 480;
    cfg.refresh_rate = 59.94f;
    cfg.model = "TestModel";
    cfg.sdk_int = 23;
    cfg.meta_data = meta;
    cfg.native_window = (void *)0x1234;
    cfg.libmain_onload = fake_libmain_onload;
    cfg.on_frame = on_frame;
    cfg.on_audio = on_audio;
    cfg.on_quit = on_quit;

    struct up_input_device pad = { .id = 5, .name = "Test Pad", .descriptor = "pad-5",
                                   .sources = UP_SOURCE_GAMEPAD | UP_SOURCE_JOYSTICK, .naxes = 4,
                                   .axis = { UP_AXIS_X, UP_AXIS_Y, UP_AXIS_HAT_X, UP_AXIS_HAT_Y },
                                   .min = { -1, -1, -1, -1 }, .max = { 1, 1, 1, 1 } };
    up_register_classes(&cfg);
    up_add_input_device(&pad);
    for (size_t i = 0; i < sizeof java_side / sizeof java_side[0]; i++)
        if (!fj_method_defined(java_side[i][0], java_side[i][1], java_side[i][2])) {
            printf("  missing %s.%s%s\n", java_side[i][0], java_side[i][1], java_side[i][2]);
            fails++;
        }
    int later = shape >= SHAPE_2018_4;       /* nativeSendSurfaceChangedEvent, new callbacks */

    /* startup */
    CHECK(up_start(&cfg) == 0);
    CHECK(R(R_ONLOAD) == 1);
    CHECK(R(R_INITJNI) == 1);
    CHECK(R(R_WEBREQ) == (shape <= SHAPE_2018_4));
    CHECK(R(R_PATH_OK) == 1);
    CHECK(R(R_MODEL_OK) == 1);
    CHECK(R(R_SDK) == 23);
    CHECK(R(R_WIDTH) == 720 && R(R_HEIGHT) == 480);
    CHECK(R(R_REFRESH_X100) == 5994);
    CHECK(R(R_LOCALE_OK) == 1);
    CHECK(R(R_PREFS_FLOAT_X100) == 75);
    CHECK(R(R_PREFS_STR_OK) == 1);
    CHECK(R(R_AUDIO_RATE) == 48000);
    CHECK(R(R_CALLBACKS_OK) == 1);
    CHECK(R(R_SWAPPY_JAVA) == 1);
    CHECK(R(R_IL2CPP_OK) == 1);
    CHECK(access("/tmp/uptest/data/files/il2cpp", W_OK) == 0);
    CHECK(R(R_ORIENT_LOCK) == (shape >= SHAPE_2020_3 ? 100 : 0));
    CHECK(R(R_NOWINDOW) == (shape >= SHAPE_2022_3 ? 1 : 0));

    /* render loop */
    CHECK(wait_for(R_RENDER, 10, 5000));
    if (shape >= SHAPE_2019_4) CHECK(R(R_SWAPPY_FRAME) == 1);   /* one posted frame, one call */
    CHECK(R(R_GFX) == 1 && R(R_FOCUS_TRUE) == 1 && R(R_RESUME) == 1);
    CHECK(R(R_SURF_CHANGED) == later);
    CHECK(R(R_ORDER_OK) == 1);
    CHECK(wait_for(R_DOFRAME, 3, 3000));
    CHECK(wait_for(R_HANDLE_MSG, 42, 3000));
    CHECK(wait_for(R_FMOD_PROCESS, 3, 3000));
    CHECK(frames_cb >= 10);
    CHECK(audio_frames >= 3 * 256);

    /* AndroidJavaProxy: Runnable posted to the UI thread, a call with a result, managed exceptions */
    CHECK(wait_for(R_PROXY_RUN, 1, 3000));
    CHECK(R(R_PROXY_RET) == 1);
    CHECK(R(R_PROXY_BADID) == 0);
    CHECK(R(R_PROXY_EXC_LOGGED) == (shape >= SHAPE_2019_4));
    CHECK(R(R_PROXY_FREED) == (shape >= SHAPE_2021_3));
    CHECK(R(R_SENDMSG_JAVA) == 1);
    if (shape >= SHAPE_2021_3) CHECK(wait_for(R_HIDE_PRESERVED, 1, 2000));
    else CHECK(R(R_HIDE_PRESERVED) == 0);

    /* input */
    up_inject_key(5, UP_SOURCE_GAMEPAD, UP_ACTION_DOWN, UP_KEYCODE_BUTTON_A, 0, 0);
    CHECK(R(R_KEY_CODE) == 96 && R(R_KEY_ACTION) == 0 && R(R_KEY_DEV_OK) == 1 && R(R_RANGES) == 4);
    up_inject_key(5, UP_SOURCE_GAMEPAD, UP_ACTION_UP, UP_KEYCODE_BUTTON_A, 0, 0);
    CHECK(R(R_KEY_ACTION) == 1);
    int axes[2] = { UP_AXIS_X, UP_AXIS_HAT_X };
    float vals[2] = { -0.5f, 1.0f };
    CHECK(up_inject_axes(5, 0, 2, axes, vals) == 1);
    CHECK(R(R_AXIS_X_X1000) == -500 && R(R_AXIS_HAT_X1000) == 1000 && R(R_MOTION_SRC) == UP_SOURCE_JOYSTICK);
    CHECK(R(R_INJECT_EXTRA) == (shape >= SHAPE_2022_3 ? 1000 : 0));
    up_send_message("GameManager", "OnHello", "w\xc3\xb6rld \xe2\x9c\x93");
    CHECK(R(R_SENDMSG) == 1);

    /* resize: surfaceChanged */
    up_resize(800, 600);
    CHECK(wait_for(R_GFX, 2, 2000));
    if (later) {
        CHECK(wait_for(R_SURF_CHANGED, 2, 2000));
        CHECK(R(R_SURF_W) == 800 && R(R_SURF_H) == 600);
    }

    /* lifecycle: pause / resume / surface loss */
    up_pause();
    CHECK(R(R_PAUSE) == 1);
    int r0 = R(R_RENDER);
    usleep(200000);
    CHECK(R(R_RENDER) - r0 <= 1);
    up_resume();
    CHECK(wait_for(R_RESUME, 2, 2000));
    CHECK(wait_for(R_RENDER, r0 + 5, 3000));
    up_set_surface(NULL);
    CHECK(R(R_GFX_NULL) == 1);
    up_set_surface((void *)0x5678);
    CHECK(wait_for(R_GFX, 3, 2000));
    if (later) CHECK(wait_for(R_SURF_CHANGED, 3, 2000));
    up_focus(0);
    usleep(100000);
    CHECK(R(R_FOCUS_FALSE) == 1);
    up_focus(1);
    CHECK(wait_for(R_FOCUS_TRUE, 2, 2000));

    /* quit: a pause (OnApplicationPause, PlayerPrefs written); the engine cleanup (nativeDone, ()V
       or ()Z) only with UA_QUIT_CLEANUP=1, which this test sets after 2017.4.9 */
    if (shape != SHAPE_2017_4_9) setenv("UA_QUIT_CLEANUP", "1", 1);
    int pauses = R(R_PAUSE);
    up_quit();
    CHECK(R(R_PAUSE) == pauses + 1);
    CHECK(R(R_DONE) == (shape != SHAPE_2017_4_9));
    CHECK(!up_running());
    /* the managed side is gone: proxy calls are refused */
    int runs = R(R_PROXY_RUN);
    fj_call(fake_unity_proxy(), "run", "()V");
    CHECK(R(R_PROXY_RUN) == runs);
    CHECK(R(R_ERRORS) == 0);

    /* prefs persisted */
    FILE *f = fopen("/tmp/uptest/data/shared_prefs/com.test.app.v2.playerprefs.prefs", "r");
    CHECK(f != NULL);
    if (f) {
        char buf[512] = { 0 };
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
        CHECK(strstr(buf, "f\tvolume\t0.75") && strstr(buf, "s\tname\tcup%2520head"));
    }
    if (getenv("FJ_DUMP_TEST")) fj_dump(stdout);
    printf("Unity %s: renders %d, doFrame %d, fmod blocks %d, audio frames %d: %s (%d)\n", shape_name[shape],
           R(R_RENDER), R(R_DOFRAME), R(R_FMOD_PROCESS), audio_frames, fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        int s = atoi(argv[1]);
        if (s < 0 || s >= SHAPE_N) return 2;
        int r = run_shape(s);
        printf("%s: %d failure(s)\n", r ? "FAILED" : "PASSED", fails);
        return r;
    }
    int failed = 0;
    for (int s = 0; s < SHAPE_N; s++) {
        fflush(stdout);
        pid_t p = fork();
        if (p == 0) { int r = run_shape(s); fflush(stdout); _exit(r); }
        int st = 0;
        if (p < 0 || waitpid(p, &st, 0) != p || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            printf("  FAIL Unity %s shape (status 0x%x)\n", shape_name[s], st);
            failed++;
        }
    }
    printf("%s: %d failure(s) in %d Unity version shapes\n", failed ? "FAILED" : "PASSED", failed, SHAPE_N);
    return failed ? 1 : 0;
}
