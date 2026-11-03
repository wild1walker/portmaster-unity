// ua_host's own Wwise player (wwplay.c): the backend of the built-in Wwise stand-in (wwise.h) that
// plays a game's banks from the files pmu's setup writes (docs/plugins.md, "Wwise", "The player").
// These calls are for the offline renderer (wwrender.c) and the unit tests; ua_host uses only
// wwplay_backend_get (wwise.h).
#pragma once
#include "wwise.h"
#include <stdio.h>

// a player on the banks in dir (UA_WWISE_DATA's role); any earlier one is closed first
const ua_wwise_backend *wwplay_open(const ua_wwise_host *host, const char *dir);
void wwplay_close(void);

typedef struct {
    unsigned long posted, played, voices_started, dropped, limited, decode_errors;
    int voices, voices_peak, playing, musics;
    double mix_cpu_s, call_cpu_s, mixed_s;      // CPU seconds in mix() and in the game's calls; audio seconds mixed
} wwplay_stats;
void wwplay_get_stats(wwplay_stats *s);

// 16-bit PCM WAV writer (the sizes are rewritten every second, so a killed program leaves a valid file)
typedef struct { FILE *f; int rate, ch; unsigned long frames, since; } wwplay_wav;
int wwplay_wav_open(wwplay_wav *w, const char *path, int rate, int ch);
void wwplay_wav_write(wwplay_wav *w, const float *buf, int frames);
void wwplay_wav_close(wwplay_wav *w);
