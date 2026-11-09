// A stand-in libpmhwdec.so.1 for test_pmhwdec: the version 1 API (pmhwdec.h, MIT) without hardware.
// Each access unit with a slice "decodes" to the test picture of its frame (the pattern functions
// test_pmhwdec hands over with fake_pmhwdec_setup), drawn into NV12 planes with a 2-pixel border and
// the visible area at (2, 2), so the caller's crop and NV12 handling are checked exactly.
// A picture is ready one unit later (a reorder delay; the last at end of stream), and every
// fifth unit with a slice is first turned away with PMHWDEC_AGAIN, so the caller's retry runs.
//   FAKE_PMHWDEC=nohw     pmhwdec_query says there is no decoder
//   FAKE_PMHWDEC=refuse   a unit carrying an SPS gets PMHWDEC_UNSUPPORTED (refused at configure)
//   FAKE_PMHWDEC=midway   the 11th unit with a slice (an IDR in test_pmhwdec's stream) gets PMHWDEC_UNSUPPORTED
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int max_width, max_height, max_bit_depth; const char *backend; } PmhwdecCaps;
typedef struct {
    int width, height, left, top, format;
    const uint8_t *plane[2];
    int stride[2];
    int64_t pts;
    int corrupt;
    int dmabuf_fd;
    uint32_t drm_fourcc;
    uint64_t drm_modifier;
    uint32_t offset[2];
    void *priv;
} PmhwdecPicture;

typedef struct {
    int sends, again, refused, pictures, held_max, sps_before_first_slice, slices;
} fake_stats_t;
fake_stats_t fake_pmhwdec_stats;

static uint8_t (*py)(int, int, int), (*pu)(int, int, int), (*pv)(int, int, int);
void fake_pmhwdec_setup(uint8_t (*y)(int, int, int), uint8_t (*u)(int, int, int), uint8_t (*v)(int, int, int))
{
    py = y; pu = u; pv = v;
}

#define B 2                               /* border around the visible picture */
typedef struct PmhwdecDecoder {
    int w, h;
    int64_t q[4];                         /* queued pictures' pts */
    int qn, out, seen_sps, eos, bounced, vp8;
} PmhwdecDecoder;

static const char *mode(void) { const char *m = getenv("FAKE_PMHWDEC"); return m ? m : ""; }

int pmhwdec_query(int codec, PmhwdecCaps *caps)
{
    if (!strcmp(mode(), "nohw") || (codec > 1 && codec != 3)) return -1;
    caps->max_width = 4096; caps->max_height = 2304; caps->max_bit_depth = 8; caps->backend = "fake";
    return 0;
}

PmhwdecDecoder *pmhwdec_open(int codec, int width, int height)
{
    if (width <= 0 || height <= 0) return NULL;
    PmhwdecDecoder *d = calloc(1, sizeof *d);
    if (d) { d->w = width; d->h = height; d->vp8 = codec == 3; }
    return d;
}

/* NAL unit types after start codes */
static void scan(const uint8_t *p, int n, int *sps, int *slice)
{
    *sps = *slice = 0;
    for (int i = 0; i + 3 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            int t = p[i + 3] & 31;
            if (t == 7) *sps = 1;
            if (t == 1 || t == 5) *slice = 1;
        }
}

int pmhwdec_send(PmhwdecDecoder *d, const uint8_t *data, int size, int64_t pts)
{
    fake_pmhwdec_stats.sends++;
    if (!data || !size) { d->eos = 1; return 0; }
    int sps, slice;
    scan(data, size, &sps, &slice);
    if (d->vp8) slice = 1;                          /* no start codes: one frame per unit */
    if (sps) d->seen_sps = 1;
    if (sps && !strcmp(mode(), "refuse")) { fake_pmhwdec_stats.refused++; return -2; }
    if (!slice) return 0;
    if (d->qn >= 4) return 1;
    if (fake_pmhwdec_stats.slices % 5 == 4 && !d->bounced) { d->bounced = 1; fake_pmhwdec_stats.again++; return 1; }
    d->bounced = 0;
    if (!fake_pmhwdec_stats.slices++) fake_pmhwdec_stats.sps_before_first_slice = d->seen_sps;
    if (!strcmp(mode(), "midway") && fake_pmhwdec_stats.slices == 11) { fake_pmhwdec_stats.refused++; return -2; }
    d->q[d->qn++] = pts;
    return 0;
}

int pmhwdec_get(PmhwdecDecoder *d, PmhwdecPicture *pic)
{
    if (!d->qn || (d->qn < 2 && !d->eos)) return 0;
    int64_t pts = d->q[0];
    memmove(d->q, d->q + 1, --d->qn * sizeof d->q[0]);
    int f = (int)((pts * 30 + 500000) / 1000000);
    int sy = d->w + 2 * B + 6, sc = sy, ch = (d->h + 1) / 2 + B;     /* odd-looking strides on purpose */
    uint8_t *m = calloc(1, (size_t)sy * (d->h + 2 * B) + (size_t)sc * ch);
    if (!m) return 0;
    uint8_t *Y = m, *UV = m + (size_t)sy * (d->h + 2 * B);
    for (int y = 0; y < d->h; y++)
        for (int x = 0; x < d->w; x++) Y[(size_t)(y + B) * sy + x + B] = py(x, y, f);
    for (int y = 0; y < (d->h + 1) / 2; y++)
        for (int x = 0; x < (d->w + 1) / 2; x++) {
            UV[(size_t)(y + B / 2) * sc + 2 * x + B] = pu(x, y, f);
            UV[(size_t)(y + B / 2) * sc + 2 * x + B + 1] = pv(x, y, f);
        }
    memset(pic, 0, sizeof *pic);
    pic->width = d->w; pic->height = d->h; pic->left = B; pic->top = B; pic->format = 1;
    pic->plane[0] = Y; pic->plane[1] = UV; pic->stride[0] = sy; pic->stride[1] = sc;
    pic->pts = pts; pic->dmabuf_fd = -1; pic->priv = m;
    d->out++;
    if (d->out > fake_pmhwdec_stats.held_max) fake_pmhwdec_stats.held_max = d->out;
    fake_pmhwdec_stats.pictures++;
    return 1;
}

void pmhwdec_put(PmhwdecDecoder *d, PmhwdecPicture *pic) { free(pic->priv); pic->priv = NULL; d->out--; }
void pmhwdec_flush(PmhwdecDecoder *d) { d->qn = 0; }
void pmhwdec_close(PmhwdecDecoder *d) { free(d); }
const char *pmhwdec_backend_name(const PmhwdecDecoder *d) { (void)d; return "fake"; }
