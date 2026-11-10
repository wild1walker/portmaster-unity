// il2cpprun.c - starts a macOS game's IL2CPP runtime through the Mach-O loader, without the
// engine: the first thing that must work before a player is put around it.
//   il2cpprun DATA_DIR IMAGE.dylib...
// DATA_DIR is the game's il2cpp_data (Metadata/global-metadata.dat inside). The images are
// loaded in the order given, GameAssembly last. After il2cpp_init a few methods of the
// game's own mscorlib are called (parsing, strings, an exception, allocation with
// collections that stop the other threads), and what they return is checked.
#define _GNU_SOURCE
#include "../darwin.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern char **environ;

static struct darwin_image *ga;
static int failed;

static void *api(const char *name)
{
    char sym[128];
    snprintf(sym, sizeof sym, "_%s", name);
    void *p = macho_export(ga->m, sym);
    if (!p) { fprintf(stderr, "il2cpprun: the image exports no %s\n", name); exit(2); }
    return p;
}

static void check(const char *name, int ok)
{
    failed += !ok;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    fflush(stdout);
}

static double ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000 + (double)t.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    char err[512];
    if (argc < 3) { fprintf(stderr, "usage: il2cpprun DATA_DIR IMAGE.dylib...\n"); return 2; }
    darwin_thread_enter();
    double t0 = ms_now();
    for (int i = 2; i < argc; i++) {
        ga = darwin_dyld_load(argv[i], err, sizeof err);
        if (!ga) { fprintf(stderr, "il2cpprun: %s: %s\n", argv[i], err); return 2; }
    }
    darwin_dyld_set_args(1, argv + argc - 1, environ);
    if (darwin_dyld_link(err, sizeof err) != 0) { fprintf(stderr, "il2cpprun: %s\n", err); return 2; }
    double t1 = ms_now();
    darwin_dyld_run_initializers();
    double t2 = ms_now();
    printf("loaded and linked in %.0f ms, initializers %.0f ms\n", t1 - t0, t2 - t1);
    fflush(stdout);

    void (*set_data_dir)(const char *) = api("il2cpp_set_data_dir");
    void (*set_config_dir)(const char *) = api("il2cpp_set_config_dir");
    long (*init)(const char *) = api("il2cpp_init");
    void *(*domain_get)(void) = api("il2cpp_domain_get");
    void *(*assembly_open)(void *, const char *) = api("il2cpp_domain_assembly_open");
    void *(*assembly_image)(void *) = api("il2cpp_assembly_get_image");
    void *(*class_from_name)(void *, const char *, const char *) = api("il2cpp_class_from_name");
    void *(*method_from_name)(void *, const char *, long) = api("il2cpp_class_get_method_from_name");
    void *(*invoke)(void *, void *, void **, void **) = api("il2cpp_runtime_invoke");
    void *(*string_new)(const char *) = api("il2cpp_string_new");
    uint16_t *(*string_chars)(void *) = api("il2cpp_string_chars");
    int (*string_length)(void *) = api("il2cpp_string_length");
    void *(*unbox)(void *) = api("il2cpp_object_unbox");
    void (*gc_collect)(long) = api("il2cpp_gc_collect");
    int64_t (*gc_used)(void) = api("il2cpp_gc_get_used_size");
    void *(*object_class)(void *) = api("il2cpp_object_get_class");
    const char *(*class_name)(void *) = api("il2cpp_class_get_name");

    char etc[1024];
    snprintf(etc, sizeof etc, "%s/etc", argv[1]);
    set_data_dir(argv[1]);
    set_config_dir(etc);
    long ok = init("IL2CPP Root Domain");
    double t3 = ms_now();
    printf("il2cpp_init: %ld, %.0f ms\n", ok, t3 - t2);
    fflush(stdout);
    if (!ok) return 1;

    void *domain = domain_get();
    void *corlib_asm = assembly_open(domain, "mscorlib");
    check("mscorlib", corlib_asm != NULL);
    if (!corlib_asm) return 1;
    void *corlib = assembly_image(corlib_asm);
    void *exc = NULL;

    // int.Parse("12345")
    void *int32 = class_from_name(corlib, "System", "Int32");
    void *parse = int32 ? method_from_name(int32, "Parse", 1) : NULL;
    check("System.Int32.Parse found", parse != NULL);
    if (parse) {
        void *args[1] = { string_new("12345") };
        void *boxed = invoke(parse, NULL, args, &exc);
        check("int.Parse(\"12345\") == 12345", !exc && boxed && *(int32_t *)unbox(boxed) == 12345);
        exc = NULL;
        args[0] = string_new("not a number");
        boxed = invoke(parse, NULL, args, &exc);        // a managed exception, thrown and caught as C++
        check("int.Parse of a word throws FormatException", exc && !strcmp(class_name(object_class(exc)), "FormatException"));
        exc = NULL;
    }

    // string.Concat(a, b), double.Parse, ToString
    void *string = class_from_name(corlib, "System", "String");
    void *concat = string ? method_from_name(string, "Concat", 2) : NULL;
    if (concat) {
        void *args[2] = { string_new("Hello, "), string_new("world!") };
        void *s = invoke(concat, NULL, args, &exc);
        int same = !exc && s && string_length(s) == 13;
        const char *want = "Hello, world!";
        for (int i = 0; same && i < 13; i++) same = string_chars(s)[i] == (uint16_t)want[i];
        check("string.Concat", same);
        exc = NULL;
    } else check("System.String.Concat found", 0);
    void *dbl = class_from_name(corlib, "System", "Double");
    void *dparse = dbl ? method_from_name(dbl, "Parse", 1) : NULL;
    if (dparse) {
        void *args[1] = { string_new("2.5") };
        void *boxed = invoke(dparse, NULL, args, &exc);
        check("double.Parse(\"2.5\") == 2.5", !exc && boxed && *(double *)unbox(boxed) == 2.5);
        exc = NULL;
    } else check("System.Double.Parse found", 0);

    // Environment: the tick count moves, the machine has a name
    void *env = class_from_name(corlib, "System", "Environment");
    void *ticks = env ? method_from_name(env, "get_TickCount", 0) : NULL;
    if (ticks) {
        void *a = invoke(ticks, NULL, NULL, &exc);
        check("Environment.TickCount", !exc && a);
        exc = NULL;
    }
    void *nl = env ? method_from_name(env, "get_ProcessorCount", 0) : NULL;
    if (nl) {
        void *a = invoke(nl, NULL, NULL, &exc);
        check("Environment.ProcessorCount > 0", !exc && a && *(int32_t *)unbox(a) > 0);
        exc = NULL;
    }

    // allocation and collections: the collector stops the runtime's other threads (finalizer)
    int64_t before = gc_used();
    int alive = 1;
    for (int round = 0; round < 20 && alive; round++) {
        for (int i = 0; i < 20000 && concat; i++) {
            void *args[2] = { string_new("some text to throw away, "), string_new("and more of it") };
            alive = invoke(concat, NULL, args, &exc) != NULL && !exc;
        }
        gc_collect(0);
    }
    int64_t after = gc_used();
    printf("heap %lld -> %lld bytes after 400000 strings and 20 collections\n", (long long)before, (long long)after);
    check("allocation with collections", alive && after < before + 64 * 1024 * 1024);

    printf("failed %d, %.0f ms in all\n", failed, ms_now() - t0);
    fflush(NULL);
    // no il2cpp_shutdown: a player ends its process the same way
    _Exit(failed != 0);
}
