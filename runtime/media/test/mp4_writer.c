// A minimal MP4 writer for the tests: ftyp, mdat (samples, tracks interleaved), moov with one
// trak per track (tkhd, edts/elst when asked, mdia: mdhd, hdlr, minf: vmhd/smhd, dinf, stbl: stsd
// (avc1 + avcC, or mp4a + esds), stts, ctts when given, stsc, stsz, stco, stss).
#include "mp4_writer.h"

#include <stdlib.h>
#include <string.h>

static void put(wb *b, const void *p, size_t n)
{
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + n) c *= 2;
        b->p = realloc(b->p, c);
        b->cap = c;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}
static void u8(wb *b, unsigned v) { uint8_t c = v; put(b, &c, 1); }
static void u16(wb *b, unsigned v) { u8(b, v >> 8); u8(b, v); }
static void u32(wb *b, uint32_t v) { u16(b, v >> 16); u16(b, v & 0xFFFF); }
static void zeros(wb *b, int n) { while (n--) u8(b, 0); }
static void box(wb *b, const char *type, wb *payload)
{
    u32(b, 8 + payload->n);
    put(b, type, 4);
    put(b, payload->p, payload->n);
    free(payload->p);
    memset(payload, 0, sizeof *payload);
}
static void fullbox_head(wb *b, int ver, uint32_t flags) { u8(b, ver); u8(b, flags >> 16); u16(b, flags & 0xFFFF); }

static void stbl(wb *out, const mp4_track *t, const uint32_t *chunk_off, const int *chunk_n, int nchunks)
{
    wb s = { 0 }, x = { 0 }, e = { 0 }, c = { 0 };
    /* stsd */
    fullbox_head(&x, 0, 0); u32(&x, 1);
    if (t->video) {
        zeros(&e, 6); u16(&e, 1);                   /* reserved, data reference index */
        zeros(&e, 16); u16(&e, t->w); u16(&e, t->h);
        u32(&e, 0x00480000); u32(&e, 0x00480000); u32(&e, 0); u16(&e, 1);
        zeros(&e, 32); u16(&e, 24); u16(&e, 0xFFFF);
        put(&c, t->cfg, t->cfgn);
        box(&e, "avcC", &c);
        box(&x, "avc1", &e);
    } else {
        zeros(&e, 6); u16(&e, 1);
        zeros(&e, 8); u16(&e, t->ch); u16(&e, 16); u32(&e, 0); u32(&e, (uint32_t)t->rate << 16);
        /* esds: ES_Descriptor(3) { DecoderConfigDescriptor(4) { DecoderSpecificInfo(5) }, SLConfig(6) } */
        wb d = { 0 };
        fullbox_head(&d, 0, 0);
        u8(&d, 3); u8(&d, (uint8_t)(23 + t->cfgn)); u16(&d, 1); u8(&d, 0);
        u8(&d, 4); u8(&d, (uint8_t)(15 + t->cfgn)); u8(&d, 0x40); u8(&d, 0x15); zeros(&d, 3); u32(&d, 128000); u32(&d, 128000);
        u8(&d, 5); u8(&d, (uint8_t)t->cfgn); put(&d, t->cfg, t->cfgn);
        u8(&d, 6); u8(&d, 1); u8(&d, 2);
        box(&e, "esds", &d);
        box(&x, "mp4a", &e);
    }
    box(&s, "stsd", &x);
    /* stts: one entry per sample (simple) */
    fullbox_head(&x, 0, 0); u32(&x, t->n);
    for (int i = 0; i < t->n; i++) { u32(&x, 1); u32(&x, (uint32_t)(i + 1 < t->n ? t->dts[i + 1] - t->dts[i] : (i ? t->dts[i] - t->dts[i - 1] : 1))); }
    box(&s, "stts", &x);
    if (t->cto) {
        fullbox_head(&x, 0, 0); u32(&x, t->n);
        for (int i = 0; i < t->n; i++) { u32(&x, 1); u32(&x, (uint32_t)t->cto[i]); }
        box(&s, "ctts", &x);
    }
    fullbox_head(&x, 0, 0); u32(&x, nchunks);
    for (int i = 0; i < nchunks; i++) { u32(&x, i + 1); u32(&x, chunk_n[i]); u32(&x, 1); }
    box(&s, "stsc", &x);
    fullbox_head(&x, 0, 0); u32(&x, 0); u32(&x, t->n);
    for (int i = 0; i < t->n; i++) u32(&x, (uint32_t)t->size[i]);
    box(&s, "stsz", &x);
    fullbox_head(&x, 0, 0); u32(&x, nchunks);
    for (int i = 0; i < nchunks; i++) u32(&x, chunk_off[i]);
    box(&s, "stco", &x);
    if (t->video) {
        int nk = 0;
        for (int i = 0; i < t->n; i++) nk += t->key[i];
        fullbox_head(&x, 0, 0); u32(&x, nk);
        for (int i = 0; i < t->n; i++) if (t->key[i]) u32(&x, i + 1);
        box(&s, "stss", &x);
    }
    box(out, "stbl", &s);
}

wb mux_mp4(mp4_track *t, int nt)
{
    wb out = { 0 }, x = { 0 };
    put(&x, "isom", 4); u32(&x, 512); put(&x, "isomiso2avc1mp41", 16);
    box(&out, "ftyp", &x);
    /* mdat: each track's samples in chunks of 5, tracks alternating */
    uint32_t *coff[4]; int *cn[4], nch[4] = { 0 };
    wb md = { 0 };
    size_t base = out.n + 8;
    int pos[4] = { 0 };
    for (int i = 0; i < nt; i++) { coff[i] = calloc(t[i].n / 5 + 2, sizeof(uint32_t)); cn[i] = calloc(t[i].n / 5 + 2, sizeof(int)); }
    for (int more = 1; more; ) {
        more = 0;
        for (int i = 0; i < nt; i++) {
            if (pos[i] >= t[i].n) continue;
            coff[i][nch[i]] = (uint32_t)(base + md.n);
            int k = 0;
            for (; k < 5 && pos[i] < t[i].n; k++, pos[i]++) put(&md, t[i].data[pos[i]], t[i].size[pos[i]]);
            cn[i][nch[i]++] = k;
            more = 1;
        }
    }
    box(&out, "mdat", &md);
    wb moov = { 0 };
    fullbox_head(&x, 0, 0); u32(&x, 0); u32(&x, 0); u32(&x, 1000); u32(&x, (uint32_t)t[0].duration_ms);
    u32(&x, 0x00010000); u16(&x, 0x0100); zeros(&x, 10);
    u32(&x, 0x00010000); zeros(&x, 12); u32(&x, 0x00010000); zeros(&x, 12); u32(&x, 0x40000000);
    zeros(&x, 24); u32(&x, nt + 1);
    box(&moov, "mvhd", &x);
    for (int i = 0; i < nt; i++) {
        wb trak = { 0 }, mdia = { 0 }, minf = { 0 }, y = { 0 };
        fullbox_head(&x, 0, 7); u32(&x, 0); u32(&x, 0); u32(&x, i + 1); u32(&x, 0); u32(&x, (uint32_t)t[i].duration_ms);
        zeros(&x, 8); u16(&x, 0); u16(&x, 0); u16(&x, t[i].video ? 0 : 0x0100); u16(&x, 0);
        u32(&x, 0x00010000); zeros(&x, 12); u32(&x, 0x00010000); zeros(&x, 12); u32(&x, 0x40000000);
        u32(&x, (uint32_t)t[i].w << 16); u32(&x, (uint32_t)t[i].h << 16);
        box(&trak, "tkhd", &x);
        if (t[i].elst_media_time) {
            wb el = { 0 };
            fullbox_head(&el, 0, 0); u32(&el, 1); u32(&el, (uint32_t)t[i].duration_ms); u32(&el, (uint32_t)t[i].elst_media_time); u32(&el, 0x00010000);
            box(&y, "elst", &el);
            box(&trak, "edts", &y);
        }
        fullbox_head(&x, 0, 0); u32(&x, 0); u32(&x, 0); u32(&x, t[i].timescale);
        u32(&x, (uint32_t)(t[i].duration_ms * t[i].timescale / 1000)); u16(&x, 0x55C4); u16(&x, 0);   /* "und" */
        box(&mdia, "mdhd", &x);
        fullbox_head(&x, 0, 0); u32(&x, 0); put(&x, t[i].video ? "vide" : "soun", 4); zeros(&x, 12); put(&x, "test", 5);
        box(&mdia, "hdlr", &x);
        if (t[i].video) { fullbox_head(&x, 0, 1); zeros(&x, 8); box(&minf, "vmhd", &x); }
        else { fullbox_head(&x, 0, 0); zeros(&x, 4); box(&minf, "smhd", &x); }
        wb dref = { 0 }, url = { 0 };
        fullbox_head(&dref, 0, 0); u32(&dref, 1);
        fullbox_head(&url, 0, 1);
        box(&dref, "url ", &url);
        box(&y, "dref", &dref);
        box(&minf, "dinf", &y);
        stbl(&minf, &t[i], coff[i], cn[i], nch[i]);
        box(&mdia, "minf", &minf);
        box(&trak, "mdia", &mdia);
        box(&moov, "trak", &trak);
        free(coff[i]); free(cn[i]);
    }
    box(&out, "moov", &moov);
    return out;
}
