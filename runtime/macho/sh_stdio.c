// sh_stdio.c - stdio. A FILE the images hold is macOS's own layout (its headers' macros read
// and write the fields: getc_unlocked, putc_unlocked, feof_unlocked, clearerr_unlocked,
// fileno_unlocked) wrapped around glibc's: the counts the macros test stay 0, so each of them
// falls into __srget or __swbuf, answered here; the end-of-file and error bits and the
// descriptor mirror glibc's after every call. Formats go through darwin_fmt (sh_fmt.inc), the
// variable arguments are read from the stack (darwin_va.h).
#define _GNU_SOURCE
#include "darwin.h"
#include "darwin_va.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#define CH char
#define FMT_PASS fmt_pass
#define FMT_FN darwin_fmt
#include "sh_fmt.inc"
#undef CH
#undef FMT_PASS
#undef FMT_FN
#define CH wchar_t
#define FMT_PASS wfmt_pass
#define FMT_FN darwin_wfmt
#include "sh_fmt.inc"
#undef CH
#undef FMT_PASS
#undef FMT_FN

// C99's scanf whatever this file is built as: with _GNU_SOURCE glibc's own names read "%as" as
// its old allocating extension; macOS reads a float and an s.
extern int __isoc99_vsscanf(const char *, const char *, va_list);
extern int __isoc99_vfscanf(FILE *, const char *, va_list);
extern int __isoc99_vswscanf(const wchar_t *, const wchar_t *, va_list);

#define PUT(buf, off, v) memcpy((unsigned char *)(buf) + (off), &(v), sizeof(v))
#define GET(buf, off, v) memcpy(&(v), (const unsigned char *)(buf) + (off), sizeof(v))

struct dfile {
    _Alignas(8) unsigned char d[DARWIN_SIZEOF_FILE];    // what the image sees
    FILE    *host;
    uint32_t magic;
};
#define DFILE_MAGIC 0x656c6946u
#define ENDS (DARWIN___SEOF | DARWIN___SERR)

static struct dfile std[3];
static void *cell_stdin = &std[0], *cell_stdout = &std[1], *cell_stderr = &std[2];

static struct dfile *df(void *file)
{
    struct dfile *f = file;
    if (f->magic != DFILE_MAGIC) {
        fprintf(stderr, "[darwin] stdio was handed a FILE it did not give out (%p)\n", file);
        abort();
    }
    return f;
}

// glibc's stream for a call. clearerr_unlocked is a macro there that clears the two bits in the
// image's FILE only: seen here, and done.
static FILE *in(void *file)
{
    struct dfile *f = df(file);
    int16_t flags;
    GET(f->d, DARWIN_FILE__flags, flags);
    if (!(flags & ENDS) && (feof_unlocked(f->host) || ferror_unlocked(f->host))) clearerr(f->host);
    return f->host;
}

// After a call: the bits as glibc has them, the macros' counts back to 0.
static void out(void *file)
{
    struct dfile *f = file;
    int16_t flags;
    int32_t zero = 0;
    GET(f->d, DARWIN_FILE__flags, flags);
    flags &= ~ENDS;
    if (feof_unlocked(f->host)) flags |= DARWIN___SEOF;
    if (ferror_unlocked(f->host)) flags |= DARWIN___SERR;
    PUT(f->d, DARWIN_FILE__flags, flags);
    PUT(f->d, DARWIN_FILE__r, zero);
    PUT(f->d, DARWIN_FILE__w, zero);
}

static void describe(struct dfile *f)
{
    int e = errno, fd = fileno(f->host), mode = fd >= 0 ? fcntl(fd, F_GETFL) : -1;
    int16_t flags = mode < 0 ? DARWIN___SRW : (mode & O_ACCMODE) == O_RDONLY ? DARWIN___SRD :
                    (mode & O_ACCMODE) == O_WRONLY ? DARWIN___SWR : DARWIN___SRW;
    int16_t file = (int16_t)fd;
    memset(f->d, 0, sizeof f->d);
    PUT(f->d, DARWIN_FILE__flags, flags);
    PUT(f->d, DARWIN_FILE__file, file);
    f->magic = DFILE_MAGIC;
    errno = e;
    out(f);
}

static void *wrap(FILE *host)
{
    if (!host) return NULL;
    struct dfile *f = calloc(1, sizeof *f);
    if (!f) { fclose(host); errno = ENOMEM; return NULL; }
    f->host = host;
    describe(f);
    return f;
}

static void unwrap(struct dfile *f)
{
    if (f >= std && f < std + 3) return;
    f->magic = 0;
    free(f);
}

__attribute__((constructor)) static void std_init(void)
{
    FILE *h[3] = { stdin, stdout, stderr };
    for (int i = 0; i < 3; i++) { std[i].host = h[i]; describe(&std[i]); }
}

// --- streams
static void *darwin_fopen(const char *path, const char *mode)
{
    char buf[PATH_MAX];
    return wrap(fopen(darwin_path(path, buf), mode));
}

static void *darwin_fdopen(int fd, const char *mode) { return wrap(fdopen(fd, mode)); }
static void *darwin_tmpfile(void) { return wrap(tmpfile()); }

static void *darwin_freopen(const char *path, const char *mode, void *file)
{
    char buf[PATH_MAX];
    struct dfile *f = df(file);
    FILE *h = freopen(path ? darwin_path(path, buf) : NULL, mode, f->host);
    if (!h) { unwrap(f); return NULL; }     // the stream is closed
    f->host = h;
    describe(f);
    return f;
}

static long darwin_fclose(void *file)
{
    struct dfile *f = df(file);
    int r = fclose(f->host);
    unwrap(f);
    return r;
}

static long darwin_fflush(void *file)
{
    if (!file) return fflush(NULL);
    int r = fflush(in(file));
    out(file);
    return r;
}

static long darwin_fileno(void *file) { return fileno(df(file)->host); }
static long darwin_feof(void *file) { return feof(in(file)) != 0; }
static long darwin_ferror(void *file) { return ferror(in(file)) != 0; }
static void darwin_clearerr(void *file) { clearerr(df(file)->host); out(file); }
static void darwin_flockfile(void *file) { flockfile(df(file)->host); }
static void darwin_funlockfile(void *file) { funlockfile(df(file)->host); }
static long darwin_ftrylockfile(void *file) { return ftrylockfile(df(file)->host); }

// A buffer of setbuf's is BUFSIZ bytes: 1024 there, more here.
static void darwin_setbuf(void *file, char *buf)
{
    setvbuf(df(file)->host, buf, buf ? _IOFBF : _IONBF, DARWIN_BUFSIZ);
}

static long darwin_setvbuf(void *file, char *buf, int mode, size_t size)
{
    int m = mode == DARWIN__IOFBF ? _IOFBF : mode == DARWIN__IOLBF ? _IOLBF : mode == DARWIN__IONBF ? _IONBF : -1;
    return setvbuf(df(file)->host, buf, m, size);
}

// --- bytes and characters
#define CALL(type, expr) do { FILE *h = in(file); type r = (expr); (void)h; out(file); return r; } while (0)
static size_t darwin_fread(void *p, size_t size, size_t n, void *file) { CALL(size_t, fread(p, size, n, h)); }
static size_t darwin_fwrite(const void *p, size_t size, size_t n, void *file) { CALL(size_t, fwrite(p, size, n, h)); }
static char *darwin_fgets(char *s, int n, void *file) { CALL(char *, fgets(s, n, h)); }
static long darwin_fputs(const char *s, void *file) { CALL(long, fputs(s, h)); }
static long darwin_fputc(int c, void *file) { CALL(long, fputc(c, h)); }
static long darwin_fgetc(void *file) { CALL(long, fgetc(h)); }
static long darwin_ungetc(int c, void *file) { CALL(long, ungetc(c, h)); }
static long darwin_getc_unlocked(void *file) { CALL(long, getc_unlocked(h)); }
static long darwin_putc_unlocked(int c, void *file) { CALL(long, putc_unlocked(c, h)); }
static wint_t darwin_fgetwc(void *file) { CALL(wint_t, fgetwc(h)); }
static wint_t darwin_fputwc(wchar_t c, void *file) { CALL(wint_t, fputwc(c, h)); }
static wint_t darwin_ungetwc(wint_t c, void *file) { CALL(wint_t, ungetwc(c, h)); }
static ssize_t darwin_getline(char **line, size_t *cap, void *file) { CALL(ssize_t, getline(line, cap, h)); }
static long darwin_fseek(void *file, long off, int whence) { CALL(long, fseek(h, off, whence)); }
static long darwin_fseeko(void *file, off_t off, int whence) { CALL(long, fseeko(h, off, whence)); }
static off_t darwin_ftello(void *file) { CALL(off_t, ftello(h)); }
static void darwin_rewind(void *file) { rewind(in(file)); out(file); }

static long darwin_puts(const char *s) { void *file = &std[1]; CALL(long, puts(s)); }
static long darwin_putchar(int c) { return darwin_fputc(c, &std[1]); }
static long darwin_getchar(void) { return darwin_fgetc(&std[0]); }

// fpos_t is an offset there
static long darwin_fgetpos(void *file, int64_t *pos)
{
    off_t at = darwin_ftello(file);
    if (at < 0) return -1;
    *pos = at;
    return 0;
}

static long darwin_fsetpos(void *file, const int64_t *pos) { return darwin_fseeko(file, *pos, SEEK_SET); }

// --- formatted
#define FMT(fmt, scan) \
    char fbuf_[256]; \
    const char *g = darwin_fmt(fmt, scan, fbuf_, sizeof fbuf_); \
    if (!g) { errno = ENOMEM; return -1; }
#define FMT_DONE(fmt) do { if (g != (fmt) && g != fbuf_) free((void *)g); } while (0)
// with: the call is made in a locale the caller names (NULL: C's), the _l names
#define IN_LOCALE(with, loc, stmt) do { if (with) DARWIN_WITH_LOCALE(loc, stmt); else stmt; } while (0)

static long v_fprintf(void *file, const char *fmt, va_list ap)
{
    FMT(fmt, 0);
    int r = vfprintf(in(file), g, ap);
    out(file);
    FMT_DONE(fmt);
    return r;
}

static long v_sprintf(char *s, const char *fmt, va_list ap)
{
    FMT(fmt, 0);
    int r = vsprintf(s, g, ap);
    FMT_DONE(fmt);
    return r;
}

static long v_snprintf(char *s, size_t n, int with, void *loc, const char *fmt, va_list ap)
{
    FMT(fmt, 0);
    int r;
    IN_LOCALE(with, loc, r = vsnprintf(s, n, g, ap));
    FMT_DONE(fmt);
    return r;
}

static long v_asprintf(char **ret, int with, void *loc, const char *fmt, va_list ap)
{
    FMT(fmt, 0);
    int r;
    IN_LOCALE(with, loc, r = vasprintf(ret, g, ap));
    FMT_DONE(fmt);
    return r;
}

static long v_sscanf(const char *s, int with, void *loc, const char *fmt, va_list ap)
{
    FMT(fmt, 1);
    int r;
    IN_LOCALE(with, loc, r = __isoc99_vsscanf(s, g, ap));
    FMT_DONE(fmt);
    return r;
}

static long v_fscanf(void *file, const char *fmt, va_list ap)
{
    FMT(fmt, 1);
    int r = __isoc99_vfscanf(in(file), g, ap);
    out(file);
    FMT_DONE(fmt);
    return r;
}

#define WFMT(fmt, scan) \
    wchar_t fbuf_[128]; \
    const wchar_t *g = darwin_wfmt(fmt, scan, fbuf_, sizeof fbuf_ / sizeof *fbuf_); \
    if (!g) { errno = ENOMEM; return -1; }

static long v_swprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap)
{
    WFMT(fmt, 0);
    int r = vswprintf(s, n, g, ap);
    FMT_DONE(fmt);
    return r;
}

static long v_swscanf(const wchar_t *s, const wchar_t *fmt, va_list ap)
{
    WFMT(fmt, 1);
    int r = __isoc99_vswscanf(s, g, ap);
    FMT_DONE(fmt);
    return r;
}

// The same call from its two shapes: the arguments after last (ARGS), or a va_list of macOS's (LIST).
#define ARGS(last, call) do { va_list ap; DARWIN_VA_START(ap, last); long r = (call); va_end(ap); return r; } while (0)
#define LIST(list, call) do { va_list ap; DARWIN_VA_FROM(ap, list); return (call); } while (0)

static long darwin_printf(const char *fmt, ...) { ARGS(fmt, v_fprintf(&std[1], fmt, ap)); }
static long darwin_vprintf(const char *fmt, void *l) { LIST(l, v_fprintf(&std[1], fmt, ap)); }
static long darwin_fprintf(void *file, const char *fmt, ...) { ARGS(fmt, v_fprintf(file, fmt, ap)); }
static long darwin_vfprintf(void *file, const char *fmt, void *l) { LIST(l, v_fprintf(file, fmt, ap)); }
static long darwin_sprintf(char *s, const char *fmt, ...) { ARGS(fmt, v_sprintf(s, fmt, ap)); }
static long darwin_vsprintf(char *s, const char *fmt, void *l) { LIST(l, v_sprintf(s, fmt, ap)); }
static long darwin_snprintf(char *s, size_t n, const char *fmt, ...) { ARGS(fmt, v_snprintf(s, n, 0, NULL, fmt, ap)); }
static long darwin_vsnprintf(char *s, size_t n, const char *fmt, void *l) { LIST(l, v_snprintf(s, n, 0, NULL, fmt, ap)); }
static long darwin_snprintf_l(char *s, size_t n, void *loc, const char *fmt, ...) { ARGS(fmt, v_snprintf(s, n, 1, loc, fmt, ap)); }
static long darwin_vsnprintf_l(char *s, size_t n, void *loc, const char *fmt, void *l) { LIST(l, v_snprintf(s, n, 1, loc, fmt, ap)); }
static long darwin_asprintf(char **ret, const char *fmt, ...) { ARGS(fmt, v_asprintf(ret, 0, NULL, fmt, ap)); }
static long darwin_vasprintf(char **ret, const char *fmt, void *l) { LIST(l, v_asprintf(ret, 0, NULL, fmt, ap)); }
static long darwin_asprintf_l(char **ret, void *loc, const char *fmt, ...) { ARGS(fmt, v_asprintf(ret, 1, loc, fmt, ap)); }
static long darwin_vasprintf_l(char **ret, void *loc, const char *fmt, void *l) { LIST(l, v_asprintf(ret, 1, loc, fmt, ap)); }
static long darwin_sscanf(const char *s, const char *fmt, ...) { ARGS(fmt, v_sscanf(s, 0, NULL, fmt, ap)); }
static long darwin_vsscanf(const char *s, const char *fmt, void *l) { LIST(l, v_sscanf(s, 0, NULL, fmt, ap)); }
static long darwin_sscanf_l(const char *s, void *loc, const char *fmt, ...) { ARGS(fmt, v_sscanf(s, 1, loc, fmt, ap)); }
static long darwin_vsscanf_l(const char *s, void *loc, const char *fmt, void *l) { LIST(l, v_sscanf(s, 1, loc, fmt, ap)); }
static long darwin_fscanf(void *file, const char *fmt, ...) { ARGS(fmt, v_fscanf(file, fmt, ap)); }
static long darwin_vfscanf(void *file, const char *fmt, void *l) { LIST(l, v_fscanf(file, fmt, ap)); }
static long darwin_swprintf(wchar_t *s, size_t n, const wchar_t *fmt, ...) { ARGS(fmt, v_swprintf(s, n, fmt, ap)); }
static long darwin_vswprintf(wchar_t *s, size_t n, const wchar_t *fmt, void *l) { LIST(l, v_swprintf(s, n, fmt, ap)); }
static long darwin_swscanf(const wchar_t *s, const wchar_t *fmt, ...) { ARGS(fmt, v_swscanf(s, fmt, ap)); }

// The checked forms a build with _FORTIFY_SOURCE calls: have is the destination's size as the
// compiler knew it ((size_t)-1: it did not).
static void overflow(const char *what)
{
    fprintf(stderr, "[darwin] %s: the text is longer than its destination\n", what);
    abort();
}

static long checked(char *s, size_t n, size_t have, const char *what, const char *fmt, va_list ap)
{
    if (n > have) overflow(what);
    return v_snprintf(s, n, 0, NULL, fmt, ap);
}

static long unsized(char *s, size_t have, const char *fmt, va_list ap)
{
    if (have == (size_t)-1) return v_sprintf(s, fmt, ap);
    long r = v_snprintf(s, have, 0, NULL, fmt, ap);
    if (r >= 0 && (size_t)r >= have) overflow("sprintf");
    return r;
}

static long darwin___snprintf_chk(char *s, size_t n, int flag, size_t have, const char *fmt, ...) { ARGS(fmt, checked(s, n, have, "snprintf", fmt, ap)); }
static long darwin___vsnprintf_chk(char *s, size_t n, int flag, size_t have, const char *fmt, void *l) { LIST(l, checked(s, n, have, "vsnprintf", fmt, ap)); }
static long darwin___sprintf_chk(char *s, int flag, size_t have, const char *fmt, ...) { ARGS(fmt, unsized(s, have, fmt, ap)); }
static long darwin___vsprintf_chk(char *s, int flag, size_t have, const char *fmt, void *l) { LIST(l, unsized(s, have, fmt, ap)); }

// void perror(const char *): the image's errno, macOS's text for it.
static void darwin_perror(const char *s)
{
    char text[256];
    int e = *darwin_error();
    darwin_strerror_r(e, text, sizeof text);
    if (s && *s) fprintf(stderr, "%s: %s\n", s, text);
    else fprintf(stderr, "%s\n", text);
    darwin_set_errno(e);
}

// What the header's getc and putc macros call when their count runs out: here, every time.
static long darwin___srget(void *file) { return darwin_getc_unlocked(file); }
static long darwin___swbuf(int c, void *file) { return darwin_putc_unlocked(c, file); }

const struct darwin_sym darwin_syms_stdio[] = {
    NAMED("___stdinp", &cell_stdin), NAMED("___stdoutp", &cell_stdout), NAMED("___stderrp", &cell_stderr),
    // with _XOPEN_SOURCE and _DARWIN_C_SOURCE both defined, these two are imported under a
    // second name; on arm64 it is the same function
    NAMED("_fopen$DARWIN_EXTSN", darwin_fopen), NAMED("_fdopen$DARWIN_EXTSN", darwin_fdopen),
    SHIM(fopen), SHIM(fdopen), SHIM(freopen), SHIM(tmpfile), SHIM(fclose), SHIM(fflush), SHIM(fileno),
    SHIM(feof), SHIM(ferror), SHIM(clearerr), SHIM(flockfile), SHIM(funlockfile), SHIM(ftrylockfile),
    SHIM(setbuf), SHIM(setvbuf),
    SHIM(fread), SHIM(fwrite), SHIM(fgets), SHIM(fputs), SHIM(puts), SHIM(fputc), SHIM(fgetc), SHIM(ungetc),
    NAMED("_putc", darwin_fputc), NAMED("_getc", darwin_fgetc), SHIM(putchar), SHIM(getchar),
    SHIM(getc_unlocked), SHIM(putc_unlocked), NAMED("___srget", darwin___srget), NAMED("___swbuf", darwin___swbuf),
    SHIM(fgetwc), NAMED("_getwc", darwin_fgetwc), SHIM(fputwc), NAMED("_putwc", darwin_fputwc), SHIM(ungetwc),
    SHIM(getline), SHIM(fseek), SHIM(fseeko), NAMED("_ftell", darwin_ftello), SHIM(ftello), SHIM(rewind),
    SHIM(fgetpos), SHIM(fsetpos), SHIM(perror),
    SHIM(printf), SHIM(vprintf), SHIM(fprintf), SHIM(vfprintf), SHIM(sprintf), SHIM(vsprintf),
    SHIM(snprintf), SHIM(vsnprintf), SHIM(snprintf_l), SHIM(vsnprintf_l),
    SHIM(asprintf), SHIM(vasprintf), SHIM(asprintf_l), SHIM(vasprintf_l),
    SHIM(sscanf), SHIM(vsscanf), SHIM(sscanf_l), SHIM(vsscanf_l), SHIM(fscanf), SHIM(vfscanf),
    SHIM(swprintf), SHIM(vswprintf), SHIM(swscanf),
    NAMED("___snprintf_chk", darwin___snprintf_chk), NAMED("___vsnprintf_chk", darwin___vsnprintf_chk),
    NAMED("___sprintf_chk", darwin___sprintf_chk), NAMED("___vsprintf_chk", darwin___vsprintf_chk),
    { NULL, NULL }
};
