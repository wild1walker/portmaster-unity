// WebM / Matroska demuxer: EBML header, Segment Info (timecode scale, duration), Tracks (codec,
// codec private data, video size and frame rate, audio rate and channels), and every block of
// every Cluster (SimpleBlock and BlockGroup, Xiph / EBML / fixed lacing) into the tracks' sample
// tables. Track formats follow Android's MatroskaExtractor: mime types, csd-0/csd-1 for Vorbis
// (identification and setup headers), csd-0/1/2 for Opus (OpusHead, codec delay and seek
// pre-roll in ns), csd-0/csd-1 for H.264 (SPS and PPS with start codes), csd-0 for AAC.
// Content encodings (compression, encryption) are not supported: such tracks are left out.
#define _GNU_SOURCE
#include "um_internal.h"

#include <stdlib.h>
#include <string.h>

enum {
    ID_EBML = 0x1A45DFA3, ID_DOCTYPE = 0x4282, ID_SEGMENT = 0x18538067, ID_INFO = 0x1549A966,
    ID_TIMECODESCALE = 0x2AD7B1, ID_DURATION = 0x4489, ID_TRACKS = 0x1654AE6B, ID_TRACKENTRY = 0xAE,
    ID_TRACKNUMBER = 0xD7, ID_TRACKTYPE = 0x83, ID_CODECID = 0x86, ID_CODECPRIVATE = 0x63A2,
    ID_DEFAULTDURATION = 0x23E383, ID_LANGUAGE = 0x22B59C, ID_CODECDELAY = 0x56AA, ID_SEEKPREROLL = 0x56BB,
    ID_VIDEO = 0xE0, ID_PIXELWIDTH = 0xB0, ID_PIXELHEIGHT = 0xBA, ID_FRAMERATE = 0x2383E3,
    ID_AUDIO = 0xE1, ID_SAMPLINGFREQ = 0xB5, ID_CHANNELS = 0x9F, ID_CONTENTENCODINGS = 0x6D80,
    ID_CLUSTER = 0x1F43B675, ID_TIMECODE = 0xE7, ID_SIMPLEBLOCK = 0xA3, ID_BLOCKGROUP = 0xA0,
    ID_BLOCK = 0xA1, ID_REFERENCEBLOCK = 0xFB, ID_CUES = 0x1C53BB6B, ID_TAGS = 0x1254C367,
};

/* ---- cached reads ------------------------------------------------------------------------------ */
typedef struct {
    um_source *src;
    uint8_t *buf;
    size_t cap;
    int64_t at;                 /* file offset of buf[0] */
    size_t len;
} rd_t;

static const uint8_t *rd(rd_t *r, int64_t pos, size_t n)
{
    if (pos < 0 || pos + (int64_t)n > r->src->size) return NULL;
    if (pos >= r->at && pos + (int64_t)n <= r->at + (int64_t)r->len) return r->buf + (pos - r->at);
    size_t want = n < 4096 ? 4096 : n;
    if (pos + (int64_t)want > r->src->size) want = r->src->size - pos;
    if (want > r->cap) {
        uint8_t *nb = realloc(r->buf, want);
        if (!nb) return NULL;
        r->buf = nb; r->cap = want;
    }
    if (um_source_read(r->src, pos, r->buf, want) != 0) { r->len = 0; return NULL; }
    r->at = pos; r->len = want;
    return r->buf;
}

/* EBML variable-size integer at p (keep_marker: element IDs keep their length bits). Returns its
   length, 0 if invalid. */
static int vint(const uint8_t *p, size_t avail, uint64_t *out, int keep_marker, int *all_ones)
{
    if (!avail || !p[0]) return 0;
    int len = 1;
    uint8_t m = 0x80;
    while (!(p[0] & m)) { m >>= 1; len++; }
    if ((size_t)len > avail || len > 8) return 0;
    uint64_t v = keep_marker ? p[0] : (p[0] & (m - 1));
    int ones = (p[0] & (m - 1)) == (m - 1);
    for (int i = 1; i < len; i++) { v = (v << 8) | p[i]; if (p[i] != 0xFF) ones = 0; }
    if (all_ones) *all_ones = ones;
    *out = v;
    return len;
}

typedef struct { uint32_t id; int64_t data; int64_t size; int unknown; } elem;

/* the element header at pos (within [pos, end)) */
static int elem_at(rd_t *r, int64_t pos, int64_t end, elem *e)
{
    size_t avail = end - pos < 12 ? (size_t)(end - pos) : 12;
    if (avail < 2) return -1;
    const uint8_t *p = rd(r, pos, avail);
    if (!p) return -1;
    uint64_t id, size;
    int ones = 0;
    int a = vint(p, avail, &id, 1, NULL);
    if (!a || a > 4) return -1;
    int b = vint(p + a, avail - a, &size, 0, &ones);
    if (!b) return -1;
    e->id = (uint32_t)id;
    e->data = pos + a + b;
    e->unknown = ones;
    e->size = ones ? end - e->data : (int64_t)size;
    if (e->data + e->size > end) {
        if (e->id == ID_SEGMENT || e->id == ID_CLUSTER) e->size = end - e->data;   /* truncated file */
        else return -1;
    }
    return 0;
}

static uint64_t read_uint(rd_t *r, elem *e)
{
    if (e->size > 8 || e->size < 0) return 0;
    const uint8_t *p = rd(r, e->data, e->size);
    uint64_t v = 0;
    for (int i = 0; p && i < e->size; i++) v = (v << 8) | p[i];
    return v;
}

static double read_float(rd_t *r, elem *e)
{
    const uint8_t *p = rd(r, e->data, e->size);
    if (!p) return 0;
    if (e->size == 4) {
        uint32_t u = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
        float f; memcpy(&f, &u, 4); return f;
    }
    if (e->size == 8) {
        uint64_t u = 0;
        for (int i = 0; i < 8; i++) u = (u << 8) | p[i];
        double d; memcpy(&d, &u, 8); return d;
    }
    return 0;
}

static char *read_str(rd_t *r, elem *e)
{
    if (e->size < 0 || e->size > 4096) return NULL;
    const uint8_t *p = rd(r, e->data, e->size);
    char *s = malloc(e->size + 1);
    if (!s) return NULL;
    if (p) memcpy(s, p, e->size);
    s[p ? e->size : 0] = 0;
    return s;
}

/* ---- tracks ------------------------------------------------------------------------------------ */
typedef struct {
    uint64_t number, type, default_duration, codec_delay, seek_preroll;
    char *codec, *lang;
    uint8_t *priv;
    size_t priv_n;
    uint64_t width, height, channels;
    double rate, fps;
    int encoded;
} tinfo;

/* Xiph-laced CodecPrivate (Vorbis): n-1, sizes, then the packets */
static int xiph_split(const uint8_t *p, size_t n, const uint8_t **pk, size_t *pn, int maxpk)
{
    if (n < 1) return -1;
    int count = p[0] + 1;
    if (count > maxpk) return -1;
    size_t at = 1, total = 0;
    for (int i = 0; i < count - 1; i++) {
        size_t s = 0;
        while (at < n && p[at] == 255) { s += 255; at++; }
        if (at >= n) return -1;
        s += p[at++];
        pn[i] = s; total += s;
    }
    if (at + total > n) return -1;
    pn[count - 1] = n - at - total;
    for (int i = 0; i < count; i++) { pk[i] = p + at; at += pn[i]; }
    return count;
}

static void put_le64(uint8_t *b, uint64_t v) { for (int i = 0; i < 8; i++) b[i] = v >> (8 * i); }

static int track_format(um_demux *d, tinfo *ti, int64_t duration_us, int *map_index)
{
    const char *c = ti->codec ? ti->codec : "";
    const char *mime = NULL;
    if (!strcmp(c, "V_VP8")) mime = UM_MIME_VP8;
    else if (!strcmp(c, "V_VP9")) mime = UM_MIME_VP9;
    else if (!strcmp(c, "V_MPEG4/ISO/AVC")) mime = UM_MIME_AVC;
    else if (!strcmp(c, "V_MPEGH/ISO/HEVC")) mime = UM_MIME_HEVC;
    else if (!strcmp(c, "A_VORBIS")) mime = UM_MIME_VORBIS;
    else if (!strcmp(c, "A_OPUS")) mime = UM_MIME_OPUS;
    else if (!strncmp(c, "A_AAC", 5)) mime = UM_MIME_AAC;
    else if (!strcmp(c, "A_MPEG/L3")) mime = UM_MIME_MPEG;
    if (!mime || ti->encoded) {
        um_log("webm: track %llu (%s) left out: %s", (unsigned long long)ti->number, c,
               ti->encoded ? "content encoding (compressed or encrypted)" : "codec not handled");
        *map_index = -1;
        return 0;
    }
    um_track *t = um_demux_add_track(d);
    if (!t) return -1;
    *map_index = d->nt - 1;
    um_format *f = t->fmt;
    um_format_set_string(f, UM_KEY_MIME, mime);
    um_format_set_int32(f, UM_KEY_TRACK_ID, (int32_t)ti->number);
    if (duration_us > 0) um_format_set_int64(f, UM_KEY_DURATION, duration_us);
    um_format_set_string(f, UM_KEY_LANGUAGE, ti->lang && *ti->lang ? ti->lang : "und");
    if (!strncmp(mime, "video/", 6)) {
        um_format_set_int32(f, UM_KEY_WIDTH, (int32_t)ti->width);
        um_format_set_int32(f, UM_KEY_HEIGHT, (int32_t)ti->height);
        if (ti->fps > 0) um_format_set_float(f, UM_KEY_FRAME_RATE, (float)ti->fps);
        else if (ti->default_duration) um_format_set_float(f, UM_KEY_FRAME_RATE, (float)(1e9 / ti->default_duration));
        if (!strcmp(mime, UM_MIME_AVC) && ti->priv) {
            uint8_t *ab; size_t an; int nls;
            if (um_avcc_to_annexb(ti->priv, ti->priv_n, &ab, &an, &nls) == 0) {
                um_format_set_buffer(f, "csd-0", ab, an);     /* SPS + PPS with start codes */
                um_format_set_int32(f, "x-nal-length-size", nls);
                free(ab);
            }
        } else if (!strcmp(mime, UM_MIME_HEVC) && ti->priv) {
            um_format_set_buffer(f, "csd-hvcc", ti->priv, ti->priv_n);
        }
    } else {
        um_format_set_int32(f, UM_KEY_SAMPLE_RATE, (int32_t)(ti->rate + 0.5));
        um_format_set_int32(f, UM_KEY_CHANNEL_COUNT, (int32_t)(ti->channels ? ti->channels : 1));
        if (!strcmp(mime, UM_MIME_VORBIS)) {
            const uint8_t *pk[3]; size_t pn[3];
            if (!ti->priv || xiph_split(ti->priv, ti->priv_n, pk, pn, 3) != 3) {
                um_log("webm: Vorbis track %llu without its three headers", (unsigned long long)ti->number);
                d->nt--; um_format_free(t->fmt); *map_index = -1;
                return 0;
            }
            um_format_set_buffer(f, "csd-0", pk[0], pn[0]);   /* identification */
            um_format_set_buffer(f, "csd-1", pk[2], pn[2]);   /* setup (Android leaves the comments out) */
        } else if (!strcmp(mime, UM_MIME_OPUS)) {
            uint8_t b[8];
            if (ti->priv) um_format_set_buffer(f, "csd-0", ti->priv, ti->priv_n);
            put_le64(b, ti->codec_delay); um_format_set_buffer(f, "csd-1", b, 8);
            put_le64(b, ti->seek_preroll); um_format_set_buffer(f, "csd-2", b, 8);
        } else if (!strcmp(mime, UM_MIME_AAC) && ti->priv) {
            um_format_set_buffer(f, "csd-0", ti->priv, ti->priv_n);
        }
    }
    return 0;
}

static int parse_track(rd_t *r, elem *te, tinfo *ti)
{
    memset(ti, 0, sizeof *ti);
    int64_t p = te->data, end = te->data + te->size;
    elem e;
    while (p < end && elem_at(r, p, end, &e) == 0) {
        switch (e.id) {
        case ID_TRACKNUMBER: ti->number = read_uint(r, &e); break;
        case ID_TRACKTYPE: ti->type = read_uint(r, &e); break;
        case ID_CODECID: free(ti->codec); ti->codec = read_str(r, &e); break;
        case ID_LANGUAGE: free(ti->lang); ti->lang = read_str(r, &e); break;
        case ID_DEFAULTDURATION: ti->default_duration = read_uint(r, &e); break;
        case ID_CODECDELAY: ti->codec_delay = read_uint(r, &e); break;
        case ID_SEEKPREROLL: ti->seek_preroll = read_uint(r, &e); break;
        case ID_CONTENTENCODINGS: ti->encoded = 1; break;
        case ID_CODECPRIVATE:
            if (e.size > 0 && e.size < (1 << 24)) {
                const uint8_t *q = rd(r, e.data, e.size);
                free(ti->priv);
                ti->priv = q ? malloc(e.size) : NULL;
                if (ti->priv) { memcpy(ti->priv, q, e.size); ti->priv_n = e.size; }
            }
            break;
        case ID_VIDEO: case ID_AUDIO: {
            int64_t q = e.data, qe = e.data + e.size;
            elem c;
            while (q < qe && elem_at(r, q, qe, &c) == 0) {
                if (c.id == ID_PIXELWIDTH) ti->width = read_uint(r, &c);
                else if (c.id == ID_PIXELHEIGHT) ti->height = read_uint(r, &c);
                else if (c.id == ID_FRAMERATE) ti->fps = read_float(r, &c);
                else if (c.id == ID_SAMPLINGFREQ) ti->rate = read_float(r, &c);
                else if (c.id == ID_CHANNELS) ti->channels = read_uint(r, &c);
                q = c.data + c.size;
            }
            break;
        }
        }
        p = e.data + e.size;
    }
    if (!ti->rate) ti->rate = 8000;      /* Matroska's default */
    return 0;
}

static void free_tinfo(tinfo *ti) { free(ti->codec); free(ti->lang); free(ti->priv); }

/* ---- blocks ------------------------------------------------------------------------------------- */
typedef struct {
    um_demux *d;
    int *map;                   /* track number -> track index (or -1) */
    int nmap;
    uint64_t scale;             /* ns per timecode unit */
    int bad;
} ctx_t;

static void add_block(rd_t *r, ctx_t *x, int64_t pos, int64_t size, int64_t cluster_tc, int simple, int key_if_block)
{
    if (size < 4) return;
    size_t hn = size < 4096 ? (size_t)size : 4096;
    const uint8_t *p = rd(r, pos, hn);
    if (!p) return;
    uint64_t tn;
    int a = vint(p, hn, &tn, 0, NULL);
    if (!a || (size_t)a + 3 > hn) return;
    if (tn >= (uint64_t)x->nmap || x->map[tn] < 0) return;
    um_track *t = &x->d->t[x->map[tn]];
    int16_t rel = (int16_t)(p[a] << 8 | p[a + 1]);
    uint8_t fl = p[a + 2];
    int64_t tc = cluster_tc + rel;
    int64_t us = (int64_t)((double)tc * (double)x->scale / 1000.0 + (tc >= 0 ? 0.5 : -0.5));
    uint32_t sf = (simple ? (fl & 0x80) : key_if_block) ? UM_SAMPLE_SYNC : 0;
    int lacing = (fl >> 1) & 3;
    size_t at = a + 3;
    if (!lacing) {
        um_track_add(t, pos + at, (uint32_t)(size - at), us, sf);
        return;
    }
    if (at >= hn) { x->bad++; return; }
    int count = p[at++] + 1;
    uint32_t sizes[256];
    int64_t total = 0;
    if (lacing == 2) {                                  /* fixed */
        int64_t each = (size - at) / count;
        for (int i = 0; i < count; i++) sizes[i] = (uint32_t)each;
        total = each * count;
    } else if (lacing == 1) {                           /* Xiph */
        for (int i = 0; i < count - 1; i++) {
            uint32_t s = 0;
            while (at < hn && p[at] == 255) { s += 255; at++; }
            if (at >= hn) { x->bad++; return; }
            s += p[at++];
            sizes[i] = s; total += s;
        }
        sizes[count - 1] = (uint32_t)(size - at - total);
    } else {                                            /* EBML: first size, then signed differences */
        uint64_t v;
        int n = vint(p + at, hn - at, &v, 0, NULL);
        if (!n) { x->bad++; return; }
        at += n; sizes[0] = (uint32_t)v; total = v;
        for (int i = 1; i < count - 1; i++) {
            n = vint(p + at, hn - at, &v, 0, NULL);
            if (!n) { x->bad++; return; }
            int64_t diff = (int64_t)v - (((int64_t)1 << (7 * n - 1)) - 1);
            at += n;
            int64_t s = (int64_t)sizes[i - 1] + diff;
            if (s < 0) { x->bad++; return; }
            sizes[i] = (uint32_t)s; total += s;
        }
        sizes[count - 1] = (uint32_t)(size - at - total);
    }
    if ((int64_t)at + total > size) { x->bad++; return; }
    int64_t off = pos + at;
    for (int i = 0; i < count; i++) {
        um_track_add(t, off, sizes[i], us, sf);
        off += sizes[i];
    }
}

static void parse_cluster(rd_t *r, ctx_t *x, elem *ce)
{
    int64_t p = ce->data, end = ce->data + ce->size, tc = 0;
    elem e;
    while (p < end && elem_at(r, p, end, &e) == 0) {
        if (ce->unknown && (e.id == ID_CLUSTER || e.id == ID_CUES || e.id == ID_TAGS)) break;
        if (e.id == ID_TIMECODE) tc = (int64_t)read_uint(r, &e);
        else if (e.id == ID_SIMPLEBLOCK) add_block(r, x, e.data, e.size, tc, 1, 0);
        else if (e.id == ID_BLOCKGROUP) {
            int64_t q = e.data, qe = e.data + e.size, bpos = -1, bsize = 0;
            int ref = 0;
            elem c;
            while (q < qe && elem_at(r, q, qe, &c) == 0) {
                if (c.id == ID_BLOCK) { bpos = c.data; bsize = c.size; }
                else if (c.id == ID_REFERENCEBLOCK) ref = 1;
                q = c.data + c.size;
            }
            if (bpos >= 0) add_block(r, x, bpos, bsize, tc, 0, !ref);
        }
        p = e.data + e.size;
    }
    if (ce->unknown) ce->size = p - ce->data;
}

int um_webm_open(um_source *src, um_demux *d)
{
    rd_t r = { .src = src };
    elem e;
    int64_t end = src->size;
    int ret = UM_ERR_MALFORMED;
    tinfo tis[32];
    int ntis = 0;
    if (elem_at(&r, 0, end, &e) != 0 || e.id != ID_EBML) goto out;
    {
        int64_t p = e.data, pe = e.data + e.size;
        elem c;
        char *doctype = NULL;
        while (p < pe && elem_at(&r, p, pe, &c) == 0) {
            if (c.id == ID_DOCTYPE) { free(doctype); doctype = read_str(&r, &c); }
            p = c.data + c.size;
        }
        if (doctype && strcmp(doctype, "webm") && strcmp(doctype, "matroska")) {
            um_log("webm: doc type %s", doctype);
            free(doctype);
            ret = UM_ERR_UNSUPPORTED;
            goto out;
        }
        free(doctype);
    }
    int64_t p = e.data + e.size;
    if (elem_at(&r, p, end, &e) != 0 || e.id != ID_SEGMENT) goto out;
    d->container = "webm";
    d->file_fmt = um_format_new();
    um_format_set_string(d->file_fmt, UM_KEY_MIME, "video/webm");
    uint64_t scale = 1000000;
    double duration = 0;
    ctx_t x = { .d = d };
    int64_t sp = e.data, se = e.data + e.size;
    int tracks_done = 0;
    while (sp < se && elem_at(&r, sp, se, &e) == 0) {
        if (e.id == ID_INFO) {
            int64_t q = e.data, qe = e.data + e.size;
            elem c;
            while (q < qe && elem_at(&r, q, qe, &c) == 0) {
                if (c.id == ID_TIMECODESCALE) scale = read_uint(&r, &c);
                else if (c.id == ID_DURATION) duration = read_float(&r, &c);
                q = c.data + c.size;
            }
        } else if (e.id == ID_TRACKS) {
            int64_t q = e.data, qe = e.data + e.size;
            elem c;
            while (q < qe && elem_at(&r, q, qe, &c) == 0) {
                if (c.id == ID_TRACKENTRY && ntis < 32) parse_track(&r, &c, &tis[ntis++]);
                q = c.data + c.size;
            }
        } else if (e.id == ID_CLUSTER) {
            if (!tracks_done) {
                /* tracks are known once the first cluster starts */
                if (!scale) scale = 1000000;
                int64_t dur_us = duration > 0 ? (int64_t)(duration * scale / 1000.0 + 0.5) : -1;
                if (dur_us > 0) um_format_set_int64(d->file_fmt, UM_KEY_DURATION, dur_us);
                uint64_t maxn = 0;
                for (int i = 0; i < ntis; i++) if (tis[i].number > maxn) maxn = tis[i].number;
                x.nmap = (int)(maxn < 1024 ? maxn + 1 : 1024);
                x.map = malloc(x.nmap * sizeof(int));
                if (!x.map) goto out;
                for (int i = 0; i < x.nmap; i++) x.map[i] = -1;
                for (int i = 0; i < ntis; i++) {
                    int idx = -1;
                    if (tis[i].number < (uint64_t)x.nmap && track_format(d, &tis[i], dur_us, &idx) == 0)
                        x.map[tis[i].number] = idx;
                }
                x.scale = scale;
                tracks_done = 1;
            }
            parse_cluster(&r, &x, &e);
        }
        sp = e.data + e.size;
    }
    if (x.bad) um_log("webm: %d blocks with bad lacing skipped", x.bad);
    free(x.map);
    ret = d->nt > 0 ? UM_OK : UM_ERR_UNSUPPORTED;
    if (ret != UM_OK) um_log("webm: no usable tracks");
out:
    for (int i = 0; i < ntis; i++) free_tinfo(&tis[i]);
    free(r.buf);
    return ret;
}
