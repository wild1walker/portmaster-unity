/*
 * A fake libmain + libunity for test_unityplayer: built like NDK code (softfp/freestanding on
 * 32-bit ARM, ordinary code on AArch64). It registers the natives the real libraries register,
 * with the names and signatures of the Unity version chosen by fake_unity_set_shape() (from
 * tools/re/jnitables.py run on each version's player), and uses the Java APIs the real libunity
 * binds (tools/re/jnibindings.py): paths, display metrics, Build fields, SharedPreferences,
 * Choreographer + JNIBridge proxies, Handler/Message, FMODAudioDevice, KeyEvent/MotionEvent/
 * InputDevice; and, as Unity's managed code does, AndroidJavaProxy through ReflectionHelper.
 */
#include "../include/jni.h"
#include "fake_unity.h"

int printf(const char *, ...);
int strcmp(const char *, const char *);

static volatile int R[R_N];
static int S = SHAPE_2017_4_9;
int fake_unity_get(int i) { return (i >= 0 && i < R_N) ? R[i] : -1; }
void fake_unity_set_shape(int shape) { S = shape; }

static JavaVM *g_vm;
static jobject g_context, g_choreo_cb, g_handler, g_proxy, g_proxy_failing;
static jmethodID m_doFrame, m_handleMessage, m_postFrame;
static jclass c_choreo;

/* AndroidJavaProxy ids: ints up to 2018.4, longs (beyond 32 bits here) from 2019.4 */
#define PROXY_ID_INT  0x77
#define PROXY_ID_LONG 0x123456789aLL
#define EXC_HANDLE    0x5e1100e5LL

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

static int str_is(JNIEnv *env, jobject s, const char *want)   /* quiet str_eq */
{
    if (!s) return 0;
    const char *c = (*env)->GetStringUTFChars(env, (jstring)s, 0);
    int ok = !strcmp(c, want);
    (*env)->ReleaseStringUTFChars(env, (jstring)s, c);
    return ok;
}

static jobject call_obj(JNIEnv *env, jobject o, const char *n, const char *sig)
{
    jclass c = (*env)->GetObjectClass(env, o);
    jmethodID m = (*env)->GetMethodID(env, c, n, sig);
    return (*env)->CallObjectMethod(env, o, m);
}

static int clear_exc(JNIEnv *env)
{
    int e = (*env)->ExceptionCheck(env);
    if (e) (*env)->ExceptionClear(env);
    return e;
}

/* ---------------- version-dependent Java use ---------------- */

/* 2019.4+: display information comes from View.getDisplay() */
static void display_size(JNIEnv *env, jobject view_or_context, int *w, int *h, jfloat *rr)
{
    jclass dc = (*env)->FindClass(env, "android/view/Display");
    jclass dmc = (*env)->FindClass(env, "android/util/DisplayMetrics");
    jobject disp;
    if (S >= SHAPE_2019_4) {
        jclass vc = (*env)->FindClass(env, "android/view/View");
        disp = (*env)->CallObjectMethod(env, view_or_context, (*env)->GetMethodID(env, vc, "getDisplay", "()Landroid/view/Display;"));
    } else {
        /* getSystemService(WINDOW_SERVICE).getDefaultDisplay() */
        jclass ctx = (*env)->FindClass(env, "android/content/Context");
        jobject ws = (*env)->GetStaticObjectField(env, ctx, (*env)->GetStaticFieldID(env, ctx, "WINDOW_SERVICE", "Ljava/lang/String;"));
        jmethodID gss = (*env)->GetMethodID(env, ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        jobject wm = (*env)->CallObjectMethod(env, g_context, gss, ws);
        jclass wmc = (*env)->FindClass(env, "android/view/WindowManager");
        disp = (*env)->CallObjectMethod(env, wm, (*env)->GetMethodID(env, wmc, "getDefaultDisplay", "()Landroid/view/Display;"));
    }
    if (!disp) { FAIL("no Display"); return; }
    jobject dm = (*env)->NewObject(env, dmc, (*env)->GetMethodID(env, dmc, "<init>", "()V"));
    (*env)->CallVoidMethod(env, disp, (*env)->GetMethodID(env, dc, S >= SHAPE_2019_4 ? "getRealMetrics" : "getMetrics",
                                                        "(Landroid/util/DisplayMetrics;)V"), dm);
    *w = (*env)->GetIntField(env, dm, (*env)->GetFieldID(env, dmc, "widthPixels", "I"));
    *h = (*env)->GetIntField(env, dm, (*env)->GetFieldID(env, dmc, "heightPixels", "I"));
    if (rr) *rr = (*env)->CallFloatMethod(env, disp, (*env)->GetMethodID(env, dc, "getRefreshRate", "()F"));
}

/* UnityPlayer methods libunity added after 2017.4 (UnityPlayerJavaWrapper) */
static int new_callbacks(JNIEnv *env, jobject player)
{
    jclass pc = (*env)->FindClass(env, "com/unity3d/player/UnityPlayer");
    int ok = 1;
#define MID(n, s) (*env)->GetMethodID(env, pc, n, s)
    jstring s = (*env)->NewStringUTF(env, "text");
    if (S == SHAPE_2018_4)
        (*env)->CallVoidMethod(env, player, MID("showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;IZ)V"),
                               s, (jint)0, (jboolean)1, (jboolean)0, (jboolean)0, (jboolean)0, s, (jint)0, (jboolean)0);
    else
        (*env)->CallVoidMethod(env, player, MID("showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;IZZ)V"),
                               s, (jint)0, (jboolean)1, (jboolean)0, (jboolean)0, (jboolean)0, s, (jint)0, (jboolean)0, (jboolean)0);
    (*env)->CallVoidMethod(env, player, MID("setClipboardText", "(Ljava/lang/String;)V"), s);
    ok &= str_eq(env, (*env)->CallObjectMethod(env, player, MID("getClipboardText", "()Ljava/lang/String;")), "text");
    if (S >= SHAPE_2019_4) {
        ok &= !(*env)->CallBooleanMethod(env, player, MID("isUaaLUseCase", "()Z"));
        ok &= (*env)->CallIntMethod(env, player, MID("getUaaLLaunchProcessType", "()I")) == 0;
        ok &= (*env)->CallObjectMethod(env, player, MID("getLaunchURL", "()Ljava/lang/String;")) == 0;
        ok &= (*env)->CallIntMethod(env, player, MID("getNetworkConnectivity", "()I")) == 0;
        ok &= !(*env)->CallBooleanMethod(env, player, MID("startOrientationListener", "(I)Z"), (jint)1);
    }
    if (S >= SHAPE_2021_3) {
        jfloat b = (*env)->CallFloatMethod(env, player, MID("getScreenBrightness", "()F"));
        ok &= b > 0.99f && b < 1.01f;
        (*env)->CallVoidMethod(env, player, MID("setScreenBrightness", "(F)V"), (jfloat)0.5f);
        b = (*env)->CallFloatMethod(env, player, MID("getScreenBrightness", "()F"));
        ok &= b > 0.49f && b < 0.51f;
        (*env)->CallVoidMethod(env, player, MID("hidePreservedContent", "()V"));
    }
    if (S >= SHAPE_2022_3) {
        (*env)->CallVoidMethod(env, player, MID("setMainSurfaceViewAspectRatio", "(F)V"), (jfloat)1.5f);
        (*env)->CallVoidMethod(env, player, MID("startActivityIndicator", "(I)V"), (jint)0);
        (*env)->CallVoidMethod(env, player, MID("stopActivityIndicator", "()V"));
    }
#undef MID
    if (clear_exc(env)) ok = 0;
    return ok;
}

/* Swappy (2019.4+) looks for its Java helpers through the activity's ClassLoader, then tries the
   dex class loaders; the runtime makes all of them absent */
/* Swappy (2019.4+): libunity registers nOnChoreographer / the SwappyDisplayManager natives as it
   loads (and stops if the classes are missing); Swappy finds the classes through the activity's
   class loader (its embedded-dex fallback loaders are absent here) and runs a frame callback */
#define SWAPPY_COOKIE 0x5a5a
static void nOnChoreographer(JNIEnv *env, jobject self, jlong cookie, jlong t)
{
    (void)env; (void)self;
    if (cookie == SWAPPY_COOKIE && t > 0) R[R_SWAPPY_FRAME]++;
}
static void nSetSupportedRefreshRates(JNIEnv *env, jobject self, jlong cookie, jlongArray p, jintArray m) { (void)env; (void)self; (void)cookie; (void)p; (void)m; }
static void nOnRefreshRateChanged(JNIEnv *env, jobject self, jlong cookie, jlong a, jlong b, jlong c) { (void)env; (void)self; (void)cookie; (void)a; (void)b; (void)c; }
static int swappy_java(JNIEnv *env)
{
    jclass cb = (*env)->FindClass(env, "com/google/androidgamesdk/ChoreographerCallback");
    jclass dm = (*env)->FindClass(env, "com/google/androidgamesdk/SwappyDisplayManager");
    if (!cb || !dm) { clear_exc(env); return 0; }
    JNINativeMethod ncb[] = { { "nOnChoreographer", "(JJ)V", (void *)nOnChoreographer } };
    JNINativeMethod ndm[] = { { "nSetSupportedRefreshRates", "(J[J[I)V", (void *)nSetSupportedRefreshRates },
                              { "nOnRefreshRateChanged", "(JJJJ)V", (void *)nOnRefreshRateChanged } };
    int ok = (*env)->RegisterNatives(env, cb, ncb, 1) == 0 && (*env)->RegisterNatives(env, dm, ndm, 2) == 0;
    jclass cc = (*env)->FindClass(env, "android/content/Context");
    jobject cl = (*env)->CallObjectMethod(env, g_context, (*env)->GetMethodID(env, cc, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass clc = (*env)->FindClass(env, "java/lang/ClassLoader");
    jmethodID load = (*env)->GetMethodID(env, clc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    ok &= (*env)->CallObjectMethod(env, cl, load, (*env)->NewStringUTF(env, "com/google/androidgamesdk/ChoreographerCallback")) != 0;
    ok &= !clear_exc(env);
    ok &= !(*env)->FindClass(env, "dalvik/system/InMemoryDexClassLoader") && clear_exc(env);
    ok &= !(*env)->FindClass(env, "dalvik/system/PathClassLoader") && clear_exc(env);
    /* Swappy's JavaChoreographerThread: new ChoreographerCallback(cookie).postFrameCallback() */
    jobject o = (*env)->NewObject(env, cb, (*env)->GetMethodID(env, cb, "<init>", "(J)V"), (jlong)SWAPPY_COOKIE);
    ok &= o != 0;
    if (o) {
        (*env)->CallVoidMethod(env, o, (*env)->GetMethodID(env, cb, "postFrameCallback", "()V"));
        ok &= !clear_exc(env);
    }
    return ok;
}

/* what libunity's IL2CPP startup needs from Java: the library path and a writable files dir */
static int il2cpp_paths(JNIEnv *env)
{
    jclass cc = (*env)->FindClass(env, "android/content/Context");
    jobject cl = (*env)->CallObjectMethod(env, g_context, (*env)->GetMethodID(env, cc, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass clc = (*env)->FindClass(env, "java/lang/ClassLoader");
    jobject p = (*env)->CallObjectMethod(env, cl, (*env)->GetMethodID(env, clc, "findLibrary", "(Ljava/lang/String;)Ljava/lang/String;"),
                                         (*env)->NewStringUTF(env, "il2cpp"));
    int ok = str_eq(env, p, "/tmp/uptest/lib/libil2cpp.so");
    jobject files = call_obj(env, g_context, "getFilesDir", "()Ljava/io/File;");
    jclass fc = (*env)->FindClass(env, "java/io/File");
    jobject d = (*env)->NewObject(env, fc, (*env)->GetMethodID(env, fc, "<init>", "(Ljava/io/File;Ljava/lang/String;)V"),
                                  files, (*env)->NewStringUTF(env, "il2cpp"));
    ok &= (*env)->CallBooleanMethod(env, d, (*env)->GetMethodID(env, fc, "mkdirs", "()Z"));
    ok &= (*env)->CallBooleanMethod(env, d, (*env)->GetMethodID(env, fc, "isDirectory", "()Z"));
    ok &= str_eq(env, call_obj(env, d, "getAbsolutePath", "()Ljava/lang/String;"), "/tmp/uptest/data/files/il2cpp");
    return ok && !clear_exc(env);
}

/* AndroidJavaProxy as UnityEngine's managed code creates it */
static jobject new_proxy(JNIEnv *env, jobject player, jclass iface)
{
    jclass rh = (*env)->FindClass(env, "com/unity3d/player/ReflectionHelper");
    jclass clsc = (*env)->FindClass(env, "java/lang/Class");
    jobjectArray ifs = (*env)->NewObjectArray(env, 1, clsc, iface);
    if (S <= SHAPE_2018_4)
        return (*env)->CallStaticObjectMethod(env, rh, (*env)->GetStaticMethodID(env, rh, "newProxyInstance",
                                              "(I[Ljava/lang/Class;)Ljava/lang/Object;"), (jint)PROXY_ID_INT, ifs);
    if (S == SHAPE_2019_4)
        return (*env)->CallStaticObjectMethod(env, rh, (*env)->GetStaticMethodID(env, rh, "newProxyInstance",
                                              "(JLjava/lang/Class;)Ljava/lang/Object;"), (jlong)PROXY_ID_LONG, iface);
    return (*env)->CallStaticObjectMethod(env, rh, (*env)->GetStaticMethodID(env, rh, "newProxyInstance",
                                          "(Lcom/unity3d/player/UnityPlayer;J[Ljava/lang/Class;)Ljava/lang/Object;"),
                                          player, (jlong)PROXY_ID_LONG, ifs);
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

    int w = 0, h = 0;
    jfloat rr = 0;
    display_size(env, player, &w, &h, &rr);
    R[R_WIDTH] = w;
    R[R_HEIGHT] = h;
    R[R_REFRESH_X100] = (int)(rr * 100.0f + 0.5f);

    /* locale */
    jclass loc = (*env)->FindClass(env, "java/util/Locale");
    jobject l = (*env)->CallStaticObjectMethod(env, loc, (*env)->GetStaticMethodID(env, loc, "getDefault", "()Ljava/util/Locale;"));
    R[R_LOCALE_OK] = str_eq(env, (*env)->CallObjectMethod(env, l, (*env)->GetMethodID(env, loc, "getLanguage", "()Ljava/lang/String;")), "en")
                  && str_eq(env, (*env)->CallObjectMethod(env, l, (*env)->GetMethodID(env, loc, "getCountry", "()Ljava/lang/String;")), "US");

    /* PlayerPrefs: SharedPreferences round trip (floats through the Android ABI paths) */
    jclass ctx = (*env)->FindClass(env, "android/content/Context");
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

    R[R_CALLBACKS_OK] = S >= SHAPE_2018_4 ? new_callbacks(env, player) : 1;
    R[R_SWAPPY_JAVA] = S >= SHAPE_2019_4 ? swappy_java(env) : 1;
    R[R_IL2CPP_OK] = il2cpp_paths(env);
    if (clear_exc(env)) FAIL("exception pending after initJni");
    (*env)->PopLocalFrame(env, 0);
}

static void nativeInitWebRequest(JNIEnv *env, jobject player, jclass cls) { (void)player; R[R_WEBREQ] = cls != 0; }

static void nativeRecreateGfxState(JNIEnv *env, jobject player, jint which, jobject surface)
{
    if (surface) R[R_GFX]++; else R[R_GFX_NULL]++;
    if (!R[R_INITJNI]) FAIL("RecreateGfxState before initJni");
}

static void nativeSendSurfaceChangedEvent(JNIEnv *env, jobject player)
{
    if (R[R_SURF_CHANGED] + 1 > R[R_GFX]) FAIL("SendSurfaceChangedEvent before RecreateGfxState");
    int w = 0, h = 0;
    display_size(env, player, &w, &h, 0);
    R[R_SURF_W] = w;
    R[R_SURF_H] = h;
    R[R_SURF_CHANGED]++;   /* last: the host waits for it */
}

static void nativeFocusChanged(JNIEnv *env, jobject player, jboolean f) { if (f) R[R_FOCUS_TRUE]++; else R[R_FOCUS_FALSE]++; }

static void nativeResume(JNIEnv *env, jobject player)
{
    R[R_RESUME]++;
    if (R[R_RESUME] > 1) return;
    /* order on UnityMain: RecreateGfxState(surface) [, SendSurfaceChangedEvent], FocusChanged(true),
       Resume, then Render */
    R[R_ORDER_OK] = R[R_GFX] == 1 && R[R_FOCUS_TRUE] == 1 && R[R_RENDER] == 0 &&
                    R[R_SURF_CHANGED] == (S >= SHAPE_2018_4 ? 1 : 0);

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

    /* managed code: an AndroidJavaRunnable posted with activity.runOnUiThread (the UI thread runs
       it: Runnable.run() -> nativeProxyInvoke), a proxy called with an argument and a result, and
       (2019.4+) a proxy method that throws in managed code */
    jclass rc = (*env)->FindClass(env, "java/lang/Runnable");
    g_proxy = (*env)->NewGlobalRef(env, new_proxy(env, player, rc));
    jclass ac = (*env)->FindClass(env, "android/app/Activity");
    (*env)->CallVoidMethod(env, g_context, (*env)->GetMethodID(env, ac, "runOnUiThread", "(Ljava/lang/Runnable;)V"), g_proxy);
    jobject hp = new_proxy(env, player, hcb);
    jobject m7 = (*env)->CallObjectMethod(env, g_handler, (*env)->GetMethodID(env, hc, "obtainMessage", "(I)Landroid/os/Message;"), (jint)7);
    R[R_PROXY_RET] = (*env)->CallBooleanMethod(env, hp, m_handleMessage, m7) == JNI_TRUE;
    if (S >= SHAPE_2019_4) {
        jclass lc = (*env)->FindClass(env, "com/example/Listener");
        g_proxy_failing = new_proxy(env, player, lc);
        (*env)->CallVoidMethod(env, g_proxy_failing, (*env)->GetMethodID(env, lc, "onFail", "()V"));
        if (clear_exc(env)) FAIL("proxy exception reached the caller");
    }

    /* managed code calling the Java UnitySendMessage */
    jclass pc = (*env)->FindClass(env, "com/unity3d/player/UnityPlayer");
    (*env)->CallStaticVoidMethod(env, pc, (*env)->GetStaticMethodID(env, pc, "UnitySendMessage",
                                 "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"),
                                 (*env)->NewStringUTF(env, "Java"), (*env)->NewStringUTF(env, "Call"), (*env)->NewStringUTF(env, "x"));
}

static jboolean nativePause(JNIEnv *env, jobject player) { R[R_PAUSE]++; return JNI_FALSE; }
static void nativeDoneV(JNIEnv *env, jobject player) { R[R_DONE]++; }
static jboolean nativeDoneZ(JNIEnv *env, jobject player) { R[R_DONE]++; return JNI_TRUE; }
static void nativeVoid(JNIEnv *env, jobject player) { }
static void nativeVoidII(JNIEnv *env, jobject player, jint a, jint b) { }
static void nativeVoidZ(JNIEnv *env, jobject player, jboolean z) { }
static jboolean nativeFalse(JNIEnv *env, jobject player) { return JNI_FALSE; }
static jboolean nativeNoWindow(JNIEnv *env, jobject player) { R[R_NOWINDOW]++; return JNI_FALSE; }
static void nativeHidePreserved(JNIEnv *env, jobject player) { R[R_HIDE_PRESERVED]++; }
static void nativeOrientationLock(JNIEnv *env, jobject self, jint v) { R[R_ORIENT_LOCK] = 100 + v; }

static jboolean nativeRender(JNIEnv *env, jobject player)
{
    R[R_RENDER]++;
    if (R[R_RESUME] == 0) FAIL("render before resume");
    return JNI_TRUE;
}

static jboolean inject_event(JNIEnv *env, jobject ev)
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
        /* what libunity reads of a mouse event */
        R[R_MOTION_ACTION] = (*env)->CallIntMethod(env, ev, (*env)->GetMethodID(env, mc, "getActionMasked", "()I"));
        R[R_MOTION_TOOL] = (*env)->CallIntMethod(env, ev, (*env)->GetMethodID(env, mc, "getToolType", "(I)I"), (jint)0);
        R[R_MOTION_BUTTONS] = (*env)->CallIntMethod(env, ev, (*env)->GetMethodID(env, mc, "getButtonState", "()I"));
        R[R_MOTION_PRESSED] = (*env)->CallBooleanMethod(env, ev, (*env)->GetMethodID(env, mc, "isButtonPressed", "(I)Z"), (jint)1);
        jfloat vs = (*env)->CallFloatMethod(env, ev, gav, (jint)9, (jint)0);
        R[R_VSCROLL_X1000] = (int)(vs * 1000.0f + (vs < 0 ? -0.5f : 0.5f));
        if (src == 0x2002) {
            jclass idc = (*env)->FindClass(env, "android/view/InputDevice");
            jobject d = (*env)->CallObjectMethod(env, ev, (*env)->GetMethodID(env, mc, "getDevice", "()Landroid/view/InputDevice;"));
            R[R_MOUSE_DEV_OK] = d && (*env)->CallBooleanMethod(env, d, (*env)->GetMethodID(env, idc, "supportsSource", "(I)Z"), (jint)0x2002) &&
                                str_eq(env, (*env)->CallObjectMethod(env, d, (*env)->GetMethodID(env, idc, "getName", "()Ljava/lang/String;")), "Test Mouse");
        }
        return JNI_TRUE;
    }
    FAIL("unknown event class");
    return JNI_FALSE;
}
static jboolean nativeInjectEvent(JNIEnv *env, jobject player, jobject ev) { return inject_event(env, ev); }
static jboolean nativeInjectEvent2(JNIEnv *env, jobject player, jobject ev, jint extra)
{
    R[R_INJECT_EXTRA] = 1000 + extra;
    return inject_event(env, ev);
}

static const char hello_utf8[] = "w\xc3\xb6rld \xe2\x9c\x93";   /* "wörld ✓" */

static void sendmsg_check(JNIEnv *env, jstring o, jstring m, int msg_ok)
{
    if (str_is(env, o, "Java")) { R[R_SENDMSG_JAVA] = msg_ok; return; }
    R[R_SENDMSG] = str_eq(env, o, "GameManager") && str_eq(env, m, "OnHello") && msg_ok;
}
/* up to 2017.4.9: the message as a String */
static void nativeUnitySendMessageS(JNIEnv *env, jclass cls, jstring o, jstring m, jstring msg)
{
    const char *want = str_is(env, o, "Java") ? "x" : hello_utf8;
    sendmsg_check(env, o, m, str_eq(env, msg, want));
}
/* 2017.4.40+: the message as UTF-8 bytes */
static void nativeUnitySendMessageB(JNIEnv *env, jclass cls, jstring o, jstring m, jbyteArray msg)
{
    const char *want = str_is(env, o, "Java") ? "x" : hello_utf8;
    jsize n = (*env)->GetArrayLength(env, msg), i;
    jbyte *b = (*env)->GetByteArrayElements(env, msg, 0);
    int ok = 1;
    for (i = 0; i < n && want[i]; i++) if ((char)b[i] != want[i]) ok = 0;
    ok = ok && i == n && !want[n];
    (*env)->ReleaseByteArrayElements(env, msg, b, 0);
    sendmsg_check(env, o, m, ok);
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

/* ReflectionHelper natives: the managed side of AndroidJavaProxy */
static jobject proxy_invoke(JNIEnv *env, jlong id, jstring name, jobjectArray args)
{
    jlong want = S <= SHAPE_2018_4 ? PROXY_ID_INT : PROXY_ID_LONG;
    if (id != want) { R[R_PROXY_BADID]++; FAIL("proxy id %llx", (long long)id); return 0; }
    jclass rh = (*env)->FindClass(env, "com/unity3d/player/ReflectionHelper");
    if (str_is(env, name, "run")) {
        if ((*env)->GetArrayLength(env, args) != 0) FAIL("run() with arguments");
        R[R_PROXY_RUN]++;
        return 0;
    }
    if (str_is(env, name, "handleMessage")) {
        jobject msg = (*env)->GetObjectArrayElement(env, args, 0);
        jclass mc = (*env)->FindClass(env, "android/os/Message");
        if ((*env)->GetIntField(env, msg, (*env)->GetFieldID(env, mc, "what", "I")) != 7) FAIL("handleMessage argument");
        jclass bc = (*env)->FindClass(env, "java/lang/Boolean");
        return (*env)->CallStaticObjectMethod(env, bc, (*env)->GetStaticMethodID(env, bc, "valueOf", "(Z)Ljava/lang/Boolean;"), (jboolean)1);
    }
    if (str_is(env, name, "onFail")) {
        /* a C# exception: 2019.4/2020.3 flag it on the proxy, 2021.3+ return an invocation error */
        if (S >= SHAPE_2021_3)
            return (*env)->CallStaticObjectMethod(env, rh, (*env)->GetStaticMethodID(env, rh, "createInvocationError", "(JZ)Ljava/lang/Object;"),
                                                  (jlong)EXC_HANDLE, (jboolean)0);
        (*env)->CallStaticVoidMethod(env, rh, (*env)->GetStaticMethodID(env, rh, "setNativeExceptionOnProxy", "(Ljava/lang/Object;JZ)V"),
                                     g_proxy_failing, (jlong)EXC_HANDLE, (jboolean)0);
        return 0;
    }
    FAIL("unexpected proxy method");
    return 0;
}
static jobject nativeProxyInvokeI(JNIEnv *env, jclass c, jint id, jstring name, jobjectArray args) { return proxy_invoke(env, id, name, args); }
static jobject nativeProxyInvokeJ(JNIEnv *env, jclass c, jlong id, jstring name, jobjectArray args) { return proxy_invoke(env, id, name, args); }
static void nativeProxyFinalizeI(JNIEnv *env, jclass c, jint id) { }
static void nativeProxyFinalizeJ(JNIEnv *env, jclass c, jlong id) { }
static void nativeProxyLogV(JNIEnv *env, jclass c) { }
static void nativeProxyLogJ(JNIEnv *env, jclass c, jlong h) { if (h == EXC_HANDLE) R[R_PROXY_EXC_LOGGED]++; else FAIL("logged handle"); }
static void nativeProxyFreeJ(JNIEnv *env, jclass c, jlong h) { if (h == EXC_HANDLE) R[R_PROXY_FREED]++; else FAIL("freed handle"); }

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

/* The UnityPlayer natives the runtime calls, with each version's signature, plus a few it does
   not call (registered as the real table does). first/last: the shapes that register an entry. */
#define SIG_SENDMSG_S "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"
#define SIG_SENDMSG_B "(Ljava/lang/String;Ljava/lang/String;[B)V"
struct nat { int first, last; JNINativeMethod m; };
static const struct nat up_natives[] = {
    { 0, SHAPE_N, { "initJni", "(Landroid/content/Context;)V", (void *)initJni } },
    { 0, SHAPE_2018_4, { "nativeDone", "()V", (void *)nativeDoneV } },
    { SHAPE_2019_4, SHAPE_N, { "nativeDone", "()Z", (void *)nativeDoneZ } },
    { 0, SHAPE_N, { "nativePause", "()Z", (void *)nativePause } },
    { 0, SHAPE_N, { "nativeRecreateGfxState", "(ILandroid/view/Surface;)V", (void *)nativeRecreateGfxState } },
    { SHAPE_2018_4, SHAPE_N, { "nativeSendSurfaceChangedEvent", "()V", (void *)nativeSendSurfaceChangedEvent } },
    { 0, SHAPE_N, { "nativeRender", "()Z", (void *)nativeRender } },
    { 0, SHAPE_N, { "nativeResume", "()V", (void *)nativeResume } },
    { 0, SHAPE_N, { "nativeLowMemory", "()V", (void *)nativeVoid } },
    { SHAPE_2019_4, SHAPE_N, { "nativeApplicationUnload", "()V", (void *)nativeVoid } },
    { 0, SHAPE_N, { "nativeFocusChanged", "(Z)V", (void *)nativeFocusChanged } },
    { SHAPE_2019_4, SHAPE_N, { "nativeSetInputArea", "(IIII)V", (void *)nativeVoid } },
    { SHAPE_2019_4, SHAPE_N, { "nativeSetKeyboardIsVisible", "(Z)V", (void *)nativeVoidZ } },
    { 0, SHAPE_N, { "nativeSoftInputClosed", "()V", (void *)nativeVoid } },
    { 0, SHAPE_N, { "nativeSoftInputCanceled", "()V", (void *)nativeVoid } },
    { 0, SHAPE_2018_4, { "nativeInitWebRequest", "(Ljava/lang/Class;)V", (void *)nativeInitWebRequest } },
    { 0, SHAPE_2021_3, { "nativeInjectEvent", "(Landroid/view/InputEvent;)Z", (void *)nativeInjectEvent } },
    { SHAPE_2022_3, SHAPE_N, { "nativeInjectEvent", "(Landroid/view/InputEvent;I)Z", (void *)nativeInjectEvent2 } },
    { 0, SHAPE_2017_4_9, { "nativeUnitySendMessage", SIG_SENDMSG_S, (void *)nativeUnitySendMessageS } },
    { SHAPE_2017_4_40, SHAPE_N, { "nativeUnitySendMessage", SIG_SENDMSG_B, (void *)nativeUnitySendMessageB } },
    { 0, SHAPE_N, { "nativeIsAutorotationOn", "()Z", (void *)nativeFalse } },
    { 0, SHAPE_N, { "nativeMuteMasterAudio", "(Z)V", (void *)nativeVoidZ } },
    { 0, SHAPE_2021_3, { "nativeRestartActivityIndicator", "()V", (void *)nativeVoid } },
    { SHAPE_2019_4, SHAPE_N, { "nativeOrientationChanged", "(II)V", (void *)nativeVoidII } },
    { SHAPE_2021_3, SHAPE_N, { "nativeHidePreservedContent", "()V", (void *)nativeHidePreserved } },
    { SHAPE_2022_3, SHAPE_N, { "nativeGetNoWindowMode", "()Z", (void *)nativeNoWindow } },
};
static const struct nat rh_natives[] = {
    { 0, SHAPE_2018_4, { "nativeProxyInvoke", "(ILjava/lang/String;[Ljava/lang/Object;)Ljava/lang/Object;", (void *)nativeProxyInvokeI } },
    { 0, SHAPE_2018_4, { "nativeProxyFinalize", "(I)V", (void *)nativeProxyFinalizeI } },
    { SHAPE_2018_4, SHAPE_2018_4, { "nativeProxyLogJNIInvokeException", "()V", (void *)nativeProxyLogV } },
    { SHAPE_2019_4, SHAPE_N, { "nativeProxyInvoke", "(JLjava/lang/String;[Ljava/lang/Object;)Ljava/lang/Object;", (void *)nativeProxyInvokeJ } },
    { SHAPE_2019_4, SHAPE_N, { "nativeProxyFinalize", "(J)V", (void *)nativeProxyFinalizeJ } },
    { SHAPE_2019_4, SHAPE_N, { "nativeProxyLogJNIInvokeException", "(J)V", (void *)nativeProxyLogJ } },
    { SHAPE_2021_3, SHAPE_N, { "nativeProxyJNIFreeGCHandle", "(J)V", (void *)nativeProxyFreeJ } },
};
static const struct nat ol_natives[] = {
    { SHAPE_2020_3, SHAPE_N, { "nativeUpdateOrientationLockState", "(I)V", (void *)nativeOrientationLock } },
};
static const JNINativeMethod fmod_natives[] = {
    { "fmodGetInfo", "(I)I", (void *)fmodGetInfo },
    { "fmodProcess", "(Ljava/nio/ByteBuffer;)I", (void *)fmodProcess },
};
static const JNINativeMethod bridge_natives[] = {
    { "invoke", "(JLjava/lang/Class;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;", (void *)bridge_invoke },
};

static void register_shape(JNIEnv *env, const char *cls, const struct nat *t, int n)
{
    JNINativeMethod m[32];
    int k = 0;
    for (int i = 0; i < n && k < 32; i++)
        if (S >= t[i].first && S <= t[i].last) m[k++] = t[i].m;
    if (k) (*env)->RegisterNatives(env, (*env)->FindClass(env, cls), m, k);
}

jint fake_libunity_onload(JavaVM *vm, void *reserved)
{
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return -1;
    R[R_ONLOAD]++;
    register_shape(env, "com/unity3d/player/UnityPlayer", up_natives, sizeof up_natives / sizeof up_natives[0]);
    register_shape(env, "com/unity3d/player/ReflectionHelper", rh_natives, sizeof rh_natives / sizeof rh_natives[0]);
    register_shape(env, "com/unity3d/player/OrientationLockListener", ol_natives, 1);
    (*env)->RegisterNatives(env, (*env)->FindClass(env, "org/fmod/FMODAudioDevice"), fmod_natives, 2);
    (*env)->RegisterNatives(env, (*env)->FindClass(env, "bitter/jnibridge/JNIBridge"), bridge_natives, 1);
    return JNI_VERSION_1_6;
}

/* the AndroidJavaProxy (Runnable) made in nativeResume, for the host side */
jobject fake_unity_proxy(void) { return g_proxy; }

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
