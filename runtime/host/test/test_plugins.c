// plugins.c's unit test (runtime/host/test/plugins_test.sh):
//   test_plugins thunks              softfp -> hard-float thunks for 10 signature shapes (32-bit ARM;
//                                    on AArch64 plugins_thunk must return the function itself)
//   test_plugins paths <tmp>         StreamingAssets paths -> files beside the APK
//   test_plugins lib <libdir> <root> the FMOD test double in <libdir> through plugins.map, the loader
//                                    and (32-bit ARM, glibc build) the thunks; stubs and refusals
#include "../plugins.h"
#include "../../loader/so_loader.h"

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
void sf_thunk_calls(void *const *fn, uint64_t *ret);
int sf_fmod_calls(void *create, void *getvca, void *setvol, void *loadbank, void *bits, const char *bank, uint32_t *out);
int sf_discord(void *create);

// hard-float targets: each checks what it received
static int ok[10];
static int t0(int a, float b) { ok[0] = a == 7 && b == 1.5f; return 40; }
static int t1(int a, long long b, float c) { ok[1] = a == 3 && b == 0x1122334455667788LL && c == -2.25f; return 41; }
static int t2(void *p, float a, float b, float c, float d, float e, float f, float g, float h)
{ ok[2] = p == (void *)0x1234 && a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6 && g == 7 && h == 8; return 42; }
static int t3(float a, double b, float c) { ok[3] = a == .5f && b == 3.25 && c == 4.75f; return 43; }
static int t4(void *p, id2 id, float v, int i) { ok[4] = p == (void *)0x5678 && id.a == 0xaabb && id.b == 0xccdd && v == .125f && i == 1; return 44; }
static int t5(vec3 v, int i) { ok[5] = v.x == 1 && v.y == 2 && v.z == 3 && i == 9; return 45; }
static double t6(double a, double b, double c, double d, double e, double f, double g, double h, double i)
{ ok[6] = a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6 && g == 7 && h == 8 && i == 9; return a + b + c + d + e + f + g + h + i; }
static float t7(float a, float b) { ok[7] = a == 1.5f && b == 4.f; return a * b; }
static int t8(int a, int b, int c, int d, int e, int f, float f0, float f1, float f2, float f3, float f4, float f5, float f6,
              float f7, float f8, float f9, float f10, float f11, float f12, float f13, float f14, float f15, float f16, float f17)
{
    float v[18] = { f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17 };
    int good = a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6;
    for (int i = 0; i < 18; i++) good &= v[i] == .5f * (i + 1);
    ok[8] = good;
    return 48;
}
static int t9(double a, int b, float c, double d, float e, float f) { ok[9] = a == 1.25 && b == 11 && c == 2.5f && d == 3.75 && e == 5.5f && f == 6.25f; return 49; }

static int test_thunks(void)
{
    struct { void *fn; const char *ret, *args; } s[10] = {
        { (void *)t0, "i", "i,f" }, { (void *)t1, "i", "i,l,f" }, { (void *)t2, "i", "i,f,f,f,f,f,f,f,f" },
        { (void *)t3, "i", "f,d,f" }, { (void *)t4, "i", "i,s2,f,i" }, { (void *)t5, "i", "F3,i" },
        { (void *)t6, "d", "d,d,d,d,d,d,d,d,d" }, { (void *)t7, "f", "f,f" },
        { (void *)t8, "i", "i,i,i,i,i,i,f,f,f,f,f,f,f,f,f,f,f,f,f,f,f,f,f,f" }, { (void *)t9, "i", "d,i,f,d,f,f" },
    };
    void *fn[10];
    for (int i = 0; i < 10; i++) {
        fn[i] = plugins_thunk(s[i].fn, s[i].ret, s[i].args);
        CHECK(fn[i] != NULL, "thunk %d", i);
#if !(defined(__arm__) && defined(__ARM_PCS_VFP))
        CHECK(fn[i] == s[i].fn, "thunk %d: AArch64 needs none", i);
#endif
    }
    CHECK(plugins_thunk((void *)t0, "i", "i,i") == (void *)t0, "no floats: the function itself");
    CHECK(plugins_thunk((void *)t0, "x", "i") == NULL, "bad result class");
    CHECK(plugins_thunk((void *)t0, "i", "i,F9") == NULL, "too big an aggregate");
    uint64_t ret[10];
    memset(ret, 0, sizeof ret);
    sf_thunk_calls(fn, ret);
    for (int i = 0; i < 10; i++) {
        CHECK(ok[i], "signature %d (%s): arguments arrived wrong", i, s[i].args);
        if (i != 6 && i != 7) CHECK((uint32_t)ret[i] == (uint32_t)(40 + i), "signature %d: result %u", i, (uint32_t)ret[i]);
    }
    double d; float f;
    memcpy(&d, &ret[6], 8);
    uint32_t fb = (uint32_t)ret[7];
    memcpy(&f, &fb, 4);
    CHECK(d == 45, "double result %g", d);
    CHECK(f == 6, "float result %g", f);
    return 0;
}

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

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = argc > 1 ? argv[1] : "thunks";
    if (!strcmp(mode, "thunks")) test_thunks();
    else if (!strcmp(mode, "paths") && argc > 2) test_paths(argv[2]);
    else if (!strcmp(mode, "lib") && argc > 3) test_lib(argv[2], argv[3]);
    else { fprintf(stderr, "usage: test_plugins thunks | paths <tmp> | lib <libdir> <root>\n"); return 2; }
    printf("%s: %s (%d failures)\n", mode, fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
