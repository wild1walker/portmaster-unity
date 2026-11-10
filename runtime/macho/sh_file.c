// sh_file.c - files and directories: macOS's open flags, fcntl and ioctl numbers, struct stat,
// struct dirent, the *at flags, statvfs, timeval, poll bits, copyfile; and its paths, which
// name a file whatever the case of the letters (darwin_path).
#define _GNU_SOURCE
#include "darwin.h"
#include "darwin_va.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define PUT(buf, off, v) memcpy((unsigned char *)(buf) + (off), &(v), sizeof(v))
#define GET(buf, off, v) memcpy(&(v), (const unsigned char *)(buf) + (off), sizeof(v))

// --- paths
static int same_nocase(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return 1;
}

// ASCII letters only: a name that differs in the case of another letter, or in how an accent
// is composed (macOS matches those too), is not found.
const char *darwin_path(const char *path, char *buf)
{
    struct stat st;
    int e = errno;
    if (!path || !*path || lstat(path, &st) == 0 || errno != ENOENT || strlen(path) >= PATH_MAX) { errno = e; return path; }
    size_t n = 0;                           // buf[0..n) names what exists
    const char *p = path;
    if (*p == '/') buf[n++] = '/';
    while (*p == '/') p++;
    while (*p) {
        size_t len = strcspn(p, "/");
        memcpy(buf + n, p, len);
        buf[n + len] = 0;
        if (lstat(buf, &st) != 0) {
            // not as spelled: the directory so far, read for the name in another case
            int found = 0;
            buf[n] = 0;
            DIR *d = opendir(n ? buf : ".");
            if (d) {
                struct dirent *de;
                while (!found && (de = readdir(d)))
                    if (strlen(de->d_name) == len && same_nocase(de->d_name, p, len)) {
                        memcpy(buf + n, de->d_name, len);
                        found = 1;
                    }
                closedir(d);
            }
            if (!found) {                   // what does not exist yet stays as it was named
                strcpy(buf + n, p);
                errno = e;
                return buf;
            }
        }
        n += len;
        p += len;
        if (*p == '/') buf[n++] = '/';
        while (*p == '/') p++;
    }
    buf[n] = 0;
    errno = e;
    return buf;
}

#define PATH(p) darwin_path(p, (char[PATH_MAX]){ 0 })

static int at_fd(int fd) { return fd == (int)DARWIN_AT_FDCWD ? AT_FDCWD : fd; }

// A path of an *at call: matched without case when it does not hang on a directory descriptor.
static const char *at_path(int fd, const char *path, char *buf)
{
    return path && (path[0] == '/' || fd == (int)DARWIN_AT_FDCWD) ? darwin_path(path, buf) : path;
}
#define AT_PATH(fd, p) at_path(fd, p, (char[PATH_MAX]){ 0 })

// The *at flags; removedir: 0x80 is AT_REMOVEDIR for unlinkat. -1: a flag with no meaning here.
static int at_flags(int d)
{
    int h = 0;
    if (d & DARWIN_AT_EACCESS) h |= AT_EACCESS;
    if (d & DARWIN_AT_SYMLINK_NOFOLLOW) h |= AT_SYMLINK_NOFOLLOW;
    if (d & DARWIN_AT_SYMLINK_FOLLOW) h |= AT_SYMLINK_FOLLOW;
    if (d & DARWIN_AT_REMOVEDIR) h |= AT_REMOVEDIR;
    if (d & ~(DARWIN_AT_EACCESS | DARWIN_AT_SYMLINK_NOFOLLOW | DARWIN_AT_SYMLINK_FOLLOW | DARWIN_AT_REMOVEDIR)) return -1;
    return h;
}

// --- open
static const struct { int darwin, host; } open_flags[] = {
    { DARWIN_O_NONBLOCK, O_NONBLOCK }, { DARWIN_O_APPEND, O_APPEND }, { DARWIN_O_ASYNC, O_ASYNC },
    { DARWIN_O_SYNC, O_SYNC }, { DARWIN_O_NOFOLLOW, O_NOFOLLOW }, { DARWIN_O_CREAT, O_CREAT },
    { DARWIN_O_TRUNC, O_TRUNC }, { DARWIN_O_EXCL, O_EXCL }, { DARWIN_O_NOCTTY, O_NOCTTY },
    { DARWIN_O_DIRECTORY, O_DIRECTORY }, { DARWIN_O_DSYNC, O_DSYNC }, { DARWIN_O_CLOEXEC, O_CLOEXEC },
    // the link itself, not what it names
    { DARWIN_O_SYMLINK, O_PATH | O_NOFOLLOW },
    // watched only, never read: nothing to ask of Linux
    { DARWIN_O_EVTONLY, 0 },
};
#define NOPEN (sizeof open_flags / sizeof *open_flags)
#define OPEN_LOCKS (DARWIN_O_SHLOCK | DARWIN_O_EXLOCK)

// -1: a flag this does not know
static int open_host(int d)
{
    int h = d & DARWIN_O_ACCMODE, known = DARWIN_O_ACCMODE | OPEN_LOCKS;
    for (size_t i = 0; i < NOPEN; i++) {
        known |= open_flags[i].darwin;
        if (d & open_flags[i].darwin) h |= open_flags[i].host;
    }
    if (d & ~known) {
        static _Atomic int said;
        if (!said) { said = 1; darwin_log("open: flags 0x%x are not answered", d & ~known); }
        return -1;
    }
    return h;
}

static int open_darwin(int h)
{
    int d = h & O_ACCMODE;
    for (size_t i = 0; i < NOPEN; i++)
        if (open_flags[i].host && (h & open_flags[i].host) == open_flags[i].host) d |= open_flags[i].darwin;
    return d;
}

static long open_at(int dirfd, const char *path, int flags, int mode)
{
    int h = open_host(flags);
    if (h < 0) { errno = EINVAL; return -1; }
    int fd = openat(dirfd, path, h, mode);
    if (fd >= 0 && (flags & OPEN_LOCKS)) {
        // BSD's lock taken with the open
        int op = ((flags & DARWIN_O_EXLOCK) ? LOCK_EX : LOCK_SH) | ((flags & DARWIN_O_NONBLOCK) ? LOCK_NB : 0);
        if (flock(fd, op) != 0) { int e = errno; close(fd); errno = e; return -1; }
    }
    return fd;
}

// int open(const char *, int, ...): the mode is a variable argument
static long darwin_open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & DARWIN_O_CREAT) { va_list ap; DARWIN_VA_START(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    return open_at(AT_FDCWD, PATH(path), flags, mode);
}

static long darwin_openat(int fd, const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & DARWIN_O_CREAT) { va_list ap; DARWIN_VA_START(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    return open_at(at_fd(fd), AT_PATH(fd, path), flags, mode);
}

static long darwin_creat(const char *path, int mode)
{
    return open_at(AT_FDCWD, PATH(path), DARWIN_O_CREAT | DARWIN_O_TRUNC | DARWIN_O_WRONLY, mode);
}

// --- fcntl, ioctl
static long lock_call(int fd, int cmd, unsigned char *d)
{
    struct flock fl;
    int64_t start, len;
    int32_t pid;
    int16_t type, whence;
    GET(d, DARWIN_FLOCK_l_start, start);
    GET(d, DARWIN_FLOCK_l_len, len);
    GET(d, DARWIN_FLOCK_l_type, type);
    GET(d, DARWIN_FLOCK_l_whence, whence);
    memset(&fl, 0, sizeof fl);
    fl.l_start = start;
    fl.l_len = len;
    fl.l_whence = whence;                   // SEEK_SET, SEEK_CUR, SEEK_END: the same numbers
    if (type == DARWIN_F_RDLCK) fl.l_type = F_RDLCK;
    else if (type == DARWIN_F_WRLCK) fl.l_type = F_WRLCK;
    else if (type == DARWIN_F_UNLCK) fl.l_type = F_UNLCK;
    else { errno = EINVAL; return -1; }
    int r = fcntl(fd, cmd, &fl);
    if (r == 0 && cmd == F_GETLK) {
        start = fl.l_start;
        len = fl.l_len;
        pid = fl.l_pid;
        type = fl.l_type == F_RDLCK ? DARWIN_F_RDLCK : fl.l_type == F_WRLCK ? DARWIN_F_WRLCK : DARWIN_F_UNLCK;
        whence = fl.l_whence;
        PUT(d, DARWIN_FLOCK_l_start, start);
        PUT(d, DARWIN_FLOCK_l_len, len);
        PUT(d, DARWIN_FLOCK_l_pid, pid);
        PUT(d, DARWIN_FLOCK_l_type, type);
        PUT(d, DARWIN_FLOCK_l_whence, whence);
    }
    return r;
}

// F_PREALLOCATE: room for the file past its end (fstore_t), its size as it was.
static long preallocate(int fd, unsigned char *d)
{
    struct stat st;
    int64_t offset, length;
    int32_t posmode;
    GET(d, DARWIN_FSTORE_fst_posmode, posmode);
    GET(d, DARWIN_FSTORE_fst_offset, offset);
    GET(d, DARWIN_FSTORE_fst_length, length);
    // from the end of the file only: where on the volume the blocks are is not Linux's to say
    if (posmode != DARWIN_F_PEOFPOSMODE || offset != 0 || length < 0) { errno = EINVAL; return -1; }
    if (fstat(fd, &st) != 0) return -1;
    if (length && fallocate(fd, FALLOC_FL_KEEP_SIZE, st.st_size, length) != 0) return -1;
    PUT(d, DARWIN_FSTORE_fst_bytesalloc, length);
    return 0;
}

// int fcntl(int, int, ...)
static long darwin_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    DARWIN_VA_START(ap, cmd);
    long arg = va_arg(ap, long);            // an int or a pointer, in one 8-byte slot
    va_end(ap);
    switch (cmd) {
    case DARWIN_F_DUPFD: return fcntl(fd, F_DUPFD, (int)arg);
    case DARWIN_F_DUPFD_CLOEXEC: return fcntl(fd, F_DUPFD_CLOEXEC, (int)arg);
    case DARWIN_F_GETFD: return fcntl(fd, F_GETFD);         // FD_CLOEXEC is 1 on both
    case DARWIN_F_SETFD: return fcntl(fd, F_SETFD, (int)arg);
    case DARWIN_F_GETFL: { int r = fcntl(fd, F_GETFL); return r < 0 ? r : open_darwin(r); }
    case DARWIN_F_SETFL: {
        int h = open_host((int)arg & ~OPEN_LOCKS);
        if (h < 0) { errno = EINVAL; return -1; }
        return fcntl(fd, F_SETFL, h);
    }
    case DARWIN_F_GETOWN: return fcntl(fd, F_GETOWN);
    case DARWIN_F_SETOWN: return fcntl(fd, F_SETOWN, (int)arg);
    case DARWIN_F_GETLK: return lock_call(fd, F_GETLK, (unsigned char *)arg);
    case DARWIN_F_SETLK: return lock_call(fd, F_SETLK, (unsigned char *)arg);
    case DARWIN_F_SETLKW: return lock_call(fd, F_SETLKW, (unsigned char *)arg);
    case DARWIN_F_FULLFSYNC: case DARWIN_F_BARRIERFSYNC: return fsync(fd);
    case DARWIN_F_PREALLOCATE: return preallocate(fd, (unsigned char *)arg);
    case DARWIN_F_RDAHEAD: case DARWIN_F_NOCACHE:
        // how the kernel is to cache the file: the data read and written is the same
        return fcntl(fd, F_GETFD) < 0 ? -1 : 0;
    case DARWIN_F_GETPATH: {                // into a buffer of macOS's MAXPATHLEN
        char link[64], path[PATH_MAX];
        snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, path, sizeof path - 1);
        if (n < 0) { if (errno == ENOENT) errno = EBADF; return -1; }
        if (n >= DARWIN_MAXPATHLEN) { errno = ENAMETOOLONG; return -1; }
        memcpy((char *)arg, path, (size_t)n);
        ((char *)arg)[n] = 0;
        return 0;
    }
    }
    char what[48];
    snprintf(what, sizeof what, "fcntl(%d)", cmd);
    darwin_unanswered(what, "this command has no answer: EINVAL");
    errno = EINVAL;
    return -1;
}

// int ioctl(int, unsigned long, ...)
static long darwin_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    DARWIN_VA_START(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    switch (request) {
    case DARWIN_FIONREAD: return ioctl(fd, FIONREAD, arg);
    case DARWIN_FIONBIO: return ioctl(fd, FIONBIO, arg);
    case DARWIN_FIOASYNC: return ioctl(fd, FIOASYNC, arg);
    case DARWIN_FIOCLEX: return ioctl(fd, FIOCLEX);
    case DARWIN_FIONCLEX: return ioctl(fd, FIONCLEX);
    case DARWIN_TIOCGWINSZ: return ioctl(fd, TIOCGWINSZ, arg);     // four unsigned shorts on both
    }
    char what[48];
    snprintf(what, sizeof what, "ioctl(0x%lx)", request);
    darwin_unanswered(what, "this request has no answer: ENOTTY");
    errno = ENOTTY;
    return -1;
}

// --- stat
// The time a file was made is not in Linux's stat: the earlier of its two change times.
static void put_stat(unsigned char *d, const struct stat *st)
{
    int32_t dev = (int32_t)st->st_dev, rdev = (int32_t)st->st_rdev, blksize = (int32_t)st->st_blksize;
    uint16_t mode = (uint16_t)st->st_mode;                  // the type bits and the rest: the same numbers
    uint16_t nlink = st->st_nlink > 0xffff ? 0xffff : (uint16_t)st->st_nlink;
    uint64_t ino = st->st_ino;
    uint32_t uid = st->st_uid, gid = st->st_gid;
    int64_t size = st->st_size, blocks = st->st_blocks;
    const struct timespec *born = st->st_mtim.tv_sec < st->st_ctim.tv_sec ||
        (st->st_mtim.tv_sec == st->st_ctim.tv_sec && st->st_mtim.tv_nsec < st->st_ctim.tv_nsec) ? &st->st_mtim : &st->st_ctim;
    memset(d, 0, DARWIN_SIZEOF_STAT);
    PUT(d, DARWIN_STAT_st_dev, dev);
    PUT(d, DARWIN_STAT_st_mode, mode);
    PUT(d, DARWIN_STAT_st_nlink, nlink);
    PUT(d, DARWIN_STAT_st_ino, ino);
    PUT(d, DARWIN_STAT_st_uid, uid);
    PUT(d, DARWIN_STAT_st_gid, gid);
    PUT(d, DARWIN_STAT_st_rdev, rdev);
    PUT(d, DARWIN_STAT_st_atimespec, st->st_atim);          // two 8-byte numbers on both
    PUT(d, DARWIN_STAT_st_mtimespec, st->st_mtim);
    PUT(d, DARWIN_STAT_st_ctimespec, st->st_ctim);
    PUT(d, DARWIN_STAT_st_birthtimespec, *born);
    PUT(d, DARWIN_STAT_st_size, size);
    PUT(d, DARWIN_STAT_st_blocks, blocks);
    PUT(d, DARWIN_STAT_st_blksize, blksize);
}

static long darwin_stat(const char *path, unsigned char *out)
{
    struct stat st;
    if (stat(PATH(path), &st) != 0) return -1;
    put_stat(out, &st);
    return 0;
}

static long darwin_lstat(const char *path, unsigned char *out)
{
    struct stat st;
    if (lstat(PATH(path), &st) != 0) return -1;
    put_stat(out, &st);
    return 0;
}

static long darwin_fstat(int fd, unsigned char *out)
{
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    put_stat(out, &st);
    return 0;
}

static long darwin_fstatat(int fd, const char *path, unsigned char *out, int flags)
{
    struct stat st;
    int h = at_flags(flags);
    if (h < 0) { errno = EINVAL; return -1; }
    if (fstatat(at_fd(fd), AT_PATH(fd, path), &st, h) != 0) return -1;
    put_stat(out, &st);
    return 0;
}

// struct statvfs: the block counts are 32 bits there; a volume with more is told in larger
// blocks, as macOS's own statvfs does.
static void put_statvfs(unsigned char *d, const struct statvfs *v)
{
    uint64_t frsize = v->f_frsize ? v->f_frsize : v->f_bsize, blocks = v->f_blocks, bfree = v->f_bfree, bavail = v->f_bavail;
    while (blocks > UINT32_MAX) { frsize *= 2; blocks /= 2; bfree /= 2; bavail /= 2; }
    uint64_t bsize = v->f_bsize, fsid = v->f_fsid, namemax = v->f_namemax;
    uint64_t flag = ((v->f_flag & ST_RDONLY) ? DARWIN_ST_RDONLY : 0) | ((v->f_flag & ST_NOSUID) ? DARWIN_ST_NOSUID : 0);
    uint32_t b = (uint32_t)blocks, bf = (uint32_t)bfree, ba = (uint32_t)bavail;
    uint32_t files = v->f_files > UINT32_MAX ? UINT32_MAX : (uint32_t)v->f_files;
    uint32_t ffree = v->f_ffree > UINT32_MAX ? UINT32_MAX : (uint32_t)v->f_ffree;
    uint32_t favail = v->f_favail > UINT32_MAX ? UINT32_MAX : (uint32_t)v->f_favail;
    memset(d, 0, DARWIN_SIZEOF_STATVFS);
    PUT(d, DARWIN_STATVFS_f_bsize, bsize);
    PUT(d, DARWIN_STATVFS_f_frsize, frsize);
    PUT(d, DARWIN_STATVFS_f_blocks, b);
    PUT(d, DARWIN_STATVFS_f_bfree, bf);
    PUT(d, DARWIN_STATVFS_f_bavail, ba);
    PUT(d, DARWIN_STATVFS_f_files, files);
    PUT(d, DARWIN_STATVFS_f_ffree, ffree);
    PUT(d, DARWIN_STATVFS_f_favail, favail);
    PUT(d, DARWIN_STATVFS_f_fsid, fsid);
    PUT(d, DARWIN_STATVFS_f_flag, flag);
    PUT(d, DARWIN_STATVFS_f_namemax, namemax);
}

static long darwin_statvfs(const char *path, unsigned char *out)
{
    struct statvfs v;
    if (statvfs(PATH(path), &v) != 0) return -1;
    put_statvfs(out, &v);
    return 0;
}

static long darwin_fstatvfs(int fd, unsigned char *out)
{
    struct statvfs v;
    if (fstatvfs(fd, &v) != 0) return -1;
    put_statvfs(out, &v);
    return 0;
}

// --- directories
// DIR as the images hold it: the descriptor first, where macOS's own keeps it (an old header's
// dirfd is a macro that reads it), and the entry readdir hands out.
struct ddir {
    int      fd;
    uint32_t magic;
    DIR     *host;
    pthread_mutex_t lock;                   // readdir_r
    unsigned char ent[DARWIN_SIZEOF_DIRENT];
};
#define DDIR_MAGIC 0x52694444u

static struct ddir *dd(void *p, const char *call)
{
    struct ddir *d = p;
    if (!d || d->magic != DDIR_MAGIC) {
        darwin_log("%s: a DIR that opendir did not give out (%p)", call, p);
        abort();
    }
    return d;
}

static void put_dirent(unsigned char *d, const struct dirent *e)
{
    size_t len = strlen(e->d_name);         // 255 bytes at most here, 1023 there
    uint64_t ino = e->d_ino, off = (uint64_t)e->d_off;
    uint16_t namlen = (uint16_t)len, reclen = (uint16_t)((DARWIN_DIRENT_d_name + len + 1 + 7) & ~(size_t)7);
    uint8_t type = e->d_type;               // DT_*: the same numbers
    PUT(d, DARWIN_DIRENT_d_ino, ino);
    PUT(d, DARWIN_DIRENT_d_seekoff, off);
    PUT(d, DARWIN_DIRENT_d_reclen, reclen);
    PUT(d, DARWIN_DIRENT_d_namlen, namlen);
    PUT(d, DARWIN_DIRENT_d_type, type);
    memcpy(d + DARWIN_DIRENT_d_name, e->d_name, len + 1);
}

static struct ddir *dir_new(void)
{
    struct ddir *d = malloc(sizeof *d);
    if (!d) { errno = ENOMEM; return NULL; }
    d->magic = DDIR_MAGIC;
    pthread_mutex_init(&d->lock, NULL);
    return d;
}

static void *darwin_opendir(const char *path)
{
    struct ddir *d = dir_new();
    if (!d) return NULL;
    if (!(d->host = opendir(PATH(path)))) { int e = errno; free(d); errno = e; return NULL; }
    d->fd = dirfd(d->host);
    return d;
}

static void *darwin_fdopendir(int fd)
{
    struct ddir *d = dir_new();
    if (!d) return NULL;
    if (!(d->host = fdopendir(fd))) { int e = errno; free(d); errno = e; return NULL; }
    d->fd = fd;
    return d;
}

static void *darwin_readdir(void *dir)
{
    struct ddir *d = dd(dir, "readdir");
    struct dirent *e = readdir(d->host);
    if (!e) return NULL;
    put_dirent(d->ent, e);
    return d->ent;
}

// int readdir_r(DIR *, struct dirent *entry, struct dirent **result): an error number
static long darwin_readdir_r(void *dir, unsigned char *entry, unsigned char **result)
{
    struct ddir *d = dd(dir, "readdir_r");
    int saved = errno, err = 0;
    pthread_mutex_lock(&d->lock);
    errno = 0;
    struct dirent *e = readdir(d->host);
    if (e) put_dirent(entry, e);
    else err = errno;
    pthread_mutex_unlock(&d->lock);
    errno = saved;
    *result = e ? entry : NULL;
    return DARWIN_ERR(err);
}

static long darwin_closedir(void *dir)
{
    struct ddir *d = dd(dir, "closedir");
    int r = closedir(d->host), e = errno;
    d->magic = 0;
    pthread_mutex_destroy(&d->lock);
    free(d);
    errno = e;
    return r;
}

static void darwin_rewinddir(void *dir) { rewinddir(dd(dir, "rewinddir")->host); }
static long darwin_telldir(void *dir) { return telldir(dd(dir, "telldir")->host); }
static void darwin_seekdir(void *dir, long where) { seekdir(dd(dir, "seekdir")->host, where); }
static long darwin_dirfd(void *dir) { return dd(dir, "dirfd")->fd; }

// --- calls that name a file
static long darwin_access(const char *path, int mode) { return access(PATH(path), mode); }     // F_OK, R_OK...: the same numbers
static long darwin_chdir(const char *path) { return chdir(PATH(path)); }
static long darwin_chmod(const char *path, int mode) { return chmod(PATH(path), (mode_t)mode); }
static long darwin_mkdir(const char *path, int mode) { return mkdir(PATH(path), (mode_t)mode); }
static long darwin_rmdir(const char *path) { return rmdir(PATH(path)); }
static long darwin_unlink(const char *path) { return unlink(PATH(path)); }
static long darwin_remove(const char *path) { return remove(PATH(path)); }
static long darwin_rename(const char *from, const char *to) { return rename(PATH(from), PATH(to)); }
static long darwin_link(const char *from, const char *to) { return link(PATH(from), PATH(to)); }
// what the link says is kept as written
static long darwin_symlink(const char *target, const char *path) { return symlink(target, PATH(path)); }
static long darwin_truncate(const char *path, int64_t len) { return truncate(PATH(path), len); }
static long darwin_readlink(const char *path, char *buf, size_t len) { return readlink(PATH(path), buf, len); }

static long darwin_faccessat(int fd, const char *path, int mode, int flags)
{
    int h = at_flags(flags);
    if (h < 0) { errno = EINVAL; return -1; }
    return faccessat(at_fd(fd), AT_PATH(fd, path), mode, h);
}

static long darwin_fchmodat(int fd, const char *path, int mode, int flags)
{
    int h = at_flags(flags);
    if (h < 0) { errno = EINVAL; return -1; }
    return fchmodat(at_fd(fd), AT_PATH(fd, path), (mode_t)mode, h);
}

static long darwin_unlinkat(int fd, const char *path, int flags)
{
    int h = at_flags(flags);
    if (h < 0) { errno = EINVAL; return -1; }
    return unlinkat(at_fd(fd), AT_PATH(fd, path), h);
}

static long darwin_mkdirat(int fd, const char *path, int mode) { return mkdirat(at_fd(fd), AT_PATH(fd, path), (mode_t)mode); }

static long darwin_renameat(int fd1, const char *from, int fd2, const char *to)
{
    return renameat(at_fd(fd1), AT_PATH(fd1, from), at_fd(fd2), AT_PATH(fd2, to));
}

static long darwin_linkat(int fd1, const char *from, int fd2, const char *to, int flags)
{
    int h = at_flags(flags);
    if (h < 0) { errno = EINVAL; return -1; }
    return linkat(at_fd(fd1), AT_PATH(fd1, from), at_fd(fd2), AT_PATH(fd2, to), h);
}

static long darwin_symlinkat(const char *target, int fd, const char *path) { return symlinkat(target, at_fd(fd), AT_PATH(fd, path)); }

static long darwin_readlinkat(int fd, const char *path, char *buf, size_t len)
{
    return readlinkat(at_fd(fd), AT_PATH(fd, path), buf, len);
}

// char *realpath(const char *, char *resolved): resolved holds macOS's PATH_MAX bytes, a
// quarter of what glibc may write; NULL: malloc'd.
static char *darwin_realpath(const char *path, char *resolved)
{
    char *r = realpath(PATH(path), NULL);
    if (!r || !resolved) return r;
    size_t n = strlen(r);
    if (n >= DARWIN_MAXPATHLEN) { free(r); errno = ENAMETOOLONG; return NULL; }
    memcpy(resolved, r, n + 1);
    free(r);
    return resolved;
}

// char *getcwd(char *, size_t): NULL allocates, on both
static char *darwin_getcwd(char *buf, size_t len) { return getcwd(buf, len); }

// off_t lseek(int, off_t, int): the two that find a file's holes have each other's number
static int64_t darwin_lseek(int fd, int64_t off, int whence)
{
    if (whence == DARWIN_SEEK_HOLE) whence = SEEK_HOLE;
    else if (whence == DARWIN_SEEK_DATA) whence = SEEK_DATA;
    return lseek(fd, off, whence);
}

// --- times
_Static_assert(DARWIN_SIZEOF_TIMESPEC == sizeof(struct timespec), "timespec");

static const struct timespec *ts_host(const struct timespec *d, struct timespec *h)
{
    if (!d) return NULL;
    for (int i = 0; i < 2; i++) {
        h[i] = d[i];
        if (d[i].tv_nsec == (long)DARWIN_UTIME_NOW) h[i].tv_nsec = UTIME_NOW;
        else if (d[i].tv_nsec == (long)DARWIN_UTIME_OMIT) h[i].tv_nsec = UTIME_OMIT;
    }
    return h;
}

static long darwin_utimensat(int fd, const char *path, const struct timespec *times, int flags)
{
    struct timespec t[2];
    int h = at_flags(flags);
    if (h < 0) { errno = EINVAL; return -1; }
    return utimensat(at_fd(fd), AT_PATH(fd, path), ts_host(times, t), h);
}

static long darwin_futimens(int fd, const struct timespec *times)
{
    struct timespec t[2];
    return futimens(fd, ts_host(times, t));
}

// struct timeval: the microseconds are an int there, after 8 bytes of seconds
static struct timeval *tv_host(const unsigned char *d, struct timeval *h, int count)
{
    if (!d) return NULL;
    for (int i = 0; i < count; i++, d += DARWIN_SIZEOF_TIMEVAL) {
        int64_t sec;
        int32_t usec;
        GET(d, 0, sec);
        GET(d, DARWIN_TIMEVAL_tv_usec, usec);
        h[i].tv_sec = sec;
        h[i].tv_usec = usec;
    }
    return h;
}

static long darwin_utimes(const char *path, const unsigned char *times)
{
    struct timeval t[2];
    return utimes(PATH(path), tv_host(times, t, 2));
}

static long darwin_lutimes(const char *path, const unsigned char *times)
{
    struct timeval t[2];
    return lutimes(PATH(path), tv_host(times, t, 2));
}

static long darwin_futimes(int fd, const unsigned char *times)
{
    struct timeval t[2];
    return futimes(fd, tv_host(times, t, 2));
}

// --- select, poll
// fd_set: 1024 bits on both, in 32-bit words there and 64-bit words here, the same bytes.
// The time left is not written back: macOS leaves the timeval as it was.
// A wait that a thread_suspend's signal interrupted goes on for the time it had left: see
// darwin_thread_stops.
static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
_Static_assert(DARWIN_SIZEOF_FD_SET == sizeof(fd_set), "fd_set");

static long darwin_select(int n, fd_set *r, fd_set *w, fd_set *e, const unsigned char *timeout)
{
    struct timeval t, *tv;
    if (n > DARWIN_FD_SETSIZE) { errno = EINVAL; return -1; }
    tv = tv_host(timeout, &t, 1);
    int64_t end = tv ? mono_ns() + (int64_t)tv->tv_sec * 1000000000 + (int64_t)tv->tv_usec * 1000 : 0;
    for (;;) {
        unsigned stops = darwin_thread_stops();
        int got = select(n, r, w, e, tv);           // an error leaves the sets as they were
        if (got >= 0 || errno != EINTR || stops == darwin_thread_stops()) return got;
        if (tv) {
            int64_t left = end - mono_ns();
            if (left < 0) left = 0;
            tv->tv_sec = left / 1000000000;
            tv->tv_usec = left % 1000000000 / 1000;
        }
    }
}

// poll's bits: writing "normal" data is POLLOUT's bit there, and the band bit sits lower
_Static_assert(DARWIN_SIZEOF_POLLFD == sizeof(struct pollfd) && DARWIN_SIZEOF_NFDS_T == 4, "pollfd");
#define POLL_SAME (POLLIN | POLLPRI | POLLOUT | POLLERR | POLLHUP | POLLNVAL | POLLRDNORM | POLLRDBAND)
_Static_assert(DARWIN_POLLIN == POLLIN && DARWIN_POLLPRI == POLLPRI && DARWIN_POLLOUT == POLLOUT && DARWIN_POLLERR == POLLERR &&
               DARWIN_POLLHUP == POLLHUP && DARWIN_POLLNVAL == POLLNVAL && DARWIN_POLLRDNORM == POLLRDNORM &&
               DARWIN_POLLRDBAND == POLLRDBAND && DARWIN_POLLWRNORM == DARWIN_POLLOUT, "poll bits");

static short poll_host(short d)
{
    return (short)((d & POLL_SAME) | ((d & DARWIN_POLLWRBAND) ? POLLWRBAND : 0));
}

static short poll_darwin(short h)
{
    return (short)((h & POLL_SAME) | ((h & POLLWRNORM) ? DARWIN_POLLWRNORM : 0) | ((h & POLLWRBAND) ? DARWIN_POLLWRBAND : 0));
}

// int poll(struct pollfd *, nfds_t, int): nfds_t is 32 bits there
static long darwin_poll(struct pollfd *fds, unsigned int n, int timeout)
{
    struct pollfd small[64], *h = n <= 64 ? small : malloc(n * sizeof *h);
    if (!h) { errno = ENOMEM; return -1; }
    for (unsigned int i = 0; i < n; i++) {
        h[i].fd = fds[i].fd;
        h[i].events = poll_host(fds[i].events);
        h[i].revents = 0;
    }
    int64_t end = timeout > 0 ? mono_ns() + (int64_t)timeout * 1000000 : 0;
    int r, e;
    for (;;) {
        unsigned stops = darwin_thread_stops();
        r = poll(h, n, timeout);
        e = errno;
        if (r >= 0 || e != EINTR || stops == darwin_thread_stops()) break;
        if (timeout > 0) {
            int64_t left = end - mono_ns();
            timeout = left > 0 ? (int)((left + 999999) / 1000000) : 0;
        }
    }
    for (unsigned int i = 0; i < n; i++) fds[i].revents = r < 0 ? fds[i].revents : poll_darwin(h[i].revents);
    if (h != small) free(h);
    errno = e;
    return r;
}

// --- copyfile
// A copy's state is asked for and freed, nothing more: what it could report is not read by a
// copy between two descriptors.
static void *darwin_copyfile_state_alloc(void) { return calloc(1, 64); }
static long darwin_copyfile_state_free(void *state) { free(state); return 0; }

// int fcopyfile(int from, int to, copyfile_state_t, copyfile_flags_t): the data from where
// each descriptor stands, the file cut after it; then the mode and the times. Access lists
// and extended attributes: these files have none to carry over.
static long darwin_fcopyfile(int from, int to, void *state, uint32_t flags)
{
    struct stat st;
    if (flags & ~(uint32_t)DARWIN_COPYFILE_ALL) {
        darwin_unanswered("fcopyfile", "a flag besides what to copy has no answer: EINVAL");
        errno = EINVAL;
        return -1;
    }
    if (fstat(from, &st) != 0) return -1;
    if (flags & DARWIN_COPYFILE_DATA) {
        size_t cap = 1 << 16;
        char *buf = malloc(cap);
        int64_t total = 0;
        if (!buf) { errno = ENOMEM; return -1; }
        for (;;) {
            ssize_t n = read(from, buf, cap);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { if (n < 0) { int e = errno; free(buf); errno = e; return -1; } break; }
            for (ssize_t done = 0; done < n;) {
                ssize_t w = write(to, buf + done, (size_t)(n - done));
                if (w < 0 && errno == EINTR) continue;
                if (w < 0) { int e = errno; free(buf); errno = e; return -1; }
                done += w;
            }
            total += n;
        }
        free(buf);
        if (ftruncate(to, total) != 0) return -1;
    }
    if (flags & DARWIN_COPYFILE_STAT) {
        // as copyfile does: what the file system will not take is passed over
        int e = errno;
        struct timespec t[2] = { st.st_atim, st.st_mtim };
        (void)!fchmod(to, st.st_mode & 07777);
        (void)!futimens(to, t);
        errno = e;
    }
    return 0;
}

const struct darwin_sym darwin_syms_file[] = {
    SHIM(open), SHIM(openat), SHIM(creat), SHIM(fcntl), SHIM(ioctl),
    SHIM(stat), SHIM(lstat), SHIM(fstat), SHIM(fstatat), SHIM(statvfs), SHIM(fstatvfs),
    SHIM(opendir), SHIM(fdopendir), SHIM(readdir), SHIM(readdir_r), SHIM(closedir), SHIM(rewinddir),
    SHIM(telldir), SHIM(seekdir), SHIM(dirfd),
    SHIM(access), SHIM(chdir), SHIM(chmod), SHIM(mkdir), SHIM(rmdir), SHIM(unlink), SHIM(remove), SHIM(rename),
    SHIM(link), SHIM(symlink), SHIM(truncate), SHIM(readlink),
    SHIM(faccessat), SHIM(fchmodat), SHIM(unlinkat), SHIM(mkdirat), SHIM(renameat), SHIM(linkat), SHIM(symlinkat),
    SHIM(readlinkat),
    SHIM(realpath), NAMED("_realpath$DARWIN_EXTSN", darwin_realpath), SHIM(getcwd), SHIM(lseek),
    SHIM(utimensat), SHIM(futimens), SHIM(utimes), SHIM(lutimes), SHIM(futimes),
    SHIM(select), SHIM(poll),
    SHIM(copyfile_state_alloc), SHIM(copyfile_state_free), SHIM(fcopyfile),
    // a descriptor and plain numbers: glibc's own (off_t and ssize_t are 64 bits on both; flock's
    // operations and iovec are the same)
    SAME(close), SAME(dup), SAME(dup2), SAME(pipe), SAME(read), SAME(write), SAME(pread), SAME(pwrite),
    SAME(readv), SAME(writev), SAME(ftruncate), SAME(fchmod), SAME(fchdir), SAME(fsync), SAME(flock),
    { NULL, NULL }
};
