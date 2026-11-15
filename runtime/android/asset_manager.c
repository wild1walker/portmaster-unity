// libandroid.so AAssetManager_* / AAsset_* / AAssetDir_*, backed by a host directory and, for what that
// lacks, by the APK's own assets/ entries (android_set_asset_apk): an Android build ported as it is keeps
// its assets in the APK, and 2023.1+ players open the Android Game SDK's Memory Advice files
// (realtime_features.json, *.tflite) through AAssetManager and abort without them. A stored entry is
// read from the APK in place (AAsset_openFileDescriptor gives the APK and the entry's offset, as
// Android does); a deflated one is inflated into memory once opened (no descriptor, as Android).
// (Unity 2017.4 itself reads the APK zip directly and does not import these.)
#define _GNU_SOURCE
#include "android_shims.h"
#include "shim_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

struct AAssetManager { char root[PATH_MAX]; };
struct AAsset {
    int fd;
    long long length, pos;
    void *map;        // AAsset_getBuffer mapping (whole file), lazily created
    long long base;   // the data's offset in fd (a stored APK entry), else 0
    void *mapbase;    // the page-aligned mapping behind map when base is not page-aligned
    size_t maplen;
    unsigned char *mem;   // an inflated APK entry (fd -1)
};
struct AAssetDir {
    DIR *dir;
    char path[PATH_MAX];
    char name[NAME_MAX + 1];
    int apk, next;                 // no such folder: the APK's assets/<prefix> entries, from index next
    char prefix[PATH_MAX];
};

static AAssetManager g_mgr;
static pthread_mutex_t g_mgr_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_mgr_set;

void android_set_asset_dir(const char *dir)
{
    pthread_mutex_lock(&g_mgr_lock);
    snprintf(g_mgr.root, sizeof g_mgr.root, "%s", dir && *dir ? dir : ".");
    size_t n = strlen(g_mgr.root);
    while (n > 1 && g_mgr.root[n - 1] == '/') g_mgr.root[--n] = 0;
    g_mgr_set = 1;
    pthread_mutex_unlock(&g_mgr_lock);
}

/* ---- the APK's assets/ entries ---- */
typedef struct { char *name; int method; long long csize, size, local; } apk_ent;   // name without "assets/"
static char g_apk[PATH_MAX];
static apk_ent *g_ents;
static int g_nents, g_indexed;
static pthread_mutex_t g_apk_lock = PTHREAD_MUTEX_INITIALIZER;

void android_set_asset_apk(const char *apk)
{
    pthread_mutex_lock(&g_apk_lock);
    snprintf(g_apk, sizeof g_apk, "%s", apk ? apk : "");
    for (int i = 0; i < g_nents; i++) free(g_ents[i].name);
    free(g_ents);
    g_ents = NULL; g_nents = 0; g_indexed = 0;
    pthread_mutex_unlock(&g_apk_lock);
}

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned long rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned long)p[3] << 24; }
static unsigned long long rd64(const unsigned char *p) { return rd32(p) | (unsigned long long)rd32(p + 4) << 32; }

// The central directory's assets/ entries, read once (zip64 offsets and sizes too). Lock held.
static void apk_index(void)
{
    if (g_indexed) return;
    g_indexed = 1;
    if (!g_apk[0]) return;
    int fd = open(g_apk, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    unsigned char *cd = NULL;
    if (fstat(fd, &st) || st.st_size < 22) goto out;
    long long tail = st.st_size < 65557 ? st.st_size : 65557;      // the end record and a comment of up to 64 KB
    unsigned char *t = malloc(tail);
    if (!t || pread(fd, t, tail, st.st_size - tail) != tail) { free(t); goto out; }
    long long eocd = -1;
    for (long long i = tail - 22; i >= 0; i--) if (rd32(t + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(t); goto out; }
    unsigned long long n = rd16(t + eocd + 10), cdsize = rd32(t + eocd + 12), cdoff = rd32(t + eocd + 16);
    if (eocd >= 20 && rd32(t + eocd - 20) == 0x07064b50) {           // zip64: the end record it points to
        unsigned char z[56];
        if (pread(fd, z, 56, (off_t)rd64(t + eocd - 20 + 8)) == 56 && rd32(z) == 0x06064b50) {
            n = rd64(z + 32); cdsize = rd64(z + 40); cdoff = rd64(z + 48);
        }
    }
    free(t);
    if (!cdsize || cdsize > 256u << 20 || cdoff + cdsize > (unsigned long long)st.st_size) goto out;
    cd = malloc(cdsize);
    if (!cd || pread(fd, cd, cdsize, (off_t)cdoff) != (ssize_t)cdsize) goto out;
    for (unsigned long long k = 0, p = 0; k < n && p + 46 <= cdsize && rd32(cd + p) == 0x02014b50; k++) {
        unsigned nl = rd16(cd + p + 28), xl = rd16(cd + p + 30), cl = rd16(cd + p + 32);
        if (p + 46 + nl + xl > cdsize) break;
        const char *nm = (const char *)cd + p + 46;
        long long csize = rd32(cd + p + 20), size = rd32(cd + p + 24), local = rd32(cd + p + 42);
        for (unsigned x = 0; x + 4 <= xl; ) {                       // zip64 extra field: the values set to ~0
            const unsigned char *e = cd + p + 46 + nl + x;
            unsigned id = rd16(e), len = rd16(e + 2), q = 4;
            if (id == 1) {
                if (size == 0xffffffffLL && q + 8 <= len + 4u) { size = (long long)rd64(e + q); q += 8; }
                if (csize == 0xffffffffLL && q + 8 <= len + 4u) { csize = (long long)rd64(e + q); q += 8; }
                if (local == 0xffffffffLL && q + 8 <= len + 4u) { local = (long long)rd64(e + q); q += 8; }
            }
            x += 4 + len;
        }
        if (nl > 7 && !memcmp(nm, "assets/", 7) && nm[nl - 1] != '/') {
            apk_ent *ne = realloc(g_ents, (g_nents + 1) * sizeof *g_ents);
            if (!ne) break;
            g_ents = ne;
            apk_ent *e = &g_ents[g_nents];
            e->name = strndup(nm + 7, nl - 7);
            e->method = rd16(cd + p + 10);
            e->csize = csize; e->size = size; e->local = local;
            if (e->name) g_nents++;
        }
        p += 46 + nl + xl + cl;
    }
    SHIM_TRACE("AAssetManager: %d assets/ entries in %s", g_nents, g_apk);
out:
    free(cd);
    close(fd);
}

// An AAsset for the APK's entry assets/<name>, or NULL.
static AAsset *apk_open(const char *name)
{
    while (*name == '/') name++;
    pthread_mutex_lock(&g_apk_lock);
    apk_index();
    apk_ent e = { 0 };
    for (int i = 0; i < g_nents; i++) if (!strcmp(g_ents[i].name, name)) { e = g_ents[i]; break; }
    char apk[PATH_MAX];
    snprintf(apk, sizeof apk, "%s", g_apk);
    pthread_mutex_unlock(&g_apk_lock);
    if (!e.name || (e.method != 0 && e.method != 8)) return NULL;
    int fd = open(apk, O_RDONLY | O_CLOEXEC);
    unsigned char lh[30];
    if (fd < 0) return NULL;
    if (pread(fd, lh, 30, (off_t)e.local) != 30 || rd32(lh) != 0x04034b50) { close(fd); return NULL; }
    long long data = e.local + 30 + rd16(lh + 26) + rd16(lh + 28);
    AAsset *a = calloc(1, sizeof *a);
    if (!a) { close(fd); return NULL; }
    a->length = e.size;
    if (e.method == 0) {                         // stored: read in place
        a->fd = fd;
        a->base = data;
        return a;
    }
    unsigned char *in = malloc(e.csize ? e.csize : 1), *out = malloc(e.size ? e.size : 1);
    z_stream z = { 0 };
    int ok = in && out && pread(fd, in, e.csize, (off_t)data) == e.csize && inflateInit2(&z, -MAX_WBITS) == Z_OK;
    if (ok) {
        z.next_in = in; z.avail_in = (uInt)e.csize; z.next_out = out; z.avail_out = (uInt)e.size;
        int r = inflate(&z, Z_FINISH);
        ok = (r == Z_STREAM_END || (r == Z_BUF_ERROR && z.avail_out == 0)) && z.total_out == (uLong)e.size;
        inflateEnd(&z);
    }
    free(in);
    close(fd);
    if (!ok) { free(out); free(a); return NULL; }
    a->fd = -1;
    a->mem = out;
    return a;
}

static AAssetManager *default_mgr(void)
{
    pthread_mutex_lock(&g_mgr_lock);
    int set = g_mgr_set;
    pthread_mutex_unlock(&g_mgr_lock);
    if (!set) {
        const char *e = getenv("ANDROID_ASSET_DIR");
        android_set_asset_dir(e && *e ? e : "./assets");
    }
    return &g_mgr;
}

AAssetManager *AAssetManager_fromJava(void *env, void *assetManager)
{
    (void)env; (void)assetManager;
    return default_mgr();
}

// Asset names are relative ("bin/Data/data.unity3d"); refuse ".." so lookups stay in the root.
static int asset_path(AAssetManager *m, const char *name, char *out, size_t outsz)
{
    if (!m) m = default_mgr();
    if (!name) return -1;
    while (*name == '/') name++;
    for (const char *p = name; *p; ) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0)) return -1;
        const char *s = strchr(p, '/');
        if (!s) break;
        p = s + 1;
    }
    int n = *name ? snprintf(out, outsz, "%s/%s", m->root, name) : snprintf(out, outsz, "%s", m->root);
    if (n < 0 || (size_t)n >= outsz) return -1;
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
    return 0;
}

AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode)
{
    (void)mode;
    char path[PATH_MAX];
    if (asset_path(mgr, filename, path, sizeof path)) return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        AAsset *z = apk_open(filename);          // not unpacked: the APK's own entry
        SHIM_TRACE("AAssetManager_open(%s): %s; in the APK: %s", filename, strerror(errno), z ? "yes" : "no");
        return z;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    AAsset *a = calloc(1, sizeof *a);
    if (!a) { close(fd); return NULL; }
    a->fd = fd;
    a->length = st.st_size;
    SHIM_TRACE("AAssetManager_open(%s) -> %p (%lld bytes)", filename, (void *)a, a->length);
    return a;
}

int AAsset_read(AAsset *a, void *buf, size_t count)
{
    if (!a) return -1;
    long long left = a->length - a->pos;
    if (left <= 0) return 0;
    if ((long long)count > left) count = left;
    if (count > INT_MAX) count = INT_MAX;
    if (a->mem) { memcpy(buf, a->mem + a->pos, count); a->pos += count; return (int)count; }
    ssize_t r = pread64(a->fd, buf, count, a->base + a->pos);
    if (r < 0) return -1;
    a->pos += r;
    return (int)r;
}

a_off64_t AAsset_seek64(AAsset *a, a_off64_t offset, int whence)
{
    if (!a) return -1;
    long long np;
    switch (whence) {
    case SEEK_SET: np = offset; break;
    case SEEK_CUR: np = a->pos + offset; break;
    case SEEK_END: np = a->length + offset; break;
    default: return -1;
    }
    if (np < 0 || np > a->length) return -1;   // like Android's _FileAsset::seek
    a->pos = np;
    return np;
}

a_off_t AAsset_seek(AAsset *a, a_off_t offset, int whence)
{
    a_off64_t r = AAsset_seek64(a, offset, whence);
    return r > LONG_MAX ? -1 : (a_off_t)r;
}

void AAsset_close(AAsset *a)
{
    if (!a) return;
    if (a->mapbase) munmap(a->mapbase, a->maplen);
    else if (a->map) munmap(a->map, a->length ? a->length : 1);
    if (a->fd >= 0) close(a->fd);
    free(a->mem);
    free(a);
}

const void *AAsset_getBuffer(AAsset *a)
{
    if (!a) return NULL;
    if (a->mem) return a->mem;
    if (!a->map && a->base) {                    // a stored APK entry: map from the page it starts in
        long long pg = sysconf(_SC_PAGESIZE), start = a->base / pg * pg;
        a->maplen = (size_t)(a->base - start + (a->length ? a->length : 1));
        a->mapbase = mmap(NULL, a->maplen, PROT_READ, MAP_PRIVATE, a->fd, (off_t)start);
        if (a->mapbase == MAP_FAILED) { a->mapbase = NULL; return NULL; }
        a->map = (char *)a->mapbase + (a->base - start);
    }
    if (!a->map) {
        if (a->length == 0) {
            a->map = mmap(NULL, 1, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        } else {
            a->map = mmap(NULL, a->length, PROT_READ, MAP_PRIVATE, a->fd, 0);
        }
        if (a->map == MAP_FAILED) { a->map = NULL; return NULL; }
    }
    return a->map;
}

a_off_t AAsset_getLength(AAsset *a) { return a ? (a_off_t)a->length : -1; }
a_off64_t AAsset_getLength64(AAsset *a) { return a ? a->length : -1; }
a_off_t AAsset_getRemainingLength(AAsset *a) { return a ? (a_off_t)(a->length - a->pos) : -1; }
a_off64_t AAsset_getRemainingLength64(AAsset *a) { return a ? a->length - a->pos : -1; }

int AAsset_openFileDescriptor64(AAsset *a, a_off64_t *outStart, a_off64_t *outLength)
{
    if (!a || a->fd < 0) return -1;             // an inflated entry has no descriptor (as on Android)
    int fd = fcntl(a->fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (outStart) *outStart = a->base;
    if (outLength) *outLength = a->length;
    return fd;
}

int AAsset_openFileDescriptor(AAsset *a, a_off_t *outStart, a_off_t *outLength)
{
    a_off64_t s, l;
    int fd = AAsset_openFileDescriptor64(a, &s, &l);
    if (fd >= 0) {
        if (outStart) *outStart = (a_off_t)s;
        if (outLength) *outLength = (a_off_t)l;
    }
    return fd;
}

int AAsset_isAllocated(AAsset *a) { return a && a->mem; }

AAssetDir *AAssetManager_openDir(AAssetManager *mgr, const char *dirName)
{
    AAssetDir *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    if (asset_path(mgr, dirName ? dirName : "", d->path, sizeof d->path) == 0)
        d->dir = opendir(d->path);
    if (!d->dir && dirName) {                    // not unpacked: the APK's entries directly in that folder
        const char *n = dirName;
        while (*n == '/') n++;
        size_t k = strlen(n);
        while (k && n[k - 1] == '/') k--;
        snprintf(d->prefix, sizeof d->prefix, "%.*s%s", (int)k, n, k ? "/" : "");
        d->apk = 1;
    }
    // Android returns a (possibly empty) AAssetDir even for missing directories.
    return d;
}

// Only regular files are listed, like the Android implementation (no subdirectories).
const char *AAssetDir_getNextFileName(AAssetDir *d)
{
    if (d && d->apk) {
        size_t k = strlen(d->prefix);
        pthread_mutex_lock(&g_apk_lock);
        apk_index();
        const char *found = NULL;
        while (!found && d->next < g_nents) {
            const char *nm = g_ents[d->next++].name;
            if (!strncmp(nm, d->prefix, k) && nm[k] && !strchr(nm + k, '/')) {
                snprintf(d->name, sizeof d->name, "%s", nm + k);
                found = d->name;
            }
        }
        pthread_mutex_unlock(&g_apk_lock);
        return found;
    }
    if (!d || !d->dir) return NULL;
    struct dirent *e;
    while ((e = readdir(d->dir))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        int isreg = e->d_type == DT_REG;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
            char p[PATH_MAX + NAME_MAX + 2];
            struct stat st;
            snprintf(p, sizeof p, "%s/%s", d->path, e->d_name);
            isreg = stat(p, &st) == 0 && S_ISREG(st.st_mode);
        }
        if (!isreg) continue;
        snprintf(d->name, sizeof d->name, "%s", e->d_name);
        return d->name;
    }
    return NULL;
}

void AAssetDir_rewind(AAssetDir *d) { if (d && d->dir) rewinddir(d->dir); if (d) d->next = 0; }

void AAssetDir_close(AAssetDir *d)
{
    if (!d) return;
    if (d->dir) closedir(d->dir);
    free(d);
}
