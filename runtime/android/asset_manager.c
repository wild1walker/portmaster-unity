// libandroid.so AAssetManager_* / AAsset_* / AAssetDir_*, backed by a host directory.
// (Unity 2017.4 itself reads the APK zip directly and does not import these; they are here for
// completeness and for any plugin that asks for them.)
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

struct AAssetManager { char root[PATH_MAX]; };
struct AAsset {
    int fd;
    long long length, pos;
    void *map;        // AAsset_getBuffer mapping (whole file), lazily created
};
struct AAssetDir {
    DIR *dir;
    char path[PATH_MAX];
    char name[NAME_MAX + 1];
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
    if (fd < 0) { SHIM_TRACE("AAssetManager_open(%s): %s", filename, strerror(errno)); return NULL; }
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
    ssize_t r = pread64(a->fd, buf, count, a->pos);
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
    if (a->map) munmap(a->map, a->length ? a->length : 1);
    close(a->fd);
    free(a);
}

const void *AAsset_getBuffer(AAsset *a)
{
    if (!a) return NULL;
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
    if (!a) return -1;
    int fd = fcntl(a->fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (outStart) *outStart = 0;
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

int AAsset_isAllocated(AAsset *a) { (void)a; return 0; }

AAssetDir *AAssetManager_openDir(AAssetManager *mgr, const char *dirName)
{
    AAssetDir *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    if (asset_path(mgr, dirName ? dirName : "", d->path, sizeof d->path) == 0)
        d->dir = opendir(d->path);
    // Android returns a (possibly empty) AAssetDir even for missing directories.
    return d;
}

// Only regular files are listed, like the Android implementation (no subdirectories).
const char *AAssetDir_getNextFileName(AAssetDir *d)
{
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

void AAssetDir_rewind(AAssetDir *d) { if (d && d->dir) rewinddir(d->dir); }

void AAssetDir_close(AAssetDir *d)
{
    if (!d) return;
    if (d->dir) closedir(d->dir);
    free(d);
}
