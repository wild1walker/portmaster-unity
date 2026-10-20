#ifndef CLIPS_H
#define CLIPS_H
#include <stddef.h>
#include <stdint.h>
#include "webm_writer.h"

#define AR 48000
typedef struct { uint8_t *data; size_t size; int64_t ms; int key; } packet;
typedef struct { packet *p; int n, cap; } plist;
typedef struct { const char *codec; int w, h; float fps; const uint8_t *priv; size_t privn; uint64_t delay_ns; plist *pk; int audio; } mtrack;

/* the test picture of frame f (VP8/VP9 clips) and the test tones (440 Hz left, 660 Hz right) */
uint8_t pix_y(int x, int y, int f);
uint8_t pix_u(int x, int y, int f);
uint8_t pix_v(int x, int y, int f);
float   tone(int ch, int64_t i);
void    padd(plist *l, const void *d, size_t n, int64_t ms, int key);
plist   encode_vpx(int vp9, int w, int h, int frames);          /* 30 fps, a key frame every 10 */
plist   encode_vorbis(int seconds, uint8_t **priv, size_t *privn);   /* 48 kHz stereo; Xiph-laced headers */
plist   encode_opus(int seconds, int *preskip);
/* clusters of 500 ms; with lace, the second cluster's audio packets go in laced blocks */
wb      mux_webm(mtrack *t, int nt, double duration_ms, int lace);
/* the clip inside a bigger file at an offset (as a clip inside an APK); returns the fd */
int     write_embedded(const char *path, const wb *clip, int64_t *off);
#endif
