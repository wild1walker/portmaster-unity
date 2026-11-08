// Hardware video decoding (H.264, HEVC) through libpmhwdec (LGPL-2.1-or-later,
// https://github.com/Knifethrower/pmhwdec), loaded with dlopen
// from the library path or a port's own copy through UM_PMHW=<path>: never linked or shipped here. libpmhwdec
// picks the handheld's decoder at run time (Allwinner's video engine, Rockchip MPP, V4L2 stateless
// or stateful) and checks it with its own start-up self-test. Without the library, on a device
// without a decoder, or for a stream it does not take (found from the parameter sets at
// configure), this decoder steps aside and um_codec gives the stream to the next decoder of its
// type (OpenH264, the system's libavcodec). Should libpmhwdec give up in the middle of a stream,
// a software decoder takes over from that access unit.
//
// Input: Annex B access units, as for OpenH264; the parameter sets (csd-0 / csd-1, codec config
// buffers) are sent ahead and again after a flush. Pictures come out as NV12 in presentation
// order with the time of the access unit they were coded in; 10-bit HEVC as its top 8 bits.
//
// UM_PMHW=0: never used. UM_H264=openh264 / ffmpeg chooses among the software decoders only (the
// one that takes over when this one steps aside). libpmhwdec's own PMHWDEC_LOG=1 /
// PMHWDEC_BACKEND=... apply.
#define _GNU_SOURCE
#include "um_internal.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ---- from pmhwdec.h (MIT), API version 1 --------------------------------------------------------- */
enum { PMHW_CODEC_H264 = 0, PMHW_CODEC_HEVC = 1 };
enum { PMHW_OK = 0, PMHW_AGAIN = 1, PMHW_ERROR = -1, PMHW_UNSUPPORTED = -2 };
typedef struct { int max_width, max_height, max_bit_depth; const char *backend; } PmhwCaps;
typedef struct {
    int width, height;           /* visible size */
    int left, top;               /* the visible area inside the planes */
    int format;                  /* PMHW_FMT_NV12 */
    const uint8_t *plane[2];     /* Y, interleaved UV */
    int stride[2];
    int64_t pts;
    int corrupt;
    int dmabuf_fd;
    uint32_t drm_fourcc;
    uint64_t drm_modifier;
    uint32_t offset[2];
    void *priv;
} PmhwPicture;
typedef struct PmhwDecoder PmhwDecoder;

static struct {
    int ok;
    int (*query)(int codec, PmhwCaps *caps);
    PmhwDecoder *(*open)(int codec, int width, int height);
    int (*send)(PmhwDecoder *d, const uint8_t *data, int size, int64_t pts);
    int (*get)(PmhwDecoder *d, PmhwPicture *pic);
    void (*put)(PmhwDecoder *d, PmhwPicture *pic);
    void (*flush)(PmhwDecoder *d);
    void (*close)(PmhwDecoder *d);
    const char *(*backend)(const PmhwDecoder *d);
} lib;
static pthread_once_t lib_once = PTHREAD_ONCE_INIT;

static void lib_load(void)
{
    const char *e = getenv("UM_PMHW");
    if (e && !strcmp(e, "0")) return;
    void *h = dlopen(e && *e ? e : "libpmhwdec.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) { if (e && *e) um_log("pmhwdec: %s: %s", e, dlerror()); return; }
#define SYM(f, n) (*(void **)&lib.f = dlsym(h, n))
    if (SYM(query, "pmhw_query") && SYM(open, "pmhw_open") && SYM(send, "pmhw_send") && SYM(get, "pmhw_get") &&
        SYM(put, "pmhw_put") && SYM(flush, "pmhw_flush") && SYM(close, "pmhw_close") &&
        SYM(backend, "pmhw_backend_name")) {
        lib.ok = 1;
        um_log("pmhwdec: libpmhwdec.so.1 loaded");
    } else {
        um_log("pmhwdec: libpmhwdec.so.1 lacks the version 1 API: not used");
        dlclose(h);
    }
#undef SYM
}

/* ---- the decoder ------------------------------------------------------------------------------- */
typedef struct {
    um_dec base;
    const char *mime;
    int codec;                  /* PMHW_CODEC_* */
    PmhwDecoder *dec;
    uint8_t *csd;               /* parameter sets, Annex B: sent at configure and before the first unit after a flush */
    size_t csdn;
    int csd_pending;
    uint8_t *buf;               /* parameter sets + access unit */
    size_t cap;
    um_format *fmt;             /* the input format, for a software decoder taking over */
    um_dec *sw;                 /* that decoder, after libpmhwdec gave up on the stream */
    int errors;
} pmhw;

static int codec_of(const char *mime)
{
    if (mime && !strcasecmp(mime, UM_MIME_AVC)) return PMHW_CODEC_H264;
    if (mime && !strcasecmp(mime, UM_MIME_HEVC)) return PMHW_CODEC_HEVC;
    return -1;
}

static void add_csd(pmhw *p, const uint8_t *data, size_t n)
{
    if (!n || (p->csdn >= n && memmem(p->csd, p->csdn, data, n))) return;   /* already there */
    uint8_t *c = realloc(p->csd, p->csdn + n);
    if (!c) return;
    memcpy(c + p->csdn, data, n);
    p->csd = c;
    p->csdn += n;
}

/* every picture ready: to the codec, then back to libpmhwdec; the count */
static int drain(pmhw *p, um_emit *em)
{
    PmhwPicture pic;
    int n = 0;
    while (lib.get(p->dec, &pic) == 1) {
        if (em && em->video_nv12) {
            const uint8_t *y = pic.plane[0] + (size_t)pic.top * pic.stride[0] + pic.left;
            const uint8_t *uv = pic.plane[1] + (size_t)(pic.top / 2) * pic.stride[1] + (pic.left & ~1);
            em->video_nv12(em->codec, y, pic.stride[0], uv, pic.stride[1], pic.width, pic.height, pic.pts);
        }
        lib.put(p->dec, &pic);
        n++;
    }
    return n;
}

/* one unit (NULL / 0: end of stream); PMHW_AGAIN (every picture buffer taken) is waited out by
   handing pictures on */
static int send_unit(pmhw *p, const uint8_t *data, size_t size, int64_t pts, um_emit *em)
{
    for (int tries = 0;; tries++) {
        int r = lib.send(p->dec, data, (int)size, pts);
        if (r != PMHW_AGAIN) return r;
        if (!drain(p, em)) {
            if (tries > 2000) { um_log("pmhwdec: no picture buffer frees up"); return PMHW_ERROR; }
            usleep(1000);
        }
    }
}

/* libpmhwdec gave up on this stream: the first software decoder of its type that takes it */
static int to_software(pmhw *p, um_emit *em)
{
    send_unit(p, NULL, 0, 0, em);                    /* the pictures still in its reorder queue */
    drain(p, em);
    lib.close(p->dec);
    p->dec = NULL;
    um_dec *(*cands[])(const char *) = { p->codec == PMHW_CODEC_H264 ? um_h264_create : NULL, um_ffmpeg_create };
    for (size_t i = 0; i < sizeof cands / sizeof cands[0] && !p->sw; i++) {
        um_dec *d = cands[i] ? cands[i](p->mime) : NULL;
        if (!d) continue;
        um_format *o = um_format_new();
        if (d->configure(d, p->fmt, o) == UM_OK) p->sw = d;
        else d->destroy(d);
        um_format_free(o);
    }
    if (!p->sw) { um_log("pmhwdec: the stream was refused and no software decoder takes it"); return UM_ERR_UNSUPPORTED; }
    um_log("pmhwdec: the hardware decoder gave up on the stream; %s goes on from here", p->sw->name);
    if (p->csdn) p->sw->decode(p->sw, p->csd, p->csdn, 0, UM_BUFFER_FLAG_CODEC_CONFIG, em);
    return UM_OK;
}

static int p_configure(um_dec *d, const um_format *in, um_format *out)
{
    pmhw *p = (pmhw *)d;
    (void)out;
    PmhwCaps caps;
    if (lib.query(p->codec, &caps) != 0) { um_log("pmhwdec: no hardware decoder for %s here", p->mime); return UM_ERR_UNSUPPORTED; }
    int32_t w = 0, h = 0;
    um_format_get_int32(in, UM_KEY_WIDTH, &w);
    um_format_get_int32(in, UM_KEY_HEIGHT, &h);
    size_t n0 = 0, n1 = 0;
    const uint8_t *c0 = um_format_get_buffer(in, "csd-0", &n0), *c1 = um_format_get_buffer(in, "csd-1", &n1);
    if (c0) add_csd(p, c0, n0);
    if (c1) add_csd(p, c1, n1);
    p->dec = lib.open(p->codec, w, h);
    if (!p->dec) { um_log("pmhwdec: %dx%d not for %s", w, h, caps.backend); return UM_ERR_UNSUPPORTED; }
    /* the parameter sets now: a stream the hardware does not take (10-bit on an 8-bit decoder,
       interlaced...) is refused here and goes to the software decoders */
    if (p->csdn && send_unit(p, p->csd, p->csdn, 0, NULL) == PMHW_UNSUPPORTED) {
        um_log("pmhwdec: %s refuses the stream", lib.backend(p->dec));
        lib.close(p->dec);
        p->dec = NULL;
        return UM_ERR_UNSUPPORTED;
    }
    um_format_free(p->fmt);
    p->fmt = um_format_copy(in);
    um_log("pmhwdec: %s %dx%d on %s", p->codec == PMHW_CODEC_H264 ? "H.264" : "HEVC", w, h, lib.backend(p->dec));
    return UM_OK;
}

static int p_decode(um_dec *d, const uint8_t *data, size_t size, int64_t pts, uint32_t flags, um_emit *em)
{
    pmhw *p = (pmhw *)d;
    if (p->sw) return p->sw->decode(p->sw, data, size, pts, flags, em);
    if (!p->dec) return UM_ERR_STATE;
    int r = PMHW_OK;
    if (flags & UM_BUFFER_FLAG_CODEC_CONFIG) {
        add_csd(p, data, size);
        r = send_unit(p, data, size, pts, em);
    } else if (size) {
        const uint8_t *in = data;
        size_t n = size;
        if (p->csd_pending && p->csdn) {
            if (p->cap < p->csdn + size) {
                uint8_t *b = realloc(p->buf, p->csdn + size);
                if (!b) return UM_ERR_UNKNOWN;
                p->buf = b;
                p->cap = p->csdn + size;
            }
            memcpy(p->buf, p->csd, p->csdn);
            memcpy(p->buf + p->csdn, data, size);
            in = p->buf;
            n = p->csdn + size;
        }
        r = send_unit(p, in, n, pts, em);
        if (r != PMHW_UNSUPPORTED) p->csd_pending = 0;
    }
    if (r == PMHW_UNSUPPORTED) {
        if (to_software(p, em) != UM_OK) return UM_ERR_UNSUPPORTED;
        return (flags & UM_BUFFER_FLAG_CODEC_CONFIG) ? UM_OK : p->sw->decode(p->sw, data, size, pts, flags, em);
    }
    if (r == PMHW_ERROR && p->errors++ < 5) um_log("pmhwdec: access unit at %lld us dropped", (long long)pts);
    if (flags & UM_BUFFER_FLAG_END_OF_STREAM) send_unit(p, NULL, 0, 0, em);
    drain(p, em);
    return UM_OK;
}

static void p_flush(um_dec *d)
{
    pmhw *p = (pmhw *)d;
    if (p->sw) { p->sw->flush(p->sw); return; }
    if (p->dec) lib.flush(p->dec);
    p->csd_pending = 1;
}

static void p_destroy(um_dec *d)
{
    pmhw *p = (pmhw *)d;
    if (p->dec) lib.close(p->dec);
    if (p->sw) p->sw->destroy(p->sw);
    um_format_free(p->fmt);
    free(p->csd);
    free(p->buf);
    free(p);
}

um_dec *um_pmhw_create(const char *mime)
{
    pthread_once(&lib_once, lib_load);
    int codec = codec_of(mime);
    PmhwCaps caps;
    if (!lib.ok || codec < 0 || lib.query(codec, &caps) != 0) return NULL;
    pmhw *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->mime = codec == PMHW_CODEC_H264 ? UM_MIME_AVC : UM_MIME_HEVC;
    p->codec = codec;
    p->base.name = "pmhwdec";
    p->base.configure = p_configure;
    p->base.decode = p_decode;
    p->base.flush = p_flush;
    p->base.destroy = p_destroy;
    return &p->base;
}
