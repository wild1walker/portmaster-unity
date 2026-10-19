/* test_load.c - load + relocate libmono/libmain/libunity, report unresolved
 * imports and find JNI_OnLoad. Does not run init arrays (needs bionic_shims). */
#include <stdio.h>
#include <stdlib.h>
#include "so_loader.h"

static void print_unres(const char *name, int is_func, void *ud)
{
    (void)ud;
    printf("    %s%s\n", name, is_func ? "" : " (data)");
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "test";
    const char *libs[] = { "libmono.so", "libmain.so", "libunity.so" };
    char path[512];
    int fail = 0;
    so_set_library_path(dir);
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, libs[i]);
        so_module *m = so_load(path, NULL);
        if (!m) { printf("FAIL %s: %s\n", libs[i], so_error()); fail = 1; continue; }
        printf("OK   %s base=%p size=0x%zx unresolved=%d JNI_OnLoad=%p\n", libs[i],
               (void *)so_base(m), so_size(m), so_unresolved_count(m), so_sym(m, "JNI_OnLoad"));
        so_foreach_unresolved(m, print_unres, NULL);
    }
    return fail;
}
