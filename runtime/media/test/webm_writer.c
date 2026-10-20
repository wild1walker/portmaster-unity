// A minimal WebM writer for the tests: EBML header, Segment (Info, Tracks, Clusters of
// SimpleBlocks, optionally laced), all sizes known. Only what the tests need.
#include "webm_writer.h"

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
static void put8(wb *b, unsigned v) { uint8_t c = (uint8_t)v; put(b, &c, 1); }
static void put_id(wb *b, uint32_t id)
{
    if (id > 0xFFFFFF) put8(b, id >> 24);
    if (id > 0xFFFF) put8(b, id >> 16);
    if (id > 0xFF) put8(b, id >> 8);
    put8(b, id);
}
static void put_size(wb *b, uint64_t s)      /* always 8 bytes: simple */
{
    put8(b, 0x01);
    for (int i = 6; i >= 0; i--) put8(b, s >> (8 * i));
}
void wb_elem(wb *b, uint32_t id, const void *data, size_t n) { put_id(b, id); put_size(b, n); put(b, data, n); }
void wb_uint(wb *b, uint32_t id, uint64_t v)
{
    uint8_t t[8];
    int n = 8;
    for (int i = 0; i < 8; i++) t[i] = v >> (8 * (7 - i));
    int s = 0;
    while (s < 7 && !t[s]) s++;
    n -= s;
    wb_elem(b, id, t + s, n);
}
void wb_float(wb *b, uint32_t id, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    uint8_t t[8];
    for (int i = 0; i < 8; i++) t[i] = u >> (8 * (7 - i));
    wb_elem(b, id, t, 8);
}
void wb_float4(wb *b, uint32_t id, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    uint8_t t[4] = { u >> 24, u >> 16, u >> 8, u };
    wb_elem(b, id, t, 4);
}
void wb_str(wb *b, uint32_t id, const char *s) { wb_elem(b, id, s, strlen(s)); }
void wb_master(wb *b, uint32_t id, wb *child) { wb_elem(b, id, child->p, child->n); wb_free(child); }
void wb_free(wb *b) { free(b->p); memset(b, 0, sizeof *b); }

/* one SimpleBlock: track (1-byte vint), relative time, flags, lacing of frames */
void wb_block(wb *cluster, int track, int rel, int key, const uint8_t *const *frames, const size_t *sizes, int n, int lacing)
{
    wb blk = { 0 };
    put8(&blk, 0x80 | track);
    put8(&blk, (rel >> 8) & 0xFF);
    put8(&blk, rel & 0xFF);
    put8(&blk, (key ? 0x80 : 0) | (n > 1 ? lacing << 1 : 0));
    if (n > 1) {
        put8(&blk, n - 1);
        if (lacing == 1) {                          /* Xiph */
            for (int i = 0; i < n - 1; i++) {
                size_t s = sizes[i];
                while (s >= 255) { put8(&blk, 255); s -= 255; }
                put8(&blk, s);
            }
        } else if (lacing == 3) {                   /* EBML: first size, then signed differences */
            uint64_t s0 = sizes[0];
            put8(&blk, 0x40 | (s0 >> 8)); put8(&blk, s0 & 0xFF);     /* 2-byte vint */
            for (int i = 1; i < n - 1; i++) {
                int64_t d = (int64_t)sizes[i] - (int64_t)sizes[i - 1];
                uint64_t v = (uint64_t)(d + 8191);   /* 2-byte signed: bias 2^13 - 1 */
                put8(&blk, 0x40 | (v >> 8)); put8(&blk, v & 0xFF);
            }
        }
    }
    for (int i = 0; i < n; i++) put(&blk, frames[i], sizes[i]);
    wb_elem(cluster, 0xA3, blk.p, blk.n);
    wb_free(&blk);
}

void wb_header(wb *out)
{
    wb h = { 0 };
    wb_uint(&h, 0x4286, 1); wb_uint(&h, 0x42F7, 1); wb_uint(&h, 0x42F2, 4); wb_uint(&h, 0x42F3, 8);
    wb_str(&h, 0x4282, "webm"); wb_uint(&h, 0x4287, 2); wb_uint(&h, 0x4285, 2);
    wb_master(out, 0x1A45DFA3, &h);
}

/* Xiph-laced CodecPrivate from n packets */
void wb_xiph_private(wb *out, const uint8_t *const *pk, const size_t *n, int count)
{
    put8(out, count - 1);
    for (int i = 0; i < count - 1; i++) {
        size_t s = n[i];
        while (s >= 255) { put8(out, 255); s -= 255; }
        put8(out, s);
    }
    for (int i = 0; i < count; i++) put(out, pk[i], n[i]);
}
