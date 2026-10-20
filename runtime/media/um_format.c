// um_format (MediaFormat), the frame pool and logging.
#define _GNU_SOURCE
#include "um_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void um_log(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    fputs("[media] ", stderr);
    fwrite(b, 1, n, stderr);
}

int um_tracing(void)
{
    static int t = -1;
    if (t < 0) { const char *e = getenv("UM_TRACE"); t = e && *e && *e != '0'; }
    return t;
}

/* ---- format ----------------------------------------------------------------------------------- */
typedef struct {
    char *key;
    int type;
    union { int32_t i; int64_t j; float f; } v;
    void *p;            /* string (NUL-terminated) or buffer */
    size_t n;
} entry;

struct um_format {
    int n, cap;
    entry *e;
};

um_format *um_format_new(void) { return calloc(1, sizeof(um_format)); }

static void entry_clear(entry *e) { free(e->p); e->p = NULL; e->n = 0; }

void um_format_free(um_format *f)
{
    if (!f) return;
    for (int i = 0; i < f->n; i++) { free(f->e[i].key); entry_clear(&f->e[i]); }
    free(f->e);
    free(f);
}

static entry *find(const um_format *f, const char *key)
{
    if (!f || !key) return NULL;
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->e[i].key, key)) return &f->e[i];
    return NULL;
}

static entry *slot(um_format *f, const char *key)
{
    entry *e = find(f, key);
    if (e) { entry_clear(e); return e; }
    if (f->n == f->cap) {
        int nc = f->cap ? f->cap * 2 : 16;
        entry *ne = realloc(f->e, nc * sizeof *ne);
        if (!ne) return NULL;
        f->e = ne; f->cap = nc;
    }
    e = &f->e[f->n++];
    memset(e, 0, sizeof *e);
    e->key = strdup(key);
    return e;
}

um_format *um_format_copy(const um_format *f)
{
    um_format *c = um_format_new();
    if (!f || !c) return c;
    for (int i = 0; i < f->n; i++) {
        const entry *s = &f->e[i];
        switch (s->type) {
        case UM_INT32: um_format_set_int32(c, s->key, s->v.i); break;
        case UM_INT64: um_format_set_int64(c, s->key, s->v.j); break;
        case UM_FLOAT: um_format_set_float(c, s->key, s->v.f); break;
        case UM_STRING: um_format_set_string(c, s->key, s->p); break;
        case UM_BUFFER: um_format_set_buffer(c, s->key, s->p, s->n); break;
        }
    }
    return c;
}

void um_format_set_int32(um_format *f, const char *key, int32_t v) { entry *e = slot(f, key); if (e) { e->type = UM_INT32; e->v.i = v; } }
void um_format_set_int64(um_format *f, const char *key, int64_t v) { entry *e = slot(f, key); if (e) { e->type = UM_INT64; e->v.j = v; } }
void um_format_set_float(um_format *f, const char *key, float v) { entry *e = slot(f, key); if (e) { e->type = UM_FLOAT; e->v.f = v; } }
void um_format_set_string(um_format *f, const char *key, const char *v)
{
    entry *e = slot(f, key);
    if (!e) return;
    e->type = UM_STRING;
    e->p = strdup(v ? v : "");
    e->n = strlen(e->p);
}
void um_format_set_buffer(um_format *f, const char *key, const void *data, size_t size)
{
    entry *e = slot(f, key);
    if (!e) return;
    e->type = UM_BUFFER;
    e->p = malloc(size ? size : 1);
    if (e->p && size) memcpy(e->p, data, size);
    e->n = size;
}
void um_format_remove(um_format *f, const char *key)
{
    entry *e = find(f, key);
    if (!e) return;
    free(e->key);
    entry_clear(e);
    int i = e - f->e;
    memmove(&f->e[i], &f->e[i + 1], (f->n - i - 1) * sizeof *e);
    f->n--;
}
int um_format_type(const um_format *f, const char *key) { entry *e = find(f, key); return e ? e->type : UM_NONE; }
int um_format_get_int32(const um_format *f, const char *key, int32_t *out)
{
    entry *e = find(f, key);
    if (!e || e->type != UM_INT32) return 0;
    if (out) *out = e->v.i;
    return 1;
}
int um_format_get_int64(const um_format *f, const char *key, int64_t *out)
{
    entry *e = find(f, key);
    if (!e || e->type != UM_INT64) return 0;
    if (out) *out = e->v.j;
    return 1;
}
int um_format_get_float(const um_format *f, const char *key, float *out)
{
    entry *e = find(f, key);
    if (!e || e->type != UM_FLOAT) return 0;
    if (out) *out = e->v.f;
    return 1;
}
const char *um_format_get_string(const um_format *f, const char *key)
{
    entry *e = find(f, key);
    return e && e->type == UM_STRING ? e->p : NULL;
}
const void *um_format_get_buffer(const um_format *f, const char *key, size_t *size)
{
    entry *e = find(f, key);
    if (!e || e->type != UM_BUFFER) return NULL;
    if (size) *size = e->n;
    return e->p;
}
int um_format_count(const um_format *f) { return f ? f->n : 0; }
const char *um_format_key(const um_format *f, int i) { return f && i >= 0 && i < f->n ? f->e[i].key : NULL; }

char *um_format_to_string(const um_format *f)
{
    size_t cap = 256, len = 0;
    char *s = malloc(cap);
    if (!s) return NULL;
    len += snprintf(s, cap, "{");
    for (int i = 0; f && i < f->n; i++) {
        char v[128];
        const entry *e = &f->e[i];
        switch (e->type) {
        case UM_INT32: snprintf(v, sizeof v, "%d", e->v.i); break;
        case UM_INT64: snprintf(v, sizeof v, "%lld", (long long)e->v.j); break;
        case UM_FLOAT: snprintf(v, sizeof v, "%g", e->v.f); break;
        case UM_STRING: snprintf(v, sizeof v, "%s", (char *)e->p); break;
        default: snprintf(v, sizeof v, "java.nio.HeapByteBuffer[pos=0 lim=%zu cap=%zu]", e->n, e->n); break;
        }
        size_t need = len + strlen(e->key) + strlen(v) + 8;
        if (need > cap) {
            while (need > cap) cap *= 2;
            char *ns = realloc(s, cap);
            if (!ns) { free(s); return NULL; }
            s = ns;
        }
        len += snprintf(s + len, cap - len, "%s%s=%s", i ? ", " : "", e->key, v);
    }
    if (len + 2 > cap) { char *ns = realloc(s, len + 2); if (!ns) { free(s); return NULL; } s = ns; }
    s[len++] = '}'; s[len] = 0;
    return s;
}

/* ---- frames ------------------------------------------------------------------------------------ */
static pthread_mutex_t pool_mu = PTHREAD_MUTEX_INITIALIZER;
static um_frame *pool;
static int pool_n;
#define POOL_MAX 8

um_frame *um_frame_get(int width, int height)
{
    int pw = (width + 1) & ~1, ph = (height + 1) & ~1;
    size_t need = (size_t)pw * (ph + ph / 2);
    um_frame *f = NULL;
    pthread_mutex_lock(&pool_mu);
    for (um_frame **pp = &pool; *pp; pp = &(*pp)->next)
        if ((*pp)->cap >= need && (*pp)->cap <= need + need / 4) { f = *pp; *pp = f->next; pool_n--; break; }
    pthread_mutex_unlock(&pool_mu);
    if (!f) {
        f = calloc(1, sizeof *f);
        if (!f) return NULL;
        f->data = malloc(need);
        if (!f->data) { free(f); return NULL; }
        f->cap = need;
    }
    f->next = NULL;
    f->width = width; f->height = height; f->pw = pw; f->ph = ph;
    f->pts_us = 0;
    return f;
}

void um_frame_put(um_frame *f)
{
    if (!f) return;
    pthread_mutex_lock(&pool_mu);
    if (pool_n < POOL_MAX) { f->next = pool; pool = f; pool_n++; f = NULL; }
    else {
        /* drop the oldest-sized ones first: a size change leaves frames nobody asks for */
        um_frame **pp = &pool;
        while ((*pp)->next) pp = &(*pp)->next;
        um_frame *last = *pp; *pp = NULL;
        f->next = pool; pool = f; f = last;
    }
    pthread_mutex_unlock(&pool_mu);
    if (f) { free(f->data); free(f); }
}

void um_frame_fill(um_frame *f, const uint8_t *y, int ys, const uint8_t *u, int us, const uint8_t *v, int vs)
{
    int w = f->width, h = f->height, pw = f->pw, ph = f->ph, cw = pw / 2, ch = ph / 2;
    int sw = (w + 1) / 2, sh = (h + 1) / 2;         /* chroma size of the source */
    uint8_t *d = f->data;
    for (int r = 0; r < ph; r++) {
        const uint8_t *s = y + (size_t)(r < h ? r : h - 1) * ys;
        memcpy(d, s, w);
        if (pw > w) memset(d + w, d[w - 1], pw - w);
        d += pw;
    }
    for (int r = 0; r < ch; r++) {
        int sr = r < sh ? r : sh - 1;
        memcpy(d, u + (size_t)sr * us, sw);
        if (cw > sw) memset(d + sw, d[sw - 1], cw - sw);
        memcpy(d + cw, v + (size_t)sr * vs, sw);
        if (cw > sw) memset(d + cw + sw, d[cw + sw - 1], cw - sw);
        d += pw;
    }
}
