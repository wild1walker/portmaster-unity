// The "Mono side" of test_plugins.c: on 32-bit ARM built -mfloat-abi=softfp, like the armeabi-v7a
// code of Unity's Mono, so its calls pass floats in core registers. Its interface to the hard-float
// test program has integer and pointer arguments only.
#include <stddef.h>
#include <stdint.h>
void *memcpy(void *, const void *, size_t);           // (built freestanding: no libc headers)

typedef struct { uint32_t a, b; } id2;
typedef struct { float x, y, z; } vec3;

static uint32_t fbits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static uint64_t dbits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }

// calls each thunk the way a P/Invoke of that signature would; results (as bits) in ret[]
void sf_thunk_calls(void *const *fn, uint64_t *ret)
{
    ret[0] = ((int (*)(int, float))fn[0])(7, 1.5f);
    ret[1] = ((int (*)(int, long long, float))fn[1])(3, 0x1122334455667788LL, -2.25f);
    ret[2] = ((int (*)(void *, float, float, float, float, float, float, float, float))fn[2])((void *)0x1234, 1, 2, 3, 4, 5, 6, 7, 8);
    ret[3] = ((int (*)(float, double, float))fn[3])(0.5f, 3.25, 4.75f);
    id2 id = { 0xaabb, 0xccdd };
    ret[4] = ((int (*)(void *, id2, float, int))fn[4])((void *)0x5678, id, 0.125f, 1);
    vec3 v = { 1, 2, 3 };
    ret[5] = ((int (*)(vec3, int))fn[5])(v, 9);
    ret[6] = dbits(((double (*)(double, double, double, double, double, double, double, double, double))fn[6])(1, 2, 3, 4, 5, 6, 7, 8, 9));
    ret[7] = fbits(((float (*)(float, float))fn[7])(1.5f, 4.f));
    ret[8] = ((int (*)(int, int, int, int, int, int, float, float, float, float, float, float, float, float, float, float,
                       float, float, float, float, float, float, float, float))fn[8])(1, 2, 3, 4, 5, 6,
              .5f, 1, 1.5f, 2, 2.5f, 3, 3.5f, 4, 4.5f, 5, 5.5f, 6, 6.5f, 7, 7.5f, 8, 8.5f, 9);
    ret[9] = ((int (*)(double, int, float, double, float, float))fn[9])(1.25, 11, 2.5f, 3.75, 5.5f, 6.25f);
}

// the FMOD calls of Unity's integration, as Mono makes them, against the test double
typedef int (*create_t)(void **, unsigned);
typedef int (*getvca_t)(void *, const char *, void **);
typedef int (*setvol_t)(void *, float);
typedef int (*loadbank_t)(void *, const char *, unsigned, void **);
typedef int (*setparam_t)(void *, id2, float, int);
typedef uint32_t (*bits_t)(void *);
int sf_fmod_calls(void *create, void *getvca, void *setvol, void *loadbank, void *bits, const char *bank, uint32_t *out)
{
    void *sys = 0, *vca = 0, *b = 0;
    int r = ((create_t)create)(&sys, 0x00020223);
    if (r) return 100 + r;
    r = ((getvca_t)getvca)(sys, "vca:/Master", &vca);
    if (r) return 200 + r;
    r = ((setvol_t)setvol)(vca, 0.75f);
    if (r) return 300 + r;
    out[0] = ((bits_t)bits)(vca);
    r = ((loadbank_t)loadbank)(sys, bank, 0, &b);
    out[1] = (uint32_t)r;
    return 0;
}

int sf_discord(void *create)
{
    void *core = (void *)1;
    int r = ((int (*)(int, void *, void **))create)(3, 0, &core);
    return core ? -1 : r;
}

// Wwise's C# integration (Unity, Wwise 2017.2's SWIG signatures) against the built-in stub: the calls
// it makes at start, a bank loaded with a callback, events, game syncs, and its per-frame read of the
// callback queue (AkCallbackManager.PostCallbacks). sym() gives an entry point (Mono's dlsym); the
// results go to o[] as bits, in order, for the hard-float side to check.
typedef void *(*sym_t)(const char *);
#define W(t, name) ((t)sym(name))
int sf_wwise(sym_t sym, uint64_t *o, uint64_t go, uint32_t rtpc, uint32_t event)
{
    int k = 0;
    o[k++] = W(int (*)(void), "CSharp_IsInitialized")();                                         // 0: 0
    void *s = W(void *(*)(void), "CSharp_new_AkInitSettings")();
    W(void (*)(void *, uint32_t), "CSharp_AkInitSettings_uDefaultPoolSize_set")(s, 1234);
    o[k++] = W(uint32_t (*)(void *), "CSharp_AkInitSettings_uDefaultPoolSize_get")(s);           // 1: 1234
    void *v = W(void *(*)(void), "CSharp_new_AkVector")();
    W(void (*)(void *, float), "CSharp_AkVector_X_set")(v, 1.5f);
    o[k++] = fbits(W(float (*)(void *), "CSharp_AkVector_X_get")(v));                           // 2: 1.5
    o[k++] = W(int (*)(void *, void *, void *, void *, void *, void *, void *, uint32_t), "CSharp_Init")(s, s, s, s, s, s, s, 0);   // 3: 1
    o[k++] = W(int (*)(void), "CSharp_IsInitialized")();                                         // 4: 1
    o[k++] = W(int (*)(const char *), "CSharp_SetBasePath")("/nowhere/Audio/GeneratedSoundBanks/Linux/");   // 5: 1
    o[k++] = W(int (*)(void *, uint32_t), "CSharp_AkCallbackSerializer_Init")((void *)0, 4096);  // 6: 1
    uint32_t bank = 0;
    o[k++] = W(int (*)(const char *, int, uint32_t *), "CSharp_LoadBank__SWIG_0")("Init.bnk", -1, &bank);   // 7: 1
    o[k++] = bank;                                                                              // 8: ID of "Init.bnk"
    o[k++] = W(int (*)(const char *, void *, void *, int, uint32_t *), "CSharp_LoadBank__SWIG_4")("SFX.bnk", 0, (void *)0x1234, -1, &bank);  // 9: 1
    uint32_t (*post3)(uint32_t, uint64_t, uint32_t, void *, void *) = W(uint32_t (*)(uint32_t, uint64_t, uint32_t, void *, void *), "CSharp_PostEvent__SWIG_3");
    uint32_t p1 = post3(event, go, 1, (void *)1, (void *)0x99);
    o[k++] = p1;                                                                                // 10: a playing ID
    o[k++] = W(uint32_t (*)(const char *, uint64_t), "CSharp_PostEvent__SWIG_11")("Play_Music", go);   // 11: another
    o[k++] = W(int (*)(uint32_t, float, uint64_t, int, int, int), "CSharp_SetRTPCValue__SWIG_0")(rtpc, 0.25f, go, 0, 4, 0);   // 12: 1
    float f = 0; int type = 1;
    o[k++] = W(int (*)(uint32_t, uint64_t, uint32_t, float *, int *), "CSharp_GetRTPCValue__SWIG_0")(rtpc, go, 0, &f, &type);   // 13: 1
    o[k++] = fbits(f);                                                                          // 14: 0.25
    o[k++] = W(int (*)(const char *, const char *), "CSharp_SetState__SWIG_1")("Music", "Combat");   // 15: 1
    uint32_t st = 0;
    W(int (*)(const char *, uint32_t *), "CSharp_GetState__SWIG_1")("Music", &st);
    o[k++] = st;                                                                                // 16: ID of "Combat"
    o[k++] = W(uint32_t (*)(const char *), "CSharp_GetIDFromString")("Play_Music");              // 17: ID
    // the callback queue, as PostCallbacks reads it
    void *(*lock)(void) = W(void *(*)(void), "CSharp_AkCallbackSerializer_Lock");
    void *(*pkg)(void *) = W(void *(*)(void *), "CSharp_AkSerializedCallbackHeader_pPackage_get");
    void *(*next)(void *) = W(void *(*)(void *), "CSharp_AkSerializedCallbackHeader_pNext_get");
    int (*type_of)(void *) = W(int (*)(void *), "CSharp_AkSerializedCallbackHeader_eType_get");
    void *(*data)(void *) = W(void *(*)(void *), "CSharp_AkSerializedCallbackHeader_GetData");
    void *h = lock();
    o[k++] = h != 0;                                                                            // 18: 1
    o[k++] = (uintptr_t)pkg(h);                                                                 // 19: 0x1234
    o[k++] = (uint32_t)type_of(h);                                                              // 20: AK_Bank
    void *d = data(h);
    o[k++] = W(uint32_t (*)(void *), "CSharp_AkBankCallbackInfo_bankID_get")(d);                 // 21: ID of "SFX.bnk"
    o[k++] = (uint32_t)W(int (*)(void *), "CSharp_AkBankCallbackInfo_loadResult_get")(d);       // 22: 1
    void *h2 = next(h);
    o[k++] = (uintptr_t)pkg(h2);                                                                // 23: 0x99
    o[k++] = (uint32_t)type_of(h2);                                                             // 24: AK_EndOfEvent
    void *d2 = W(void *(*)(void *), "CSharp_AkEventCallbackInfo_SWIGUpcast")(data(h2));
    o[k++] = W(uint64_t (*)(void *), "CSharp_AkCallbackInfo_gameObjID_get")(d2);                 // 25: go
    o[k++] = W(uint32_t (*)(void *), "CSharp_AkEventCallbackInfo_playingID_get")(data(h2));      // 26: p1
    o[k++] = W(uint32_t (*)(void *), "CSharp_AkEventCallbackInfo_eventID_get")(data(h2));        // 27: event
    o[k++] = (uintptr_t)next(h2);                                                               // 28: 0 (the end)
    W(void (*)(void), "CSharp_AkCallbackSerializer_Unlock")();
    o[k++] = (uintptr_t)lock();                                                                 // 29: 0 (nothing more)
    W(void (*)(void), "CSharp_AkCallbackSerializer_Unlock")();
    // a cancelled callback never arrives
    post3(event, go, 1, (void *)1, (void *)0x77);
    W(void (*)(void *), "CSharp_CancelEventCallbackCookie")((void *)0x77);
    o[k++] = (uintptr_t)lock();                                                                 // 30: 0
    W(void (*)(void), "CSharp_AkCallbackSerializer_Unlock")();
    // a transform of 9 floats, a position
    void *t = W(void *(*)(void), "CSharp_new_AkTransform")();
    W(void (*)(void *, float, float, float, float, float, float, float, float, float), "CSharp_AkTransform_Set__SWIG_1")(t, 1, 2, 3, 0, 0, 1, 0, 1, 0);
    o[k++] = W(int (*)(uint64_t, void *), "CSharp_SetObjectPosition")(go, t);                   // 31: 1
    o[k++] = W(int (*)(void), "CSharp_RenderAudio__SWIG_1")();                                   // 32: 1 (AKRESULT)
    o[k++] = W(uint32_t (*)(void), "CSharp_GetBufferTick")();                                    // 33: 0 (or the backend's)
    W(void (*)(void *, const char *), "CSharp_AkExternalSourceInfo_szFile_set")(s, "x.wem");
    o[k++] = (uintptr_t)W(const char *(*)(void *), "CSharp_AkExternalSourceInfo_szFile_get")(s);     // 34: 0 (no string back)
    W(void (*)(void *), "CSharp_delete_AkVector")(v);
    W(void (*)(void *), "CSharp_delete_AkTransform")(t);
    W(void (*)(void), "CSharp_Term")();
    o[k++] = W(int (*)(void), "CSharp_IsInitialized")();                                         // 35: 0
    return k;
}
