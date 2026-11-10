// darwin_dl.c - a test of dlopen for plugins that are Mach-O files (dl_plug.c, built by
// build-mac.sh beside this image): run on macOS natively and through the loader on Linux, its
// output must be the same on both. With the argument "probe", which only the loader's run is
// given: the names a game's code may have for a plugin (darwin_dl.probe).
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int failed;
static char dir[1024];
#define CHECK(name, ok) do { int ok_ = (ok); failed += !ok_; printf("%s %s\n", ok_ ? "ok  " : "FAIL", name); } while (0)

static const char *at(const char *leaf)
{
    static char buf[4][1200];
    static int next;
    char *p = buf[next++ & 3];
    snprintf(p, sizeof buf[0], "%s/%s", dir, leaf);
    return p;
}

static int call(void *handle, const char *name)
{
    int (*fn)(void) = handle ? (int (*)(void))dlsym(handle, name) : NULL;
    return fn ? fn() : -1;
}

static const void *added[16];
static int nadded, watching;
static void on_add(const struct mach_header *header, intptr_t slide)
{
    (void)slide;
    if (watching && nadded < 16) added[nadded++] = header;
}

static void *other_thread_tls(void *plug) { return (void *)(intptr_t)call(plug, "plug_tls_bump"); }

static struct { pthread_mutex_t m; pthread_cond_t c; int ready, go; } gate = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0 };
static void *racer(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&gate.m);
    gate.ready++;
    pthread_cond_broadcast(&gate.c);
    while (!gate.go) pthread_cond_wait(&gate.c, &gate.m);
    pthread_mutex_unlock(&gate.m);
    return dlopen(at("dl_race.dylib"), RTLD_NOW);
}

static void probe(void)
{
    static const char *const names[] = {
        "dl_plug", "dl_plug.dylib", "libdl_plug.so", "@rpath/dl_plug.dylib", "/data/app/x/lib/arm64/libdl_plug.so",
        "dl_bundle", "dl_bundle.bundle", "Plugins/dl_bundle.bundle", "libdl_bundle.so",
    };
    void *plug = dlopen(at("dl_plug.dylib"), RTLD_NOW), *bundle = dlopen(at("dl_bundle.bundle"), RTLD_NOW);
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        void *h = dlopen(names[i], RTLD_LAZY);
        printf("%s %s\n", h && h == (strstr(names[i], "bundle") ? bundle : plug) ? "ok  " : "FAIL", names[i]);
        failed += !(h && h == (strstr(names[i], "bundle") ? bundle : plug));
    }
    CHECK("a name no file has", dlopen("dl_nothing", RTLD_LAZY) == NULL && dlerror() != NULL);
    CHECK("opened once", call(plug, "plug_inits") == 1);
}

long test_main(long argc, char **argv)
{
    setvbuf(stdout, NULL, _IOFBF, 0);
    Dl_info me;
    if (!dladdr((void *)test_main, &me) || !me.dli_fname) { printf("FAIL dladdr of the test\n"); return 1; }
    snprintf(dir, sizeof dir, "%s", me.dli_fname);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else strcpy(dir, ".");
    if (argc > 1 && !strcmp(argv[1], "probe")) { probe(); printf("failed %d\n", failed); fflush(stdout); return failed; }

    _dyld_register_func_for_add_image(on_add);
    CHECK("not loaded: RTLD_NOLOAD", dlopen(at("dl_plug.dylib"), RTLD_NOLOAD) == NULL);
    CHECK("not loaded: its names", dlsym(RTLD_DEFAULT, "plug_value") == NULL);
    uint32_t before = _dyld_image_count();
    watching = 1;
    void *plug = dlopen(at("dl_plug.dylib"), RTLD_NOW);
    watching = 0;
    CHECK("dlopen", plug != NULL);
    if (!plug) { printf("     %s\nfailed %d\n", dlerror(), failed); fflush(stdout); return failed; }
    printf("images added %u, told of %d\n", _dyld_image_count() - before, nadded);
    const char *(*order)(void) = (const char *(*)(void))dlsym(plug, "dl_order_log");
    printf("initializers: %s\n", order ? order() : "?");
    printf("plug_value %d, inner_value %d, dep_value %d\n", call(plug, "plug_value"), call(plug, "plug_inner"), call(plug, "dep_value"));
    CHECK("its names: RTLD_DEFAULT", dlsym(RTLD_DEFAULT, "plug_value") != NULL && dlsym(RTLD_DEFAULT, "plug_value") == dlsym(plug, "plug_value"));
    CHECK("its dylib's names: RTLD_DEFAULT", dlsym(RTLD_DEFAULT, "dep_value") != NULL);
    CHECK("opened from the initializer", dlsym(RTLD_DEFAULT, "inner_value") != NULL);
    CHECK("a name it has not", dlsym(plug, "plug_nothing") == NULL && dlerror() != NULL);
    CHECK("again: the same handle", dlopen(at("dl_plug.dylib"), RTLD_LAZY) == plug);
    CHECK("another spelling: the same handle", dlopen(at("./dl_plug.dylib"), RTLD_LAZY) == plug);
    CHECK("loaded: RTLD_NOLOAD", dlopen(at("dl_plug.dylib"), RTLD_NOLOAD) == plug);
    printf("initialized %d, its dylib %d\n", call(plug, "plug_inits"), dlsym(plug, "dep_inits") ? *(int *)dlsym(plug, "dep_inits") : -1);

    Dl_info info;
    memset(&info, 0, sizeof info);
    void *value = dlsym(plug, "plug_value");
    int found = dladdr(value, &info);
    const char *leaf = found && info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;
    printf("dladdr %d %s %s\n", found, leaf ? leaf + 1 : "?", found && info.dli_sname ? info.dli_sname : "?");
    CHECK("dladdr: the symbol's address", found && info.dli_saddr == value);
    int told = 0;
    for (int i = 0; i < nadded; i++) told += found && added[i] == info.dli_fbase;
    CHECK("the add-image function heard of it", told == 1);

    int a = call(plug, "plug_tls_bump"), b = call(plug, "plug_tls_bump");
    pthread_t t;
    void *c = NULL;
    pthread_create(&t, NULL, other_thread_tls, plug);
    pthread_join(t, &c);
    printf("thread-local %d %d, another thread %d, back %d\n", a, b, (int)(intptr_t)c, call(plug, "plug_tls_bump"));

    void *bundle = dlopen(at("dl_bundle.bundle"), RTLD_LAZY);
    printf("bundle %d\n", call(bundle, "bundle_value"));

    void *local = dlopen(at("dl_local.dylib"), RTLD_NOW | RTLD_LOCAL);
    printf("local %d\n", call(local, "local_value"));
    CHECK("RTLD_LOCAL: not among RTLD_DEFAULT's", dlsym(RTLD_DEFAULT, "local_value") == NULL);
    CHECK("opened again RTLD_GLOBAL: the same handle", dlopen(at("dl_local.dylib"), RTLD_NOW | RTLD_GLOBAL) == local);
    CHECK("then among RTLD_DEFAULT's", dlsym(RTLD_DEFAULT, "local_value") != NULL);

    CHECK("no such file", dlopen(at("dl_none.dylib"), RTLD_NOW) == NULL);
    CHECK("dlerror says so", dlerror() != NULL);
    CHECK("dlerror says it once", dlerror() == NULL);
    CHECK("a name nothing defines: no handle", dlopen(at("dl_bad.dylib"), RTLD_NOW) == NULL && dlerror() != NULL);
    CHECK("and none when asked again", dlopen(at("dl_bad.dylib"), RTLD_NOW) == NULL && dlerror() != NULL);
    CHECK("its names are not there", dlsym(RTLD_DEFAULT, "bad_value") == NULL);
    CHECK("fails after its dylib loaded: no handle", dlopen(at("dl_bad2.dylib"), RTLD_NOW) == NULL && dlerror() != NULL);
    CHECK("the dylib is gone with it", dlsym(RTLD_DEFAULT, "dep2_inits") == NULL && dlopen(at("dl_dep2.dylib"), RTLD_NOLOAD) == NULL);
    void *dep2 = dlopen(at("dl_dep2.dylib"), RTLD_NOW);
    printf("the dylib alone: %d, initialized %d\n", call(dep2, "dep2_value"), dep2 && dlsym(dep2, "dep2_inits") ? *(int *)dlsym(dep2, "dep2_inits") : -1);

    void *fix = dlopen(at("dl_fix.dylib"), RTLD_NOW);
    printf("pointers the loader sets: its own %d, another image's %d, the rest %#x\n",
           call(fix, "fix_mine"), call(fix, "fix_theirs"), call(fix, "fix_rest"));

    CHECK("not loaded yet: RTLD_NOLOAD", dlopen(at("dl_late.dylib"), RTLD_NOLOAD) == NULL);
    void *late = dlopen(at("dl_late.dylib"), RTLD_LAZY);
    CHECK("then loaded", late != NULL && dlopen(at("dl_late.dylib"), RTLD_NOLOAD) == late);

    pthread_t r[2];
    void *h[2] = { NULL, NULL };
    for (int i = 0; i < 2; i++) pthread_create(&r[i], NULL, racer, NULL);
    pthread_mutex_lock(&gate.m);
    while (gate.ready < 2) pthread_cond_wait(&gate.c, &gate.m);
    gate.go = 1;
    pthread_cond_broadcast(&gate.c);
    pthread_mutex_unlock(&gate.m);
    for (int i = 0; i < 2; i++) pthread_join(r[i], &h[i]);
    CHECK("two threads at once: one image", h[0] != NULL && h[0] == h[1]);
    printf("initialized %d\n", call(h[0], "race_inits"));

    void *self = dlopen(NULL, RTLD_LAZY);
    CHECK("dlopen(NULL)", self != NULL && dlsym(self, "test_main") == (void *)test_main);
    printf("dlclose %d %d\n", dlclose(plug), dlclose(bundle));
    CHECK("still there", call(plug, "plug_value") == 8);
    printf("failed %d\n", failed);
    fflush(stdout);
    return failed;
}
