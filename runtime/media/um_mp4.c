// MP4 / QuickTime demuxer (ISO base media file format): moov / trak / mdia / minf / stbl into
// sample tables (decode order; times are presentation times: stts + ctts, shifted by the edit
// list as Android's MPEG4Extractor does), for H.264 / HEVC video and AAC / MP3 audio, the codecs
// Unity's macOS and Windows players transcode video clips to.
//
// Track formats as Android's: video/avc with csd-0 (SPS) and csd-1 (PPS) in Annex B form,
// video/hevc with csd-0 (VPS + SPS + PPS), audio/mp4a-latm with csd-0 (AudioSpecificConfig).
// As Android's extractor, H.264 / HEVC samples are handed out in Annex B form (each NAL length
// replaced by a start code): um_extractor_read converts them (um_track.nal_len).
#define _GNU_SOURCE
#include "um_internal.h"

#include <stdlib.h>
#include <string.h>

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* ---- H.264 / HEVC parameter sets ------------------------------------------------------------------ */
static void put_nal(uint8_t **o, size_t *n, size_t *cap, const uint8_t *p, size_t len)
{
    if (*n + len + 4 > *cap) { *cap = (*n + len + 4) * 2; *o = realloc(*o, *cap); }
    memcpy(*o + *n, "\0\0\0\1", 4);
    memcpy(*o + *n + 4, p, len);
    *n += len + 4;
}

/* avcC -> csd-0 (the SPS) and csd-1 (the PPS), Annex B */
int um_avcc_csd(um_format *f, const uint8_t *a, size_t n, int *nls)
{
    if (n < 7 || a[0] != 1) return -1;
    *nls = (a[4] & 3) + 1;
    size_t p = 6;
    for (int k = 0; k < 2; k++) {
        int cnt = k == 0 ? a[5] & 31 : (p < n ? a[p++] : 0);
        uint8_t *o = NULL;
        size_t on = 0, cap = 0;
        for (int i = 0; i < cnt; i++) {
            if (p + 2 > n) { free(o); return -1; }
            size_t l = be16(a + p);
            p += 2;
            if (p + l > n) { free(o); return -1; }
            put_nal(&o, &on, &cap, a + p, l);
            p += l;
        }
        if (o) um_format_set_buffer(f, k ? "csd-1" : "csd-0", o, on);
        free(o);
    }
    return 0;
}

/* hvcC -> csd-0 with every VPS / SPS / PPS, Annex B */
int um_hvcc_csd(um_format *f, const uint8_t *a, size_t n, int *nls)
{
    if (n < 23) return -1;
    *nls = (a[21] & 3) + 1;
    int arrays = a[22];
    size_t p = 23;
    uint8_t *o = NULL;
    size_t on = 0, cap = 0;
    for (int i = 0; i < arrays; i++) {
        if (p + 3 > n) break;
        int cnt = be16(a + p + 1);
        p += 3;
        for (int j = 0; j < cnt; j++) {
            if (p + 2 > n) break;
            size_t l = be16(a + p);
            p += 2;
            if (p + l > n) break;
            put_nal(&o, &on, &cap, a + p, l);
            p += l;
        }
    }
    if (o) um_format_set_buffer(f, "csd-0", o, on);
    free(o);
    return o ? 0 : -1;
}

/* length-prefixed NAL units -> start codes into out (cap): the converted size, or -1 */
ssize_t um_nal_convert(const uint8_t *in, size_t n, int nls, uint8_t *out, size_t cap)
{
    size_t p = 0, o = 0;
    while (p + nls <= n) {
        size_t l = 0;
        for (int i = 0; i < nls; i++) l = l << 8 | in[p + i];
        p += nls;
        if (l > n - p || o + 4 + l > cap) return -1;
        memmove(out + o + 4, in + p, l);       /* (in == out with nls 4: in place) */
        memcpy(out + o, "\0\0\0\1", 4);
        o += 4 + l;
        p += l;
    }
    return (ssize_t)o;
}

/* ---- boxes ---------------------------------------------------------------------------------------- */
typedef struct { uint32_t type; int64_t data, size; } box;

static int box_at(um_source *s, int64_t pos, int64_t end, box *b)
{
    uint8_t h[16];
    if (end - pos < 8 || um_source_read(s, pos, h, 8) != 0) return -1;
    uint64_t size = be32(h);
    b->type = be32(h + 4);
    int64_t hdr = 8;
    if (size == 1) {
        if (end - pos < 16 || um_source_read(s, pos + 8, h + 8, 8) != 0) return -1;
        size = be64(h + 8);
        hdr = 16;
    } else if (size == 0) size = end - pos;
    if ((int64_t)size < hdr || pos + (int64_t)size > end) return -1;
    b->data = pos + hdr;
    b->size = (int64_t)size - hdr;
    return 0;
}

/* the whole payload of a box (tables), malloc'd */
static uint8_t *box_load(um_source *s, const box *b, size_t max)
{
    if (b->size < 0 || (uint64_t)b->size > max) return NULL;
    uint8_t *p = malloc(b->size ? b->size : 1);
    if (p && um_source_read(s, b->data, p, b->size) != 0) { free(p); return NULL; }
    return p;
}

static int find_child(um_source *s, const box *parent, int64_t skip, uint32_t type, box *out)
{
    int64_t p = parent->data + skip, end = parent->data + parent->size;
    box b;
    while (p < end && box_at(s, p, end, &b) == 0) {
        if (b.type == type) { *out = b; return 0; }
        p = b.data + b.size;
    }
    return -1;
}

/* ---- one track ------------------------------------------------------------------------------------ */
typedef struct {
    uint32_t handler, timescale, codec;
    int64_t elst_shift;         /* media time of the first edit (media timescale) */
    int64_t elst_delay;         /* an initial empty edit (us) */
    int width, height, channels, rate, nls;
    char lang[4];
    uint8_t *cfg; size_t cfgn;  /* avcC / hvcC / AudioSpecificConfig */
    int aot;                    /* MPEG-4 audio object type indication (esds) */
    uint8_t *stts, *ctts, *stsc, *stsz, *stco, *stss;
    size_t sttsn, cttsn, stscn, stszn, stcon, stssn;
    int co64, stz2;
} tk;

static void tk_free(tk *t)
{
    free(t->cfg); free(t->stts); free(t->ctts); free(t->stsc); free(t->stsz); free(t->stco); free(t->stss);
}

/* MPEG-4 descriptor length: 1-4 bytes of 7 bits */
static size_t desc_len(const uint8_t *p, size_t n, size_t *at)
{
    size_t l = 0;
    for (int i = 0; i < 4 && *at < n; i++) {
        uint8_t c = p[(*at)++];
        l = l << 7 | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    return l;
}

static void parse_esds(tk *t, const uint8_t *p, size_t n)
{
    size_t at = 4;                                   /* version + flags */
    if (at >= n || p[at++] != 0x03) return;          /* ES_Descriptor */
    desc_len(p, n, &at);
    if (at + 3 > n) return;
    uint8_t fl = p[at + 2];
    at += 3;
    if (fl & 0x80) at += 2;
    if (fl & 0x40) { if (at >= n) return; at += 1 + p[at]; }
    if (fl & 0x20) at += 2;
    if (at >= n || p[at++] != 0x04) return;          /* DecoderConfigDescriptor */
    desc_len(p, n, &at);
    if (at + 13 > n) return;
    t->aot = p[at];
    at += 13;
    if (at >= n || p[at++] != 0x05) return;          /* DecoderSpecificInfo */
    size_t l = desc_len(p, n, &at);
    if (at + l > n) return;
    free(t->cfg);
    t->cfg = malloc(l ? l : 1);
    memcpy(t->cfg, p + at, l);
    t->cfgn = l;
}

static void parse_stsd(um_source *s, tk *t, const box *stsd)
{
    uint8_t *d = box_load(s, stsd, 1 << 20);
    if (!d || stsd->size < 16) { free(d); return; }
    /* version/flags, entry count, then the first sample entry (size, type) */
    size_t es = be32(d + 8);
    t->codec = be32(d + 12);
    if (es > (size_t)stsd->size - 8) es = stsd->size - 8;
    const uint8_t *e = d + 8;
    size_t child = 0;
    if (t->handler == FOURCC('v', 'i', 'd', 'e') && es >= 86) {
        t->width = be16(e + 32);
        t->height = be16(e + 34);
        child = 86;
    } else if (t->handler == FOURCC('s', 'o', 'u', 'n') && es >= 36) {
        int ver = be16(e + 16);
        t->channels = be16(e + 24);
        t->rate = be32(e + 32) >> 16;
        child = 36 + (ver == 1 ? 16 : ver == 2 ? 36 : 0);
    }
    for (size_t p = child; p + 8 <= es; ) {
        size_t bs = be32(e + p);
        uint32_t bt = be32(e + p + 4);
        if (bs < 8 || p + bs > es) break;
        const uint8_t *bd = e + p + 8;
        size_t bn = bs - 8;
        if (bt == FOURCC('a', 'v', 'c', 'C') || bt == FOURCC('h', 'v', 'c', 'C')) {
            free(t->cfg);
            t->cfg = malloc(bn ? bn : 1);
            memcpy(t->cfg, bd, bn);
            t->cfgn = bn;
        } else if (bt == FOURCC('e', 's', 'd', 's')) {
            parse_esds(t, bd, bn);
        } else if (bt == FOURCC('w', 'a', 'v', 'e')) {        /* QuickTime: the esds is inside */
            for (size_t q = 0; q + 8 <= bn; ) {
                size_t qs = be32(bd + q);
                if (qs < 8 || q + qs > bn) break;
                if (be32(bd + q + 4) == FOURCC('e', 's', 'd', 's')) parse_esds(t, bd + q + 8, qs - 8);
                q += qs;
            }
        }
        p += bs;
    }
    free(d);
}

#define TABLE(field, n, b, minsz) do { free(t->field); t->field = box_load(s, &b, 256u << 20); \
    t->n = t->field && b.size >= (minsz) ? (size_t)b.size : 0; } while (0)

static void parse_stbl(um_source *s, tk *t, const box *stbl)
{
    int64_t p = stbl->data, end = stbl->data + stbl->size;
    box b;
    while (p < end && box_at(s, p, end, &b) == 0) {
        switch (b.type) {
        case FOURCC('s', 't', 's', 'd'): parse_stsd(s, t, &b); break;
        case FOURCC('s', 't', 't', 's'): TABLE(stts, sttsn, b, 8); break;
        case FOURCC('c', 't', 't', 's'): TABLE(ctts, cttsn, b, 8); break;
        case FOURCC('s', 't', 's', 'c'): TABLE(stsc, stscn, b, 8); break;
        case FOURCC('s', 't', 's', 'z'): TABLE(stsz, stszn, b, 12); t->stz2 = 0; break;
        case FOURCC('s', 't', 'z', '2'): TABLE(stsz, stszn, b, 12); t->stz2 = 1; break;
        case FOURCC('s', 't', 'c', 'o'): TABLE(stco, stcon, b, 8); t->co64 = 0; break;
        case FOURCC('c', 'o', '6', '4'): TABLE(stco, stcon, b, 8); t->co64 = 1; break;
        case FOURCC('s', 't', 's', 's'): TABLE(stss, stssn, b, 8); break;
        }
        p = b.data + b.size;
    }
}

static int parse_trak(um_source *s, const box *trak, uint32_t movie_ts, tk *t)
{
    memset(t, 0, sizeof *t);
    box mdia, b;
    if (find_child(s, trak, 0, FOURCC('m', 'd', 'i', 'a'), &mdia) != 0) return -1;
    if (find_child(s, &mdia, 0, FOURCC('h', 'd', 'l', 'r'), &b) == 0 && b.size >= 12) {
        uint8_t h[12];
        if (um_source_read(s, b.data, h, 12) == 0) t->handler = be32(h + 8);
    }
    if (find_child(s, &mdia, 0, FOURCC('m', 'd', 'h', 'd'), &b) == 0 && b.size >= 24) {
        /* v0: version/flags, creation, modification, timescale (12), duration, language (20);
           v1: 64-bit times: timescale (20), duration (24, 8 bytes), language (32) */
        uint8_t h[36];
        size_t n = b.size >= 36 ? 36 : 24;
        if (um_source_read(s, b.data, h, n) == 0) {
            int v1 = h[0] == 1;
            if (!v1 || n == 36) {
                t->timescale = be32(h + (v1 ? 20 : 12));
                uint16_t lg = be16(h + (v1 ? 32 : 20));
                t->lang[0] = 0x60 + (lg >> 10 & 31); t->lang[1] = 0x60 + (lg >> 5 & 31); t->lang[2] = 0x60 + (lg & 31);
                if (lg == 0 || lg == 0x7FFF) memcpy(t->lang, "und", 4);
            }
        }
    }
    box edts, elst;
    if (find_child(s, trak, 0, FOURCC('e', 'd', 't', 's'), &edts) == 0 &&
        find_child(s, &edts, 0, FOURCC('e', 'l', 's', 't'), &elst) == 0) {
        uint8_t *d = box_load(s, &elst, 1 << 20);
        if (d && elst.size >= 8) {
            int v1 = d[0] == 1;
            uint32_t n = be32(d + 4);
            size_t es = v1 ? 20 : 12;
            for (uint32_t i = 0; i < n && 8 + (i + 1) * es <= (size_t)elst.size; i++) {
                const uint8_t *e = d + 8 + i * es;
                int64_t dur = v1 ? (int64_t)be64(e) : be32(e);
                int64_t mt = v1 ? (int64_t)be64(e + 8) : (int32_t)be32(e + 4);
                if (mt == -1) { if (movie_ts) t->elst_delay += dur * 1000000 / movie_ts; continue; }
                t->elst_shift = mt;
                break;
            }
        }
        free(d);
    }
    box minf, stbl;
    if (find_child(s, &mdia, 0, FOURCC('m', 'i', 'n', 'f'), &minf) != 0 ||
        find_child(s, &minf, 0, FOURCC('s', 't', 'b', 'l'), &stbl) != 0) return -1;
    parse_stbl(s, t, &stbl);
    return 0;
}

/* the sample table of t into a um_track */
static int build_samples(tk *t, um_track *ut)
{
    if (!t->stsz || !t->stco || !t->stsc || !t->stts || !t->timescale) return -1;
    uint32_t nsamples = be32(t->stsz + 8);
    uint32_t fixed = t->stz2 ? 0 : be32(t->stsz + 4);
    int fbits = t->stz2 ? t->stsz[7] : 32;
    if (t->stz2 && fbits != 4 && fbits != 8 && fbits != 16) return -1;
    if (!fixed) {
        size_t need = 12 + (t->stz2 ? ((size_t)nsamples * fbits + 7) / 8 : (size_t)nsamples * 4);
        if (need > t->stszn) return -1;
    }
    uint32_t nchunks = be32(t->stco + 4);
    if (8 + (size_t)nchunks * (t->co64 ? 8 : 4) > t->stcon) return -1;
    uint32_t nstsc = be32(t->stsc + 4);
    if (8 + (size_t)nstsc * 12 > t->stscn) return -1;
    uint32_t nstts = be32(t->stts + 4);
    if (8 + (size_t)nstts * 8 > t->sttsn) return -1;
    uint32_t nctts = t->ctts ? be32(t->ctts + 4) : 0;
    if (t->ctts && 8 + (size_t)nctts * 8 > t->cttsn) nctts = 0;
    int ctts_signed = t->ctts && t->ctts[0] == 1;
    uint32_t nstss = t->stss ? be32(t->stss + 4) : 0;
    if (t->stss && 8 + (size_t)nstss * 4 > t->stssn) nstss = 0;

    uint32_t si = 0, stts_i = 0, stts_left = nstts ? be32(t->stts + 8) : 0, ctts_i = 0, ctts_left = nctts ? be32(t->ctts + 8) : 0;
    uint32_t stss_i = 0;
    int64_t dts = 0;
    for (uint32_t c = 0; c < nchunks && si < nsamples; c++) {
        /* samples in chunk c+1: the last stsc entry whose first_chunk <= c+1 */
        uint32_t per = 0;
        for (uint32_t k = 0; k < nstsc; k++) {
            if (be32(t->stsc + 8 + k * 12) <= c + 1) per = be32(t->stsc + 8 + k * 12 + 4);
            else break;
        }
        int64_t off = t->co64 ? (int64_t)be64(t->stco + 8 + (size_t)c * 8) : be32(t->stco + 8 + (size_t)c * 4);
        for (uint32_t k = 0; k < per && si < nsamples; k++, si++) {
            uint32_t size = fixed;
            if (!fixed) {
                if (!t->stz2) size = be32(t->stsz + 12 + (size_t)si * 4);
                else if (fbits == 16) size = be16(t->stsz + 12 + (size_t)si * 2);
                else if (fbits == 8) size = t->stsz[12 + si];
                else size = (t->stsz[12 + si / 2] >> (si & 1 ? 0 : 4)) & 15;
            }
            int64_t cto = 0;
            if (nctts) {
                while (ctts_left == 0 && ++ctts_i < nctts) ctts_left = be32(t->ctts + 8 + (size_t)ctts_i * 8);
                if (ctts_i < nctts) {
                    uint32_t v = be32(t->ctts + 8 + (size_t)ctts_i * 8 + 4);
                    cto = ctts_signed ? (int32_t)v : (int64_t)v;
                    ctts_left--;
                }
            }
            uint32_t flags = UM_SAMPLE_SYNC;
            if (t->stss) {
                flags = 0;
                while (stss_i < nstss && be32(t->stss + 8 + (size_t)stss_i * 4) < si + 1) stss_i++;
                if (stss_i < nstss && be32(t->stss + 8 + (size_t)stss_i * 4) == si + 1) flags = UM_SAMPLE_SYNC;
            }
            int64_t pts = dts + cto - t->elst_shift;
            int64_t us = pts * 1000000 / (int64_t)t->timescale + t->elst_delay;
            if (um_track_add(ut, off, size, us, flags) != 0) return -1;
            off += size;
            while (stts_left == 0 && ++stts_i < nstts) stts_left = be32(t->stts + 8 + (size_t)stts_i * 8);
            if (stts_i < nstts) { dts += be32(t->stts + 8 + (size_t)stts_i * 8 + 4); stts_left--; }
        }
    }
    return ut->n > 0 ? 0 : -1;
}

static const int aac_rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

static int track_format(tk *t, um_track *ut, int64_t duration_us)
{
    um_format *f = ut->fmt;
    if (t->handler == FOURCC('v', 'i', 'd', 'e')) {
        if (t->codec == FOURCC('a', 'v', 'c', '1') || t->codec == FOURCC('a', 'v', 'c', '3')) {
            um_format_set_string(f, UM_KEY_MIME, UM_MIME_AVC);
            if (t->cfg) um_avcc_csd(f, t->cfg, t->cfgn, &t->nls);
        } else if (t->codec == FOURCC('h', 'v', 'c', '1') || t->codec == FOURCC('h', 'e', 'v', '1')) {
            um_format_set_string(f, UM_KEY_MIME, UM_MIME_HEVC);
            if (t->cfg) um_hvcc_csd(f, t->cfg, t->cfgn, &t->nls);
        } else return -1;
        um_format_set_int32(f, UM_KEY_WIDTH, t->width);
        um_format_set_int32(f, UM_KEY_HEIGHT, t->height);
        ut->nal_len = t->nls ? t->nls : 4;
    } else if (t->handler == FOURCC('s', 'o', 'u', 'n')) {
        if (t->codec == FOURCC('m', 'p', '4', 'a') && (t->aot == 0x40 || t->aot == 0x66 || t->aot == 0x67 || t->aot == 0x68)) {
            um_format_set_string(f, UM_KEY_MIME, UM_MIME_AAC);
            if (t->cfg && t->cfgn >= 2) {
                um_format_set_buffer(f, "csd-0", t->cfg, t->cfgn);
                int idx = (t->cfg[0] & 7) << 1 | t->cfg[1] >> 7, ch = (t->cfg[1] >> 3) & 15;
                if (idx < 13) t->rate = aac_rates[idx];
                if (ch > 0 && ch < 8) t->channels = ch == 7 ? 8 : ch;
            }
        } else if (t->codec == FOURCC('.', 'm', 'p', '3') || (t->codec == FOURCC('m', 'p', '4', 'a') && (t->aot == 0x6B || t->aot == 0x69))) {
            um_format_set_string(f, UM_KEY_MIME, UM_MIME_MPEG);
        } else return -1;
        um_format_set_int32(f, UM_KEY_SAMPLE_RATE, t->rate);
        um_format_set_int32(f, UM_KEY_CHANNEL_COUNT, t->channels ? t->channels : 2);
    } else return -1;
    if (duration_us > 0) um_format_set_int64(f, UM_KEY_DURATION, duration_us);
    um_format_set_string(f, UM_KEY_LANGUAGE, t->lang[0] ? t->lang : "und");
    return 0;
}

int um_mp4_open(um_source *src, um_demux *d)
{
    int64_t p = 0, end = src->size;
    box b, moov = { 0 };
    int have = 0;
    while (p < end && box_at(src, p, end, &b) == 0) {
        if (b.type == FOURCC('m', 'o', 'o', 'v')) { moov = b; have = 1; break; }
        p = b.data + b.size;
    }
    if (!have) { um_log("mp4: no moov box"); return UM_ERR_MALFORMED; }
    uint32_t movie_ts = 0;
    int64_t movie_dur_us = -1;
    if (find_child(src, &moov, 0, FOURCC('m', 'v', 'h', 'd'), &b) == 0 && b.size >= 20) {
        uint8_t h[32];
        size_t n = b.size >= 32 ? 32 : 20;
        if (um_source_read(src, b.data, h, n) == 0) {
            int v1 = h[0] == 1;
            movie_ts = be32(h + (v1 ? 20 : 12));
            uint64_t dur = v1 ? (n >= 32 ? be64(h + 24) : 0) : be32(h + 16);
            if (movie_ts) movie_dur_us = (int64_t)(dur * 1000000 / movie_ts);
        }
    }
    d->container = "mp4";
    d->file_fmt = um_format_new();
    um_format_set_string(d->file_fmt, UM_KEY_MIME, "video/mp4");
    if (movie_dur_us > 0) um_format_set_int64(d->file_fmt, UM_KEY_DURATION, movie_dur_us);
    p = moov.data;
    int64_t mend = moov.data + moov.size;
    int tid = 0;
    while (p < mend && box_at(src, p, mend, &b) == 0) {
        if (b.type == FOURCC('t', 'r', 'a', 'k')) {
            tk t;
            tid++;
            if (parse_trak(src, &b, movie_ts, &t) == 0 && (t.handler == FOURCC('v', 'i', 'd', 'e') || t.handler == FOURCC('s', 'o', 'u', 'n'))) {
                um_track *ut = um_demux_add_track(d);
                if (!ut) { tk_free(&t); return UM_ERR_UNKNOWN; }
                if (track_format(&t, ut, movie_dur_us) != 0 || build_samples(&t, ut) != 0) {
                    char cc[5] = { t.codec >> 24, t.codec >> 16, t.codec >> 8, t.codec, 0 };
                    um_log("mp4: track %d (%s) left out", tid, cc);
                    um_format_free(ut->fmt); free(ut->s);
                    d->nt--;
                } else {
                    um_format_set_int32(ut->fmt, UM_KEY_TRACK_ID, tid);
                    if (ut->nal_len && ut->nal_len < 4) ut->max_size = ut->max_size * 2 + 64;   /* start codes are longer */
                }
            }
            tk_free(&t);
        }
        p = b.data + b.size;
    }
    if (!d->nt) { um_log("mp4: no usable tracks"); return UM_ERR_UNSUPPORTED; }
    return UM_OK;
}
