// monoimage.c: the game's assemblies read in place from the APK (UA_MONO_IMAGE_MAP=1; =trace only logs).
//
// Mono keeps every assembly it loads whole in memory (MonoImage's raw_data: its metadata and IL are read from it
// for as long as the game runs). On Android, Unity hands Mono the bytes it read from the APK: through
// mono_image_open_from_data* (Mono copies them into its own malloc when asked to) or through its file map callbacks
// (mono_file_map_override: a copy in Unity's malloc). Either way the assemblies sit in anonymous memory, about
// 13 MB for Blasphemous, though the APK holds the same bytes uncompressed (Unity's assets/bin/Data/Managed are
// stored entries). With UA_MONO_IMAGE_MAP=1 such a copy is replaced by a private map of that APK entry, once its
// bytes are found equal to the copy's (every byte compared): the same bytes for Mono, file pages the kernel can
// drop and read again instead of memory that must stay. Unity's own copy is given back through its unmap callback
// when it was one from its map callback; with mono_image_open_from_data* Mono is told not to copy (need_copy 0),
// and Unity frees its buffer as it always does. Anything that does not match is left as it was.
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "monoimage.h"

static int g_mode = -1;                          // 0 off, 1 map, 2 trace only
static char g_apk[600];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static long long g_saved;

static int mode(void)
{
    if (g_mode < 0) {
        const char *e = getenv("UA_MONO_IMAGE_MAP");
        g_mode = !e || !*e || !strcmp(e, "0") ? 0 : !strcmp(e, "trace") ? 2 : 1;
    }
    return g_mode;
}

void monoimage_set_apk(const char *apk) { snprintf(g_apk, sizeof g_apk, "%s", apk ? apk : ""); }

// The APK's stored entries of at least 64 KB (where the assemblies are): data offset and size
typedef struct { long long off, size; char name[96]; } ent;
static ent *g_ents;
static int g_nents, g_indexed;

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned long rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned long)p[3] << 24; }
static unsigned long long rd64(const unsigned char *p) { return rd32(p) | (unsigned long long)rd32(p + 4) << 32; }

static void index_apk(void)                     // (g_mu held)
{
    if (g_indexed) return;
    g_indexed = 1;
    int fd = g_apk[0] ? open(g_apk, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0) return;
    struct stat st;
    unsigned char *cd = NULL, *t = NULL;
    if (fstat(fd, &st) || st.st_size < 22) goto out;
    long long tail = st.st_size < 65557 ? st.st_size : 65557;
    t = malloc(tail);
    if (!t || pread(fd, t, tail, st.st_size - tail) != tail) goto out;
    long long eocd = -1;
    for (long long i = tail - 22; i >= 0; i--) if (rd32(t + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) goto out;
    unsigned long long n = rd16(t + eocd + 10), cdsize = rd32(t + eocd + 12), cdoff = rd32(t + eocd + 16);
    if (eocd >= 20 && rd32(t + eocd - 20) == 0x07064b50) {
        unsigned char z[56];
        if (pread(fd, z, 56, (off_t)rd64(t + eocd - 20 + 8)) == 56 && rd32(z) == 0x06064b50) {
            n = rd64(z + 32); cdsize = rd64(z + 40); cdoff = rd64(z + 48);
        }
    }
    if (!cdsize || cdsize > 256u << 20 || cdoff + cdsize > (unsigned long long)st.st_size) goto out;
    cd = malloc(cdsize);
    if (!cd || pread(fd, cd, cdsize, (off_t)cdoff) != (ssize_t)cdsize) goto out;
    for (unsigned long long k = 0, p = 0; k < n && p + 46 <= cdsize && rd32(cd + p) == 0x02014b50; k++) {
        unsigned nl = rd16(cd + p + 28), xl = rd16(cd + p + 30), cl = rd16(cd + p + 32);
        if (p + 46 + nl + xl > cdsize) break;
        const char *nm = (const char *)cd + p + 46;
        long long csize = rd32(cd + p + 20), size = rd32(cd + p + 24), local = rd32(cd + p + 42);
        for (unsigned x = 0; x + 4 <= xl; ) {
            const unsigned char *e = cd + p + 46 + nl + x;
            unsigned id = rd16(e), len = rd16(e + 2), q = 4;
            if (id == 1) {
                if (size == 0xffffffffLL && q + 8 <= len + 4u) { size = (long long)rd64(e + q); q += 8; }
                if (csize == 0xffffffffLL && q + 8 <= len + 4u) { csize = (long long)rd64(e + q); q += 8; }
                if (local == 0xffffffffLL && q + 8 <= len + 4u) { local = (long long)rd64(e + q); q += 8; }
            }
            x += 4 + len;
        }
        unsigned char lh[30];
        if (rd16(cd + p + 10) == 0 && size >= 65536 && size == csize && nl > 4 && !memcmp(nm + nl - 4, ".dll", 4)
            && pread(fd, lh, 30, (off_t)local) == 30 && rd32(lh) == 0x04034b50) {
            ent *ne = realloc(g_ents, (g_nents + 1) * sizeof *g_ents);
            if (!ne) break;
            g_ents = ne;
            ent *e = &g_ents[g_nents++];
            e->off = local + 30 + rd16(lh + 26) + rd16(lh + 28);
            e->size = size;
            const char *b = memrchr(nm, '/', nl);
            b = b ? b + 1 : nm;
            snprintf(e->name, sizeof e->name, "%.*s", (int)(nm + nl - b), b);
        }
        p += 46 + nl + xl + cl;
    }
out:
    free(t);
    free(cd);
    close(fd);
}

// A private map of the APK entry holding exactly these bytes, or NULL
static void *find_map(const void *data, size_t len, const char **name)
{
    if (!data || len < 65536) return NULL;
    pthread_mutex_lock(&g_mu);
    index_apk();
    void *r = NULL;
    int fd = -1;
    for (int i = 0; i < g_nents && !r; i++) {
        if (g_ents[i].size != (long long)len) continue;
        if (fd < 0 && (fd = open(g_apk, O_RDONLY | O_CLOEXEC)) < 0) break;
        long long pg = g_ents[i].off & ~4095LL, d = g_ents[i].off - pg;
        unsigned char *m = mmap(NULL, len + d, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, (off_t)pg);
        if (m == MAP_FAILED) continue;
        if (!memcmp(m + d, data, len)) { r = m + d; *name = g_ents[i].name; g_saved += len; }
        else munmap(m, len + d);
    }
    if (fd >= 0) close(fd);
    pthread_mutex_unlock(&g_mu);
    return r;
}

// mono_image_open_from_data* (Mono 2.x and later: the same leading arguments)
typedef void *(*ofd_t)(char *, unsigned, int, int *);
typedef void *(*ofdf_t)(char *, unsigned, int, int *, int);
typedef void *(*ofdn_t)(char *, unsigned, int, int *, int, const char *);
static ofd_t r_ofd;
static ofdf_t r_ofdf;
static ofdn_t r_ofdn;

static char *swap_data(char *data, unsigned len, int *need_copy, const char *how, const char *nm)
{
    if (mode() == 2) { fprintf(stderr, "[monoimage] %s %s: %u bytes, need_copy %d\n", how, nm ? nm : "-", len, *need_copy); return data; }
    if (!*need_copy) { if (len >= 65536) fprintf(stderr, "[monoimage] %s: %u bytes the caller keeps (need_copy 0): left as it is\n", nm ? nm : how, len); return data; }
    const char *name = NULL;
    char *m = find_map(data, len, &name);
    if (!m) { if (len >= 65536) fprintf(stderr, "[monoimage] %s: %u bytes, no stored APK entry holds them: copied as before\n", nm ? nm : how, len); return data; }
    fprintf(stderr, "[monoimage] %s: %u bytes read in place from the APK (%s)\n", nm ? nm : name, len, name);
    *need_copy = 0;
    return m;
}
static void *w_ofd(char *d, unsigned n, int c, int *s) { d = swap_data(d, n, &c, "open_from_data", NULL); return r_ofd(d, n, c, s); }
static void *w_ofdf(char *d, unsigned n, int c, int *s, int ro) { d = swap_data(d, n, &c, "open_from_data_full", NULL); return r_ofdf(d, n, c, s, ro); }
static void *w_ofdn(char *d, unsigned n, int c, int *s, int ro, const char *nm) { d = swap_data(d, n, &c, "open_from_data_with_name", nm); return r_ofdn(d, n, c, s, ro, nm); }

// mono_file_map_override: Unity's callbacks, its map wrapped
typedef void *(*map_t)(size_t, int, int, unsigned long long, void **);
typedef int (*unmap_t)(void *, void *);
typedef void (*override_t)(void *, void *, void *, void *, void *, void *);
static override_t r_override;
static map_t u_map;
static unmap_t u_unmap;
enum { MAXM = 64 };
static struct { void *addr; size_t len; } g_mine[MAXM];   // our maps, by the handle we gave (index + 1)

static void *w_map(size_t length, int flags, int fd, unsigned long long offset, void **handle)
{
    void *p = u_map(length, flags, fd, offset, handle);
    if (mode() == 2) { fprintf(stderr, "[monoimage] file map %zu bytes at %llu, flags %d: %p\n", length, offset, flags, p); return p; }
    if (!p || (flags & 2)) return p;            // (a writable map: left as it is)
    const char *name = NULL;
    char *m = find_map(p, length, &name);
    if (!m) { if (length >= 65536) fprintf(stderr, "[monoimage] file map of %zu bytes: no stored APK entry holds them, left as it is\n", length); return p; }
    pthread_mutex_lock(&g_mu);
    int k = 0;
    while (k < MAXM && g_mine[k].addr) k++;
    if (k == MAXM) { pthread_mutex_unlock(&g_mu); return p; }   // (the map stays, unused: harmless)
    g_mine[k].addr = m; g_mine[k].len = length;
    pthread_mutex_unlock(&g_mu);
    u_unmap(p, *handle);                        // Unity's copy given back
    *handle = (void *)(intptr_t)(k + 1);
    fprintf(stderr, "[monoimage] %s: %zu bytes read in place from the APK (Unity's copy freed)\n", name, length);
    return m;
}
static int w_unmap(void *addr, void *handle)
{
    intptr_t k = (intptr_t)handle - 1;
    pthread_mutex_lock(&g_mu);
    if (k >= 0 && k < MAXM && g_mine[k].addr == addr) {
        uintptr_t pg = (uintptr_t)addr & ~(uintptr_t)4095;
        munmap((void *)pg, g_mine[k].len + ((uintptr_t)addr - pg));
        g_mine[k].addr = NULL;
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    pthread_mutex_unlock(&g_mu);
    return u_unmap(addr, handle);
}
static void w_override(void *open_f, void *size_f, void *fd_f, void *close_f, void *map_f, void *unmap_f)
{
    u_map = (map_t)map_f; u_unmap = (unmap_t)unmap_f;
    if (map_f && unmap_f) r_override(open_f, size_f, fd_f, close_f, (void *)w_map, (void *)w_unmap);
    else r_override(open_f, size_f, fd_f, close_f, map_f, unmap_f);
}

void *monoimage_dlsym_hook(const char *sym, void *p)
{
    if (!p || strncmp(sym, "mono_", 5) || !mode()) return p;
    if (!strcmp(sym, "mono_image_open_from_data")) { r_ofd = (ofd_t)p; return (void *)w_ofd; }
    if (!strcmp(sym, "mono_image_open_from_data_full")) { r_ofdf = (ofdf_t)p; return (void *)w_ofdf; }
    if (!strcmp(sym, "mono_image_open_from_data_with_name")) { r_ofdn = (ofdn_t)p; return (void *)w_ofdn; }
    if (!strcmp(sym, "mono_file_map_override")) { r_override = (override_t)p; return (void *)w_override; }
    return p;
}
