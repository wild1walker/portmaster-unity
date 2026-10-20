// The Java media classes (../jni/up_media.c) driven through JNI the way libunity's
// AndroidMediaJNI does (players 2017.4 - 2021.3), on a clip made by clips.c: the clip inside a
// bigger file opened through ParcelFileDescriptor.adoptFd + MediaExtractor.setDataSource(fd,
// offset, length), track formats and their Java exceptions, a video decoder configured with a
// Surface(SurfaceTexture) whose listener hears of every rendered frame on the looper, the
// ByteBuffer arrays Unity checks (every buffer non-null with a capacity), BufferInfo fields, an
// audio decoder's PCM, and the exceptions of bad calls.
#define _GNU_SOURCE
#include "../um.h"
#include "../../jni/unityplayer.h"
#include "../../jni/fakejni.h"
#include "clips.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void up_media_init(void);

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static JNIEnv *E;
#define J (*E)

/* the pending exception's class name (and clear it), or NULL */
static const char *exc(void)
{
    static char name[128];
    if (!J->ExceptionCheck(E)) return NULL;
    jthrowable t = J->ExceptionOccurred(E);
    J->ExceptionClear(E);
    snprintf(name, sizeof name, "%s", fj_class_name(fj_object_class(t)));
    return name;
}
static int no_exc(void) { const char *e = exc(); if (e) printf("   unexpected %s\n", e); return e == NULL; }

static jclass C(const char *n) { jclass c = J->FindClass(E, n); CHECK(c != NULL, "class %s", n); return c; }
static jmethodID Mi(jclass c, const char *n, const char *s) { jmethodID m = J->GetMethodID(E, c, n, s); CHECK(m && fj_method_defined(fj_class_name(c), n, s), "%s.%s%s", fj_class_name(c), n, s); return m; }
static jmethodID Ms(jclass c, const char *n, const char *s) { jmethodID m = J->GetStaticMethodID(E, c, n, s); CHECK(m && fj_method_defined(fj_class_name(c), n, s), "static %s.%s%s", fj_class_name(c), n, s); return m; }
static jstring S(const char *s) { return J->NewStringUTF(E, s); }
static jobject key(jclass fmt, const char *field)
{
    jfieldID f = J->GetStaticFieldID(E, fmt, field, "Ljava/lang/String;");
    return J->GetStaticObjectField(E, fmt, f);
}

static int heard;
static jvalue on_frame(JNIEnv *env, jobject self, const jvalue *a, void *ud) { (void)env; (void)self; (void)ud; if (a[0].l) heard++; return fj_v(); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    struct up_config cfg = { 0 };
    cfg.files_dir = "/tmp/umj/files"; cfg.cache_dir = "/tmp/umj/cache"; cfg.external_files_dir = "/tmp/umj/ext";
    cfg.data_dir = "/tmp/umj"; cfg.package_name = "com.test.media";
    up_register_classes(&cfg);
    up_media_init();
    E = fj_env();

    /* a clip of 30 frames and 2 s of audio inside a file */
    plist vp = encode_vpx(0, 128, 72, 30);
    uint8_t *priv; size_t privn;
    plist au = encode_vorbis(2, &priv, &privn);
    mtrack t[2] = { { "V_VP8", 128, 72, 30.0f, NULL, 0, 0, &vp, 0 }, { "A_VORBIS", 0, 0, 0, priv, privn, 0, &au, 1 } };
    wb clip = mux_webm(t, 2, 1000, 0);
    char path[] = "/tmp/umj-XXXXXX";
    int tfd = mkstemp(path); close(tfd);
    int64_t off;
    int fd = write_embedded(path, &clip, &off);

    jclass cPFD = C("android/os/ParcelFileDescriptor"), cEX = C("android/media/MediaExtractor"),
           cFMT = C("android/media/MediaFormat"), cMC = C("android/media/MediaCodec"),
           cBI = C("android/media/MediaCodec$BufferInfo"), cST = C("android/graphics/SurfaceTexture"),
           cSURF = C("android/view/Surface"), cBUF = C("java/nio/Buffer");

    printf("-- MediaExtractor over an fd range\n");
    jobject pfd = J->CallStaticObjectMethod(E, cPFD, Ms(cPFD, "adoptFd", "(I)Landroid/os/ParcelFileDescriptor;"), fd);
    jobject fdo = J->CallObjectMethod(E, pfd, Mi(cPFD, "getFileDescriptor", "()Ljava/io/FileDescriptor;"));
    jobject ex = J->NewObject(E, cEX, Mi(cEX, "<init>", "()V"));
    jmethodID setDS = Mi(cEX, "setDataSource", "(Ljava/io/FileDescriptor;JJ)V");
    J->CallVoidMethod(E, ex, setDS, (jobject)NULL, (jlong)0, (jlong)0);
    const char *x = exc();
    CHECK(x && !strcmp(x, "java/lang/IllegalArgumentException"), "null FileDescriptor: IllegalArgumentException");
    J->CallVoidMethod(E, ex, setDS, fdo, (jlong)off, (jlong)clip.n);
    CHECK(no_exc(), "setDataSource(fd, offset, length)");
    jint dfd = J->CallIntMethod(E, pfd, Mi(cPFD, "detachFd", "()I"));
    CHECK(dfd == fd, "detachFd gives the fd back");
    close(fd);                                   /* Unity closes its own descriptor */
    unlink(path);
    CHECK(J->CallIntMethod(E, ex, Mi(cEX, "getTrackCount", "()I")) == 2, "two tracks");

    jmethodID getFmt = Mi(cEX, "getTrackFormat", "(I)Landroid/media/MediaFormat;");
    jmethodID fGetStr = Mi(cFMT, "getString", "(Ljava/lang/String;)Ljava/lang/String;"),
              fGetInt = Mi(cFMT, "getInteger", "(Ljava/lang/String;)I"),
              fGetLong = Mi(cFMT, "getLong", "(Ljava/lang/String;)J"),
              fGetFloat = Mi(cFMT, "getFloat", "(Ljava/lang/String;)F"),
              fContains = Mi(cFMT, "containsKey", "(Ljava/lang/String;)Z"),
              fToString = Mi(cFMT, "toString", "()Ljava/lang/String;");
    jobject vf = J->CallObjectMethod(E, ex, getFmt, 0), af = J->CallObjectMethod(E, ex, getFmt, 1);
    const char *mime = fj_cstr(J->CallObjectMethod(E, vf, fGetStr, key(cFMT, "KEY_MIME")));
    CHECK(mime && !strcmp(mime, "video/x-vnd.on2.vp8"), "video mime %s", mime ? mime : "-");
    CHECK(J->CallIntMethod(E, vf, fGetInt, key(cFMT, "KEY_WIDTH")) == 128 && J->CallIntMethod(E, vf, fGetInt, key(cFMT, "KEY_HEIGHT")) == 72, "size");
    CHECK(J->CallLongMethod(E, vf, fGetLong, key(cFMT, "KEY_DURATION")) == 1000000, "durationUs");
    CHECK(J->CallBooleanMethod(E, vf, fContains, key(cFMT, "KEY_FRAME_RATE")) &&
          J->CallFloatMethod(E, vf, fGetFloat, key(cFMT, "KEY_FRAME_RATE")) == 30.0f, "frame-rate (float)");
    J->CallIntMethod(E, vf, fGetInt, key(cFMT, "KEY_FRAME_RATE"));
    x = exc();
    CHECK(x && !strcmp(x, "java/lang/ClassCastException"), "getInteger of a float throws ClassCastException (%s)", x ? x : "none");
    J->CallIntMethod(E, vf, fGetInt, S("no-such-key"));
    x = exc();
    CHECK(x && !strcmp(x, "java/lang/NullPointerException"), "getInteger of a missing key throws NullPointerException");
    CHECK(!J->CallBooleanMethod(E, vf, fContains, S("no-such-key")), "containsKey false");
    CHECK(J->CallIntMethod(E, af, fGetInt, key(cFMT, "KEY_SAMPLE_RATE")) == 48000 && J->CallIntMethod(E, af, fGetInt, key(cFMT, "KEY_CHANNEL_COUNT")) == 2, "audio format");
    const char *ts = fj_cstr(J->CallObjectMethod(E, af, fToString));
    CHECK(ts && strstr(ts, "mime=audio/vorbis"), "toString");

    printf("-- MediaCodec with a Surface(SurfaceTexture)\n");
    jclass cL = fj_class_define("test/FrameListener", NULL);
    fj_class_implements("test/FrameListener", "android/graphics/SurfaceTexture$OnFrameAvailableListener");
    fj_method_define("test/FrameListener", "onFrameAvailable", "(Landroid/graphics/SurfaceTexture;)V", 0, on_frame, NULL);
    jobject lis = fj_new(cL, NULL, NULL);
    jobject st = J->NewObject(E, cST, Mi(cST, "<init>", "(I)V"), 7);
    J->CallVoidMethod(E, st, Mi(cST, "setOnFrameAvailableListener", "(Landroid/graphics/SurfaceTexture$OnFrameAvailableListener;)V"), lis);
    jobject surf = J->NewObject(E, cSURF, Mi(cSURF, "<init>", "(Landroid/graphics/SurfaceTexture;)V"), st);
    CHECK(no_exc() && surf, "Surface(SurfaceTexture)");
    jobject codec = J->CallStaticObjectMethod(E, cMC, Ms(cMC, "createDecoderByType", "(Ljava/lang/String;)Landroid/media/MediaCodec;"), S(mime));
    CHECK(no_exc() && codec, "createDecoderByType");
    const char *nm = fj_cstr(J->CallObjectMethod(E, codec, Mi(cMC, "getName", "()Ljava/lang/String;")));
    CHECK(nm && !strcmp(nm, "OMX.google.vp8.decoder"), "codec name");
    jmethodID dqIn = Mi(cMC, "dequeueInputBuffer", "(J)I");
    J->CallIntMethod(E, codec, dqIn, (jlong)0);
    x = exc();
    CHECK(x && !strcmp(x, "java/lang/IllegalStateException"), "dequeueInputBuffer before start throws");
    J->CallVoidMethod(E, codec, Mi(cMC, "configure", "(Landroid/media/MediaFormat;Landroid/view/Surface;Landroid/media/MediaCrypto;I)V"), vf, surf, (jobject)NULL, 0);
    J->CallVoidMethod(E, codec, Mi(cMC, "start", "()V"));
    CHECK(no_exc(), "configure + start");
    jobjectArray inb = J->CallObjectMethod(E, codec, Mi(cMC, "getInputBuffers", "()[Ljava/nio/ByteBuffer;"));
    jobjectArray outb = J->CallObjectMethod(E, codec, Mi(cMC, "getOutputBuffers", "()[Ljava/nio/ByteBuffer;"));
    int okbufs = inb && outb && J->GetArrayLength(E, inb) > 0 && J->GetArrayLength(E, outb) > 0;
    for (int k = 0; okbufs && k < J->GetArrayLength(E, inb); k++) {
        jobject b = J->GetObjectArrayElement(E, inb, k);
        okbufs &= J->GetDirectBufferAddress(E, b) != NULL && J->GetDirectBufferCapacity(E, b) >= 4096;
    }
    for (int k = 0; okbufs && k < J->GetArrayLength(E, outb); k++) {
        jobject b = J->GetObjectArrayElement(E, outb, k);
        okbufs &= J->GetDirectBufferAddress(E, b) != NULL && J->GetDirectBufferCapacity(E, b) > 0;
    }
    CHECK(okbufs, "every input/output ByteBuffer has memory (Unity's Buffers::Init refuses others)");
    J->CallObjectMethod(E, J->GetObjectArrayElement(E, inb, 0), Mi(cBUF, "clear", "()Ljava/nio/Buffer;"));
    CHECK(no_exc(), "Buffer.clear");

    J->CallVoidMethod(E, ex, Mi(cEX, "selectTrack", "(I)V"), 0);
    jmethodID rd = Mi(cEX, "readSampleData", "(Ljava/nio/ByteBuffer;I)I"), stime = Mi(cEX, "getSampleTime", "()J"),
              strack = Mi(cEX, "getSampleTrackIndex", "()I"), adv = Mi(cEX, "advance", "()Z"),
              qIn = Mi(cMC, "queueInputBuffer", "(IIIJI)V"), dqOut = Mi(cMC, "dequeueOutputBuffer", "(Landroid/media/MediaCodec$BufferInfo;J)I"),
              rel = Mi(cMC, "releaseOutputBuffer", "(IZ)V"), ofmt = Mi(cMC, "getOutputFormat", "()Landroid/media/MediaFormat;");
    jobject bi = J->NewObject(E, cBI, Mi(cBI, "<init>", "()V"));
    jfieldID fPts = J->GetFieldID(E, cBI, "presentationTimeUs", "J"), fFlags = J->GetFieldID(E, cBI, "flags", "I"),
             fSize = J->GetFieldID(E, cBI, "size", "I"), fOff = J->GetFieldID(E, cBI, "offset", "I");
    int frames = 0, formats = 0, eos = 0, in_eos = 0, ordered = 1;
    jlong last = -1;
    for (int spins = 0; spins < 5000 && !eos; spins++) {
        if (!in_eos) {
            jint i = J->CallIntMethod(E, codec, dqIn, (jlong)10000);
            if (i >= 0) {
                jint n = J->CallIntMethod(E, ex, rd, J->GetObjectArrayElement(E, inb, i), 0);
                if (n < 0) { J->CallVoidMethod(E, codec, qIn, i, 0, 0, (jlong)0, 4); in_eos = 1; }
                else {
                    CHECK(J->CallIntMethod(E, ex, strack) == 0, "only the selected track");
                    J->CallVoidMethod(E, codec, qIn, i, 0, n, J->CallLongMethod(E, ex, stime), 0);
                    J->CallBooleanMethod(E, ex, adv);
                }
            }
        }
        jint o = J->CallIntMethod(E, codec, dqOut, bi, (jlong)10000);
        if (o == -2) {
            jobject of = J->CallObjectMethod(E, codec, ofmt);
            CHECK(J->CallIntMethod(E, of, fGetInt, S("width")) == 128 && J->CallIntMethod(E, of, fGetInt, S("crop-right")) == 127 &&
                  J->CallIntMethod(E, of, fGetInt, S("crop-bottom")) == 71, "output format");
            formats++;
        } else if (o >= 0) {
            if (J->GetIntField(E, bi, fFlags) & 4) eos = 1;
            else {
                jlong p = J->GetLongField(E, bi, fPts);
                if (p <= last) ordered = 0;
                last = p;
                frames++;
            }
            J->CallVoidMethod(E, codec, rel, o, (jboolean)1);
        }
        up_poll();                               /* the main looper: onFrameAvailable */
    }
    for (int k = 0; k < 100 && heard < frames; k++) { up_poll(); usleep(10000); }
    CHECK(formats == 1 && frames == 30 && eos && ordered, "%d frames, %d format changes, eos %d", frames, formats, eos);
    CHECK(heard == frames, "onFrameAvailable once per rendered frame on the looper (%d)", heard);
    J->CallVoidMethod(E, st, Mi(cST, "updateTexImage", "()V"));
    CHECK(no_exc(), "updateTexImage without GL: no exception");
    jfloatArray m = J->NewFloatArray(E, 16);
    J->CallVoidMethod(E, st, Mi(cST, "getTransformMatrix", "([F)V"), m);
    jfloat mv[16];
    J->GetFloatArrayRegion(E, m, 0, 16, mv);
    CHECK(mv[0] == 1 && mv[5] == -1 && mv[12] == 0 && mv[13] == 1 && mv[15] == 1, "getTransformMatrix: Android's y flip");
    J->CallVoidMethod(E, codec, Mi(cMC, "stop", "()V"));
    J->CallVoidMethod(E, codec, Mi(cMC, "release", "()V"));
    J->CallIntMethod(E, codec, dqIn, (jlong)0);
    x = exc();
    CHECK(x && !strcmp(x, "java/lang/IllegalStateException"), "a released codec throws");
    J->CallVoidMethod(E, st, Mi(cST, "release", "()V"));
    CHECK(no_exc(), "SurfaceTexture.release");

    printf("-- audio to buffers\n");
    J->CallVoidMethod(E, ex, Mi(cEX, "unselectTrack", "(I)V"), 0);
    J->CallVoidMethod(E, ex, Mi(cEX, "selectTrack", "(I)V"), 1);
    J->CallVoidMethod(E, ex, Mi(cEX, "seekTo", "(JI)V"), (jlong)0, 0);
    codec = J->CallStaticObjectMethod(E, cMC, Ms(cMC, "createDecoderByType", "(Ljava/lang/String;)Landroid/media/MediaCodec;"), S("audio/vorbis"));
    J->CallVoidMethod(E, codec, Mi(cMC, "configure", "(Landroid/media/MediaFormat;Landroid/view/Surface;Landroid/media/MediaCrypto;I)V"), af, (jobject)NULL, (jobject)NULL, 0);
    J->CallVoidMethod(E, codec, Mi(cMC, "start", "()V"));
    inb = J->CallObjectMethod(E, codec, Mi(cMC, "getInputBuffers", "()[Ljava/nio/ByteBuffer;"));
    outb = J->CallObjectMethod(E, codec, Mi(cMC, "getOutputBuffers", "()[Ljava/nio/ByteBuffer;"));
    long long pcm_frames = 0;
    int rate = 0, ch = 0, nonzero = 0;
    eos = in_eos = 0;
    for (int spins = 0; spins < 5000 && !eos; spins++) {
        if (!in_eos) {
            jint i = J->CallIntMethod(E, codec, dqIn, (jlong)10000);
            if (i >= 0) {
                jint n = J->CallIntMethod(E, ex, rd, J->GetObjectArrayElement(E, inb, i), 0);
                if (n < 0) { J->CallVoidMethod(E, codec, qIn, i, 0, 0, (jlong)0, 4); in_eos = 1; }
                else { J->CallVoidMethod(E, codec, qIn, i, 0, n, J->CallLongMethod(E, ex, stime), 0); J->CallBooleanMethod(E, ex, adv); }
            }
        }
        jint o = J->CallIntMethod(E, codec, dqOut, bi, (jlong)10000);
        if (o == -2) {
            jobject of = J->CallObjectMethod(E, codec, ofmt);
            rate = J->CallIntMethod(E, of, fGetInt, key(cFMT, "KEY_SAMPLE_RATE"));
            ch = J->CallIntMethod(E, of, fGetInt, key(cFMT, "KEY_CHANNEL_COUNT"));
        } else if (o >= 0) {
            if (J->GetIntField(E, bi, fFlags) & 4) eos = 1;
            int16_t *p = J->GetDirectBufferAddress(E, J->GetObjectArrayElement(E, outb, o));
            int n = J->GetIntField(E, bi, fSize) / 4;
            p += J->GetIntField(E, bi, fOff) / 2;
            for (int k = 0; k < n * 2; k++) if (p[k]) { nonzero = 1; break; }
            pcm_frames += n;
            J->CallVoidMethod(E, codec, rel, o, (jboolean)0);
        }
    }
    CHECK(rate == 48000 && ch == 2, "PCM format %d Hz %d ch", rate, ch);
    CHECK(llabs(pcm_frames - 2 * 48000) < 2048 && nonzero, "%lld PCM frames", pcm_frames);
    J->CallVoidMethod(E, codec, Mi(cMC, "release", "()V"));
    J->CallVoidMethod(E, ex, Mi(cEX, "release", "()V"));

    printf("-- errors\n");
    J->CallStaticObjectMethod(E, cMC, Ms(cMC, "createDecoderByType", "(Ljava/lang/String;)Landroid/media/MediaCodec;"), S("video/unknown"));
    x = exc();
    CHECK(x && !strcmp(x, "java/io/IOException"), "no decoder: IOException");
    jobject ex2 = J->NewObject(E, cEX, Mi(cEX, "<init>", "()V"));
    J->CallVoidMethod(E, ex2, Mi(cEX, "setDataSource", "(Ljava/lang/String;)V"), S("/nonexistent.webm"));
    x = exc();
    CHECK(x && !strcmp(x, "java/io/IOException"), "missing file: IOException");
    J->CallVoidMethod(E, ex2, Mi(cEX, "setDataSource", "(Ljava/lang/String;)V"), S("https://example.invalid/clip.webm"));
    x = exc();
    CHECK(x && !strcmp(x, "java/io/IOException"), "no network: IOException");

    printf("%s: %d failure(s) in %d checks\n", fails ? "FAILED" : "PASSED", fails, checks);
    return fails != 0;
}
