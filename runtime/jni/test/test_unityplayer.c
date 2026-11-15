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
#include <zlib.h>

int fake_unity_get(int i);
void fake_unity_set_shape(int shape);
jobject fake_unity_proxy(void);
jint FJ_ABI fake_libmain_onload(JavaVM *vm, void *reserved);
jint FJ_ABI fake_libunity_onload(JavaVM *vm, void *reserved);
static int no_libmain;     /* the newest shape once more without libmain: libunity's JNI_OnLoad called directly (Unity 7) */
void fake_unity_soft_input(int op, int arg);
const char *fake_unity_si_seq(void);
const char *fake_unity_si_str(void);
void fake_unity_si_reset(void);

static const char *const shape_name[SHAPE_N] = { "2017.4.9", "2017.4.40", "2018.4", "2019.4", "2020.3", "2021.3", "2022.3", "6000.6" };

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define R(i) fake_unity_get(i)

static int frames_cb, audio_frames, quit_cb;
static void on_frame(void *u) { frames_cb++; }
static void on_audio(void *u, const int16_t *pcm, int frames, int rate) { audio_frames += frames; usleep(1000); }
static void on_quit(void *u) { quit_cb++; }

/* System.loadLibrary("plugin") and System.load(<abs path>) reach the host; nothing else may */
static int loadlib_bad;
static int on_load_library(void *u, const char *name)
{
    if (!strcmp(name, "plugin") || !strcmp(name, "/tmp/uptest/lib/libplugin.so")) return 1;
    if (strcmp(name, "missing")) { printf("  host asked to load \"%s\"\n", name); loadlib_bad++; }
    return 0;
}

/* a STORE zip as the converter writes an OBB: assets first, the GUID file at the root */
static void put16(FILE *f, unsigned v) { fputc(v & 0xff, f); fputc(v >> 8 & 0xff, f); }
static void put32(FILE *f, unsigned long v) { put16(f, v & 0xffff); put16(f, v >> 16 & 0xffff); }
static void write_obb(const char *path)
{
    static const char *const names[2] = { "assets/first.bin", "unity_obb_guid" };
    static const char *const data[2] = { "xxxxxxxx", "5678-obb-guid" };
    unsigned long off[2], crc[2];
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (int i = 0; i < 2; i++) {
        size_t nl = strlen(names[i]), dl = strlen(data[i]);
        off[i] = ftell(f);
        crc[i] = crc32(0, (const Bytef *)data[i], dl);
        put32(f, 0x04034b50); put16(f, 20); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0);
        put32(f, crc[i]); put32(f, dl); put32(f, dl); put16(f, nl); put16(f, 0);
        fwrite(names[i], 1, nl, f); fwrite(data[i], 1, dl, f);
    }
    unsigned long cd = ftell(f);
    for (int i = 0; i < 2; i++) {
        size_t nl = strlen(names[i]), dl = strlen(data[i]);
        put32(f, 0x02014b50); put16(f, 20); put16(f, 20); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0);
        put32(f, crc[i]); put32(f, dl); put32(f, dl); put16(f, nl); put16(f, 0); put16(f, 0); put16(f, 0);
        put16(f, 0); put32(f, 0); put32(f, off[i]);
        fwrite(names[i], 1, nl, f);
    }
    unsigned long end = ftell(f);
    put32(f, 0x06054b50); put16(f, 0); put16(f, 0); put16(f, 2); put16(f, 2); put32(f, end - cd); put32(f, cd); put16(f, 0);
    fclose(f);
}

static int wait_for(int idx, int min, int ms)
{
    for (int t = 0; t < ms; t += 10) {
        up_poll();
        if (R(idx) >= min) return 1;
        usleep(10000);
    }
    return 0;
}

/* the GL-thread soft input natives libunity got, in order (fake_unity.c's seq letters) */
static int wait_seq(const char *want)
{
    for (int t = 0; t < 2000; t += 10) {
        up_poll();
        if (!strcmp(fake_unity_si_seq(), want)) return 1;
        usleep(10000);
    }
    printf("  soft input natives \"%s\", want \"%s\"\n", fake_unity_si_seq(), want);
    return 0;
}
static struct up_soft_input si;
static int wait_active(int on)
{
    for (int t = 0; t < 2000; t += 10) {
        up_poll();
        up_soft_input_get(&si);
        if (si.active == on) return 1;
        usleep(10000);
    }
    return 0;
}
static int text_is(const char *want)            /* the field's text (ASCII want) */
{
    if (si.len != (int)strlen(want)) return 0;
    for (int i = 0; i < si.len; i++) if (si.text[i] != (unsigned char)want[i]) return 0;
    return 1;
}

/* the soft keyboard as each version's dialog reports it: S string, L selection, C closed,
   X canceled, V visible, A area (GL thread, in order); config changed and lost focus counted */
static int show_kb(int op, int type, int sel, int lay)     /* opens it and waits for its reports */
{
    fake_unity_si_reset();
    fake_unity_soft_input(op, type);
    int ok = wait_active(1) && wait_seq(sel ? (lay ? "LVA" : "L") : (lay ? "VA" : ""));
    fake_unity_si_reset();
    return ok;
}
static int hide_kb(int lay)                                /* the game closes it */
{
    fake_unity_si_reset();
    fake_unity_soft_input(3, 0);
    int ok = wait_active(0) && wait_seq(lay ? "AV" : "");
    fake_unity_si_reset();
    return ok;
}
static void soft_input(int shape)
{
    int sel = shape >= SHAPE_2018_4, lay = shape >= SHAPE_2019_4;
    char want[32];
#define SEQ(...) (snprintf(want, sizeof want, __VA_ARGS__), wait_seq(want))
    up_soft_input_set_area(0, 300, 720, 180);
    fake_unity_si_reset();
    int conf = R(R_SI_CONFIG);
    CHECK(up_soft_input_commit("x") == 0);                 /* nothing open: nothing typed */
    fake_unity_soft_input(1, 0);                            /* "ab", default keyboard */
    CHECK(wait_active(1));
    CHECK(text_is("ab") && si.sel_start == 2 && si.sel_end == 2 && !strcmp(si.placeholder, "Name"));
    CHECK(si.input_type == 0x8001 && !si.multiline && !si.secure && si.char_limit == (sel ? 6 : 0));
    CHECK(SEQ("%s%s", sel ? "L" : "", lay ? "VA" : ""));
    if (sel) CHECK(R(R_SI_SEL_START) == 2 && R(R_SI_SEL_LEN) == 0);
    if (lay) CHECK(R(R_SI_VISIBLE) == 1 && R(R_SI_AREA_TOP) == 300 && R(R_SI_AREA_W) == 720);
    CHECK(R(R_SI_CONFIG) == conf + lay);
    fake_unity_soft_input(7, 0);
    CHECK(R(R_SI_LAYOUT) == 1);
    /* typing: a two-byte character, caret moves, backspace */
    fake_unity_si_reset();
    CHECK(up_soft_input_commit("c\xc3\xa9") == 2);
    CHECK(SEQ("S%s", sel ? "L" : ""));
    CHECK(!strcmp(fake_unity_si_str(), "abc\xc3\xa9"));
    if (sel) CHECK(R(R_SI_SEL_START) == 4 && R(R_SI_SEL_LEN) == 0);
    fake_unity_si_reset();
    up_soft_input_select(1, 1);
    up_soft_input_delete(1, 0);
    CHECK(SEQ("%s", sel ? "LSL" : "S"));
    CHECK(!strcmp(fake_unity_si_str(), "bc\xc3\xa9"));
    if (sel) {                                              /* the LengthFilter: 6 UTF-16 units */
        fake_unity_si_reset();
        CHECK(up_soft_input_commit("12345") == 3);
        CHECK(SEQ("SL") && !strcmp(fake_unity_si_str(), "123bc\xc3\xa9"));
        CHECK(R(R_SI_SEL_START) == 3);
    }
    /* the game sets the text, then the selection */
    fake_unity_si_reset();
    fake_unity_soft_input(4, 0);
    CHECK(SEQ("%s", sel ? "SLL" : "S"));
    CHECK(!strcmp(fake_unity_si_str(), "xyz"));
    up_soft_input_get(&si);
    CHECK(text_is("xyz") && si.sel_start == 3);
    if (sel) {
        CHECK(R(R_SI_SEL_START) == 3 && R(R_SI_SEL_LEN) == 0);
        fake_unity_si_reset();
        fake_unity_soft_input(5, 1);
        CHECK(SEQ("L") && R(R_SI_SEL_START) == 1 && R(R_SI_SEL_LEN) == 1);
        up_soft_input_get(&si);
        CHECK(si.sel_start == 1 && si.sel_end == 2);
    }
    /* Done: the caret to 0, the text, closed, then the area and visibility */
    fake_unity_si_reset();
    conf = R(R_SI_CONFIG);
    int closed = R(R_SI_CLOSED);
    up_soft_input_finish(UP_SOFT_INPUT_DONE);
    CHECK(SEQ("%sSC%s", sel ? "L" : "", lay ? "AV" : ""));
    CHECK(!strcmp(fake_unity_si_str(), "xyz") && R(R_SI_CLOSED) == closed + 1);
    if (sel) CHECK(R(R_SI_SEL_START) == 0 && R(R_SI_SEL_LEN) == 0);
    if (lay) CHECK(R(R_SI_VISIBLE) == 0);
    CHECK(R(R_SI_CONFIG) == conf + lay);
    up_soft_input_get(&si);
    CHECK(!si.active && up_soft_input_commit("q") == 0);
    /* number pad (type 4): digits only; Back cancels */
    CHECK(show_kb(1, 4, sel, lay) && si.input_type == 2);
    CHECK(up_soft_input_commit("1a2-.") == 2);
    int canceled = R(R_SI_CANCELED);
    up_soft_input_finish(UP_SOFT_INPUT_CANCEL);
    CHECK(SEQ("S%s%sXC%s", sel ? "L" : "", sel ? "L" : "", lay ? "AV" : ""));
    CHECK(R(R_SI_CANCELED) == canceled + 1 && !strcmp(fake_unity_si_str(), "ab12"));
    /* decimal pad (11): one decimal point; secure numbers (8 + 4) */
    CHECK(show_kb(1, 11, sel, lay) && si.input_type == 0x2002);
    CHECK(up_soft_input_commit("1.2.3") == 4);
    CHECK(SEQ("S%s", sel ? "L" : ""));
    CHECK(hide_kb(lay));
    CHECK(show_kb(8, 4, sel, lay) && si.secure && si.input_type == 0x12);
    CHECK(hide_kb(lay));
    /* multiline: newlines go in; the game hides the keyboard: no close report */
    CHECK(show_kb(2, 0, sel, lay) && si.multiline && si.input_type == 0x28001);
    CHECK(up_soft_input_commit("x\ny") == 3);
    CHECK(SEQ("S%s", sel ? "L" : "") && !strcmp(fake_unity_si_str(), "abx\ny"));
    closed = R(R_SI_CLOSED);
    CHECK(hide_kb(lay) && R(R_SI_CLOSED) == closed);
    /* dismissed (lost focus): closed without the text */
    CHECK(show_kb(1, 0, sel, lay));
    int lost = R(R_SI_LOST);
    up_soft_input_finish(UP_SOFT_INPUT_LOST_FOCUS);
    CHECK(SEQ("C%s", lay ? "AV" : "") && R(R_SI_LOST) == lost + 1);
    up_soft_input_get(&si);
    CHECK(!si.active);
    fake_unity_si_reset();
#undef SEQ
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

/* up_services.c: java.security / javax.crypto against the standard test vectors, Play Billing's
   unavailable answer through a listener, In-App Review's failed task */
static void unhex(const char *h, unsigned char *out, int *n)
{
    *n = 0;
    for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); out[(*n)++] = (unsigned char)v; }
}
static jbyteArray jbytes(JNIEnv *e, const unsigned char *p, int n)
{
    jbyteArray a = (*e)->NewByteArray(e, n);
    (*e)->SetByteArrayRegion(e, a, 0, n, (const jbyte *)p);
    return a;
}
static int same_hex(JNIEnv *e, jbyteArray a, const char *want)
{
    if (!a) return 0;
    char got[300] = "";
    int n = (*e)->GetArrayLength(e, a);
    jbyte *b = (*e)->GetByteArrayElements(e, a, NULL);
    for (int i = 0; i < n && i < 140; i++) sprintf(got + 2 * i, "%02x", (unsigned char)b[i]);
    (*e)->ReleaseByteArrayElements(e, a, b, 0);
    if (strcmp(got, want)) printf("  got %s\n  want %s\n", got, want);
    return !strcmp(got, want);
}
static jbyteArray digest(JNIEnv *e, const char *alg, const char *msg)
{
    jclass c = (*e)->FindClass(e, "java/security/MessageDigest");
    jobject md = (*e)->CallStaticObjectMethod(e, c, (*e)->GetStaticMethodID(e, c, "getInstance", "(Ljava/lang/String;)Ljava/security/MessageDigest;"),
                                              (*e)->NewStringUTF(e, alg));
    if (!md) return NULL;
    (*e)->CallVoidMethod(e, md, (*e)->GetMethodID(e, c, "update", "([B)V"), jbytes(e, (const unsigned char *)msg, (int)strlen(msg)));
    return (*e)->CallObjectMethod(e, md, (*e)->GetMethodID(e, c, "digest", "()[B"));
}
static jbyteArray hmac(JNIEnv *e, const char *alg, const char *key, const char *msg)
{
    jclass c = (*e)->FindClass(e, "javax/crypto/Mac"), kc = (*e)->FindClass(e, "javax/crypto/spec/SecretKeySpec");
    jobject k = (*e)->NewObject(e, kc, (*e)->GetMethodID(e, kc, "<init>", "([BLjava/lang/String;)V"),
                                jbytes(e, (const unsigned char *)key, (int)strlen(key)), (*e)->NewStringUTF(e, alg));
    jobject m = (*e)->CallStaticObjectMethod(e, c, (*e)->GetStaticMethodID(e, c, "getInstance", "(Ljava/lang/String;)Ljavax/crypto/Mac;"),
                                             (*e)->NewStringUTF(e, alg));
    (*e)->CallVoidMethod(e, m, (*e)->GetMethodID(e, c, "init", "(Ljava/security/Key;)V"), k);
    return (*e)->CallObjectMethod(e, m, (*e)->GetMethodID(e, c, "doFinal", "([B)[B"), jbytes(e, (const unsigned char *)msg, (int)strlen(msg)));
}
/* Cipher.getInstance(t).init(mode, key[, iv]).doFinal(in) */
static jbyteArray aes(JNIEnv *e, const char *t, int mode, const char *keyhex, const char *ivhex, const unsigned char *in, int n)
{
    unsigned char kb[32], ivb[16];
    int kn, ivn;
    unhex(keyhex, kb, &kn);
    jclass c = (*e)->FindClass(e, "javax/crypto/Cipher"), kc = (*e)->FindClass(e, "javax/crypto/spec/SecretKeySpec"),
           ic = (*e)->FindClass(e, "javax/crypto/spec/IvParameterSpec");
    jobject k = (*e)->NewObject(e, kc, (*e)->GetMethodID(e, kc, "<init>", "([BLjava/lang/String;)V"), jbytes(e, kb, kn), (*e)->NewStringUTF(e, "AES"));
    jobject ci = (*e)->CallStaticObjectMethod(e, c, (*e)->GetStaticMethodID(e, c, "getInstance", "(Ljava/lang/String;)Ljavax/crypto/Cipher;"),
                                              (*e)->NewStringUTF(e, t));
    if (!ci) return NULL;
    if (ivhex) {
        unhex(ivhex, ivb, &ivn);
        jobject iv = (*e)->NewObject(e, ic, (*e)->GetMethodID(e, ic, "<init>", "([B)V"), jbytes(e, ivb, ivn));
        (*e)->CallVoidMethod(e, ci, (*e)->GetMethodID(e, c, "init", "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V"), mode, k, iv);
    } else (*e)->CallVoidMethod(e, ci, (*e)->GetMethodID(e, c, "init", "(ILjava/security/Key;)V"), mode, k);
    return (*e)->CallObjectMethod(e, ci, (*e)->GetMethodID(e, c, "doFinal", "([B)[B"), jbytes(e, in, n));
}
static volatile int g_billing_code = -1, g_review_done, g_failed;
static jvalue t_on_failure(JNIEnv *env, jobject self, const jvalue *a, void *ud)
{
    (void)env; (void)self; (void)ud;
    g_failed += a[0].l != NULL;
    return fj_v();
}
static jvalue t_on_setup(JNIEnv *env, jobject self, const jvalue *a, void *ud)
{
    (void)self; (void)ud;
    jclass c = (*env)->FindClass(env, "com/android/billingclient/api/BillingResult");
    g_billing_code = (*env)->CallIntMethod(env, a[0].l, (*env)->GetMethodID(env, c, "getResponseCode", "()I"));
    return fj_v();
}
static jvalue t_on_complete(JNIEnv *env, jobject self, const jvalue *a, void *ud)
{
    (void)self; (void)ud;
    jclass c = (*env)->FindClass(env, "com/google/android/play/core/tasks/Task");
    g_review_done = 1 + !(*env)->CallBooleanMethod(env, a[0].l, (*env)->GetMethodID(env, c, "isSuccessful", "()Z"));
    return fj_v();
}
static void test_services(void)
{
    JNIEnv *e = fj_env();
    (*e)->PushLocalFrame(e, 64);
    CHECK(same_hex(e, digest(e, "MD5", "abc"), "900150983cd24fb0d6963f7d28e17f72"));
    CHECK(same_hex(e, digest(e, "SHA-1", "abc"), "a9993e364706816aba3e25717850c26c9cd0d89d"));
    CHECK(same_hex(e, digest(e, "SHA-256", "abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(same_hex(e, digest(e, "SHA-256", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                   "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK(digest(e, "SHA-999", "abc") == NULL && (*e)->ExceptionCheck(e));          /* NoSuchAlgorithmException */
    (*e)->ExceptionClear(e);
    CHECK(same_hex(e, hmac(e, "HmacMD5", "Jefe", "what do ya want for nothing?"), "750c783e6ab0b503eaa86e310a5db738"));
    CHECK(same_hex(e, hmac(e, "HmacSHA1", "Jefe", "what do ya want for nothing?"), "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
    CHECK(same_hex(e, hmac(e, "HmacSHA256", "Jefe", "what do ya want for nothing?"),
                   "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    unsigned char pt[64];
    int n;
    unhex("00112233445566778899aabbccddeeff", pt, &n);                     /* FIPS-197 C.1, C.3 */
    CHECK(same_hex(e, aes(e, "AES/ECB/NoPadding", 1, "000102030405060708090a0b0c0d0e0f", NULL, pt, n), "69c4e0d86a7b0430d8cdb78070b4c55a"));
    CHECK(same_hex(e, aes(e, "AES/ECB/NoPadding", 1, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", NULL, pt, n),
                   "8ea2b7ca516745bfeafc49904b496089"));
    unhex("69c4e0d86a7b0430d8cdb78070b4c55a", pt, &n);
    CHECK(same_hex(e, aes(e, "AES/ECB/NoPadding", 2, "000102030405060708090a0b0c0d0e0f", NULL, pt, n), "00112233445566778899aabbccddeeff"));
    unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", pt, &n);   /* SP 800-38A F.2.1, F.5.1 */
    CHECK(same_hex(e, aes(e, "AES/CBC/NoPadding", 1, "2b7e151628aed2a6abf7158809cf4f3c", "000102030405060708090a0b0c0d0e0f", pt, n),
                   "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"));
    CHECK(same_hex(e, aes(e, "AES/CTR/NoPadding", 1, "2b7e151628aed2a6abf7158809cf4f3c", "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", pt, n),
                   "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"));
    const unsigned char msg[] = "twenty bytes of text";                    /* PKCS5: two blocks there and back */
    jbyteArray enc = aes(e, "AES/CBC/PKCS5Padding", 1, "2b7e151628aed2a6abf7158809cf4f3c", "000102030405060708090a0b0c0d0e0f", msg, 20);
    CHECK(enc && (*e)->GetArrayLength(e, enc) == 32);
    if (enc) {
        unsigned char eb[32];
        (*e)->GetByteArrayRegion(e, enc, 0, 32, (jbyte *)eb);
        jbyteArray dec = aes(e, "AES/CBC/PKCS5Padding", 2, "2b7e151628aed2a6abf7158809cf4f3c", "000102030405060708090a0b0c0d0e0f", eb, 32);
        CHECK(same_hex(e, dec, "7477656e7479206279746573206f662074657874"));
        eb[31] ^= 1;                                                       /* a broken pad: BadPaddingException */
        CHECK(aes(e, "AES/CBC/PKCS5Padding", 2, "2b7e151628aed2a6abf7158809cf4f3c", "000102030405060708090a0b0c0d0e0f", eb, 32) == NULL &&
              (*e)->ExceptionCheck(e));
        (*e)->ExceptionClear(e);
    }
    jclass sr = (*e)->FindClass(e, "java/security/SecureRandom");
    jobject r = (*e)->NewObject(e, sr, (*e)->GetMethodID(e, sr, "<init>", "()V"));
    jbyteArray rb = (*e)->NewByteArray(e, 32), rb2 = (*e)->NewByteArray(e, 32);
    (*e)->CallVoidMethod(e, r, (*e)->GetMethodID(e, sr, "nextBytes", "([B)V"), rb);
    (*e)->CallVoidMethod(e, r, (*e)->GetMethodID(e, sr, "nextBytes", "([B)V"), rb2);
    unsigned char x[32], y[32];
    (*e)->GetByteArrayRegion(e, rb, 0, 32, (jbyte *)x);
    (*e)->GetByteArrayRegion(e, rb2, 0, 32, (jbyte *)y);
    CHECK(memcmp(x, y, 32) != 0);

    /* Play Billing: a client made as Unity IAP 4 does, connected; the listener hears BILLING_UNAVAILABLE */
    fj_class_define("test/SetupListener", NULL);
    fj_method_define("test/SetupListener", "onBillingSetupFinished", "(Lcom/android/billingclient/api/BillingResult;)V", 0, t_on_setup, NULL);
    jclass bc = (*e)->FindClass(e, "com/android/billingclient/api/BillingClient"), bb = (*e)->FindClass(e, "com/android/billingclient/api/BillingClient$Builder");
    jobject b = (*e)->CallStaticObjectMethod(e, bc, (*e)->GetStaticMethodID(e, bc, "newBuilder", "(Landroid/content/Context;)Lcom/android/billingclient/api/BillingClient$Builder;"), NULL);
    b = (*e)->CallObjectMethod(e, b, (*e)->GetMethodID(e, bb, "enablePendingPurchases", "()Lcom/android/billingclient/api/BillingClient$Builder;"));
    jobject client = (*e)->CallObjectMethod(e, b, (*e)->GetMethodID(e, bb, "build", "()Lcom/android/billingclient/api/BillingClient;"));
    CHECK(client && !(*e)->CallBooleanMethod(e, client, (*e)->GetMethodID(e, bc, "isReady", "()Z")));
    jobject l = fj_new_by_name("test/SetupListener", NULL, NULL);
    (*e)->CallVoidMethod(e, client, (*e)->GetMethodID(e, bc, "startConnection", "(Lcom/android/billingclient/api/BillingClientStateListener;)V"), l);
    for (int i = 0; i < 200 && g_billing_code < 0; i++) { up_poll(); usleep(10000); }   /* on the UI thread (this one) */
    CHECK(g_billing_code == 3);

    /* In-App Review: the flow's task has failed; an OnCompleteListener hears it at once */
    fj_class_define("test/CompleteListener", NULL);
    fj_method_define("test/CompleteListener", "onComplete", "(Lcom/google/android/play/core/tasks/Task;)V", 0, t_on_complete, NULL);
    jclass rf = (*e)->FindClass(e, "com/google/android/play/core/review/ReviewManagerFactory"),
           rm = (*e)->FindClass(e, "com/google/android/play/core/review/ReviewManager"), tk = (*e)->FindClass(e, "com/google/android/play/core/tasks/Task");
    jobject mgr = (*e)->CallStaticObjectMethod(e, rf, (*e)->GetStaticMethodID(e, rf, "create", "(Landroid/content/Context;)Lcom/google/android/play/core/review/ReviewManager;"), NULL);
    jobject task = (*e)->CallObjectMethod(e, mgr, (*e)->GetMethodID(e, rm, "requestReviewFlow", "()Lcom/google/android/play/core/tasks/Task;"));
    (*e)->CallObjectMethod(e, task, (*e)->GetMethodID(e, tk, "addOnCompleteListener",
                           "(Lcom/google/android/play/core/tasks/OnCompleteListener;)Lcom/google/android/play/core/tasks/Task;"),
                           fj_new_by_name("test/CompleteListener", NULL, NULL));
    CHECK(g_review_done == 2);                                              /* complete, not successful */

    /* Mobile Notifications: Initialize's calls get a manager (blocked for the app, the device's API level, no
       channels); a builder keeps the extras the C# puts */
    jclass um = (*e)->FindClass(e, "com/unity/androidnotifications/UnityNotificationManager"), nb = (*e)->FindClass(e, "android/app/Notification$Builder"),
           bu = (*e)->FindClass(e, "android/os/Bundle");
    jobject nm = (*e)->CallStaticObjectMethod(e, um, (*e)->GetStaticMethodID(e, um, "getNotificationManagerImpl",
                 "(Landroid/app/Activity;Lcom/unity/androidnotifications/NotificationCallback;)Lcom/unity/androidnotifications/UnityNotificationManager;"), NULL, NULL);
    CHECK(nm && (*e)->CallIntMethod(e, nm, (*e)->GetMethodID(e, um, "getTargetSdk", "()I")) == 23);
    CHECK(nm && (*e)->CallIntMethod(e, nm, (*e)->GetMethodID(e, um, "areNotificationsEnabled", "()I")) == 5);
    jobject chans = nm ? (*e)->CallObjectMethod(e, nm, (*e)->GetMethodID(e, um, "getNotificationChannels", "()[Lcom/unity/androidnotifications/NotificationChannelWrapper;")) : NULL;
    CHECK(chans && (*e)->GetArrayLength(e, chans) == 0);
    jobject bld = nm ? (*e)->CallObjectMethod(e, nm, (*e)->GetMethodID(e, um, "createNotificationBuilder", "(Ljava/lang/String;)Landroid/app/Notification$Builder;"),
                                             (*e)->NewStringUTF(e, "default")) : NULL;
    CHECK(bld && (*e)->IsSameObject(e, bld, (*e)->CallObjectMethod(e, bld, (*e)->GetMethodID(e, nb, "setAutoCancel", "(Z)Landroid/app/Notification$Builder;"), 1)));
    jobject ex = bld ? (*e)->CallObjectMethod(e, bld, (*e)->GetMethodID(e, nb, "getExtras", "()Landroid/os/Bundle;")) : NULL;
    if (ex) (*e)->CallVoidMethod(e, ex, (*e)->GetMethodID(e, bu, "putInt", "(Ljava/lang/String;I)V"), (*e)->NewStringUTF(e, "id"), 7);
    CHECK(ex && (*e)->CallIntMethod(e, ex, (*e)->GetMethodID(e, bu, "getInt", "(Ljava/lang/String;)I"), (*e)->NewStringUTF(e, "id")) == 7);
    /* Play Games v2: the sign-in task fails; an OnFailureListener hears the exception */
    fj_class_define("test/FailureListener", NULL);
    fj_method_define("test/FailureListener", "onFailure", "(Ljava/lang/Exception;)V", 0, t_on_failure, NULL);
    jclass pg = (*e)->FindClass(e, "com/google/android/gms/games/PlayGames"), sc = (*e)->FindClass(e, "com/google/android/gms/games/GamesSignInClient"),
           gt = (*e)->FindClass(e, "com/google/android/gms/tasks/Task");
    jobject sic = (*e)->CallStaticObjectMethod(e, pg, (*e)->GetStaticMethodID(e, pg, "getGamesSignInClient", "(Landroid/app/Activity;)Lcom/google/android/gms/games/GamesSignInClient;"), NULL);
    jobject at = sic ? (*e)->CallObjectMethod(e, sic, (*e)->GetMethodID(e, sc, "isAuthenticated", "()Lcom/google/android/gms/tasks/Task;")) : NULL;
    if (at) (*e)->CallObjectMethod(e, at, (*e)->GetMethodID(e, gt, "addOnFailureListener", "(Lcom/google/android/gms/tasks/OnFailureListener;)Lcom/google/android/gms/tasks/Task;"),
                                   fj_new_by_name("test/FailureListener", NULL, NULL));
    CHECK(at && g_failed == 1);
    (*e)->PopLocalFrame(e, NULL);
}

static int run_shape(int shape)
{
    setenv("UA_PREFS_DEFAULTS", "particles=0 bad =3", 1);   /* (a malformed entry is skipped) */
    printf("== Unity %s shape%s\n", shape_name[shape], no_libmain ? " without libmain" : "");
    fake_unity_set_shape(shape);
    if (system("rm -rf /tmp/uptest && mkdir -p /tmp/uptest/lib && touch /tmp/uptest/lib/libil2cpp.so") != 0)
        printf("  (could not reset /tmp/uptest)\n");
    write_obb("/tmp/uptest/main.1.com.test.app.obb");
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
    cfg.libmain_onload = no_libmain ? NULL : fake_libmain_onload;
    cfg.libunity_onload = fake_libunity_onload;
    cfg.on_frame = on_frame;
    cfg.on_audio = on_audio;
    cfg.on_quit = on_quit;
    cfg.on_load_library = on_load_library;

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
    if (shape == SHAPE_2021_3 && !no_libmain) test_services();
    CHECK(R(R_ONLOAD) == 1);
    CHECK(R(R_INITJNI) == 1);
    CHECK(R(R_WEBREQ) == (shape <= SHAPE_2018_4));
    CHECK(R(R_WEBREQ_ERR) == (shape <= SHAPE_2018_4 ? 7 : 0));   /* unknown host */
    CHECK(R(R_PATH_OK) == 1);
    CHECK(R(R_MODEL_OK) == 1);
    CHECK(R(R_SDK) == 23);
    CHECK(R(R_WIDTH) == 720 && R(R_HEIGHT) == 480);
    CHECK(R(R_REFRESH_X100) == 5994);
    CHECK(R(R_LOCALE_OK) == 1);
    CHECK(R(R_PREFS_FLOAT_X100) == 75);
    CHECK(R(R_PREFS_STR_OK) == 1);
    CHECK(R(R_PREFS_DEFAULT) == 0);
    CHECK(R(R_AUDIO_RATE) == 48000);
    CHECK(R(R_CALLBACKS_OK) == 1);
    CHECK(R(R_SWAPPY_JAVA) == 1);
    if (shape >= SHAPE_2019_4) CHECK(R(R_SWAPPY_MODES) == 1);
    CHECK(R(R_IL2CPP_OK) == 1);
    CHECK(access("/tmp/uptest/data/files/il2cpp", W_OK) == 0);
    CHECK(R(R_ORIENT_LOCK) == (shape >= SHAPE_2020_3 ? 100 : 0));
    CHECK(R(R_NOWINDOW) == (shape >= SHAPE_2022_3 ? 1 : 0));
    CHECK(R(R_U6_INITJNI) == (shape >= SHAPE_6000_6));
    CHECK(R(R_SWAPPY_DISPLAY) == 1);
    CHECK(R(R_OBB_GUID_OK) == 1);
    CHECK(R(R_LOADLIB_OK) == 1 && loadlib_bad == 0);

    /* render loop */
    CHECK(wait_for(R_RENDER, 10, 5000));
    /* one posted frame, one call, at the next vsync on Swappy's own looper thread */
    if (shape >= SHAPE_2019_4) CHECK(wait_for(R_SWAPPY_FRAME, 1, 3000) && R(R_SWAPPY_FRAME) == 1);
    CHECK(R(R_GFX) == 1 && R(R_FOCUS_TRUE) == 1 && R(R_RESUME) == 1);
    CHECK(R(R_U6_JAVA) == (shape >= SHAPE_6000_6));
    if (shape >= SHAPE_6000_6) {         /* low memory: nativeMemoryUsageChanged(Critical) */
        up_low_memory();
        CHECK(wait_for(R_U6_MEMORY, 4, 2000));
    }
    CHECK(R(R_SURF_CHANGED) == later);
    CHECK(R(R_ORDER_OK) == 1);
    CHECK(wait_for(R_DOFRAME, 3, 3000));
    CHECK(wait_for(R_HANDLE_MSG, 42, 3000));
    CHECK(wait_for(R_FMOD_PROCESS, 3, 3000));
    if (shape >= SHAPE_2021_3) CHECK(wait_for(R_VSYNC, 5, 3000) && wait_for(R_VSYNC_WAITED, 3, 3000));
    else CHECK(R(R_VSYNC) == 0);
    for (int t = 0; t < 300 && frames_cb < 10; t++) usleep(10000);   /* the 10th frame may still wait for its vsync */
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
    /* a mouse (host pointer mode): hover, press, wheel */
    struct up_input_device mouse = { .id = 9, .name = "Test Mouse", .descriptor = "test-mouse", .sources = UP_SOURCE_MOUSE,
                                     .naxes = 2, .axis = { UP_AXIS_X, UP_AXIS_Y }, .max = { 639, 479 } };
    up_add_input_device(&mouse);
    CHECK(up_inject_mouse(9, UP_ACTION_HOVER_MOVE, 0, 100, 50, 0, 0) == 1);
    CHECK(R(R_MOTION_SRC) == UP_SOURCE_MOUSE && R(R_MOTION_ACTION) == UP_ACTION_HOVER_MOVE && R(R_MOTION_TOOL) == 3 &&
          R(R_MOTION_BUTTONS) == 0 && R(R_MOTION_PRESSED) == 0 && R(R_AXIS_X_X1000) == 100000 && R(R_MOUSE_DEV_OK) == 1);
    up_inject_mouse(9, UP_ACTION_DOWN, UP_BUTTON_PRIMARY, 100, 50, 0, 0);
    CHECK(R(R_MOTION_ACTION) == UP_ACTION_DOWN && R(R_MOTION_BUTTONS) == UP_BUTTON_PRIMARY && R(R_MOTION_PRESSED) == 1);
    up_inject_mouse(9, UP_ACTION_UP, 0, 100, 50, 0, 0);
    CHECK(R(R_MOTION_ACTION) == UP_ACTION_UP && R(R_MOTION_BUTTONS) == 0);
    up_inject_mouse(9, UP_ACTION_SCROLL, 0, 100, 50, 1, 0);
    CHECK(R(R_MOTION_ACTION) == UP_ACTION_SCROLL && R(R_VSCROLL_X1000) == 1000);
    up_send_message("GameManager", "OnHello", "w\xc3\xb6rld \xe2\x9c\x93");
    CHECK(R(R_SENDMSG) == 1);
    soft_input(shape);

    /* resize: surfaceChanged */
    up_resize(800, 600);
    CHECK(wait_for(R_GFX, 2, 2000));
    if (later) {
        CHECK(wait_for(R_SURF_CHANGED, 2, 2000));
        CHECK(R(R_SURF_W) == 800 && R(R_SURF_H) == 600);
    }

    /* lifecycle: pause / resume / surface loss */
    int canceled = R(R_SI_CANCELED), closed = R(R_SI_CLOSED);
    up_pause();
    CHECK(R(R_PAUSE) == 1);
    CHECK(wait_seq(shape >= SHAPE_2019_4 ? "XCAV" : "XC"));        /* reportSoftInputStr(null, 1, true) */
    CHECK(R(R_SI_CANCELED) == canceled + 1 && R(R_SI_CLOSED) == closed + 1);
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

/* the battery from a made-up /sys/class/power_supply */
static void put(const char *path, const char *v)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(v, f); fclose(f); }
}
static void test_battery(void)
{
    printf("== battery\n");
    const char *r = "/tmp/uptest-power";
    if (system("rm -rf /tmp/uptest-power && mkdir -p /tmp/uptest-power/axp-battery /tmp/uptest-power/axp-usb "
               "/tmp/uptest-power/ac") != 0) { fails++; return; }
    struct up_battery b;
    up_battery_read("/tmp/uptest-power-none", &b);           /* no such folder (a PC): full, as before */
    CHECK(b.present == 1 && b.level == 100 && b.status == 5 && b.plugged == 0 && b.health == 2);
    up_battery_read(r, &b);                                  /* only empty supplies: still full */
    CHECK(b.level == 100 && b.status == 5);
    put("/tmp/uptest-power/axp-battery/type", "Battery\n");
    put("/tmp/uptest-power/axp-battery/capacity", "37\n");
    put("/tmp/uptest-power/axp-battery/status", "Discharging\n");
    put("/tmp/uptest-power/axp-battery/health", "Good\n");
    put("/tmp/uptest-power/axp-battery/temp", "312\n");
    put("/tmp/uptest-power/axp-battery/voltage_now", "3815000\n");
    put("/tmp/uptest-power/axp-usb/type", "USB\n");
    put("/tmp/uptest-power/axp-usb/online", "0\n");
    put("/tmp/uptest-power/ac/type", "Mains\n");
    put("/tmp/uptest-power/ac/online", "0\n");
    up_battery_read(r, &b);
    CHECK(b.present == 1 && b.level == 37 && b.status == 3 && b.plugged == 0 && b.health == 2);
    CHECK(b.temperature == 312 && b.voltage_mv == 3815);
    put("/tmp/uptest-power/axp-battery/status", "Charging");
    put("/tmp/uptest-power/axp-usb/online", "1");
    up_battery_read(r, &b);
    CHECK(b.status == 2 && b.plugged == 2);                  /* USB */
    put("/tmp/uptest-power/ac/online", "1");
    up_battery_read(r, &b);
    CHECK(b.plugged == 1);                                   /* AC wins over USB */
    put("/tmp/uptest-power/axp-battery/capacity", "140");
    put("/tmp/uptest-power/axp-battery/status", "Full");
    up_battery_read(r, &b);
    CHECK(b.level == 100 && b.status == 5);
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
    test_battery();
    if (fails) { printf("  FAIL battery\n"); failed++; fails = 0; }
    for (int i = 0; i <= SHAPE_N; i++) {
        int s = i < SHAPE_N ? i : SHAPE_N - 1;
        fflush(stdout);
        pid_t p = fork();
        if (p == 0) { no_libmain = i == SHAPE_N; int r = run_shape(s); fflush(stdout); _exit(r); }
        int st = 0;
        if (p < 0 || waitpid(p, &st, 0) != p || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            printf("  FAIL Unity %s shape%s (status 0x%x)\n", shape_name[s], i == SHAPE_N ? " without libmain" : "", st);
            failed++;
        }
    }
    printf("%s: %d failure(s) in %d Unity version shapes (and the newest without libmain)\n", failed ? "FAILED" : "PASSED", failed, SHAPE_N);
    return failed ? 1 : 0;
}
