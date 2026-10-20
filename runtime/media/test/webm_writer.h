#ifndef WEBM_WRITER_H
#define WEBM_WRITER_H
#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t *p; size_t n, cap; } wb;

void wb_elem(wb *b, uint32_t id, const void *data, size_t n);
void wb_uint(wb *b, uint32_t id, uint64_t v);
void wb_float(wb *b, uint32_t id, double v);
void wb_float4(wb *b, uint32_t id, float v);
void wb_str(wb *b, uint32_t id, const char *s);
void wb_master(wb *b, uint32_t id, wb *child);     /* frees child */
void wb_free(wb *b);
void wb_block(wb *cluster, int track, int rel, int key, const uint8_t *const *frames, const size_t *sizes, int n, int lacing);
void wb_header(wb *out);
void wb_xiph_private(wb *out, const uint8_t *const *pk, const size_t *n, int count);

#endif
