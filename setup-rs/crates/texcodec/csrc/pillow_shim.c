/* Images for the vendored Pillow Resample.c (texcodec's resize): 4-byte pixels (RGBa / RGBA, as
 * Pillow stores them) in one block with row pointers, and tc_resample(), what Pillow's
 * Image.resize does for such an image once the mode is settled (_imaging.c's _resize with a box:
 * ImagingResample). */
#include "Imaging.h"

void *ImagingError_MemoryError(void) { return NULL; }
void *ImagingError_ModeError(void) { return NULL; }
void *ImagingError_ValueError(const char *message) { (void)message; return NULL; }

/* (ImagingNewDirty for the 4-byte modes: uninitialised pixels) */
Imaging ImagingNewDirty(ModeID mode, int xsize, int ysize)
{
    Imaging im = calloc(1, sizeof(*im));
    if (!im) return NULL;
    im->mode = mode;
    im->type = IMAGING_TYPE_UINT8;
    im->bands = 4;
    im->xsize = xsize;
    im->ysize = ysize;
    size_t line = (size_t)xsize * 4;
    im->block = malloc(line * (size_t)ysize ? line * (size_t)ysize : 1);
    im->image = malloc(sizeof(char *) * (size_t)(ysize ? ysize : 1));
    if (!im->block || !im->image) {
        free(im->block);
        free(im->image);
        free(im);
        return NULL;
    }
    for (int y = 0; y < ysize; y++) im->image[y] = im->block + line * (size_t)y;
    im->image32 = (INT32 **)im->image;
    return im;
}

void ImagingDelete(Imaging im)
{
    if (!im) return;
    free(im->block);
    free(im->image);
    free(im);
}

Imaging ImagingCopy(Imaging in)
{
    Imaging im = ImagingNewDirty(in->mode, in->xsize, in->ysize);
    if (!im) return NULL;
    for (int y = 0; y < in->ysize; y++) memcpy(im->image[y], in->image[y], (size_t)in->xsize * 4);
    return im;
}

/* tc_resample: the w x h 4-byte image `in` (rows top first, w * 4 bytes each) resampled to ow x oh
 * from the source rectangle box (x0, y0, x1, y1: floats, as Pillow takes them) with filter (an
 * IMAGING_TRANSFORM_* value), into out (ow * oh * 4 bytes). 0, or -1 when Pillow fails. */
int tc_resample(const UINT8 *in, int w, int h, UINT8 *out, int ow, int oh, int filter, const float box_in[4])
{
    struct ImagingMemoryInstance src;
    memset(&src, 0, sizeof src);
    src.mode = IMAGING_MODE_RGBa;
    src.type = IMAGING_TYPE_UINT8;
    src.bands = 4;
    src.xsize = w;
    src.ysize = h;
    src.image = malloc(sizeof(char *) * (size_t)(h ? h : 1));
    if (!src.image) return -1;
    for (int y = 0; y < h; y++) src.image[y] = (char *)in + (size_t)w * 4 * (size_t)y;
    src.image32 = (INT32 **)src.image;
    float box[4] = {box_in[0], box_in[1], box_in[2], box_in[3]};
    Imaging res = ImagingResample(&src, ow, oh, filter, box);
    free(src.image);
    if (!res) return -1;
    for (int y = 0; y < oh; y++) memcpy(out + (size_t)ow * 4 * (size_t)y, res->image[y], (size_t)ow * 4);
    ImagingDelete(res);
    return 0;
}
