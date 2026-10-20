// Backends not built in yet (replaced by um_mp4.c / um_h264.c / um_ffmpeg.c).
#include "um_internal.h"

#include <stdlib.h>
#include <string.h>

#ifndef UM_HAVE_MP4
int um_mp4_open(um_source *src, um_demux *d) { (void)src; (void)d; return UM_ERR_UNSUPPORTED; }
int um_avcc_to_annexb(const uint8_t *avcc, size_t n, uint8_t **out, size_t *outn, int *nal_len_size)
{
    (void)avcc; (void)n; (void)out; (void)outn; (void)nal_len_size;
    return -1;
}
#endif
#ifndef UM_HAVE_H264
um_dec *um_h264_create(const char *mime) { (void)mime; return NULL; }
#endif
#ifndef UM_HAVE_FFMPEG
um_dec *um_ffmpeg_create(const char *mime) { (void)mime; return NULL; }
int um_ffmpeg_available(void) { return 0; }
const char *um_ffmpeg_version(void) { return NULL; }
int um_ffmpeg_demux_open(um_source *src, um_demux *d) { (void)src; (void)d; return UM_ERR_UNSUPPORTED; }
#endif
