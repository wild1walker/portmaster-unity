// dl_plug.c - the plugins darwin_dl.c opens: one source, built once per -D (build-mac.sh).
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(DL_DEP)             // dl_dep.dylib: what dl_plug and dl_inner need
static char order[128];
int dep_inits;
void dl_order_add(const char *s) { strlcat(order, s, sizeof order); }
const char *dl_order_log(void) { return order; }
int dep_value(void) { return 7; }
__attribute__((constructor)) static void init(void) { dep_inits++; dl_order_add("dep "); }

#elif defined(DL_PLUG)          // dl_plug.dylib: needs dl_dep; its initializer opens dl_inner
void dl_order_add(const char *s);
int dep_value(void);
static int inits, inner;
static __thread int tls = 5;
int plug_value(void) { return dep_value() + 1; }
int plug_inits(void) { return inits; }
int plug_inner(void) { return inner; }
int plug_tls_bump(void) { return ++tls; }
__attribute__((constructor)) static void init(void)
{
    inits++;
    dl_order_add("plug[ ");
    Dl_info me;
    char path[1024];
    if (dladdr((void *)plug_value, &me) && me.dli_fname) {
        snprintf(path, sizeof path, "%s", me.dli_fname);
        char *slash = strrchr(path, '/');
        snprintf(slash ? slash + 1 : path, 32, "dl_inner.dylib");
        void *h = dlopen(path, RTLD_NOW);
        int (*value)(void) = h ? (int (*)(void))dlsym(h, "inner_value") : NULL;
        inner = value ? value() : -1;
    }
    dl_order_add("] ");
}

#elif defined(DL_INNER)         // dl_inner.dylib: opened from within dl_plug's initializer
void dl_order_add(const char *s);
int inner_value(void) { return 11; }
__attribute__((constructor)) static void init(void) { dl_order_add("inner "); }

#elif defined(DL_BUNDLE)        // dl_bundle.bundle, dl_local.dylib, dl_late.dylib: stand alone
int bundle_value(void) { return 21; }
#elif defined(DL_LOCAL)
int local_value(void) { return 31; }
#elif defined(DL_LATE)
int late_value(void) { return 41; }

#elif defined(DL_RACE)          // dl_race.dylib: two threads open it at once
static atomic_int inits;
int race_inits(void) { return inits; }
__attribute__((constructor)) static void init(void) { usleep(50000); inits++; }

#elif defined(DL_BAD)           // dl_bad.dylib: a name nothing defines
extern int dl_no_such_variable;
int *bad_ptr = &dl_no_such_variable;
int bad_value(void) { return *bad_ptr; }

#elif defined(DL_DEP2)          // dl_dep2.dylib: what dl_bad2 needs
int dep2_inits;
int dep2_value(void) { return 51; }
__attribute__((constructor)) static void init(void) { dep2_inits++; }

#elif defined(DL_BAD2)          // dl_bad2.dylib: fails after its dylib was loaded
extern int dl_no_such_variable;
int dep2_value(void);
int *bad2_ptr = &dl_no_such_variable;
int bad2_value(void) { return *bad2_ptr + dep2_value(); }

#elif defined(DL_FIXDEP)        // dl_fixdep.dylib: what dl_fix points into
int fix_table[6000];
int fix_twice(int v) { return 2 * v; }
__attribute__((constructor)) static void init(void) { for (int i = 0; i < 6000; i++) fix_table[i] = 3 * i + 1; }

#elif defined(DL_FIX)           // dl_fix.dylib: the pointers a linker leaves for the loader to set
extern int fix_table[6000];
int fix_twice(int v);
extern int fix_absent __attribute__((weak_import));     // nothing defines it: its address is 0
static int own[4096];
static int own_twice(int v) { return 2 * v; }
#define A1(i) &own[i],
#define B1(i) &fix_table[5 * (i)],
#define X4(X, i) X(i) X((i) + 1) X((i) + 2) X((i) + 3)
#define X16(X, i) X4(X, i) X4(X, (i) + 4) X4(X, (i) + 8) X4(X, (i) + 12)
#define X64(X, i) X16(X, i) X16(X, (i) + 16) X16(X, (i) + 32) X16(X, (i) + 48)
#define X256(X, i) X64(X, i) X64(X, (i) + 64) X64(X, (i) + 128) X64(X, (i) + 192)
#define X1024(X, i) X256(X, i) X256(X, (i) + 256) X256(X, (i) + 512) X256(X, (i) + 768)
// Pages full of pointers to the image's own data, pages with none, then pointers into another
// image at every distance from its symbol, small and large. (None is static: a pointer the
// compiler can see all uses of is one it need not leave to the loader.)
struct {
    int *mine[4096];
    char none[50000];
    int *theirs[1024];
    char odd[3];
    int *last;
} big = { { X1024(A1, 0) X1024(A1, 1024) X1024(A1, 2048) X1024(A1, 3072) }, { 1 }, { X1024(B1, 0) }, { 2 }, &own[4095] };
int *const near = &fix_table[3];
int *far = &fix_table[5999];
int *before = fix_table - 2;
int *absent = &fix_absent;
int (*const theirs_fn)(int) = fix_twice;
int (*mine_fn)(int) = own_twice;
const char *const texts[] = { "one", "two", "three" };
__thread int *tls_ptr = &own[9];
__attribute__((constructor)) static void init(void) { for (int i = 0; i < 4096; i++) own[i] = i ^ 0x155; }

int fix_mine(void)
{
    int s = 0;
    for (int i = 0; i < 4096; i++) s += big.mine[i] == &own[i] ? *big.mine[i] : -100000;
    return s + (big.last == &own[4095]) + big.none[0] + big.none[49999] + big.odd[0];
}

int fix_theirs(void)
{
    int s = 0;
    for (int i = 0; i < 1024; i++) s += big.theirs[i] == &fix_table[5 * i] ? *big.theirs[i] : -100000;
    return s;
}

// One bit for each of the single pointers that is right.
int fix_rest(void)
{
    return (near == &fix_table[3] && *near == 10) | (far == &fix_table[5999] && *far == 17998) << 1 |
           (before + 2 == fix_table) << 2 | (absent == NULL) << 3 | (theirs_fn(4) == 8) << 4 |
           (mine_fn(5) == 10) << 5 | (!strcmp(texts[0], "one") && !strcmp(texts[2], "three")) << 6 |
           (tls_ptr == &own[9] && *tls_ptr == (9 ^ 0x155)) << 7;
}
#endif
