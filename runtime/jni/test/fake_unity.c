/*
 * A fake libmain + libunity for test_unityplayer: built softfp/freestanding like NDK code. It
 * registers the same natives as the real libraries (spec/natives.tsv) and, from them, uses the
 * Java APIs the real libunity binds (spec/jni_bindings_libunity.tsv): paths, display metrics,
 * Build fields, SharedPreferences, Choreographer + JNIBridge proxies, Handler/Message,
 * FMODAudioDevice, KeyEvent/MotionEvent/InputDevice.
 */
#include "../include/jni.h"

int printf(const char *, ...);
int strcmp(const char *, const char *);

/* results, read by the host through fake_unity_get(); integers only (ABI-neutral) */
enum { R_ONLOAD, R_INITJNI, R_RESUME, R_PAUSE, R_DONE, R_FOCUS_TRUE, R_FOCUS_FALSE, R_GFX, R_GFX_NULL,
       R_RENDER, R_WIDTH, R_HEIGHT, R_REFRESH_X100, R_SDK, R_MODEL_OK, R_PATH_OK, R_PREFS_FLOAT_X100,
       R_PREFS_STR_OK, R_DOFRAME, R_HANDLE_MSG, R_FMOD_PROCESS, R_KEY_CODE, R_KEY_ACTION, R_KEY_DEV_OK,
       R_AXIS_X_X1000, R_AXIS_HAT_X1000, R_MOTION_SRC, R_RANGES, R_AUDIO_RATE, R_WEBREQ, R_SENDMSG,
       R_ORDER_OK, R_LOCALE_OK, R_ERRORS, R_N };
static volatile int R[R_N];
int fake_unity_get(int i) { return (i >= 0 && i < R_N) ? R[i] : -1; }

static JavaVM *g_vm;
static jobject g_context, g_choreo_cb, g_handler;
static jmethodID m_doFrame, m_handleMessage, m_postFrame;
static jclass c_choreo;

#define FAIL(...) do { printf("  [fake-unity] " __VA_ARGS__); printf("\n"); R[R_ERRORS]++; } while (0)

static int str_eq(JNIEnv *env, jobject s, const char *want)
{
    if (!s) return 0;
    const char *c = (*env)->GetStringUTFChars(env, (jstring)s, 0);
    int ok = !strcmp(c, want);
    if (!ok) printf("  [fake-unity] got \"%s\" want \"%s\"\n", c, want);
    (*env)->ReleaseStringUTFChars(env, (jstring)s, c);
    return ok;
}

static jobject call_obj(JNIEnv *env, jobject o, const char *n, const char *sig)
{
    jclass c = (*env)->GetObjectClass(env, o);
    jmethodID m = (*env)->GetMethodID(env, c, n, sig);
    return (*env)->CallObjectMethod(env, o, m);
}

/* ---------------- UnityPlayer natives ---------------- */
static void initJni(JNIEnv *env, jobject player, jobject context)
{
    R[R_INITJNI]++;
    g_context = (*env)->NewGlobalRef(env, context);
    (*env)->PushLocalFrame(env, 64);

    jobject apk = call_obj(env, context, "getPackageCodePath", "()Ljava/lang/String;");
    jobject files = call_obj(env, context, "getFilesDir", "()Ljava/io/File;");
    jobject fpath = call_obj(env, files, "getAbsolutePath", "()Ljava/lang/String;");
    R[R_PATH_OK] = str_eq(env, apk, "/tmp/uptest/base.apk") && str_eq(env, fpath, "/tmp/uptest/data/files");

    jclass build = (*env)->FindClass(env, "android/os/Build");
    jfieldID fmodel = (*env)->GetStaticFieldID(env, build, "MODEL", "Ljava/lang/String;");
    R[R_MODEL_OK] = str_eq(env, (*env)->GetStaticObjectField(env, build, fmodel), "TestModel");
    jclass ver = (*env)->FindClass(env, "android/os/Build$VERSION");
    R[R_SDK] = (*env)->GetStaticIntField(env, ver, (*env)->GetStaticFieldID(env, ver, "SDK_INT", "I"));

    /* display: getSystemService(WINDOW_SERVICE).getDefaultDisplay().getMetrics(new DisplayMetrics) */
    jclass ctx = (*env)->FindClass(env, "android/content/Context");
    jobject ws = (*env)->GetStaticObjectField(env, ctx, (*env)->GetStaticFieldID(env, ctx, "WINDOW_SERVICE", "Ljava/lang/String;"));
    jmethodID gss = (*env)->GetMethodID(env, ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject wm = (*env)->CallObjectMethod(env, context, gss, ws);
    jclass wmc = (*env)->FindClass(env, "android/view/WindowManager");
    jobject disp = (*env)->CallObjectMethod(env, wm, (*env)->GetMethodID(env, wmc, "getDefaultDisplay", "()Landroid/view/Display;"));
    jclass dmc = (*env)->FindClass(env, "android/util/DisplayMetrics");
    jobject dm = (*env)->NewObject(env, dmc, (*env)->GetMethodID(env, dmc, "<init>", "()V"));
    jclass dc = (*env)->FindClass(env, "android/view/Display");
    (*env)->CallVoidMethod(env, disp, (*env)->GetMethodID(env, dc, "getMetrics", "(Landroid/util/DisplayMetrics;)V"), dm);
    R[R_WIDTH] = (*env)->GetIntField(env, dm, (*env)->GetFieldID(env, dmc, "widthPixels", "I"));
    R[R_HEIGHT] = (*env)->GetIntField(env, dm, (*env)->GetFieldID(env, dmc, "heightPixels", "I"));
    jfloat rr = (*env)->CallFloatMethod(env, disp, (*env)->GetMethodID(env, dc, "getRefreshRate", "()F"));
    R[R_REFRESH_X100] = (int)(rr * 100.0f + 0.5f);

    /* locale */
    jclass loc = (*env)->FindClass(env, "java/util/Locale");
    jobject l = (*env)->CallStaticObjectMethod(env, loc, (*env)->GetStaticMethodID(env, loc, "getDefault", "()Ljava/util/Locale;"));
    R[R_LOCALE_OK] = str_eq(env, (*env)->CallObjectMethod(env, l, (*env)->GetMethodID(env, loc, "getLanguage", "()Ljava/lang/String;")), "en")
                  && str_eq(env, (*env)->CallObjectMethod(env, l, (*env)->GetMethodID(env, loc, "getCountry", "()Ljava/lang/String;")), "US");

    /* PlayerPrefs: SharedPreferences round trip (floats through the softfp paths) */
    jstring pname = (*env)->NewStringUTF(env, "com.test.app.v2.playerprefs");
    jobject sp = (*env)->CallObjectMethod(env, context,
        (*env)->GetMethodID(env, ctx, "getSharedPreferences", "(Ljava/lang/String;I)Landroid/content/SharedPreferences;"), pname, (jint)0);
    jclass spc = (*env)->FindClass(env, "android/content/SharedPreferences");
    jclass edc = (*env)->FindClass(env, "android/content/SharedPreferences$Editor");
    jstring kf = (*env)->NewStringUTF(env, "volume"), ks = (*env)->NewStringUTF(env, "name");
    jfloat prev = (*env)->CallFloatMethod(env, sp, (*env)->GetMethodID(env, spc, "getFloat", "(Ljava/lang/String;F)F"), kf, (jfloat)-1.0f);
    if (prev != -1.0f && prev != 0.75f) FAIL("unexpected stored float");
    jobject ed = (*env)->CallObjectMethod(env, sp, (*env)->GetMethodID(env, spc, "edit", "()Landroid/content/SharedPreferences$Editor;"));
    (*env)->CallObjectMethod(env, ed, (*env)->GetMethodID(env, edc, "putFloat", "(Ljava/lang/String;F)Landroid/content/SharedPreferences$Editor;"), kf, (jfloat)0.75f);
    (*env)->CallObjectMethod(env, ed, (*env)->GetMethodID(env, edc, "putString", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;"),
                            ks, (*env)->NewStringUTF(env, "cup%20head"));
    (*env)->CallBooleanMethod(env, ed, (*env)->GetMethodID(env, edc, "commit", "()Z"));
    jfloat now = (*env)->CallFloatMethod(env, sp, (*env)->GetMethodID(env, spc, "getFloat", "(Ljava/lang/String;F)F"), kf, (jfloat)-1.0f);
    R[R_PREFS_FLOAT_X100] = (int)(now * 100.0f + 0.5f);
    R[R_PREFS_STR_OK] = str_eq(env, (*env)->CallObjectMethod(env, sp,
        (*env)->GetMethodID(env, spc, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"), ks, (jobject)0), "cup%20head");

    /* FMOD inside libunity */
    jclass as = (*env)->FindClass(env, "android/media/AudioSystem");
    R[R_AUDIO_RATE] = (*env)->CallStaticIntMethod(env, as, (*env)->GetStaticMethodID(env, as, "getPrimaryOutputSamplingRate", "()I"));

    (*env)->PopLocalFrame(env, 0);
}

static void nativeInitWebRequest(JNIEnv *env, jobject player, jclass cls) { (void)player; R[R_WEBREQ] = cls != 0; }

static void nativeRecreateGfxState(JNIEnv *env, jobject player, jint which, jobject surface)
{
    if (surface) R[R_GFX]++; else R[R_GFX_NULL]++;
    if (!R[R_INITJNI]) FAIL("RecreateGfxState before initJni");
}

static void nativeFocusChanged(JNIEnv *env, jobject player, jboolean f) { if (f) R[R_FOCUS_TRUE]++; else R[R_FOCUS_FALSE]++; }

static void nativeResume(JNIEnv *env, jobject player)
{
    R[R_RESUME]++;
    if (R[R_RESUME] > 1) return;
    /* order on UnityMain: RecreateGfxState(surface), FocusChanged(true), Resume, then Render */
    R[R_ORDER_OK] = R[R_GFX] == 1 && R[R_FOCUS_TRUE] == 1 && R[R_RENDER] == 0;

    /* Choreographer + JNIBridge proxy (Unity's VSYNC timing) */
    jclass bridge = (*env)->FindClass(env, "bitter/jnibridge/JNIBridge");
    jmethodID np = (*env)->GetStaticMethodID(env, bridge, "newInterfaceProxy", "(J[Ljava/lang/Class;)Ljava/lang/Object;");
    jclass fcb = (*env)->FindClass(env, "android/view/Choreographer$FrameCallback");
    m_doFrame = (*env)->GetMethodID(env, fcb, "doFrame", "(J)V");
    jclass clsc = (*env)->FindClass(env, "java/lang/Class");
    jobjectArray ifs = (*env)->NewObjectArray(env, 1, clsc, fcb);
    g_choreo_cb = (*env)->NewGlobalRef(env, (*env)->CallStaticObjectMethod(env, bridge, np, (jlong)0x1111, ifs));
    c_choreo = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "android/view/Choreographer"));
    jobject ch = (*env)->CallStaticObjectMethod(env, c_choreo, (*env)->GetStaticMethodID(env, c_choreo, "getInstance", "()Landroid/view/Choreographer;"));
    m_postFrame = (*env)->GetMethodID(env, c_choreo, "postFrameCallback", "(Landroid/view/Choreographer$FrameCallback;)V");
    (*env)->CallVoidMethod(env, ch, m_postFrame, g_choreo_cb);

    /* Handler(Looper.myLooper(), proxy Callback).obtainMessage(42).sendToTarget() */
    jclass hcb = (*env)->FindClass(env, "android/os/Handler$Callback");
    m_handleMessage = (*env)->GetMethodID(env, hcb, "handleMessage", "(Landroid/os/Message;)Z");
    jobjectArray ifs2 = (*env)->NewObjectArray(env, 1, clsc, hcb);
    jobject cbp = (*env)->CallStaticObjectMethod(env, bridge, np, (jlong)0x2222, ifs2);
    jclass lpc = (*env)->FindClass(env, "android/os/Looper");
    jobject lp = (*env)->CallStaticObjectMethod(env, lpc, (*env)->GetStaticMethodID(env, lpc, "myLooper", "()Landroid/os/Looper;"));
    if (!lp) FAIL("no Looper on UnityMain");
    jclass hc = (*env)->FindClass(env, "android/os/Handler");
    g_handler = (*env)->NewGlobalRef(env, (*env)->NewObject(env, hc,
        (*env)->GetMethodID(env, hc, "<init>", "(Landroid/os/Looper;Landroid/os/Handler$Callback;)V"), lp, cbp));
    jobject msg = (*env)->CallObjectMethod(env, g_handler, (*env)->GetMethodID(env, hc, "obtainMessage", "(I)Landroid/os/Message;"), (jint)42);
    jclass mc = (*env)->FindClass(env, "android/os/Message");
    (*env)->CallVoidMethod(env, msg, (*env)->GetMethodID(env, mc, "sendToTarget", "()V"));

    /* FMOD Java output device */
    jclass fd = (*env)->FindClass(env, "org/fmod/FMODAudioDevice");
    jobject dev = (*env)->NewObject(env, fd, (*env)->GetMethodID(env, fd, "<init>", "()V"));
    (*env)->CallVoidMethod(env, dev, (*env)->GetMethodID(env, fd, "start", "()V"));
}

static jboolean nativePause(JNIEnv *env, jobject player) { R[R_PAUSE]++; return JNI_FALSE; }
static void nativeDone(JNIEnv *env, jobject player) { R[R_DONE]++; }
static void nativeLowMemory(JNIEnv *env, jobject player) { }
static void nativeRestart(JNIEnv *env, jobject player) { }
static void nativeSoftInput(JNIEnv *env, jobject player) { }

static jboolean nativeRender(JNIEnv *env, jobject player)
{
    R[R_RENDER]++;
    if (R[R_RESUME] == 0) FAIL("render before resume");
    return JNI_TRUE;
}

static jboolean nativeInjectEvent(JNIEnv *env, jobject player, jobject ev)
{
    jclass kc = (*env)->FindClass(env, "android/view/KeyEvent");
    jclass mc = (*env)->FindClass(env, "android/view/MotionEvent");
    jclass ie = (*env)->FindClass(env, "android/view/InputEvent");
    jint src = (*env)->CallIntMethod(env, ev, (*env)->GetMethodID(env, ie, "getSource", "()I"));
    jint dev;
    if ((*env)->IsInstanceOf(env, ev, kc)) {
        R[R_KEY_CODE] = (*env)->CallIntMethod(env, ev, (*env)->GetMethodID(env, kc, "getKeyCode", "()I"));
        R[R_KEY_ACTION] = (*env)->CallIntMethod(env, ev, (*env)->GetMethodID(env, kc, "getAction", "()I"));
        dev = (*env)->CallIntMethod(env, ev, (*env)->GetMethodID(env, kc, "getDeviceId", "()I"));
        jclass idc = (*env)->FindClass(env, "android/view/InputDevice");
        jobject d = (*env)->CallStaticObjectMethod(env, idc, (*env)->GetStaticMethodID(env, idc, "getDevice", "(I)Landroid/view/InputDevice;"), dev);
        R[R_KEY_DEV_OK] = d && str_eq(env, (*env)->CallObjectMethod(env, d, (*env)->GetMethodID(env, idc, "getName", "()Ljava/lang/String;")), "Test Pad");
        jobject ranges = (*env)->CallObjectMethod(env, d, (*env)->GetMethodID(env, idc, "getMotionRanges", "()Ljava/util/List;"));
        jclass lc = (*env)->FindClass(env, "java/util/List");
        R[R_RANGES] = (*env)->CallIntMethod(env, ranges, (*env)->GetMethodID(env, lc, "size", "()I"));
        return JNI_TRUE;
    }
    if ((*env)->IsInstanceOf(env, ev, mc)) {
        R[R_MOTION_SRC] = src;
        jmethodID gav = (*env)->GetMethodID(env, mc, "getAxisValue", "(II)F");
        jfloat x = (*env)->CallFloatMethod(env, ev, gav, (jint)0, (jint)0);
        jfloat h = (*env)->CallFloatMethod(env, ev, gav, (jint)15, (jint)0);
        R[R_AXIS_X_X1000] = (int)(x * 1000.0f + (x < 0 ? -0.5f : 0.5f));
        R[R_AXIS_HAT_X1000] = (int)(h * 1000.0f + (h < 0 ? -0.5f : 0.5f));
        return JNI_TRUE;
    }
    FAIL("unknown event class");
    return JNI_FALSE;
}

static void nativeUnitySendMessage(JNIEnv *env, jobject player, jstring o, jstring m, jstring msg)
{
    R[R_SENDMSG] = str_eq(env, o, "GameManager") && str_eq(env, m, "OnHello") && str_eq(env, msg, "world");
}

/* JNIBridge.invoke(long ptr, Class iface, Method m, Object[] args) */
static jobject bridge_invoke(JNIEnv *env, jclass cls, jlong ptr, jclass iface, jobject method, jobjectArray args)
{
    jmethodID mid = (*env)->FromReflectedMethod(env, method);
    if (ptr == 0x1111 && mid == m_doFrame) {
        jobject boxed = (*env)->GetObjectArrayElement(env, args, 0);
        jclass lc = (*env)->FindClass(env, "java/lang/Long");
        jlong t = (*env)->CallLongMethod(env, boxed, (*env)->GetMethodID(env, lc, "longValue", "()J"));
        if (t <= 0) FAIL("bad frame time");
        R[R_DOFRAME]++;
        jobject ch = (*env)->CallStaticObjectMethod(env, c_choreo, (*env)->GetStaticMethodID(env, c_choreo, "getInstance", "()Landroid/view/Choreographer;"));
        (*env)->CallVoidMethod(env, ch, m_postFrame, g_choreo_cb);   /* re-arm like Unity */
        return 0;
    }
    if (ptr == 0x2222 && mid == m_handleMessage) {
        jobject msg = (*env)->GetObjectArrayElement(env, args, 0);
        jclass mc = (*env)->FindClass(env, "android/os/Message");
        R[R_HANDLE_MSG] = (*env)->GetIntField(env, msg, (*env)->GetFieldID(env, mc, "what", "I"));
        jclass bc = (*env)->FindClass(env, "java/lang/Boolean");
        return (*env)->NewObject(env, bc, (*env)->GetMethodID(env, bc, "<init>", "(Z)V"), (jboolean)1);
    }
    FAIL("unexpected proxy call ptr=%x", (int)ptr);
    return 0;
}

/* FMOD natives */
static jint fmodGetInfo(JNIEnv *env, jobject self, jint what)
{
    switch (what) { case 0: return 48000; case 1: return 256; case 2: return 4; case 3: return 1; }
    return 0;
}
static jint fmodProcess(JNIEnv *env, jobject self, jobject buf)
{
    short *p = (*env)->GetDirectBufferAddress(env, buf);
    jlong cap = (*env)->GetDirectBufferCapacity(env, buf);
    if (!p || cap != 256 * 4) FAIL("bad FMOD buffer");
    else p[0] = (short)R[R_FMOD_PROCESS];
    R[R_FMOD_PROCESS]++;
    return 0;
}

static const JNINativeMethod up_natives[] = {
    { "initJni", "(Landroid/content/Context;)V", (void *)initJni },
    { "nativeDone", "()V", (void *)nativeDone },
    { "nativePause", "()Z", (void *)nativePause },
    { "nativeRecreateGfxState", "(ILandroid/view/Surface;)V", (void *)nativeRecreateGfxState },
    { "nativeRender", "()Z", (void *)nativeRender },
    { "nativeResume", "()V", (void *)nativeResume },
    { "nativeLowMemory", "()V", (void *)nativeLowMemory },
    { "nativeFocusChanged", "(Z)V", (void *)nativeFocusChanged },
    { "nativeSoftInputClosed", "()V", (void *)nativeSoftInput },
    { "nativeSoftInputCanceled", "()V", (void *)nativeSoftInput },
    { "nativeInitWebRequest", "(Ljava/lang/Class;)V", (void *)nativeInitWebRequest },
    { "nativeInjectEvent", "(Landroid/view/InputEvent;)Z", (void *)nativeInjectEvent },
    { "nativeUnitySendMessage", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", (void *)nativeUnitySendMessage },
    { "nativeRestartActivityIndicator", "()V", (void *)nativeRestart },
};
static const JNINativeMethod fmod_natives[] = {
    { "fmodGetInfo", "(I)I", (void *)fmodGetInfo },
    { "fmodProcess", "(Ljava/nio/ByteBuffer;)I", (void *)fmodProcess },
};
static const JNINativeMethod bridge_natives[] = {
    { "invoke", "(JLjava/lang/Class;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;", (void *)bridge_invoke },
};

jint fake_libunity_onload(JavaVM *vm, void *reserved)
{
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return -1;
    R[R_ONLOAD]++;
    (*env)->RegisterNatives(env, (*env)->FindClass(env, "com/unity3d/player/UnityPlayer"), up_natives, sizeof up_natives / sizeof up_natives[0]);
    (*env)->RegisterNatives(env, (*env)->FindClass(env, "org/fmod/FMODAudioDevice"), fmod_natives, 2);
    (*env)->RegisterNatives(env, (*env)->FindClass(env, "bitter/jnibridge/JNIBridge"), bridge_natives, 1);
    return JNI_VERSION_1_6;
}

/* fake libmain: NativeLoader.load(dir) "dlopens" libunity and runs its JNI_OnLoad */
static jboolean nl_load(JNIEnv *env, jclass cls, jstring dir)
{
    if (!str_eq(env, dir, "/tmp/uptest/lib")) FAIL("NativeLoader.load: wrong dir");
    return fake_libunity_onload(g_vm, 0) > 0;
}
static jboolean nl_unload(JNIEnv *env, jclass cls) { return JNI_TRUE; }
static const JNINativeMethod nl_natives[] = {
    { "load", "(Ljava/lang/String;)Z", (void *)nl_load },
    { "unload", "()Z", (void *)nl_unload },
};

jint fake_libmain_onload(JavaVM *vm, void *reserved)
{
    JNIEnv *env;
    g_vm = vm;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return -1;
    (*env)->RegisterNatives(env, (*env)->FindClass(env, "com/unity3d/player/NativeLoader"), nl_natives, 2);
    return JNI_VERSION_1_6;
}
