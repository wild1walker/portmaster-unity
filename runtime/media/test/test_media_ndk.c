// The NDK media API (../android/media_ndk.c over the media core) driven as libunity 2022.3's
// AndroidVideoMedia<AndroidMediaNDK> does: AMediaExtractor over an fd range and over an
// AMediaDataSource, AMediaFormat getters, an AMediaCodec configured with the ANativeWindow of a
// Java Surface(SurfaceTexture) (ANativeWindow_fromSurface, ../android/native_window.c) whose
// listener hears of each rendered frame, output format changes, PCM from an audio decoder.
#define _GNU_SOURCE
#include "../um.h"
#include "../../jni/unityplayer.h"
#include "../../jni/fakejni.h"
#include "../../android/android_shims.h"
#include "clips.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void up_media_init(void);

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct AMediaExtractor AMediaExtractor;
typedef struct AMediaFormat AMediaFormat;
typedef struct AMediaCodec AMediaCodec;
typedef struct AMediaDataSource AMediaDataSource;
typedef struct { int32_t offset; int32_t size; int64_t presentationTimeUs; uint32_t flags; } AMediaCodecBufferInfo;
AMediaExtractor *AMediaExtractor_new(void);
int32_t AMediaExtractor_delete(AMediaExtractor *);
int32_t AMediaExtractor_setDataSourceFd(AMediaExtractor *, int, int64_t, int64_t);
int32_t AMediaExtractor_setDataSourceCustom(AMediaExtractor *, AMediaDataSource *);
size_t AMediaExtractor_getTrackCount(AMediaExtractor *);
AMediaFormat *AMediaExtractor_getTrackFormat(AMediaExtractor *, size_t);
int32_t AMediaExtractor_selectTrack(AMediaExtractor *, size_t);
ssize_t AMediaExtractor_readSampleData(AMediaExtractor *, uint8_t *, size_t);
int64_t AMediaExtractor_getSampleTime(AMediaExtractor *);
int AMediaExtractor_getSampleTrackIndex(AMediaExtractor *);
bool AMediaExtractor_advance(AMediaExtractor *);
int32_t AMediaExtractor_seekTo(AMediaExtractor *, int64_t, int);
bool AMediaFormat_getInt32(AMediaFormat *, const char *, int32_t *);
bool AMediaFormat_getInt64(AMediaFormat *, const char *, int64_t *);
bool AMediaFormat_getFloat(AMediaFormat *, const char *, float *);
bool AMediaFormat_getString(AMediaFormat *, const char *, const char **);
const char *AMediaFormat_toString(AMediaFormat *);
int32_t AMediaFormat_delete(AMediaFormat *);
AMediaCodec *AMediaCodec_createDecoderByType(const char *);
int32_t AMediaCodec_configure(AMediaCodec *, const AMediaFormat *, ANativeWindow *, void *, uint32_t);
int32_t AMediaCodec_start(AMediaCodec *);
int32_t AMediaCodec_stop(AMediaCodec *);
int32_t AMediaCodec_delete(AMediaCodec *);
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *, int64_t);
uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *, size_t, size_t *);
uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *, size_t, size_t *);
int32_t AMediaCodec_queueInputBuffer(AMediaCodec *, size_t, long, size_t, uint64_t, uint32_t);
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *, AMediaCodecBufferInfo *, int64_t);
AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *);
int32_t AMediaCodec_releaseOutputBuffer(AMediaCodec *, size_t, bool);
AMediaDataSource *AMediaDataSource_new(void);
void AMediaDataSource_delete(AMediaDataSource *);
void AMediaDataSource_setUserdata(AMediaDataSource *, void *);
void AMediaDataSource_setReadAt(AMediaDataSource *, void *);
void AMediaDataSource_setGetSize(AMediaDataSource *, void *);
extern const char *AMEDIAFORMAT_KEY_MIME, *AMEDIAFORMAT_KEY_WIDTH, *AMEDIAFORMAT_KEY_DURATION, *AMEDIAFORMAT_KEY_FRAME_RATE,
                  *AMEDIAFORMAT_KEY_SAMPLE_RATE, *AMEDIAFORMAT_KEY_CHANNEL_COUNT, *AMEDIAFORMAT_KEY_COLOR_RANGE;

static int heard;
static jvalue on_frame(JNIEnv *env, jobject self, const jvalue *a, void *ud) { (void)env; (void)self; (void)ud; if (a[0].l) heard++; return fj_v(); }

/* an AMediaDataSource over memory */
static const wb *g_clip;
static ssize_t mem_read(void *user, int64_t off, void *buf, size_t n)
{
    (void)user;
    if (off < 0 || (size_t)off >= g_clip->n) return 0;
    if (n > g_clip->n - (size_t)off) n = g_clip->n - (size_t)off;
    memcpy(buf, g_clip->p + off, n);
    return (ssize_t)n;
}
static ssize_t mem_size(void *user) { (void)user; return (ssize_t)g_clip->n; }

/* Unity's loop on one track; returns frames (video) or PCM frames (audio) */
static long run(AMediaExtractor *ex, AMediaCodec *c, int track, int render, int *formats, int32_t *w, int *eos_out)
{
    long out = 0;
    int in_eos = 0, eos = 0;
    AMediaCodecBufferInfo bi;
    for (int spins = 0; spins < 5000 && !eos; spins++) {
        if (!in_eos) {
            ssize_t i = AMediaCodec_dequeueInputBuffer(c, 10000);
            if (i >= 0) {
                size_t cap;
                uint8_t *b = AMediaCodec_getInputBuffer(c, i, &cap);
                ssize_t n = AMediaExtractor_getSampleTrackIndex(ex) == track ? AMediaExtractor_readSampleData(ex, b, cap) : -1;
                if (n < 0) { AMediaCodec_queueInputBuffer(c, i, 0, 0, 0, 4); in_eos = 1; }
                else { AMediaCodec_queueInputBuffer(c, i, 0, n, AMediaExtractor_getSampleTime(ex), 0); AMediaExtractor_advance(ex); }
            }
        }
        ssize_t o = AMediaCodec_dequeueOutputBuffer(c, &bi, 10000);
        if (o == -2) {
            AMediaFormat *f = AMediaCodec_getOutputFormat(c);
            if (w) AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, w);
            AMediaFormat_delete(f);
            (*formats)++;
        } else if (o >= 0) {
            if (bi.flags & 4) eos = 1;
            else out += render ? 1 : bi.size / 4;
            AMediaCodec_releaseOutputBuffer(c, o, render);
        }
        up_poll();
    }
    *eos_out = eos;
    return out;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    struct up_config cfg = { 0 };
    cfg.files_dir = "/tmp/umn/files"; cfg.cache_dir = "/tmp/umn/cache"; cfg.external_files_dir = "/tmp/umn/ext";
    cfg.data_dir = "/tmp/umn"; cfg.package_name = "com.test.mediandk";
    up_register_classes(&cfg);
    up_media_init();
    JNIEnv *E = fj_env();

    plist vp = encode_vpx(0, 96, 54, 20);
    uint8_t *priv; size_t privn;
    plist au = encode_vorbis(1, &priv, &privn);
    mtrack t[2] = { { "V_VP8", 96, 54, 30.0f, NULL, 0, 0, &vp, 0 }, { "A_VORBIS", 0, 0, 0, priv, privn, 0, &au, 1 } };
    wb clip = mux_webm(t, 2, 667, 0);
    char path[] = "/tmp/umn-XXXXXX";
    int tfd = mkstemp(path); close(tfd);
    int64_t off;
    int fd = write_embedded(path, &clip, &off);
    unlink(path);

    printf("-- AMediaExtractor over an fd range\n");
    AMediaExtractor *ex = AMediaExtractor_new();
    CHECK(ex && AMediaExtractor_setDataSourceFd(ex, fd, off, clip.n) == 0, "setDataSourceFd");
    close(fd);
    CHECK(AMediaExtractor_getTrackCount(ex) == 2, "tracks");
    AMediaFormat *vf = AMediaExtractor_getTrackFormat(ex, 0);
    const char *mime = NULL;
    int32_t i32 = 0; int64_t i64 = 0; float fr = 0;
    CHECK(AMediaFormat_getString(vf, AMEDIAFORMAT_KEY_MIME, &mime) && !strcmp(mime, "video/x-vnd.on2.vp8"), "mime");
    CHECK(AMediaFormat_getInt32(vf, AMEDIAFORMAT_KEY_WIDTH, &i32) && i32 == 96, "width");
    CHECK(AMediaFormat_getInt64(vf, AMEDIAFORMAT_KEY_DURATION, &i64) && i64 == 667000, "duration %lld", (long long)i64);
    CHECK(AMediaFormat_getFloat(vf, AMEDIAFORMAT_KEY_FRAME_RATE, &fr) && fr == 30.0f, "frame rate");
    CHECK(!AMediaFormat_getInt32(vf, AMEDIAFORMAT_KEY_COLOR_RANGE, &i32), "no full-range flag (Unity refuses full range)");
    CHECK(strstr(AMediaFormat_toString(vf), "mime=video/x-vnd.on2.vp8") != NULL, "toString");

    printf("-- AMediaCodec into a Surface(SurfaceTexture)'s window\n");
    jclass cST = (*E)->FindClass(E, "android/graphics/SurfaceTexture"), cSURF = (*E)->FindClass(E, "android/view/Surface");
    jclass cL = fj_class_define("test/NdkFrameListener", NULL);
    fj_class_implements("test/NdkFrameListener", "android/graphics/SurfaceTexture$OnFrameAvailableListener");
    fj_method_define("test/NdkFrameListener", "onFrameAvailable", "(Landroid/graphics/SurfaceTexture;)V", 0, on_frame, NULL);
    jobject st = (*E)->NewObject(E, cST, (*E)->GetMethodID(E, cST, "<init>", "(I)V"), 9);
    (*E)->CallVoidMethod(E, st, (*E)->GetMethodID(E, cST, "setOnFrameAvailableListener", "(Landroid/graphics/SurfaceTexture$OnFrameAvailableListener;)V"),
                         fj_new(cL, NULL, NULL));
    jobject surf = (*E)->NewObject(E, cSURF, (*E)->GetMethodID(E, cSURF, "<init>", "(Landroid/graphics/SurfaceTexture;)V"), st);
    ANativeWindow *win = ANativeWindow_fromSurface(E, surf);
    CHECK(win && win != android_window_get(), "a window of its own for the SurfaceTexture's Surface");
    jobject main_surface = fj_new_by_name("android/view/Surface", android_window_get(), NULL);   /* as up_start makes it */
    CHECK(ANativeWindow_fromSurface(E, main_surface) == android_window_get(), "the main Surface still gives the main window");
    AMediaCodec *c = AMediaCodec_createDecoderByType(mime);
    CHECK(c && AMediaCodec_configure(c, vf, win, NULL, 0) == 0 && AMediaCodec_start(c) == 0, "configure with the window, start");
    size_t cap = 0;
    CHECK(AMediaCodec_getInputBuffer(c, 0, &cap) && cap > 0, "input buffer");
    AMediaExtractor_selectTrack(ex, 0);
    int formats = 0, eos = 0;
    int32_t w = 0;
    long frames = run(ex, c, 0, 1, &formats, &w, &eos);
    for (int k = 0; k < 100 && heard < frames; k++) { up_poll(); usleep(10000); }
    CHECK(frames == 20 && formats == 1 && w == 96 && eos, "%ld frames, %d format changes, eos %d", frames, formats, eos);
    CHECK(heard == frames, "onFrameAvailable for every rendered frame (%d)", heard);
    AMediaCodec_stop(c);
    AMediaCodec_delete(c);
    ANativeWindow_release(win);
    AMediaFormat_delete(vf);
    AMediaExtractor_delete(ex);

    printf("-- AMediaDataSource, audio\n");
    g_clip = &clip;
    AMediaDataSource *ds = AMediaDataSource_new();
    AMediaDataSource_setUserdata(ds, NULL);
    AMediaDataSource_setReadAt(ds, (void *)mem_read);
    AMediaDataSource_setGetSize(ds, (void *)mem_size);
    ex = AMediaExtractor_new();
    CHECK(AMediaExtractor_setDataSourceCustom(ex, ds) == 0 && AMediaExtractor_getTrackCount(ex) == 2, "setDataSourceCustom");
    AMediaFormat *af = AMediaExtractor_getTrackFormat(ex, 1);
    CHECK(AMediaFormat_getInt32(af, AMEDIAFORMAT_KEY_SAMPLE_RATE, &i32) && i32 == 48000, "audio rate");
    AMediaExtractor_selectTrack(ex, 1);
    c = AMediaCodec_createDecoderByType("audio/vorbis");
    CHECK(c && AMediaCodec_configure(c, af, NULL, NULL, 0) == 0 && AMediaCodec_start(c) == 0, "audio decoder");
    formats = 0;
    long pcm = run(ex, c, 1, 0, &formats, NULL, &eos);
    CHECK(labs(pcm - 48000) < 2048 && formats == 1 && eos, "%ld PCM frames", pcm);
    AMediaCodec_delete(c);
    AMediaFormat_delete(af);
    AMediaExtractor_delete(ex);
    AMediaDataSource_delete(ds);

    printf("%s: %d failure(s) in %d checks\n", fails ? "FAILED" : "PASSED", fails, checks);
    return fails != 0;
}
