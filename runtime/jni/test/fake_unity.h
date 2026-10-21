/* Shared between test_unityplayer.c (host) and fake_unity.c (built like NDK code): result slots
 * and the Unity versions whose JNI shapes the fake libmain/libunity can take. Integers only
 * across the boundary (ABI-neutral), plus one object pointer. */
#ifndef FAKE_UNITY_H
#define FAKE_UNITY_H

/* Each shape registers the natives (names and signatures) of that version's libunity and calls
 * the Java side the way that version's libunity and managed code do. */
enum { SHAPE_2017_4_9, SHAPE_2017_4_40, SHAPE_2018_4, SHAPE_2019_4, SHAPE_2020_3, SHAPE_2021_3,
       SHAPE_2022_3, SHAPE_6000_6, SHAPE_N };

enum { R_ONLOAD, R_INITJNI, R_RESUME, R_PAUSE, R_DONE, R_FOCUS_TRUE, R_FOCUS_FALSE, R_GFX, R_GFX_NULL,
       R_RENDER, R_WIDTH, R_HEIGHT, R_REFRESH_X100, R_SDK, R_MODEL_OK, R_PATH_OK, R_PREFS_FLOAT_X100,
       R_PREFS_STR_OK, R_DOFRAME, R_HANDLE_MSG, R_FMOD_PROCESS, R_KEY_CODE, R_KEY_ACTION, R_KEY_DEV_OK,
       R_AXIS_X_X1000, R_AXIS_HAT_X1000, R_MOTION_SRC, R_RANGES, R_AUDIO_RATE, R_WEBREQ, R_SENDMSG,
       R_ORDER_OK, R_LOCALE_OK, R_ERRORS,
       /* version-dependent paths */
       R_SENDMSG_JAVA,      /* UnityPlayer.UnitySendMessage (static Java) reached the native      */
       R_INJECT_EXTRA,      /* 1000 + the int of nativeInjectEvent(InputEvent, int) (2022.3)      */
       R_SURF_CHANGED,      /* nativeSendSurfaceChangedEvent calls (2018.4+)                      */
       R_SURF_W, R_SURF_H,  /* display size seen from nativeSendSurfaceChangedEvent              */
       R_NOWINDOW,          /* nativeGetNoWindowMode calls (2022.3)                               */
       R_ORIENT_LOCK,       /* 100 + nativeUpdateOrientationLockState value (2020.3+)             */
       R_CALLBACKS_OK,      /* new UnityPlayer callbacks answered as a handheld should (2018.4+)  */
       R_SWAPPY_JAVA,       /* Swappy's Java classes load and register; dex class loaders absent  */
       R_SWAPPY_FRAME,      /* ChoreographerCallback.postFrameCallback reached nOnChoreographer   */
       R_SWAPPY_MODES,      /* SwappyDisplayManager's constructor reported the panel's one mode   */
       R_IL2CPP_OK,         /* findLibrary("il2cpp") and <filesDir>/il2cpp work                   */
       R_PROXY_RUN,         /* AndroidJavaProxy Runnable.run() reached nativeProxyInvoke          */
       R_PROXY_RET,         /* a proxy call with an argument and a boxed boolean result           */
       R_PROXY_BADID,       /* nativeProxyInvoke with a wrong id                                  */
       R_PROXY_EXC_LOGGED,  /* nativeProxyLogJNIInvokeException(handle) after a managed exception */
       R_PROXY_FREED,       /* nativeProxyJNIFreeGCHandle(handle) (2021.3+)                       */
       R_HIDE_PRESERVED,    /* nativeHidePreservedContent calls (2021.3+)                         */
       R_SWAPPY_DISPLAY,    /* Swappy's display query through NativeActivity, Build.DISPLAY       */
       R_OBB_GUID_OK,       /* ZipFile(obb).getEntry("unity_obb_guid") read with Scanner (2021.3+) */
       R_VSYNC,             /* doFrame on a HandlerThread's own Choreographer (2021.3+)            */
       R_VSYNC_WAITED,      /* nativeRender waited for one of those and it came (2021.3+)         */
       R_LOADLIB_OK,        /* loadLibrary / System.load name rules                                */
       R_WEBREQ_ERR,        /* UnityWebRequest.run() (Java, up to 2018.4): errorCallback code     */
       R_U6_INITJNI,        /* initJni(Context, 0, "") on UnityPlayer (6000.6)                     */
       R_U6_JAVA,           /* runningOnMainThread on UnityMain, DeviceInfo, MemoryUsage (2023.1+) */
       R_U6_MEMORY,         /* nativeMemoryUsageChanged value (2023.1+: for up_low_memory)        */
       /* mouse MotionEvents (pointer mode) */
       R_MOTION_ACTION, R_MOTION_TOOL, R_MOTION_BUTTONS, R_MOTION_PRESSED, R_VSCROLL_X1000,
       R_MOUSE_DEV_OK,      /* getDevice() of a SOURCE_MOUSE event is the registered mouse        */
       /* soft keyboard natives */
       R_SI_STR, R_SI_SEL_START, R_SI_SEL_LEN, R_SI_CLOSED, R_SI_CANCELED, R_SI_LOST, R_SI_CONFIG,
       R_SI_VISIBLE, R_SI_AREA_TOP, R_SI_AREA_W, R_SI_LAYOUT,
       R_N };

#endif
