// glreplay: a UA_GL_REC_FILE trace (glrec.c: every GL call a game made, written after it ran) replayed
// against any EGL/GLES driver, each call timed, to compare what drivers cost the CPU on the same frames.
//
//   glreplay <trace.gz> [--from <frame>] [--to <frame>] [--size <w>x<h>] [--check] [--no-finish]
//
// The trace is replayed from its first call (every object made as the game made it); the frames from
// --from up to --to (default: the last 300 of the trace) are measured: each call timed with the ARM
// generic timer's counter (CNTVCT; clock_gettime where it cannot be read), summed by entry point and by
// class (draw, framebuffer bind, clear, invalidate, uploads, uniforms, program, state, frame end).
// Before each measured frame the window's calls are already in memory (nothing read from the file
// between them). At each frame's end (the game's swap) eglSwapBuffers and glFlush are timed ("frame
// end", the driver's submission), then glFinish waits for the GPU untimed (--no-finish: not), so a
// call never waits for the previous frame's GPU work: the times are the driver's CPU work. Thread and
// process CPU time a frame are reported too (the process's includes the driver's own threads, as
// mesa_glthread's).
//   --hash <out> [--hash-frames A-B] [--hash-draws] [--hash-dump <dir>]   pixels, not times: each framebuffer's
//                colour read back and hashed where a pass ends (a bind) and at the swap, with --hash-draws after
//                every draw and clear too (glreplay_hash.c; two logs compared by glpixdiff)
//   --check      glGetError after every call: the calls a driver refused, by entry point (the replay is
//                only a fair comparison where no driver refuses anything the game's driver took)
// Environment:
//   GLREP_EGL, GLREP_GLES  the libraries (default libEGL.so.1, libGLESv2.so.2: the system's, or what
//                          LD_LIBRARY_PATH finds first)
//   GLREP_PLATFORM=surfaceless  Mesa's surfaceless EGL platform (with drm-shim: no display at all);
//                          default eglGetDisplay(EGL_DEFAULT_DISPLAY) (libmali fbdev), a pbuffer either way
// Names the game got (textures, buffers, programs, ...) are mapped to the names this driver gives, and
// uniform locations and block indices to what this driver answers for the same names. Mapped writes
// (glMapBufferRange: the game's bytes copied into this driver's mapping at the flush or unmap) are timed
// apart ("map writes"): the game's own CPU time, not the driver's. Calls that cannot be replayed
// (client-side vertex arrays, program binaries) are counted and named.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define GLREC_PART 1
#include "glrec_gen.h"
#undef GLREC_PART
#include "glreplay_hash.h"

#define TR_TAG (1ull << 63)
#define TR_FRAME 0xFFFFF0u
static void *rp_fn[OP_N];
#define RX(f) ((T_##f)rp_fn[OP_##f])
static inline float rec_bitsf(uint64_t v) { uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }

// ---- the timer (as glrec.c's UA_GL_REC=time)
static int g_counter;
static double g_tick_ns = 1;
#if defined(__arm__)
static inline uint64_t counter(void)
{
    uint32_t lo, hi;
    __asm__ volatile("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi) :: "memory");
    return (uint64_t)hi << 32 | lo;
}
static uint32_t counter_freq(void) { uint32_t f; __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(f)); return f; }
#elif defined(__aarch64__)
static inline uint64_t counter(void) { uint64_t v; __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) :: "memory"); return v; }
static uint32_t counter_freq(void) { uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return (uint32_t)f; }
#else
static inline uint64_t counter(void) { return 0; }
static uint32_t counter_freq(void) { return 0; }
#endif
static inline uint64_t ticks(void)
{
    if (g_counter) return counter();
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static sigjmp_buf g_jb;
static void on_ill(int s) { (void)s; siglongjmp(g_jb, 1); }
static void time_init(void)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_ill;
    sigaction(SIGILL, &sa, &old);
    volatile uint32_t f = 0; volatile uint64_t a = 0, b = 0;
    if (!sigsetjmp(g_jb, 1)) { f = counter_freq(); a = counter(); usleep(1000); b = counter(); }
    sigaction(SIGILL, &old, NULL);
    g_counter = f > 0 && b > a;
    g_tick_ns = g_counter ? 1e9 / f : 1.0;
}
static double cpu_s(clockid_t c) { struct timespec t; clock_gettime(c, &t); return t.tv_sec + t.tv_nsec / 1e9; }

// ---- names: what the game got -> what this driver gave
enum { MK_TEX, MK_BUF, MK_FBO, MK_RBO, MK_VAO, MK_SMP, MK_PROG, MK_SH, MK_PIPE, MK_QRY, MK_XFB, MK_N };
typedef struct { uint64_t *k; uint64_t *v; size_t cap, n; } hmap;
static hmap g_names[MK_N], g_locs, g_ubis, g_syncs;
static void hput(hmap *m, uint64_t k, uint64_t v);
static int hget(const hmap *m, uint64_t k, uint64_t *v)
{
    if (!m->cap) return 0;
    for (size_t i = (k * 0x9E3779B97F4A7C15ull) >> 40 & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (m->k[i] == ~0ull) return 0;
        if (m->k[i] == k) { *v = m->v[i]; return 1; }
    }
}
static void hput(hmap *m, uint64_t k, uint64_t v)
{
    if ((m->n + 1) * 2 > m->cap) {
        hmap o = *m;
        m->cap = o.cap ? o.cap * 2 : 1024; m->n = 0;
        m->k = malloc(m->cap * 8); m->v = malloc(m->cap * 8);
        memset(m->k, 0xff, m->cap * 8);
        for (size_t i = 0; i < o.cap; i++) if (o.k[i] != ~0ull) hput(m, o.k[i], o.v[i]);
        free(o.k); free(o.v);
    }
    for (size_t i = (k * 0x9E3779B97F4A7C15ull) >> 40 & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (m->k[i] == ~0ull) { m->k[i] = k; m->v[i] = v; m->n++; return; }
        if (m->k[i] == k) { m->v[i] = v; return; }
    }
}
static unsigned long g_unmapped_names, g_loc_miss;
static GLuint rp_map(int kind, GLuint name)
{
    uint64_t v;
    if (!name) return 0;
    if (hget(&g_names[kind], name, &v)) return (GLuint)v;
    g_unmapped_names++;
    return name;
}
static GLuint *g_scratch; static size_t g_nscratch;
static GLuint *rp_out(GLsizei n)
{
    if ((size_t)(n > 0 ? n : 0) + 1 > g_nscratch) { g_nscratch = (size_t)n + 64; g_scratch = realloc(g_scratch, g_nscratch * 4); }
    return g_scratch;
}
static void rp_gen(int kind, const GLuint *old, const GLuint *now, GLsizei n)
{
    for (GLsizei i = 0; old && i < n; i++) hput(&g_names[kind], old[i], now[i]);
}
static const GLuint *rp_maparr(int kind, const void *p, GLsizei n)
{
    const GLuint *old = p;
    GLuint *o = rp_out(n);
    for (GLsizei i = 0; old && i < n; i++) o[i] = rp_map(kind, old[i]);
    return old ? o : NULL;
}
static GLuint rp_prog;                  // the program in use (the game's name)
static GLint rp_loc(GLuint prog, GLint loc)
{
    uint64_t v;
    if (loc < 0) return loc;
    if (hget(&g_locs, (uint64_t)prog << 32 | (uint32_t)loc, &v)) return (GLint)v;
    g_loc_miss++;
    return loc;
}
static GLuint rp_ubi(GLuint prog, GLuint idx)
{
    uint64_t v;
    if (idx == GL_INVALID_INDEX) return idx;
    if (hget(&g_ubis, (uint64_t)prog << 32 | idx, &v)) return (GLuint)v;
    g_loc_miss++;
    return idx;
}
static GLsync rp_sync(uint64_t s)
{
    uint64_t v;
    return s && hget(&g_syncs, s, &v) ? (GLsync)(uintptr_t)v : NULL;
}

// ---- the record being replayed: its blobs
static const unsigned char *g_bl[64];
static unsigned char *g_zeros; static size_t g_nzeros;
static const void *rp_ptr(uint64_t v)
{
    if (!(v & TR_TAG)) return (const void *)(uintptr_t)v;
    unsigned i = (unsigned)(v & 0xffff);
    return i < 64 ? g_bl[i] : NULL;
}

// ---- by hand: maps, fences, sources, locations, programs
static double g_mapwrite_ticks; static unsigned long g_mapwrites;
static int g_measuring;
typedef struct { GLenum target; unsigned char *p; } rmap;
static rmap g_rmap[16];
static rmap *rmap_of(GLenum t, int add)
{
    for (int i = 0; i < 16; i++) if (g_rmap[i].p && g_rmap[i].target == t) return &g_rmap[i];
    if (add) for (int i = 0; i < 16; i++) if (!g_rmap[i].p) { g_rmap[i].target = t; return &g_rmap[i]; }
    return NULL;
}
static void mapwrite(void *dst, const void *src, size_t n)
{
    uint64_t t = g_measuring ? ticks() : 0;
    memcpy(dst, src, n);
    if (g_measuring) { g_mapwrite_ticks += (double)(ticks() - t); g_mapwrites++; }
}
static unsigned long g_unsupported[OP_N];
static void rp_custom_glMapBufferRange(const uint64_t *a, uint64_t r)
{
    (void)r;
    void *p = RX(glMapBufferRange)((GLenum)a[0], (GLintptr)(int64_t)a[1], (GLsizeiptr)(int64_t)a[2], (GLbitfield)a[3]);
    rmap *m = p ? rmap_of((GLenum)a[0], 1) : NULL;
    if (m) m->p = p;
}
static void rp_custom_glFlushMappedBufferRange(const uint64_t *a, uint64_t r)
{
    (void)r;
    rmap *m = rmap_of((GLenum)a[0], 0);
    if (m && a[3]) mapwrite(m->p + (int64_t)a[1], rp_ptr(a[3]), (size_t)(int64_t)a[2]);
    RX(glFlushMappedBufferRange)((GLenum)a[0], (GLintptr)(int64_t)a[1], (GLsizeiptr)(int64_t)a[2]);
}
static void rp_custom_glUnmapBuffer(const uint64_t *a, uint64_t r)
{
    (void)r;
    rmap *m = rmap_of((GLenum)a[0], 0);
    if (m && a[1]) {
        const unsigned char *b = rp_ptr(a[1]);
        uint64_t len; memcpy(&len, b - 8, 8);
        mapwrite(m->p, b, (size_t)(len & ~TR_TAG));
    }
    RX(glUnmapBuffer)((GLenum)a[0]);
    if (m) m->p = NULL;
}
static void rp_custom_glFenceSync(const uint64_t *a, uint64_t r)
{
    GLsync s = RX(glFenceSync)((GLenum)a[0], (GLbitfield)a[1]);
    if (r) hput(&g_syncs, r, (uint64_t)(uintptr_t)s);
}
static void rp_custom_glClientWaitSync(const uint64_t *a, uint64_t r)
{
    (void)r;
    GLsync s = rp_sync(a[0]);
    if (s) RX(glClientWaitSync)(s, (GLbitfield)a[1], (GLuint64)a[2]);
}
static void rp_custom_glWaitSync(const uint64_t *a, uint64_t r)
{
    (void)r;
    GLsync s = rp_sync(a[0]);
    if (s) RX(glWaitSync)(s, (GLbitfield)a[1], (GLuint64)a[2]);
}
static void rp_custom_glDeleteSync(const uint64_t *a, uint64_t r)
{
    (void)r;
    GLsync s = rp_sync(a[0]);
    if (s) RX(glDeleteSync)(s);
    if (a[0]) hput(&g_syncs, a[0], 0);
}
static void rp_custom_glShaderSource(const uint64_t *a, uint64_t r)
{
    (void)r;
    const GLchar *src = rp_ptr(a[1]);
    RX(glShaderSource)(rp_map(MK_SH, (GLuint)a[0]), 1, &src, NULL);
}
static void rp_custom_glCreateShader(const uint64_t *a, uint64_t r) { hput(&g_names[MK_SH], r, RX(glCreateShader)((GLenum)a[0])); }
static void rp_custom_glCreateProgram(const uint64_t *a, uint64_t r) { (void)a; hput(&g_names[MK_PROG], r, RX(glCreateProgram)()); }
static void rp_custom_glCreateShaderProgramv(const uint64_t *a, uint64_t r) { (void)a; (void)r; g_unsupported[OP_glCreateShaderProgramv]++; }
static void rp_custom_glUseProgram(const uint64_t *a, uint64_t r) { (void)r; rp_prog = (GLuint)a[0]; RX(glUseProgram)(rp_map(MK_PROG, rp_prog)); }
static unsigned long g_attrib_moved;
static void rp_custom_glGetUniformLocation(const uint64_t *a, uint64_t r)
{
    GLint now = RX(glGetUniformLocation)(rp_map(MK_PROG, (GLuint)a[0]), rp_ptr(a[1]));
    if ((int32_t)r >= 0) hput(&g_locs, a[0] << 32 | (uint32_t)r, (uint64_t)(uint32_t)now);
}
static void rp_custom_glGetUniformBlockIndex(const uint64_t *a, uint64_t r)
{
    GLuint now = RX(glGetUniformBlockIndex)(rp_map(MK_PROG, (GLuint)a[0]), rp_ptr(a[1]));
    if ((GLuint)r != GL_INVALID_INDEX) hput(&g_ubis, a[0] << 32 | (uint32_t)r, now);
}
static void rp_custom_glGetAttribLocation(const uint64_t *a, uint64_t r)
{
    GLint now = RX(glGetAttribLocation)(rp_map(MK_PROG, (GLuint)a[0]), rp_ptr(a[1]));
    if (now != (GLint)r) g_attrib_moved++;
}
static void rp_custom_glGetProgramResourceLocation(const uint64_t *a, uint64_t r)
{
    GLint now = RX(glGetProgramResourceLocation)(rp_map(MK_PROG, (GLuint)a[0]), (GLenum)a[1], rp_ptr(a[2]));
    if ((GLenum)a[1] == GL_UNIFORM && (int32_t)r >= 0) hput(&g_locs, a[0] << 32 | (uint32_t)r, (uint64_t)(uint32_t)now);
}
static void rp_custom_glGetProgramResourceIndex(const uint64_t *a, uint64_t r)
{
    GLuint now = RX(glGetProgramResourceIndex)(rp_map(MK_PROG, (GLuint)a[0]), (GLenum)a[1], rp_ptr(a[2]));
    if ((GLenum)a[1] == GL_UNIFORM_BLOCK && (GLuint)r != GL_INVALID_INDEX) hput(&g_ubis, a[0] << 32 | (uint32_t)r, now);
}
static void rp_custom_glGetFragDataLocation(const uint64_t *a, uint64_t r) { (void)a; (void)r; }
static unsigned char *g_readbuf;
static void rp_custom_glReadPixels(const uint64_t *a, uint64_t r)
{
    (void)r;
    GLint pack = 0;
    RX(glGetIntegerv)(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    GLsizei w = (GLsizei)(int64_t)a[2], h = (GLsizei)(int64_t)a[3];
    void *p = (void *)(uintptr_t)a[6];
    if (!pack) { g_readbuf = realloc(g_readbuf, (size_t)(w > 0 ? w : 1) * (size_t)(h > 0 ? h : 1) * 16); p = g_readbuf; }
    RX(glReadPixels)((GLint)(int64_t)a[0], (GLint)(int64_t)a[1], w, h, (GLenum)a[4], (GLenum)a[5], p);
}
static void rp_custom_glMapBufferOES(const uint64_t *a, uint64_t r) { (void)a; (void)r; g_unsupported[OP_glMapBufferOES]++; }
static void rp_custom_glUnmapBufferOES(const uint64_t *a, uint64_t r) { (void)a; (void)r; g_unsupported[OP_glUnmapBufferOES]++; }
static void rp_custom_glMapBufferRangeEXT(const uint64_t *a, uint64_t r) { (void)a; (void)r; g_unsupported[OP_glMapBufferRangeEXT]++; }
static void rp_custom_glFlushMappedBufferRangeEXT(const uint64_t *a, uint64_t r) { (void)a; (void)r; g_unsupported[OP_glFlushMappedBufferRangeEXT]++; }

#define GLREC_PART 3
#include "glrec_gen.h"
#undef GLREC_PART

// ---- classes
enum { C_DRAW, C_BINDFB, C_CLEAR, C_INVAL, C_UPLOAD, C_UNIFORM, C_PROGRAM, C_STATE, C_SHADER, C_END, C_N };
static const char *const c_name[C_N] = { "draw", "framebuffer bind", "clear", "invalidate", "uploads (buffers, textures, maps)",
                                         "uniforms", "glUseProgram", "state (binds, enables, blend, viewport, ...)",
                                         "shaders, programs, objects", "frame end (eglSwapBuffers + glFlush)" };
static unsigned char g_class[OP_N];
static int has(const char *n, const char *p) { return !strncmp(n, p, strlen(p)); }
static void classes(void)
{
    for (unsigned i = 0; i < OP_N; i++) {
        const char *n = glrec_name[i];
        int c = C_STATE;
        if (has(n, "glDraw") || has(n, "glMultiDraw")) c = C_DRAW;
        else if (!strcmp(n, "glBindFramebuffer")) c = C_BINDFB;
        else if (has(n, "glClear")) c = C_CLEAR;
        else if (has(n, "glInvalidate") || has(n, "glDiscardFramebuffer")) c = C_INVAL;
        else if (has(n, "glBufferData") || has(n, "glBufferSubData") || has(n, "glTexImage") || has(n, "glTexSubImage") ||
                 has(n, "glCompressedTex") || has(n, "glMapBuffer") || has(n, "glFlushMappedBuffer") || has(n, "glUnmapBuffer") ||
                 has(n, "glCopyBufferSubData") || has(n, "glGenerateMipmap") || has(n, "glTexStorage") || has(n, "glBufferStorage"))
            c = C_UPLOAD;
        else if (has(n, "glUniform") || has(n, "glProgramUniform")) c = C_UNIFORM;
        else if (!strcmp(n, "glUseProgram")) c = C_PROGRAM;
        else if (has(n, "glShader") || has(n, "glCompileShader") || has(n, "glLinkProgram") || has(n, "glCreate") ||
                 has(n, "glAttachShader") || has(n, "glDetachShader") || has(n, "glDelete") || has(n, "glGen") ||
                 has(n, "glBindAttribLocation") || has(n, "glGetUniformLocation") || has(n, "glGetUniformBlockIndex") ||
                 has(n, "glGetAttribLocation") || has(n, "glGetProgramResource"))
            c = C_SHADER;
        g_class[i] = (unsigned char)c;
    }
}

// ---- reading records
typedef struct { uint32_t op; uint16_t nargs, nblobs; uint8_t hasret, thread; uint64_t ret; uint64_t *a; unsigned char **bl; } rec;
static gzFile g_f;
static unsigned g_opmap[1 << 12];       // the trace's op numbers -> ours
static unsigned g_trace_nops;
static int rd(void *p, size_t n) { return n == 0 || gzread(g_f, p, (unsigned)n) == (int)n; }
// one record into r (its slots and blobs in memory of its own: kept for the measured window)
static int read_rec(rec *r)
{
    uint64_t h;
    if (!rd(&h, 8)) return 0;
    uint32_t op = (uint32_t)(h & 0xffffff);
    r->nargs = (uint16_t)(h >> 24 & 0xff); r->nblobs = (uint16_t)(h >> 32 & 0xffff);
    r->hasret = (uint8_t)(h >> 48 & 1); r->thread = (uint8_t)(h >> 49 & 31);
    r->op = op == TR_FRAME ? TR_FRAME : op < g_trace_nops ? g_opmap[op] : OP_N;
    size_t nw = (size_t)r->nargs + r->hasret;
    r->a = malloc((nw ? nw : 1) * 8);
    if (!rd(r->a, nw * 8)) return 0;
    r->ret = r->hasret ? r->a[r->nargs] : 0;
    r->bl = r->nblobs ? malloc(r->nblobs * sizeof *r->bl) : NULL;
    for (unsigned i = 0; i < r->nblobs; i++) {
        uint64_t len;
        if (!rd(&len, 8)) return 0;
        size_t n = (size_t)(len & ~TR_TAG);
        unsigned char *b = malloc(8 + (n + 7) / 8 * 8 + 8);
        memcpy(b, &len, 8);
        if (len & TR_TAG) {
            if (n > g_nzeros) { free(g_zeros); g_nzeros = n + (1 << 20); g_zeros = calloc(1, g_nzeros); }
            r->bl[i] = b;           // (zeros: pointed at the shared zero buffer when replayed)
        } else {
            if (!rd(b + 8, (n + 7) / 8 * 8)) return 0;
            r->bl[i] = b;
        }
    }
    return 1;
}
static void free_rec(rec *r) { for (unsigned i = 0; i < r->nblobs; i++) free(r->bl[i]); free(r->bl); free(r->a); }

// ---- statistics
static double g_op_ticks[OP_N]; static unsigned long g_op_calls[OP_N], g_op_err[OP_N]; static GLenum g_op_firsterr[OP_N];
static double g_end_ticks; static unsigned long g_end_n;
static int g_check;
static unsigned long g_link_fail;
static unsigned long g_other_thread_calls;
static unsigned g_main_thread = 255;

// --hash: the trace's call count, the frame, the game's draw framebuffer, the window's size
static int g_hash; static unsigned long g_hcall; static long g_hframe; static unsigned g_hfb; static int g_hw = 720, g_hh = 480;

static void replay_rec(const rec *r)
{
    unsigned long hcall = g_hcall++;
    for (unsigned i = 0; i < r->nblobs && i < 64; i++) {
        uint64_t len; memcpy(&len, r->bl[i], 8);
        g_bl[i] = len & TR_TAG ? g_zeros : r->bl[i] + 8;
    }
    if (r->op >= OP_N) return;
    if (!rp_how[r->op] || !rp_fn[r->op]) return;
    if (g_main_thread != 255 && r->thread != g_main_thread) g_other_thread_calls++;
    int hbind = r->op == OP_glBindFramebuffer && ((GLenum)r->a[0] == GL_FRAMEBUFFER || (GLenum)r->a[0] == GL_DRAW_FRAMEBUFFER);
    if (g_hash && hbind && rph_on(g_hframe)) rph_read(g_hframe, hcall, "bind", g_hfb, !g_hfb, g_hw, g_hh);   // (the pass that ends)
    uint64_t t = g_measuring ? ticks() : 0;
    rp_call(r->op, r->a, r->ret);
    if (g_hash) {
        if (hbind) g_hfb = (unsigned)r->a[1];
        else if ((g_class[r->op] == C_DRAW || g_class[r->op] == C_CLEAR) && rph_draws() && rph_on(g_hframe))
            rph_read(g_hframe, hcall, "draw", g_hfb, !g_hfb, g_hw, g_hh);
    }
    if (g_measuring) { g_op_ticks[r->op] += (double)(ticks() - t); g_op_calls[r->op]++; }
    if (g_check && r->op == OP_glLinkProgram) {
        GLint ok = 0; GLuint pr = rp_map(MK_PROG, (GLuint)r->a[0]);
        RX(glGetProgramiv)(pr, GL_LINK_STATUS, &ok);
        if (!ok && g_link_fail++ < 3) {
            char log[512] = { 0 };
            RX(glGetProgramInfoLog)(pr, sizeof log, NULL, log);
            fprintf(stderr, "glreplay: program %u did not link: %.400s\n", (unsigned)r->a[0], log);
        }
    }
    if (g_check) {
        GLenum e = RX(glGetError)();
        if (e) { if (!g_op_err[r->op]++) g_op_firsterr[r->op] = e; while (RX(glGetError)()) {} }
    }
}

static EGLDisplay g_dpy; static EGLSurface g_surf;
static EGLBoolean (*e_SwapBuffers)(EGLDisplay, EGLSurface);
static int g_finish = 1;
static unsigned long g_frames_measured;

static void frame_end(void)
{
    if (g_hash && rph_on(g_hframe)) rph_read(g_hframe, g_hcall, "swap", 0, 1, g_hw, g_hh);   // (before the swap: after it the window is undefined)
    g_hframe++;
    uint64_t t = g_measuring ? ticks() : 0;
    e_SwapBuffers(g_dpy, g_surf);
    RX(glFlush)();
    if (g_measuring) { g_end_ticks += (double)(ticks() - t); g_end_n++; }
    if (g_finish) RX(glFinish)();
}

static void *g_egl, *g_gles;
static void *sym(const char *n)
{
    static void *(*gpa)(const char *);
    if (!gpa) *(void **)&gpa = dlsym(g_egl, "eglGetProcAddress");
    void *p = dlsym(g_gles, n);
    if (!p) p = dlsym(g_egl, n);
    if (!p && gpa) p = gpa(n);
    return p;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    long from = -1, to = -1;
    int w = 720, h = 480;
    const char *hash_out = NULL, *hash_frames = NULL, *hash_dump = NULL;
    int hash_draws = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--from") && i + 1 < argc) from = atol(argv[++i]);
        else if (!strcmp(argv[i], "--to") && i + 1 < argc) to = atol(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
        else if (!strcmp(argv[i], "--check")) g_check = 1;
        else if (!strcmp(argv[i], "--no-finish")) g_finish = 0;
        else if (!strcmp(argv[i], "--hash") && i + 1 < argc) hash_out = argv[++i];
        else if (!strcmp(argv[i], "--hash-frames") && i + 1 < argc) hash_frames = argv[++i];
        else if (!strcmp(argv[i], "--hash-draws")) hash_draws = 1;
        else if (!strcmp(argv[i], "--hash-dump") && i + 1 < argc) hash_dump = argv[++i];
        else path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: glreplay <trace.gz> [--from F] [--to F] [--size WxH] [--check] [--no-finish] [--hash <out> [--hash-frames A-B] [--hash-draws] [--hash-dump <dir>]]\n"); return 2; }
    time_init();
    classes();
    // the frames: counted first (a pass over the file)
    g_f = gzopen(path, "rb");
    char magic[8]; uint32_t hdr[3];
    if (!g_f || !rd(magic, 8) || memcmp(magic, "UAGLTRC1", 8) || !rd(hdr, sizeof hdr)) { fprintf(stderr, "glreplay: %s: not a UA_GL_REC_FILE trace\n", path); return 1; }
    g_trace_nops = hdr[1];
    if (g_trace_nops > (1u << 12)) return 1;
    for (unsigned i = 0; i < g_trace_nops; i++) {
        uint16_t l; char n[128] = { 0 };
        if (!rd(&l, 2) || l >= sizeof n || !rd(n, l)) return 1;
        g_opmap[i] = OP_N;
        for (unsigned k = 0; k < OP_N; k++) if (!strcmp(glrec_name[k], n)) { g_opmap[i] = k; break; }
    }
    z_off_t start = gztell(g_f);
    long nframes = 0; unsigned long ncalls = 0;
    rec r;
    while (read_rec(&r)) { if (r.op == TR_FRAME) { nframes++; if (g_main_thread == 255) g_main_thread = r.thread; } else ncalls++; free_rec(&r); }
    if (to < 0 || to > nframes) to = nframes;
    if (from < 0) from = to > 300 ? to - 300 : 0;
    if (from >= to) { fprintf(stderr, "glreplay: %ld frames in the trace: nothing to measure from %ld to %ld\n", nframes, from, to); return 1; }
    fprintf(stderr, "glreplay: %s: %ld frames, %lu calls; measuring frames %ld - %ld\n", path, nframes, ncalls, from, to - 1);

    // the driver
    const char *el = getenv("GLREP_EGL"), *gl = getenv("GLREP_GLES"), *pf = getenv("GLREP_PLATFORM");
    g_egl = dlopen(el && *el ? el : "libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!g_egl) g_egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    g_gles = dlopen(gl && *gl ? gl : "libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!g_gles) g_gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_egl || !g_gles) { fprintf(stderr, "glreplay: no EGL/GLES: %s\n", dlerror()); return 1; }
    EGLDisplay (*e_GetDisplay)(EGLNativeDisplayType) = sym("eglGetDisplay");
    PFNEGLGETPLATFORMDISPLAYEXTPROC e_GetPlatformDisplay = sym("eglGetPlatformDisplayEXT");
    EGLBoolean (*e_Initialize)(EGLDisplay, EGLint *, EGLint *) = sym("eglInitialize");
    EGLBoolean (*e_ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = sym("eglChooseConfig");
    EGLBoolean (*e_BindAPI)(EGLenum) = sym("eglBindAPI");
    EGLSurface (*e_CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *) = sym("eglCreatePbufferSurface");
    EGLContext (*e_CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = sym("eglCreateContext");
    EGLBoolean (*e_MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = sym("eglMakeCurrent");
    *(void **)&e_SwapBuffers = sym("eglSwapBuffers");
    if (pf && !strcmp(pf, "surfaceless")) g_dpy = e_GetPlatformDisplay ? e_GetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL) : EGL_NO_DISPLAY;
    else g_dpy = e_GetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0;
    if (g_dpy == EGL_NO_DISPLAY || !e_Initialize(g_dpy, &maj, &min)) { fprintf(stderr, "glreplay: eglInitialize failed\n"); return 1; }
    const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */, EGL_RED_SIZE, 8,
                          EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!e_ChooseConfig(g_dpy, ca, &cfg, 1, &n) || n < 1) { fprintf(stderr, "glreplay: no pbuffer RGBA8 D24S8 GLES3 config\n"); return 1; }
    e_BindAPI(EGL_OPENGL_ES_API);
    const EGLint sa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    g_surf = e_CreatePbufferSurface(g_dpy, cfg, sa);
    const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = e_CreateContext(g_dpy, cfg, EGL_NO_CONTEXT, xa);
    if (g_surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !e_MakeCurrent(g_dpy, g_surf, g_surf, ctx)) {
        fprintf(stderr, "glreplay: no pbuffer context\n"); return 1;
    }
    unsigned nfn = 0;
    for (unsigned i = 0; i < OP_N; i++) if ((rp_fn[i] = sym(glrec_name[i]))) nfn++;
    if (hash_out) {                                      // --hash (glreplay_hash.c)
        if (rph_init(hash_out, hash_frames, hash_draws, hash_dump, sym) < 0) return 1;
        g_hash = 1; g_hw = w; g_hh = h;
    }
    const GLubyte *(*g_GetString)(GLenum) = sym("glGetString");
    fprintf(stderr, "glreplay: EGL %d.%d, %s, %s, %s; %u entry points\n", maj, min, g_GetString(GL_VENDOR), g_GetString(GL_RENDERER),
            g_GetString(GL_VERSION), nfn);

    // replay: up to the window from the file, then the window read into memory and replayed
    gzseek(g_f, start, SEEK_SET);
    long frame = 0;
    while (frame < from && read_rec(&r)) {
        if (r.op == TR_FRAME) { frame_end(); frame++; }
        else replay_rec(&r);
        free_rec(&r);
    }
    size_t cap = 1 << 16, nw = 0;
    rec *win = malloc(cap * sizeof *win);
    long wf = frame;
    while (wf < to && read_rec(&r)) {
        if (nw == cap) { cap *= 2; win = realloc(win, cap * sizeof *win); }
        win[nw++] = r;
        if (r.op == TR_FRAME) wf++;
    }
    gzclose(g_f);
    unsigned long missing = 0;
    for (size_t i = 0; i < nw; i++) if (win[i].op < OP_N && rp_how[win[i].op] && !rp_fn[win[i].op]) missing++;
    double t0 = cpu_s(CLOCK_THREAD_CPUTIME_ID), p0 = cpu_s(CLOCK_PROCESS_CPUTIME_ID), w0 = cpu_s(CLOCK_MONOTONIC);
    double ft0 = t0, fp0 = p0, fw0 = w0;
    double thr_frame_sum = 0, proc_frame_sum = 0, wall_frame_sum = 0;
    g_measuring = 1;
    for (size_t i = 0; i < nw; i++) {
        if (win[i].op == TR_FRAME) {
            frame_end();
            double t1 = cpu_s(CLOCK_THREAD_CPUTIME_ID), p1 = cpu_s(CLOCK_PROCESS_CPUTIME_ID), w1 = cpu_s(CLOCK_MONOTONIC);
            thr_frame_sum += t1 - ft0; proc_frame_sum += p1 - fp0; wall_frame_sum += w1 - fw0;
            g_frames_measured++;
            ft0 = cpu_s(CLOCK_THREAD_CPUTIME_ID); fp0 = cpu_s(CLOCK_PROCESS_CPUTIME_ID); fw0 = cpu_s(CLOCK_MONOTONIC);
        } else replay_rec(&win[i]);
    }
    g_measuring = 0;

    if (g_hash) rph_close();
    // the report
    double F = g_frames_measured ? (double)g_frames_measured : 1, us = g_tick_ns / 1000;
    double cls_t[C_N] = { 0 }, cls_n[C_N] = { 0 }, total = 0;
    for (unsigned i = 0; i < OP_N; i++) { cls_t[g_class[i]] += g_op_ticks[i]; cls_n[g_class[i]] += (double)g_op_calls[i]; total += g_op_ticks[i]; }
    cls_t[C_END] = g_end_ticks; cls_n[C_END] = (double)g_end_n; total += g_end_ticks;
    printf("glreplay %s\ndriver: %s | %s | %s\n", path, g_GetString(GL_VENDOR), g_GetString(GL_RENDERER), g_GetString(GL_VERSION));
    printf("frames measured: %lu (%ld - %ld), timer: %s%s\n", g_frames_measured, from, to - 1,
           g_counter ? "generic timer" : "clock_gettime", g_finish ? ", glFinish after each frame (untimed)" : "");
    printf("\n%-46s %10s %10s %10s\n", "class", "calls/fr", "us/call", "ms/frame");
    for (int c = 0; c < C_N; c++)
        if (cls_n[c] > 0) printf("%-46s %10.1f %10.2f %10.3f\n", c_name[c], cls_n[c] / F, cls_t[c] * us / cls_n[c], cls_t[c] * us / 1000 / F);
    printf("%-46s %10s %10s %10.3f\n", "all GL calls (timed, CPU ms a frame)", "", "", total * us / 1000 / F);
    if (g_mapwrites) printf("%-46s %10.1f %10.2f %10.3f\n", "map writes (the game's copies, not GL)", g_mapwrites / F, g_mapwrite_ticks * us / g_mapwrites, g_mapwrite_ticks * us / 1000 / F);
    printf("thread CPU a frame %.3f ms, process CPU a frame %.3f ms, wall a frame %.3f ms (incl. untimed glFinish)\n",
           thr_frame_sum * 1000 / F, proc_frame_sum * 1000 / F, wall_frame_sum * 1000 / F);
    // the top entry points
    unsigned idx[OP_N]; unsigned k = 0;
    for (unsigned i = 0; i < OP_N; i++) if (g_op_calls[i]) idx[k++] = i;
    for (unsigned i = 0; i < k; i++) for (unsigned j = i + 1; j < k; j++) if (g_op_ticks[idx[j]] > g_op_ticks[idx[i]]) { unsigned t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    printf("\n%-40s %10s %10s %10s\n", "entry point", "calls/fr", "us/call", "ms/frame");
    for (unsigned i = 0; i < k && i < 30; i++)
        printf("%-40s %10.1f %10.2f %10.3f\n", glrec_name[idx[i]], g_op_calls[idx[i]] / F, g_op_ticks[idx[i]] * us / g_op_calls[idx[i]], g_op_ticks[idx[i]] * us / 1000 / F);
    printf("\nnot replayable: names not seen made %lu, uniform locations not asked for %lu, attribute locations that moved %lu, "
           "calls from other threads %lu, entry points this driver lacks (window) %lu\n",
           g_unmapped_names, g_loc_miss, g_attrib_moved, g_other_thread_calls, missing);
    for (unsigned i = 0; i < OP_N; i++) if (g_unsupported[i]) printf("  unsupported %s: %lu\n", glrec_name[i], g_unsupported[i]);
    if (g_check) {
        unsigned long tot = 0;
        for (unsigned i = 0; i < OP_N; i++) if (g_op_err[i]) { printf("  GL errors in %s: %lu (first 0x%04x)\n", glrec_name[i], g_op_err[i], g_op_firsterr[i]); tot += g_op_err[i]; }
        printf("GL errors: %lu; programs that did not link: %lu\n", tot, g_link_fail);
    }
    return 0;
}
