// load_probe.c - loads an image with every import answered by one dummy address and prints what
// the loader read: counts, sections, a few exports (no code of the image runs).
#include "../macho.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_n;
static void *any(const char *name, const char *lib, void *ctx)
{
    static char dummy[64];
    if (getenv("PROBE_NAMES")) printf("  import %s (%s)\n", name, lib);
    g_n++;
    return dummy;
}

static void count(const char *n, void *a, void *ctx) { (*(int *)ctx)++; }

int main(int argc, char **argv)
{
    char err[512];
    struct macho_image *m = macho_load(argv[1], 2 << 20, any, NULL, err, sizeof err);
    if (!m) { fprintf(stderr, "load: %s\n", err); return 1; }
    printf("base %p slide 0x%lx vmsize %llu MiB island %p+%zu\n", (void *)m->base, (long)m->slide, (unsigned long long)m->vmsize >> 20, (void *)m->island, m->island_bytes);
    printf("rebased %d, bound %d (lookups %d), symbols %u\n", m->nrebased, m->nbound, g_n, m->nsyms);
    for (int i = 0; i < m->nseg; i++) printf("  segment %-14s %p %8llu KiB file %8llu KiB prot %d\n", m->seg[i].name, (void *)m->seg[i].addr, (unsigned long long)m->seg[i].vmsize >> 10, (unsigned long long)m->seg[i].filesize >> 10, m->seg[i].prot);
    for (int i = 0; i < m->nsect; i++) printf("  section %s,%s %llu bytes flags 0x%x\n", m->sect[i].seg, m->sect[i].name, (unsigned long long)m->sect[i].size, m->sect[i].flags);
    int n = 0;
    macho_each_export(m, count, &n);
    printf("exports %d\n", n);
    for (int i = 2; i < argc; i++) {
        void *e = macho_export(m, argv[i]), *s = macho_symbol(m, argv[i]);
        uint64_t off = 0;
        const char *nm = e ? macho_symbolize(m, (char *)e + 4, &off) : NULL;
        printf("  %s: export %p symbol %p -> %s+%llu\n", argv[i], e, s, nm ? nm : "?", (unsigned long long)off);
    }
    return 0;
}
