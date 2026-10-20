/*
 * up_input.c - android.view.KeyEvent / MotionEvent / InputDevice / InputManager objects read by
 * libunity's nativeInjectEvent, the Choreographer (Unity's VSYNC timing), and
 * org.fmod.FMODAudioDevice (FMOD's AudioTrack output, replaced by a C thread).
 */
#define _GNU_SOURCE
#include "up_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern jobject up_g_input_mgr;

#define M(c, n, s, f)       fj_method_define(c, n, s, 0, f, NULL)
#define MU(c, n, s, f, u)   fj_method_define(c, n, s, 0, f, (void *)(u))
#define SM(c, n, s, f)      fj_method_define(c, n, s, 1, f, NULL)

/* ---- KeyEvent ---------------------------------------------------------------------------- */
#define KEV(o) ((up_key_ev *)fj_data(o))
static up_key_ev k_empty;
static up_key_ev *kev(jobject o) { up_key_ev *k = KEV(o); return k ? k : &k_empty; }

H(k_init)   /* KeyEvent(int action, int code) */
{
    up_key_ev *k = calloc(1, sizeof *k);
    k->action = a[0].i; k->keycode = a[1].i; k->source = UP_SOURCE_KEYBOARD; k->device = -1;
    k->time_ms = k->down_ms = up_now_ms();
    fj_set_data(self, k, free);
    return fj_v();
}
H(k_action)   { return fj_i(kev(self)->action); }
H(k_keycode)  { return fj_i(kev(self)->keycode); }
H(k_repeat)   { return fj_i(kev(self)->repeat); }
H(k_meta)     { return fj_i(kev(self)->meta); }
H(k_device)   { return fj_i(kev(self)->device); }
H(k_source)   { return fj_i(kev(self)->source); }
H(k_scancode) { return fj_i(kev(self)->scancode); }
H(k_flags)    { return fj_i(kev(self)->flags); }
H(k_time)     { return fj_j(kev(self)->time_ms); }
H(k_downtime) { return fj_j(kev(self)->down_ms); }
H(k_null)     { return fj_l(NULL); }

static int key_unicode(int code, int meta)
{
    int shift = meta & 1;
    if (code >= 29 && code <= 54) return (shift ? 'A' : 'a') + code - 29;   /* KEYCODE_A..Z */
    if (code >= 7 && code <= 16) return '0' + code - 7;                     /* KEYCODE_0..9 */
    switch (code) {
    case 62: return ' ';  case 66: return '\n'; case 61: return '\t';
    case 55: return ',';  case 56: return '.';  case 69: return '-';  case 70: return '=';
    case 76: return '/';  case 74: return ';';  case 75: return '\''; case 71: return '[';
    case 72: return ']';  case 73: return '\\'; case 68: return '`';
    default: return 0;
    }
}
H(k_unicode)  { return fj_i(key_unicode(kev(self)->keycode, kev(self)->meta)); }
H(k_unicodeM) { return fj_i(key_unicode(kev(self)->keycode, a[0].i)); }
H(k_isSystem) { int c = kev(self)->keycode; return fj_z(c == 3 || c == 4 || c == 24 || c == 25 || c == 82); }
H(k_device_obj) { return fj_l(up_make_input_device(kev(self)->device)); }
H(k_ctrl)     { return fj_z((kev(self)->meta & 0x1000) != 0); }   /* META_CTRL_ON */
H(k_shift)    { return fj_z((kev(self)->meta & 0x1) != 0); }      /* META_SHIFT_ON */
H(kcm_load)   { return fj_l(fj_new_by_name("android/view/KeyCharacterMap", NULL, NULL)); }
H(kcm_get)    { return fj_i(key_unicode(a[0].i, a[1].i)); }

/* ---- MotionEvent ------------------------------------------------------------------------- */
static up_motion_ev m_empty;
static up_motion_ev *mev(jobject o) { up_motion_ev *m = fj_data(o); return m ? m : &m_empty; }
static int pidx(up_motion_ev *m, int i) { return (i >= 0 && i < m->n && i < UP_MAX_POINTERS) ? i : 0; }
static float axis(up_motion_ev *m, int ax, int i) { return (ax >= 0 && ax < UP_NAXIS) ? m->ax[pidx(m, i)][ax] : 0.0f; }

H(m_action)        { return fj_i(mev(self)->action); }
H(m_actionMasked)  { return fj_i(mev(self)->action & 0xff); }
H(m_actionIndex)   { return fj_i((mev(self)->action >> 8) & 0xff); }
H(m_source)        { return fj_i(mev(self)->source); }
H(m_device)        { return fj_i(mev(self)->device); }
H(m_time)          { return fj_j(mev(self)->time_ms); }
H(m_downtime)      { return fj_j(mev(self)->down_ms); }
H(m_buttons)       { return fj_i(mev(self)->buttons); }
H(m_meta)          { return fj_i(mev(self)->meta); }
H(m_flags)         { return fj_i(mev(self)->flags); }
H(m_zero)          { return fj_i(0); }
H(m_pcount)        { return fj_i(mev(self)->n); }
H(m_pid)           { up_motion_ev *m = mev(self); return fj_i(m->ids[pidx(m, a[0].i)]); }
H(m_tool)          { up_motion_ev *m = mev(self); return fj_i(m->tool[pidx(m, a[0].i)]); }
H(m_x0)            { return fj_f(axis(mev(self), 0, 0)); }
H(m_y0)            { return fj_f(axis(mev(self), 1, 0)); }
H(m_x)             { return fj_f(axis(mev(self), 0, a[0].i)); }
H(m_y)             { return fj_f(axis(mev(self), 1, a[0].i)); }
H(m_pressure)      { return fj_f(axis(mev(self), 2, a[0].i)); }
H(m_size)          { return fj_f(axis(mev(self), 3, a[0].i)); }
H(m_axis1)         { return fj_f(axis(mev(self), a[0].i, 0)); }
H(m_axis2)         { return fj_f(axis(mev(self), a[0].i, a[1].i)); }
H(m_hist_axis)     { return fj_f(axis(mev(self), a[0].i, a[1].i)); }   /* (axis, pointer, pos) */
H(m_hist_x)        { return fj_f(axis(mev(self), 0, a[0].i)); }        /* (pointer, pos) */
H(m_hist_y)        { return fj_f(axis(mev(self), 1, a[0].i)); }
H(m_hist_time)     { return fj_j(mev(self)->time_ms); }
H(m_device_obj)    { return fj_l(up_make_input_device(mev(self)->device)); }
H(m_axis0)         { return fj_f(axis(mev(self), (int)(intptr_t)ud, 0)); }            /* ud: axis, pointer 0 */
H(m_axisp)         { return fj_f(axis(mev(self), (int)(intptr_t)ud, a[0].i)); }       /* ud: axis, (pointer) */
H(m_obtain)
{
    up_motion_ev *c = malloc(sizeof *c);
    *c = *mev(a[0].l);
    return fj_l(fj_new_by_name("android/view/MotionEvent", c, free));
}
H(m_axisToString)
{
    static const char *names[UP_NAXIS] = {
        "AXIS_X", "AXIS_Y", "AXIS_PRESSURE", "AXIS_SIZE", "AXIS_TOUCH_MAJOR", "AXIS_TOUCH_MINOR",
        "AXIS_TOOL_MAJOR", "AXIS_TOOL_MINOR", "AXIS_ORIENTATION", "AXIS_VSCROLL", "AXIS_HSCROLL",
        "AXIS_Z", "AXIS_RX", "AXIS_RY", "AXIS_RZ", "AXIS_HAT_X", "AXIS_HAT_Y", "AXIS_LTRIGGER",
        "AXIS_RTRIGGER", "AXIS_THROTTLE", "AXIS_RUDDER", "AXIS_WHEEL", "AXIS_GAS", "AXIS_BRAKE",
        "AXIS_DISTANCE", "AXIS_TILT", "AXIS_SCROLL", "AXIS_RELATIVE_X", "AXIS_RELATIVE_Y" };
    int ax = a[0].i;
    char b[32];
    if (ax >= 0 && ax < UP_NAXIS && names[ax]) return fj_l(fj_string(names[ax]));
    if (ax >= 32 && ax < 48) snprintf(b, sizeof b, "AXIS_GENERIC_%d", ax - 31);
    else snprintf(b, sizeof b, "%d", ax);
    return fj_l(fj_string(b));
}

/* ---- InputDevice / MotionRange / InputManager ------------------------------------------ */
typedef struct { struct up_input_device d; char *name, *desc; } dev_data;
static void dev_free(void *p) { dev_data *d = p; free(d->name); free(d->desc); free(d); }
typedef struct { int axis, source; float min, max, flat, fuzz; } range_data;

jobject up_make_input_device(int id)
{
    struct up_input_device d;
    if (!up_input_device_get(id, &d)) return NULL;
    dev_data *x = calloc(1, sizeof *x);
    x->d = d;
    x->name = strdup(d.name ? d.name : "");
    x->desc = strdup(d.descriptor ? d.descriptor : "");
    x->d.name = x->name;
    x->d.descriptor = x->desc;
    return fj_new_by_name("android/view/InputDevice", x, dev_free);
}
static struct up_input_device *DEV(jobject o)
{
    static struct up_input_device none;
    dev_data *x = fj_data(o);
    return x ? &x->d : &none;
}
static jobject make_range(struct up_input_device *d, int i)
{
    range_data *r = calloc(1, sizeof *r);
    r->axis = d->axis[i];
    r->source = (d->sources & UP_SOURCE_JOYSTICK) ? UP_SOURCE_JOYSTICK : d->sources;
    r->min = d->min[i]; r->max = d->max[i]; r->flat = d->flat[i]; r->fuzz = d->fuzz[i];
    if (r->min == 0 && r->max == 0) { r->min = -1; r->max = 1; }
    return fj_new_by_name("android/view/InputDevice$MotionRange", r, free);
}

H(d_getDeviceIds)
{
    int ids[32];
    int n = up_input_device_ids(ids, 32);
    jarray arr = fj_array_new("I", n);
    memcpy(fj_array_data(arr), ids, n * sizeof(int));
    return fj_l(arr);
}
H(d_getDevice)     { return fj_l(up_make_input_device(a[0].i)); }
H(d_id)            { return fj_i(DEV(self)->id); }
H(d_name)          { return fj_l(fj_string(DEV(self)->name)); }
H(d_descriptor)    { return fj_l(fj_string(DEV(self)->descriptor)); }
H(d_sources)       { return fj_i(DEV(self)->sources); }
H(d_vendor)        { return fj_i(DEV(self)->vendor_id); }
H(d_product)       { return fj_i(DEV(self)->product_id); }
H(d_kbtype)        { return fj_i(1); }   /* KEYBOARD_TYPE_NON_ALPHABETIC */
H(d_supportsSrc)   { return fj_z((DEV(self)->sources & a[0].i) == a[0].i); }
H(d_motionRanges)
{
    struct up_input_device *d = DEV(self);
    JNIEnv *e = fj_env();
    jobject list = fj_new_by_name("java/util/ArrayList", NULL, NULL);
    jarray arr = fj_array_new("Ljava/lang/Object;", d->naxes);
    for (int i = 0; i < d->naxes; i++) (*e)->SetObjectArrayElement(e, arr, i, make_range(d, i));
    fj_set_obj(list, "elems", "[Ljava/lang/Object;", arr);
    return fj_l(list);
}
H(d_motionRange)   /* (axis) or (axis, source) */
{
    struct up_input_device *d = DEV(self);
    for (int i = 0; i < d->naxes; i++)
        if (d->axis[i] == a[0].i) return fj_l(make_range(d, i));
    return fj_l(NULL);
}
H(d_hasKeys)
{
    jsize n = fj_array_len(a[0].l);
    jarray r = fj_array_new("Z", n);
    jboolean *z = fj_array_data(r);
    for (jsize i = 0; i < n; i++) z[i] = JNI_TRUE;
    return fj_l(r);
}
#define RNG(o) ((range_data *)fj_data(o))
H(r_axis)    { return fj_i(RNG(self) ? RNG(self)->axis : 0); }
H(r_source)  { return fj_i(RNG(self) ? RNG(self)->source : 0); }
H(r_min)     { return fj_f(RNG(self) ? RNG(self)->min : -1); }
H(r_max)     { return fj_f(RNG(self) ? RNG(self)->max : 1); }
H(r_range)   { return fj_f(RNG(self) ? RNG(self)->max - RNG(self)->min : 2); }
H(r_flat)    { return fj_f(RNG(self) ? RNG(self)->flat : 0); }
H(r_fuzz)    { return fj_f(RNG(self) ? RNG(self)->fuzz : 0); }
H(r_res)     { return fj_f(0); }

#define MAX_LISTENERS 8
static jobject g_dev_listeners[MAX_LISTENERS];
static pthread_mutex_t g_list_mu = PTHREAD_MUTEX_INITIALIZER;

H(im_register)
{
    pthread_mutex_lock(&g_list_mu);
    for (int i = 0; i < MAX_LISTENERS; i++)
        if (!g_dev_listeners[i]) { g_dev_listeners[i] = fj_global(a[0].l); break; }
    pthread_mutex_unlock(&g_list_mu);
    up_log("InputManager.registerInputDeviceListener(%p)", a[0].l);
    return fj_v();
}
H(im_unregister)
{
    pthread_mutex_lock(&g_list_mu);
    for (int i = 0; i < MAX_LISTENERS; i++)
        if (g_dev_listeners[i] == a[0].l) { fj_release(g_dev_listeners[i]); g_dev_listeners[i] = NULL; }
    pthread_mutex_unlock(&g_list_mu);
    return fj_v();
}
H(im_getDevice) { return fj_l(up_make_input_device(a[0].i)); }

struct dev_note { int id, added; };
static void job_notify_dev(void *arg)
{
    struct dev_note *n = arg;
    jobject ls[MAX_LISTENERS];
    pthread_mutex_lock(&g_list_mu);
    for (int i = 0; i < MAX_LISTENERS; i++) ls[i] = g_dev_listeners[i] ? fj_global(g_dev_listeners[i]) : NULL;
    pthread_mutex_unlock(&g_list_mu);
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (!ls[i]) continue;
        fj_call(ls[i], n->added ? "onInputDeviceAdded" : "onInputDeviceRemoved", "(I)V", n->id);
        fj_release(ls[i]);
    }
    free(n);
}
void up_notify_input_device(int id, int added)
{
    struct dev_note *n = malloc(sizeof *n);
    n->id = id;
    n->added = added;
    /* the listeners were registered with a Handler on Unity's thread: deliver on UnityMain */
    looper_post(&L_main, job_notify_dev, n, NULL, 0);
}

/* ---- Choreographer ------------------------------------------------------------------------ */
#define MAX_FRAME_CB 16
static jobject g_choreo;
static struct { jobject cb; int64_t at_ns; } g_frame_cb[MAX_FRAME_CB];
static pthread_mutex_t g_choreo_mu = PTHREAD_MUTEX_INITIALIZER;

H(ch_getInstance) { return fj_l(g_choreo); }
H(ch_post)        /* ud: 1 = postFrameCallbackDelayed(cb, ms) */
{
    pthread_mutex_lock(&g_choreo_mu);
    int i;
    for (i = 0; i < MAX_FRAME_CB; i++) if (!g_frame_cb[i].cb) break;
    if (i < MAX_FRAME_CB) {
        g_frame_cb[i].cb = fj_global(a[0].l);
        g_frame_cb[i].at_ns = up_now_ns() + (ud ? a[1].j * 1000000LL : 0);
    } else up_log("Choreographer: too many frame callbacks");
    pthread_mutex_unlock(&g_choreo_mu);
    return fj_v();
}
H(ch_remove)
{
    pthread_mutex_lock(&g_choreo_mu);
    for (int i = 0; i < MAX_FRAME_CB; i++)
        if (g_frame_cb[i].cb == a[0].l) { fj_release(g_frame_cb[i].cb); g_frame_cb[i].cb = NULL; }
    pthread_mutex_unlock(&g_choreo_mu);
    return fj_v();
}
/* Swappy's Java helpers (Android Frame Pacing, 2019.4+): libunity registers their natives when it
 * loads (and stops if the classes are missing), and Swappy finds them through the activity's class
 * loader. ChoreographerCallback is a Choreographer.FrameCallback that calls
 * nOnChoreographer(cookie, frameTimeNanos) for each frame it posts; SwappyDisplayManager offers
 * no refresh rate choices (the panel's one rate) and reports none. */
#define GSDK "com/google/androidgamesdk/"
H(cc_init)      { fj_set_data(self, (void *)(intptr_t)a[0].j, NULL); return fj_v(); }
H(cc_post)      /* ud: 1 = postFrameCallbackDelayed(ms) */
{
    jvalue b[2] = { fj_l(self), fj_j(ud ? a[0].j : 0) };
    return ch_post(env, g_choreo, b, ud);
}
H(cc_terminate) { jvalue b[1] = { fj_l(self) }; return ch_remove(env, g_choreo, b, NULL); }
H(cc_doFrame)
{
    jvalue b[2] = { fj_j((jlong)(intptr_t)fj_data(self)), fj_j(a[0].j) };
    return up_call_native(GSDK "ChoreographerCallback", "nOnChoreographer", "(JJ)V", self, b);
}
H(sdm_void)     { return fj_v(); }

void up_choreographer_tick(void)
{
    jobject due[MAX_FRAME_CB];
    int n = 0;
    int64_t now = up_now_ns();
    pthread_mutex_lock(&g_choreo_mu);
    for (int i = 0; i < MAX_FRAME_CB; i++)
        if (g_frame_cb[i].cb && g_frame_cb[i].at_ns <= now) { due[n++] = g_frame_cb[i].cb; g_frame_cb[i].cb = NULL; }
    pthread_mutex_unlock(&g_choreo_mu);
    for (int i = 0; i < n; i++) {
        fj_push_frame();
        fj_call(due[i], "doFrame", "(J)V", (jlong)now);
        fj_pop_frame();
        fj_release(due[i]);
    }
}

/* ---- org.fmod.FMODAudioDevice ----------------------------------------------------------- */
typedef struct fmod_dev {
    jobject self;            /* global ref while running */
    pthread_t th;
    volatile int running;
    int started;
    struct fmod_dev *next;
} fmod_dev;
static fmod_dev *g_fmod;
static pthread_mutex_t g_fmod_mu = PTHREAD_MUTEX_INITIALIZER;

static jint fmod_info(jobject self, int what)
{
    jvalue a = fj_i(what);
    return fj_call_native_a("org/fmod/FMODAudioDevice", "fmodGetInfo", "(I)I", self, &a).i;
}

static void *fmod_thread(void *p)
{
    fmod_dev *d = p;
    pthread_setname_np(pthread_self(), "FMODAudioDevice");
    JavaVM *vm = fj_vm();
    JNIEnv *env;
    (*vm)->AttachCurrentThread(vm, &env, NULL);
    int rate = fmod_info(d->self, 0), block = fmod_info(d->self, 1), nblocks = fmod_info(d->self, 2);
    up_log("FMODAudioDevice: rate %d, block %d frames x %d", rate, block, nblocks);
    if (rate <= 0) rate = UPC.audio_sample_rate;
    if (block <= 0) block = 1024;
    size_t bytes = (size_t)block * 2 * 2;   /* 16-bit stereo */
    int16_t *pcm = calloc(1, bytes);
    fj_push_frame();
    jobject buf = fj_global((*env)->NewDirectByteBuffer(env, pcm, (jlong)bytes));
    fj_pop_frame();
    int64_t next = up_now_ns();
    while (d->running) {
        if (fmod_info(d->self, 3) == 1) {
            jvalue a = fj_l(buf);
            fj_push_frame();
            fj_call_native_a("org/fmod/FMODAudioDevice", "fmodProcess", "(Ljava/nio/ByteBuffer;)I", d->self, &a);
            fj_pop_frame();
            if (UPC.on_audio) { UPC.on_audio(UPC.user, pcm, block, rate); continue; }
        }
        /* no sink (or FMOD idle): pace like a blocking AudioTrack */
        next += (int64_t)block * 1000000000LL / rate;
        int64_t now = up_now_ns();
        if (next > now) { struct timespec ts = { (next - now) / 1000000000LL, (next - now) % 1000000000LL }; nanosleep(&ts, NULL); }
        else next = now;
    }
    fj_release(buf);
    free(pcm);
    (*vm)->DetachCurrentThread(vm);
    return NULL;
}
static fmod_dev *fmod_of(jobject self)
{
    fmod_dev *d = fj_data(self);
    if (!d) {
        d = calloc(1, sizeof *d);
        fj_set_data(self, d, NULL);
        pthread_mutex_lock(&g_fmod_mu);
        d->next = g_fmod;
        g_fmod = d;
        pthread_mutex_unlock(&g_fmod_mu);
    }
    return d;
}
static void fmod_stop(fmod_dev *d)
{
    if (!d->started) return;
    d->running = 0;
    pthread_join(d->th, NULL);
    d->started = 0;
    if (d->self) { fj_release(d->self); d->self = NULL; }
}
H(fm_init)    { fmod_of(self); return fj_v(); }
H(fm_start)
{
    fmod_dev *d = fmod_of(self);
    fmod_stop(d);
    if (!fj_native("org/fmod/FMODAudioDevice", "fmodProcess", NULL)) { up_log("FMODAudioDevice.start: natives not registered"); return fj_v(); }
    d->self = fj_global(self);
    d->running = 1;
    d->started = 1;
    pthread_create(&d->th, NULL, fmod_thread, d);
    up_log("FMODAudioDevice.start");
    return fj_v();
}
H(fm_stop)    { fmod_stop(fmod_of(self)); return fj_v(); }
H(fm_running) { fmod_dev *d = fmod_of(self); return fj_z(d->started && d->running); }
H(fm_record)  { return fj_i(-1); }
void up_fmod_stop_all(void)
{
    pthread_mutex_lock(&g_fmod_mu);
    for (fmod_dev *d = g_fmod; d; d = d->next) fmod_stop(d);
    pthread_mutex_unlock(&g_fmod_mu);
}

/* ---- registration ------------------------------------------------------------------------- */
void up_input_register(void)
{
    fj_class_define("android/view/InputEvent", NULL);
    fj_class_define("android/view/KeyEvent", "android/view/InputEvent");
    fj_class_define("android/view/MotionEvent", "android/view/InputEvent");
    fj_class_define("android/view/InputDevice", NULL);
    fj_class_define("android/view/InputDevice$MotionRange", NULL);
    fj_class_define("android/view/KeyCharacterMap", NULL);
    fj_class_define("android/hardware/input/InputManager", NULL);
    fj_class_define("android/view/Choreographer", NULL);
    fj_class_define(GSDK "ChoreographerCallback", NULL);
    fj_class_implements(GSDK "ChoreographerCallback", "android/view/Choreographer$FrameCallback");
    fj_class_define(GSDK "SwappyDisplayManager", NULL);
    fj_class_define("org/fmod/FMODAudioDevice", NULL);
    fj_class_implements("org/fmod/FMODAudioDevice", "java/lang/Runnable");

    const char *K = "android/view/KeyEvent";
    fj_static_int(K, "KEYCODE_VOLUME_DOWN", 25);
    fj_static_int(K, "KEYCODE_VOLUME_UP", 24);
    fj_static_int(K, "KEYCODE_ZOOM_IN", 168);
    fj_static_int(K, "KEYCODE_ZOOM_OUT", 169);
    fj_static_int(K, "KEYCODE_BACK", 4);
    fj_static_int(K, "KEYCODE_MENU", 82);
    fj_static_int(K, "ACTION_DOWN", 0);
    fj_static_int(K, "ACTION_UP", 1);
    fj_static_int(K, "ACTION_MULTIPLE", 2);
    M(K, "<init>", "(II)V", k_init);
    M(K, "getAction", "()I", k_action);
    M(K, "getKeyCode", "()I", k_keycode);
    M(K, "getRepeatCount", "()I", k_repeat);
    M(K, "getMetaState", "()I", k_meta);
    M(K, "getModifiers", "()I", k_meta);
    M(K, "getDeviceId", "()I", k_device);
    M(K, "getSource", "()I", k_source);
    M(K, "getScanCode", "()I", k_scancode);
    M(K, "getFlags", "()I", k_flags);
    M(K, "getEventTime", "()J", k_time);
    M(K, "getDownTime", "()J", k_downtime);
    M(K, "getCharacters", "()Ljava/lang/String;", k_null);
    M(K, "getUnicodeChar", "()I", k_unicode);
    M(K, "getUnicodeChar", "(I)I", k_unicodeM);
    M(K, "isSystem", "()Z", k_isSystem);
    M(K, "getDevice", "()Landroid/view/InputDevice;", k_device_obj);
    M(K, "isCtrlPressed", "()Z", k_ctrl);
    M(K, "isShiftPressed", "()Z", k_shift);
    fj_method_define("android/view/KeyCharacterMap", "load", "(I)Landroid/view/KeyCharacterMap;", 1, kcm_load, NULL);
    M("android/view/KeyCharacterMap", "get", "(II)I", kcm_get);

    const char *ME = "android/view/MotionEvent";
    fj_static_int(ME, "FLAG_WINDOW_IS_OBSCURED", 1);
    fj_static_int(ME, "ACTION_DOWN", 0);
    fj_static_int(ME, "ACTION_UP", 1);
    fj_static_int(ME, "ACTION_MOVE", 2);
    fj_static_int(ME, "ACTION_CANCEL", 3);
    M(ME, "getAction", "()I", m_action);
    M(ME, "getActionMasked", "()I", m_actionMasked);
    M(ME, "getActionIndex", "()I", m_actionIndex);
    M(ME, "getSource", "()I", m_source);
    M(ME, "getDeviceId", "()I", m_device);
    M(ME, "getEventTime", "()J", m_time);
    M(ME, "getDownTime", "()J", m_downtime);
    M(ME, "getButtonState", "()I", m_buttons);
    M(ME, "getMetaState", "()I", m_meta);
    M(ME, "getFlags", "()I", m_flags);
    M(ME, "getEdgeFlags", "()I", m_zero);
    M(ME, "getHistorySize", "()I", m_zero);
    M(ME, "getPointerCount", "()I", m_pcount);
    M(ME, "getPointerId", "(I)I", m_pid);
    M(ME, "getToolType", "(I)I", m_tool);
    M(ME, "getX", "()F", m_x0);
    M(ME, "getY", "()F", m_y0);
    M(ME, "getRawX", "()F", m_x0);
    M(ME, "getRawY", "()F", m_y0);
    M(ME, "getX", "(I)F", m_x);
    M(ME, "getY", "(I)F", m_y);
    M(ME, "getPressure", "(I)F", m_pressure);
    M(ME, "getSize", "(I)F", m_size);
    M(ME, "getAxisValue", "(I)F", m_axis1);
    M(ME, "getAxisValue", "(II)F", m_axis2);
    M(ME, "getHistoricalAxisValue", "(III)F", m_hist_axis);
    M(ME, "getHistoricalX", "(II)F", m_hist_x);
    M(ME, "getHistoricalY", "(II)F", m_hist_y);
    M(ME, "getHistoricalEventTime", "(I)J", m_hist_time);
    M(ME, "recycle", "()V", m_zero);
    M(ME, "getDevice", "()Landroid/view/InputDevice;", m_device_obj);
    /* AXIS_PRESSURE 2, AXIS_SIZE 3, AXIS_TOUCH_MAJOR 6, AXIS_TOUCH_MINOR 7, AXIS_ORIENTATION 8;
       the history is empty, so historical values are the current ones (as getHistoricalX) */
    MU(ME, "getPressure", "()F", m_axis0, 2);
    MU(ME, "getOrientation", "()F", m_axis0, 8);
    MU(ME, "getTouchMajor", "(I)F", m_axisp, 6);
    MU(ME, "getTouchMinor", "(I)F", m_axisp, 7);
    MU(ME, "getHistoricalPressure", "(II)F", m_axisp, 2);
    MU(ME, "getHistoricalSize", "(II)F", m_axisp, 3);
    MU(ME, "getHistoricalTouchMajor", "(II)F", m_axisp, 6);
    MU(ME, "getHistoricalTouchMinor", "(II)F", m_axisp, 7);
    M("android/view/InputEvent", "getDevice", "()Landroid/view/InputDevice;", NULL);   /* KeyEvent/MotionEvent */
    SM(ME, "obtain", "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;", m_obtain);
    SM(ME, "axisToString", "(I)Ljava/lang/String;", m_axisToString);

    const char *D = "android/view/InputDevice";
    fj_static_int(D, "SOURCE_KEYBOARD", UP_SOURCE_KEYBOARD);
    fj_static_int(D, "SOURCE_DPAD", UP_SOURCE_DPAD);
    fj_static_int(D, "SOURCE_GAMEPAD", UP_SOURCE_GAMEPAD);
    fj_static_int(D, "SOURCE_TOUCHSCREEN", UP_SOURCE_TOUCHSCREEN);
    fj_static_int(D, "SOURCE_MOUSE", UP_SOURCE_MOUSE);
    fj_static_int(D, "SOURCE_JOYSTICK", UP_SOURCE_JOYSTICK);
    fj_static_int(D, "SOURCE_CLASS_JOYSTICK", 16);
    fj_static_int(D, "SOURCE_CLASS_BUTTON", 1);
    fj_static_int(D, "SOURCE_CLASS_POINTER", 2);
    SM(D, "getDeviceIds", "()[I", d_getDeviceIds);
    SM(D, "getDevice", "(I)Landroid/view/InputDevice;", d_getDevice);
    M(D, "getId", "()I", d_id);
    M(D, "getName", "()Ljava/lang/String;", d_name);
    M(D, "getDescriptor", "()Ljava/lang/String;", d_descriptor);
    M(D, "getSources", "()I", d_sources);
    M(D, "getVendorId", "()I", d_vendor);
    M(D, "getProductId", "()I", d_product);
    M(D, "getKeyboardType", "()I", d_kbtype);
    M(D, "isVirtual", "()Z", m_zero);
    M(D, "isExternal", "()Z", d_kbtype);
    M(D, "getControllerNumber", "()I", d_kbtype);
    M(D, "supportsSource", "(I)Z", d_supportsSrc);
    M(D, "getMotionRanges", "()Ljava/util/List;", d_motionRanges);
    M(D, "getMotionRange", "(I)Landroid/view/InputDevice$MotionRange;", d_motionRange);
    M(D, "getMotionRange", "(II)Landroid/view/InputDevice$MotionRange;", d_motionRange);
    M(D, "hasKeys", "([I)[Z", d_hasKeys);
    M(D, "getKeyCharacterMap", "()Landroid/view/KeyCharacterMap;", kcm_load);
    const char *R = "android/view/InputDevice$MotionRange";
    M(R, "getAxis", "()I", r_axis);
    M(R, "getSource", "()I", r_source);
    M(R, "getMin", "()F", r_min);
    M(R, "getMax", "()F", r_max);
    M(R, "getRange", "()F", r_range);
    M(R, "getFlat", "()F", r_flat);
    M(R, "getFuzz", "()F", r_fuzz);
    M(R, "getResolution", "()F", r_res);

    const char *IM = "android/hardware/input/InputManager";
    M(IM, "registerInputDeviceListener", "(Landroid/hardware/input/InputManager$InputDeviceListener;Landroid/os/Handler;)V", im_register);
    M(IM, "unregisterInputDeviceListener", "(Landroid/hardware/input/InputManager$InputDeviceListener;)V", im_unregister);
    M(IM, "getInputDeviceIds", "()[I", d_getDeviceIds);
    M(IM, "getInputDevice", "(I)Landroid/view/InputDevice;", im_getDevice);

    const char *CH = "android/view/Choreographer";
    g_choreo = fj_global(fj_new_by_name(CH, NULL, NULL));
    SM(CH, "getInstance", "()Landroid/view/Choreographer;", ch_getInstance);
    M(CH, "postFrameCallback", "(Landroid/view/Choreographer$FrameCallback;)V", ch_post);
    MU(CH, "postFrameCallbackDelayed", "(Landroid/view/Choreographer$FrameCallback;J)V", ch_post, 1);
    M(CH, "removeFrameCallback", "(Landroid/view/Choreographer$FrameCallback;)V", ch_remove);
    const char *CC = GSDK "ChoreographerCallback", *SDM = GSDK "SwappyDisplayManager";
    M(CC, "<init>", "(J)V", cc_init);
    M(CC, "postFrameCallback", "()V", cc_post);
    MU(CC, "postFrameCallbackDelayed", "(J)V", cc_post, 1);
    M(CC, "terminate", "()V", cc_terminate);
    M(CC, "doFrame", "(J)V", cc_doFrame);
    M(SDM, "<init>", "(JLandroid/app/Activity;)V", sdm_void);
    M(SDM, "setPreferredDisplayModeId", "(I)V", sdm_void);
    M(SDM, "terminate", "()V", sdm_void);

    const char *FM = "org/fmod/FMODAudioDevice";
    M(FM, "<init>", "()V", fm_init);
    M(FM, "start", "()V", fm_start);
    M(FM, "stop", "()V", fm_stop);
    M(FM, "close", "()V", fm_stop);
    M(FM, "isRunning", "()Z", fm_running);
    M(FM, "startAudioRecord", "(III)I", fm_record);
    M(FM, "stopAudioRecord", "()V", fm_stop);
}
