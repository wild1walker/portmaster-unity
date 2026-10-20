// Backends not built in yet (replaced by um_mp4.c / um_h264.c / um_ffmpeg.c).
#include "um_internal.h"

#include <stdlib.h>
#include <string.h>

#ifndef UM_HAVE_MP4
int um_mp4_open(um_source *src, um_demux *d) { (void)src; (void)d; return UM_ERR_UNSUPPORTED; }
int um_avcc_csd(um_format *f, const uint8_t *a, size_t n, int *nls) { (void)f; (void)a; (void)n; (void)nls; return -1; }
int um_hvcc_csd(um_format *f, const uint8_t *a, size_t n, int *nls) { (void)f; (void)a; (void)n; (void)nls; return -1; }
ssize_t um_nal_convert(const uint8_t *in, size_t n, int nls, uint8_t *out, size_t cap) { (void)in; (void)n; (void)nls; (void)out; (void)cap; return -1; }
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
