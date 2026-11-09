// wwplay.c's unit test: a made-up bank (JSON + pak written here, the Ogg Vorbis tone from
// test/data) played through the backend interface and mixed at 48 kHz, the results measured.
//   test_wwplay <scratch dir> <test/data dir>
#define _GNU_SOURCE
#include "../wwplay.h"

#include <math.h>
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t done_ids[256];
static int ndone;
static void h_done(uint32_t id) { if (ndone < 256) done_ids[ndone++] = id; }
static void h_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("  [wwise] "); vprintf(fmt, ap); printf("\n"); va_end(ap); }
static uint32_t h_id(const char *s) { uint32_t h = 2166136261u; for (; *s; s++) { h *= 16777619u; h ^= (unsigned char)(*s | (*s >= 'A' && *s <= 'Z' ? 32 : 0)); } return h; }
static const ua_wwise_host HOST = { UA_WWISE_BACKEND_VERSION, h_done, NULL, NULL, h_id, NULL, h_log };
static int was_done(uint32_t id) { for (int i = 0; i < ndone; i++) if (done_ids[i] == id) return 1; return 0; }

// ---- the made-up bank ----
static FILE *pak;
static long pakoff;
static char media_json[8192];
static void add_media(int id, const char *codec, int rate, int ch, int samples, const void *data, long len, const char *extra)
{
    while (pakoff % 16) { fputc(0, pak); pakoff++; }
    fwrite(data, 1, len, pak);
    snprintf(media_json + strlen(media_json), sizeof media_json - strlen(media_json),
             "%s\"%d\": {\"off\": %ld, \"len\": %ld, \"codec\": \"%s\", \"rate\": %d, \"channels\": %d, \"samples\": %d%s}",
             media_json[0] ? ", " : "", id, pakoff, len, codec, rate, ch, samples, extra ? extra : "");
    pakoff += len;
}

static int16_t *tone(int n, int rate, float hz, float amp)
{
    int16_t *s = malloc(n * 2);
    for (int i = 0; i < n; i++) s[i] = (int16_t)lrintf(amp * 32767 * sinf(2 * (float)M_PI * hz * i / rate));
    return s;
}
static int16_t *dc(int n, float amp) { int16_t *s = malloc(n * 2); for (int i = 0; i < n; i++) s[i] = (int16_t)lrintf(amp * 32767); return s; }

// an IMA encoder with Wwise's block layout (header sample, step index, reserved; 63 nibbles, low first)
static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int8_t IDX[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static uint8_t *ima_encode(const int16_t *s, int n, long *len)
{
    int blocks = (n + 63) / 64, idx = 0;
    uint8_t *o = calloc(blocks, 36);
    for (int b = 0; b < blocks; b++) {
        uint8_t *h = o + b * 36;
        int pred = s[b * 64];
        h[0] = pred & 255; h[1] = (pred >> 8) & 255; h[2] = (uint8_t)idx; h[3] = 0;
        for (int i = 1; i < 64; i++) {
            int x = b * 64 + i < n ? s[b * 64 + i] : 0, step = STEP[idx], diff = x - pred, code = 0;
            if (diff < 0) { code = 8; diff = -diff; }
            int q = (diff * 4) / step;
            if (q > 7) q = 7;
            code |= q;
            int delta = ((2 * (code & 7) + 1) * step) >> 3;
            pred += code & 8 ? -delta : delta;
            pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
            idx += IDX[code];
            idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
            h[4 + (i - 1) / 2] |= (uint8_t)(code << (((i - 1) & 1) * 4));
        }
    }
    *len = blocks * 36L;
    return o;
}

static const char *NODES =
    "\"10\": {\"type\": \"sound\", \"media\": \"1\", \"bus\": \"500\"},"
    "\"11\": {\"type\": \"sound\", \"media\": \"2\", \"bus\": \"500\"},"
    "\"12\": {\"type\": \"sound\", \"media\": \"3\"},"
    "\"13\": {\"type\": \"sound\", \"media\": \"4\", \"bus\": \"500\"},"
    "\"14\": {\"type\": \"sound\", \"media\": \"1\", \"loop\": 3, \"bus\": \"500\"},"
    "\"15\": {\"type\": \"sound\", \"media\": \"1\", \"delay_ms\": 100, \"bus\": \"500\"},"
    "\"16\": {\"type\": \"sound\", \"media\": \"5\", \"limit\": 1, \"loop\": 0, \"bus\": \"500\"},"
    "\"17\": {\"type\": \"sound\", \"media\": \"1\", \"pitch\": 1200, \"bus\": \"500\"},"
    "\"18\": {\"type\": \"sound\", \"media\": \"5\", \"loop\": 0, \"bus\": \"501\"},"
    "\"19\": {\"type\": \"sound\", \"media\": \"6\", \"loop\": 0, \"vol\": -6, \"bus\": \"500\"},"
    "\"20\": {\"type\": \"random\", \"children\": [\"21\", \"22\"], \"weights\": [100, 0]},"
    "\"21\": {\"type\": \"sound\", \"media\": \"7\"}, \"22\": {\"type\": \"sound\", \"media\": \"8\"},"
    "\"23\": {\"type\": \"random\", \"children\": [\"21\", \"22\"], \"avoid_repeat\": 1},"
    "\"24\": {\"type\": \"sequence\", \"children\": [\"21\", \"22\", \"25\"]},"
    "\"25\": {\"type\": \"sound\", \"media\": \"9\"},"
    "\"26\": {\"type\": \"switch\", \"group\": \"700\", \"map\": {\"701\": [\"21\"], \"702\": [\"22\"]}, \"default\": \"701\"},"
    "\"27\": {\"type\": \"layer\", \"children\": [\"21\", \"22\"]},"
    "\"28\": {\"type\": \"sequence\", \"children\": [\"21\", \"22\"], \"continuous\": true, \"loop\": 2},"
    "\"30\": {\"type\": \"music_switch\", \"args\": [{\"group\": \"800\", \"state\": true}], \"bus\": \"502\","
    "  \"tree\": {\"801\": \"31\", \"802\": \"32\", \"803\": \"36\", \"0\": \"31\"},"
    "  \"rules\": [{\"src\": [\"-1\"], \"dst\": [\"36\"], \"sync\": 0}, {\"src\": [\"31\"], \"dst\": [\"-1\"], \"sync\": 7}]},"
    "\"31\": {\"type\": \"playlist\", \"parent\": \"30\", \"items\": {\"kind\": \"sequence\", \"loop\": 0, \"children\": [{\"segment\": \"33\"}, {\"segment\": \"34\"}]}},"
    "\"32\": {\"type\": \"playlist\", \"parent\": \"30\", \"items\": {\"kind\": \"sequence\", \"loop\": 0, \"children\": [{\"segment\": \"35\"}]}},"
    "\"36\": {\"type\": \"playlist\", \"parent\": \"30\", \"items\": {\"kind\": \"sequence\", \"children\": [{\"kind\": \"random\", \"loop\": 0, \"children\": [{\"segment\": \"37\"}]}]}},"
    "\"33\": {\"type\": \"segment\", \"parent\": \"31\", \"children\": [\"40\"], \"duration_ms\": 500, \"exit_ms\": 400},"
    "\"34\": {\"type\": \"segment\", \"parent\": \"31\", \"children\": [\"41\"], \"duration_ms\": 500, \"entry_ms\": 100, \"exit_ms\": 400},"
    "\"35\": {\"type\": \"segment\", \"parent\": \"32\", \"children\": [\"42\"], \"duration_ms\": 500, \"exit_ms\": 500},"
    "\"37\": {\"type\": \"segment\", \"parent\": \"36\", \"children\": [\"43\"], \"duration_ms\": 500, \"exit_ms\": 400},"
    "\"40\": {\"type\": \"track\", \"parent\": \"33\", \"clips\": [{\"media\": \"20\", \"src_ms\": 500}]},"
    "\"41\": {\"type\": \"track\", \"parent\": \"34\", \"clips\": [{\"media\": \"21\", \"src_ms\": 500}]},"
    "\"42\": {\"type\": \"track\", \"parent\": \"35\", \"clips\": [{\"media\": \"22\", \"src_ms\": 500}]},"
    "\"43\": {\"type\": \"track\", \"parent\": \"37\", \"clips\": [{\"media\": \"23\", \"src_ms\": 500}]},"
    "\"39\": {\"type\": \"music_switch\", \"tree\": \"38\"},"
    "\"38\": {\"type\": \"segment\", \"parent\": \"39\", \"children\": [\"44\"], \"duration_ms\": 1000, \"exit_ms\": 1000},"
    "\"44\": {\"type\": \"track\", \"parent\": \"38\", \"clips\": [{\"media\": \"24\", \"src_ms\": 100, \"end_ms\": 200},"
    "  {\"media\": \"24\", \"at_ms\": 500, \"begin_ms\": 50, \"end_ms\": -20, \"src_ms\": 100}]},"
    "\"50\": {\"type\": \"mixer\", \"vol\": -6, \"bus\": \"503\", \"children\": [\"51\"], \"states\": {\"810\": {\"811\": {\"vol\": -6}}}},"
    "\"51\": {\"type\": \"sound\", \"parent\": \"50\", \"media\": \"6\", \"loop\": 0},"
    "\"60\": {\"type\": \"sound\", \"media\": \"5\", \"limit\": 1, \"limit_newest\": true, \"loop\": 0, \"bus\": \"500\"}";

static const char *EVENTS =
    "\"100\": [{\"action\": \"play\", \"target\": \"10\"}],"
    "\"101\": [{\"action\": \"play\", \"target\": \"11\"}],"
    "\"102\": [{\"action\": \"play\", \"target\": \"12\"}],"
    "\"103\": [{\"action\": \"play\", \"target\": \"13\"}],"
    "\"104\": [{\"action\": \"play\", \"target\": \"14\"}],"
    "\"105\": [{\"action\": \"play\", \"target\": \"15\"}],"
    "\"106\": [{\"action\": \"play\", \"target\": \"16\"}],"
    "\"107\": [{\"action\": \"stop\", \"target\": \"16\"}],"
    "\"108\": [{\"action\": \"play\", \"target\": \"17\"}],"
    "\"109\": [{\"action\": \"play\", \"target\": \"18\"}],"
    "\"110\": [{\"action\": \"pause\", \"target\": \"18\"}], \"111\": [{\"action\": \"resume\", \"target\": \"18\"}],"
    "\"112\": [{\"action\": \"play\", \"target\": \"19\"}],"
    "\"120\": [{\"action\": \"play\", \"target\": \"20\"}], \"123\": [{\"action\": \"play\", \"target\": \"23\"}],"
    "\"124\": [{\"action\": \"play\", \"target\": \"24\"}], \"126\": [{\"action\": \"play\", \"target\": \"26\"}],"
    "\"127\": [{\"action\": \"play\", \"target\": \"27\"}], \"128\": [{\"action\": \"play\", \"target\": \"28\"}],"
    "\"130\": [{\"action\": \"play\", \"target\": \"30\"}],"
    "\"131\": [{\"action\": \"set_state\", \"group\": \"800\", \"value\": \"802\"}],"
    "\"132\": [{\"action\": \"set_state\", \"group\": \"800\", \"value\": \"803\"}],"
    "\"133\": [{\"action\": \"stop\", \"target\": \"30\", \"fade_ms\": 50}],"
    "\"140\": [{\"action\": \"set_switch\", \"group\": \"700\", \"value\": \"702\"}],"
    "\"134\": [{\"action\": \"play\", \"target\": \"39\"}],"
    "\"141\": [{\"action\": \"stop_all\"}],"
    "\"160\": [{\"action\": \"play\", \"target\": \"60\"}],"
    "\"150\": [{\"action\": \"play\", \"target\": \"51\"}],"
    "\"151\": [{\"action\": \"set_state\", \"group\": \"810\", \"value\": \"811\"}],"
    "\"152\": [{\"action\": \"set_bus_volume\", \"target\": \"503\", \"bus\": true, \"amount\": -6, \"meaning\": \"absolute\"}],"
    "\"153\": [{\"action\": \"reset_bus_volume\", \"target\": \"503\", \"bus\": true}],"
    "\"154\": [{\"action\": \"stop\", \"target\": \"50\", \"object\": true}],"
    "\"155\": [{\"action\": \"stop\", \"target\": \"50\"}],"
    "\"156\": [{\"action\": \"stop_all\", \"except\": [\"50\"]}],"
    "\"157\": [{\"action\": \"set_rtpc\", \"group\": \"900\", \"amount\": 50, \"meaning\": \"absolute\"}],"
    "\"158\": [{\"action\": \"set_volume\", \"target\": \"51\", \"amount\": -6, \"meaning\": \"offset\"},"
    "  {\"action\": \"play\", \"target\": \"51\", \"delay_ms\": 50}, {\"action\": \"stop\", \"target\": \"51\", \"delay_ms\": 100}]";

static void write_bank(const char *dir, const char *data)
{
    char p[512];
    snprintf(p, sizeof p, "%s/Test.uaww.pak", dir);
    pak = fopen(p, "wb");
    int16_t *s;
    long len;
    s = tone(4800, 48000, 1000, 0.5f); add_media(1, "pcm", 48000, 1, 4800, s, 9600, NULL); free(s);          // 0.1 s
    s = tone(9600, 48000, 500, 0.5f);                                                                        // 0.2 s ADPCM
    uint8_t *a = ima_encode(s, 9600, &len); add_media(2, "adpcm", 48000, 1, 9600, a, len, ", \"block_align\": 36"); free(a);
    {   // 0.25 s stereo at 44.1 kHz: left 0.4, right -0.2
        int16_t *st = malloc(11025 * 4);
        for (int i = 0; i < 11025; i++) { st[2 * i] = 13107; st[2 * i + 1] = -6554; }
        add_media(3, "pcm", 44100, 2, 11025, st, 11025 * 4, NULL); free(st);
    }
    {   // the Vorbis tone
        snprintf(p, sizeof p, "%s/tone440.ogg", data);
        FILE *f = fopen(p, "rb");
        static uint8_t ogg[65536];
        long n = f ? (long)fread(ogg, 1, sizeof ogg, f) : 0;
        if (f) fclose(f);
        CHECK(n > 1000, "no %s", p);
        add_media(4, "vorbis", 22050, 2, 11025, ogg, n, NULL);
    }
    s = dc(4800, 0.5f); add_media(5, "pcm", 48000, 1, 4800, s, 9600, ", \"loop\": [1200, 3600]"); free(s);
    s = dc(4800, 0.5f); add_media(6, "pcm", 48000, 1, 4800, s, 9600, NULL); free(s);
    s = dc(2400, 0.1f); add_media(7, "pcm", 48000, 1, 2400, s, 4800, NULL); free(s);          // 50 ms each, told apart by level
    s = dc(2400, 0.2f); add_media(8, "pcm", 48000, 1, 2400, s, 4800, NULL); free(s);
    s = dc(2400, 0.3f); add_media(9, "pcm", 48000, 1, 2400, s, 4800, NULL); free(s);
    // music tracks: 0.5 s each, levels 0.1 / 0.2 / 0.3 / 0.4
    for (int k = 0; k < 4; k++) { s = dc(24000, 0.1f * (k + 1)); add_media(20 + k, "pcm", 48000, 1, 24000, s, 48000, NULL); free(s); }
    s = dc(4800, 0.25f); add_media(24, "pcm", 48000, 1, 4800, s, 9600, NULL); free(s);     // a 0.1 s clip
    fclose(pak);
    snprintf(p, sizeof p, "%s/Test.uaww.json", dir);
    FILE *j = fopen(p, "w");
    fprintf(j, "{\"format\": 1, \"bank\": {\"name\": \"Test\", \"id\": \"4242\", \"bank_version\": 128},\n"
               "\"media\": {%s},\n\"nodes\": {%s},\n\"events\": {%s},\n"
               "\"buses\": {\"500\": {\"parent\": \"510\", \"vol\": 0}, \"510\": {\"parent\": null, \"vol\": 0}, \"501\": {\"parent\": \"510\", \"vol\": -6}, \"502\": {\"parent\": null, \"vol\": 0}, \"503\": {\"vol\": 0}},\n"
               "\"rtpc\": {\"900\": [{\"target\": \"510\", \"prop\": \"volume\", \"curve\": [[0, -96, 4], [50, -6.0206, 4], [100, 0, 4]]}],\n"
               "  \"901\": [{\"target\": \"503\", \"prop\": \"volume\", \"scaling\": \"db\", \"curve\": [[0, -1, 4], [100, 0, 4]]}]},\n"
               "\"defaults\": {\"states\": {\"800\": \"801\"}, \"switches\": {\"700\": \"701\"}, \"rtpc\": {\"900\": 100, \"901\": 100}, \"state_groups\": {\"810\": {\"fade_ms\": 20, \"transitions\": [[\"0\", \"811\", 30]]}}},\n"
               "\"future_key\": {\"ignored\": [1, 2, {\"x\": \"\\u00e9\"}]}}\n", media_json, NODES, EVENTS);
    fclose(j);
}

// ---- mixing ----
#define RATE 48000
static const ua_wwise_backend *B;
static float out[RATE * 4 * 2];
static void run(float secs)        // mixes secs into out[] in blocks of 480 frames
{
    int n = (int)(secs * RATE + 0.5f);
    memset(out, 0, n * 2 * sizeof *out);
    for (int i = 0; i < n; i += 480) B->mix(out + 2 * i, n - i < 480 ? n - i : 480, RATE, 2);
}
static float rms(int ch, float t0, float t1)
{
    int a = (int)(t0 * RATE), b = (int)(t1 * RATE);
    double s = 0;
    for (int i = a; i < b; i++) s += (double)out[2 * i + ch] * out[2 * i + ch];
    return b > a ? (float)sqrt(s / (b - a)) : 0;
}
static float mean(int ch, float t0, float t1)
{
    int a = (int)(t0 * RATE), b = (int)(t1 * RATE);
    double s = 0;
    for (int i = a; i < b; i++) s += out[2 * i + ch];
    return b > a ? (float)(s / (b - a)) : 0;
}
static float last_nonzero(int ch) { int n = 4 * RATE; while (n > 0 && fabsf(out[2 * (n - 1) + ch]) < 1e-4f) n--; return (float)n / RATE; }
static int voices(void) { wwplay_stats s; wwplay_get_stats(&s); return s.voices; }
static uint32_t pid = 1000;
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static float big[RATE * 4 * 2];
static double mix_t0, mix_t1;
static void *long_mix(void *a)      // the output's thread in one long mix (4 s of audio at once)
{
    (void)a;
    mix_t0 = now_s(); B->mix(big, RATE * 4, RATE, 2); mix_t1 = now_s();
    return NULL;
}
static uint32_t post(uint32_t ev, uint64_t obj, int *on) { pid++; *on = B->post_event(ev, obj, pid, 0); return pid; }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: test_wwplay <scratch dir> <test data dir>\n"); return 2; }
    mkdir(argv[1], 0755);
    write_bank(argv[1], argv[2]);
    setenv("UA_WWISE_STATS", "0", 1);
    setenv("UA_WWISE_LIMITER", "0", 1);    // sample-exact checks; the limiter has its own below
    B = wwplay_open(&HOST, argv[1]);
    CHECK(B && B->mix && B->version == 2, "no backend");
    if (!B) return 1;
    uint32_t bank = 0;
    CHECK(B->load_bank("Test.bnk", 1234, NULL, 0, &bank) == UA_AK_SUCCESS && bank == 4242, "bank id %u", bank);
    B->load_bank("Missing.bnk", 1, NULL, 0, &bank);
    int on;
    uint32_t p;

    // a sound: 0.1 s at 1 kHz, amplitude 0.5, centred (-3 dB a side)
    p = post(100, 1, &on);
    CHECK(on == 1, "post 100 -> %d", on);
    run(0.3f);
    CHECK(fabsf(rms(0, 0.01f, 0.09f) - 0.5f * 0.7071f / sqrtf(2)) < 0.01f, "pcm rms %f", rms(0, 0.01f, 0.09f));
    CHECK(rms(0, 0.11f, 0.3f) < 1e-4f && was_done(p) && voices() == 0, "pcm end: done %d voices %d", was_done(p), voices());
    CHECK(B->post_event(999, 1, ++pid, 0) == 0, "an unknown event plays");

    // ADPCM (500 Hz, 0.2 s): close to the tone
    p = post(101, 1, &on);
    run(0.3f);
    {
        double err = 0, sig = 0;
        for (int i = 0; i < 9600; i++) {
            float ref = 0.5f * 0.7071f * sinf(2 * (float)M_PI * 500 * i / RATE);
            err += (out[2 * i] - ref) * (out[2 * i] - ref);
            sig += ref * ref;
        }
        double snr = 10 * log10(sig / (err + 1e-12));
        CHECK(snr > 25, "adpcm snr %.1f dB", snr);
        CHECK(fabsf(last_nonzero(0) - 0.2f) < 0.003f && was_done(p), "adpcm length %f", last_nonzero(0));
    }

    // stereo PCM at 44.1 kHz: resampled, left/right kept, 0.25 s long
    p = post(102, 1, &on);
    run(0.4f);
    CHECK(fabsf(mean(0, 0.01f, 0.24f) - 0.4f) < 0.002f && fabsf(mean(1, 0.01f, 0.24f) + 0.2f) < 0.002f, "stereo %f %f", mean(0, 0.01f, 0.24f), mean(1, 0.01f, 0.24f));
    CHECK(fabsf(last_nonzero(0) - 0.25f) < 0.002f, "stereo length %f", last_nonzero(0));

    // Ogg Vorbis (22.05 kHz stereo 440 Hz)
    p = post(103, 1, &on);
    run(0.7f);
    CHECK(rms(0, 0.05f, 0.45f) > 0.03f && last_nonzero(0) > 0.45f && last_nonzero(0) < 0.53f && was_done(p), "vorbis rms %f length %f", rms(0, 0.05f, 0.45f), last_nonzero(0));
    {   // its pitch: zero crossings over 0.3 s ~ 2 * 440 * 0.3
        int zc = 0;
        for (int i = (int)(0.1f * RATE); i < (int)(0.4f * RATE); i++) zc += (out[2 * i] >= 0) != (out[2 * i + 2] >= 0);
        CHECK(abs(zc - 264) < 6, "vorbis zero crossings %d", zc);
    }

    // loop 3 times: 0.3 s; delay 100 ms; pitch +1200: half the length
    post(104, 1, &on); run(0.5f);
    CHECK(fabsf(last_nonzero(0) - 0.3f) < 0.003f, "loop x3 length %f", last_nonzero(0));
    post(105, 1, &on); run(0.5f);
    CHECK(rms(0, 0, 0.099f) < 1e-4f && rms(0, 0.11f, 0.19f) > 0.1f && fabsf(last_nonzero(0) - 0.2f) < 0.003f, "delay: %f %f", rms(0, 0, 0.099f), last_nonzero(0));
    post(108, 1, &on); run(0.3f);
    CHECK(fabsf(last_nonzero(0) - 0.05f) < 0.003f, "pitch +1200 length %f", last_nonzero(0));

    // instance limit 1 (a looping sound with loop points), then stop
    uint32_t a = post(106, 1, &on), b = post(106, 1, &on);
    run(0.2f);
    CHECK(voices() == 1 && was_done(a) && !was_done(b), "limit: voices %d", voices());
    CHECK(fabsf(mean(0, 0.15f, 0.2f) - 0.5f * 0.7071f) < 0.01f, "looping level %f", mean(0, 0.15f, 0.2f));
    p = post(107, 1, &on);
    run(0.1f);
    CHECK(voices() == 0 && was_done(b) && was_done(p) && rms(0, 0.02f, 0.1f) < 1e-4f, "stop: voices %d", voices());

    // instance limit 1, the newest refused
    a = post(160, 1, &on); b = post(160, 1, &on);
    run(0.02f);
    CHECK(was_done(b) && !was_done(a) && voices() == 1, "limit_newest: voices %d", voices());
    B->stop(a); run(0.05f);
    CHECK(voices() == 0 && was_done(a), "limit_newest stop");

    // bus volume from an RTPC curve on the parent bus: 50 -> -6 dB; bus 501 is -6 dB itself
    p = post(109, 1, &on);
    run(0.1f);
    float full = mean(0, 0.05f, 0.1f);
    CHECK(fabsf(full - 0.5f * 0.7071f * 0.5012f) < 0.005f, "bus -6 dB: %f", full);
    B->set_rtpc(900, 50, 0, 0, 1);
    run(0.1f);
    CHECK(fabsf(mean(0, 0.05f, 0.1f) - full * 0.5f) < 0.003f, "rtpc 50: %f vs %f", mean(0, 0.05f, 0.1f), full);
    B->set_rtpc(900, 0, 0, 0, 1);
    run(0.1f);
    CHECK(fabsf(mean(0, 0.05f, 0.1f)) < 1e-4f, "rtpc 0: %f", mean(0, 0.05f, 0.1f));
    B->set_rtpc(900, 100, 0, 0, 1);
    post(110, 1, &on); run(0.1f);
    CHECK(rms(0, 0.02f, 0.1f) < 1e-4f, "paused: %f", rms(0, 0.02f, 0.1f));
    post(111, 1, &on); run(0.1f);
    CHECK(fabsf(mean(0, 0.02f, 0.1f) - full) < 0.005f, "resumed: %f", mean(0, 0.02f, 0.1f));
    B->stop(p);
    run(0.05f);
    CHECK(voices() == 0 && was_done(p), "StopPlayingID");

    // node volume -6 dB; StopAll on its game object; unregistering the object
    p = post(112, 7, &on); run(0.05f);
    CHECK(fabsf(mean(0, 0.02f, 0.05f) - 0.5f * 0.7071f * 0.5012f) < 0.005f, "node -6 dB: %f", mean(0, 0.02f, 0.05f));
    B->stop_all(8, 0); run(0.05f);
    CHECK(voices() == 1, "StopAll on another object stopped it");
    B->game_object(7, NULL, 0); run(0.05f);
    CHECK(voices() == 0 && was_done(p), "unregistered object's sound still plays");

    // containers: weights, avoid-repeat, sequence, switch, layer, continuous sequence (twice)
    int ok = 1;
    for (int i = 0; i < 6; i++) { post(120, 1, &on); run(0.06f); ok &= fabsf(mean(0, 0.01f, 0.04f) - 0.1f * 0.7071f) < 0.002f; }
    CHECK(ok, "weights 100/0 picked the other child");
    float prev = 0;
    ok = 1;
    for (int i = 0; i < 8; i++) { post(123, 1, &on); run(0.06f); float m = mean(0, 0.01f, 0.04f); ok &= fabsf(m - prev) > 0.03f; prev = m; }
    CHECK(ok, "avoid repeat");
    float seq[4];
    for (int i = 0; i < 4; i++) { post(124, 1, &on); run(0.06f); seq[i] = mean(0, 0.01f, 0.04f) / 0.7071f; }
    CHECK(fabsf(seq[0] - 0.1f) < 0.002f && fabsf(seq[1] - 0.2f) < 0.002f && fabsf(seq[2] - 0.3f) < 0.002f && fabsf(seq[3] - 0.1f) < 0.002f,
          "sequence %f %f %f %f", seq[0], seq[1], seq[2], seq[3]);
    post(126, 1, &on); run(0.06f);
    CHECK(fabsf(mean(0, 0.01f, 0.04f) / 0.7071f - 0.1f) < 0.002f, "switch default");
    post(140, 1, &on);
    post(126, 1, &on); run(0.06f);
    CHECK(fabsf(mean(0, 0.01f, 0.04f) / 0.7071f - 0.2f) < 0.002f, "switch set on the object: %f", mean(0, 0.01f, 0.04f) / 0.7071f);
    post(126, 2, &on); run(0.06f);
    CHECK(fabsf(mean(0, 0.01f, 0.04f) / 0.7071f - 0.1f) < 0.002f, "switch on another object");
    post(127, 1, &on); run(0.06f);
    CHECK(fabsf(mean(0, 0.01f, 0.04f) / 0.7071f - 0.3f) < 0.002f, "layer %f", mean(0, 0.01f, 0.04f) / 0.7071f);
    p = post(128, 1, &on); run(0.3f);
    CHECK(fabsf(mean(0, 0.01f, 0.04f) / 0.7071f - 0.1f) < 0.002f && fabsf(mean(0, 0.065f, 0.095f) / 0.7071f - 0.2f) < 0.002f &&
          fabsf(mean(0, 0.12f, 0.14f) / 0.7071f - 0.1f) < 0.002f && last_nonzero(0) > 0.19f && last_nonzero(0) < 0.24f && was_done(p),
          "continuous x2: %f %f end %f", mean(0, 0.01f, 0.04f) / 0.7071f, mean(0, 0.065f, 0.095f) / 0.7071f, last_nonzero(0));

    // music: state 801 -> playlist 31: segments 33 (0.1), 34 (0.2), exit cues at 400 ms, 34's entry at
    // 100 ms: 33 at 0, 34 at 0.3 (its entry on 33's exit), 33 at 0.7, 34 at 1.0; music bus 502
    uint32_t mp = post(130, 1, &on);
    CHECK(on == 1, "music");
    run(1.2f);
    float m1 = mean(0, 0.05f, 0.25f) / 0.7071f, m2 = mean(0, 0.32f, 0.48f) / 0.7071f, m3 = mean(0, 0.72f, 0.78f) / 0.7071f;
    CHECK(fabsf(m1 - 0.1f) < 0.003f && fabsf(m2 - 0.3f) < 0.003f && fabsf(m3 - 0.3f) < 0.003f, "music: %f %f %f", m1, m2, m3);
    // (0.3 - 0.5: 33's tail 0.1 and 34 0.2 overlap = 0.3; 0.7 - 0.8: 34's tail and 33 again)
    // state 802 at 1.2 s: taken at the current segment's exit (34 began at 1.0: exit at 1.4, where 33
    // would have come), so 35 (0.3) follows 34 (0.2) at 1.4
    p = post(131, 1, &on);
    run(0.6f);
    CHECK(on == 1 && was_done(p), "a state event: %d, ended %d", on, was_done(p));
    float before = mean(0, 0.02f, 0.18f) / 0.7071f, after = mean(0, 0.32f, 0.55f) / 0.7071f;
    CHECK(fabsf(before - 0.2f) < 0.003f && fabsf(after - 0.3f) < 0.003f, "state change at exit: %f %f", before, after);
    // 803: a rule syncs to "immediate": at once
    post(132, 1, &on);
    run(0.2f);
    CHECK(fabsf(mean(0, 0.06f, 0.15f) / 0.7071f - 0.4f) < 0.003f, "state change at once: %f", mean(0, 0.06f, 0.15f) / 0.7071f);
    CHECK(!was_done(mp), "music ended");
    post(133, 1, &on);
    run(0.2f);
    CHECK(rms(0, 0.06f, 0.2f) < 1e-4f && was_done(mp), "music stop: %f done %d", rms(0, 0.06f, 0.2f), was_done(mp));

    // clips: one looped to 0.3 s, one trimmed (0.55 - 0.58 s); the lone segment plays once
    mp = post(134, 1, &on);
    run(1.1f);
    CHECK(fabsf(mean(0, 0.01f, 0.29f) - 0.25f * 0.7071f) < 0.003f && rms(0, 0.31f, 0.54f) < 1e-4f &&
          fabsf(mean(0, 0.555f, 0.575f) - 0.25f * 0.7071f) < 0.003f && rms(0, 0.59f, 1.1f) < 1e-4f && was_done(mp),
          "clips: %f %f %f %f done %d", mean(0, 0.01f, 0.29f), rms(0, 0.31f, 0.54f), mean(0, 0.555f, 0.575f), rms(0, 0.59f, 1.1f), was_done(mp));

    // an actor-mixer's volume (-6), bus (503) and state (-6); bus volume actions; RTPCs with dB scaling
    float lv = 0.5f * 0.7071f * 0.5012f;
    p = post(150, 5, &on); run(0.05f);
    CHECK(fabsf(mean(0, 0.02f, 0.05f) - lv) < 0.003f, "mixer volume: %f", mean(0, 0.02f, 0.05f));
    post(151, 5, &on); run(0.05f);     // with the group's transition's fade, 30 ms
    CHECK(fabsf(mean(0, 0.035f, 0.05f) - lv * 0.5012f) < 0.003f && mean(0, 0.005f, 0.015f) > lv * 0.6f && mean(0, 0.005f, 0.015f) < lv * 0.95f,
          "mixer state: %f, fading %f", mean(0, 0.035f, 0.05f), mean(0, 0.005f, 0.015f));
    post(152, 5, &on); run(0.05f);
    CHECK(fabsf(mean(0, 0.02f, 0.05f) - lv * 0.2512f) < 0.002f, "set_bus_volume: %f", mean(0, 0.02f, 0.05f));
    post(153, 5, &on); B->set_rtpc(901, 50, 0, 0, 1); run(0.05f);
    CHECK(fabsf(mean(0, 0.02f, 0.05f) - lv * 0.2512f) < 0.002f, "reset + rtpc (dB scaling) 50: %f", mean(0, 0.02f, 0.05f));
    B->set_rtpc(901, 100, 0, 0, 1);
    // stop scope: "object" on another object does nothing; global stops it; stop_all with an exception
    post(154, 99, &on); run(0.02f);
    CHECK(voices() == 1, "stop on another object (object scope) stopped it");
    post(155, 99, &on); run(0.02f);
    CHECK(voices() == 0 && was_done(p), "global stop");
    p = post(150, 5, &on); uint32_t p2 = post(109, 5, &on);
    post(156, 5, &on); run(0.02f);
    CHECK(voices() == 1 && was_done(p2) && !was_done(p), "stop_all except");
    B->stop(p); run(0.02f);
    // set_rtpc action on the parent bus of 500/501: -6 dB; then back to the default
    post(157, 1, &on); p = post(109, 1, &on); run(0.05f);
    CHECK(fabsf(mean(0, 0.02f, 0.05f) - full * 0.5f) < 0.003f, "set_rtpc action: %f", mean(0, 0.02f, 0.05f));
    B->set_rtpc(900, 100, 0, 0, 1); B->stop(p); run(0.02f);
    // delayed actions: set_volume -6 at once, play at 50 ms, stop at 100 ms (the mixer's state -6 still set)
    p = post(158, 5, &on); run(0.15f);
    CHECK(rms(0, 0, 0.049f) < 1e-4f && fabsf(mean(0, 0.06f, 0.09f) - lv * 0.2512f) < 0.003f && rms(0, 0.11f, 0.15f) < 1e-4f && was_done(p),
          "delayed actions: %f %f %f", rms(0, 0, 0.049f), mean(0, 0.06f, 0.09f), rms(0, 0.11f, 0.15f));

    // ExecuteActionOnEvent(event, stop, game object)
    p = post(109, 3, &on); run(0.05f);
    {
        const char *cls[] = { "u", "i", "l" };
        ua_ak_value args[3], r;
        args[0].u = 109; args[1].i = 0; args[2].u = 3;
        ua_ak_call c = { "CSharp_ExecuteActionOnEvent__SWIG_3", "ExecuteActionOnEvent", 'i', 3, cls, args };
        CHECK(B->call(&c, &r) == 1 && r.i == UA_AK_SUCCESS, "ExecuteActionOnEvent not answered");
    }
    run(0.05f);
    CHECK(voices() == 0 && was_done(p), "ExecuteActionOnEvent stop");

    // the voice cap: 4 voices; louder ones replace quieter ones
    B->term();
    wwplay_close();
    setenv("UA_WWISE_VOICES", "4", 1);
    B = wwplay_open(&HOST, argv[1]);
    B->load_bank("Test", 0, NULL, 0, &bank);
    for (int i = 0; i < 6; i++) post(i < 4 ? 112 : 109, 10 + i, &on);
    wwplay_stats st;
    wwplay_get_stats(&st);
    CHECK(st.voices == 4 && st.dropped == 2, "cap: %d voices, %lu dropped", st.voices, st.dropped);
    run(0.05f);
    B->stop_all(0, 1);
    run(0.05f);
    CHECK(voices() == 0, "StopAll");

    // the master limiter: five stereo voices (left 5 x 0.4) held to the ceiling, the gain smooth; one
    // voice passes as it is, 64 frames later
    B->term();
    wwplay_close();
    setenv("UA_WWISE_LIMITER", "1", 1);
    setenv("UA_WWISE_VOICES", "16", 1);
    B = wwplay_open(&HOST, argv[1]);
    B->load_bank("Test", 0, NULL, 0, &bank);
    post(102, 1, &on); run(0.3f);
    CHECK(fabsf(mean(0, 0.01f, 0.24f) - 0.4f) < 0.002f && rms(0, 0, 63.0f / RATE) < 1e-6f && fabsf(out[2 * 64] - 0.4f) < 0.01f,
          "limiter, one voice: %f, first frame %f", mean(0, 0.01f, 0.24f), out[2 * 64]);
    for (int i = 0; i < 5; i++) post(102, 1, &on);
    run(0.3f);
    {
        float peak = 0;
        for (int i = 0; i < RATE * 3 / 10; i++) if (fabsf(out[2 * i]) > peak) peak = fabsf(out[2 * i]);
        CHECK(peak <= 0.9801f && fabsf(mean(0, 0.05f, 0.2f) - 0.98f) < 0.01f && fabsf(mean(1, 0.05f, 0.2f) + 0.49f) < 0.01f,
              "limiter: peak %f, level %f / %f", peak, mean(0, 0.05f, 0.2f), mean(1, 0.05f, 0.2f));
    }

    // RenderAudio (once a frame, on the game's thread) while the output's thread is inside a mix: it
    // returns at once (it used to wait for the whole mix), and an event the game posts meanwhile is
    // queued as always and plays from the next block on, from its first frame
    {
        for (int i = 0; i < 4; i++) post(102, 1, &on);
        pthread_t t;
        pthread_create(&t, NULL, long_mix, NULL);
        while (mix_t0 == 0) { struct timespec ts = { 0, 100000 }; nanosleep(&ts, NULL); }
        double r0 = now_s(); B->render(); double r1 = now_s();
        uint32_t q = post(100, 1, &on);
        pthread_join(t, NULL);
        double during = r0 < mix_t1 ? r1 - r0 : -1, mixing = mix_t1 - mix_t0;
        // the tone (1 kHz) in the rest of that mix or in the blocks after it: the largest AC level of a
        // 20 ms window (the other voices are steady levels)
        run(0.3f);
        float ac = 0;
        for (int pass = 0; pass < 2; pass++) {
            const float *b = pass ? out : big; int n = pass ? RATE * 3 / 10 : RATE * 4, w = RATE / 50;
            for (int a = 0; a + w <= n; a += w) {
                double m = 0, v = 0;
                for (int i = a; i < a + w; i++) m += b[2 * i];
                m /= w;
                for (int i = a; i < a + w; i++) v += (b[2 * i] - m) * (b[2 * i] - m);
                if (sqrt(v / w) > ac) ac = (float)sqrt(v / w);
            }
        }
        CHECK(on == 1 && ac > 0.1f && was_done(q), "event posted during a mix: AC level %f, done %d", ac, was_done(q));
        if (during >= 0 && mixing > 0.01)
            CHECK(during < mixing / 4, "RenderAudio waited for the mix: %.1f ms of a %.1f ms mix", during * 1e3, mixing * 1e3);
        else
            printf("  (RenderAudio %.2f ms, the mix %.2f ms: the mix ended first, the wait not tested)\n", (r1 - r0) * 1e3, mixing * 1e3);
    }

    // a render as the offline tool does: the sum must not be NaN
    int nan = 0;
    for (int i = 0; i < RATE * 2; i++) nan |= isnan(out[i]);
    CHECK(!nan, "NaN in the mix");
    wwplay_close();
    printf("%s (%d failure%s)\n", fails ? "FAILED" : "test_wwplay: all passed", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
