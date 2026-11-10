// sh_mem.c - memory and strings: malloc and the str* and mem* calls are glibc's own; mmap
// speaks Darwin's flags and hands out memory on macOS's 16 KiB page grid.
#define _GNU_SOURCE
#include "darwin.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wchar.h>

// glibc's fortified entries: (dst, ..., n, the bytes dst has), abort on overflow like macOS's
extern void *__memcpy_chk(void *, const void *, size_t, size_t);
extern void *__memmove_chk(void *, const void *, size_t, size_t);
extern void *__memset_chk(void *, int, size_t, size_t);
extern char *__strcpy_chk(char *, const char *, size_t);
extern char *__stpcpy_chk(char *, const char *, size_t);
extern char *__strncpy_chk(char *, const char *, size_t, size_t);
extern char *__stpncpy_chk(char *, const char *, size_t, size_t);
extern char *__strcat_chk(char *, const char *, size_t);
extern char *__strncat_chk(char *, const char *, size_t, size_t);

size_t darwin_page_size(void)
{
    static size_t page;
    if (!page) {
        size_t host = (size_t)sysconf(_SC_PAGESIZE);
        page = host > 16384 ? host : 16384;
    }
    return page;
}

static long darwin_getpagesize(void) { return (long)darwin_page_size(); }

static long darwin_posix_memalign(void **out, size_t align, size_t size)
{
    return DARWIN_ERR(posix_memalign(out, align, size));
}

// macOS's mmap never returns an address off its 16 KiB grid and code built for it may count
// on that (a collector's block headers, a page-table of its own). Where the kernel's pages are
// smaller, the range is reserved with room to spare, the aligned part mapped over it and the
// rest given back.
static void *darwin_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    const int known = DARWIN_MAP_SHARED | DARWIN_MAP_PRIVATE | DARWIN_MAP_FIXED | DARWIN_MAP_ANON | DARWIN_MAP_NORESERVE |
                      DARWIN_MAP_JIT | DARWIN_MAP_NOCACHE | DARWIN_MAP_HASSEMAPHORE | DARWIN_MAP_NOEXTEND;
    if (flags & ~known) {
        darwin_log("mmap: flags 0x%x not answered", flags & ~known);
        errno = EINVAL;
        return MAP_FAILED;
    }
    int f = 0;
    if (flags & DARWIN_MAP_SHARED) f |= MAP_SHARED;
    if (flags & DARWIN_MAP_PRIVATE) f |= MAP_PRIVATE;
    if (flags & DARWIN_MAP_FIXED) f |= MAP_FIXED;
    if (flags & DARWIN_MAP_ANON) { f |= MAP_ANONYMOUS; fd = -1; }
    if (flags & DARWIN_MAP_NORESERVE) f |= MAP_NORESERVE;
    // MAP_JIT, MAP_NOCACHE, MAP_HASSEMAPHORE, MAP_NOEXTEND: hints with nothing to do here
    size_t page = darwin_page_size(), host = (size_t)sysconf(_SC_PAGESIZE);
    if ((f & MAP_FIXED) || page == host || len == 0 || len > SIZE_MAX - 2 * page) return mmap(addr, len, prot, f, fd, off);
    size_t span = ((len + host - 1) & ~(host - 1)) + page - host;
    uint8_t *r = mmap(addr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (r == MAP_FAILED) return MAP_FAILED;
    uint8_t *at = (uint8_t *)(((uintptr_t)r + page - 1) & ~(uintptr_t)(page - 1));
    void *p = mmap(at, len, prot, f | MAP_FIXED, fd, off);
    if (p == MAP_FAILED) {
        int e = errno;
        munmap(r, span);
        errno = e;
        return MAP_FAILED;
    }
    uint8_t *end = at + ((len + host - 1) & ~(host - 1));
    if (at > r) munmap(r, (size_t)(at - r));
    if (r + span > end) munmap(end, (size_t)(r + span - end));
    return p;
}

static void pattern(void *b, const void *pattern, size_t len, size_t unit)
{
    uint8_t *d = b;
    while (len >= unit) { memcpy(d, pattern, unit); d += unit; len -= unit; }
    memcpy(d, pattern, len);
}
static void darwin_memset_pattern4(void *b, const void *p, size_t len) { pattern(b, p, len, 4); }
static void darwin_memset_pattern8(void *b, const void *p, size_t len) { pattern(b, p, len, 8); }
static void darwin_memset_pattern16(void *b, const void *p, size_t len) { pattern(b, p, len, 16); }

static size_t darwin_strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

static size_t darwin_strlcat(char *dst, const char *src, size_t size)
{
    size_t have = strnlen(dst, size), n = strlen(src);
    if (have == size) return size + n;          // no end within size: nothing is written
    size_t c = n < size - have - 1 ? n : size - have - 1;
    memcpy(dst + have, src, c);
    dst[have + c] = 0;
    return have + n;
}

// little within the first len bytes of big, which ends at its 0
static char *darwin_strnstr(const char *big, const char *little, size_t len)
{
    size_t n = strlen(little);
    if (!n) return (char *)big;
    for (; len >= n && *big; big++, len--)
        if (*big == *little && !strncmp(big, little, n)) return (char *)big;
    return NULL;
}

// The fortified entries glibc has not: (dst, ..., n, the bytes dst has).
static void overflow(const char *what)
{
    fprintf(stderr, "[darwin] %s: more bytes than its destination has\n", what);
    abort();
}
static size_t darwin___strlcpy_chk(char *dst, const char *src, size_t n, size_t have)
{
    if (n > have) overflow("strlcpy");
    return darwin_strlcpy(dst, src, n);
}
static size_t darwin___strlcat_chk(char *dst, const char *src, size_t n, size_t have)
{
    if (n > have) overflow("strlcat");
    return darwin_strlcat(dst, src, n);
}
static void *darwin___memccpy_chk(void *dst, const void *src, int c, size_t n, size_t have)
{
    if (n > have) overflow("memccpy");
    return memccpy(dst, src, c, n);
}

// macOS's wcscmp and wcsncmp answer how far apart the first two different values are, as an
// int (glibc: -1 or 1); its wmemcmp orders them as the signed values wchar_t has there, and
// on arm64 Linux wchar_t is unsigned.
static long darwin_wcscmp(const uint32_t *a, const uint32_t *b)
{
    for (; *a == *b; a++, b++) if (!*a) return 0;
    return (int32_t)(*a - *b);
}
static long darwin_wcsncmp(const uint32_t *a, const uint32_t *b, size_t n)
{
    for (; n && *a == *b; a++, b++, n--) if (!*a) return 0;
    return n ? (int32_t)(*a - *b) : 0;
}
static long darwin_wmemcmp(const int32_t *a, const int32_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

const struct darwin_sym darwin_syms_mem[] = {
    SAME(malloc), SAME(calloc), SAME(realloc), SAME(free), SAME(aligned_alloc), SHIM(posix_memalign),
    SHIM(mmap), SAME(munmap), SAME(mprotect), SHIM(getpagesize),
    SAME(bzero), SAME(memchr), SAME(memcmp), SAME(memcpy), SAME(memmove), SAME(memset), SAME(memccpy), SAME(memmem),
    SHIM(memset_pattern4), SHIM(memset_pattern8), SHIM(memset_pattern16),
    SAME(__memcpy_chk), SAME(__memmove_chk), SAME(__memset_chk), SHIM(__memccpy_chk),
    SAME(__strcpy_chk), SAME(__stpcpy_chk), SAME(__strncpy_chk), SAME(__stpncpy_chk), SAME(__strcat_chk), SAME(__strncat_chk),
    SHIM(__strlcpy_chk), SHIM(__strlcat_chk),
    SAME(strchr), SAME(strcmp), SAME(strcpy), SAME(strdup), SHIM(strlcpy), SAME(strlen), SAME(strncmp),
    SAME(strncat), SAME(strncpy), SAME(strrchr), SAME(strspn), SAME(strstr), SAME(strtok),
    SAME(strcat), SHIM(strlcat), SAME(stpcpy), SAME(stpncpy), SAME(strnlen), SAME(strndup), SAME(strpbrk), SAME(strcspn),
    SAME(strsep), SAME(strtok_r), SAME(strcasecmp), SAME(strncasecmp), SAME(strcasestr), SHIM(strnstr), SAME(index), SAME(rindex),
    SAME(wcslen), SAME(wmemchr), SHIM(wmemcmp), SHIM(wcscmp), SHIM(wcsncmp),
    SAME(wcscpy), SAME(wcsncpy), SAME(wcscat), SAME(wcschr), SAME(wcsrchr), SAME(wcsstr), SAME(wmemcpy), SAME(wmemmove), SAME(wmemset),
    SAME(bsearch), SAME(qsort),
    { NULL, NULL }
};
