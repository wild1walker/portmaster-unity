// um_extractor: Android's MediaExtractor over the demuxers (webm.c, mp4.c, libavformat).
//
// As Android's (NuMediaExtractor): every track is listed, only selected tracks give samples, and
// the next sample is the one with the earliest time among the selected tracks (the track index
// breaks ties). seekTo moves each selected track: the video tracks (and every track whose
// samples are not all sync samples) to the sync sample the mode asks for, the others to the
// sample at or before the time the first video track reached.
#define _GNU_SOURCE
#include "um_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- sources ---------------------------------------------------------------------------------- */
int um_source_fd(um_source *s, int fd, int64_t offset, int64_t length)
{
    memset(s, 0, sizeof *s);
    s->fd = -1;
    if (fd < 0 || offset < 0) return UM_ERR_INVALID;
    struct stat st;
    if (fstat(fd, &st) != 0) return UM_ERR_IO;
    if (offset > st.st_size) return UM_ERR_INVALID;
    if (length < 0 || offset + length > st.st_size) length = st.st_size - offset;
    s->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (s->fd < 0) return UM_ERR_IO;
    s->base = offset;
    s->size = length;
    return UM_OK;
}

int um_source_path(um_source *s, const char *path)
{
    memset(s, 0, sizeof *s);
    s->fd = -1;
    if (!path) return UM_ERR_INVALID;
    if (!strncmp(path, "file://", 7)) path += 7;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return UM_ERR_IO;
    int r = um_source_fd(s, fd, 0, -1);
    close(fd);
    return r;
}

void um_source_close(um_source *s)
{
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    if (s->close_cb) s->close_cb(s->user);
    s->close_cb = NULL;
    s->read_at = NULL;
}

int um_source_read(um_source *s, int64_t off, void *buf, size_t n)
{
    if (off < 0 || off + (int64_t)n > s->size) return -1;
    size_t got = 0;
    while (got < n) {
        ssize_t r;
        if (s->read_at) r = s->read_at(s->user, off + got, (char *)buf + got, n - got);
        else r = pread(s->fd, (char *)buf + got, n - got, s->base + off + got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

/* ---- tracks ------------------------------------------------------------------------------------ */
um_track *um_demux_add_track(um_demux *d)
{
    um_track *t = realloc(d->t, (d->nt + 1) * sizeof *t);
    if (!t) return NULL;
    d->t = t;
    t = &d->t[d->nt++];
    memset(t, 0, sizeof *t);
    t->fmt = um_format_new();
    return t;
}

int um_track_add(um_track *t, int64_t pos, uint32_t size, int64_t pts_us, uint32_t flags)
{
    if (t->n == t->cap) {
        int nc = t->cap ? t->cap * 2 : 256;
        um_sample *ns = realloc(t->s, nc * sizeof *ns);
        if (!ns) return -1;
        t->s = ns; t->cap = nc;
    }
    um_sample *s = &t->s[t->n++];
    s->pos = pos; s->size = size; s->pts_us = pts_us; s->flags = flags;
    if (size > t->max_size) t->max_size = size;
    return 0;
}

static int cmp_sample(const void *a, const void *b)
{
    const um_sample *x = a, *y = b;
    if (x->pts_us != y->pts_us) return x->pts_us < y->pts_us ? -1 : 1;
    return x->pos < y->pos ? -1 : x->pos > y->pos;
}

void um_track_finish(um_track *t)
{
    const char *mime = um_format_get_string(t->fmt, UM_KEY_MIME);
    int video = mime && !strncmp(mime, "video/", 6);
    /* decode order stays file order (B-frames: presentation times are not monotonic in the file);
       audio is in time order anyway */
    (void)cmp_sample;
    /* decode order is not presentation order (B-frames): decoders that reorder badly step aside */
    if (video) {
        for (int i = 1; i < t->n; i++)
            if (t->s[i].pts_us < t->s[i - 1].pts_us) { um_format_set_int32(t->fmt, "x-reordered", 1); break; }
    }
    if (!um_format_type(t->fmt, UM_KEY_MAX_INPUT_SIZE) && t->max_size)
        um_format_set_int32(t->fmt, UM_KEY_MAX_INPUT_SIZE, t->max_size);
    /* frame rate from the samples when the container gave none: (frames - 1) over the time from
       the first to the last frame (a constant-rate clip with millisecond timestamps comes out
       at its real rate, e.g. 29.97, where two neighbouring timestamps would not) */
    if (video && !um_format_type(t->fmt, UM_KEY_FRAME_RATE) && t->n > 1) {
        int64_t lo = t->s[0].pts_us, hi = lo;
        for (int i = 1; i < t->n; i++) {
            if (t->s[i].pts_us < lo) lo = t->s[i].pts_us;
            if (t->s[i].pts_us > hi) hi = t->s[i].pts_us;
        }
        if (hi > lo) um_format_set_float(t->fmt, UM_KEY_FRAME_RATE, (float)((t->n - 1) * 1e6 / (double)(hi - lo)));
    }
}

void um_demux_free(um_demux *d)
{
    for (int i = 0; i < d->nt; i++) { um_format_free(d->t[i].fmt); free(d->t[i].s); }
    free(d->t);
    um_format_free(d->file_fmt);
    memset(d, 0, sizeof *d);
}

/* ---- extractor ---------------------------------------------------------------------------------- */
struct um_extractor {
    pthread_mutex_t mu;
    um_source src;
    um_demux d;
    int open;
};

um_extractor *um_extractor_new(void)
{
    um_extractor *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    pthread_mutex_init(&e->mu, NULL);
    e->src.fd = -1;
    return e;
}

void um_extractor_free(um_extractor *e)
{
    if (!e) return;
    um_demux_free(&e->d);
    um_source_close(&e->src);
    pthread_mutex_destroy(&e->mu);
    free(e);
}

static int open_demux(um_extractor *e)
{
    uint8_t head[16] = {0};
    if (um_source_read(&e->src, 0, head, e->src.size < 16 ? (size_t)e->src.size : 16) != 0)
        return UM_ERR_IO;
    int r = UM_ERR_UNSUPPORTED;
    if (head[0] == 0x1A && head[1] == 0x45 && head[2] == 0xDF && head[3] == 0xA3)
        r = um_webm_open(&e->src, &e->d);
    else if (!memcmp(head + 4, "ftyp", 4) || !memcmp(head + 4, "moov", 4) || !memcmp(head + 4, "mdat", 4) ||
             !memcmp(head + 4, "free", 4) || !memcmp(head + 4, "wide", 4) || !memcmp(head + 4, "skip", 4))
        r = um_mp4_open(&e->src, &e->d);
    if (r != UM_OK && um_ffmpeg_available()) {
        um_demux_free(&e->d);
        r = um_ffmpeg_demux_open(&e->src, &e->d);
    }
    if (r != UM_OK) {
        um_demux_free(&e->d);
        um_log("extractor: no demuxer for this file (%02x %02x %02x %02x ...): %d", head[0], head[1], head[2], head[3], r);
        return r;
    }
    for (int i = 0; i < e->d.nt; i++) {
        um_track_finish(&e->d.t[i]);
        char *s = um_format_to_string(e->d.t[i].fmt);
        um_log("extractor: %s track %d: %d samples %s", e->d.container, i, e->d.t[i].n, s ? s : "");
        free(s);
    }
    e->open = 1;
    return UM_OK;
}

int um_extractor_set_fd(um_extractor *e, int fd, int64_t offset, int64_t length)
{
    pthread_mutex_lock(&e->mu);
    um_demux_free(&e->d);
    um_source_close(&e->src);
    int r = um_source_fd(&e->src, fd, offset, length);
    if (r == UM_OK) r = open_demux(e);
    pthread_mutex_unlock(&e->mu);
    return r;
}

int um_extractor_set_path(um_extractor *e, const char *path)
{
    pthread_mutex_lock(&e->mu);
    um_demux_free(&e->d);
    um_source_close(&e->src);
    int r = um_source_path(&e->src, path);
    if (r == UM_OK) r = open_demux(e);
    else um_log("extractor: cannot open %s", path ? path : "(null)");
    pthread_mutex_unlock(&e->mu);
    return r;
}

int um_extractor_set_reader(um_extractor *e, um_read_at_fn read_at, int64_t size, void *user, void (*close_cb)(void *))
{
    pthread_mutex_lock(&e->mu);
    um_demux_free(&e->d);
    um_source_close(&e->src);
    memset(&e->src, 0, sizeof e->src);
    e->src.fd = -1;
    e->src.read_at = read_at;
    e->src.user = user;
    e->src.close_cb = close_cb;
    e->src.size = size;
    int r = read_at && size > 0 ? open_demux(e) : UM_ERR_INVALID;
    pthread_mutex_unlock(&e->mu);
    return r;
}

int um_extractor_track_count(um_extractor *e) { return e ? e->d.nt : 0; }

um_format *um_extractor_track_format(um_extractor *e, int track)
{
    if (!e || track < 0 || track >= e->d.nt) return NULL;
    pthread_mutex_lock(&e->mu);
    um_format *f = um_format_copy(e->d.t[track].fmt);
    pthread_mutex_unlock(&e->mu);
    return f;
}

um_format *um_extractor_file_format(um_extractor *e)
{
    if (!e || !e->open) return NULL;
    return um_format_copy(e->d.file_fmt);
}

const char *um_extractor_container(um_extractor *e) { return e && e->open ? e->d.container : NULL; }

int um_extractor_select(um_extractor *e, int track, int on)
{
    if (!e || track < 0 || track >= e->d.nt) return UM_ERR_INVALID;
    pthread_mutex_lock(&e->mu);
    e->d.t[track].selected = on;
    pthread_mutex_unlock(&e->mu);
    return UM_OK;
}

/* the selected track whose next sample comes first */
static um_track *next_track(um_extractor *e, int *idx)
{
    um_track *best = NULL;
    for (int i = 0; i < e->d.nt; i++) {
        um_track *t = &e->d.t[i];
        if (!t->selected || t->cur >= t->n) continue;
        if (!best || t->s[t->cur].pts_us < best->s[best->cur].pts_us) { best = t; if (idx) *idx = i; }
    }
    return best;
}

ssize_t um_extractor_read(um_extractor *e, void *buf, size_t cap)
{
    if (!e) return -1;
    pthread_mutex_lock(&e->mu);
    um_track *t = next_track(e, NULL);
    ssize_t r = -1;
    if (t) {
        um_sample *s = &t->s[t->cur];
        if (t->nal_len && t->nal_len < 4) {
            /* NAL lengths of 1 or 2 bytes: the start codes make the sample longer */
            uint8_t *tmp = malloc(s->size ? s->size : 1);
            if (tmp && um_source_read(&e->src, s->pos, tmp, s->size) == 0) r = um_nal_convert(tmp, s->size, t->nal_len, buf, cap);
            free(tmp);
        } else if (s->size > cap) { um_log("extractor: sample of %u bytes, buffer of %zu", s->size, cap); r = -1; }
        else if (um_source_read(&e->src, s->pos, buf, s->size) == 0) {
            r = s->size;
            if (t->nal_len == 4) r = um_nal_convert(buf, s->size, 4, buf, cap);   /* (in place) */
        }
    }
    pthread_mutex_unlock(&e->mu);
    return r;
}

int64_t um_extractor_sample_time(um_extractor *e)
{
    pthread_mutex_lock(&e->mu);
    um_track *t = next_track(e, NULL);
    int64_t r = t ? t->s[t->cur].pts_us : -1;
    pthread_mutex_unlock(&e->mu);
    return r;
}

int um_extractor_sample_track(um_extractor *e)
{
    int i = -1;
    pthread_mutex_lock(&e->mu);
    if (!next_track(e, &i)) i = -1;
    pthread_mutex_unlock(&e->mu);
    return i;
}

uint32_t um_extractor_sample_flags(um_extractor *e)
{
    pthread_mutex_lock(&e->mu);
    um_track *t = next_track(e, NULL);
    uint32_t r = t ? t->s[t->cur].flags : 0;
    pthread_mutex_unlock(&e->mu);
    return r;
}

int64_t um_extractor_sample_size(um_extractor *e)
{
    pthread_mutex_lock(&e->mu);
    um_track *t = next_track(e, NULL);
    int64_t r = t ? (int64_t)t->s[t->cur].size : -1;
    pthread_mutex_unlock(&e->mu);
    return r;
}

int um_extractor_advance(um_extractor *e)
{
    pthread_mutex_lock(&e->mu);
    um_track *t = next_track(e, NULL);
    if (t) t->cur++;
    int more = next_track(e, NULL) != NULL;
    pthread_mutex_unlock(&e->mu);
    return more;
}

static int all_sync(um_track *t)
{
    for (int i = 0; i < t->n; i++) if (!(t->s[i].flags & UM_SAMPLE_SYNC)) return 0;
    return 1;
}

/* index of the sample a seek to time us lands on in t */
static int seek_index(um_track *t, int64_t us, int mode, int sync_only)
{
    int prev = -1, next = -1;
    for (int i = 0; i < t->n; i++) {
        if (sync_only && !(t->s[i].flags & UM_SAMPLE_SYNC)) continue;
        if (t->s[i].pts_us <= us) { if (prev < 0 || t->s[i].pts_us >= t->s[prev].pts_us) prev = i; }
        else if (next < 0 || t->s[i].pts_us < t->s[next].pts_us) next = i;
    }
    if (mode == UM_SEEK_NEXT_SYNC) {
        if (prev >= 0 && t->s[prev].pts_us == us) return prev;
        return next >= 0 ? next : t->n;
    }
    if (mode == UM_SEEK_CLOSEST_SYNC && prev >= 0 && next >= 0)
        return us - t->s[prev].pts_us <= t->s[next].pts_us - us ? prev : next;
    if (prev >= 0) return prev;
    return next >= 0 ? next : 0;
}

/* decode order: a track's cursor goes to the first sample in file order at or after index i
   (for B-frame streams samples before a sync sample in file order are not needed) */
int um_extractor_seek(um_extractor *e, int64_t us, int mode)
{
    if (!e) return UM_ERR_INVALID;
    pthread_mutex_lock(&e->mu);
    int64_t anchor = us;
    int have_anchor = 0;
    /* first the tracks that need a sync sample (video), then the others to the time they reached */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < e->d.nt; i++) {
            um_track *t = &e->d.t[i];
            if (!t->selected || !t->n) continue;
            int sync = !all_sync(t);
            if (sync != (pass == 0)) continue;
            if (sync) {
                t->cur = seek_index(t, us, mode, 1);
                if (!have_anchor && t->cur < t->n) { anchor = t->s[t->cur].pts_us; have_anchor = 1; }
            } else {
                t->cur = seek_index(t, have_anchor ? anchor : us, UM_SEEK_PREVIOUS_SYNC, 0);
            }
        }
    pthread_mutex_unlock(&e->mu);
    UM_TRACE("extractor: seek %lld mode %d", (long long)us, mode);
    return UM_OK;
}

/* ---- misc ------------------------------------------------------------------------------------- */
int64_t um_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int um_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
