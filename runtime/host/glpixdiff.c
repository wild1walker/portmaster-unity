// glpixdiff: two `glreplay --hash` logs of one trace compared (glreplay_hash.c): the readbacks whose hashes
// differ, the first of them, and where to look next.
//
//   glpixdiff <a.txt> <b.txt> [--all]
//
// Lines are matched by (frame, call, event, framebuffer): the same point of the same trace in both
// replays. Printed: how many readbacks each log has, how many both have, how many differ (by event), the
// first difference, and with --all every one. A difference at a pass's end (`bind`, `swap`) says only that
// something in that pass (or before it, in a texture it read) differs: replay that frame again with
// `--hash-frames F-F --hash-draws` on both sides and compare again; the first `draw` line that differs is
// the call that made the difference (its index in the trace is the line's call number). Exit status: 0 the
// same, 1 a difference, 2 an error.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { long frame; unsigned long call; char event[8], fb[16], size[24], kind[12]; unsigned long long hash; int readerr; } line_t;
typedef struct { line_t *l; size_t n, cap; } log_t;

static int load(const char *path, log_t *g)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "glpixdiff: cannot open %s\n", path); return -1; }
    char s[512];
    while (fgets(s, sizeof s, f)) {
        if (s[0] == '#') continue;
        line_t x;
        memset(&x, 0, sizeof x);
        if (sscanf(s, "%ld %lu %7s %15s %23s %11s %llx", &x.frame, &x.call, x.event, x.fb, x.size, x.kind, &x.hash) != 7) continue;
        x.readerr = strstr(s, "readerror") != NULL;
        if (g->n == g->cap) { g->cap = g->cap ? 2 * g->cap : 4096; g->l = realloc(g->l, g->cap * sizeof *g->l); }
        g->l[g->n++] = x;
    }
    fclose(f);
    return 0;
}

static int key_cmp(const line_t *a, const line_t *b)
{
    if (a->call != b->call) return a->call < b->call ? -1 : 1;
    int c = strcmp(a->event, b->event);
    if (c) return c;
    return strcmp(a->fb, b->fb);
}
static int by_key(const void *a, const void *b) { return key_cmp(a, b); }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: glpixdiff <a.txt> <b.txt> [--all]\n"); return 2; }
    int all = argc > 3 && !strcmp(argv[3], "--all");
    log_t a = { 0 }, b = { 0 };
    if (load(argv[1], &a) < 0 || load(argv[2], &b) < 0) return 2;
    qsort(a.l, a.n, sizeof *a.l, by_key);
    qsort(b.l, b.n, sizeof *b.l, by_key);
    size_t i = 0, j = 0, both = 0, diff = 0, only_a = 0, only_b = 0, ev_bind = 0, ev_swap = 0, ev_draw = 0, readerr = 0;
    const line_t *first = NULL, *first_b = NULL;
    while (i < a.n || j < b.n) {
        int c = i >= a.n ? 1 : j >= b.n ? -1 : key_cmp(&a.l[i], &b.l[j]);
        if (c < 0) { only_a++; i++; continue; }
        if (c > 0) { only_b++; j++; continue; }
        const line_t *x = &a.l[i++], *y = &b.l[j++];
        both++;
        if (x->readerr || y->readerr) { readerr++; continue; }
        if (x->hash == y->hash && !strcmp(x->size, y->size) && !strcmp(x->kind, y->kind)) continue;
        diff++;
        if (!strcmp(x->event, "bind")) ev_bind++; else if (!strcmp(x->event, "swap")) ev_swap++; else ev_draw++;
        if (!first) { first = x; first_b = y; }
        if (all) printf("differs: frame %ld call %lu %s %s %s %s: %016llx against %016llx\n", x->frame, x->call, x->event, x->fb,
                        x->size, x->kind, x->hash, y->hash);
    }
    printf("glpixdiff: %zu readbacks in %s, %zu in %s, %zu in both (%zu only in the first, %zu only in the second, %zu unreadable)\n",
           a.n, argv[1], b.n, argv[2], both, only_a, only_b, readerr);
    if (!diff) { printf("the same: no readback differs\n"); return only_a || only_b ? 1 : 0; }
    printf("%zu differ (pass ends %zu, frame ends %zu, after draws %zu)\n", diff, ev_bind, ev_swap, ev_draw);
    printf("first: frame %ld call %lu %s %s %s %s: %016llx against %016llx (%s)\n", first->frame, first->call, first->event, first->fb,
           first->size, first->kind, first->hash, first_b->hash, strcmp(first->size, first_b->size) ? "another size" : "the pixels");
    if (strcmp(first->event, "draw"))
        printf("next: both replays again with --hash-frames %ld-%ld --hash-draws: the first `draw` line that differs is the call\n",
               first->frame, first->frame);
    else
        printf("the call that made it: trace call %lu (frame %ld), drawing into %s\n", first->call, first->frame, first->fb);
    return 1;
}
