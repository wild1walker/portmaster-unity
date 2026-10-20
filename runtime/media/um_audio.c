// Audio decoders: Vorbis (libvorbis + libogg, BSD-3-Clause, UM_HAVE_VORBIS) and Opus (libopus,
// BSD-3-Clause, UM_HAVE_OPUS), both linked in statically. Output: interleaved 16-bit PCM, as
// Android's software decoders give it (Unity reads shorts).
//
// Vorbis: the identification and setup headers come from csd-0 / csd-1 (WebM's CodecPrivate,
// split by the extractor as Android's does) or as CODEC_CONFIG input buffers; a comment header is
// made up in between. Opus: csd-0 is OpusHead (channel mapping, pre-skip, output gain); the first
// pre-skip samples, and after a flush (seek) the seek pre-roll from csd-2, are not output.
#include "um_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline int16_t f2s(float x)
{
    float v = x * 32768.0f;
    if (v >= 32767.0f) return 32767;
    if (v <= -32768.0f) return -32768;
    return (int16_t)lrintf(v);
}

/* ---- Vorbis ----------------------------------------------------------------------------------- */
#ifndef UM_HAVE_VORBIS
um_dec *um_vorbis_create(const char *mime) { (void)mime; return NULL; }
#else
#include <vorbis/codec.h>

typedef struct {
    um_dec base;
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    int nheaders, ready;
    int64_t packetno;
    int16_t *pcm;
    size_t pcm_n;
} vorbisdec;

static int vb_header(vorbisdec *v, const uint8_t *p, size_t n)
{
    ogg_packet op = { 0 };
    op.packet = (unsigned char *)p;
    op.bytes = (long)n;
    op.b_o_s = v->nheaders == 0;
    op.packetno = v->packetno++;
    int r = vorbis_synthesis_headerin(&v->vi, &v->vc, &op);
    if (r != 0) { um_log("vorbis: header %d rejected (%d)", v->nheaders, r); return UM_ERR_MALFORMED; }
    v->nheaders++;
    if (v->nheaders == 1) {
        /* Android's extractor passes no comment header: an empty one */
        static const uint8_t vendor[] = "um";
        uint8_t c[7 + 4 + 2 + 4 + 1];
        memcpy(c, "\003vorbis", 7);
        c[7] = 2; c[8] = c[9] = c[10] = 0;
        memcpy(c + 11, vendor, 2);
        memset(c + 13, 0, 4);
        c[17] = 1;
        ogg_packet cp = { 0 };
        cp.packet = c; cp.bytes = sizeof c; cp.packetno = v->packetno++;
        if (vorbis_synthesis_headerin(&v->vi, &v->vc, &cp) != 0) return UM_ERR_MALFORMED;
        v->nheaders++;
    }
    if (v->nheaders == 3) {
        if (vorbis_synthesis_init(&v->vd, &v->vi) != 0 || vorbis_block_init(&v->vd, &v->vb) != 0) return UM_ERR_MALFORMED;
        v->ready = 1;
    }
    return UM_OK;
}

static int vb_configure(um_dec *d, const um_format *in, um_format *out)
{
    vorbisdec *v = (vorbisdec *)d;
    size_t n0 = 0, n1 = 0;
    const void *c0 = um_format_get_buffer(in, "csd-0", &n0);
    const void *c1 = um_format_get_buffer(in, "csd-1", &n1);
    if (c0 && c1) {
        if (vb_header(v, c0, n0) != UM_OK || vb_header(v, c1, n1) != UM_OK) return UM_ERR_MALFORMED;
        um_format_set_int32(out, UM_KEY_SAMPLE_RATE, (int32_t)v->vi.rate);
        um_format_set_int32(out, UM_KEY_CHANNEL_COUNT, v->vi.channels);
    }
    return UM_OK;
}

static int vb_decode(um_dec *d, const uint8_t *data, size_t size, int64_t pts, uint32_t flags, um_emit *em)
{
    vorbisdec *v = (vorbisdec *)d;
    if (!v->ready) {
        if (!size) return UM_OK;
        return vb_header(v, data, size);
    }
    if (!size) return UM_OK;
    ogg_packet op = { 0 };
    op.packet = (unsigned char *)data;
    op.bytes = (long)size;
    op.granulepos = -1;
    op.packetno = v->packetno++;
    if (vorbis_synthesis(&v->vb, &op) != 0) return UM_ERR_MALFORMED;   /* (a header or a bad packet) */
    vorbis_synthesis_blockin(&v->vd, &v->vb);
    float **pcm;
    int n;
    int ch = v->vi.channels;
    while ((n = vorbis_synthesis_pcmout(&v->vd, &pcm)) > 0) {
        size_t need = (size_t)n * ch;
        if (need > v->pcm_n) {
            int16_t *t = realloc(v->pcm, need * sizeof *t);
            if (!t) return UM_ERR_UNKNOWN;
            v->pcm = t; v->pcm_n = need;
        }
        for (int i = 0; i < n; i++)
            for (int c = 0; c < ch; c++) v->pcm[(size_t)i * ch + c] = f2s(pcm[c][i]);
        em->audio(em->codec, v->pcm, n, ch, (int)v->vi.rate, pts);
        vorbis_synthesis_read(&v->vd, n);
    }
    (void)flags;
    return UM_OK;
}

static void vb_flush(um_dec *d)
{
    vorbisdec *v = (vorbisdec *)d;
    if (v->ready) vorbis_synthesis_restart(&v->vd);
}

static void vb_destroy(um_dec *d)
{
    vorbisdec *v = (vorbisdec *)d;
    if (v->ready) { vorbis_block_clear(&v->vb); vorbis_dsp_clear(&v->vd); }
    vorbis_comment_clear(&v->vc);
    vorbis_info_clear(&v->vi);
    free(v->pcm);
    free(v);
}

um_dec *um_vorbis_create(const char *mime)
{
    (void)mime;
    vorbisdec *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    vorbis_info_init(&v->vi);
    vorbis_comment_init(&v->vc);
    v->base.name = "libvorbis";
    v->base.configure = vb_configure;
    v->base.decode = vb_decode;
    v->base.flush = vb_flush;
    v->base.destroy = vb_destroy;
    return &v->base;
}
#endif

/* ---- Opus -------------------------------------------------------------------------------------- */
#ifndef UM_HAVE_OPUS
um_dec *um_opus_create(const char *mime) { (void)mime; return NULL; }
#else
#include <opus/opus_multistream.h>

typedef struct {
    um_dec base;
    OpusMSDecoder *dec;
    int channels, pre_skip, preroll, skip;
    int16_t *pcm;
} opusdec;

static int op_configure(um_dec *d, const um_format *in, um_format *out)
{
    opusdec *o = (opusdec *)d;
    size_t n = 0, n2 = 0;
    const uint8_t *h = um_format_get_buffer(in, "csd-0", &n);
    const uint8_t *c2 = um_format_get_buffer(in, "csd-2", &n2);
    int32_t chs = 2;
    um_format_get_int32(in, UM_KEY_CHANNEL_COUNT, &chs);
    unsigned char map[255];
    int streams = 1, coupled = chs > 1, gain = 0;
    o->channels = chs;
    map[0] = 0; map[1] = 1;
    if (h && n >= 19 && !memcmp(h, "OpusHead", 8)) {
        o->channels = h[9];
        o->pre_skip = h[10] | h[11] << 8;
        gain = (int16_t)(h[16] | h[17] << 8);
        int family = h[18];
        if (family == 0) { streams = 1; coupled = o->channels > 1; map[0] = 0; map[1] = 1; }
        else if (n >= 21u + o->channels) {
            streams = h[19]; coupled = h[20];
            memcpy(map, h + 21, o->channels);
        } else return UM_ERR_MALFORMED;
    }
    if (o->channels < 1 || o->channels > 8) return UM_ERR_UNSUPPORTED;
    if (c2 && n2 == 8) {
        uint64_t ns = 0;
        for (int i = 7; i >= 0; i--) ns = ns << 8 | c2[i];
        o->preroll = (int)(ns * 48000 / 1000000000ULL);
    } else o->preroll = 3840;            /* 80 ms, the Opus/WebM recommendation */
    int err = 0;
    o->dec = opus_multistream_decoder_create(48000, o->channels, streams, coupled, map, &err);
    if (!o->dec) { um_log("opus: decoder: %s", opus_strerror(err)); return UM_ERR_MALFORMED; }
    if (gain) opus_multistream_decoder_ctl(o->dec, OPUS_SET_GAIN(gain));
    o->skip = o->pre_skip;
    o->pcm = malloc(5760 * sizeof(int16_t) * o->channels);
    um_format_set_int32(out, UM_KEY_SAMPLE_RATE, 48000);
    um_format_set_int32(out, UM_KEY_CHANNEL_COUNT, o->channels);
    return o->pcm ? UM_OK : UM_ERR_UNKNOWN;
}

static int op_decode(um_dec *d, const uint8_t *data, size_t size, int64_t pts, uint32_t flags, um_emit *em)
{
    opusdec *o = (opusdec *)d;
    if (!o->dec || !size || (flags & UM_BUFFER_FLAG_CODEC_CONFIG)) return UM_OK;
    int n = opus_multistream_decode(o->dec, data, (opus_int32)size, o->pcm, 5760, 0);
    if (n < 0) { um_log("opus: %s", opus_strerror(n)); return UM_ERR_MALFORMED; }
    int start = 0;
    if (o->skip > 0) { start = o->skip < n ? o->skip : n; o->skip -= start; }
    if (n > start)
        em->audio(em->codec, o->pcm + (size_t)start * o->channels, n - start, o->channels, 48000,
                  pts + start * 1000000LL / 48000);
    return UM_OK;
}

static void op_flush(um_dec *d)
{
    opusdec *o = (opusdec *)d;
    if (o->dec) opus_multistream_decoder_ctl(o->dec, OPUS_RESET_STATE);
    o->skip = o->preroll;
}

static void op_destroy(um_dec *d)
{
    opusdec *o = (opusdec *)d;
    if (o->dec) opus_multistream_decoder_destroy(o->dec);
    free(o->pcm);
    free(o);
}

um_dec *um_opus_create(const char *mime)
{
    (void)mime;
    opusdec *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->base.name = "libopus";
    o->base.configure = op_configure;
    o->base.decode = op_decode;
    o->base.flush = op_flush;
    o->base.destroy = op_destroy;
    return &o->base;
}
#endif
