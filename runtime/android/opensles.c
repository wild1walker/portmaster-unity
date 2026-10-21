// libOpenSLES.so on SDL2 audio: engine, output mix, and PCM buffer-queue audio players.
//
// This is the subset FMOD (statically linked in libunity) uses, see README.md:
//   slCreateEngine -> Object.Realize/GetInterface(SL_IID_ENGINE) -> Engine.CreateOutputMix ->
//   Engine.CreateAudioPlayer(src = Android simple buffer queue + PCM, sink = output mix,
//   ids = {ANDROIDSIMPLEBUFFERQUEUE, ANDROIDCONFIGURATION}) -> AndroidConfiguration.SetConfiguration
//   (stream type) -> Realize -> GetInterface(PLAY, ANDROIDSIMPLEBUFFERQUEUE) -> BQ.RegisterCallback
//   -> BQ.Enqueue x N -> Play.SetPlayState(PLAYING/STOPPED) -> Object.Destroy.
// Each audio player opens its own SDL audio device (SDL converts/mixes). The SDL callback consumes
// the enqueued buffers in order and calls the buffer-queue callback (without our lock held) each
// time a buffer is used up, like Android's AudioTrack callback thread does.
//
// Output (ANDROID_SLES_OUTPUT): sdl = an SDL audio device; pipe = raw PCM piped into a sound-server
// client (ANDROID_SLES_PIPE_CMD, else pw-cat for PipeWire or pacat for PulseAudio, found through
// XDG_RUNTIME_DIR, else the system's aplay on ALSA alone) - for 32-bit programs on a 64-bit system
// whose sound server they cannot reach directly (PipeWire holding the card, no 32-bit client
// plugins) or whose 32-bit alsa-lib cannot drive the kernel; auto (default) = sdl, else pipe.
#define _GNU_SOURCE
#include "sles_min.h"
#include "shim_internal.h"

#include <SDL2/SDL.h>
#include <math.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

// ---- interface IDs (values from Android's OpenSLES_IID.cpp) --------------------------------
#define IID(name, a, b, c, d, n0, n1, n2, n3, n4, n5) \
    static const struct SLInterfaceID_ iid_##name = { a, b, c, d, { n0, n1, n2, n3, n4, n5 } }; \
    const SLInterfaceID SL_IID_##name = &iid_##name;
IID(NULL, 0xec7178ec, 0xe5e1, 0x4432, 0xa3f4, 0x46, 0x57, 0xe6, 0x79, 0x52, 0x10)
IID(OBJECT, 0x79216360, 0xddd7, 0x11db, 0xac16, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(ENGINE, 0x8d97c260, 0xddd4, 0x11db, 0x958f, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(ENGINECAPABILITIES, 0x8320d0a0, 0xddd5, 0x11db, 0xa1b1, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(OUTPUTMIX, 0x97750f60, 0xddd7, 0x11db, 0x92b1, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(PLAY, 0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(RECORD, 0xc5657aa0, 0xdddb, 0x11db, 0x82f7, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(VOLUME, 0x09e8ede0, 0xddde, 0x11db, 0xb4f6, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(BUFFERQUEUE, 0x2bc99cc0, 0xddd4, 0x11db, 0x8d99, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(ANDROIDSIMPLEBUFFERQUEUE, 0x198e4940, 0xc5d7, 0x11df, 0xa2a6, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(ANDROIDCONFIGURATION, 0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(EFFECTSEND, 0x56e7d200, 0xddd4, 0x11db, 0xaefb, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(ANDROIDEFFECTSEND, 0x7be462c0, 0xbc43, 0x11df, 0x8670, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(MUTESOLO, 0x5a28ebe0, 0xddd7, 0x11db, 0x8220, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(PLAYBACKRATE, 0x2e3b2a40, 0xddda, 0x11db, 0xa349, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(SEEK, 0xd43135a0, 0xdddc, 0x11db, 0xb458, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(PREFETCHSTATUS, 0x2a41ee80, 0xddd8, 0x11db, 0xa41f, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(METADATAEXTRACTION, 0xaa5b1f80, 0xddd6, 0x11db, 0xac8e, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(ANDROIDEFFECT, 0xae12da60, 0x99ac, 0x11df, 0xb456, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)
IID(ANDROIDBUFFERQUEUESOURCE, 0x7fc1a460, 0xeec1, 0x11df, 0xa306, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)

static int iid_eq(SLInterfaceID a, SLInterfaceID b)
{
    return a && b && (a == b || memcmp(a, b, sizeof *a) == 0);
}

static const char *iid_name(SLInterfaceID id)
{
#define N(x) if (iid_eq(id, SL_IID_##x)) return #x;
    N(OBJECT) N(ENGINE) N(ENGINECAPABILITIES) N(OUTPUTMIX) N(PLAY) N(RECORD) N(VOLUME) N(BUFFERQUEUE)
    N(ANDROIDSIMPLEBUFFERQUEUE) N(ANDROIDCONFIGURATION) N(EFFECTSEND) N(ANDROIDEFFECTSEND) N(MUTESOLO)
    N(PLAYBACKRATE) N(SEEK) N(PREFETCHSTATUS) N(METADATAEXTRACTION) N(ANDROIDEFFECT)
    N(ANDROIDBUFFERQUEUESOURCE) N(NULL)
#undef N
    return "unknown";
}

// ---- objects --------------------------------------------------------------------------------
enum kind { K_ENGINE, K_OUTPUTMIX, K_PLAYER };

struct obj;
// An interface handle points at `vtbl` (SLxxxItf is `const struct SLxxxItf_ *const *`).
typedef struct itf { const void *vtbl; struct obj *obj; } itf;

struct qbuf { const uint8_t *ptr; SLuint32 size; };

typedef struct obj {
    itf object;                   // SLObjectItf
    enum kind kind;
    SLuint32 state;
    slObjectCallback obj_cb;
    void *obj_cb_ctx;
    SLint32 priority;
    SLboolean preemptable;
    union {
        struct { itf engine; } e;
        struct { itf mix; } m;
        struct {
            itf play, bq, volume, config;
            pthread_mutex_t lock;
            SDL_AudioDeviceID dev;
            SDL_AudioSpec spec;           // requested == obtained (SDL converts)
            int frame_bytes;
            int is_bq;                    // source is a buffer queue
            SLuint32 play_state;
            slPlayCallback play_cb;
            void *play_cb_ctx;
            SLuint32 play_mask;
            uint64_t bytes_played;        // since last STOPPED
            struct qbuf *queue;
            SLuint32 cap, front, count, play_index, consumed;
            slBufferQueueCallback bq_cb;
            void *bq_cb_ctx;
            SLmillibel level;
            SLboolean mute;
            float gain;                   // from level/mute
            SLint32 stream_type, perf_mode;
            unsigned underruns;
            int starved;                  // queue ran dry while playing (HEADATEND sent)
            int pipe_fd;                  // pipe output (instead of dev): write end, -1 if none
            pid_t pipe_pid;
            pthread_t pipe_thread;
            volatile int pipe_run;
        } p;
    };
} obj;

#define OBJ(self) (((itf *)(self))->obj)

static int g_sdl_audio_users;
static pthread_mutex_t g_sdl_lock = PTHREAD_MUTEX_INITIALIZER;

static int sdl_audio_acquire(void)
{
    pthread_mutex_lock(&g_sdl_lock);
    int ok = 1;
    if (g_sdl_audio_users == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SHIM_WARN("OpenSL: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
        ok = 0;
    }
    if (ok) {
        g_sdl_audio_users++;
        if (g_sdl_audio_users == 1) SHIM_TRACE("OpenSL: SDL audio driver %s", SDL_GetCurrentAudioDriver());
    }
    pthread_mutex_unlock(&g_sdl_lock);
    return ok;
}

static void sdl_audio_release(void)
{
    pthread_mutex_lock(&g_sdl_lock);
    if (g_sdl_audio_users > 0 && --g_sdl_audio_users == 0) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    pthread_mutex_unlock(&g_sdl_lock);
}

// ---- audio player: SDL callback -------------------------------------------------------------
static void apply_gain(obj *o, uint8_t *buf, int len)
{
    float g = o->p.gain;
    if (g == 1.0f) return;
    switch (o->p.spec.format) {
    case AUDIO_S16LSB: {
        int16_t *s = (int16_t *)buf;
        for (int i = 0; i < len / 2; i++) s[i] = (int16_t)(s[i] * g);
        break;
    }
    case AUDIO_S32LSB: {
        int32_t *s = (int32_t *)buf;
        for (int i = 0; i < len / 4; i++) s[i] = (int32_t)(s[i] * (double)g);
        break;
    }
    case AUDIO_F32LSB: {
        float *s = (float *)buf;
        for (int i = 0; i < len / 4; i++) s[i] *= g;
        break;
    }
    case AUDIO_U8:
        for (int i = 0; i < len; i++) buf[i] = (uint8_t)(128 + (buf[i] - 128) * g);
        break;
    }
}

static void player_fill(obj *o, Uint8 *stream, int len)
{
    int off = 0;
    pthread_mutex_lock(&o->p.lock);
    while (off < len && o->p.play_state == SL_PLAYSTATE_PLAYING) {
        if (o->p.count == 0) {
            if (o->p.starved) break;
            o->p.starved = 1;
            o->p.underruns++;
            if (o->p.play_cb && (o->p.play_mask & SL_PLAYEVENT_HEADATEND)) {
                slPlayCallback cb = o->p.play_cb;
                void *ctx = o->p.play_cb_ctx;
                pthread_mutex_unlock(&o->p.lock);
                cb((SLPlayItf)&o->p.play, ctx, SL_PLAYEVENT_HEADATEND);
                pthread_mutex_lock(&o->p.lock);
            }
            break;
        }
        struct qbuf *b = &o->p.queue[o->p.front];
        SLuint32 n = b->size - o->p.consumed;
        if (n > (SLuint32)(len - off)) n = len - off;
        memcpy(stream + off, b->ptr + o->p.consumed, n);
        off += n;
        o->p.consumed += n;
        o->p.bytes_played += n;
        if (o->p.consumed >= b->size) {
            o->p.consumed = 0;
            o->p.front = (o->p.front + 1) % o->p.cap;
            o->p.count--;
            o->p.play_index++;
            if (o->p.bq_cb) {
                slBufferQueueCallback cb = o->p.bq_cb;
                void *ctx = o->p.bq_cb_ctx;
                pthread_mutex_unlock(&o->p.lock);      // the callback usually Enqueues
                cb((SLBufferQueueItf)&o->p.bq, ctx);
                pthread_mutex_lock(&o->p.lock);
            }
        }
    }
    apply_gain(o, stream, off);
    pthread_mutex_unlock(&o->p.lock);
    if (off < len) memset(stream + off, o->p.spec.silence, len - off);
}

static void player_sdl_callback(void *ud, Uint8 *stream, int len) { player_fill((obj *)ud, stream, len); }

// ---- audio player: pipe output ---------------------------------------------------------------
static void *pipe_writer(void *ud)
{
    obj *o = ud;
    int len = 256 * o->p.frame_bytes;          // ~5 ms per write at 48 kHz
    Uint8 *buf = malloc(len);
    while (buf && o->p.pipe_run) {
        player_fill(o, buf, len);
        for (int done = 0; done < len && o->p.pipe_run; ) {   // blocks while the pipe is full: pacing
            ssize_t k = write(o->p.pipe_fd, buf + done, len - done);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) { SHIM_WARN("OpenSL: audio pipe closed (%s)", k < 0 ? strerror(errno) : "EOF"); o->p.pipe_run = 0; break; }
            done += k;
        }
    }
    free(buf);
    return NULL;
}

static int in_path(const char *prog)
{
    const char *path = getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin";
    char dir[512], f[600];
    while (*path) {
        size_t n = strcspn(path, ":");
        snprintf(dir, sizeof dir, "%.*s", (int)n, path);
        snprintf(f, sizeof f, "%s/%s", dir, prog);
        if (access(f, X_OK) == 0) return 1;
        path += n + (path[n] == ':');
    }
    return 0;
}

static const char *pipe_format(SDL_AudioFormat f, int pulse)
{
    switch (f) {
    case AUDIO_S16LSB: return "s16le";
    case AUDIO_F32LSB: return pulse ? "float32le" : "f32";
    case AUDIO_S32LSB: return "s32le";
    case AUDIO_U8: return "u8";
    default: return NULL;
    }
}

static const char *aplay_format(SDL_AudioFormat f)
{
    switch (f) {
    case AUDIO_S16LSB: return "S16_LE";
    case AUDIO_F32LSB: return "FLOAT_LE";
    case AUDIO_S32LSB: return "S32_LE";
    case AUDIO_U8: return "U8";
    default: return NULL;
    }
}

// ALSA alone (no sound server; the 32-bit alsa-lib can fail where the system's 64-bit one works,
// e.g. Debian's time64 libasound2t64 on kernels before 5.6): the system's aplay, trying the devices
// of ANDROID_SLES_APLAY_DEVICES in order (default "plughw:0,0 default": the card directly, without
// dmix's buffer, else the system's default device when the card is busy). aplay opens the device
// before it reads stdin, so a device that fails to open loses no audio. 0 if it does not fit.
static int aplay_cmd(char *cmd, size_t n, const char *fmt, int rate, int ch)
{
    const char *list = getenv("ANDROID_SLES_APLAY_DEVICES");
    if (!list || !*list) list = "plughw:0,0 default";
    size_t len = 0;
    int k = 0;
    cmd[0] = 0;
    for (const char *p = list; *p; ) {
        size_t m = strcspn(p, " ");
        if (m && m < 64 && strspn(p, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789:,_=.-") >= m) {
            int last = !p[m] || !p[m + strspn(p + m, " ")];
            int w = snprintf(cmd + len, n - len, "%s%saplay -q -t raw -f %s -r %d -c %d -D %.*s --buffer-time=40000 --period-time=10000 -%s",
                             k ? " || { echo 'OpenSL: aplay: next device' >&2; " : "", last ? "exec " : "",
                             fmt, rate, ch, (int)m, p, last ? "" : " 2>/dev/null");
            if (w < 0 || (size_t)w >= n - len) return 0;
            len += w; k++;
        }
        p += m;
        p += strspn(p, " ");
    }
    for (int i = 1; i < k; i++) {               // close the { ...; } groups
        if (len + 3 >= n) return 0;
        memcpy(cmd + len, "; }", 4); len += 3;
    }
    return k > 0;
}

static int pipe_open(obj *o)
{
    char cmd[512];
    const char *user = getenv("ANDROID_SLES_PIPE_CMD"), *rt = getenv("XDG_RUNTIME_DIR");
    const char *fmt = pipe_format(o->p.spec.format, 0), *pfmt = pipe_format(o->p.spec.format, 1);
    char sock[300];
    if (user && *user)
        snprintf(cmd, sizeof cmd, "%s", user);
    else if (fmt && rt && (snprintf(sock, sizeof sock, "%s/pipewire-0", rt), access(sock, F_OK) == 0) && in_path("pw-cat"))
        snprintf(cmd, sizeof cmd, "exec pw-cat --playback --raw --format %s --rate %d --channels %d --latency 20ms -",
                 fmt[0] == 's' && fmt[1] == '1' ? "s16" : fmt[0] == 's' ? "s32" : fmt, o->p.spec.freq, o->p.spec.channels);
    else if (pfmt && rt && (snprintf(sock, sizeof sock, "%s/pulse/native", rt), access(sock, F_OK) == 0) && in_path("pacat"))
        snprintf(cmd, sizeof cmd, "exec pacat --playback --raw --format=%s --rate=%d --channels=%d --latency-msec=20",
                 pfmt, o->p.spec.freq, o->p.spec.channels);
    else if (aplay_format(o->p.spec.format) && in_path("aplay") &&
             aplay_cmd(cmd, sizeof cmd, aplay_format(o->p.spec.format), o->p.spec.freq, o->p.spec.channels))
        ;
    else
        return 0;
    int fds[2];
    if (pipe(fds) != 0) return 0;
    fcntl(fds[1], F_SETPIPE_SZ, 4096);         // one page: ~21 ms of 48 kHz stereo s16 in flight
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    // the client is a program of the host system: none of our (32-bit) library overrides
    char *envp[256];
    int ne = 0;
    for (char **e = environ; *e && ne < 255; e++)
        if (strncmp(*e, "LD_LIBRARY_PATH=", 16) && strncmp(*e, "LD_PRELOAD=", 11)) envp[ne++] = *e;
    envp[ne] = NULL;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[0], 0);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    char *argv[] = { "/bin/sh", "-c", cmd, NULL };
    int r = posix_spawn(&o->p.pipe_pid, "/bin/sh", &fa, NULL, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[0]);
    if (r != 0) { close(fds[1]); return 0; }
    signal(SIGPIPE, SIG_IGN);                  // a client that dies must not take the game with it
    o->p.pipe_fd = fds[1];
    o->p.pipe_run = 1;
    if (pthread_create(&o->p.pipe_thread, NULL, pipe_writer, o) != 0) {
        o->p.pipe_run = 0; close(fds[1]); o->p.pipe_fd = -1;
        waitpid(o->p.pipe_pid, NULL, 0);
        return 0;
    }
    pthread_setname_np(o->p.pipe_thread, "SLESPipe");
    SHIM_WARN("OpenSL: player %p -> pipe: %s", (void *)o, cmd);
    return 1;
}

static void pipe_close(obj *o)
{
    if (o->p.pipe_fd < 0) return;
    o->p.pipe_run = 0;
    close(o->p.pipe_fd);                       // EOF for the client; a blocked write returns EPIPE
    pthread_join(o->p.pipe_thread, NULL);
    o->p.pipe_fd = -1;
    waitpid(o->p.pipe_pid, NULL, 0);
}

// ---- Object interface -----------------------------------------------------------------------
static SLresult player_realize(obj *o)
{
    if (!o->p.is_bq) return SL_RESULT_CONTENT_UNSUPPORTED;
    const char *mode = getenv("ANDROID_SLES_OUTPUT") ? getenv("ANDROID_SLES_OUTPUT") : "auto";
    o->p.pipe_fd = -1;
    if (!strcmp(mode, "pipe")) {
        if (pipe_open(o)) return SL_RESULT_SUCCESS;
        SHIM_WARN("OpenSL: no sound-server client for pipe output (ANDROID_SLES_PIPE_CMD, pw-cat, pacat)");
        return SL_RESULT_RESOURCE_ERROR;
    }
    if (!sdl_audio_acquire()) {
        if (!strcmp(mode, "auto") && pipe_open(o)) return SL_RESULT_SUCCESS;
        return SL_RESULT_RESOURCE_ERROR;
    }
    SDL_AudioSpec want = o->p.spec, have;
    want.callback = player_sdl_callback;
    want.userdata = o;
    const char *dev = getenv("ANDROID_SLES_DEVICE");
    o->p.dev = SDL_OpenAudioDevice(dev && *dev ? dev : NULL, 0, &want, &have, 0);
    if (!o->p.dev) {
        SHIM_WARN("OpenSL: SDL_OpenAudioDevice(%d Hz, %d ch, fmt 0x%x) failed: %s",
                  want.freq, want.channels, want.format, SDL_GetError());
        sdl_audio_release();
        if (!strcmp(mode, "auto") && pipe_open(o)) return SL_RESULT_SUCCESS;
        return SL_RESULT_RESOURCE_ERROR;
    }
    o->p.spec.silence = have.silence;
    SDL_PauseAudioDevice(o->p.dev, 0);   // the callback outputs silence until SL_PLAYSTATE_PLAYING
    SHIM_TRACE("OpenSL: player %p -> SDL device %u: %d Hz, %d ch, fmt 0x%x, %d frames", (void *)o,
               o->p.dev, have.freq, have.channels, have.format, have.samples);
    return SL_RESULT_SUCCESS;
}

static SLresult Object_Realize(SLObjectItf self, SLboolean async)
{
    obj *o = OBJ(self);
    if (o->state != SL_OBJECT_STATE_UNREALIZED) return SL_RESULT_PRECONDITIONS_VIOLATED;
    SLresult r = SL_RESULT_SUCCESS;
    if (o->kind == K_ENGINE) r = sdl_audio_acquire() ? SL_RESULT_SUCCESS : SL_RESULT_RESOURCE_ERROR;
    else if (o->kind == K_PLAYER) r = player_realize(o);
    if (r == SL_RESULT_SUCCESS) o->state = SL_OBJECT_STATE_REALIZED;
    if (async && o->obj_cb) o->obj_cb(self, o->obj_cb_ctx, SL_OBJECT_EVENT_ASYNC_TERMINATION, r, o->state, NULL);
    return r;
}

static SLresult Object_Resume(SLObjectItf self, SLboolean async)
{
    (void)async;
    return OBJ(self)->state == SL_OBJECT_STATE_SUSPENDED ? SL_RESULT_SUCCESS : SL_RESULT_PRECONDITIONS_VIOLATED;
}

static SLresult Object_GetState(SLObjectItf self, SLuint32 *pState)
{
    if (!pState) return SL_RESULT_PARAMETER_INVALID;
    *pState = OBJ(self)->state;
    return SL_RESULT_SUCCESS;
}

static itf *find_itf(obj *o, SLInterfaceID id)
{
    if (iid_eq(id, SL_IID_OBJECT)) return &o->object;
    switch (o->kind) {
    case K_ENGINE:
        if (iid_eq(id, SL_IID_ENGINE)) return &o->e.engine;
        break;
    case K_OUTPUTMIX:
        if (iid_eq(id, SL_IID_OUTPUTMIX)) return &o->m.mix;
        break;
    case K_PLAYER:
        if (iid_eq(id, SL_IID_PLAY)) return &o->p.play;
        if (iid_eq(id, SL_IID_VOLUME)) return &o->p.volume;
        if (iid_eq(id, SL_IID_ANDROIDCONFIGURATION)) return &o->p.config;
        if (o->p.is_bq && (iid_eq(id, SL_IID_ANDROIDSIMPLEBUFFERQUEUE) || iid_eq(id, SL_IID_BUFFERQUEUE)))
            return &o->p.bq;
        break;
    }
    return NULL;
}

static SLresult Object_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *pInterface)
{
    obj *o = OBJ(self);
    if (!pInterface) return SL_RESULT_PARAMETER_INVALID;
    *(void **)pInterface = NULL;
    itf *i = find_itf(o, iid);
    SHIM_TRACE("OpenSL: GetInterface(%p, %s) -> %p", (void *)o, iid_name(iid), (void *)i);
    if (!i) return SL_RESULT_FEATURE_UNSUPPORTED;
    // Android allows the configuration interface before Realize (it must be used before it).
    if (o->state != SL_OBJECT_STATE_REALIZED && !(o->kind == K_PLAYER && i == &o->p.config))
        return SL_RESULT_PRECONDITIONS_VIOLATED;
    *(void **)pInterface = i;
    return SL_RESULT_SUCCESS;
}

static SLresult Object_RegisterCallback(SLObjectItf self, slObjectCallback cb, void *ctx)
{
    obj *o = OBJ(self);
    o->obj_cb = cb;
    o->obj_cb_ctx = ctx;
    return SL_RESULT_SUCCESS;
}

static void Object_AbortAsyncOperation(SLObjectItf self) { (void)self; }

static void Object_Destroy(SLObjectItf self)
{
    obj *o = OBJ(self);
    SHIM_TRACE("OpenSL: Destroy(%p kind %d)", (void *)o, o->kind);
    if (o->kind == K_PLAYER) {
        if (o->p.dev) {
            SDL_CloseAudioDevice(o->p.dev);   // waits for a running callback; we hold no lock
            o->p.dev = 0;
            if (o->state == SL_OBJECT_STATE_REALIZED) sdl_audio_release();
        }
        pipe_close(o);
        if (o->p.underruns) SHIM_TRACE("OpenSL: player %p had %u underruns", (void *)o, o->p.underruns);
        pthread_mutex_destroy(&o->p.lock);
        free(o->p.queue);
    } else if (o->kind == K_ENGINE && o->state == SL_OBJECT_STATE_REALIZED) {
        sdl_audio_release();
    }
    free(o);
}

static SLresult Object_SetPriority(SLObjectItf self, SLint32 priority, SLboolean preemptable)
{
    obj *o = OBJ(self);
    o->priority = priority;
    o->preemptable = preemptable;
    return SL_RESULT_SUCCESS;
}

static SLresult Object_GetPriority(SLObjectItf self, SLint32 *pPriority, SLboolean *pPreemptable)
{
    obj *o = OBJ(self);
    if (!pPriority || !pPreemptable) return SL_RESULT_PARAMETER_INVALID;
    *pPriority = o->priority;
    *pPreemptable = o->preemptable;
    return SL_RESULT_SUCCESS;
}

static SLresult Object_SetLossOfControlInterfaces(SLObjectItf self, SLint16 n, SLInterfaceID *ids, SLboolean en)
{
    (void)self; (void)n; (void)ids; (void)en;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const struct SLObjectItf_ object_vtbl = {
    Object_Realize, Object_Resume, Object_GetState, Object_GetInterface, Object_RegisterCallback,
    Object_AbortAsyncOperation, Object_Destroy, Object_SetPriority, Object_GetPriority,
    Object_SetLossOfControlInterfaces,
};

static obj *obj_new(enum kind k)
{
    obj *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->object = (itf){ &object_vtbl, o };
    o->kind = k;
    o->state = SL_OBJECT_STATE_UNREALIZED;
    return o;
}

// Interfaces an object can provide, to check `required` requests at creation.
static int check_required(enum kind k, int is_bq, SLuint32 n, const SLInterfaceID *ids, const SLboolean *req)
{
    if (n && !ids) return 0;
    obj probe = { .kind = k };
    probe.p.is_bq = is_bq;
    for (SLuint32 i = 0; i < n; i++) {
        int have = find_itf(&probe, ids[i]) != NULL;
        if (!have) {
            SHIM_TRACE("OpenSL: interface %s not available (%s)", iid_name(ids[i]),
                       req && req[i] ? "required" : "optional");
            if (req && req[i]) return 0;
        }
    }
    return 1;
}

// ---- Play interface -------------------------------------------------------------------------
static SLresult Play_SetPlayState(SLPlayItf self, SLuint32 state)
{
    obj *o = OBJ(self);
    if (state < SL_PLAYSTATE_STOPPED || state > SL_PLAYSTATE_PLAYING) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&o->p.lock);
    SLuint32 old = o->p.play_state;
    o->p.play_state = state;
    if (state == SL_PLAYSTATE_STOPPED) o->p.bytes_played = 0;   // position resets; the queue is kept
    pthread_mutex_unlock(&o->p.lock);
    SHIM_TRACE("OpenSL: player %p play state %u -> %u (queued %u)", (void *)o, old, state, o->p.count);
    // The SDL device keeps running (silence unless PLAYING): no SDL call here, so SetPlayState never
    // waits on SDL's device lock while the audio thread is inside an FMOD callback.
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetPlayState(SLPlayItf self, SLuint32 *pState)
{
    if (!pState) return SL_RESULT_PARAMETER_INVALID;
    *pState = OBJ(self)->p.play_state;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetDuration(SLPlayItf self, SLmillisecond *pMsec)
{
    (void)self;
    if (!pMsec) return SL_RESULT_PARAMETER_INVALID;
    *pMsec = SL_TIME_UNKNOWN;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetPosition(SLPlayItf self, SLmillisecond *pMsec)
{
    obj *o = OBJ(self);
    if (!pMsec) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&o->p.lock);
    uint64_t frames = o->p.frame_bytes ? o->p.bytes_played / o->p.frame_bytes : 0;
    pthread_mutex_unlock(&o->p.lock);
    *pMsec = o->p.spec.freq ? (SLmillisecond)(frames * 1000 / o->p.spec.freq) : 0;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_RegisterCallback(SLPlayItf self, slPlayCallback cb, void *ctx)
{
    obj *o = OBJ(self);
    pthread_mutex_lock(&o->p.lock);
    o->p.play_cb = cb;
    o->p.play_cb_ctx = ctx;
    pthread_mutex_unlock(&o->p.lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Play_SetCallbackEventsMask(SLPlayItf self, SLuint32 flags)
{
    OBJ(self)->p.play_mask = flags;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetCallbackEventsMask(SLPlayItf self, SLuint32 *flags)
{
    if (!flags) return SL_RESULT_PARAMETER_INVALID;
    *flags = OBJ(self)->p.play_mask;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_SetMarkerPosition(SLPlayItf self, SLmillisecond ms) { (void)self; (void)ms; return SL_RESULT_SUCCESS; }
static SLresult Play_ClearMarkerPosition(SLPlayItf self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult Play_GetMarkerPosition(SLPlayItf self, SLmillisecond *ms)
{ (void)self; if (ms) *ms = SL_TIME_UNKNOWN; return SL_RESULT_PRECONDITIONS_VIOLATED; }
static SLresult Play_SetPositionUpdatePeriod(SLPlayItf self, SLmillisecond ms) { (void)self; (void)ms; return SL_RESULT_SUCCESS; }
static SLresult Play_GetPositionUpdatePeriod(SLPlayItf self, SLmillisecond *ms)
{ (void)self; if (!ms) return SL_RESULT_PARAMETER_INVALID; *ms = 1000; return SL_RESULT_SUCCESS; }

static const struct SLPlayItf_ play_vtbl = {
    Play_SetPlayState, Play_GetPlayState, Play_GetDuration, Play_GetPosition, Play_RegisterCallback,
    Play_SetCallbackEventsMask, Play_GetCallbackEventsMask, Play_SetMarkerPosition,
    Play_ClearMarkerPosition, Play_GetMarkerPosition, Play_SetPositionUpdatePeriod,
    Play_GetPositionUpdatePeriod,
};

// ---- Buffer queue interface (SL_IID_BUFFERQUEUE and SL_IID_ANDROIDSIMPLEBUFFERQUEUE) --------
static SLresult BQ_Enqueue(SLBufferQueueItf self, const void *pBuffer, SLuint32 size)
{
    obj *o = OBJ(self);
    if (!pBuffer || !size) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&o->p.lock);
    SLresult r = SL_RESULT_SUCCESS;
    if (o->p.count >= o->p.cap) {
        r = SL_RESULT_BUFFER_INSUFFICIENT;
    } else {
        o->p.queue[(o->p.front + o->p.count) % o->p.cap] = (struct qbuf){ pBuffer, size };
        o->p.count++;
        o->p.starved = 0;
    }
    pthread_mutex_unlock(&o->p.lock);
    return r;
}

static SLresult BQ_Clear(SLBufferQueueItf self)
{
    obj *o = OBJ(self);
    pthread_mutex_lock(&o->p.lock);
    o->p.front = o->p.count = o->p.consumed = o->p.play_index = 0;
    pthread_mutex_unlock(&o->p.lock);
    return SL_RESULT_SUCCESS;
}

static SLresult BQ_GetState(SLBufferQueueItf self, SLBufferQueueState *st)
{
    obj *o = OBJ(self);
    if (!st) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&o->p.lock);
    st->count = o->p.count;
    st->index = o->p.play_index;
    pthread_mutex_unlock(&o->p.lock);
    return SL_RESULT_SUCCESS;
}

static SLresult BQ_RegisterCallback(SLBufferQueueItf self, slBufferQueueCallback cb, void *ctx)
{
    obj *o = OBJ(self);
    pthread_mutex_lock(&o->p.lock);
    SLresult r = SL_RESULT_SUCCESS;
    if (o->p.play_state != SL_PLAYSTATE_STOPPED) {
        r = SL_RESULT_PRECONDITIONS_VIOLATED;   // as on Android
    } else {
        o->p.bq_cb = cb;
        o->p.bq_cb_ctx = ctx;
    }
    pthread_mutex_unlock(&o->p.lock);
    return r;
}

static const struct SLBufferQueueItf_ bq_vtbl = { BQ_Enqueue, BQ_Clear, BQ_GetState, BQ_RegisterCallback };

// ---- Volume interface -----------------------------------------------------------------------
static void update_gain(obj *o)
{
    float g = o->p.mute || o->p.level <= SL_MILLIBEL_MIN ? 0.0f : powf(10.0f, o->p.level / 2000.0f);
    if (g > 1.0f) g = 1.0f;
    o->p.gain = g;
}

static SLresult Vol_SetVolumeLevel(SLVolumeItf self, SLmillibel level)
{
    obj *o = OBJ(self);
    if (level > 0) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&o->p.lock);
    o->p.level = level;
    update_gain(o);
    pthread_mutex_unlock(&o->p.lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_GetVolumeLevel(SLVolumeItf self, SLmillibel *level)
{
    if (!level) return SL_RESULT_PARAMETER_INVALID;
    *level = OBJ(self)->p.level;
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_GetMaxVolumeLevel(SLVolumeItf self, SLmillibel *level)
{
    (void)self;
    if (!level) return SL_RESULT_PARAMETER_INVALID;
    *level = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_SetMute(SLVolumeItf self, SLboolean mute)
{
    obj *o = OBJ(self);
    pthread_mutex_lock(&o->p.lock);
    o->p.mute = mute ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
    update_gain(o);
    pthread_mutex_unlock(&o->p.lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_GetMute(SLVolumeItf self, SLboolean *mute)
{
    if (!mute) return SL_RESULT_PARAMETER_INVALID;
    *mute = OBJ(self)->p.mute;
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_EnableStereoPosition(SLVolumeItf self, SLboolean en)
{ (void)self; return en ? SL_RESULT_FEATURE_UNSUPPORTED : SL_RESULT_SUCCESS; }
static SLresult Vol_IsEnabledStereoPosition(SLVolumeItf self, SLboolean *en)
{ (void)self; if (!en) return SL_RESULT_PARAMETER_INVALID; *en = SL_BOOLEAN_FALSE; return SL_RESULT_SUCCESS; }
static SLresult Vol_SetStereoPosition(SLVolumeItf self, SLpermille pos) { (void)self; (void)pos; return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult Vol_GetStereoPosition(SLVolumeItf self, SLpermille *pos)
{ (void)self; if (!pos) return SL_RESULT_PARAMETER_INVALID; *pos = 0; return SL_RESULT_SUCCESS; }

static const struct SLVolumeItf_ volume_vtbl = {
    Vol_SetVolumeLevel, Vol_GetVolumeLevel, Vol_GetMaxVolumeLevel, Vol_SetMute, Vol_GetMute,
    Vol_EnableStereoPosition, Vol_IsEnabledStereoPosition, Vol_SetStereoPosition, Vol_GetStereoPosition,
};

// ---- Android configuration interface --------------------------------------------------------
static SLresult Cfg_SetConfiguration(SLAndroidConfigurationItf self, const SLchar *key, const void *val, SLuint32 size)
{
    obj *o = OBJ(self);
    if (!key || !val || size < sizeof(SLint32)) return SL_RESULT_PARAMETER_INVALID;
    SLint32 v;
    memcpy(&v, val, sizeof v);
    SHIM_TRACE("OpenSL: SetConfiguration(%s = %d)", (const char *)key, v);
    if (!strcmp((const char *)key, SL_ANDROID_KEY_STREAM_TYPE)) o->p.stream_type = v;
    else if (!strcmp((const char *)key, SL_ANDROID_KEY_PERFORMANCE_MODE)) o->p.perf_mode = v;
    // Other keys (recording preset, ...) are accepted and ignored.
    return SL_RESULT_SUCCESS;
}

static SLresult Cfg_GetConfiguration(SLAndroidConfigurationItf self, const SLchar *key, SLuint32 *size, void *val)
{
    obj *o = OBJ(self);
    if (!key || !size) return SL_RESULT_PARAMETER_INVALID;
    SLint32 v;
    if (!strcmp((const char *)key, SL_ANDROID_KEY_STREAM_TYPE)) v = o->p.stream_type;
    else if (!strcmp((const char *)key, SL_ANDROID_KEY_PERFORMANCE_MODE)) v = o->p.perf_mode;
    else return SL_RESULT_PARAMETER_INVALID;
    if (!val) { *size = sizeof v; return SL_RESULT_SUCCESS; }
    if (*size < sizeof v) return SL_RESULT_BUFFER_INSUFFICIENT;
    memcpy(val, &v, sizeof v);
    *size = sizeof v;
    return SL_RESULT_SUCCESS;
}

static SLresult Cfg_AcquireJavaProxy(SLAndroidConfigurationItf self, SLuint32 type, void **obj)
{ (void)self; (void)type; if (obj) *obj = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult Cfg_ReleaseJavaProxy(SLAndroidConfigurationItf self, SLuint32 type)
{ (void)self; (void)type; return SL_RESULT_PRECONDITIONS_VIOLATED; }

static const struct SLAndroidConfigurationItf_ config_vtbl = {
    Cfg_SetConfiguration, Cfg_GetConfiguration, Cfg_AcquireJavaProxy, Cfg_ReleaseJavaProxy,
};

// ---- Output mix interface -------------------------------------------------------------------
static SLresult Mix_GetDestinationOutputDeviceIDs(SLOutputMixItf self, SLint32 *num, SLuint32 *ids)
{
    (void)self;
    if (!num) return SL_RESULT_PARAMETER_INVALID;
    if (ids && *num >= 1) ids[0] = 0xFFFFFFFEu;   // SL_DEFAULTDEVICEID_AUDIOOUTPUT
    *num = 1;
    return SL_RESULT_SUCCESS;
}
static SLresult Mix_RegisterDeviceChangeCallback(SLOutputMixItf self, void *cb, void *ctx)
{ (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
static SLresult Mix_ReRoute(SLOutputMixItf self, SLint32 n, SLuint32 *ids)
{ (void)self; (void)n; (void)ids; return SL_RESULT_FEATURE_UNSUPPORTED; }

static const struct SLOutputMixItf_ mix_vtbl = {
    Mix_GetDestinationOutputDeviceIDs, Mix_RegisterDeviceChangeCallback, Mix_ReRoute,
};

// ---- Engine interface -----------------------------------------------------------------------
static SLresult pcm_to_sdl(const SLDataFormat_PCM *f, SDL_AudioSpec *spec)
{
    SLuint32 rep = 0;
    if (f->formatType == SL_ANDROID_DATAFORMAT_PCM_EX) rep = ((const SLAndroidDataFormat_PCM_EX *)f)->representation;
    else if (f->formatType != SL_DATAFORMAT_PCM) return SL_RESULT_CONTENT_UNSUPPORTED;
    if (f->numChannels < 1 || f->numChannels > 8 || f->samplesPerSec < 8000 * 1000u) return SL_RESULT_CONTENT_UNSUPPORTED;
    if (f->containerSize && f->containerSize != f->bitsPerSample) return SL_RESULT_CONTENT_UNSUPPORTED;
    int be = f->endianness == SL_BYTEORDER_BIGENDIAN;
    SDL_AudioFormat fmt;
    switch (f->bitsPerSample) {
    case 8: fmt = AUDIO_U8; break;
    case 16: fmt = be ? AUDIO_S16MSB : AUDIO_S16LSB; break;
    case 32: fmt = rep == SL_ANDROID_PCM_REPRESENTATION_FLOAT ? (be ? AUDIO_F32MSB : AUDIO_F32LSB)
                                                               : (be ? AUDIO_S32MSB : AUDIO_S32LSB); break;
    default: return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    memset(spec, 0, sizeof *spec);
    spec->freq = f->samplesPerSec / 1000;
    spec->channels = f->numChannels;
    spec->format = fmt;
    const char *e = getenv("ANDROID_SLES_SAMPLES");
    int samples = e ? atoi(e) : 0;
    spec->samples = samples > 0 ? samples : 512;
    return SL_RESULT_SUCCESS;
}

static SLresult Engine_CreateAudioPlayer(SLEngineItf self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                         SLuint32 n, const SLInterfaceID *ids, const SLboolean *req)
{
    (void)self;
    if (!pPlayer || !src || !src->pLocator || !snk || !snk->pLocator) return SL_RESULT_PARAMETER_INVALID;
    *pPlayer = NULL;
    SLuint32 loc = *(const SLuint32 *)src->pLocator;
    if (loc != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE && loc != SL_DATALOCATOR_BUFFERQUEUE) {
        SHIM_WARN("OpenSL: CreateAudioPlayer: source locator 0x%x not supported (PCM buffer queues only)", loc);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    const SLDataLocator_BufferQueue *bql = src->pLocator;
    if (!bql->numBuffers || !src->pFormat) return SL_RESULT_PARAMETER_INVALID;
    if (*(const SLuint32 *)snk->pLocator != SL_DATALOCATOR_OUTPUTMIX) return SL_RESULT_CONTENT_UNSUPPORTED;
    SDL_AudioSpec spec;
    SLresult r = pcm_to_sdl(src->pFormat, &spec);
    if (r) {
        SHIM_WARN("OpenSL: CreateAudioPlayer: PCM format not supported");
        return r;
    }
    if (!check_required(K_PLAYER, 1, n, ids, req)) return SL_RESULT_FEATURE_UNSUPPORTED;
    obj *o = obj_new(K_PLAYER);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    o->p.queue = calloc(bql->numBuffers, sizeof *o->p.queue);
    if (!o->p.queue) { free(o); return SL_RESULT_MEMORY_FAILURE; }
    o->p.cap = bql->numBuffers;
    o->p.play = (itf){ &play_vtbl, o };
    o->p.bq = (itf){ &bq_vtbl, o };
    o->p.volume = (itf){ &volume_vtbl, o };
    o->p.config = (itf){ &config_vtbl, o };
    pthread_mutex_init(&o->p.lock, NULL);
    o->p.is_bq = 1;
    o->p.spec = spec;
    o->p.frame_bytes = SDL_AUDIO_BITSIZE(spec.format) / 8 * spec.channels;
    o->p.pipe_fd = -1;                         // no pipe output yet (0 would be stdin)
    o->p.pipe_fd = -1;
    o->p.play_state = SL_PLAYSTATE_STOPPED;
    o->p.gain = 1.0f;
    o->p.stream_type = SL_ANDROID_STREAM_MEDIA;
    SHIM_TRACE("OpenSL: CreateAudioPlayer -> %p: %d Hz, %d ch, fmt 0x%x, %u buffers", (void *)o,
               spec.freq, spec.channels, spec.format, bql->numBuffers);
    *pPlayer = (SLObjectItf)&o->object;
    return SL_RESULT_SUCCESS;
}

static SLresult Engine_CreateOutputMix(SLEngineItf self, SLObjectItf *pMix, SLuint32 n, const SLInterfaceID *ids,
                                       const SLboolean *req)
{
    (void)self;
    if (!pMix) return SL_RESULT_PARAMETER_INVALID;
    *pMix = NULL;
    if (!check_required(K_OUTPUTMIX, 0, n, ids, req)) return SL_RESULT_FEATURE_UNSUPPORTED;
    obj *o = obj_new(K_OUTPUTMIX);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    o->m.mix = (itf){ &mix_vtbl, o };
    *pMix = (SLObjectItf)&o->object;
    return SL_RESULT_SUCCESS;
}

static SLresult Engine_CreateAudioRecorder(SLEngineItf self, SLObjectItf *p, SLDataSource *src, SLDataSink *snk,
                                           SLuint32 n, const SLInterfaceID *ids, const SLboolean *req)
{
    (void)self; (void)src; (void)snk; (void)n; (void)ids; (void)req;
    if (p) *p = NULL;
    SHIM_WARN("OpenSL: CreateAudioRecorder: no recording support");
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult Engine_CreateDevice(SLEngineItf s, SLObjectItf *p, SLuint32 id, SLuint32 n, const SLInterfaceID *ids, const SLboolean *req)
{ (void)s; (void)id; (void)n; (void)ids; (void)req; if (p) *p = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult Engine_CreateMidiPlayer(SLEngineItf s, SLObjectItf *p, SLDataSource *a, SLDataSource *b, SLDataSink *c,
                                        SLDataSink *d, SLDataSink *e, SLuint32 n, const SLInterfaceID *ids, const SLboolean *req)
{ (void)s; (void)a; (void)b; (void)c; (void)d; (void)e; (void)n; (void)ids; (void)req; if (p) *p = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult Engine_CreateListener(SLEngineItf s, SLObjectItf *p, SLuint32 n, const SLInterfaceID *ids, const SLboolean *req)
{ (void)s; (void)n; (void)ids; (void)req; if (p) *p = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult Engine_CreateMetadataExtractor(SLEngineItf s, SLObjectItf *p, SLDataSource *src, SLuint32 n,
                                               const SLInterfaceID *ids, const SLboolean *req)
{ (void)s; (void)src; (void)n; (void)ids; (void)req; if (p) *p = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult Engine_CreateExtensionObject(SLEngineItf s, SLObjectItf *p, void *par, SLuint32 id, SLuint32 n,
                                             const SLInterfaceID *ids, const SLboolean *req)
{ (void)s; (void)par; (void)id; (void)n; (void)ids; (void)req; if (p) *p = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult Engine_QueryNumSupportedInterfaces(SLEngineItf s, SLuint32 id, SLuint32 *num)
{ (void)s; (void)id; if (!num) return SL_RESULT_PARAMETER_INVALID; *num = 0; return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult Engine_QuerySupportedInterfaces(SLEngineItf s, SLuint32 id, SLuint32 i, SLInterfaceID *out)
{ (void)s; (void)id; (void)i; if (out) *out = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult Engine_QueryNumSupportedExtensions(SLEngineItf s, SLuint32 *num)
{ (void)s; if (!num) return SL_RESULT_PARAMETER_INVALID; *num = 0; return SL_RESULT_SUCCESS; }
static SLresult Engine_QuerySupportedExtension(SLEngineItf s, SLuint32 i, SLchar *name, SLint16 *len)
{ (void)s; (void)i; (void)name; (void)len; return SL_RESULT_PARAMETER_INVALID; }
static SLresult Engine_IsExtensionSupported(SLEngineItf s, const SLchar *name, SLboolean *sup)
{ (void)s; (void)name; if (!sup) return SL_RESULT_PARAMETER_INVALID; *sup = SL_BOOLEAN_FALSE; return SL_RESULT_SUCCESS; }

static const struct SLEngineItf_ engine_vtbl = {
    Engine_CreateDevice, Engine_CreateDevice, Engine_CreateAudioPlayer, Engine_CreateAudioRecorder,
    Engine_CreateMidiPlayer, Engine_CreateListener, Engine_CreateListener /* Create3DGroup: same shape */,
    Engine_CreateOutputMix, Engine_CreateMetadataExtractor, Engine_CreateExtensionObject,
    Engine_QueryNumSupportedInterfaces, Engine_QuerySupportedInterfaces, Engine_QueryNumSupportedExtensions,
    Engine_QuerySupportedExtension, Engine_IsExtensionSupported,
};

// ---- entry points ---------------------------------------------------------------------------
SLresult slCreateEngine(SLObjectItf *pEngine, SLuint32 numOptions, const SLEngineOption *opts,
                        SLuint32 n, const SLInterfaceID *ids, const SLboolean *req)
{
    (void)numOptions; (void)opts;   // SL_ENGINEOPTION_THREADSAFE etc.: we are always thread-safe
    if (!pEngine) return SL_RESULT_PARAMETER_INVALID;
    *pEngine = NULL;
    if (!check_required(K_ENGINE, 0, n, ids, req)) return SL_RESULT_FEATURE_UNSUPPORTED;
    obj *o = obj_new(K_ENGINE);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    o->e.engine = (itf){ &engine_vtbl, o };
    SHIM_TRACE("OpenSL: slCreateEngine -> %p", (void *)o);
    *pEngine = (SLObjectItf)&o->object;
    return SL_RESULT_SUCCESS;
}

SLresult slQueryNumSupportedEngineInterfaces(SLuint32 *pNum)
{
    if (!pNum) return SL_RESULT_PARAMETER_INVALID;
    *pNum = 2;
    return SL_RESULT_SUCCESS;
}

SLresult slQuerySupportedEngineInterfaces(SLuint32 index, SLInterfaceID *pId)
{
    if (!pId) return SL_RESULT_PARAMETER_INVALID;
    if (index > 1) { *pId = NULL; return SL_RESULT_PARAMETER_INVALID; }
    *pId = index == 0 ? SL_IID_OBJECT : SL_IID_ENGINE;
    return SL_RESULT_SUCCESS;
}
