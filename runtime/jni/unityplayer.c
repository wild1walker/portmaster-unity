/*
 * unityplayer.c - lifecycle, threads and input of Unity's com.unity3d.player.UnityPlayer,
 * reimplemented in C from how the player package's Java side behaves (no Unity code is included).
 * The model is 2017.4's and holds up to 2022.3 (same UnityMain looper and natives); natives are
 * called with the signature the loaded libunity registered (up_call_native), and the steps later
 * versions added run only when their natives are registered.
 *
 * Behaviour reproduced here:
 *   UnityPlayer.<clinit>   : loadLibrary("main")              -> libmain JNI_OnLoad
 *   UnityPlayer(Context)   : currentActivity = activity
 *                            NativeLoader.load(nativeLibraryDir) -> libunity JNI_OnLoad
 *                            initJni(context)                   (UI thread)
 *                            SurfaceView, window flags
 *                            nativeInitWebRequest(UnityWebRequest.class)
 *                            UnityMain thread start (Looper + idle handler)
 *   onResume  -> resume()  : paused=false; f(); nativeRestartActivityIndicator()
 *   surfaceCreated/Changed : UnityMain: nativeRecreateGfxState(0, surface); SURFACE_ACQUIRED
 *   onWindowFocusChanged   : FOCUS_GAINED -> nativeFocusChanged(true) once a surface exists
 *   f()                    : when loaded && focus && !paused && !running:
 *                            GL job nativeResume(); RESUME message
 *   idle handler           : executeGLThreadJobs(); if resumed && surface: nativeRender()
 *   onPause   -> pause()   : PAUSE; UnityMain: nativePause() (true => finish: nativeDone())
 *   quit()                 : pause (nativePause); nativeDone only with UA_QUIT_CLEANUP=1; QUIT; NativeLoader.unload()
 * Added by later versions (when registered):
 *   2018.4 surfaceChanged  : nativeSendSurfaceChangedEvent() after nativeRecreateGfxState
 *   2020.3 constructor     : OrientationLockListener.nativeUpdateOrientationLockState(auto-rotate)
 *   2022.3 constructor     : nativeGetNoWindowMode()
 */
#define _GNU_SOURCE
#include "up_internal.h"

#include <errno.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct up_config UPC;
jobject g_activity, g_player, g_surface, g_surfaceview, g_holder;
up_looper L_ui, L_main;
__thread up_looper *t_looper;

static pthread_t g_main_thread;
static int g_main_started;
static pthread_mutex_t g_state_mu = PTHREAD_MUTEX_INITIALIZER;

/* Java state: class l (f) and UnityPlayer fields, plus UnityMain thread (class e) fields */
static struct {
    volatile int loaded;     /* l.a: native libraries loaded            */
    volatile int perm;       /* l.b: permissions granted                */
    volatile int focus;      /* l.c: window focus                       */
    volatile int running;    /* l.d: nativeResume sent                  */
    volatile int paused;     /* l.e: activity paused (initially true)   */
    volatile int finishing;  /* UnityPlayer.s                           */
    /* UnityMain thread (UnityPlayer$e) */
    int m_resumed;           /* e.b */
    int m_surface;           /* e.c */
    int m_focus;             /* e.d: 1 = focused, 2 = lost, 3 = pending  */
    int quit_done;
} S = { 0, 1, 0, 0, 1, 0, 0, 0, 2, 0 };

/* ======================================================================================= */
/* time, loopers                                                                           */
/* ======================================================================================= */

int64_t up_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
int64_t up_now_ms(void) { return up_now_ns() / 1000000; }

int up_on_main_thread(void) { return g_main_started && pthread_equal(pthread_self(), g_main_thread); }

void looper_init(up_looper *l, const char *name)
{
    pthread_mutex_init(&l->mu, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&l->cv, &ca);
    l->head = l->tail = NULL;
    l->quit = 0;
    l->name = name;
}

void looper_post(up_looper *l, void (*fn)(void *), void *arg, jobject runnable, int64_t delay_ms)
{
    up_job *j = calloc(1, sizeof *j);
    if (!j) abort();
    j->fn = fn;
    j->arg = arg;
    j->runnable = runnable ? fj_global(runnable) : NULL;
    j->at_ms = delay_ms > 0 ? up_now_ms() + delay_ms : 0;
    pthread_mutex_lock(&l->mu);
    if (l->tail) l->tail->next = j; else l->head = j;
    l->tail = j;
    pthread_cond_broadcast(&l->cv);
    pthread_mutex_unlock(&l->mu);
}

/* take the first due job; optionally wait up to max_wait_ms (<0: forever) */
up_job *looper_take(up_looper *l, int wait, int64_t max_wait_ms)
{
    int64_t deadline = max_wait_ms >= 0 ? up_now_ms() + max_wait_ms : -1;
    pthread_mutex_lock(&l->mu);
    for (;;) {
        int64_t now = up_now_ms(), next_at = -1;
        up_job *prev = NULL;
        for (up_job *j = l->head; j; prev = j, j = j->next) {
            if (j->at_ms <= now) {
                if (prev) prev->next = j->next; else l->head = j->next;
                if (l->tail == j) l->tail = prev;
                pthread_mutex_unlock(&l->mu);
                j->next = NULL;
                return j;
            }
            if (next_at < 0 || j->at_ms < next_at) next_at = j->at_ms;
        }
        if (!wait || l->quit) break;
        int64_t until = next_at;
        if (deadline >= 0 && (until < 0 || deadline < until)) until = deadline;
        if (deadline >= 0 && now >= deadline) break;
        if (until < 0) pthread_cond_wait(&l->cv, &l->mu);
        else {
            struct timespec ts = { until / 1000, (until % 1000) * 1000000 };
            pthread_cond_timedwait(&l->cv, &l->mu, &ts);
        }
    }
    pthread_mutex_unlock(&l->mu);
    return NULL;
}

void looper_run_job(up_job *j)
{
    fj_push_frame();
    if (j->fn) j->fn(j->arg);
    else if (j->runnable) fj_call(j->runnable, "run", "()V");
    fj_pop_frame();
    if (j->runnable) fj_release(j->runnable);
    free(j);
}

void looper_loop(up_looper *l)
{
    up_looper *saved = t_looper;
    t_looper = l;
    while (!l->quit) {
        up_job *j = looper_take(l, 1, -1);
        if (j) looper_run_job(j);
    }
    t_looper = saved;
}

static void looper_wake(up_looper *l)
{
    pthread_mutex_lock(&l->mu);
    pthread_cond_broadcast(&l->cv);
    pthread_mutex_unlock(&l->mu);
}

/* ======================================================================================= */
/* natives                                                                                 */
/* ======================================================================================= */

/* Natives are looked up by name and called with the signature the library registered: Unity
 * changed some across versions (nativeUnitySendMessage takes the message as byte[] from 2017.4.40,
 * nativeDone returns a boolean from 2019.4, nativeInjectEvent gained an int in 2022.3,
 * ReflectionHelper's proxy ids became long in 2019.4). `sig` is the signature the caller's
 * arguments follow; when the registered one differs, each argument is converted to the
 * registered parameter at its position (String -> byte[] as UTF-8, int <-> long, boolean/char/
 * short/byte <-> int) and missing ones are 0/null. The result is the registered return value. */

/* parameter descriptors of a method signature: start pointers and lengths; returns the count */
static int sig_params(const char *sig, const char **p, int *len, int max)
{
    int n = 0;
    if (!sig || *sig != '(') return 0;
    const char *s = sig + 1;
    while (*s && *s != ')' && n < max) {
        const char *b = s;
        while (*s == '[') s++;
        if (*s == 'L') { while (*s && *s != ';') s++; if (*s) s++; }
        else if (*s) s++;
        p[n] = b;
        len[n++] = (int)(s - b);
    }
    return n;
}

static int is_ref(char c) { return c == 'L' || c == '['; }
static int is_intlike(char c) { return c == 'Z' || c == 'B' || c == 'C' || c == 'S' || c == 'I'; }

static jvalue adapt_arg(const char *want, int wlen, const char *have, int hlen, jvalue v)
{
    if (wlen == hlen && !strncmp(want, have, wlen)) return v;
    char w = *want, h = *have;
    if (w == '[' && wlen == 2 && want[1] == 'B' && h == 'L' && !strncmp(have, "Ljava/lang/String;", hlen)) {
        const char *s = fj_cstr(v.l);   /* standard UTF-8, as String.getBytes("UTF-8") */
        size_t n = s ? strlen(s) : 0;
        jarray b = fj_array_new("B", (jsize)n);
        if (n) memcpy(fj_array_data(b), s, n);
        return fj_l(b);
    }
    if (is_ref(w) && is_ref(h)) return v;
    if (w == 'J' && is_intlike(h)) return fj_j(h == 'Z' ? v.z : h == 'B' ? v.b : h == 'C' ? v.c : h == 'S' ? v.s : v.i);
    if (is_intlike(w) && h == 'J') return w == 'Z' ? fj_z(v.j != 0) : fj_i((jint)v.j);
    if (is_intlike(w) && is_intlike(h)) {
        jint x = h == 'Z' ? v.z : h == 'B' ? v.b : h == 'C' ? v.c : h == 'S' ? v.s : v.i;
        return w == 'Z' ? fj_z(x != 0) : fj_i(x);
    }
    if (w == 'F' && h == 'D') return fj_f((jfloat)v.d);
    if (w == 'D' && h == 'F') return fj_d(v.f);
    return fj_v();
}

jvalue up_call_native(const char *cls, const char *name, const char *sig, jobject self, const jvalue *args)
{
    jvalue r = fj_v();
    const char *reg = fj_native_sig(cls, name);
    if (!reg) {
        fj_call_native_a(cls, name, sig, self, args);   /* logs "not registered" */
        return r;
    }
    if (!sig || !strcmp(sig, reg))
        return fj_call_native_a(cls, name, reg, self, args);
    enum { NP = 32 };
    const char *wp[NP], *hp[NP];
    int wl[NP], hl[NP];
    int nw = sig_params(reg, wp, wl, NP), nh = sig_params(sig, hp, hl, NP);
    jvalue a[NP];
    for (int i = 0; i < nw; i++)
        a[i] = i < nh && args ? adapt_arg(wp[i], wl[i], hp[i], hl[i], args[i]) : fj_v();
    /* say it once per native */
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static const char *logged[16];
    int seen = 0, i;
    pthread_mutex_lock(&mu);
    for (i = 0; i < 16 && logged[i] && !seen; i++) seen = logged[i] == reg;
    if (!seen && i < 16) logged[i] = reg;
    pthread_mutex_unlock(&mu);
    if (!seen) up_log("%s.%s registered as %s: adapting the call from %s", cls, name, reg, sig);
    return fj_call_native_a(cls, name, reg, self, a);
}

jvalue up_native(const char *name, const char *sig, const jvalue *args)
{
    fj_push_frame();
    jvalue r = up_call_native(UP_CLS, name, sig, g_player, args);
    fj_pop_frame();
    return r;
}

static int have_native(const char *name)
{
    return fj_native_sig(UP_CLS, name) != NULL;
}

/* ======================================================================================= */
/* GL thread jobs (UnityPlayer.g + executeGLThreadJobs)                                    */
/* ======================================================================================= */

static up_looper L_gl;   /* only used as a queue */

void up_gl_post(void (*fn)(void *), void *arg, jobject runnable)
{
    if (!S.loaded) return;
    if (up_on_main_thread()) {
        up_job j = { NULL, fn, arg, runnable ? fj_global(runnable) : NULL, 0 };
        up_job *h = malloc(sizeof *h);
        *h = j;
        looper_run_job(h);
        return;
    }
    looper_post(&L_gl, fn, arg, runnable, 0);
    looper_wake(&L_main);
}

void up_run_gl_jobs(void)
{
    up_job *j;
    while ((j = looper_take(&L_gl, 0, 0))) looper_run_job(j);
}

/* ======================================================================================= */
/* UnityMain thread                                                                        */
/* ======================================================================================= */

static void focus_check(void)
{
    if (S.m_focus == 3 && S.m_surface) {
        jvalue a = fj_z(JNI_TRUE);
        up_native("nativeFocusChanged", "(Z)V", &a);
        S.m_focus = 1;
    }
}

enum { MSG_PAUSE, MSG_RESUME, MSG_QUIT, MSG_SURFACE_LOST, MSG_SURFACE_ACQUIRED,
       MSG_FOCUS_LOST, MSG_FOCUS_GAINED };

static void main_msg(void *arg)
{
    switch ((int)(intptr_t)arg) {
    case MSG_QUIT: L_main.quit = 1; break;
    case MSG_RESUME: S.m_resumed = 1; break;
    case MSG_PAUSE: S.m_resumed = 0; break;
    case MSG_SURFACE_LOST: S.m_surface = 0; break;
    case MSG_SURFACE_ACQUIRED: S.m_surface = 1; focus_check(); break;
    case MSG_FOCUS_GAINED: S.m_focus = 3; focus_check(); break;
    case MSG_FOCUS_LOST:
        if (S.m_focus == 1) { jvalue a = fj_z(JNI_FALSE); up_native("nativeFocusChanged", "(Z)V", &a); }
        S.m_focus = 2;
        break;
    }
}

static void post_msg(int m) { looper_post(&L_main, main_msg, (void *)(intptr_t)m, NULL, 0); }

static void *unity_main(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "UnityMain");
    JavaVM *vm = fj_vm();
    JNIEnv *env;
    (*vm)->AttachCurrentThread(vm, &env, NULL);
    t_looper = &L_main;
    up_log("UnityMain thread running");
    unsigned long frames = 0;
    while (!L_main.quit) {
        up_job *j;
        while (!L_main.quit && (j = looper_take(&L_main, 0, 0))) looper_run_job(j);
        if (L_main.quit) break;
        /* idle handler */
        up_run_gl_jobs();
        if (S.m_resumed && S.m_surface && !up_is_finishing()) {   /* finishing: wait for PAUSE/QUIT below */
            up_choreographer_tick();
            if (!up_is_finishing()) {
                jvalue r = up_native("nativeRender", "()Z", NULL);
                frames++;
                if (fj_tracing() || frames <= 3) up_log("nativeRender #%lu -> %d", frames, r.z);
                if (!r.z) up_request_finish("nativeRender returned false");
            }
            if (UPC.on_frame) UPC.on_frame(UPC.user);
            continue;   /* NEXT_FRAME */
        }
        /* nothing to render: sleep until a message arrives (or a GL job is posted) */
        j = looper_take(&L_main, 1, 100);
        if (j) looper_run_job(j);
    }
    up_log("UnityMain thread exits after %lu frames", frames);
    (*vm)->DetachCurrentThread(vm);
    return NULL;
}

/* ======================================================================================= */
/* UnityPlayer Java methods (UI side)                                                      */
/* ======================================================================================= */

int up_is_finishing(void) { return S.finishing; }

static void ui_finish_job(void *arg)
{
    (void)arg;
    up_quit();
    exit(0);
}

void up_request_finish(const char *why)
{
    if (S.finishing) return;
    up_log("finish requested: %s", why);
    S.finishing = 1;
    if (UPC.on_quit) UPC.on_quit(UPC.user);
    else looper_post(&L_ui, ui_finish_job, NULL, NULL, 0);
}

static void job_native_resume(void *arg) { (void)arg; up_native("nativeResume", "()V", NULL); }
static void job_soft_input_cancel_close(void *arg)
{
    (void)arg;
    if (S.finishing) return;
    if (have_native("nativeSoftInputCanceled")) up_native("nativeSoftInputCanceled", "()V", NULL);
    if (have_native("nativeSoftInputClosed")) up_native("nativeSoftInputClosed", "()V", NULL);
}

/* UnityPlayer.f() */
static void try_resume(void)
{
    pthread_mutex_lock(&g_state_mu);
    int go = S.loaded && S.focus && S.perm && !S.paused && !S.running;
    if (go) S.running = 1;
    pthread_mutex_unlock(&g_state_mu);
    if (!go) return;
    up_log("resume: nativeResume + RESUME");
    up_gl_post(job_native_resume, NULL, NULL);
    post_msg(MSG_RESUME);
}

struct pause_ctx { sem_t sem; int quit; };

/* nativeDone runs Unity's engine cleanup (CleanupEngine), which the host does not need: the process
   exits right after quitting, and Unity has written its PlayerPrefs in nativePause. In Unity 2017.4
   that cleanup can crash (CleanupAllObjects -> Material::ResetAllCachedMaterialData), so it only runs
   with UA_QUIT_CLEANUP=1. */
static void native_done(void)
{
    if (getenv("UA_QUIT_CLEANUP") && *getenv("UA_QUIT_CLEANUP") == '1') {
        /* ()V up to 2018.4; ()Z from 2019.4: true asks Java to kill the process, which the
           host does anyway after quitting */
        jvalue r = up_native("nativeDone", "()V", NULL);
        const char *sig = fj_native_sig(UP_CLS, "nativeDone");
        if (sig && !strcmp(sig, "()Z")) up_log("nativeDone -> %d (kill process)", r.z);
    }
    else up_log("quit: engine cleanup skipped (UA_QUIT_CLEANUP=1 runs nativeDone)");
}

static void job_pause(void *arg)
{
    struct pause_ctx *c = arg;
    /* also when quitting: nativePause sends OnApplicationPause and saves PlayerPrefs */
    jvalue r = up_native("nativePause", "()Z", NULL);
    if (r.z || S.finishing) {
        if (r.z) up_log("nativePause requested quit");
        S.finishing = 1;
        native_done();
        c->quit = 1;
    }
    sem_post(&c->sem);
}

static int sem_wait_4s(sem_t *s)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 4;
    while (sem_timedwait(s, &ts) != 0) if (errno != EINTR) return 0;
    return 1;
}

void up_pause(void)
{
    if (!g_main_started) return;
    up_log("pause");
    /* reportSoftInputStr(null, 1, true) */
    up_gl_post(job_soft_input_cancel_close, NULL, NULL);
    if (S.running && S.loaded) {
        struct pause_ctx *c = calloc(1, sizeof *c);
        sem_init(&c->sem, 0, 0);
        post_msg(MSG_PAUSE);
        looper_post(&L_main, job_pause, c, NULL, 0);
        if (!sem_wait_4s(&c->sem)) up_log("Timeout while trying to pause the Unity Engine.");
        int q = c->quit;
        /* c is leaked on timeout on purpose (the job may still run) */
        if (!q) { sem_destroy(&c->sem); free(c); }
        S.running = 0;
        S.paused = 1;
        if (q) { up_quit(); return; }
    }
    S.running = 0;
    S.paused = 1;
}

void up_resume(void)
{
    if (!g_main_started) return;
    up_log("resume");
    S.paused = 0;
    try_resume();
    if (have_native("nativeRestartActivityIndicator"))   /* up to 2021.3 */
        up_native("nativeRestartActivityIndicator", "()V", NULL);
}

void up_focus(int has_focus)
{
    if (!g_main_started) return;
    up_log("window focus %d", has_focus);
    S.focus = has_focus;
    post_msg(has_focus ? MSG_FOCUS_GAINED : MSG_FOCUS_LOST);
    try_resume();
}

struct gfx_ctx { sem_t sem; jobject surface; int wait; };

static void job_recreate_gfx(void *arg)
{
    struct gfx_ctx *c = arg;
    jvalue a[2] = { fj_i(0), fj_l(c->surface) };
    up_native("nativeRecreateGfxState", "(ILandroid/view/Surface;)V", a);
    if (c->wait) sem_post(&c->sem);
    else free(c);
}

/* 2018.4+: SurfaceHolder.Callback.surfaceChanged queues nativeSendSurfaceChangedEvent after the
   display update (it runs on UnityMain after nativeRecreateGfxState) */
static void job_surface_changed(void *arg) { (void)arg; up_native("nativeSendSurfaceChangedEvent", "()V", NULL); }

/* surfaceCreated + surfaceChanged: (re)attach the surface */
static void surface_changed(void)
{
    struct gfx_ctx *c = calloc(1, sizeof *c);
    c->surface = g_surface;
    looper_post(&L_main, job_recreate_gfx, c, NULL, 0);
    post_msg(MSG_SURFACE_ACQUIRED);
    if (have_native("nativeSendSurfaceChangedEvent")) looper_post(&L_main, job_surface_changed, NULL, NULL, 0);
}

void up_set_surface(void *native_window)
{
    if (!g_main_started) return;
    if (native_window) {
        fj_set_data(g_surface, native_window, NULL);
        up_log("surface %p -> nativeRecreateGfxState(0, surface)", native_window);
        surface_changed();
    } else {
        up_log("surface destroyed -> nativeRecreateGfxState(0, null)");
        struct gfx_ctx *c = calloc(1, sizeof *c);
        sem_init(&c->sem, 0, 0);
        c->wait = 1;
        post_msg(MSG_SURFACE_LOST);
        looper_post(&L_main, job_recreate_gfx, c, NULL, 0);
        if (!sem_wait_4s(&c->sem)) up_log("Timeout while trying detaching primary window.");
        else { sem_destroy(&c->sem); free(c); }
        fj_set_data(g_surface, NULL, NULL);
    }
}

void up_resize(int width, int height)
{
    if (width <= 0 || height <= 0 || (width == UPC.width && height == UPC.height)) return;
    up_log("resize %dx%d -> %dx%d", UPC.width, UPC.height, width, height);
    UPC.width = width;
    UPC.height = height;
    if (g_main_started && fj_data(g_surface)) surface_changed();
}

static void job_low_memory(void *arg) { (void)arg; up_native("nativeLowMemory", "()V", NULL); }
void up_low_memory(void) { up_gl_post(job_low_memory, NULL, NULL); }

void up_quit(void)
{
    if (!g_main_started || S.quit_done) return;
    S.quit_done = 1;
    up_log("quit");
    S.finishing = 1;
    if (!S.paused) up_pause();
    post_msg(MSG_QUIT);
    looper_wake(&L_main);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 4;
    if (pthread_timedjoin_np(g_main_thread, NULL, &ts) != 0) up_log("UnityMain did not exit within 4 s");
    up_fmod_stop_all();
    if (fj_native("com/unity3d/player/NativeLoader", "unload", "()Z")) {
        jvalue r = fj_call_native("com/unity3d/player/NativeLoader", "unload", "()Z", NULL);
        if (!r.z) up_log("NativeLoader.unload failed");
    }
    S.loaded = 0;
}

int up_running(void) { return g_main_started && !S.finishing && !L_main.quit; }
int up_unity_quit_done(void) { return S.quit_done; }
void up_wait(void) { if (g_main_started) pthread_join(g_main_thread, NULL); }

void up_poll(void)
{
    up_looper *saved = t_looper;
    t_looper = &L_ui;
    up_job *j;
    while ((j = looper_take(&L_ui, 0, 0))) looper_run_job(j);
    t_looper = saved;
}

/* UnityPlayer.UnitySendMessage: the static native takes (String, String, String) up to 2017.4.9
   and (String, String, byte[] UTF-8) from 2017.4.40 */
void up_send_message_j(jstring obj, jstring method, jstring msg)
{
    if (!S.loaded) {
        up_log("Native libraries not loaded - dropping message for %s.%s", fj_cstr(obj), fj_cstr(method));
        return;
    }
    fj_push_frame();
    jvalue a[3] = { fj_l(obj), fj_l(method), fj_l(msg ? msg : fj_string("")) };
    up_call_native(UP_CLS, "nativeUnitySendMessage", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", NULL, a);
    fj_pop_frame();
}

void up_send_message(const char *obj, const char *method, const char *msg)
{
    fj_push_frame();
    up_send_message_j(fj_string(obj), fj_string(method), fj_string(msg ? msg : ""));
    fj_pop_frame();
}

jobject up_activity(void) { return g_activity; }
jobject up_player(void) { return g_player; }
jobject up_surface(void) { return g_surface; }
void *up_surface_window(jobject s) { return fj_data(s); }
void *up_asset_manager(jobject am) { return fj_data(am); }

/* ======================================================================================= */
/* configuration & start                                                                   */
/* ======================================================================================= */

static char *dir_of(const char *p)
{
    char *d = strdup(p ? p : ".");
    char *s = strrchr(d, '/');
    if (s && s != d) *s = 0;
    return d;
}

static char *join2(const char *a, const char *b)
{
    size_t n = strlen(a) + strlen(b) + 2;
    char *r = malloc(n);
    snprintf(r, n, "%s/%s", a, b);
    return r;
}

static void fill_defaults(const struct up_config *in)
{
    UPC = *in;
#define DEF(f, v) if (!UPC.f) UPC.f = v
    DEF(package_name, "com.pmunity.game");
    DEF(apk_path, "/tmp/base.apk");
    DEF(files_dir, "/tmp/files");
    DEF(data_dir, dir_of(UPC.files_dir));
    DEF(native_lib_dir, join2(UPC.data_dir, "lib"));
    DEF(cache_dir, join2(UPC.data_dir, "cache"));
    DEF(external_files_dir, UPC.files_dir);
    DEF(external_cache_dir, UPC.cache_dir);
    DEF(obb_dir, UPC.data_dir);
    DEF(external_storage, UPC.data_dir);
    DEF(version_code, 1);
    DEF(version_name, "1.0");
    DEF(width, 640);
    DEF(height, 480);
    DEF(density_dpi, 160);
    if (UPC.refresh_rate <= 0) UPC.refresh_rate = 60.0f;
    DEF(sdk_int, 22);
    DEF(os_release, "5.1");
    DEF(manufacturer, "Anbernic");
    DEF(model, "RG34XXSP");
    DEF(brand, "anbernic");
    DEF(device, "rg34xxsp");
    DEF(product, "rg34xxsp");
    DEF(hardware, "sun50iw9");
    DEF(fingerprint, "anbernic/rg34xxsp/rg34xxsp:5.1/LMY47D/1:user/release-keys");
    DEF(language, "en");
    DEF(country, "US");
    DEF(android_id, "f00dcafe12345678");
    DEF(cpu_count, 4);
    DEF(total_mem, 1024LL * 1024 * 1024);
    DEF(audio_sample_rate, 48000);
    DEF(audio_frames_per_buffer, 512);
#undef DEF
}

void up_register_classes(const struct up_config *cfg)
{
    static int done;
    if (done) return;
    done = 1;
    fill_defaults(cfg);
    fj_init();
    looper_init(&L_ui, "main");
    looper_init(&L_main, "UnityMain");
    looper_init(&L_gl, "gl-jobs");
    up_mkdirs(UPC.files_dir);
    up_mkdirs(UPC.cache_dir);
    up_mkdirs(UPC.external_files_dir);
    /* IL2CPP players copy their data to <files_dir>/il2cpp at startup and stop if they cannot */
    if (access(UPC.files_dir, W_OK) != 0)
        up_log("WARNING: files dir %s is not writable (IL2CPP players extract <files>/il2cpp there)", UPC.files_dir);
    up_android_register();
    up_android_create_objects();
}

int up_start(const struct up_config *cfg)
{
    up_register_classes(cfg);
    JavaVM *vm = fj_vm();
    JNIEnv *env = fj_env();
    (void)env;
    t_looper = &L_ui;

    /* UnityPlayer.<clinit>: loadLibrary("main") */
    if (UPC.libmain_onload) {
        jint v = UPC.libmain_onload(vm, NULL);
        up_log("libmain JNI_OnLoad -> 0x%x", v);
    }
    /* UnityPlayer(Context): NativeLoader.load(applicationInfo.nativeLibraryDir) */
    if (fj_native("com/unity3d/player/NativeLoader", "load", "(Ljava/lang/String;)Z")) {
        fj_push_frame();
        jvalue r = fj_call_native("com/unity3d/player/NativeLoader", "load", "(Ljava/lang/String;)Z", NULL,
                                  fj_string(UPC.native_lib_dir));
        fj_pop_frame();
        up_log("NativeLoader.load(%s) -> %d", UPC.native_lib_dir, r.z);
        S.loaded = r.z;
    }
    if (!S.loaded && UPC.libunity_onload) {
        jint v = UPC.libunity_onload(vm, NULL);
        up_log("libunity JNI_OnLoad -> 0x%x", v);
        S.loaded = v > 0;
    }
    if (!S.loaded) {
        up_log("Failure to initialize! (native libraries not loaded)");
        return -1;
    }
    if (!have_native("initJni")) {
        up_log("UnityPlayer.initJni not registered by libunity");
        return -2;
    }

    /* initJni(context) on the UI thread */
    jvalue a = fj_l(g_activity);
    up_native("initJni", "(Landroid/content/Context;)V", &a);
    /* SurfaceView + window flags happen in up_android_create_objects; then: */
    if (have_native("nativeInitWebRequest")) {   /* up to 2018.4 */
        jvalue c = fj_l(fj_class_find("com/unity3d/player/UnityWebRequest"));
        up_native("nativeInitWebRequest", "(Ljava/lang/Class;)V", &c);
    }
    /* 2020.3+: OrientationLockListener reports the system auto-rotate setting when it is
       created: off (a handheld without an accelerometer) */
    if (fj_native_sig("com/unity3d/player/OrientationLockListener", "nativeUpdateOrientationLockState")) {
        jvalue l = fj_i(0);
        fj_push_frame();
        up_call_native("com/unity3d/player/OrientationLockListener", "nativeUpdateOrientationLockState", "(I)V", NULL, &l);
        fj_pop_frame();
    }
    /* 2022.3: Java asks whether the engine wants to run without an Android window (batch
       mode); the surface is provided either way */
    if (have_native("nativeGetNoWindowMode")) {
        jvalue nw = up_native("nativeGetNoWindowMode", "()Z", NULL);
        up_log("nativeGetNoWindowMode -> %d", nw.z);
    }

    /* UnityMain thread */
    if (pthread_create(&g_main_thread, NULL, unity_main, NULL) != 0) return -3;
    g_main_started = 1;

    /* Activity.onStart (nothing) / onResume */
    up_resume();
    /* surfaceCreated */
    if (UPC.native_window) up_set_surface(UPC.native_window);
    /* onWindowFocusChanged(true) */
    if (!UPC.no_focus_on_start) up_focus(1);
    up_poll();
    return 0;
}

/* ======================================================================================= */
/* input                                                                                   */
/* ======================================================================================= */

#define MAX_DEV 16
static struct up_input_device g_dev[MAX_DEV];
static char *g_dev_strs[MAX_DEV][2];
static int g_ndev;
static pthread_mutex_t g_dev_mu = PTHREAD_MUTEX_INITIALIZER;

int up_add_input_device(const struct up_input_device *d)
{
    if (!d || d->id <= 0) return -1;
    pthread_mutex_lock(&g_dev_mu);
    int i;
    for (i = 0; i < g_ndev; i++) if (g_dev[i].id == d->id) break;
    if (i == MAX_DEV) { pthread_mutex_unlock(&g_dev_mu); return -1; }
    if (i == g_ndev) g_ndev++;
    free(g_dev_strs[i][0]); free(g_dev_strs[i][1]);
    g_dev[i] = *d;
    g_dev_strs[i][0] = strdup(d->name ? d->name : "Gamepad");
    g_dev_strs[i][1] = strdup(d->descriptor ? d->descriptor : g_dev_strs[i][0]);
    g_dev[i].name = g_dev_strs[i][0];
    g_dev[i].descriptor = g_dev_strs[i][1];
    if (g_dev[i].naxes > UP_MAX_AXES) g_dev[i].naxes = UP_MAX_AXES;
    pthread_mutex_unlock(&g_dev_mu);
    up_log("input device %d \"%s\" sources 0x%x axes %d", d->id, g_dev[i].name, d->sources, g_dev[i].naxes);
    if (g_main_started) up_notify_input_device(d->id, 1);
    return 0;
}

void up_remove_input_device(int id)
{
    pthread_mutex_lock(&g_dev_mu);
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].id == id) {
            free(g_dev_strs[i][0]); free(g_dev_strs[i][1]);
            g_dev[i] = g_dev[g_ndev - 1];
            g_dev_strs[i][0] = g_dev_strs[g_ndev - 1][0];
            g_dev_strs[i][1] = g_dev_strs[g_ndev - 1][1];
            g_dev_strs[g_ndev - 1][0] = g_dev_strs[g_ndev - 1][1] = NULL;
            g_ndev--;
            break;
        }
    pthread_mutex_unlock(&g_dev_mu);
    if (g_main_started) up_notify_input_device(id, 0);
}

int up_input_device_ids(int *ids, int max)
{
    pthread_mutex_lock(&g_dev_mu);
    int n = 0;
    for (int i = 0; i < g_ndev && n < max; i++) ids[n++] = g_dev[i].id;
    pthread_mutex_unlock(&g_dev_mu);
    return n;
}

int up_input_device_get(int id, struct up_input_device *out)
{
    int ok = 0;
    pthread_mutex_lock(&g_dev_mu);
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].id == id) { *out = g_dev[i]; ok = 1; break; }   /* strings stay owned here */
    pthread_mutex_unlock(&g_dev_mu);
    return ok;
}

/* nativeInjectEvent(InputEvent)Z; 2022.3: (InputEvent, int)Z, Java's injectEvent(event) passes 0 */
static int inject(jobject ev)
{
    if (!S.loaded || !g_player) return 0;
    jvalue a = fj_l(ev);
    jvalue r = up_call_native(UP_CLS, "nativeInjectEvent", "(Landroid/view/InputEvent;)Z", g_player, &a);
    return r.z;
}

static int64_t g_key_down_ms[512];

static int inject_key_flags(int device_id, int source, int action, int keycode, int repeat, int meta, int flags);
int up_inject_key(int device_id, int source, int action, int keycode, int repeat, int meta)
{
    return inject_key_flags(device_id, source, action, keycode, repeat, meta, 0);
}
static int inject_key_flags(int device_id, int source, int action, int keycode, int repeat, int meta, int flags)
{
    up_key_ev *k = calloc(1, sizeof *k);
    k->flags = flags;
    k->action = action;
    k->keycode = keycode;
    k->repeat = repeat;
    k->meta = meta;
    k->device = device_id;
    k->source = source ? source : UP_SOURCE_GAMEPAD;
    k->time_ms = up_now_ms();
    if (keycode >= 0 && keycode < 512) {
        if (action == UP_ACTION_DOWN && !repeat) g_key_down_ms[keycode] = k->time_ms;
        k->down_ms = g_key_down_ms[keycode] ? g_key_down_ms[keycode] : k->time_ms;
    }
    fj_push_frame();
    jobject ev = fj_new_by_name("android/view/KeyEvent", k, free);
    int r = inject(ev);
    fj_pop_frame();
    if (fj_tracing()) up_log("key dev %d code %d action %d -> %d", device_id, keycode, action, r);
    return r;
}

static int64_t g_motion_down_ms;

static up_motion_ev *motion_new(int action, int source, int device)
{
    up_motion_ev *m = calloc(1, sizeof *m);
    m->action = action;
    m->source = source;
    m->device = device;
    m->time_ms = up_now_ms();
    if ((action & 0xff) == UP_ACTION_DOWN) g_motion_down_ms = m->time_ms;
    m->down_ms = g_motion_down_ms ? g_motion_down_ms : m->time_ms;
    return m;
}

/* Android's ViewRootImpl.SyntheticJoystickHandler: a joystick MotionEvent the app did not handle
 * becomes DPAD key events (KeyEvent.FLAG_FALLBACK), from AXIS_HAT_X/Y or else the X/Y stick past
 * half travel, sent when the direction changes (key repeats are not synthesized here) */
#define FLAG_FALLBACK 0x400
static struct { int device, x, y, xkey, ykey; } g_synth[8];
static int direction(float v) { return v >= 0.5f ? 1 : v <= -0.5f ? -1 : 0; }
static void synthesize_dpad(const up_motion_ev *m)
{
    int i, slot = -1;
    for (i = 0; i < 8; i++) if (g_synth[i].device == m->device) { slot = i; break; }
    if (slot < 0) for (i = 0; i < 8; i++) if (!g_synth[i].device) { slot = i; g_synth[i].device = m->device; break; }
    if (slot < 0) return;
    int x = direction(m->ax[0][UP_AXIS_HAT_X]); if (!x) x = direction(m->ax[0][UP_AXIS_X]);
    int y = direction(m->ax[0][UP_AXIS_HAT_Y]); if (!y) y = direction(m->ax[0][UP_AXIS_Y]);
    if (x != g_synth[slot].x) {
        if (g_synth[slot].xkey) inject_key_flags(m->device, m->source, UP_ACTION_UP, g_synth[slot].xkey, 0, 0, FLAG_FALLBACK);
        g_synth[slot].xkey = x ? (x > 0 ? UP_KEYCODE_DPAD_RIGHT : UP_KEYCODE_DPAD_LEFT) : 0;
        if (x) inject_key_flags(m->device, m->source, UP_ACTION_DOWN, g_synth[slot].xkey, 0, 0, FLAG_FALLBACK);
        g_synth[slot].x = x;
    }
    if (y != g_synth[slot].y) {
        if (g_synth[slot].ykey) inject_key_flags(m->device, m->source, UP_ACTION_UP, g_synth[slot].ykey, 0, 0, FLAG_FALLBACK);
        g_synth[slot].ykey = y ? (y > 0 ? UP_KEYCODE_DPAD_DOWN : UP_KEYCODE_DPAD_UP) : 0;
        if (y) inject_key_flags(m->device, m->source, UP_ACTION_DOWN, g_synth[slot].ykey, 0, 0, FLAG_FALLBACK);
        g_synth[slot].y = y;
    }
}

int up_inject_axes(int device_id, int source, int n, const int *axes, const float *values)
{
    up_motion_ev *m = motion_new(UP_ACTION_MOVE, source ? source : UP_SOURCE_JOYSTICK, device_id);
    m->n = 1;
    m->ids[0] = 0;
    m->tool[0] = 0;   /* TOOL_TYPE_UNKNOWN */
    for (int i = 0; i < n; i++)
        if (axes[i] >= 0 && axes[i] < UP_NAXIS) m->ax[0][axes[i]] = values[i];
    up_motion_ev copy = *m;
    fj_push_frame();
    jobject ev = fj_new_by_name("android/view/MotionEvent", m, free);
    int r = inject(ev);
    fj_pop_frame();
    static int logged;
    if (!logged++) up_log("joystick MotionEvent handled by Unity: %d", r);
    if (!r && (copy.source & 0x10)) synthesize_dpad(&copy);   /* SOURCE_CLASS_JOYSTICK */
    return r;
}

int up_inject_touch(int action, int n, const int *ids, const float *x, const float *y)
{
    if (n > UP_MAX_POINTERS) n = UP_MAX_POINTERS;
    up_motion_ev *m = motion_new(action, UP_SOURCE_TOUCHSCREEN, 1);
    m->n = n;
    for (int i = 0; i < n; i++) {
        m->ids[i] = ids ? ids[i] : i;
        m->tool[i] = 1;   /* TOOL_TYPE_FINGER */
        m->ax[i][UP_AXIS_X] = x[i];
        m->ax[i][UP_AXIS_Y] = y[i];
        m->ax[i][2] = 1.0f;   /* AXIS_PRESSURE */
        m->ax[i][3] = 0.1f;   /* AXIS_SIZE */
    }
    fj_push_frame();
    jobject ev = fj_new_by_name("android/view/MotionEvent", m, free);
    int r = inject(ev);
    fj_pop_frame();
    return r;
}
