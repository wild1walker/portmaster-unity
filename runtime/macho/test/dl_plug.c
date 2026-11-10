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
#endif
