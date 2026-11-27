// server <port> [<cert> <key>]: a local HTTP(S) server for tls_test.sh (127.0.0.1 only; it was
// test/tls/server.py): GET answers "hello from tls.test", POST/PUT answer "got <n> bytes <the first 16>",
// /big a 300000-byte body; every answer echoes the request's X-Test header as X-Echo. HTTP/1.1 with
// keep-alive, one process a connection. TLS is mbedTLS (the image's libmbedtls), a log line a request
// on stderr.
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>

static const char *g_port;
static mbedtls_ssl_config g_conf;
static int g_tls;

typedef struct {
    int fd;
    mbedtls_ssl_context ssl;
    char buf[65536];
    size_t have;
} Conn;

static int cread(Conn *c, void *p, size_t n)
{
    if (!g_tls) return (int)read(c->fd, p, n);
    for (;;) {
        int r = mbedtls_ssl_read(&c->ssl, p, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return r;
    }
}

static int cwrite(Conn *c, const void *p, size_t n)
{
    const unsigned char *b = p;
    while (n) {
        int r = g_tls ? mbedtls_ssl_write(&c->ssl, b, n) : (int)write(c->fd, b, n);
        if (g_tls && (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)) continue;
        if (r <= 0) return -1;
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

// the next n bytes of the request (from what is buffered first)
static int take(Conn *c, char *out, size_t n)
{
    while (c->have < n) {
        int r = cread(c, c->buf + c->have, sizeof c->buf - c->have);
        if (r <= 0) return -1;
        c->have += (size_t)r;
    }
    memcpy(out, c->buf, n);
    memmove(c->buf, c->buf + n, c->have - n);
    c->have -= n;
    return 0;
}

// the request head (up to the blank line), or -1 at the connection's end
static long head(Conn *c)
{
    for (;;) {
        for (size_t i = 3; i < c->have; i++)
            if (!memcmp(c->buf + i - 3, "\r\n\r\n", 4)) return (long)(i + 1);
        if (c->have == sizeof c->buf) return -1;
        int r = cread(c, c->buf + c->have, sizeof c->buf - c->have);
        if (r <= 0) return -1;
        c->have += (size_t)r;
    }
}

static void header(const char *h, size_t n, const char *name, char *out, size_t cap)
{
    size_t k = strlen(name);
    for (const char *p = h; p < h + n;) {
        const char *e = memchr(p, '\n', (size_t)(h + n - p));
        if (!e) break;
        if ((size_t)(e - p) > k && !strncasecmp(p, name, k) && p[k] == ':') {
            const char *v = p + k + 1;
            while (*v == ' ') v++;
            size_t l = (size_t)(e - v);
            if (l && v[l - 1] == '\r') l--;
            if (l >= cap) l = cap - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return;
        }
        p = e + 1;
    }
}

static void serve(Conn *c)
{
    for (;;) {
        long n = head(c);
        if (n < 0) return;
        char h[65536], method[16] = "", path[1024] = "", len[32] = "", echo[256] = "-";
        memcpy(h, c->buf, (size_t)n);
        h[n] = 0;
        memmove(c->buf, c->buf + n, c->have - (size_t)n);
        c->have -= (size_t)n;
        sscanf(h, "%15s %1023s", method, path);
        header(h, (size_t)n, "Content-Length", len, sizeof len);
        header(h, (size_t)n, "X-Test", echo, sizeof echo);
        size_t blen = (size_t)atol(len);
        char *body = malloc(blen + 1), *reply;
        size_t rlen;
        if (!body || take(c, body, blen)) { free(body); return; }
        if (!strcmp(method, "POST") || !strcmp(method, "PUT")) {
            reply = malloc(64);
            int k = snprintf(reply, 48, "got %zu bytes ", blen);
            size_t first = blen < 16 ? blen : 16;
            memcpy(reply + k, body, first);
            reply[k + first] = '\n';
            rlen = (size_t)k + first + 1;
        } else if (!strcmp(path, "/big")) {
            rlen = 300000;
            reply = malloc(rlen);
            memset(reply, 'x', rlen);
        } else {
            reply = strdup("hello from tls.test\n");
            rlen = strlen(reply);
        }
        char top[512];
        int tl = snprintf(top, sizeof top,
                          "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nX-Echo: %s\r\n\r\n", rlen, echo);
        int bad = cwrite(c, top, (size_t)tl) || (strcmp(method, "HEAD") && cwrite(c, reply, rlen));
        fprintf(stderr, "[server %s] \"%s %s HTTP/1.1\" 200 -\n", g_port, method, path);
        free(body);
        free(reply);
        if (bad) return;
    }
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 4) {
        fprintf(stderr, "usage: server <port> [<cert> <key>]\n");
        return 2;
    }
    g_port = argv[1];
    g_tls = argc == 4;
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    mbedtls_x509_crt crt;
    mbedtls_pk_context key;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    if (g_tls) {
        mbedtls_x509_crt_init(&crt);
        mbedtls_pk_init(&key);
        mbedtls_entropy_init(&ent);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_ssl_config_init(&g_conf);
        if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, NULL, 0) || mbedtls_x509_crt_parse_file(&crt, argv[2]) ||
#if MBEDTLS_VERSION_MAJOR >= 3
            mbedtls_pk_parse_keyfile(&key, argv[3], NULL, mbedtls_ctr_drbg_random, &drbg) ||
#else
            mbedtls_pk_parse_keyfile(&key, argv[3], NULL) ||
#endif
            mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) ||
            mbedtls_ssl_conf_own_cert(&g_conf, &crt, &key)) {
            fprintf(stderr, "server %s: cannot set up TLS with %s and %s\n", argv[1], argv[2], argv[3]);
            return 1;
        }
        mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &drbg);
    }
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)atoi(argv[1])), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16)) {
        fprintf(stderr, "server %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) continue;
        if (fork() == 0) {
            close(s);
            static Conn c;
            c.fd = fd;
            if (g_tls) {
                mbedtls_ssl_init(&c.ssl);
                if (mbedtls_ssl_setup(&c.ssl, &g_conf)) _exit(1);
                mbedtls_ssl_set_bio(&c.ssl, &c.fd, mbedtls_net_send, mbedtls_net_recv, NULL);
                int r;
                while ((r = mbedtls_ssl_handshake(&c.ssl)) == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {}
                if (r) _exit(0);   // (the client refused our certificate: what the bad-CA cases want)
            }
            serve(&c);
            if (g_tls) mbedtls_ssl_close_notify(&c.ssl);
            close(fd);
            _exit(0);
        }
        close(fd);
    }
}
