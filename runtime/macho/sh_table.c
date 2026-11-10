// sh_table.c - the shims by name: the areas' tables (sh_*.c, dyld.c) as one sorted list.
#define _GNU_SOURCE
#include "darwin.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct darwin_sym *const areas[] = {
    darwin_syms_errno, darwin_syms_mem, darwin_syms_stdio, darwin_syms_file, darwin_syms_thread,
    darwin_syms_time, darwin_syms_locale, darwin_syms_misc, darwin_syms_net, darwin_syms_dyld,
    darwin_syms_cf, darwin_syms_tlv, darwin_syms_signal, darwin_syms_context,
};
static struct darwin_sym *all;
static int nall;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static int by_name(const void *a, const void *b)
{
    return strcmp(((const struct darwin_sym *)a)->name, ((const struct darwin_sym *)b)->name);
}

static void build(void)
{
    int n = 0;
    for (size_t a = 0; a < sizeof areas / sizeof *areas; a++)
        for (const struct darwin_sym *s = areas[a]; s->name; s++) n++;
    all = malloc((size_t)n * sizeof *all);
    if (!all) { fprintf(stderr, "[darwin] no memory for the shim table\n"); abort(); }
    for (size_t a = 0; a < sizeof areas / sizeof *areas; a++)
        for (const struct darwin_sym *s = areas[a]; s->name; s++) all[nall++] = *s;
    qsort(all, (size_t)nall, sizeof *all, by_name);
    for (int i = 1; i < nall; i++)
        if (!strcmp(all[i - 1].name, all[i].name)) { fprintf(stderr, "[darwin] %s is answered twice\n", all[i].name); abort(); }
}

void *darwin_lookup(const char *name)
{
    pthread_once(&once, build);
    struct darwin_sym key = { name, NULL };
    struct darwin_sym *s = bsearch(&key, all, (size_t)nall, sizeof *all, by_name);
    return s ? s->addr : NULL;
}

int darwin_shim_count(void)
{
    pthread_once(&once, build);
    return nall;
}

void darwin_log(const char *fmt, ...)
{
    int e = errno;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[darwin] %s\n", line);
    errno = e;
}

void darwin_unanswered(const char *name, const char *what)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static const char *said[256];
    static int nsaid;
    int e = errno, first = 1;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nsaid; i++) if (!strcmp(said[i], name)) { first = 0; break; }
    if (first && nsaid < 256) said[nsaid++] = name;
    pthread_mutex_unlock(&lock);
    if (first) fprintf(stderr, "[darwin] %s: %s\n", name, what);
    errno = e;
}
