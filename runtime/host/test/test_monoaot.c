// monoaot.c's check (runtime/host/test/plugins_test.sh): UA_MONO_AOT's path rewrite (an image served from
// the folder when it is there, nothing else touched) and UA_MONO_MAIN through a stand-in Mono: the file maps it
// installs read a file, UA_MONO_DIRS reaches mono_set_dirs, the arguments reach mono_main, its result returns.
// Usage: test_monoaot <writable dir>
#include "../monoaot.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// the stand-in Mono
typedef void *(*fm_open_fn)(const char *);
typedef uint64_t (*fm_size_fn)(void *);
typedef int (*fm_fd_fn)(void *);
typedef int (*fm_close_fn)(void *);
typedef void *(*fm_map_fn)(size_t, int, int, uint64_t, void **);
typedef int (*fm_unmap_fn)(void *, void *);
static fm_open_fn g_open; static fm_size_fn g_size; static fm_fd_fn g_fd; static fm_close_fn g_close;
static fm_map_fn g_map; static fm_unmap_fn g_unmap;
static char g_root[256], g_cfg[256], g_argv[8][256];
static int g_argc, g_set_dirs;
static void f_override(void *o, void *s, void *f, void *c, void *m, void *u)
{
    g_open = (fm_open_fn)o; g_size = (fm_size_fn)s; g_fd = (fm_fd_fn)f; g_close = (fm_close_fn)c; g_map = (fm_map_fn)m; g_unmap = (fm_unmap_fn)u;
}
static void f_set_dirs(const char *root, const char *cfg)
{
    g_set_dirs++;
    snprintf(g_root, sizeof g_root, "%s", root ? root : "(null)");
    snprintf(g_cfg, sizeof g_cfg, "%s", cfg ? cfg : "(null)");
}
static int f_main(int argc, char **argv)
{
    g_argc = argc;
    for (int i = 0; i < argc && i < 8; i++) snprintf(g_argv[i], sizeof g_argv[i], "%s", argv[i]);
    return 7;
}
static int with_main = 1;
static void *fake_sym(void *h, const char *n)
{
    (void)h;
    if (!strcmp(n, "mono_main")) return with_main ? (void *)f_main : NULL;
    if (!strcmp(n, "mono_set_dirs")) return (void *)f_set_dirs;
    if (!strcmp(n, "mono_file_map_override")) return (void *)f_override;
    return NULL;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    char path[512], buf[512];

    // UA_MONO_AOT: an <assembly>.dll.so from the folder when it is there
    snprintf(path, sizeof path, "%s/Game.dll.so", dir);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL, "cannot write %s", path);
    if (f) { fputs("0123456789abcdef", f); fclose(f); }
    unsetenv("UA_MONO_AOT");
    CHECK(!monoaot_rewrite("/x.obb/assets/bin/Data/Managed/Game.dll.so", buf, sizeof buf), "nothing served without UA_MONO_AOT");
    setenv("UA_MONO_AOT", dir, 1);
    const char *r = monoaot_rewrite("/x.obb/assets/bin/Data/Managed/Game.dll.so", buf, sizeof buf);
    CHECK(r && !strcmp(r, path), "the image from the folder: %s", r ? r : "(null)");
    CHECK(!monoaot_rewrite("/x.obb/assets/bin/Data/Managed/Other.dll.so", buf, sizeof buf), "a missing image is not served");
    CHECK(!monoaot_rewrite("/x/lib/libunity.so", buf, sizeof buf) && !monoaot_rewrite("/x/Game.dll", buf, sizeof buf),
          "other libraries and files pass through");
    char small[8];
    CHECK(!monoaot_rewrite("/x/Game.dll.so", small, sizeof small), "a path longer than the buffer is not served");

    // UA_MONO_MAIN through the stand-in
    setenv("UA_MONO_DIRS", "/r/lib:/r/etc", 1);
    int rc = monoaot_main((void *)1, fake_sym, "--runtime=v2.0.50727  --aot=nodebug /m/Game.dll");
    CHECK(rc == 7, "mono_main's result: %d", rc);
    CHECK(g_argc == 4 && !strcmp(g_argv[0], "mono") && !strcmp(g_argv[1], "--runtime=v2.0.50727") &&
          !strcmp(g_argv[2], "--aot=nodebug") && !strcmp(g_argv[3], "/m/Game.dll"), "the arguments: %d", g_argc);
    CHECK(g_set_dirs == 1 && !strcmp(g_root, "/r/lib") && !strcmp(g_cfg, "/r/etc"), "mono_set_dirs(%s, %s)", g_root, g_cfg);
    CHECK(g_open && g_size && g_fd && g_close && g_map && g_unmap, "the file maps installed");
    if (g_open) {
        void *m = g_open(path);
        CHECK(m != NULL && g_size(m) == 16, "open and size");
        void *h = NULL;
        char *p = m ? g_map(16, 1 | 16, g_fd(m), 0, &h) : NULL;     // READ | PRIVATE
        CHECK(p && !memcmp(p, "0123456789abcdef", 16), "the map reads the file");
        if (p) CHECK(g_unmap(p, h) == 0, "unmap");
        if (m) CHECK(g_close(m) == 0, "close");
        CHECK(g_open("/no/such/file") == NULL, "a missing file: no map");
    }
    unsetenv("UA_MONO_DIRS");
    with_main = 0;
    CHECK(monoaot_main((void *)1, fake_sym, "--version") == 1, "no mono_main: 1");
    unlink(path);
    printf("monoaot: %s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
