// sh_net.c - sockets and name lookups: macOS's address layout (a length byte, then the family
// in one byte), its option and flag numbers, struct msghdr, addrinfo and ifaddrs.
#define _GNU_SOURCE
#include "darwin.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define PUT(p, off, v) memcpy((unsigned char *)(p) + (off), &(v), sizeof(v))
#define GET(p, off, v) memcpy(&(v), (const unsigned char *)(p) + (off), sizeof(v))

// A call a thread_suspend's signal broke off is made again (darwin_thread_stops): a socket
// with a timeout (SO_RCVTIMEO, SO_SNDTIMEO) is not restarted by the kernel. The timeout is one
// from the call's start still: the socket is waited for (poll) for what is left of it before
// the call is made again, and the call ends with EAGAIN when it ran out, as on its own. (A
// call made again counts its timeout anew in the kernel: only when another thread took what
// the poll saw does it wait past the time, and a stop in that wait brings it back here.)
static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

// which: SO_RCVTIMEO or SO_SNDTIMEO, the timeout of the call that started at t0.
// 1: make the call again; 0: it is over, with errno
static int stopped_wait(int fd, int which, int64_t t0)
{
    struct timeval tv = { 0, 0 };
    socklen_t n = sizeof tv;
    if (getsockopt(fd, SOL_SOCKET, which, &tv, &n) != 0 || (tv.tv_sec == 0 && tv.tv_usec == 0)) return 1;
    int64_t end = t0 + (int64_t)tv.tv_sec * 1000000000 + (int64_t)tv.tv_usec * 1000;
    for (;;) {
        int64_t left = end - mono_ns();
        if (left <= 0) { errno = EAGAIN; return 0; }
        left = (left + 999999) / 1000000;
        struct pollfd p = { fd, which == SO_RCVTIMEO ? POLLIN : POLLOUT, 0 };
        unsigned stops = darwin_thread_stops();
        int r = poll(&p, 1, left > INT32_MAX ? INT32_MAX : (int)left);
        if (r > 0) return 1;
        if (r == 0) { errno = EAGAIN; return 0; }
        if (errno != EINTR) return 1;                           // the call says what is wrong
        if (stops == darwin_thread_stops()) return 0;           // a signal of the image's own: EINTR
    }
}

#define AGAIN(r, fd, which, call) do { \
        int64_t ua_t0_ = mono_ns(); \
        unsigned ua_stops_; \
        do { ua_stops_ = darwin_thread_stops(); (r) = (call); } \
        while ((r) < 0 && errno == EINTR && ua_stops_ != darwin_thread_stops() && stopped_wait(fd, which, ua_t0_)); \
    } while (0)

// ---- addresses ---------------------------------------------------------------------------------
// After the first two bytes sockaddr_in, sockaddr_in6 and sockaddr_un are laid out alike.

static int family_host(int d)
{
    switch (d) {
    case DARWIN_AF_UNSPEC: return AF_UNSPEC;
    case DARWIN_AF_UNIX:   return AF_UNIX;
    case DARWIN_AF_INET:   return AF_INET;
    case DARWIN_AF_INET6:  return AF_INET6;
    }
    return -1;
}

static int family_darwin(int h)
{
    switch (h) {
    case AF_UNSPEC: return DARWIN_AF_UNSPEC;
    case AF_UNIX:   return DARWIN_AF_UNIX;
    case AF_INET:   return DARWIN_AF_INET;
    case AF_INET6:  return DARWIN_AF_INET6;
    }
    return -1;
}

// An address of the image's as the kernel takes it. NULL: none given. -1: not one known here.
static int sa_host(const unsigned char *d, uint32_t dlen, struct sockaddr_storage *h, socklen_t *hlen)
{
    if (!d) { *hlen = 0; return 0; }
    if (dlen < 2 || dlen > sizeof *h) { errno = EINVAL; return -1; }
    int family = family_host(d[DARWIN_SOCKADDR_sa_family]);
    if (family < 0) { errno = EAFNOSUPPORT; return -1; }
    memset(h, 0, sizeof *h);
    memcpy(h, d, dlen);
    h->ss_family = (sa_family_t)family;
    *hlen = dlen;
    return 0;
}

// The length of a host address as macOS counts it
static uint32_t sa_darwin_len(const struct sockaddr *h, socklen_t hlen)
{
    switch (h->sa_family) {
    case AF_INET:  return DARWIN_SIZEOF_SOCKADDR_IN;
    case AF_INET6: return DARWIN_SIZEOF_SOCKADDR_IN6;
    case AF_UNIX: {
        size_t path = hlen > 2 ? strnlen(((const struct sockaddr_un *)h)->sun_path, hlen - 2) : 0;
        size_t n = 2 + path + 1;
        return n > DARWIN_SIZEOF_SOCKADDR_UN ? DARWIN_SIZEOF_SOCKADDR_UN : (uint32_t)n;
    }
    }
    return hlen < 2 ? 2 : hlen;
}

// A host address into the image's buffer of *dlen bytes (cut to it, as the kernel cuts); *dlen
// becomes the whole length.
static void sa_darwin(const struct sockaddr *h, socklen_t hlen, unsigned char *d, uint32_t *dlen)
{
    if (!d || !dlen) return;
    if (hlen < 2) { *dlen = 0; return; }
    unsigned char tmp[sizeof(struct sockaddr_storage)] = { 0 };
    uint32_t n = sa_darwin_len(h, hlen);
    int family = family_darwin(h->sa_family);
    memcpy(tmp, h, hlen < sizeof tmp ? hlen : sizeof tmp);
    tmp[DARWIN_SOCKADDR_sa_len] = (unsigned char)n;
    tmp[DARWIN_SOCKADDR_sa_family] = (unsigned char)(family < 0 ? 0 : family);
    memcpy(d, tmp, n < *dlen ? n : *dlen);
    *dlen = n;
}

// ---- SO_NOSIGPIPE ------------------------------------------------------------------------------
// macOS marks the socket, Linux each send (MSG_NOSIGNAL): the mark is kept here by descriptor
// and cleared when socket or accept hand a descriptor out. A write() to a marked socket still
// raises SIGPIPE here.
#define MARKS 65536
static _Atomic unsigned char nosigpipe[MARKS];

static void mark_set(int fd, int on) { if (fd >= 0 && fd < MARKS) atomic_store(&nosigpipe[fd], (unsigned char)(on != 0)); }
static int marked(int fd) { return fd >= 0 && fd < MARKS && atomic_load(&nosigpipe[fd]); }

// ---- flags -------------------------------------------------------------------------------------

static const struct { int d, h; } msg_flags[] = {
    { DARWIN_MSG_OOB, MSG_OOB }, { DARWIN_MSG_PEEK, MSG_PEEK }, { DARWIN_MSG_DONTROUTE, MSG_DONTROUTE },
    { DARWIN_MSG_EOR, MSG_EOR }, { DARWIN_MSG_TRUNC, MSG_TRUNC }, { DARWIN_MSG_CTRUNC, MSG_CTRUNC },
    { DARWIN_MSG_WAITALL, MSG_WAITALL }, { DARWIN_MSG_DONTWAIT, MSG_DONTWAIT }, { DARWIN_MSG_NOSIGNAL, MSG_NOSIGNAL },
};
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

// -1: a flag not known here
static int flags_host(int d)
{
    int h = 0;
    for (size_t i = 0; i < COUNT_OF(msg_flags); i++)
        if (d & msg_flags[i].d) { h |= msg_flags[i].h; d &= ~msg_flags[i].d; }
    return d ? -1 : h;
}

static int flags_darwin(int h)
{
    int d = 0;
    for (size_t i = 0; i < COUNT_OF(msg_flags); i++)
        if (h & msg_flags[i].h) d |= msg_flags[i].d;
    return d;
}

static int bad_flags(const char *call)
{
    darwin_unanswered(call, "a message flag that is not known here");
    errno = EINVAL;
    return -1;
}

// ---- the calls ---------------------------------------------------------------------------------

static long darwin_socket(int domain, int type, int protocol)
{
    int family = family_host(domain);
    if (family < 0) { errno = EAFNOSUPPORT; return -1; }
    int fd = socket(family, type, protocol);       // the types and protocols have the same numbers
    mark_set(fd, 0);
    return fd;
}

static long darwin_socketpair(int domain, int type, int protocol, int *fds)
{
    int family = family_host(domain);
    if (family < 0) { errno = EAFNOSUPPORT; return -1; }
    if (socketpair(family, type, protocol, fds) != 0) return -1;
    mark_set(fds[0], 0);
    mark_set(fds[1], 0);
    return 0;
}

static long darwin_bind(int fd, const unsigned char *addr, uint32_t len)
{
    struct sockaddr_storage h;
    socklen_t hlen;
    if (!addr) { errno = EFAULT; return -1; }
    if (sa_host(addr, len, &h, &hlen) != 0) return -1;
    return bind(fd, (struct sockaddr *)&h, hlen);
}

// A connect broken off by a stop goes on in the kernel: its end is waited for here.
static long darwin_connect(int fd, const unsigned char *addr, uint32_t len)
{
    struct sockaddr_storage h;
    socklen_t hlen;
    if (!addr) { errno = EFAULT; return -1; }
    if (sa_host(addr, len, &h, &hlen) != 0) return -1;
    unsigned stops = darwin_thread_stops();
    int r = connect(fd, (struct sockaddr *)&h, hlen);
    while (r < 0 && errno == EINTR && stops != darwin_thread_stops()) {
        struct pollfd p = { fd, POLLOUT, 0 };
        stops = darwin_thread_stops();
        if (poll(&p, 1, -1) < 0) continue;          // EINTR again: by a stop, or the image's own signal
        int err = 0;
        socklen_t n = sizeof err;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &n) != 0) return -1;
        if (err) { errno = err; return -1; }
        return 0;
    }
    return r;
}

static long darwin_accept(int fd, unsigned char *addr, uint32_t *len)
{
    struct sockaddr_storage h;
    socklen_t hlen = sizeof h;
    int r;
    AGAIN(r, fd, SO_RCVTIMEO, accept(fd, addr ? (struct sockaddr *)&h : NULL, addr ? &hlen : NULL));
    if (r < 0) return -1;
    mark_set(r, 0);
    if (addr) sa_darwin((struct sockaddr *)&h, hlen, addr, len);
    return r;
}

static long darwin_getsockname(int fd, unsigned char *addr, uint32_t *len)
{
    struct sockaddr_storage h;
    socklen_t hlen = sizeof h;
    if (getsockname(fd, (struct sockaddr *)&h, &hlen) != 0) return -1;
    sa_darwin((struct sockaddr *)&h, hlen, addr, len);
    return 0;
}

static long darwin_getpeername(int fd, unsigned char *addr, uint32_t *len)
{
    struct sockaddr_storage h;
    socklen_t hlen = sizeof h;
    if (getpeername(fd, (struct sockaddr *)&h, &hlen) != 0) return -1;
    sa_darwin((struct sockaddr *)&h, hlen, addr, len);
    return 0;
}

static ssize_t darwin_send(int fd, const void *buf, size_t n, int flags)
{
    int h = flags_host(flags);
    if (h < 0) return bad_flags("send");
    if (marked(fd)) h |= MSG_NOSIGNAL;
    ssize_t r;
    AGAIN(r, fd, SO_SNDTIMEO, send(fd, buf, n, h));
    return r;
}

static ssize_t darwin_recvfrom(int fd, void *buf, size_t n, int flags, unsigned char *addr, uint32_t *len)
{
    int hf = flags_host(flags);
    if (hf < 0) return bad_flags("recvfrom");
    struct sockaddr_storage h;
    socklen_t hlen = sizeof h;
    ssize_t r;
    AGAIN(r, fd, SO_RCVTIMEO, recvfrom(fd, buf, n, hf, addr ? (struct sockaddr *)&h : NULL, addr ? (hlen = sizeof h, &hlen) : NULL));
    if (r >= 0 && addr) sa_darwin((struct sockaddr *)&h, hlen, addr, len);
    return r;
}

static ssize_t darwin_recv(int fd, void *buf, size_t n, int flags)
{
    return darwin_recvfrom(fd, buf, n, flags, NULL, NULL);
}

// ---- sendmsg, recvmsg --------------------------------------------------------------------------
// struct msghdr: the lengths are 32 bits there. A control message's header is 12 bytes there
// (a 32-bit length, data aligned to 4) and 16 here; the kinds carried across are descriptors
// (SCM_RIGHTS) and IP_PKTINFO, IP_RECVTTL, IP_RECVTOS, whose data is laid out alike.

#define D_ALIGN(n)      (((n) + 3u) & ~3u)
#define D_CMSG_HDR      D_ALIGN(DARWIN_SIZEOF_CMSGHDR)

struct dmsg { void *name; uint32_t namelen; struct iovec *iov; int iovlen; unsigned char *control; uint32_t controllen; int flags; };

static void dmsg_read(const unsigned char *m, struct dmsg *d)
{
    GET(m, DARWIN_MSGHDR_msg_name, d->name);
    GET(m, DARWIN_MSGHDR_msg_namelen, d->namelen);
    GET(m, DARWIN_MSGHDR_msg_iov, d->iov);
    GET(m, DARWIN_MSGHDR_msg_iovlen, d->iovlen);
    GET(m, DARWIN_MSGHDR_msg_control, d->control);
    GET(m, DARWIN_MSGHDR_msg_controllen, d->controllen);
    GET(m, DARWIN_MSGHDR_msg_flags, d->flags);
}

// A control message's kind across; 0: not one carried
static int cmsg_host(int dlevel, int dtype, int *level, int *type)
{
    if (dlevel == DARWIN_SOL_SOCKET && dtype == DARWIN_SCM_RIGHTS) { *level = SOL_SOCKET; *type = SCM_RIGHTS; return 1; }
    if (dlevel == DARWIN_IPPROTO_IP && dtype == DARWIN_IP_PKTINFO) { *level = IPPROTO_IP; *type = IP_PKTINFO; return 1; }
    return 0;
}

static int cmsg_darwin(int level, int type, int *dlevel, int *dtype)
{
    if (level == SOL_SOCKET && type == SCM_RIGHTS) { *dlevel = DARWIN_SOL_SOCKET; *dtype = DARWIN_SCM_RIGHTS; return 1; }
    if (level == IPPROTO_IP && type == IP_PKTINFO) { *dlevel = DARWIN_IPPROTO_IP; *dtype = DARWIN_IP_PKTINFO; return 1; }
    if (level == IPPROTO_IP && type == IP_TTL) { *dlevel = DARWIN_IPPROTO_IP; *dtype = DARWIN_IP_RECVTTL; return 1; }
    if (level == IPPROTO_IP && type == IP_TOS) { *dlevel = DARWIN_IPPROTO_IP; *dtype = DARWIN_IP_RECVTOS; return 1; }
    return 0;
}

static ssize_t darwin_sendmsg(int fd, const unsigned char *msg, int flags)
{
    struct dmsg d;
    struct msghdr h = { 0 };
    struct sockaddr_storage name;
    socklen_t namelen = 0;
    int hf = flags_host(flags);
    if (hf < 0) return bad_flags("sendmsg");
    if (!msg) { errno = EFAULT; return -1; }
    dmsg_read(msg, &d);
    if (sa_host(d.name, d.namelen, &name, &namelen) != 0) return -1;
    h.msg_name = d.name ? &name : NULL;
    h.msg_namelen = namelen;
    h.msg_iov = d.iov;
    h.msg_iovlen = (size_t)d.iovlen;

    unsigned char *control = NULL;
    if (d.control && d.controllen >= DARWIN_SIZEOF_CMSGHDR) {
        control = calloc(1, (size_t)d.controllen * 2 + CMSG_SPACE(0));
        if (!control) { errno = ENOMEM; return -1; }
        size_t used = 0;
        for (uint32_t at = 0; at + DARWIN_SIZEOF_CMSGHDR <= d.controllen;) {
            uint32_t len;
            int dlevel, dtype, level, type;
            GET(d.control, at, len);
            GET(d.control, at + DARWIN_CMSGHDR_cmsg_level, dlevel);
            GET(d.control, at + DARWIN_CMSGHDR_cmsg_type, dtype);
            if (len < D_CMSG_HDR || len > d.controllen - at) { free(control); errno = EINVAL; return -1; }
            if (!cmsg_host(dlevel, dtype, &level, &type)) {
                free(control);
                darwin_unanswered("sendmsg", "a control message of a kind not carried across");
                errno = EINVAL;
                return -1;
            }
            size_t data = len - D_CMSG_HDR;
            struct cmsghdr *c = (struct cmsghdr *)(control + used);
            c->cmsg_len = CMSG_LEN(data);
            c->cmsg_level = level;
            c->cmsg_type = type;
            memcpy(CMSG_DATA(c), d.control + at + D_CMSG_HDR, data);
            used += CMSG_SPACE(data);
            at += D_ALIGN(len);
        }
        h.msg_control = control;
        h.msg_controllen = used;
    }
    if (marked(fd)) hf |= MSG_NOSIGNAL;
    ssize_t r;
    AGAIN(r, fd, SO_SNDTIMEO, sendmsg(fd, &h, hf));
    int e = errno;
    free(control);
    errno = e;
    return r;
}

static ssize_t darwin_recvmsg(int fd, unsigned char *msg, int flags)
{
    struct dmsg d;
    struct msghdr h = { 0 };
    struct sockaddr_storage name;
    int hf = flags_host(flags);
    if (hf < 0) return bad_flags("recvmsg");
    if (!msg) { errno = EFAULT; return -1; }
    dmsg_read(msg, &d);
    h.msg_name = d.name ? &name : NULL;
    h.msg_namelen = d.name ? sizeof name : 0;
    h.msg_iov = d.iov;
    h.msg_iovlen = (size_t)d.iovlen;
    unsigned char *control = NULL;
    if (d.control && d.controllen) {
        h.msg_controllen = (size_t)d.controllen * 2 + CMSG_SPACE(0);
        control = h.msg_control = calloc(1, h.msg_controllen);
        if (!control) { errno = ENOMEM; return -1; }
    }
    ssize_t r;
    AGAIN(r, fd, SO_RCVTIMEO, recvmsg(fd, &h, hf));
    if (r < 0) { int e = errno; free(control); errno = e; return -1; }

    int out_flags = flags_darwin(h.msg_flags);
    uint32_t namelen = d.namelen, used = 0;
    if (d.name) sa_darwin((struct sockaddr *)&name, h.msg_namelen, d.name, &namelen);
    for (struct cmsghdr *c = control ? CMSG_FIRSTHDR(&h) : NULL; c; c = CMSG_NXTHDR(&h, c)) {
        int dlevel, dtype;
        if (!cmsg_darwin(c->cmsg_level, c->cmsg_type, &dlevel, &dtype)) {
            darwin_unanswered("recvmsg", "a control message of a kind not carried across was dropped");
            continue;
        }
        size_t data = c->cmsg_len - CMSG_LEN(0);
        uint32_t len = (uint32_t)(D_CMSG_HDR + data);
        if (used + len > d.controllen) { out_flags |= DARWIN_MSG_CTRUNC; break; }
        PUT(d.control, used, len);
        PUT(d.control, used + DARWIN_CMSGHDR_cmsg_level, dlevel);
        PUT(d.control, used + DARWIN_CMSGHDR_cmsg_type, dtype);
        memcpy(d.control + used + D_CMSG_HDR, CMSG_DATA(c), data);
        used += D_ALIGN(len) <= d.controllen - used ? D_ALIGN(len) : len;
    }
    free(control);
    if (d.name) PUT(msg, DARWIN_MSGHDR_msg_namelen, namelen);
    PUT(msg, DARWIN_MSGHDR_msg_controllen, used);
    PUT(msg, DARWIN_MSGHDR_msg_flags, out_flags);
    return r;
}

// ---- socket options ----------------------------------------------------------------------------

enum { V_SAME, V_TIMEVAL, V_ERRNO, V_MARK, V_NREAD, V_DONTFRAG, V_MREQ_SOURCE };

static const struct opt { int dlevel, dname, level, name, kind; } opts[] = {
    { DARWIN_SOL_SOCKET, DARWIN_SO_DEBUG, SOL_SOCKET, SO_DEBUG, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_ACCEPTCONN, SOL_SOCKET, SO_ACCEPTCONN, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_REUSEADDR, SOL_SOCKET, SO_REUSEADDR, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_KEEPALIVE, SOL_SOCKET, SO_KEEPALIVE, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_DONTROUTE, SOL_SOCKET, SO_DONTROUTE, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_BROADCAST, SOL_SOCKET, SO_BROADCAST, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_LINGER, SOL_SOCKET, SO_LINGER, V_SAME },         // struct linger: two ints on both
    { DARWIN_SOL_SOCKET, DARWIN_SO_LINGER_SEC, SOL_SOCKET, SO_LINGER, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_OOBINLINE, SOL_SOCKET, SO_OOBINLINE, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_REUSEPORT, SOL_SOCKET, SO_REUSEPORT, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_TIMESTAMP, SOL_SOCKET, SO_TIMESTAMP, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_SNDBUF, SOL_SOCKET, SO_SNDBUF, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_RCVBUF, SOL_SOCKET, SO_RCVBUF, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_SNDLOWAT, SOL_SOCKET, SO_SNDLOWAT, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_RCVLOWAT, SOL_SOCKET, SO_RCVLOWAT, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_SNDTIMEO, SOL_SOCKET, SO_SNDTIMEO, V_TIMEVAL },
    { DARWIN_SOL_SOCKET, DARWIN_SO_RCVTIMEO, SOL_SOCKET, SO_RCVTIMEO, V_TIMEVAL },
    { DARWIN_SOL_SOCKET, DARWIN_SO_ERROR, SOL_SOCKET, SO_ERROR, V_ERRNO },
    { DARWIN_SOL_SOCKET, DARWIN_SO_TYPE, SOL_SOCKET, SO_TYPE, V_SAME },
    { DARWIN_SOL_SOCKET, DARWIN_SO_NOSIGPIPE, 0, 0, V_MARK },
    { DARWIN_SOL_SOCKET, DARWIN_SO_NREAD, 0, 0, V_NREAD },
    { DARWIN_IPPROTO_IP, DARWIN_IP_OPTIONS, IPPROTO_IP, IP_OPTIONS, V_SAME },
    { DARWIN_IPPROTO_IP, DARWIN_IP_HDRINCL, IPPROTO_IP, IP_HDRINCL, V_SAME },
    { DARWIN_IPPROTO_IP, DARWIN_IP_TOS, IPPROTO_IP, IP_TOS, V_SAME },
    { DARWIN_IPPROTO_IP, DARWIN_IP_TTL, IPPROTO_IP, IP_TTL, V_SAME },
    { DARWIN_IPPROTO_IP, DARWIN_IP_MULTICAST_IF, IPPROTO_IP, IP_MULTICAST_IF, V_SAME },
    { DARWIN_IPPROTO_IP, DARWIN_IP_MULTICAST_TTL, IPPROTO_IP, IP_MULTICAST_TTL, V_SAME },
    { DARWIN_IPPROTO_IP, DARWIN_IP_MULTICAST_LOOP, IPPROTO_IP, IP_MULTICAST_LOOP, V_SAME },
    { DARWIN_IPPROTO_IP, DARWIN_IP_ADD_MEMBERSHIP, IPPROTO_IP, IP_ADD_MEMBERSHIP, V_SAME },     // struct ip_mreq
    { DARWIN_IPPROTO_IP, DARWIN_IP_DROP_MEMBERSHIP, IPPROTO_IP, IP_DROP_MEMBERSHIP, V_SAME },
    { DARWIN_IPPROTO_IP, DARWIN_IP_ADD_SOURCE_MEMBERSHIP, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, V_MREQ_SOURCE },
    { DARWIN_IPPROTO_IP, DARWIN_IP_DROP_SOURCE_MEMBERSHIP, IPPROTO_IP, IP_DROP_SOURCE_MEMBERSHIP, V_MREQ_SOURCE },
    { DARWIN_IPPROTO_IP, DARWIN_IP_BLOCK_SOURCE, IPPROTO_IP, IP_BLOCK_SOURCE, V_MREQ_SOURCE },
    { DARWIN_IPPROTO_IP, DARWIN_IP_UNBLOCK_SOURCE, IPPROTO_IP, IP_UNBLOCK_SOURCE, V_MREQ_SOURCE },
    { DARWIN_IPPROTO_IP, DARWIN_IP_PKTINFO, IPPROTO_IP, IP_PKTINFO, V_SAME },
    { DARWIN_IPPROTO_IP, DARWIN_IP_RECVTOS, IPPROTO_IP, IP_RECVTOS, V_SAME },
    { DARWIN_IPPROTO_IP, DARWIN_IP_RECVTTL, IPPROTO_IP, IP_RECVTTL, V_SAME },
    { DARWIN_IPPROTO_IP, DARWIN_IP_DONTFRAG, IPPROTO_IP, IP_MTU_DISCOVER, V_DONTFRAG },
    { DARWIN_IPPROTO_IPV6, DARWIN_IPV6_UNICAST_HOPS, IPPROTO_IPV6, IPV6_UNICAST_HOPS, V_SAME },
    { DARWIN_IPPROTO_IPV6, DARWIN_IPV6_MULTICAST_IF, IPPROTO_IPV6, IPV6_MULTICAST_IF, V_SAME },
    { DARWIN_IPPROTO_IPV6, DARWIN_IPV6_MULTICAST_HOPS, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, V_SAME },
    { DARWIN_IPPROTO_IPV6, DARWIN_IPV6_MULTICAST_LOOP, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, V_SAME },
    { DARWIN_IPPROTO_IPV6, DARWIN_IPV6_JOIN_GROUP, IPPROTO_IPV6, IPV6_JOIN_GROUP, V_SAME },     // struct ipv6_mreq
    { DARWIN_IPPROTO_IPV6, DARWIN_IPV6_LEAVE_GROUP, IPPROTO_IPV6, IPV6_LEAVE_GROUP, V_SAME },
    { DARWIN_IPPROTO_IPV6, DARWIN_IPV6_V6ONLY, IPPROTO_IPV6, IPV6_V6ONLY, V_SAME },
    { DARWIN_IPPROTO_IPV6, DARWIN_IPV6_TCLASS, IPPROTO_IPV6, IPV6_TCLASS, V_SAME },
    { DARWIN_IPPROTO_TCP, DARWIN_TCP_NODELAY, IPPROTO_TCP, TCP_NODELAY, V_SAME },
    { DARWIN_IPPROTO_TCP, DARWIN_TCP_MAXSEG, IPPROTO_TCP, TCP_MAXSEG, V_SAME },
    { DARWIN_IPPROTO_TCP, DARWIN_TCP_KEEPALIVE, IPPROTO_TCP, TCP_KEEPIDLE, V_SAME },
    { DARWIN_IPPROTO_TCP, DARWIN_TCP_KEEPINTVL, IPPROTO_TCP, TCP_KEEPINTVL, V_SAME },
    { DARWIN_IPPROTO_TCP, DARWIN_TCP_KEEPCNT, IPPROTO_TCP, TCP_KEEPCNT, V_SAME },
    { DARWIN_IPPROTO_TCP, DARWIN_TCP_NOPUSH, IPPROTO_TCP, TCP_CORK, V_SAME },
};
_Static_assert(DARWIN_SIZEOF_LINGER == sizeof(struct linger) && DARWIN_SIZEOF_IP_MREQ == sizeof(struct ip_mreq) &&
               DARWIN_SIZEOF_IPV6_MREQ == sizeof(struct ipv6_mreq) && DARWIN_SIZEOF_IP_MREQ_SOURCE == sizeof(struct ip_mreq_source) &&
               DARWIN_SIZEOF_IN_PKTINFO == sizeof(struct in_pktinfo), "socket option values");

static const struct opt *opt_of(const char *call, int level, int name)
{
    for (size_t i = 0; i < COUNT_OF(opts); i++)
        if (opts[i].dlevel == level && opts[i].dname == name) return &opts[i];
    darwin_unanswered(call, "a socket option that is not known here");
    errno = ENOPROTOOPT;
    return NULL;
}

static long darwin_setsockopt(int fd, int level, int name, const unsigned char *value, uint32_t len)
{
    const struct opt *o = opt_of("setsockopt", level, name);
    if (!o) return -1;
    if (!value && o->kind != V_SAME) { errno = EFAULT; return -1; }
    switch (o->kind) {
    case V_TIMEVAL: {
        if (len < DARWIN_SIZEOF_TIMEVAL) { errno = EINVAL; return -1; }
        int64_t sec; int32_t usec;
        GET(value, 0, sec);
        GET(value, DARWIN_TIMEVAL_tv_usec, usec);
        struct timeval t = { sec, usec };
        return setsockopt(fd, o->level, o->name, &t, sizeof t);
    }
    case V_MARK: {
        int on;
        if (len < sizeof on) { errno = EINVAL; return -1; }
        GET(value, 0, on);
        mark_set(fd, on);
        return 0;
    }
    case V_DONTFRAG: {
        int on, how;
        if (len < sizeof on) { errno = EINVAL; return -1; }
        GET(value, 0, on);
        how = on ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
        return setsockopt(fd, o->level, o->name, &how, sizeof how);
    }
    case V_MREQ_SOURCE: {                   // the source and the interface sit in the other order
        struct ip_mreq_source m;
        if (len < DARWIN_SIZEOF_IP_MREQ_SOURCE) { errno = EINVAL; return -1; }
        GET(value, DARWIN_IP_MREQ_SOURCE_imr_multiaddr, m.imr_multiaddr);
        GET(value, DARWIN_IP_MREQ_SOURCE_imr_sourceaddr, m.imr_sourceaddr);
        GET(value, DARWIN_IP_MREQ_SOURCE_imr_interface, m.imr_interface);
        return setsockopt(fd, o->level, o->name, &m, sizeof m);
    }
    case V_ERRNO: case V_NREAD:
        errno = ENOPROTOOPT;
        return -1;
    }
    return setsockopt(fd, o->level, o->name, value, len);
}

static long put_int(unsigned char *value, uint32_t *len, int v)
{
    uint32_t n = *len < sizeof v ? *len : (uint32_t)sizeof v;
    memcpy(value, &v, n);
    *len = n;
    return 0;
}

static long darwin_getsockopt(int fd, int level, int name, unsigned char *value, uint32_t *len)
{
    const struct opt *o = opt_of("getsockopt", level, name);
    if (!o) return -1;
    if (!value || !len) { errno = EFAULT; return -1; }
    switch (o->kind) {
    case V_TIMEVAL: {
        struct timeval t;
        socklen_t n = sizeof t;
        if (*len < DARWIN_SIZEOF_TIMEVAL) { errno = EINVAL; return -1; }
        if (getsockopt(fd, o->level, o->name, &t, &n) != 0) return -1;
        int64_t sec = t.tv_sec; int32_t usec = (int32_t)t.tv_usec;
        memset(value, 0, DARWIN_SIZEOF_TIMEVAL);
        PUT(value, 0, sec);
        PUT(value, DARWIN_TIMEVAL_tv_usec, usec);
        *len = DARWIN_SIZEOF_TIMEVAL;
        return 0;
    }
    case V_ERRNO: {
        int err = 0;
        socklen_t n = sizeof err;
        if (getsockopt(fd, o->level, o->name, &err, &n) != 0) return -1;
        return put_int(value, len, err ? darwin_errno_of(err) : 0);
    }
    case V_MARK: return put_int(value, len, marked(fd));
    case V_NREAD: {
        int waiting = 0;
        if (ioctl(fd, FIONREAD, &waiting) != 0) return -1;
        return put_int(value, len, waiting);
    }
    case V_DONTFRAG: {
        int how = 0;
        socklen_t n = sizeof how;
        if (getsockopt(fd, o->level, o->name, &how, &n) != 0) return -1;
        return put_int(value, len, how == IP_PMTUDISC_DO || how == IP_PMTUDISC_PROBE);
    }
    case V_MREQ_SOURCE:
        errno = ENOPROTOOPT;
        return -1;
    }
    socklen_t n = *len;
    if (getsockopt(fd, o->level, o->name, value, &n) != 0) return -1;
    *len = n;
    return 0;
}

// ---- names -------------------------------------------------------------------------------------

static const struct { int d, h; } ai_flags[] = {
    { DARWIN_AI_PASSIVE, AI_PASSIVE }, { DARWIN_AI_CANONNAME, AI_CANONNAME }, { DARWIN_AI_NUMERICHOST, AI_NUMERICHOST },
    { DARWIN_AI_NUMERICSERV, AI_NUMERICSERV }, { DARWIN_AI_ALL, AI_ALL }, { DARWIN_AI_ADDRCONFIG, AI_ADDRCONFIG },
    { DARWIN_AI_V4MAPPED, AI_V4MAPPED }, { DARWIN_AI_V4MAPPED_CFG, AI_V4MAPPED },
};

static long eai_darwin(int h)
{
    switch (h) {
    case 0:              return 0;
    case EAI_BADFLAGS:   return DARWIN_EAI_BADFLAGS;
    case EAI_NONAME:     return DARWIN_EAI_NONAME;
    case EAI_AGAIN:      return DARWIN_EAI_AGAIN;
    case EAI_FAIL:       return DARWIN_EAI_FAIL;
    case EAI_NODATA:     return DARWIN_EAI_NODATA;
    case EAI_FAMILY:     return DARWIN_EAI_FAMILY;
    case EAI_SOCKTYPE:   return DARWIN_EAI_SOCKTYPE;
    case EAI_SERVICE:    return DARWIN_EAI_SERVICE;
    case EAI_ADDRFAMILY: return DARWIN_EAI_ADDRFAMILY;
    case EAI_MEMORY:     return DARWIN_EAI_MEMORY;
    case EAI_SYSTEM:     return DARWIN_EAI_SYSTEM;   // errno says which, and the image's errno is folded from it
    case EAI_OVERFLOW:   return DARWIN_EAI_OVERFLOW;
    }
    return DARWIN_EAI_FAIL;
}

// void freeaddrinfo(struct addrinfo *): each entry is one block made by getaddrinfo below
static void darwin_freeaddrinfo(unsigned char *ai)
{
    while (ai) {
        unsigned char *next;
        GET(ai, DARWIN_ADDRINFO_ai_next, next);
        free(ai);
        ai = next;
    }
}

static long darwin_getaddrinfo(const char *node, const char *service, const unsigned char *hints, unsigned char **res)
{
    struct addrinfo h = { 0 }, *list = NULL;
    if (hints) {
        int flags, family;
        GET(hints, DARWIN_ADDRINFO_ai_flags, flags);
        GET(hints, DARWIN_ADDRINFO_ai_family, family);
        GET(hints, DARWIN_ADDRINFO_ai_socktype, h.ai_socktype);
        GET(hints, DARWIN_ADDRINFO_ai_protocol, h.ai_protocol);
        for (size_t i = 0; i < COUNT_OF(ai_flags); i++)
            if (flags & ai_flags[i].d) { h.ai_flags |= ai_flags[i].h; flags &= ~ai_flags[i].d; }
        if (flags) return DARWIN_EAI_BADFLAGS;
        h.ai_family = family_host(family);
        if (h.ai_family < 0) return DARWIN_EAI_FAMILY;
    }
    int r = getaddrinfo(node, service, hints ? &h : NULL, &list);
    if (r != 0) return eai_darwin(r);

    unsigned char *first = NULL, *last = NULL;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        int family = family_darwin(a->ai_family);
        if (family < 0 || !a->ai_addr) continue;
        size_t name = a->ai_canonname ? strlen(a->ai_canonname) + 1 : 0;
        unsigned char *d = calloc(1, DARWIN_SIZEOF_ADDRINFO + DARWIN_SIZEOF_SOCKADDR_STORAGE + name);
        if (!d) { darwin_freeaddrinfo(first); freeaddrinfo(list); return DARWIN_EAI_MEMORY; }
        unsigned char *addr = d + DARWIN_SIZEOF_ADDRINFO;
        uint32_t addrlen = DARWIN_SIZEOF_SOCKADDR_STORAGE;
        int flags = 0;
        for (size_t i = 0; i < COUNT_OF(ai_flags); i++)
            if ((a->ai_flags & ai_flags[i].h) && ai_flags[i].d != DARWIN_AI_V4MAPPED_CFG) flags |= ai_flags[i].d;
        sa_darwin(a->ai_addr, a->ai_addrlen, addr, &addrlen);
        PUT(d, DARWIN_ADDRINFO_ai_flags, flags);
        PUT(d, DARWIN_ADDRINFO_ai_family, family);
        PUT(d, DARWIN_ADDRINFO_ai_socktype, a->ai_socktype);
        PUT(d, DARWIN_ADDRINFO_ai_protocol, a->ai_protocol);
        PUT(d, DARWIN_ADDRINFO_ai_addrlen, addrlen);
        PUT(d, DARWIN_ADDRINFO_ai_addr, addr);
        if (name) {
            char *canon = (char *)addr + DARWIN_SIZEOF_SOCKADDR_STORAGE;
            memcpy(canon, a->ai_canonname, name);
            PUT(d, DARWIN_ADDRINFO_ai_canonname, canon);
        }
        if (last) PUT(last, DARWIN_ADDRINFO_ai_next, d); else first = d;
        last = d;
    }
    freeaddrinfo(list);
    if (!first) return DARWIN_EAI_NONAME;
    *res = first;
    return 0;
}

static long darwin_getnameinfo(const unsigned char *addr, uint32_t len, char *host, uint32_t hostlen,
                               char *serv, uint32_t servlen, int flags)
{
    struct sockaddr_storage h;
    socklen_t hlen;
    if (!addr) return DARWIN_EAI_FAIL;
    if (sa_host(addr, len, &h, &hlen) != 0) return DARWIN_EAI_FAMILY;
    int hf = ((flags & DARWIN_NI_NOFQDN) ? NI_NOFQDN : 0) | ((flags & DARWIN_NI_NUMERICHOST) ? NI_NUMERICHOST : 0) |
             ((flags & DARWIN_NI_NAMEREQD) ? NI_NAMEREQD : 0) | ((flags & DARWIN_NI_NUMERICSERV) ? NI_NUMERICSERV : 0) |
             ((flags & DARWIN_NI_DGRAM) ? NI_DGRAM : 0);
    return eai_darwin(getnameinfo((struct sockaddr *)&h, hlen, host, hostlen, serv, servlen, hf));
}

static const char *darwin_inet_ntop(int af, const void *src, char *dst, uint32_t size)
{
    int family = family_host(af);
    if (family < 0) { errno = EAFNOSUPPORT; return NULL; }
    return inet_ntop(family, src, dst, size);
}

static long darwin_inet_pton(int af, const char *src, void *dst)
{
    int family = family_host(af);
    if (family < 0) { errno = EAFNOSUPPORT; return -1; }
    return inet_pton(family, src, dst);
}

// ---- getifaddrs --------------------------------------------------------------------------------
// One block for each entry: the struct, its name and three addresses. A link-layer address
// (AF_PACKET here) is a sockaddr_dl there: the interface's name, then its hardware address.

#define IFA_ADDR_ROOM 64

static const struct { unsigned d, h; } if_flags[] = {
    { DARWIN_IFF_UP, IFF_UP }, { DARWIN_IFF_BROADCAST, IFF_BROADCAST }, { DARWIN_IFF_LOOPBACK, IFF_LOOPBACK },
    { DARWIN_IFF_POINTOPOINT, IFF_POINTOPOINT }, { DARWIN_IFF_RUNNING, IFF_RUNNING }, { DARWIN_IFF_NOARP, IFF_NOARP },
    { DARWIN_IFF_PROMISC, IFF_PROMISC }, { DARWIN_IFF_ALLMULTI, IFF_ALLMULTI }, { DARWIN_IFF_MULTICAST, IFF_MULTICAST },
};

// 1: written. 0: an address of a family macOS has no name for
static int ifa_addr(const struct sockaddr *h, const char *ifname, unsigned char *d)
{
    if (h->sa_family == AF_PACKET) {
        const struct sockaddr_ll *ll = (const struct sockaddr_ll *)h;
        size_t nlen = strlen(ifname), alen = ll->sll_halen;
        if (nlen > 16) nlen = 16;
        if (alen > sizeof ll->sll_addr) alen = sizeof ll->sll_addr;
        size_t len = DARWIN_SOCKADDR_DL_sdl_data + nlen + alen;
        if (len < DARWIN_SIZEOF_SOCKADDR_DL) len = DARWIN_SIZEOF_SOCKADDR_DL;
        uint16_t index = (uint16_t)ll->sll_ifindex;
        d[DARWIN_SOCKADDR_sa_len] = (unsigned char)len;
        d[DARWIN_SOCKADDR_sa_family] = DARWIN_AF_LINK;
        PUT(d, DARWIN_SOCKADDR_DL_sdl_index, index);
        d[DARWIN_SOCKADDR_DL_sdl_type] = ll->sll_hatype == ARPHRD_LOOPBACK ? 0x18 : 0x6;   // IFT_LOOP, IFT_ETHER
        d[DARWIN_SOCKADDR_DL_sdl_nlen] = (unsigned char)nlen;
        d[DARWIN_SOCKADDR_DL_sdl_alen] = (unsigned char)alen;
        memcpy(d + DARWIN_SOCKADDR_DL_sdl_data, ifname, nlen);
        memcpy(d + DARWIN_SOCKADDR_DL_sdl_data + nlen, ll->sll_addr, alen);
        return 1;
    }
    if (h->sa_family != AF_INET && h->sa_family != AF_INET6) return 0;
    uint32_t room = IFA_ADDR_ROOM;
    sa_darwin(h, h->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6), d, &room);
    return 1;
}

static void darwin_freeifaddrs(unsigned char *ifa)
{
    while (ifa) {
        unsigned char *next;
        GET(ifa, DARWIN_IFADDRS_ifa_next, next);
        free(ifa);
        ifa = next;
    }
}

static long darwin_getifaddrs(unsigned char **out)
{
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0) return -1;
    unsigned char *first = NULL, *last = NULL;
    for (struct ifaddrs *a = list; a; a = a->ifa_next) {
        size_t name = strlen(a->ifa_name) + 1;
        unsigned char *d = calloc(1, DARWIN_SIZEOF_IFADDRS + 3 * IFA_ADDR_ROOM + name);
        if (!d) { darwin_freeifaddrs(first); freeifaddrs(list); errno = ENOMEM; return -1; }
        unsigned char *addrs = d + DARWIN_SIZEOF_IFADDRS;
        char *dname = (char *)addrs + 3 * IFA_ADDR_ROOM;
        uint32_t flags = 0;
        for (size_t i = 0; i < COUNT_OF(if_flags); i++)
            if (a->ifa_flags & if_flags[i].h) flags |= if_flags[i].d;
        memcpy(dname, a->ifa_name, name);
        PUT(d, DARWIN_IFADDRS_ifa_name, dname);
        PUT(d, DARWIN_IFADDRS_ifa_flags, flags);
        const struct sockaddr *src[3] = { a->ifa_addr, a->ifa_netmask,
                                          (a->ifa_flags & (IFF_BROADCAST | IFF_POINTOPOINT)) ? a->ifa_dstaddr : NULL };
        const int at[3] = { DARWIN_IFADDRS_ifa_addr, DARWIN_IFADDRS_ifa_netmask, DARWIN_IFADDRS_ifa_dstaddr };
        for (int i = 0; i < 3; i++) {
            unsigned char *to = addrs + i * IFA_ADDR_ROOM;
            if (src[i] && ifa_addr(src[i], a->ifa_name, to)) PUT(d, at[i], to);
        }
        if (last) PUT(last, DARWIN_IFADDRS_ifa_next, d); else first = d;
        last = d;
    }
    freeifaddrs(list);
    *out = first;
    return 0;
}

const struct darwin_sym darwin_syms_net[] = {
    SHIM(socket), SHIM(socketpair), SHIM(bind), SHIM(connect), SHIM(accept), SHIM(getsockname), SHIM(getpeername),
    SHIM(send), SHIM(recv), SHIM(recvfrom), SHIM(sendmsg), SHIM(recvmsg), SHIM(setsockopt), SHIM(getsockopt),
    SHIM(getaddrinfo), SHIM(freeaddrinfo), SHIM(getnameinfo), SHIM(inet_ntop), SHIM(inet_pton),
    SHIM(getifaddrs), SHIM(freeifaddrs),
    SAME(listen), SAME(shutdown), SAME(if_nametoindex),
    { NULL, NULL }
};
