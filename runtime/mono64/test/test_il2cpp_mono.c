// test_il2cpp_mono <libil2cpp_mono.so>: the bridge against fake_mono.c (UA_MONO64_RUNTIME):
// internal calls registered before il2cpp_init reach Mono after it, method lookup by name searches
// the parents (IL2CPP's semantics), the write barrier stores and marks only heap fields, 32-bit GC
// handles come back zero-extended, and a thread's first allocation attaches it.
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fclass { const char *name; struct fclass *parent; const char *method; } fclass;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void *bridge;
#define B(type, name) __typeof__(type) name = (type)dlsym(bridge, #name)

static void *thread_main(void *arg)
{
    (void)arg;
    void *(*string_new)(const char *) = (void *(*)(const char *))dlsym(bridge, "il2cpp_string_new");
    string_new("x");
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    setenv("UA_UNITY_VERSION", "2019.4.8f1", 1);
    bridge = dlopen(argv[1], RTLD_NOW);
    if (!bridge) { printf("FAIL %s\n", dlerror()); return 1; }
    void *mono = dlopen(getenv("UA_MONO64_RUNTIME"), RTLD_NOW | RTLD_NOLOAD);
    int *icalls = (int *)dlsym(mono, "fake_icalls"), *attaches = (int *)dlsym(mono, "fake_attaches");
    int *nostore = (int *)dlsym(mono, "fake_nostore");
    const char **last = (const char **)dlsym(mono, "fake_last_icall");
    const char **dirs = (const char **)dlsym(mono, "fake_dirs_assembly");

    B(void (*)(const char *, void *), il2cpp_add_internal_call);
    B(void *(*)(const char *), il2cpp_resolve_icall);
    B(void (*)(const char *), il2cpp_set_data_dir);
    B(int (*)(const char *), il2cpp_init);
    B(void *(*)(fclass *, const char *, int), il2cpp_class_get_method_from_name);
    B(void (*)(void *, void **, void *), il2cpp_gc_wbarrier_set_field);
    B(uintptr_t (*)(void *, int), il2cpp_gchandle_new);
    B(uint32_t (*)(void), il2cpp_object_header_size);

    // internal calls: queued before il2cpp_init (libunity registers them first), passed on at it
    il2cpp_add_internal_call("UnityEngine.Transform::get_position_Injected", (void *)main);
    CHECK(*icalls == 0, "an internal call reached Mono before il2cpp_init (%d)", *icalls);
    il2cpp_set_data_dir("/nonexistent/il2cpp");
    CHECK(il2cpp_init("IL2CPP Root Domain") == 1, "il2cpp_init");
    CHECK(*icalls == 1 && *last && !strcmp(*last, "UnityEngine.Transform::get_position_Injected"), "queued icall not registered (%d)", *icalls);
    CHECK(*dirs && !strcmp(*dirs, "/nonexistent/il2cpp"), "assemblies dir %s", *dirs ? *dirs : "(null)");
    il2cpp_add_internal_call("UnityEngine.Time::get_deltaTime", (void *)thread_main);
    CHECK(*icalls == 2, "icall after init not registered at once");
    CHECK(il2cpp_resolve_icall("UnityEngine.Time::get_deltaTime") == (void *)thread_main, "il2cpp_resolve_icall");

    // IL2CPP looks a method up in the class and then its parents
    fclass base = { "Base", NULL, "Awake" }, derived = { "Derived", &base, NULL };
    CHECK(il2cpp_class_get_method_from_name(&derived, "Awake", 0) == &base, "method from a parent not found");
    CHECK(il2cpp_class_get_method_from_name(&derived, "Update", 0) == NULL, "method that is nowhere found");

    // write barrier: always the store; Mono's barrier (incremental collector) only for heap objects
    void *slot = NULL, *obj = &slot, *val = (void *)0x1234;
    il2cpp_gc_wbarrier_set_field(NULL, &slot, val);
    CHECK(slot == val && *nostore == 0, "stack store: slot %p, barriers %d", slot, *nostore);
    il2cpp_gc_wbarrier_set_field(obj, &slot, NULL);
    CHECK(slot == NULL && *nostore == 1, "heap store: slot %p, barriers %d", slot, *nostore);

    // 32-bit handles (Mono without *_v2), zero-extended as 2019.4's libunity reads them
    CHECK(il2cpp_gchandle_new(obj, 1) == 0x80000001u, "gchandle %lx", (unsigned long)il2cpp_gchandle_new(obj, 1));
    CHECK(il2cpp_object_header_size() == 16, "object header %u", il2cpp_object_header_size());

    // a thread's first managed call attaches it
    int before = *attaches;
    pthread_t t;
    pthread_create(&t, NULL, thread_main, NULL);
    pthread_join(t, NULL);
    CHECK(*attaches == before + 1, "thread not attached (%d -> %d)", before, *attaches);

    printf("%s\n", fails ? "FAILED" : "test_il2cpp_mono: OK");
    return fails != 0;
}
