/*
 * up_media.c - android.media's video path as Unity's VideoPlayer uses it (players 2017.4 - 2021.3
 * decode through Java; 2022.3 through the NDK, ../android/media_ndk.c, with the SurfaceTexture
 * from here): MediaExtractor, MediaFormat, MediaCodec (+ BufferInfo), SurfaceTexture (+ its
 * OnFrameAvailableListener), Surface(SurfaceTexture), ParcelFileDescriptor and FileDescriptor.
 * The work is done by the media core, ../media (libuamedia.a: demuxers, decoders, the
 * SurfaceTexture's frame queue and GL upload); these classes give it Android's Java behaviour:
 * the same exceptions (IOException from setDataSource / createDecoderByType, IllegalStateException
 * from calls in the wrong state, NullPointerException / ClassCastException from MediaFormat's
 * typed getters), direct ByteBuffers over the codec's buffer memory, BufferInfo fields, and
 * onFrameAvailable delivered on the SurfaceTexture's looper thread (the thread that created it,
 * or the handler's), as Android does.
 *
 * What Unity does with them (libunity 2017.4, AndroidVideoMedia<AndroidMediaJNI>):
 *  - opens the clip itself (inside the APK/OBB: an fd with the entry's offset and length) and
 *    passes it as ParcelFileDescriptor.adoptFd(fd).getFileDescriptor() to
 *    MediaExtractor.setDataSource(FileDescriptor, long, long), then detachFd() and closes its fd
 *    (the extractor keeps a dup, as Android's);
 *  - reads the tracks' formats (mime, width/height, durationUs, frame-rate, rotation-degrees,
 *    channel-count, sample-rate, language), selects the video track and the audio tracks;
 *  - creates a decoder per track (createDecoderByType), configures the video one with a
 *    Surface(SurfaceTexture(texName)) whose texture it made as GL_TEXTURE_EXTERNAL_OES, takes
 *    getInputBuffers/getOutputBuffers, feeds readSampleData into dequeued input buffers;
 *  - video: dequeueOutputBuffer (INFO_OUTPUT_FORMAT_CHANGED -> getOutputFormat: width, height,
 *    crop-*), releaseOutputBuffer(i, true) for the frame to show, waits for onFrameAvailable,
 *    then on its render thread updateTexImage + getTransformMatrix (scale and offset from it)
 *    and draws the texture with Hidden/VideoDecodeAndroid's RGBAExternal_To_RGBA pass;
 *  - audio: 16-bit PCM from the output buffers (rate and channels from the output format).
 */
#define _GNU_SOURCE
#include "up_internal.h"
#include "../media/um.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define M(c, n, s, f)       fj_method_define(c, n, s, 0, f, NULL)
#define MU(c, n, s, f, u)   fj_method_define(c, n, s, 0, f, (void *)(u))
#define SM(c, n, s, f)      fj_method_define(c, n, s, 1, f, NULL)
#define CLS(c, s)           fj_class_define(c, s)

#define C_FD   "java/io/FileDescriptor"
#define C_PFD  "android/os/ParcelFileDescriptor"
#define C_EX   "android/media/MediaExtractor"
#define C_FMT  "android/media/MediaFormat"
#define C_MC   "android/media/MediaCodec"
#define C_BI   "android/media/MediaCodec$BufferInfo"
#define C_ST   "android/graphics/SurfaceTexture"
#define C_STL  "android/graphics/SurfaceTexture$OnFrameAvailableListener"
#define C_SURF "android/view/Surface"

static const char *jstr(jvalue v) { const char *s = fj_cstr(v.l); return s; }

/* ---- FileDescriptor / ParcelFileDescriptor ------------------------------------------------- */
static jobject make_fd(int fd)
{
    jobject o = fj_new_by_name(C_FD, NULL, NULL);
    fj_set_int(o, "descriptor", fd);
    return o;
}
static int fd_of(jobject fdobj) { return fdobj ? fj_get_field(fdobj, "descriptor", "I").i : -1; }

H(fd_init)    { fj_set_int(self, "descriptor", -1); return fj_v(); }
H(fd_valid)   { return fj_z(fd_of(self) >= 0); }
H(pfd_adopt)  { jobject p = fj_new_by_name(C_PFD, NULL, NULL); fj_set_int(p, "mFd", a[0].i); return fj_l(p); }
H(pfd_dup)
{
    int fd = dup(fd_of(a[0].l));
    if (fd < 0) { fj_throw(env, "java/io/IOException", "dup failed"); return fj_l(NULL); }
    jobject p = fj_new_by_name(C_PFD, NULL, NULL);
    fj_set_int(p, "mFd", fd);
    return fj_l(p);
}
H(pfd_getfd)  { return fj_i(fj_get_field(self, "mFd", "I").i); }
H(pfd_getFileDescriptor) { return fj_l(make_fd(fj_get_field(self, "mFd", "I").i)); }
H(pfd_detach)
{
    int fd = fj_get_field(self, "mFd", "I").i;
    fj_set_int(self, "mFd", -1);
    return fj_i(fd);
}
H(pfd_close)
{
    int fd = fj_get_field(self, "mFd", "I").i;
    if (fd >= 0) close(fd);
    fj_set_int(self, "mFd", -1);
    return fj_v();
}

/* ---- MediaFormat ----------------------------------------------------------------------------- */
static um_format *fmt_of(jobject o) { return o ? fj_data(o) : NULL; }
static void fmt_free(void *p) { um_format_free(p); }
static jobject make_format(um_format *f)  /* takes f */
{
    return fj_new_by_name(C_FMT, f ? f : um_format_new(), fmt_free);
}
H(fmt_init) { fj_set_data(self, um_format_new(), fmt_free); return fj_v(); }
H(fmt_create)    /* ud: 0 = createVideoFormat(mime, width, height), 1 = createAudioFormat(mime, rate, channels) */
{
    um_format *f = um_format_new();
    um_format_set_string(f, UM_KEY_MIME, jstr(a[0]));
    um_format_set_int32(f, ud ? UM_KEY_SAMPLE_RATE : UM_KEY_WIDTH, a[1].i);
    um_format_set_int32(f, ud ? UM_KEY_CHANNEL_COUNT : UM_KEY_HEIGHT, a[2].i);
    return fj_l(make_format(f));
}
H(fmt_contains) { return fj_z(um_format_type(fmt_of(self), jstr(a[0])) != UM_NONE); }
/* Java: ((Integer) map.get(name)).intValue(): NullPointerException when absent, ClassCastException
   for another type; the (name, default) forms (API 29) return the default when absent */
static int fmt_typed(JNIEnv *env, jobject self, const char *key, int want, int has_default)
{
    int t = um_format_type(fmt_of(self), key);
    if (t == want) return 1;
    if (t == UM_NONE) {
        if (!has_default) fj_throw(env, "java/lang/NullPointerException", NULL);
        return 0;
    }
    fj_throw(env, "java/lang/ClassCastException", "MediaFormat value of another type");
    return 0;
}
H(fmt_getInteger)
{
    int32_t v = ud ? a[1].i : 0;
    if (fmt_typed(env, self, jstr(a[0]), UM_INT32, ud != NULL)) um_format_get_int32(fmt_of(self), jstr(a[0]), &v);
    return fj_i(v);
}
H(fmt_getLong)
{
    int64_t v = ud ? a[1].j : 0;
    if (fmt_typed(env, self, jstr(a[0]), UM_INT64, ud != NULL)) um_format_get_int64(fmt_of(self), jstr(a[0]), &v);
    return fj_j(v);
}
H(fmt_getFloat)
{
    float v = ud ? a[1].f : 0;
    if (fmt_typed(env, self, jstr(a[0]), UM_FLOAT, ud != NULL)) um_format_get_float(fmt_of(self), jstr(a[0]), &v);
    return fj_f(v);
}
H(fmt_getString)
{
    const char *k = jstr(a[0]);
    int t = um_format_type(fmt_of(self), k);
    if (t == UM_NONE) return fj_l(ud ? a[1].l : NULL);
    if (t != UM_STRING) { fj_throw(env, "java/lang/ClassCastException", "MediaFormat value of another type"); return fj_l(NULL); }
    return fj_l(fj_string(um_format_get_string(fmt_of(self), k)));
}
H(fmt_getByteBuffer)
{
    size_t n = 0;
    const void *p = um_format_get_buffer(fmt_of(self), jstr(a[0]), &n);
    if (!p) return fj_l(NULL);
    void *copy = malloc(n ? n : 1);
    if (!copy) return fj_l(NULL);
    memcpy(copy, p, n);
    jobject b = fj_new_by_name("java/nio/ByteBuffer", copy, free);
    fj_set_field(b, "capacity", "J", fj_j(n));
    return fj_l(b);
}
H(fmt_setInteger) { um_format_set_int32(fmt_of(self), jstr(a[0]), a[1].i); return fj_v(); }
H(fmt_setLong)    { um_format_set_int64(fmt_of(self), jstr(a[0]), a[1].j); return fj_v(); }
H(fmt_setFloat)   { um_format_set_float(fmt_of(self), jstr(a[0]), a[1].f); return fj_v(); }
H(fmt_setString)  { um_format_set_string(fmt_of(self), jstr(a[0]), jstr(a[1])); return fj_v(); }
H(fmt_setByteBuffer)
{
    void *p = a[1].l ? (*env)->GetDirectBufferAddress(env, a[1].l) : NULL;
    jlong n = a[1].l ? (*env)->GetDirectBufferCapacity(env, a[1].l) : 0;
    if (p && n > 0) um_format_set_buffer(fmt_of(self), jstr(a[0]), p, (size_t)n);
    return fj_v();
}
H(fmt_remove) { um_format_remove(fmt_of(self), jstr(a[0])); return fj_v(); }
H(fmt_toString)
{
    char *s = um_format_to_string(fmt_of(self));
    jobject r = fj_string(s ? s : "{}");
    free(s);
    return fj_l(r);
}

/* ---- MediaExtractor ---------------------------------------------------------------------------- */
static um_extractor *ex_of(JNIEnv *env, jobject o)
{
    um_extractor *e = fj_data(o);
    if (!e) fj_throw(env, "java/lang/IllegalStateException", "MediaExtractor released");
    return e;
}
static void ex_free(void *p) { um_extractor_free(p); }
H(ex_init) { fj_set_data(self, um_extractor_new(), ex_free); return fj_v(); }
static jvalue ex_result(JNIEnv *env, int r)
{
    if (r != UM_OK) fj_throw(env, "java/io/IOException", "Failed to instantiate extractor.");
    return fj_v();
}
H(ex_setDataSource_path)
{
    um_extractor *e = ex_of(env, self);
    const char *p = jstr(a[0]);
    if (!e) return fj_v();
    if (!p) { fj_throw(env, "java/lang/IllegalArgumentException", NULL); return fj_v(); }
    if (!strncmp(p, "http:", 5) || !strncmp(p, "https:", 6)) {
        up_log("MediaExtractor: %s: no network", p);
        fj_throw(env, "java/io/IOException", "Failed to instantiate extractor.");
        return fj_v();
    }
    return ex_result(env, um_extractor_set_path(e, p));
}
H(ex_setDataSource_fd)   /* ud: 0 = (FileDescriptor, long, long), 1 = (FileDescriptor) */
{
    um_extractor *e = ex_of(env, self);
    if (!e) return fj_v();
    if (!a[0].l) { fj_throw(env, "java/lang/IllegalArgumentException", "Null parameter detected"); return fj_v(); }
    int fd = fd_of(a[0].l);
    up_log("MediaExtractor.setDataSource(fd %d, %lld, %lld)", fd, ud ? 0LL : (long long)a[1].j, ud ? -1LL : (long long)a[2].j);
    return ex_result(env, um_extractor_set_fd(e, fd, ud ? 0 : a[1].j, ud ? -1 : a[2].j));
}
H(ex_trackCount) { um_extractor *e = ex_of(env, self); return fj_i(e ? um_extractor_track_count(e) : 0); }
H(ex_trackFormat)
{
    um_extractor *e = ex_of(env, self);
    if (!e) return fj_l(NULL);
    um_format *f = um_extractor_track_format(e, a[0].i);
    if (!f) { fj_throw(env, "java/lang/IllegalArgumentException", "track index out of range"); return fj_l(NULL); }
    return fj_l(make_format(f));
}
H(ex_select)    /* ud: 1 select, 0 unselect */
{
    um_extractor *e = ex_of(env, self);
    if (e && um_extractor_select(e, a[0].i, ud != NULL) != UM_OK) fj_throw(env, "java/lang/IllegalArgumentException", NULL);
    return fj_v();
}
H(ex_readSampleData)
{
    um_extractor *e = ex_of(env, self);
    if (!e || !a[0].l) return fj_i(-1);
    uint8_t *p = (*env)->GetDirectBufferAddress(env, a[0].l);
    jlong cap = (*env)->GetDirectBufferCapacity(env, a[0].l);
    int off = a[1].i;
    if (!p || off < 0 || off > cap) { fj_throw(env, "java/lang/IllegalArgumentException", "readSampleData: bad buffer"); return fj_i(-1); }
    ssize_t n = um_extractor_read(e, p + off, (size_t)(cap - off));
    if (n >= 0) { fj_set_int(a[0].l, "position", off); fj_set_int(a[0].l, "limit", off + (int)n); }
    return fj_i((jint)n);
}
H(ex_sampleTime)  { um_extractor *e = ex_of(env, self); return fj_j(e ? um_extractor_sample_time(e) : -1); }
H(ex_sampleTrack) { um_extractor *e = ex_of(env, self); return fj_i(e ? um_extractor_sample_track(e) : -1); }
H(ex_sampleFlags) { um_extractor *e = ex_of(env, self); return fj_i(e ? (jint)um_extractor_sample_flags(e) : 0); }
H(ex_sampleSize)  { um_extractor *e = ex_of(env, self); return fj_j(e ? um_extractor_sample_size(e) : -1); }
H(ex_advance)     { um_extractor *e = ex_of(env, self); return fj_z(e ? um_extractor_advance(e) : 0); }
H(ex_seekTo)      { um_extractor *e = ex_of(env, self); if (e) um_extractor_seek(e, a[0].j, a[1].i); return fj_v(); }
H(ex_cached)      { return fj_j(-1); }
H(ex_release)
{
    um_extractor *e = fj_data(self);
    fj_set_data(self, NULL, NULL);
    um_extractor_free(e);
    return fj_v();
}

/* ---- SurfaceTexture / Surface ---------------------------------------------------------------- */
typedef struct st_data {
    pthread_mutex_t mu;
    int refs;
    um_surface *s;
    jobject self;               /* global ref while not released */
    jobject listener;           /* global ref */
    up_looper *looper;
    int released;
} st_data;

static void st_put(st_data *d)
{
    pthread_mutex_lock(&d->mu);
    int left = --d->refs;
    pthread_mutex_unlock(&d->mu);
    if (left) return;
    um_surface_unref(d->s);
    pthread_mutex_destroy(&d->mu);
    free(d);
}

static void job_frame_available(void *arg)
{
    st_data *d = arg;
    pthread_mutex_lock(&d->mu);
    jobject l = d->released ? NULL : d->listener, st = d->self;
    pthread_mutex_unlock(&d->mu);
    if (l && st) {
        fj_push_frame();
        fj_call(l, "onFrameAvailable", "(L" C_ST ";)V", st);
        JNIEnv *env = fj_env();
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        fj_pop_frame();
    }
    st_put(d);
}

/* the media core queued a frame (on the thread releasing the codec buffer): Android tells the
   listener on the SurfaceTexture's looper */
static void on_frame(void *user, um_surface *s)
{
    (void)s;
    st_data *d = user;
    pthread_mutex_lock(&d->mu);
    int go = !d->released && d->listener;
    if (go) d->refs++;
    up_looper *l = d->looper;
    pthread_mutex_unlock(&d->mu);
    if (go) looper_post(l ? l : &L_ui, job_frame_available, d, NULL, 0);
}

static st_data *st_of(jobject o) { return o ? fj_data(o) : NULL; }
static void st_free_data(void *p) { st_data *d = p; if (d) st_put(d); }

H(st_init)      /* (int texName), (int, boolean singleBufferMode) */
{
    st_data *d = calloc(1, sizeof *d);
    if (!d) return fj_v();
    pthread_mutex_init(&d->mu, NULL);
    d->refs = 1;
    d->s = um_surface_new((unsigned)a[0].i);
    d->looper = t_looper ? t_looper : &L_ui;
    d->self = fj_global(self);          /* until release(): the listener calls get it */
    um_surface_set_listener(d->s, on_frame, d);
    fj_set_data(self, d, st_free_data);
    up_log("SurfaceTexture(%d) on %s", a[0].i, d->looper->name ? d->looper->name : "?");
    return fj_v();
}
H(st_setListener)   /* ud: 0 = (listener), 1 = (listener, Handler) */
{
    st_data *d = st_of(self);
    if (!d) return fj_v();
    jobject nl = a[0].l ? fj_global(a[0].l) : NULL;
    pthread_mutex_lock(&d->mu);
    jobject old = d->listener;
    d->listener = nl;
    if (ud && a[1].l) {
        jobject lp = fj_get_field(a[1].l, "mLooper", "Landroid/os/Looper;").l;
        up_looper *l = lp ? fj_data(lp) : NULL;
        if (l) d->looper = l;
    }
    pthread_mutex_unlock(&d->mu);
    if (old) fj_release(old);
    return fj_v();
}
H(st_update)
{
    st_data *d = st_of(self);
    if (!d || d->released) { fj_throw(env, "java/lang/IllegalStateException", "SurfaceTexture released"); return fj_v(); }
    um_surface_update(d->s);
    return fj_v();
}
H(st_matrix)
{
    st_data *d = st_of(self);
    if (!a[0].l || fj_array_len(a[0].l) < 16) { fj_throw(env, "java/lang/IllegalArgumentException", NULL); return fj_v(); }
    um_surface_matrix(d ? d->s : NULL, (float *)fj_array_data(a[0].l));
    return fj_v();
}
H(st_timestamp) { st_data *d = st_of(self); return fj_j(d ? um_surface_timestamp(d->s) : 0); }
H(st_size)      { st_data *d = st_of(self); if (d) um_surface_set_default_size(d->s, a[0].i, a[1].i); return fj_v(); }
H(st_detach)    { st_data *d = st_of(self); if (d) um_surface_detach_gl(d->s); return fj_v(); }
H(st_released)  { st_data *d = st_of(self); return fj_z(!d || d->released); }
H(st_release)
{
    st_data *d = st_of(self);
    if (!d) return fj_v();
    pthread_mutex_lock(&d->mu);
    jobject l = d->listener, s = d->self;
    int was = d->released;
    d->released = 1;
    d->listener = NULL;
    d->self = NULL;
    pthread_mutex_unlock(&d->mu);
    if (!was) um_surface_set_listener(d->s, NULL, NULL);
    if (l) fj_release(l);
    if (s) fj_release(s);
    return fj_v();
}

/* A Surface made from a SurfaceTexture: its data is an up_video_surface (the window surface's
   data is the host's ANativeWindow) */
#define VSURF_MAGIC 0x56535246u
typedef struct { uint32_t magic; um_surface *s; } up_video_surface;
static void vsurf_free(void *p) { up_video_surface *v = p; if (v) { um_surface_unref(v->s); free(v); } }
H(surf_init_st)
{
    st_data *d = st_of(a[0].l);
    if (!d) { fj_throw(env, "java/lang/IllegalArgumentException", "SurfaceTexture is null"); return fj_v(); }
    up_video_surface *v = calloc(1, sizeof *v);
    if (!v) return fj_v();
    v->magic = VSURF_MAGIC;
    v->s = um_surface_ref(d->s);
    fj_set_data(self, v, vsurf_free);
    return fj_v();
}
static um_surface *surface_of(jobject surf)
{
    up_video_surface *v = surf ? fj_data(surf) : NULL;
    return v && v->magic == VSURF_MAGIC ? v->s : NULL;
}
/* for the NDK side (ANativeWindow_fromSurface on a Surface(SurfaceTexture)) */
void *up_media_surface(jobject surf) { return surface_of(surf); }

/* ---- MediaCodec ---------------------------------------------------------------------------------- */
typedef struct {
    um_codec *c;
    jobject in_arr, out_arr;    /* ByteBuffer[] (global refs), made once per configure */
    uint8_t dummy[16];          /* behind a surface decoder's output ByteBuffers */
    int configured, started;
} mc_data;

static void mc_drop_arrays(mc_data *m)
{
    if (m->in_arr) fj_release(m->in_arr);
    if (m->out_arr) fj_release(m->out_arr);
    m->in_arr = m->out_arr = NULL;
}
static void mc_free(void *p)
{
    mc_data *m = p;
    if (!m) return;
    mc_drop_arrays(m);
    um_codec_free(m->c);
    free(m);
}
static mc_data *mc_of(JNIEnv *env, jobject o)
{
    mc_data *m = fj_data(o);
    if (!m || !m->c) { fj_throw(env, "java/lang/IllegalStateException", "MediaCodec released"); return NULL; }
    return m;
}
static jobject make_codec(um_codec *c)
{
    mc_data *m = calloc(1, sizeof *m);
    if (!m) { um_codec_free(c); return NULL; }
    m->c = c;
    return fj_new_by_name(C_MC, m, mc_free);
}
H(mc_createDecoder)
{
    const char *mime = jstr(a[0]);
    if (!mime) { fj_throw(env, "java/lang/NullPointerException", NULL); return fj_l(NULL); }
    um_codec *c = um_codec_create(mime);
    if (!c) { fj_throw(env, "java/io/IOException", "Failed to allocate component instance"); return fj_l(NULL); }
    return fj_l(make_codec(c));
}
H(mc_createByName)
{
    um_codec *c = um_codec_create_by_name(jstr(a[0]));
    if (!c) { fj_throw(env, "java/io/IOException", "Failed to allocate component instance"); return fj_l(NULL); }
    return fj_l(make_codec(c));
}
H(mc_createEncoder) { fj_throw(env, "java/io/IOException", "no encoders"); return fj_l(NULL); }
H(mc_getName) { mc_data *m = mc_of(env, self); return fj_l(m ? fj_string(um_codec_name(m->c)) : NULL); }
static void state_error(JNIEnv *env, int r)
{
    if (r == UM_ERR_STATE) fj_throw(env, "java/lang/IllegalStateException", NULL);
    else if (r == UM_ERR_INVALID) fj_throw(env, "java/lang/IllegalArgumentException", NULL);
    else if (r != UM_OK) fj_throw(env, "android/media/MediaCodec$CodecException", "codec error");
}
H(mc_configure)   /* (MediaFormat, Surface, MediaCrypto, int flags) */
{
    mc_data *m = mc_of(env, self);
    if (!m) return fj_v();
    if (a[3].i & 1) { fj_throw(env, "java/lang/IllegalStateException", "no encoders"); return fj_v(); }
    um_surface *s = surface_of(a[1].l);
    if (a[1].l && !s) up_log("MediaCodec.configure: a Surface that is not a SurfaceTexture's: decoding to buffers");
    int r = um_codec_configure(m->c, fmt_of(a[0].l), s);
    if (r == UM_OK) { m->configured = 1; mc_drop_arrays(m); }
    else if (r == UM_ERR_STATE) fj_throw(env, "java/lang/IllegalStateException", NULL);
    else fj_throw(env, "android/media/MediaCodec$CodecException", "configure failed");
    return fj_v();
}
H(mc_setOutputSurface)
{
    mc_data *m = mc_of(env, self);
    if (m) state_error(env, um_codec_set_surface(m->c, surface_of(a[0].l)));
    return fj_v();
}
H(mc_start) { mc_data *m = mc_of(env, self); if (m) { int r = um_codec_start(m->c); state_error(env, r); m->started = r == UM_OK; } return fj_v(); }
H(mc_stop)  { mc_data *m = mc_of(env, self); if (m) { um_codec_stop(m->c); m->started = m->configured = 0; } return fj_v(); }
H(mc_flush) { mc_data *m = mc_of(env, self); if (m) state_error(env, um_codec_flush(m->c)); return fj_v(); }
H(mc_release)
{
    mc_data *m = fj_data(self);
    if (m && m->c) { mc_drop_arrays(m); um_codec_free(m->c); m->c = NULL; }
    return fj_v();
}
static jobject buffer_array(JNIEnv *env, mc_data *m, int out)
{
    int n = out ? um_codec_output_count(m->c) : um_codec_input_count(m->c);
    jobjectArray arr = fj_array_new("Ljava/nio/ByteBuffer;", n);
    for (int i = 0; i < n; i++) {
        size_t cap = 0;
        uint8_t *p = out ? um_codec_output_buffer(m->c, i, &cap) : um_codec_input_buffer(m->c, i, &cap);
        if (!p || !cap) { p = m->dummy; cap = sizeof m->dummy; }   /* (Unity refuses empty buffers) */
        jobject b = (*env)->NewDirectByteBuffer(env, p, (jlong)cap);
        (*env)->SetObjectArrayElement(env, arr, i, b);
        (*env)->DeleteLocalRef(env, b);
    }
    return arr;
}
H(mc_getBuffers)   /* ud: 0 input, 1 output */
{
    mc_data *m = mc_of(env, self);
    if (!m) return fj_l(NULL);
    if (!m->started) { fj_throw(env, "java/lang/IllegalStateException", NULL); return fj_l(NULL); }
    jobject *slot = ud ? &m->out_arr : &m->in_arr;
    if (!*slot) *slot = fj_global(buffer_array(env, m, ud != NULL));
    return fj_l(*slot);
}
H(mc_getBuffer)    /* getInputBuffer(I) / getOutputBuffer(I) (API 21): a buffer, null for a surface's */
{
    mc_data *m = mc_of(env, self);
    if (!m) return fj_l(NULL);
    size_t cap = 0;
    uint8_t *p = ud ? um_codec_output_buffer(m->c, a[0].i, &cap) : um_codec_input_buffer(m->c, a[0].i, &cap);
    return fj_l(p ? (*env)->NewDirectByteBuffer(env, p, (jlong)cap) : NULL);
}
H(mc_dequeueInput)
{
    mc_data *m = mc_of(env, self);
    if (!m) return fj_i(-1);
    int r = um_codec_dequeue_input(m->c, a[0].j);
    if (r < -1) { state_error(env, r); return fj_i(-1); }
    return fj_i(r);
}
H(mc_queueInput)   /* (int index, int offset, int size, long presentationTimeUs, int flags) */
{
    mc_data *m = mc_of(env, self);
    if (m) state_error(env, um_codec_queue_input(m->c, a[0].i, a[1].i, a[2].i, a[3].j, (uint32_t)a[4].i));
    return fj_v();
}
H(mc_dequeueOutput)   /* (BufferInfo, long timeoutUs) */
{
    mc_data *m = mc_of(env, self);
    if (!m) return fj_i(-1);
    um_buffer_info bi;
    int r = um_codec_dequeue_output(m->c, &bi, a[1].j);
    if (r < -3) { state_error(env, r); return fj_i(-1); }
    if (r == UM_INFO_OUTPUT_BUFFERS_CHANGED && m->out_arr) { fj_release(m->out_arr); m->out_arr = NULL; }
    if (r >= 0 && a[0].l) {
        fj_set_int(a[0].l, "offset", bi.offset);
        fj_set_int(a[0].l, "size", bi.size);
        fj_set_long(a[0].l, "presentationTimeUs", bi.pts_us);
        fj_set_int(a[0].l, "flags", (jint)bi.flags);
    }
    return fj_i(r);
}
H(mc_releaseOutput)   /* (int, boolean render) / (int, long renderTimestampNs): render */
{
    mc_data *m = mc_of(env, self);
    if (m) state_error(env, um_codec_release_output(m->c, a[0].i, ud ? 1 : a[1].z));
    return fj_v();
}
H(mc_outputFormat)   /* ud: 0 output, 1 input */
{
    mc_data *m = mc_of(env, self);
    if (!m) return fj_l(NULL);
    if (!m->configured) { fj_throw(env, "java/lang/IllegalStateException", NULL); return fj_l(NULL); }
    return fj_l(make_format(ud ? um_codec_input_format(m->c) : um_codec_output_format(m->c)));
}

H(bi_init) { return fj_v(); }
H(bi_set)
{
    fj_set_int(self, "offset", a[0].i);
    fj_set_int(self, "size", a[1].i);
    fj_set_long(self, "presentationTimeUs", a[2].j);
    fj_set_int(self, "flags", a[3].i);
    return fj_v();
}

/* java.nio.Buffer members Unity calls on the codec's buffers */
H(buf_true)  { return fj_z(JNI_TRUE); }
H(buf_rewind) { fj_set_int(self, "position", 0); return fj_l(self); }
H(buf_cap)   { return fj_i((jint)(*env)->GetDirectBufferCapacity(env, self)); }
H(buf_clear) { fj_set_int(self, "position", 0); fj_set_int(self, "limit", (jint)(*env)->GetDirectBufferCapacity(env, self)); return fj_l(self); }
H(buf_position) { return fj_i(fj_get_field(self, "position", "I").i); }
H(buf_limit) { return fj_i(fj_get_field(self, "limit", "I").i); }
H(buf_setpos) { fj_set_int(self, "position", a[0].i); return fj_l(self); }
H(buf_setlim) { fj_set_int(self, "limit", a[0].i); return fj_l(self); }
H(buf_remaining) { return fj_i(fj_get_field(self, "limit", "I").i - fj_get_field(self, "position", "I").i); }

/* ---- registration ---------------------------------------------------------------------------------- */
static const char *const media_keys[][2] = {
    { "KEY_MIME", "mime" }, { "KEY_WIDTH", "width" }, { "KEY_HEIGHT", "height" },
    { "KEY_DURATION", "durationUs" }, { "KEY_FRAME_RATE", "frame-rate" },
    { "KEY_SAMPLE_RATE", "sample-rate" }, { "KEY_CHANNEL_COUNT", "channel-count" },
    { "KEY_CHANNEL_MASK", "channel-mask" }, { "KEY_MAX_INPUT_SIZE", "max-input-size" },
    { "KEY_LANGUAGE", "language" }, { "KEY_ROTATION", "rotation-degrees" },
    { "KEY_COLOR_FORMAT", "color-format" }, { "KEY_COLOR_RANGE", "color-range" },
    { "KEY_COLOR_STANDARD", "color-standard" }, { "KEY_COLOR_TRANSFER", "color-transfer" },
    { "KEY_STRIDE", "stride" }, { "KEY_SLICE_HEIGHT", "slice-height" }, { "KEY_PROFILE", "profile" },
    { "KEY_LEVEL", "level" }, { "KEY_BIT_RATE", "bitrate" }, { "KEY_PCM_ENCODING", "pcm-encoding" },
    { "KEY_IS_ADTS", "is-adts" }, { "KEY_AAC_PROFILE", "aac-profile" }, { "KEY_TRACK_ID", "track-id" },
    { "KEY_MAX_WIDTH", "max-width" }, { "KEY_MAX_HEIGHT", "max-height" },
    { "KEY_I_FRAME_INTERVAL", "i-frame-interval" }, { "KEY_PRIORITY", "priority" },
    { "KEY_LOW_LATENCY", "low-latency" }, { "KEY_OPERATING_RATE", "operating-rate" },
    { "MIMETYPE_VIDEO_VP8", UM_MIME_VP8 }, { "MIMETYPE_VIDEO_VP9", UM_MIME_VP9 },
    { "MIMETYPE_VIDEO_AVC", UM_MIME_AVC }, { "MIMETYPE_VIDEO_HEVC", UM_MIME_HEVC },
    { "MIMETYPE_AUDIO_VORBIS", UM_MIME_VORBIS }, { "MIMETYPE_AUDIO_OPUS", UM_MIME_OPUS },
    { "MIMETYPE_AUDIO_AAC", UM_MIME_AAC }, { "MIMETYPE_AUDIO_MPEG", UM_MIME_MPEG },
    { "MIMETYPE_AUDIO_RAW", UM_MIME_RAW },
};

void up_media_register(void)
{
    CLS(C_FD, NULL);
    CLS(C_PFD, NULL);
    CLS(C_EX, NULL);
    CLS(C_FMT, NULL);
    CLS(C_MC, NULL);
    CLS(C_BI, NULL);
    CLS("android/media/MediaCodec$CodecException", "java/lang/IllegalStateException");
    CLS("android/media/MediaCrypto", NULL);
    CLS(C_ST, NULL);
    CLS(C_STL, NULL);
    CLS(C_SURF, NULL);

    fj_field_define(C_FD, "descriptor", "I", 0, fj_i(-1));
    M(C_FD, "<init>", "()V", fd_init);
    M(C_FD, "valid", "()Z", fd_valid);
    fj_field_define(C_PFD, "mFd", "I", 0, fj_i(-1));
    SM(C_PFD, "adoptFd", "(I)L" C_PFD ";", pfd_adopt);
    SM(C_PFD, "fromFd", "(I)L" C_PFD ";", pfd_adopt);
    SM(C_PFD, "dup", "(L" C_FD ";)L" C_PFD ";", pfd_dup);
    M(C_PFD, "getFd", "()I", pfd_getfd);
    M(C_PFD, "getFileDescriptor", "()L" C_FD ";", pfd_getFileDescriptor);
    M(C_PFD, "detachFd", "()I", pfd_detach);
    M(C_PFD, "close", "()V", pfd_close);

    for (size_t i = 0; i < sizeof media_keys / sizeof media_keys[0]; i++)
        fj_static_string(C_FMT, media_keys[i][0], media_keys[i][1]);
    M(C_FMT, "<init>", "()V", fmt_init);
    fj_method_define(C_FMT, "createVideoFormat", "(Ljava/lang/String;II)L" C_FMT ";", 1, fmt_create, NULL);
    fj_method_define(C_FMT, "createAudioFormat", "(Ljava/lang/String;II)L" C_FMT ";", 1, fmt_create, (void *)1);
    M(C_FMT, "containsKey", "(Ljava/lang/String;)Z", fmt_contains);
    M(C_FMT, "getInteger", "(Ljava/lang/String;)I", fmt_getInteger);
    MU(C_FMT, "getInteger", "(Ljava/lang/String;I)I", fmt_getInteger, 1);
    M(C_FMT, "getLong", "(Ljava/lang/String;)J", fmt_getLong);
    MU(C_FMT, "getLong", "(Ljava/lang/String;J)J", fmt_getLong, 1);
    M(C_FMT, "getFloat", "(Ljava/lang/String;)F", fmt_getFloat);
    MU(C_FMT, "getFloat", "(Ljava/lang/String;F)F", fmt_getFloat, 1);
    M(C_FMT, "getString", "(Ljava/lang/String;)Ljava/lang/String;", fmt_getString);
    MU(C_FMT, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", fmt_getString, 1);
    M(C_FMT, "getByteBuffer", "(Ljava/lang/String;)Ljava/nio/ByteBuffer;", fmt_getByteBuffer);
    M(C_FMT, "setInteger", "(Ljava/lang/String;I)V", fmt_setInteger);
    M(C_FMT, "setLong", "(Ljava/lang/String;J)V", fmt_setLong);
    M(C_FMT, "setFloat", "(Ljava/lang/String;F)V", fmt_setFloat);
    M(C_FMT, "setString", "(Ljava/lang/String;Ljava/lang/String;)V", fmt_setString);
    M(C_FMT, "setByteBuffer", "(Ljava/lang/String;Ljava/nio/ByteBuffer;)V", fmt_setByteBuffer);
    M(C_FMT, "removeKey", "(Ljava/lang/String;)V", fmt_remove);
    M(C_FMT, "toString", "()Ljava/lang/String;", fmt_toString);

    M(C_EX, "<init>", "()V", ex_init);
    M(C_EX, "setDataSource", "(Ljava/lang/String;)V", ex_setDataSource_path);
    M(C_EX, "setDataSource", "(L" C_FD ";JJ)V", ex_setDataSource_fd);
    MU(C_EX, "setDataSource", "(L" C_FD ";)V", ex_setDataSource_fd, 1);
    M(C_EX, "getTrackCount", "()I", ex_trackCount);
    M(C_EX, "getTrackFormat", "(I)L" C_FMT ";", ex_trackFormat);
    MU(C_EX, "selectTrack", "(I)V", ex_select, 1);
    M(C_EX, "unselectTrack", "(I)V", ex_select);
    M(C_EX, "readSampleData", "(Ljava/nio/ByteBuffer;I)I", ex_readSampleData);
    M(C_EX, "getSampleTime", "()J", ex_sampleTime);
    M(C_EX, "getSampleTrackIndex", "()I", ex_sampleTrack);
    M(C_EX, "getSampleFlags", "()I", ex_sampleFlags);
    M(C_EX, "getSampleSize", "()J", ex_sampleSize);
    M(C_EX, "advance", "()Z", ex_advance);
    M(C_EX, "seekTo", "(JI)V", ex_seekTo);
    M(C_EX, "getCachedDuration", "()J", ex_cached);
    M(C_EX, "hasCacheReachedEndOfStream", "()Z", ex_cached);
    M(C_EX, "release", "()V", ex_release);
    fj_static_int(C_EX, "SEEK_TO_PREVIOUS_SYNC", UM_SEEK_PREVIOUS_SYNC);
    fj_static_int(C_EX, "SEEK_TO_NEXT_SYNC", UM_SEEK_NEXT_SYNC);
    fj_static_int(C_EX, "SEEK_TO_CLOSEST_SYNC", UM_SEEK_CLOSEST_SYNC);
    fj_static_int(C_EX, "SAMPLE_FLAG_SYNC", UM_SAMPLE_SYNC);
    fj_static_int(C_EX, "SAMPLE_FLAG_ENCRYPTED", UM_SAMPLE_ENCRYPTED);
    fj_static_int(C_EX, "SAMPLE_FLAG_PARTIAL_FRAME", UM_SAMPLE_PARTIAL);

    SM(C_MC, "createDecoderByType", "(Ljava/lang/String;)L" C_MC ";", mc_createDecoder);
    SM(C_MC, "createByCodecName", "(Ljava/lang/String;)L" C_MC ";", mc_createByName);
    SM(C_MC, "createEncoderByType", "(Ljava/lang/String;)L" C_MC ";", mc_createEncoder);
    M(C_MC, "getName", "()Ljava/lang/String;", mc_getName);
    M(C_MC, "configure", "(L" C_FMT ";L" C_SURF ";Landroid/media/MediaCrypto;I)V", mc_configure);
    M(C_MC, "setOutputSurface", "(L" C_SURF ";)V", mc_setOutputSurface);
    M(C_MC, "start", "()V", mc_start);
    M(C_MC, "stop", "()V", mc_stop);
    M(C_MC, "flush", "()V", mc_flush);
    M(C_MC, "release", "()V", mc_release);
    M(C_MC, "reset", "()V", mc_stop);
    M(C_MC, "getInputBuffers", "()[Ljava/nio/ByteBuffer;", mc_getBuffers);
    MU(C_MC, "getOutputBuffers", "()[Ljava/nio/ByteBuffer;", mc_getBuffers, 1);
    M(C_MC, "getInputBuffer", "(I)Ljava/nio/ByteBuffer;", mc_getBuffer);
    MU(C_MC, "getOutputBuffer", "(I)Ljava/nio/ByteBuffer;", mc_getBuffer, 1);
    M(C_MC, "dequeueInputBuffer", "(J)I", mc_dequeueInput);
    M(C_MC, "queueInputBuffer", "(IIIJI)V", mc_queueInput);
    M(C_MC, "dequeueOutputBuffer", "(L" C_BI ";J)I", mc_dequeueOutput);
    M(C_MC, "releaseOutputBuffer", "(IZ)V", mc_releaseOutput);
    MU(C_MC, "releaseOutputBuffer", "(IJ)V", mc_releaseOutput, 1);
    M(C_MC, "getOutputFormat", "()L" C_FMT ";", mc_outputFormat);
    M(C_MC, "getOutputFormat", "(I)L" C_FMT ";", mc_outputFormat);
    MU(C_MC, "getInputFormat", "()L" C_FMT ";", mc_outputFormat, 1);
    fj_static_int(C_MC, "INFO_TRY_AGAIN_LATER", UM_INFO_TRY_AGAIN_LATER);
    fj_static_int(C_MC, "INFO_OUTPUT_FORMAT_CHANGED", UM_INFO_OUTPUT_FORMAT_CHANGED);
    fj_static_int(C_MC, "INFO_OUTPUT_BUFFERS_CHANGED", UM_INFO_OUTPUT_BUFFERS_CHANGED);
    fj_static_int(C_MC, "BUFFER_FLAG_SYNC_FRAME", UM_BUFFER_FLAG_KEY_FRAME);
    fj_static_int(C_MC, "BUFFER_FLAG_KEY_FRAME", UM_BUFFER_FLAG_KEY_FRAME);
    fj_static_int(C_MC, "BUFFER_FLAG_CODEC_CONFIG", UM_BUFFER_FLAG_CODEC_CONFIG);
    fj_static_int(C_MC, "BUFFER_FLAG_END_OF_STREAM", UM_BUFFER_FLAG_END_OF_STREAM);
    fj_static_int(C_MC, "BUFFER_FLAG_PARTIAL_FRAME", UM_BUFFER_FLAG_PARTIAL_FRAME);
    fj_static_int(C_MC, "CONFIGURE_FLAG_ENCODE", 1);
    fj_static_int(C_MC, "VIDEO_SCALING_MODE_SCALE_TO_FIT", 1);

    fj_field_define(C_BI, "offset", "I", 0, fj_i(0));
    fj_field_define(C_BI, "size", "I", 0, fj_i(0));
    fj_field_define(C_BI, "presentationTimeUs", "J", 0, fj_j(0));
    fj_field_define(C_BI, "flags", "I", 0, fj_i(0));
    M(C_BI, "<init>", "()V", bi_init);
    M(C_BI, "set", "(IIJI)V", bi_set);

    M(C_ST, "<init>", "(I)V", st_init);
    M(C_ST, "<init>", "(IZ)V", st_init);
    M(C_ST, "setOnFrameAvailableListener", "(L" C_STL ";)V", st_setListener);
    MU(C_ST, "setOnFrameAvailableListener", "(L" C_STL ";Landroid/os/Handler;)V", st_setListener, 1);
    M(C_ST, "updateTexImage", "()V", st_update);
    M(C_ST, "getTransformMatrix", "([F)V", st_matrix);
    M(C_ST, "getTimestamp", "()J", st_timestamp);
    M(C_ST, "setDefaultBufferSize", "(II)V", st_size);
    M(C_ST, "detachFromGLContext", "()V", st_detach);
    M(C_ST, "attachToGLContext", "(I)V", st_detach);
    M(C_ST, "isReleased", "()Z", st_released);
    M(C_ST, "release", "()V", st_release);
    M(C_SURF, "<init>", "(L" C_ST ";)V", surf_init_st);

    const char *B = "java/nio/Buffer";
    fj_field_define(B, "position", "I", 0, fj_i(0));
    fj_field_define(B, "limit", "I", 0, fj_i(0));
    M(B, "clear", "()Ljava/nio/Buffer;", buf_clear);
    M(B, "rewind", "()Ljava/nio/Buffer;", buf_rewind);
    M(B, "capacity", "()I", buf_cap);
    M(B, "position", "()I", buf_position);
    M(B, "limit", "()I", buf_limit);
    M(B, "position", "(I)Ljava/nio/Buffer;", buf_setpos);
    M(B, "limit", "(I)Ljava/nio/Buffer;", buf_setlim);
    M(B, "remaining", "()I", buf_remaining);
    M("java/nio/ByteBuffer", "isDirect", "()Z", buf_true);
}

/* the host's call: pulls this file (and the media core) into the program and says what it decodes */
void up_media_init(void)
{
    um_log("decoders: %s", um_decoders());
}
