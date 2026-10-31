/* guided_astc's core (portmaster-unity convert/native/guided_astc, MIT), single-threaded, with the
 * Python wrapper replaced by plain calls. guided_core.inc is cut from guided_astc.c by build.sh. */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "guided_core.inc"

#define EXPORT(n) __attribute__((export_name(n)))

/* guided_astc.encode(): blocks (16 bytes each, zero where left to the encoder) and one class per
 * block (0 other, 1 one colour, 2 fitted, 3 seeded, 4 line, 5 near). dxt: NULL or the seed blocks. */
EXPORT("pm_guided")
int pm_guided(const uint8_t *img, int w, int h, int bw, int bh, int max_err, int line, int near,
              const uint8_t *dxt, int dxt5, uint8_t *blocks, uint8_t *cls)
{
    if (w <= 0 || h <= 0 || bw < 4 || bh < 4 || bw > 12 || bh > 12) return -1;
    init_tables();
    int bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
    memset(blocks, 0, (size_t)bx * by * 16);
    job J;
    J.img = img; J.w = w; J.h = h; J.bw = bw; J.bh = bh; J.bx = bx; J.by = by;
    J.max_err = max_err; J.line = line; J.near = near; J.dxt = dxt; J.dxt5 = dxt5;
    J.out = blocks; J.cls = cls; J.err_fit = 0; J.y0 = 0; J.y1 = by;
    work(&J);
    return 0;
}

/* guided_astc.gather(): the texels of the n blocks in rest (uint32 block numbers) as an image of
 * cols blocks across (dst: cols*bw x rows*bh RGBA), the last row filled with the first block. */
EXPORT("pm_gather")
void pm_gather(const uint8_t *img, int w, int h, int bw, int bh, const uint32_t *rest, int n, int cols, uint8_t *dst)
{
    int bx = (w + bw - 1) / bw, rows = (n + cols - 1) / cols;
    size_t W = (size_t)cols * bw;
    job J;
    memset(&J, 0, sizeof J);
    J.img = img; J.w = w; J.h = h; J.bw = bw; J.bh = bh;
    uint8_t px[4 * 144];
    for (int k = 0; k < rows * cols; k++) {
        uint32_t b = rest[k < n ? k : 0];
        load_block(&J, (int)(b % bx), (int)(b / bx), px);
        size_t ox = (size_t)(k % cols) * bw, oy = (size_t)(k / cols) * bh;
        for (int j = 0; j < bh; j++) memcpy(dst + 4 * ((oy + j) * W + ox), px + 4 * j * bw, 4 * bw);
    }
}
