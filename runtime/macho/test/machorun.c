// machorun.c - runs test code built for macOS through the Mach-O loader and the Darwin shims:
//   machorun [--cover NAMES] IMAGE.dylib... [-- ARG...]
// The images are loaded in the order given (libc++abi and libc++ before what uses them),
// linked, their initializers run; the last one's test_main(argc, argv) is called and its
// return value is this program's. What the test dlopens by name is looked for beside the
// last image. --cover NAMES: every import name in the file (nm's
// "_name" at the start of a line) must have an answer.
#define _GNU_SOURCE
#include "../darwin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

static int cover(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 1; }
    char line[512];
    int n = 0, missing = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, " \t\r\n")] = 0;
        if (!line[0]) continue;
        n++;
        if (!darwin_lookup(line)) { printf("no answer: %s\n", line); missing++; }
    }
    fclose(f);
    printf("%s: %d names, %d without an answer (%d shims)\n", path, n, missing, darwin_shim_count());
    return missing != 0;
}

int main(int argc, char **argv)
{
    char err[512];
    struct darwin_image *last = NULL;
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "--cover")) {
        if (cover(argv[i + 1])) return 1;
        i += 2;
    }
    if (i >= argc) return 0;
    darwin_thread_enter();
    for (; i < argc && strcmp(argv[i], "--") != 0; i++) {
        last = darwin_dyld_load(argv[i], err, sizeof err);
        if (!last) { fprintf(stderr, "machorun: %s: %s\n", argv[i], err); return 2; }
    }
    if (!last) { fprintf(stderr, "machorun: no image\n"); return 2; }
    char **targv = i < argc ? argv + i : argv + argc - 1;      // "--" or the image stands for argv[0]
    int targc = (int)(argv + argc - targv);
    darwin_dyld_set_args(targc, targv, environ);
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", last->m->path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else strcpy(dir, ".");
    darwin_dyld_set_plugin_dir(dir);
    if (darwin_dyld_link(err, sizeof err) != 0) { fprintf(stderr, "machorun: %s\n", err); return 2; }
    darwin_dyld_run_initializers();
    long (*test_main)(long, char **) = (long (*)(long, char **))macho_export(last->m, "_test_main");
    if (!test_main) { fprintf(stderr, "machorun: no test_main in the last image\n"); return 2; }
    int r = (int)test_main(targc, targv);
    darwin_tlv_thread_exit();
    fflush(NULL);
    return r;
}
