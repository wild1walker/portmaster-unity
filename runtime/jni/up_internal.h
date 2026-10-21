/* Internal interface between unityplayer.c (lifecycle, threads, input) and up_android.c
 * (fake Android framework classes). Not for the host. */
#ifndef UP_INTERNAL_H
#define UP_INTERNAL_H

#include <pthread.h>
#include "unityplayer.h"

#define UP_CLS "com/unity3d/player/UnityPlayer"
/* Unity 2023.1+: UnityPlayer's Activity implementation, where the per-instance natives are */
#define UP_CLS_AOS "com/unity3d/player/UnityPlayerForActivityOrService"
#define H(n) static jvalue n(JNIEnv *env, jobject self, const jvalue *a, void *ud)

#define up_log(...) fj_log("up: " __VA_ARGS__)

/* effective configuration (defaults filled in) */
extern struct up_config UPC;

/* global objects of the fake Java world (global refs) */
extern jobject g_activity, g_player, g_surface, g_surfaceview, g_holder;

/* loopers: FIFO job queues serviced by one thread */
typedef struct up_job {
    struct up_job *next;
    void (*fn)(void *arg);
    void *arg;
    jobject runnable;           /* global ref; run() is called when fn is NULL */
    int64_t at_ms;              /* earliest run time (postDelayed) */
} up_job;

typedef struct up_looper {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    up_job *head, *tail;
    int quit;
    jobject obj;                /* android.os.Looper object (global ref) */
    const char *name;
} up_looper;

extern up_looper L_ui, L_main;
extern __thread up_looper *t_looper;

void     looper_init(up_looper *l, const char *name);
void     looper_post(up_looper *l, void (*fn)(void *), void *arg, jobject runnable, int64_t delay_ms);
up_job  *looper_take(up_looper *l, int wait, int64_t max_wait_ms);
void     looper_run_job(up_job *j);
void     looper_loop(up_looper *l);            /* until quit */

int64_t  up_now_ms(void);                      /* SystemClock.uptimeMillis */
int64_t  up_now_ns(void);
int      up_on_main_thread(void);

/* UnityPlayer state used by the framework handlers */
int      up_is_finishing(void);
void     up_request_finish(const char *why);   /* Activity.finish / nativeRender false */
void     up_gl_post(void (*fn)(void *), void *arg, jobject runnable);  /* UnityPlayer.b(Runnable) */
void     up_run_gl_jobs(void);                 /* executeGLThreadJobs */
jvalue   up_native(const char *name, const char *sig, const jvalue *args);   /* UnityPlayer native */
/* call native `name` of `cls` with the signature it was registered with; `args` follow `sig`
   and are converted where the registered signature differs (see unityplayer.c) */
jvalue   up_call_native(const char *cls, const char *name, const char *sig, jobject self, const jvalue *args);
void     up_send_message_j(jstring obj, jstring method, jstring msg);   /* UnitySendMessage */
int      up_unity_quit_done(void);             /* UnityPlayer.quit() ran: no more managed calls */

/* input devices registry (unityplayer.c) */
int      up_input_device_ids(int *ids, int max);
int      up_input_device_get(int id, struct up_input_device *out);

/* event payloads (fj_data of KeyEvent / MotionEvent objects) */
#define UP_MAX_POINTERS 10
#define UP_NAXIS 48
typedef struct {
    int action, keycode, repeat, meta, device, source, scancode, flags;
    int64_t down_ms, time_ms;
} up_key_ev;
typedef struct {
    int action, source, device, n, buttons, meta, flags;
    int ids[UP_MAX_POINTERS];
    int tool[UP_MAX_POINTERS];
    float ax[UP_MAX_POINTERS][UP_NAXIS];
    int64_t down_ms, time_ms;
} up_motion_ev;

/* framework (up_android.c) */
void     up_android_register(void);            /* classes, methods, constants      */
void     up_android_create_objects(void);      /* activity, display, ...  (after register) */
jobject  up_make_file(const char *path);       /* java.io.File, local ref          */
jobject  up_make_input_device(int id);         /* android.view.InputDevice or NULL */
void     up_choreographer_tick(void);          /* UnityMain: run due FrameCallbacks */
void     up_notify_input_device(int id, int added);
void     up_fmod_stop_all(void);
void     up_mkdirs(const char *path);

/* Unity 2023.1+ Java side (up_unity6.c) */
void     up_unity6_register(void);             /* from up_android_register         */
void     up_unity6_objects(void);              /* from up_android_create_objects   */
/* Unity 5.6 - 2017.2 Java side (up_legacy.c) */
void     up_legacy_register(void);             /* from up_android_register         */
void     up_legacy_start(void);                /* after initJni: nativeFile, nativeInitWWW */

#endif
