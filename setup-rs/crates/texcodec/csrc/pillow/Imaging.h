/* The part of Pillow's libImaging/Imaging.h (and ImagingUtils.h, Mode.h) that the vendored
 * Resample.c (setup-rs/vendor/pillow) uses, for texcodec's resize: the image struct's fields it
 * reads, the constants, and the allocation and error functions (pillow_shim.c). Not Pillow's own
 * header: only what that file needs, with the same names and meanings. */
#ifndef TC_PILLOW_IMAGING_H
#define TC_PILLOW_IMAGING_H

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef uint8_t UINT8;
typedef int8_t INT8;
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef uint32_t UINT32;
typedef int32_t INT32;
typedef float FLOAT32;

typedef enum {
    IMAGING_MODE_UNKNOWN,
    IMAGING_MODE_1,
    IMAGING_MODE_L,
    IMAGING_MODE_P,
    IMAGING_MODE_RGBA,
    IMAGING_MODE_RGBa,
    IMAGING_MODE_I_16B,
    IMAGING_MODE_I_16N,
} ModeID;

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

typedef struct ImagingMemoryInstance *Imaging;

struct ImagingMemoryInstance {
    ModeID mode;
    int type;
    int bands;
    int xsize;
    int ysize;
    UINT8 **image8;  /* 1-byte pixels (unused here: always NULL) */
    INT32 **image32; /* 4-byte pixels: the rows */
    char **image;    /* the rows */
    char *block;     /* the pixels, one allocation */
};

#define IMAGING_PIXEL_I(im, x, y) ((im)->image32[(y)][(x)])
#define IMAGING_PIXEL_F(im, x, y) (((FLOAT32 *)(im)->image32[y])[x])

/* little-endian (ImagingUtils.h) */
#define MAKE_UINT32(u0, u1, u2, u3) \
    ((UINT32)(u0) | ((UINT32)(u1) << 8) | ((UINT32)(u2) << 16) | ((UINT32)(u3) << 24))
#define CLIP16(v) ((v) <= 0 ? 0 : (v) < 65536 ? (v) : 65535)

typedef void *ImagingSectionCookie;
static inline void ImagingSectionEnter(ImagingSectionCookie *cookie) { (void)cookie; }
static inline void ImagingSectionLeave(ImagingSectionCookie *cookie) { (void)cookie; }

static inline int isModeI16(const ModeID mode) {
    return mode == IMAGING_MODE_I_16B || mode == IMAGING_MODE_I_16N;
}

void *ImagingError_MemoryError(void);
void *ImagingError_ModeError(void);
void *ImagingError_ValueError(const char *message);
Imaging ImagingNewDirty(ModeID mode, int xsize, int ysize);
void ImagingDelete(Imaging im);
Imaging ImagingCopy(Imaging im);

Imaging ImagingResample(Imaging imIn, int xsize, int ysize, int filter, float box[4]);

#endif
