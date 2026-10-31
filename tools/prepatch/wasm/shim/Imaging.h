/* A stand-in for Pillow's Imaging.h: just what Resample.c and BcnDecode.c use, so both compile
 * unchanged for pmucore.wasm (an image is rows of 4-byte pixels, or of 1-byte ones for "L"). */
#pragma once
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t UINT8;
typedef int8_t INT8;
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef uint32_t UINT32;
typedef int32_t INT32;
typedef float FLOAT32;
typedef double FLOAT64;
typedef long Py_ssize_t;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { IMAGING_MODE_1 = 1, IMAGING_MODE_L, IMAGING_MODE_P, IMAGING_MODE_RGB, IMAGING_MODE_RGBA, IMAGING_MODE_RGBa,
       IMAGING_MODE_I_16, IMAGING_MODE_I_16B, IMAGING_MODE_I_16L, IMAGING_MODE_I_16N };
#define isModeI16(m) ((m) >= IMAGING_MODE_I_16)
/* (the 16- and 32-bit paths, never taken here) */
#define CLIP16(v) ((v) <= 0 ? 0 : (v) < 65536 ? (v) : 65535)
#define IMAGING_PIXEL_I(im, x, y) ((im)->image32[(y)][(x)])
#define IMAGING_PIXEL_F(im, x, y) (((FLOAT32 *)(im)->image32[(y)])[(x)])

#define IMAGING_TYPE_UINT8 0
#define IMAGING_TYPE_INT32 1
#define IMAGING_TYPE_FLOAT32 2
#define IMAGING_TYPE_SPECIAL 3

#define IMAGING_TRANSFORM_NEAREST 0
#define IMAGING_TRANSFORM_BOX 4
#define IMAGING_TRANSFORM_BILINEAR 2
#define IMAGING_TRANSFORM_HAMMING 5
#define IMAGING_TRANSFORM_BICUBIC 3
#define IMAGING_TRANSFORM_LANCZOS 1

#define MAKE_UINT32(u0, u1, u2, u3) \
    ((UINT32)(u0) | ((UINT32)(u1) << 8) | ((UINT32)(u2) << 16) | ((UINT32)(u3) << 24))

typedef struct ImagingMemoryInstance *Imaging;
struct ImagingMemoryInstance {
    int mode, type, bands, pixelsize, xsize, ysize;
    UINT8 **image8;              /* 1-byte pixels ("L"), else NULL */
    INT32 **image32;             /* 4-byte pixels, else NULL */
    char **image;                /* rows (either kind) */
    char *block;
};

typedef struct ImagingCodecStateInstance {
    int count, state, errcode;
    int x, y, ystep, xsize, ysize, xoff, yoff;
    void *context;
} *ImagingCodecState;

typedef int ImagingSectionCookie;
#define ImagingSectionEnter(c) ((void)(c))
#define ImagingSectionLeave(c) ((void)(c))

static inline void *ImagingError_MemoryError(void) { return NULL; }
static inline void *ImagingError_ModeError(void) { return NULL; }
static inline void *ImagingError_ValueError(const char *m) { (void)m; return NULL; }

static inline Imaging ImagingNewDirty(int mode, int xsize, int ysize) {
    int ps = mode == IMAGING_MODE_L ? 1 : 4;
    Imaging im = calloc(1, sizeof(*im));
    if (!im) return NULL;
    im->mode = mode; im->type = IMAGING_TYPE_UINT8; im->pixelsize = ps;
    im->bands = mode == IMAGING_MODE_L ? 1 : mode == IMAGING_MODE_RGB ? 3 : 4;
    im->xsize = xsize; im->ysize = ysize;
    im->block = malloc((size_t)xsize * ysize * ps + 1);
    im->image = malloc(sizeof(char *) * (ysize ? ysize : 1));
    if (!im->block || !im->image) { free(im->block); free(im->image); free(im); return NULL; }
    for (int y = 0; y < ysize; y++) im->image[y] = im->block + (size_t)y * xsize * ps;
    if (ps == 1) im->image8 = (UINT8 **)im->image; else im->image32 = (INT32 **)im->image;
    return im;
}
static inline void ImagingDelete(Imaging im) {
    if (!im) return;
    free(im->block); free(im->image); free(im);
}
static inline Imaging ImagingCopy(Imaging in) {
    Imaging o = ImagingNewDirty(in->mode, in->xsize, in->ysize);
    if (o) memcpy(o->block, in->block, (size_t)in->xsize * in->ysize * in->pixelsize);
    return o;
}

Imaging ImagingResample(Imaging imIn, int xsize, int ysize, int filter, float box[4]);
