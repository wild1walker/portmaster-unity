#ifndef MP4_WRITER_H
#define MP4_WRITER_H
#include <stddef.h>
#include <stdint.h>
#include "webm_writer.h"

typedef struct {
    int video;
    uint32_t timescale;
    int w, h, rate, ch;
    const uint8_t *cfg;         /* avcC, or the AudioSpecificConfig */
    size_t cfgn;
    int n;                      /* samples, in decode order */
    const uint8_t **data;
    const size_t *size;
    const int64_t *dts;         /* in timescale units */
    const int64_t *cto;         /* composition offsets, or NULL */
    const int *key;
    int64_t duration_ms;
    int64_t elst_media_time;    /* an edit list starting at this media time (0: none) */
} mp4_track;

wb mux_mp4(mp4_track *t, int nt);
#endif
