/* End-to-end test of the C UnityPlayer against a fake libmain/libunity (softfp on ARM). */
#include "../unityplayer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { R_ONLOAD, R_INITJNI, R_RESUME, R_PAUSE, R_DONE, R_FOCUS_TRUE, R_FOCUS_FALSE, R_GFX, R_GFX_NULL,
       R_RENDER, R_WIDTH, R_HEIGHT, R_REFRESH_X100, R_SDK, R_MODEL_OK, R_PATH_OK, R_PREFS_FLOAT_X100,
       R_PREFS_STR_OK, R_DOFRAME, R_HANDLE_MSG, R_FMOD_PROCESS, R_KEY_CODE, R_KEY_ACTION, R_KEY_DEV_OK,
       R_AXIS_X_X1000, R_AXIS_HAT_X1000, R_MOTION_SRC, R_RANGES, R_AUDIO_RATE, R_WEBREQ, R_SENDMSG,
       R_ORDER_OK, R_LOCALE_OK, R_ERRORS, R_N };
int fake_unity_get(int i);
jint FJ_ABI fake_libmain_onload(JavaVM *vm, void *reserved);

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

int main(void)
{
    if (system("rm -rf /tmp/uptest && mkdir -p /tmp/uptest") != 0) printf("  (could not reset /tmp/uptest)\n");
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

    CHECK(up_start(&cfg) == 0);
    CHECK(R(R_ONLOAD) == 1);
    CHECK(R(R_INITJNI) == 1);
    CHECK(R(R_WEBREQ) == 1);
    CHECK(R(R_PATH_OK) == 1);
    CHECK(R(R_MODEL_OK) == 1);
    CHECK(R(R_SDK) == 23);
    CHECK(R(R_WIDTH) == 720 && R(R_HEIGHT) == 480);
    CHECK(R(R_REFRESH_X100) == 5994);
    CHECK(R(R_LOCALE_OK) == 1);
    CHECK(R(R_PREFS_FLOAT_X100) == 75);
    CHECK(R(R_PREFS_STR_OK) == 1);
    CHECK(R(R_AUDIO_RATE) == 48000);

    CHECK(wait_for(R_RENDER, 10, 5000));
    CHECK(R(R_GFX) == 1 && R(R_FOCUS_TRUE) == 1 && R(R_RESUME) == 1);
    CHECK(R(R_ORDER_OK) == 1);
    CHECK(wait_for(R_DOFRAME, 3, 3000));
    CHECK(wait_for(R_HANDLE_MSG, 42, 3000));
    CHECK(wait_for(R_FMOD_PROCESS, 3, 3000));
    CHECK(frames_cb >= 10);
    CHECK(audio_frames >= 3 * 256);

    /* input */
    up_inject_key(5, UP_SOURCE_GAMEPAD, UP_ACTION_DOWN, UP_KEYCODE_BUTTON_A, 0, 0);
    CHECK(R(R_KEY_CODE) == 96 && R(R_KEY_ACTION) == 0 && R(R_KEY_DEV_OK) == 1 && R(R_RANGES) == 4);
    up_inject_key(5, UP_SOURCE_GAMEPAD, UP_ACTION_UP, UP_KEYCODE_BUTTON_A, 0, 0);
    CHECK(R(R_KEY_ACTION) == 1);
    int axes[2] = { UP_AXIS_X, UP_AXIS_HAT_X };
    float vals[2] = { -0.5f, 1.0f };
    up_inject_axes(5, 0, 2, axes, vals);
    CHECK(R(R_AXIS_X_X1000) == -500 && R(R_AXIS_HAT_X1000) == 1000 && R(R_MOTION_SRC) == UP_SOURCE_JOYSTICK);
    up_send_message("GameManager", "OnHello", "world");
    CHECK(R(R_SENDMSG) == 1);

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
    CHECK(wait_for(R_GFX, 2, 2000));
    up_focus(0);
    usleep(100000);
    CHECK(R(R_FOCUS_FALSE) == 1);
    up_focus(1);
    CHECK(wait_for(R_FOCUS_TRUE, 2, 2000));

    /* quit: a pause (OnApplicationPause, PlayerPrefs written), no engine cleanup (nativeDone)
       unless UA_QUIT_CLEANUP=1 */
    int pauses = R(R_PAUSE);
    up_quit();
    CHECK(R(R_PAUSE) == pauses + 1);
    CHECK(R(R_DONE) == 0);
    CHECK(!up_running());
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
    printf("renders %d, doFrame %d, fmod blocks %d, audio frames %d\n", R(R_RENDER), R(R_DOFRAME), R(R_FMOD_PROCESS), audio_frames);
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
