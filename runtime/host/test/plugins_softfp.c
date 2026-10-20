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
