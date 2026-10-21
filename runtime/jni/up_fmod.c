/* org.fmod.FMOD: the Java side of FMOD Engine's Android libraries (libfmod.so, libfmodstudio.so), for
 * a game whose FMOD Studio integration uses FMOD's Android build (docs/plugins.md). FMOD's native code
 * calls these static methods through JNI (its JNI_OnLoad keeps the JavaVM; libunity's plugin loading,
 * UnityPlayer.loadLibrary, runs it); in an Android app they come with FMOD's fmod.jar, here they
 * answer as that class does on a device:
 *   - checkInit: true. An Android build of the integration calls FMOD.init(activity) first; a
 *     desktop build's managed code does not, so the activity is the context from the start.
 *   - getAssetManager: the activity's AssetManager (FMOD opens "file:///android_asset/..." through
 *     AAssetManager_fromJava, which the android shims serve from the asset directory).
 *   - the output's native rate and block size: AudioManager's (up_config.audio_sample_rate /
 *     audio_frames_per_buffer, as for Unity's own FMOD).
 *   - low latency supported, no AAudio: FMOD's automatic output choice is then OpenSL ES, which the
 *     android shims play (SDL2, or a sound server through pw-cat/pacat).
 *   - one output device: the built-in speaker; no Bluetooth, no spatial audio, no content:// URIs.
 * org.fmod.AudioDevice (FMOD's AudioTrack output, chosen only when OpenSL is not) refuses to start
 * and says so: FMOD then reports an output error and the integration falls back to no sound.
 * Method names and signatures as FMOD's native libraries look them up. */
#include "up_internal.h"

#include <stdint.h>
#include <string.h>

#define M(c, n, s, f)       fj_method_define(c, n, s, 0, f, NULL)
#define SM(c, n, s, f)      fj_method_define(c, n, s, 1, f, NULL)
#define SMU(c, n, s, f, u)  fj_method_define(c, n, s, 1, f, (void *)(u))

static jobject g_fmod_context;

H(f_bool)  { return fj_z(ud != NULL); }
H(f_void)  { return fj_v(); }
H(f_init)
{
    if (g_fmod_context) fj_release(g_fmod_context);
    g_fmod_context = a[0].l ? fj_global(a[0].l) : NULL;
    up_log("org.fmod.FMOD.init(context)");
    return fj_v();
}
H(f_close) { up_log("org.fmod.FMOD.close()"); return fj_v(); }
H(f_checkInit)
{
    static int once;
    if (!once++) up_log("org.fmod.FMOD.checkInit: true (the activity is FMOD's context)");
    return fj_z(JNI_TRUE);
}
H(f_assets)
{
    jobject ctx = g_fmod_context ? g_fmod_context : g_activity;
    return fj_call(ctx, "getAssets", "()Landroid/content/res/AssetManager;");
}
H(f_rate)  { return fj_i(UPC.audio_sample_rate > 0 ? UPC.audio_sample_rate : 48000); }
H(f_block) { return fj_i(UPC.audio_frames_per_buffer > 0 ? UPC.audio_frames_per_buffer : 512); }
H(f_fd)    { return fj_i(-1); }                     /* fileDescriptorFromUri: no content:// URIs */
H(f_devices)                                         /* getAudioDevices(flags): the built-in speaker */
{
    jobjectArray arr = fj_array_new("Landroid/media/AudioDeviceInfo;", 1);
    (*env)->SetObjectArrayElement(env, arr, 0, fj_new_by_name("android/media/AudioDeviceInfo", NULL, NULL));
    return fj_l(arr);
}
H(adi_id)       { return fj_i(1); }
H(adi_name)     { return fj_l(fj_string("Speaker")); }
H(adi_rates)    { jintArray r = fj_array_new("I", 1); ((jint *)fj_array_data(r))[0] = UPC.audio_sample_rate > 0 ? UPC.audio_sample_rate : 48000; return fj_l(r); }
H(adi_channels) { jintArray r = fj_array_new("I", 1); ((jint *)fj_array_data(r))[0] = 2; return fj_l(r); }
H(adi_true)     { return fj_z(JNI_TRUE); }

H(ad_init)                                           /* AudioDevice.init(channels, rate, ...) */
{
    up_log("org.fmod.AudioDevice.init(%d, %d, %d, %d): FMOD's AudioTrack output is not provided (FMOD uses "
           "OpenSL ES here); FMOD falls back to no sound", a[0].i, a[1].i, a[2].i, a[3].i);
    return fj_z(JNI_FALSE);
}

void up_fmod_register(void)
{
    const char *F = "org/fmod/FMOD";
    fj_class_define(F, NULL);
    SM(F, "init", "(Landroid/content/Context;)V", f_init);
    SM(F, "close", "()V", f_close);
    SM(F, "checkInit", "()Z", f_checkInit);
    SM(F, "getAssetManager", "()Landroid/content/res/AssetManager;", f_assets);
    SMU(F, "supportsLowLatency", "()Z", f_bool, 1);
    SMU(F, "lowLatencyFlag", "()Z", f_bool, 1);
    SMU(F, "proAudioFlag", "()Z", f_bool, 0);
    SMU(F, "supportsAAudio", "()Z", f_bool, 0);
    SMU(F, "supportsSpatial", "()Z", f_bool, 0);
    SMU(F, "isBluetoothOn", "()Z", f_bool, 0);
    SM(F, "getOutputSampleRate", "()I", f_rate);
    SM(F, "getOutputBlockSize", "()I", f_block);
    SM(F, "fileDescriptorFromUri", "(Ljava/lang/String;)I", f_fd);
    SM(F, "getAudioDevices", "(I)[Landroid/media/AudioDeviceInfo;", f_devices);
    const char *ADI = "android/media/AudioDeviceInfo";
    M(ADI, "getId", "()I", adi_id);
    M(ADI, "getProductName", "()Ljava/lang/CharSequence;", adi_name);
    M(ADI, "getSampleRates", "()[I", adi_rates);
    M(ADI, "getChannelCounts", "()[I", adi_channels);
    M(ADI, "isSink", "()Z", adi_true);
    const char *AD = "org/fmod/AudioDevice";
    fj_class_define(AD, NULL);
    M(AD, "<init>", "()V", f_void);
    M(AD, "init", "(IIII)Z", ad_init);
    M(AD, "write", "([SI)V", f_void);
    M(AD, "close", "()V", f_void);
}
