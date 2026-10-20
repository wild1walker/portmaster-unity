// umbench: what decoding a clip costs on this machine (a device check, like ../host/shadercheck).
//   umbench <file> [offset size]     (offset/size: a clip inside a bigger file, e.g. a game's
//                                     .resource file, as a VideoClip's m_ExternalResources gives)
// Decodes every track of the clip as fast as the decoders go (each on its decoding thread, as in
// the game) and prints, per track, the decoder, the frames or seconds of audio, the wall time per
// frame (average and 95th percentile) and how many times real time that is.
#define _GNU_SOURCE
#include "um.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: umbench <file> [offset size]\n"); return 2; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror(argv[1]); return 1; }
    int64_t off = argc > 3 ? atoll(argv[2]) : 0, len = argc > 3 ? atoll(argv[3]) : -1;
    printf("decoders: %s\n", um_decoders());
    um_extractor *probe = um_extractor_new();
    if (um_extractor_set_fd(probe, fd, off, len) != UM_OK) { fprintf(stderr, "not a clip this build reads\n"); return 1; }
    int nt = um_extractor_track_count(probe);
    um_extractor_free(probe);
    for (int t = 0; t < nt; t++) {
        um_extractor *ex = um_extractor_new();
        um_extractor_set_fd(ex, fd, off, len);
        um_format *f = um_extractor_track_format(ex, t);
        const char *mime = um_format_get_string(f, UM_KEY_MIME);
        int video = mime && !strncmp(mime, "video/", 6);
        int32_t w = 0, h = 0;
        float fps = 30;
        int64_t dur = 0;
        um_format_get_int32(f, UM_KEY_WIDTH, &w);
        um_format_get_int32(f, UM_KEY_HEIGHT, &h);
        um_format_get_float(f, UM_KEY_FRAME_RATE, &fps);
        um_format_get_int64(f, UM_KEY_DURATION, &dur);
        um_codec *c = um_codec_create(mime);
        if (!c || um_codec_configure(c, f, NULL) != UM_OK || um_codec_start(c) != UM_OK) {
            printf("track %d %s: no decoder\n", t, mime ? mime : "?");
            um_codec_free(c); um_format_free(f); um_extractor_free(ex);
            continue;
        }
        um_extractor_select(ex, t, 1);
        int n = 0, cap = 4096, eos = 0, in_eos = 0;
        double *dt = malloc(cap * sizeof *dt), t0 = now(), last = t0;
        long pcm = 0;
        int rate = 0, ch = 0;
        while (!eos) {
            if (!in_eos) {
                int i = um_codec_dequeue_input(c, 0);
                if (i >= 0) {
                    size_t bc;
                    uint8_t *b = um_codec_input_buffer(c, i, &bc);
                    ssize_t r = um_extractor_read(ex, b, bc);
                    if (r < 0) { um_codec_queue_input(c, i, 0, 0, 0, UM_BUFFER_FLAG_END_OF_STREAM); in_eos = 1; }
                    else { um_codec_queue_input(c, i, 0, (int)r, um_extractor_sample_time(ex), 0); um_extractor_advance(ex); }
                }
            }
            um_buffer_info bi;
            int o = um_codec_dequeue_output(c, &bi, 1000);
            if (o == UM_INFO_OUTPUT_FORMAT_CHANGED) {
                um_format *of = um_codec_output_format(c);
                um_format_get_int32(of, UM_KEY_SAMPLE_RATE, &rate);
                um_format_get_int32(of, UM_KEY_CHANNEL_COUNT, &ch);
                um_format_free(of);
            } else if (o >= 0) {
                if (bi.flags & UM_BUFFER_FLAG_END_OF_STREAM) eos = 1;
                else if (video) {
                    double tn = now();
                    if (n == cap) { cap *= 2; dt = realloc(dt, cap * sizeof *dt); }
                    dt[n++] = tn - last;
                    last = tn;
                } else pcm += bi.size / (2 * (ch ? ch : 2));
                um_codec_release_output(c, o, 0);
            }
        }
        double total = now() - t0;
        if (video && n) {
            qsort(dt, n, sizeof *dt, cmp);
            double avg = total / n * 1000, p95 = dt[(int)(n * 0.95)] * 1000;
            printf("track %d %s %dx%d (%s): %d frames, %.2f ms/frame (95%%: %.2f ms), %.1f fps = %.1fx the clip's %.2f fps\n",
                   t, mime, w, h, um_codec_name(c), n, avg, p95, n / total, n / total / fps, fps);
        } else if (rate) {
            double secs = (double)pcm / rate;
            printf("track %d %s %d Hz %d ch (%s): %.2f s of audio in %.3f s = %.1fx real time\n", t, mime, rate, ch,
                   um_codec_name(c), secs, total, secs / total);
        }
        free(dt);
        um_codec_free(c); um_format_free(f); um_extractor_free(ex);
    }
    close(fd);
    return 0;
}
