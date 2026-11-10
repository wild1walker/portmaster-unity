// sh_errno.c - errno as the images read it: macOS's numbers, in a cell of their own.
#define _GNU_SOURCE
#include "darwin.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define E(n) [n] = DARWIN_##n
static const unsigned char to_darwin[] = {
    E(EPERM), E(ENOENT), E(ESRCH), E(EINTR), E(EIO), E(ENXIO), E(E2BIG), E(ENOEXEC), E(EBADF), E(ECHILD),
    E(EDEADLK), E(ENOMEM), E(EACCES), E(EFAULT), E(ENOTBLK), E(EBUSY), E(EEXIST), E(EXDEV), E(ENODEV),
    E(ENOTDIR), E(EISDIR), E(EINVAL), E(ENFILE), E(EMFILE), E(ENOTTY), E(ETXTBSY), E(EFBIG), E(ENOSPC),
    E(ESPIPE), E(EROFS), E(EMLINK), E(EPIPE), E(EDOM), E(ERANGE), E(EAGAIN), E(EINPROGRESS), E(EALREADY),
    E(ENOTSOCK), E(EDESTADDRREQ), E(EMSGSIZE), E(EPROTOTYPE), E(ENOPROTOOPT), E(EPROTONOSUPPORT),
    E(ESOCKTNOSUPPORT), E(EPFNOSUPPORT), E(EAFNOSUPPORT), E(EADDRINUSE), E(EADDRNOTAVAIL), E(ENETDOWN),
    E(ENETUNREACH), E(ENETRESET), E(ECONNABORTED), E(ECONNRESET), E(ENOBUFS), E(EISCONN), E(ENOTCONN),
    E(ESHUTDOWN), E(ETOOMANYREFS), E(ETIMEDOUT), E(ECONNREFUSED), E(ELOOP), E(ENAMETOOLONG), E(EHOSTDOWN),
    E(EHOSTUNREACH), E(ENOTEMPTY), E(EUSERS), E(EDQUOT), E(ESTALE), E(EREMOTE), E(ENOLCK), E(ENOSYS),
    E(EOVERFLOW), E(ECANCELED), E(EIDRM), E(ENOMSG), E(EILSEQ), E(EBADMSG), E(EMULTIHOP), E(ENODATA),
    E(ENOLINK), E(ENOSR), E(ENOSTR), E(EPROTO), E(ETIME), E(ENOTRECOVERABLE), E(EOWNERDEAD),
    // Linux has one number for both; a file call's meaning here, a socket call's in sh_net.c
    [ENOTSUP] = DARWIN_ENOTSUP,
};
#undef E

int darwin_errno_of(int host)
{
    if (host == 0) return 0;
    if (host > 0 && (size_t)host < sizeof to_darwin && to_darwin[host]) return to_darwin[host];
    static _Atomic int said;
    if (!said) { said = 1; darwin_log("errno %d (%s) has no macOS number: EIO is given", host, strerror(host)); }
    return DARWIN_EIO;
}

int darwin_errno_host(int darwin)
{
    if (darwin <= 0) return 0;
    if (darwin == DARWIN_EOPNOTSUPP) return EOPNOTSUPP;
    for (size_t i = 1; i < sizeof to_darwin; i++) if (to_darwin[i] == darwin) return (int)i;
    return 0;
}

static __thread int cell;

int *darwin_error(void)
{
    int e = errno;
    if (e) { cell = darwin_errno_of(e); errno = 0; }
    return &cell;
}

void darwin_set_errno(int darwin)
{
    errno = 0;
    cell = darwin;
}

// macOS's texts, as its strerror gives them (the same error reads differently on Linux)
static const char *const texts[] = DARWIN_ERRTEXTS;

// int strerror_r(int, char *, size_t): 0, EINVAL for a number that is none (the text says so),
// ERANGE when the text was cut.
int darwin_strerror_r(int e, char *buf, size_t len)
{
    char text[256];
    int r = 0;
    if (e >= 0 && (size_t)e < sizeof texts / sizeof texts[0]) snprintf(text, sizeof text, "%s", texts[e]);
    else { snprintf(text, sizeof text, "Unknown error: %d", e); r = DARWIN_EINVAL; }
    if (len) snprintf(buf, len, "%s", text);
    if (strlen(text) >= len) return DARWIN_ERANGE;
    return r;
}

// char *strerror(int): the text stays the thread's until its next call.
static char *darwin_strerror(int e)
{
    static __thread char text[256];
    darwin_strerror_r(e, text, sizeof text);
    return text;
}

const struct darwin_sym darwin_syms_errno[] = {
    NAMED("___error", darwin_error),
    SHIM(strerror_r), SHIM(strerror),
    { NULL, NULL }
};
