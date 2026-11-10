// a64fix_image.c - an image's code through a64fix. list: every instruction beyond ARMv8.0 (the
// image's address, the word, the kind), to hold against a disassembler's reading; fix: the
// rewrite as on an ARMv8.0 core, with what it found, rewrote and refused. No code of the image runs.
#include "../a64fix.h"
#include "../macho.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *resolve(const char *name, const char *lib, void *ctx)
{
    static long dummy;
    (void)name; (void)lib; (void)ctx;
    return &dummy;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "list") != 0 && strcmp(argv[1], "fix") != 0)) { fprintf(stderr, "usage: a64fix_image list|fix IMAGE\n"); return 2; }
    int list = strcmp(argv[1], "list") == 0;
    char err[512];
    size_t island = 2u << 20;
    struct macho_image *m = macho_load(argv[2], island, resolve, NULL, err, sizeof err);
    if (!m) { fprintf(stderr, "%s\n", err); return 1; }
    long n = macho_data_in_code(m, NULL, 0);
    if (n < 0) { fprintf(stderr, "data-in-code ranges outside the image\n"); return 1; }
    const uint8_t **skip = calloc((size_t)n * 2 + 2, sizeof *skip);
    if (!skip || macho_data_in_code(m, skip, n) != n) return 1;
    fprintf(stderr, "data in code: %ld ranges\n", n);
    if (!list) setenv("UA_A64FIX", "all", 1);
    struct a64fix f;
    a64fix_init(&f, m->island, m->island_bytes);
    const struct macho_section *s;
    for (int i = 0; (s = macho_code_section(m, i)); i++) {
        fprintf(stderr, "code: %s,%s %llu bytes (flags 0x%08x)\n", s->seg, s->name, (unsigned long long)s->size, s->flags);
        if (list) {
            long k = 0;
            for (const uint8_t *p = s->addr; p + 4 <= s->addr + s->size; p += 4) {
                while (k < n && p >= skip[2 * k + 1]) k++;
                if (k < n && p >= skip[2 * k]) continue;
                uint32_t w;
                memcpy(&w, p, 4);
                int kind = a64fix_kind(w);
                if (kind >= 0) printf("%llx %08x %s\n", (unsigned long long)((intptr_t)(uintptr_t)p - m->slide), w, a64fix_kind_name(kind));
            }
        } else if (a64fix_code(&f, s->addr, s->size, skip, (int)n) != 0) {
            fprintf(stderr, "the island is full or out of reach (%zu of %zu bytes used)\n", f.island_used, f.island_bytes);
            return 1;
        }
    }
    if (!list) {
        for (int k = 0; k < A64_NKIND; k++) if (f.found[k]) printf("%-24s %ld\n", a64fix_kind_name(k), f.found[k]);
        printf("rewritten %ld, refused %ld%s%s, island %zu bytes\n", f.rewritten, f.refused, f.refused ? ": " : "", f.refused_what, f.island_used);
        if (macho_protect(m) != 0) { perror("protect"); return 1; }
    }
    return 0;
}
