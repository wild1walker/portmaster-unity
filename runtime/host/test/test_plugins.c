// plugins.c's unit test (runtime/host/test/plugins_test.sh):
//   test_plugins paths <tmp>         StreamingAssets paths -> files beside the APK
//   test_plugins lib <libdir> <root> the FMOD test double in <libdir> through plugins.map and the
//                                    loader; stubs and refusals
//   test_plugins wwise <root> [backend.so]   the Wwise stub (wwise.c) built from fn lines, called as
//                                    Mono calls Wwise 2017.2's integration:
//                                    results, the callback queue, game syncs; with a backend, what
//                                    it gets and an event it ends later
//   test_plugins fmod <root>         the built-in FMOD (fmodstub.c) under fmod and fmodstudio, called
//                                    as the integration calls it
#define _GNU_SOURCE
#include "../plugins.h"
#include "../../loader/so_loader.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { uint32_t a, b; } id2;
typedef struct { float x, y, z; } vec3;
int sf_fmod_calls(void *create, void *getvca, void *setvol, void *loadbank, void *bits, const char *bank, uint32_t *out);
int sf_discord(void *create);
int sf_wwise(void *(*sym)(const char *), uint64_t *o, uint64_t go, uint32_t rtpc, uint32_t event);

static void touch(const char *p)
{
    char d[512];
    snprintf(d, sizeof d, "%s", p);
    for (char *s = d + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(d, 0755); *s = '/'; }
    FILE *f = fopen(p, "w");
    if (f) { fputs("RIFF....", f); fclose(f); }
}

static int test_paths(const char *tmp)
{
    char p[512], b[1024], in[600];
    plugins_init(tmp, tmp);                              // (no map: sets the root)
    snprintf(p, sizeof p, "%s/assets/Master.bank", tmp); touch(p);
    snprintf(p, sizeof p, "%s/dirapk/assets/Sub/Music.bank", tmp); touch(p);
    snprintf(in, sizeof in, "jar:file://%s/main.1.com.x.obb!/assets/Master.bank", tmp);
    snprintf(p, sizeof p, "%s/assets/Master.bank", tmp);
    const char *r = plugins_asset_path(in, b, sizeof b);
    CHECK(r && !strcmp(r, p), "OBB path -> %s", r ? r : "NULL");
    snprintf(in, sizeof in, "jar:file://%s/dirapk!/assets/Sub/Music.bank", tmp);
    snprintf(p, sizeof p, "%s/dirapk/assets/Sub/Music.bank", tmp);
    r = plugins_asset_path(in, b, sizeof b);
    CHECK(r && !strcmp(r, p), "directory APK -> %s", r ? r : "NULL");
    r = plugins_asset_path("file:///android_asset/Master.bank", b, sizeof b);
    snprintf(p, sizeof p, "%s/assets/Master.bank", tmp);
    CHECK(r && !strcmp(r, p), "android_asset -> %s", r ? r : "NULL");
    snprintf(in, sizeof in, "jar:file://%s/x.obb!/assets/../../etc/passwd", tmp);
    CHECK(plugins_asset_path(in, b, sizeof b) == NULL, "'..' refused");
    snprintf(in, sizeof in, "jar:file://%s/x.obb!/assets/none.bank", tmp);
    CHECK(plugins_asset_path(in, b, sizeof b) == NULL, "missing file: NULL");
    CHECK(plugins_asset_path("/plain/path.bank", b, sizeof b) == NULL, "plain paths are left alone");
    char v[6][256];
    int n = plugins_name_variants("fmodstudio", v, 6);
    CHECK(n == 4 && !strcmp(v[0], "fmodstudio") && !strcmp(v[1], "libfmodstudio.so") && !strcmp(v[2], "libfmodstudio") && !strcmp(v[3], "fmodstudio.so"),
          "variants of fmodstudio: %d", n);
    n = plugins_name_variants("libsteam_api.so", v, 6);
    CHECK(n >= 3 && !strcmp(v[0], "libsteam_api.so") && !strcmp(v[1], "steam_api"), "variants of libsteam_api.so: %d %s", n, v[1]);
    return 0;
}

int sf_fmod_stub(void *const *fn, uint32_t *out);
static const char *const FMOD_STUB_CALLS[] = {
    "FMOD5_Memory_GetStats", "FMOD_Studio_System_Create", "FMOD_Studio_System_Initialize", "FMOD_Studio_System_GetLowLevelSystem",
    "FMOD_Studio_System_LoadBankFile", "FMOD_Studio_Bank_GetSampleLoadingState", "FMOD_Studio_System_LookupID",
    "FMOD_Studio_System_GetEventByID", "FMOD_Studio_EventDescription_CreateInstance", "FMOD_Studio_EventInstance_Start",
    "FMOD_Studio_EventInstance_GetPlaybackState", "FMOD_Studio_System_GetBus", "FMOD_Studio_Bus_SetVolume", "FMOD_Studio_Bus_GetVolume",
    "FMOD_Studio_EventInstance_SetParameterValue", "FMOD_Studio_EventInstance_GetParameterValue", "FMOD_Studio_EventInstance_GetParameter",
    "FMOD_Studio_ParameterInstance_SetValue", "FMOD_Studio_ParameterInstance_GetValue", "FMOD_Studio_EventInstance_Release",
    "FMOD_Studio_EventInstance_IsValid", "FMOD5_System_GetSoftwareChannels",
};

// the built-in FMOD (fmodstub.c) under both DllImport names, as the integration calls it
static int test_fmod(const char *root)
{
    char p[600];
    snprintf(p, sizeof p, "%s/plugins.map", root);
    FILE *f = fopen(p, "w");
    fprintf(f, "map fmodstudio stub fmod\nmap fmod stub fmod\n");
    fclose(f);
    plugins_init(root, root);
    void *h = so_dlopen("libfmodstudio.so", 0), *h2 = so_dlopen("fmod", 0);
    CHECK(h != NULL && h2 != NULL, "the stub under both names");
    if (!h) return 0;
    int n = (int)(sizeof FMOD_STUB_CALLS / sizeof FMOD_STUB_CALLS[0]);
    void *fn[32];
    for (int i = 0; i < n; i++) { fn[i] = so_dlsym(h, FMOD_STUB_CALLS[i]); CHECK(fn[i] != NULL, "%s", FMOD_STUB_CALLS[i]); if (!fn[i]) return 0; }
    CHECK(so_dlsym(h2, "FMOD5_Memory_GetStats") != NULL, "FMOD5_Memory_GetStats under fmod");
    uint32_t o[32] = { 0 };
    sf_fmod_stub(fn, o);
    float v;
    for (int i = 0; i <= 4; i++) CHECK(o[i] == 0, "%s: %u", FMOD_STUB_CALLS[i], o[i]);
    CHECK(o[5] == 3, "bank sample loading state LOADED: %u", o[5]);
    for (int i = 6; i <= 9; i++) CHECK(o[i] == 0, "%s: %u", FMOD_STUB_CALLS[i], o[i]);
    CHECK(o[10] == 2, "a started instance is STOPPED: %u", o[10]);
    CHECK(o[11] == 0 && o[12] == 0, "bus: %u %u", o[11], o[12]);
    memcpy(&v, &o[13], 4); CHECK(v == .625f, "bus volume back: %g (float argument through the ABI)", v);
    CHECK(o[14] == 0, "SetParameterValue: %u", o[14]);
    memcpy(&v, &o[15], 4); CHECK(v == 1.0f, "parameter value back: %g", v);
    CHECK(o[16] == 0 && o[17] == 0, "parameter instance: %u %u", o[16], o[17]);
    memcpy(&v, &o[18], 4); CHECK(v == .25f, "parameter instance value back: %g", v);
    CHECK(o[19] == 0 && o[20] == 0, "release, then not valid: %u %u", o[19], o[20]);
    CHECK(o[21] == 0, "an unimplemented entry point: %u", o[21]);
    return 0;
}

static int test_lib(const char *libdir, const char *root)
{
    so_add_shims(bionic_compat_table());
    so_set_library_path(libdir);
    so_set_dlsym_hook(plugins_dlsym_hook);
    char p[600];
    snprintf(p, sizeof p, "%s/plugins.map", root);
    FILE *f = fopen(p, "w");
    fprintf(f, "# test\nmap fmodstudio lib libfmodstudio.so\nmap discord_game_sdk stub discord_game_sdk\n"
               "map GalaxyCSharpGlue none\nmap sqlite3 host libnotthere.so.0\n"
               "sig fmodstudio FMOD_Studio_VCA_SetVolume i i,f\nsig fmodstudio FMOD5_ChannelGroup_SetVolume i i,f\n");
    fclose(f);
    snprintf(p, sizeof p, "%s/assets/Master.bank", root); touch(p);
    plugins_init(root, libdir);
    void *h = so_dlopen("libfmodstudio.so", 0);
    CHECK(h != NULL, "dlopen libfmodstudio.so: %s", so_dlerror());
    void *create = h ? so_dlsym(h, "FMOD_Studio_System_Create") : NULL, *getvca = h ? so_dlsym(h, "FMOD_Studio_System_GetVCA") : NULL;
    void *setvol = h ? so_dlsym(h, "FMOD_Studio_VCA_SetVolume") : NULL, *loadbank = h ? so_dlsym(h, "FMOD_Studio_System_LoadBankFile") : NULL;
    void *bits = h ? so_dlsym(h, "fmod_double_volume_bits") : NULL;
    CHECK(create && getvca && setvol && loadbank && bits, "symbols");
    if (create && getvca && setvol && loadbank && bits) {
        uint32_t out[2] = { 0, 0 };
        char bank[700];
        snprintf(bank, sizeof bank, "jar:file://%s/main.1.com.test.obb!/assets/Master.bank", root);
        int r = sf_fmod_calls(create, getvca, setvol, loadbank, bits, bank, out);
        float v;
        memcpy(&v, &out[0], 4);
        CHECK(r == 0, "FMOD calls: %d", r);
        CHECK(v == .75f, "VCA volume arrived as %g (float argument through the ABI)", v);
        CHECK(out[1] == 0, "LoadBankFile through the StreamingAssets rewrite: FMOD result %u", out[1]);
    }
    void *d = so_dlopen("discord_game_sdk", 0);
    void *dc = d ? so_dlsym(d, "DiscordCreate") : NULL;
    CHECK(dc != NULL, "discord stub");
    if (dc) CHECK(sf_discord(dc) == 27, "DiscordCreate -> NotRunning");
    CHECK(so_dlopen("libGalaxyCSharpGlue.so", 0) == NULL, "none: dlopen fails");
    CHECK(so_dlopen("sqlite3", 0) == NULL, "host library missing: dlopen fails");
    CHECK(plugins_stub_table("steam") != NULL && plugins_stub_table("nope") == NULL, "stub tables");
    return 0;
}

// Wwise 2017.2's signatures of the calls sf_wwise makes (as android-data writes them)
static const char *const WWISE_FNS =
    "fn AkSoundEngine CSharp_IsInitialized b -\n"
    "fn AkSoundEngine CSharp_new_AkInitSettings p -\n"
    "fn AkSoundEngine CSharp_AkInitSettings_uDefaultPoolSize_set v p,u\n"
    "fn AkSoundEngine CSharp_AkInitSettings_uDefaultPoolSize_get u p\n"
    "fn AkSoundEngine CSharp_new_AkVector p -\n"
    "fn AkSoundEngine CSharp_delete_AkVector v p\n"
    "fn AkSoundEngine CSharp_AkVector_X_set v p,f\n"
    "fn AkSoundEngine CSharp_AkVector_X_get f p\n"
    "fn AkSoundEngine CSharp_Init i p,p,p,p,p,p,p,u\n"
    "fn AkSoundEngine CSharp_Term v -\n"
    "fn AkSoundEngine CSharp_SetBasePath i z\n"
    "fn AkSoundEngine CSharp_AkCallbackSerializer_Init i p,u\n"
    "fn AkSoundEngine CSharp_AkCallbackSerializer_Lock p -\n"
    "fn AkSoundEngine CSharp_AkCallbackSerializer_Unlock v -\n"
    "fn AkSoundEngine CSharp_AkSerializedCallbackHeader_pPackage_get p p\n"
    "fn AkSoundEngine CSharp_AkSerializedCallbackHeader_pNext_get p p\n"
    "fn AkSoundEngine CSharp_AkSerializedCallbackHeader_eType_get i p\n"
    "fn AkSoundEngine CSharp_AkSerializedCallbackHeader_GetData p p\n"
    "fn AkSoundEngine CSharp_AkBankCallbackInfo_bankID_get u p\n"
    "fn AkSoundEngine CSharp_AkBankCallbackInfo_loadResult_get i p\n"
    "fn AkSoundEngine CSharp_AkCallbackInfo_gameObjID_get l p\n"
    "fn AkSoundEngine CSharp_AkEventCallbackInfo_playingID_get u p\n"
    "fn AkSoundEngine CSharp_AkEventCallbackInfo_eventID_get u p\n"
    "fn AkSoundEngine CSharp_AkEventCallbackInfo_SWIGUpcast p p\n"
    "fn AkSoundEngine CSharp_LoadBank__SWIG_0 i z,i,r\n"
    "fn AkSoundEngine CSharp_LoadBank__SWIG_4 i z,p,p,i,r\n"
    "fn AkSoundEngine CSharp_PostEvent__SWIG_3 u u,l,u,p,p\n"
    "fn AkSoundEngine CSharp_PostEvent__SWIG_11 u z,l\n"
    "fn AkSoundEngine CSharp_CancelEventCallbackCookie v p\n"
    "fn AkSoundEngine CSharp_SetRTPCValue__SWIG_0 i u,f,l,i,i,b\n"
    "fn AkSoundEngine CSharp_GetRTPCValue__SWIG_0 i u,l,u,r,r\n"
    "fn AkSoundEngine CSharp_SetState__SWIG_1 i z,z\n"
    "fn AkSoundEngine CSharp_GetState__SWIG_1 i z,r\n"
    "fn AkSoundEngine CSharp_GetIDFromString u z\n"
    "fn AkSoundEngine CSharp_new_AkTransform p -\n"
    "fn AkSoundEngine CSharp_delete_AkTransform v p\n"
    "fn AkSoundEngine CSharp_AkTransform_Set__SWIG_1 v p,f,f,f,f,f,f,f,f,f\n"
    "fn AkSoundEngine CSharp_SetObjectPosition i l,p\n"
    "fn AkSoundEngine CSharp_RenderAudio__SWIG_1 i -\n"
    "fn AkSoundEngine CSharp_GetBufferTick u -\n"
    "fn AkSoundEngine CSharp_AkExternalSourceInfo_szFile_set v p,z\n"
    "fn AkSoundEngine CSharp_AkExternalSourceInfo_szFile_get z p\n"
    "fn AkSoundEngine CSharp_new_AkDeviceSettings p -\n"
    "fn AkSoundEngine CSharp_AkDeviceSettings_threadProperties_get p p\n"
    "fn AkSoundEngine CSharp_AkThreadProperties_dwAffinityMask_set v p,u\n"
    "fn AkSoundEngine CSharp_AkThreadProperties_dwAffinityMask_get u p\n"
    "const AkSoundEngine AKRESULT.AK_Success 1\n"
    "const AkSoundEngine AkCallbackType.AK_EndOfEvent 1\n"
    "const AkSoundEngine AkCallbackType.AK_Bank 1073741824\n";

static void *g_wh;
static void *wsym(const char *name)
{
    void *p = g_wh ? so_dlsym(g_wh, name) : NULL;
    if (!p) { printf("FAIL: no entry point %s\n", name); exit(1); }
    return p;
}

// Wwise's ID of a name (FNV-1 of the lower-case name), written out here as Wwise's documentation has it
static uint32_t ak_id(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h * 16777619u) ^ (uint32_t)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
    return h;
}

static int test_wwise(const char *root, const char *backend)
{
    so_add_shims(bionic_compat_table());
    char p[600];
    snprintf(p, sizeof p, "%s/plugins.map", root);
    FILE *f = fopen(p, "w");
    fprintf(f, "map AkSoundEngine stub wwise\n%s", WWISE_FNS);
    fclose(f);
    if (backend) setenv("UA_WWISE_BACKEND", backend, 1);
    plugins_init(root, root);
    g_wh = so_dlopen("libAkSoundEngine.so", 0);
    CHECK(g_wh != NULL && so_dlopen("AkSoundEngine", 0) != NULL, "the stub under the DllImport's names");
    if (!g_wh) return 0;
    // an embedded struct's getter (SWIG gives its address): an object, the same one each time, with fields
    void *ds = ((void *(*)(void))wsym("CSharp_new_AkDeviceSettings"))();
    void *(*thread_get)(void *) = wsym("CSharp_AkDeviceSettings_threadProperties_get");
    void *tp = ds ? thread_get(ds) : NULL;
    CHECK(tp != NULL && thread_get(ds) == tp, "AkDeviceSettings.threadProperties: %p", tp);
    ((void (*)(void *, uint32_t))wsym("CSharp_AkThreadProperties_dwAffinityMask_set"))(tp, 3);
    CHECK(((uint32_t (*)(void *))wsym("CSharp_AkThreadProperties_dwAffinityMask_get"))(tp) == 3, "its field back");
    // strings marshaled as UTF-16 ([MarshalAs(LPWStr)]) are the same names as C strings
    uint32_t (*id_of)(const void *) = wsym("CSharp_GetIDFromString");
    static const uint16_t wide[] = { 'P', 'l', 'a', 'y', '_', 'M', 'u', 's', 'i', 'c', 0 };
    static const uint16_t wide_e[] = { 'C', 'a', 'f', 0xe9, 0 }, wide_one[] = { 'A', 0 };
    CHECK(id_of(wide) == ak_id("Play_Music"), "a UTF-16 string: %#x", id_of(wide));
    CHECK(id_of(wide_e) == id_of("Caf\xc3\xa9"), "a UTF-16 string beyond ASCII: UTF-8");
    CHECK(id_of(wide_one) == ak_id("A") && id_of("A") == ak_id("A"), "one character");
    uint64_t o[64], go = 0x1122334455667788ULL;
    uint32_t rtpc = ak_id("Music_Volume"), event = ak_id("Play_Footstep");
    memset(o, 0xee, sizeof o);
    int n = sf_wwise(wsym, o, go, rtpc, event);
    float fv;
    CHECK(n == 36, "calls made: %d", n);
    CHECK(o[0] == 0 && o[4] == 1 && o[35] == 0, "IsInitialized: before Init %llu, after %llu, after Term %llu",
          (unsigned long long)o[0], (unsigned long long)o[4], (unsigned long long)o[35]);
    CHECK(o[1] == 1234, "an object's uint field back: %llu", (unsigned long long)o[1]);
    uint32_t b = (uint32_t)o[2]; memcpy(&fv, &b, 4);
    CHECK(fv == 1.5f, "an object's float field back: %g", fv);
    CHECK(o[3] == 1 && o[5] == 1 && o[6] == 1 && o[7] == 1 && o[9] == 1 && o[12] == 1 && o[13] == 1 && o[15] == 1,
          "AKRESULT calls answer AK_Success");
    CHECK(o[8] == ak_id("Init") && o[8] == 0x50c63a23u, "LoadBank's bank ID out: the ID of the name without its extension, as in the bank's header: %#llx", (unsigned long long)o[8]);
    CHECK(o[10] != 0 && o[11] != 0 && o[10] != o[11], "playing IDs %llu %llu", (unsigned long long)o[10], (unsigned long long)o[11]);
    b = (uint32_t)o[14]; memcpy(&fv, &b, 4);
    CHECK(fv == 0.25f, "GetRTPCValue gives back the value set: %g", fv);
    CHECK(o[16] == ak_id("Combat"), "GetState gives back the state set");
    CHECK(o[17] == ak_id("Play_Music") && o[17] == ak_id("play_music"), "GetIDFromString: FNV-1 of the lower-case name");
    CHECK(o[18] == 1 && o[19] == 0x1234 && o[20] == 0x40000000u, "the bank callback first: package %#llx, type %#llx",
          (unsigned long long)o[19], (unsigned long long)o[20]);
    CHECK(o[21] == ak_id("SFX") && o[22] == 1, "bank callback info: ID, result");
    if (!backend) {
        CHECK(o[23] == 0x99 && o[24] == 1, "then the end of the event: package %#llx, type %llu", (unsigned long long)o[23], (unsigned long long)o[24]);
        CHECK(o[25] == go && o[26] == o[10] && o[27] == event, "end-of-event info: game object, playing ID, event ID");
        CHECK(o[28] == 0 && o[29] == 0 && o[30] == 0, "nothing more queued (also not the cancelled one)");
        CHECK(o[33] == 0, "an unsigned result: 0");
    }
    CHECK(o[31] == 1 && o[32] == 1, "SetObjectPosition, RenderAudio");
    CHECK(o[34] == 0, "no string result back");
    if (backend) {
        // the backend plays events: no end of event until it says so
        void *bh = dlopen(backend, RTLD_NOW | RTLD_NOLOAD);
        char *bank = bh ? dlsym(bh, "wb_bank") : NULL;
        float *rt = bh ? dlsym(bh, "wb_rtpc") : NULL, *pos = bh ? dlsym(bh, "wb_pos") : NULL;
        void (*finish)(void) = bh ? (void (*)(void))dlsym(bh, "wb_finish") : NULL;
        CHECK(bank && !strcmp(bank, "Init.bnk"), "the backend loaded the bank by name: %s", bank ? bank : "?");
        CHECK(rt && *rt == 0.25f, "the backend got the RTPC");
        CHECK(pos && pos[0] == 1 && pos[1] == 2 && pos[2] == 3, "the backend got the position");
        CHECK(o[28] == 0 && o[33] == 42, "no end of event while it plays; the backend answered GetBufferTick: %llu", (unsigned long long)o[33]);
        if (finish) finish();
        void *(*lock)(void) = wsym("CSharp_AkCallbackSerializer_Lock");
        void *(*type_of)(void *) = wsym("CSharp_AkSerializedCallbackHeader_eType_get");
        void *(*pkg)(void *) = wsym("CSharp_AkSerializedCallbackHeader_pPackage_get");
        void *h = lock();
        CHECK(h && (uintptr_t)type_of(h) == 1 && (uintptr_t)pkg(h) == 0x99, "the end of event once the backend ended it");
        ((void (*)(void))wsym("CSharp_AkCallbackSerializer_Unlock"))();
    }
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "paths") && argc > 2) test_paths(argv[2]);
    else if (!strcmp(mode, "lib") && argc > 3) test_lib(argv[2], argv[3]);
    else if (!strcmp(mode, "wwise") && argc > 2) test_wwise(argv[2], argc > 3 ? argv[3] : NULL);
    else if (!strcmp(mode, "fmod") && argc > 2) test_fmod(argv[2]);
    else { fprintf(stderr, "usage: test_plugins paths <tmp> | lib <libdir> <root> | wwise <root> [backend.so] | fmod <root>\n"); return 2; }
    printf("%s: %s (%d failures)\n", mode, fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
