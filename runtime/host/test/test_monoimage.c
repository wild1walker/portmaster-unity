// monoimage.c check: a small APK (one stored assembly, one too small, one deflated) and fake Mono/Unity entry
// points: a copy equal to the stored entry is replaced by a map of it (need_copy cleared, Unity's own copy given back
// through its unmap callback); a copy that differs by one byte, a caller-owned buffer, a writable map and the
// switch off leave everything as it was. Plain C: cc test/test_monoimage.c -o /tmp/t && /tmp/t
#include "../monoimage.c"

#include <assert.h>

static unsigned char asm_bytes[100000];
static char apk_path[] = "/tmp/test_monoimage_XXXXXX";

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
static void put32(FILE *f, unsigned long v) { put16(f, v & 0xffff); put16(f, v >> 16 & 0xffff); }
// one zip entry: local header + data; its central record into cd
static void entry(FILE *f, FILE *cd, const char *name, const void *data, unsigned size, unsigned method, unsigned *n)
{
    long off = ftell(f);
    unsigned nl = (unsigned)strlen(name);
    put32(f, 0x04034b50); put16(f, 20); put16(f, 0); put16(f, method); put16(f, 0); put16(f, 0);
    put32(f, 0); put32(f, size); put32(f, size); put16(f, nl); put16(f, 3);
    fwrite(name, 1, nl, f); fwrite("xyz", 1, 3, f);                 // (an extra field: the data starts later)
    fwrite(data, 1, size, f);
    put32(cd, 0x02014b50); put16(cd, 20); put16(cd, 20); put16(cd, 0); put16(cd, method); put16(cd, 0); put16(cd, 0);
    put32(cd, 0); put32(cd, size); put32(cd, size); put16(cd, nl); put16(cd, 0); put16(cd, 0);
    put16(cd, 0); put16(cd, 0); put32(cd, 0); put32(cd, (unsigned long)off);
    fwrite(name, 1, nl, cd);
    (*n)++;
}
static void make_apk(void)
{
    for (unsigned i = 0; i < sizeof asm_bytes; i++) asm_bytes[i] = (unsigned char)(i * 7 + (i >> 9));
    asm_bytes[0] = 'M'; asm_bytes[1] = 'Z';
    int fd = mkstemp(apk_path);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "wb"), *cd = tmpfile();
    unsigned n = 0;
    fwrite("pad", 1, 3, f);                                          // (entries at odd offsets)
    entry(f, cd, "assets/bin/Data/Managed/small.dll", asm_bytes, 1000, 0, &n);
    entry(f, cd, "assets/bin/Data/Managed/deflated.dll", asm_bytes, sizeof asm_bytes, 8, &n);
    entry(f, cd, "assets/bin/Data/Managed/Game.dll", asm_bytes, sizeof asm_bytes, 0, &n);
    long cdoff = ftell(f), cdsize = ftell(cd);
    rewind(cd);
    char b[4096]; size_t k;
    while ((k = fread(b, 1, sizeof b, cd)) > 0) fwrite(b, 1, k, f);
    put32(f, 0x06054b50); put16(f, 0); put16(f, 0); put16(f, n); put16(f, n); put32(f, cdsize); put32(f, cdoff); put16(f, 0);
    fclose(cd); fclose(f);
}

// fake Mono
static char *got_data; static int got_copy;
static void *m_ofdn(char *d, unsigned n, int c, int *s, int ro, const char *nm)
{ (void)n; (void)s; (void)ro; (void)nm; got_data = d; got_copy = c; return (void *)1; }
static void *m_ofd(char *d, unsigned n, int c, int *s) { (void)n; (void)s; got_data = d; got_copy = c; return (void *)1; }
static void *got_map, *got_unmap;
static void m_override(void *o, void *s, void *fd, void *c, void *map, void *unmap) { (void)o; (void)s; (void)fd; (void)c; got_map = map; got_unmap = unmap; }
// fake Unity: its map is a malloc'd copy of the "file" (here the assembly's bytes), its unmap frees it
static void *u_freed; static int u_unmaps;
static void *u_map_f(size_t len, int flags, int fd, unsigned long long off, void **h)
{ (void)flags; (void)fd; (void)off; unsigned char *p = malloc(len); memcpy(p, asm_bytes, len); *h = p; return p; }
static int u_unmap_f(void *addr, void *h) { assert(addr == h); u_freed = addr; u_unmaps++; free(addr); return 0; }

static void reset(int m) { g_mode = m; got_data = NULL; got_copy = -1; }

int main(void)
{
    make_apk();
    monoimage_set_apk(apk_path);

    // off: nothing wrapped
    reset(0);
    assert(monoimage_dlsym_hook("mono_image_open_from_data_with_name", (void *)m_ofdn) == (void *)m_ofdn);

    reset(1);
    ofdn_t ofdn = (ofdn_t)monoimage_dlsym_hook("mono_image_open_from_data_with_name", (void *)m_ofdn);
    ofd_t ofd = (ofd_t)monoimage_dlsym_hook("mono_image_open_from_data", (void *)m_ofd);
    assert(ofdn != m_ofdn && ofd != m_ofd);
    assert(monoimage_dlsym_hook("mono_jit_init", (void *)m_ofd) == (void *)m_ofd);

    // a copy of the stored entry: Mono gets a map of it, told not to copy
    char *copy = malloc(sizeof asm_bytes);
    memcpy(copy, asm_bytes, sizeof asm_bytes);
    int st;
    ofdn(copy, sizeof asm_bytes, 1, &st, 0, "Game.dll");
    assert(got_data != copy && got_copy == 0 && !memcmp(got_data, asm_bytes, sizeof asm_bytes));
    assert(g_saved == (long long)sizeof asm_bytes);

    // one byte different: as before
    reset(1);
    copy[50000] ^= 1;
    ofd(copy, sizeof asm_bytes, 1, &st);
    assert(got_data == copy && got_copy == 1);
    copy[50000] ^= 1;

    // the caller keeps its buffer (need_copy 0): as before
    reset(1);
    ofd(copy, sizeof asm_bytes, 0, &st);
    assert(got_data == copy && got_copy == 0);

    // too small for the index, or only a deflated entry of that size: as before
    reset(1);
    ofd(copy, 1000, 1, &st);
    assert(got_data == copy && got_copy == 1);

    // Unity's file map callbacks: its copy given back, ours unmapped by ours
    reset(1);
    override_t ov = (override_t)monoimage_dlsym_hook("mono_file_map_override", (void *)m_override);
    ov(NULL, NULL, NULL, NULL, (void *)u_map_f, (void *)u_unmap_f);
    assert(got_map == (void *)w_map && got_unmap == (void *)w_unmap);
    void *h = NULL;
    unsigned char *p = ((map_t)got_map)(sizeof asm_bytes, 1 | 16, 3, 0, &h);
    assert(p && u_unmaps == 1 && u_freed && (void *)p != u_freed && !memcmp(p, asm_bytes, sizeof asm_bytes));
    assert(((unmap_t)got_unmap)(p, h) == 0 && u_unmaps == 1);
    // a writable map: Unity's own
    h = NULL;
    p = ((map_t)got_map)(sizeof asm_bytes, 1 | 2 | 16, 3, 0, &h);
    assert((void *)p == h && u_unmaps == 1);
    assert(((unmap_t)got_unmap)(p, h) == 0 && u_unmaps == 2 && u_freed == (void *)p);
    // bytes the APK does not hold: Unity's own
    asm_bytes[10] ^= 1;
    h = NULL;
    p = ((map_t)got_map)(sizeof asm_bytes, 1 | 16, 3, 0, &h);
    assert((void *)p == h && u_unmaps == 2);
    ((unmap_t)got_unmap)(p, h);
    assert(u_unmaps == 3);
    asm_bytes[10] ^= 1;

    // trace: logs only
    reset(2);
    ofd(copy, sizeof asm_bytes, 1, &st);
    assert(got_data == copy && got_copy == 1);

    unlink(apk_path);
    free(copy);
    printf("monoimage: ok\n");
    return 0;
}
