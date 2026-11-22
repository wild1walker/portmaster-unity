// glreplay: a UA_GL_REC_FILE trace (glrec.c: every GL call a game made, written after it ran) replayed
// against any EGL/GLES driver, each call timed, to compare what drivers cost the CPU on the same frames.
//
//   glreplay <trace.gz> [--from <frame>] [--to <frame>] [--size <w>x<h>] [--check] [--no-finish] [--evict <KB>]
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
//   --hash <out> [--hash-frames A-B|last:N] [--hash-draws] [--hash-dump <dir>]   pixels, not times: each framebuffer's
//                colour read back and hashed where a pass ends (a bind) and at the swap, with --hash-draws after
//                every draw and clear too (glreplay_hash.c; two logs compared by glpixdiff)
//   --evict <KB> between calls (untimed), run <KB> of code and read <KB> of data, each from the next slice of
//                a 4 MB region of its own: the caches as a game's own code and data between its GL calls leave
//                them (a replayer alone keeps the driver's code hot, which the game does not)
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
#include <sys/mman.h>
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
// A map is kept by the buffer bound to its target (the trace's name for it, followed from the binds):
// Unity keeps a particle system's vertex and index buffers mapped while it maps, writes and unmaps
// other buffers on the same targets, and binds them again to flush them.
typedef struct { GLuint buf; unsigned char *p; } rmap;
static rmap g_rmap[32];
static struct { GLenum target; GLuint buf; } g_rbound[16];
static GLuint g_rvao, g_rebo[256];
static void rbound_note(GLenum t, GLuint buf)
{
    if (t == GL_ELEMENT_ARRAY_BUFFER) { g_rebo[g_rvao & 255] = buf; return; }
    for (int i = 0; i < 16; i++) if (g_rbound[i].target == t || !g_rbound[i].target) { g_rbound[i].target = t; g_rbound[i].buf = buf; return; }
}
static GLuint rbound(GLenum t)
{
    if (t == GL_ELEMENT_ARRAY_BUFFER) return g_rebo[g_rvao & 255];
    for (int i = 0; i < 16; i++) if (g_rbound[i].target == t) return g_rbound[i].buf;
    return 0;
}
static rmap *rmap_of(GLenum t, int add)
{
    GLuint buf = rbound(t);
    for (int i = 0; i < 32; i++) if (g_rmap[i].p && g_rmap[i].buf == buf) return &g_rmap[i];
    if (add) for (int i = 0; i < 32; i++) if (!g_rmap[i].p) { g_rmap[i].buf = buf; return &g_rmap[i]; }
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
// --show A-B: the calls A..B (glreplay's call numbers, as --hash names them) printed with their first
// arguments, and each program they use with its shaders' sources (kept from glShaderSource)
static const char *const glrec_name[OP_N];
static unsigned long g_show_a = 1, g_show_b;
static char *g_show_src[1 << 16];
static unsigned g_show_att[1 << 16][4];
static unsigned char g_show_done[1 << 16];
// --zero-discard (a measurement): every fragment shader with the one colour output Unity's translator names
// SV_Target0 gets a main() that runs the game's and then discards a fragment whose output is exactly vec4(0):
// under One/OneMinusSrcAlpha, SrcAlpha/OneMinusSrcAlpha and SrcAlpha/One such a fragment leaves its pixel as it
// was. Where a shader is drawn with another blend state the picture changes: compare the --hash logs.
static int g_zero_discard;
static unsigned long g_zero_discard_n;
static char *zero_discard_source(const char *src)
{
    const char *m = strstr(src, "void main()");
    if (!m || !strstr(src, "SV_Target0") || strstr(src, "SV_Target1") || strstr(src, "gl_FragDepth") || strstr(src, "gl_Position")) return NULL;
    static const char tail[] = "\nvoid main()\n{\n    ua_game_main();\n    if (SV_Target0 == vec4(0.0)) discard;\n}\n";
    size_t n = strlen(src);
    char *o = malloc(n + sizeof tail + 16);
    if (!o) return NULL;
    size_t k = (size_t)(m - src);
    memcpy(o, src, k);
    k += (size_t)sprintf(o + k, "void ua_game_main()");
    strcpy(o + k, m + 11);
    strcat(o, tail);
    return o;
}
static void rp_custom_glShaderSource(const uint64_t *a, uint64_t r)
{
    (void)r;
    const GLchar *src = rp_ptr(a[1]);
    char *zd = g_zero_discard && src ? zero_discard_source(src) : NULL;
    if (zd) { const GLchar *z = zd; g_zero_discard_n++; RX(glShaderSource)(rp_map(MK_SH, (GLuint)a[0]), 1, &z, NULL); free(zd); return; }
    if (g_show_b && src) { free(g_show_src[a[0] & 0xffff]); g_show_src[a[0] & 0xffff] = strdup(src); }
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
static unsigned long g_link_fail, g_compile_fail;
static unsigned long g_other_thread_calls;
static unsigned g_main_thread = 255;

// --hash: the trace's call count, the frame, the game's draw framebuffer, the window's size
static int g_hash; static unsigned long g_hcall; static long g_hframe; static unsigned g_hfb; static int g_hw = 720, g_hh = 480;

// ---- --evict: the caches between calls as a game leaves them
static size_t g_evict;                          // bytes a call
static unsigned char *g_ev_code, *g_ev_data;
static size_t g_ev_at;
#define EV_REGION (4u << 20)
static void evict_init(size_t kb)
{
    g_evict = kb << 10;
    if (!g_evict) return;
    if (g_evict > EV_REGION / 4) g_evict = EV_REGION / 4;
    g_ev_code = mmap(NULL, EV_REGION + 64, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_ev_data = malloc(EV_REGION);
    if (g_ev_code == MAP_FAILED || !g_ev_data) { g_evict = 0; return; }
    memset(g_ev_data, 1, EV_REGION);
    uint32_t *c = (uint32_t *)g_ev_code;
    size_t n = EV_REGION / 4;
#if defined(__aarch64__)
    for (size_t i = 0; i < n; i++) c[i] = 0xd503201f;                 // nop
    for (size_t i = 0; i + 1 < n; i += g_evict / 4) if (i) c[i - 1] = 0xd65f03c0;   // ret at each slice's end
    c[n - 1] = 0xd65f03c0; c[n] = 0xd65f03c0;
#elif defined(__arm__)
    for (size_t i = 0; i < n; i++) c[i] = 0xe1a00000;                 // mov r0, r0
    for (size_t i = 0; i + 1 < n; i += g_evict / 4) if (i) c[i - 1] = 0xe12fff1e;   // bx lr
    c[n - 1] = 0xe12fff1e; c[n] = 0xe12fff1e;
#else
    g_evict = 0; return;
#endif
    __builtin___clear_cache((char *)g_ev_code, (char *)g_ev_code + EV_REGION + 64);
    if (mprotect(g_ev_code, EV_REGION + 64, PROT_READ | PROT_EXEC)) g_evict = 0;
}
static volatile uint32_t g_ev_sink;
static void evict(void)
{
    if (!g_evict) return;
    if (g_ev_at + g_evict > EV_REGION) g_ev_at = 0;
    ((void (*)(void))(g_ev_code + g_ev_at))();
    uint32_t s = 0;
    for (size_t i = 0; i < g_evict; i += 64) s += g_ev_data[g_ev_at + i];
    g_ev_sink += s;
    g_ev_at += g_evict;
}

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
    if (g_show_b && r->op == OP_glAttachShader)
        for (int k = 0; k < 4; k++) if (!g_show_att[r->a[0] & 0xffff][k]) { g_show_att[r->a[0] & 0xffff][k] = (unsigned)r->a[1]; break; }
    if (g_show_b && hcall >= g_show_a && hcall <= g_show_b) {
        printf("call %lu %s %llx %llx %llx %llx\n", hcall, glrec_name[r->op], (unsigned long long)r->a[0], (unsigned long long)r->a[1],
               (unsigned long long)r->a[2], (unsigned long long)r->a[3]);
        unsigned pr = (unsigned)r->a[0] & 0xffff;
        if (r->op == OP_glUseProgram && !g_show_done[pr]) {
            g_show_done[pr] = 1;
            for (int k = 0; k < 4 && g_show_att[pr][k]; k++)
                printf("--- program %u shader %u ---\n%s\n--- end ---\n", pr, g_show_att[pr][k],
                       g_show_src[g_show_att[pr][k] & 0xffff] ? g_show_src[g_show_att[pr][k] & 0xffff] : "(no source)");
        }
    }
    if (r->op == OP_glBindBuffer) rbound_note((GLenum)r->a[0], (GLuint)r->a[1]);         // (for the maps: rmap_of)
    else if (r->op == OP_glBindBufferBase || r->op == OP_glBindBufferRange) rbound_note((GLenum)r->a[0], (GLuint)r->a[2]);
    else if (r->op == OP_glBindVertexArray) g_rvao = (GLuint)r->a[0];
    if (g_measuring) evict();
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
            GLuint sh[4]; GLsizei nsh = 0;
            RX(glGetAttachedShaders)(pr, 4, &nsh, sh);
            for (GLsizei k = 0; k < nsh; k++) {
                GLint cs = 0; char sl[1024] = { 0 };
                RX(glGetShaderiv)(sh[k], GL_COMPILE_STATUS, &cs);
                RX(glGetShaderInfoLog)(sh[k], sizeof sl, NULL, sl);
                fprintf(stderr, "glreplay:   its shader %u: compiled %d: %.900s\n", sh[k], cs, sl);
            }
        }
    }
    if (g_check && r->op == OP_glCompileShader && rp_fn[OP_glGetShaderiv] && rp_fn[OP_glGetShaderInfoLog]) {
        GLint ok = 0; GLuint sh = rp_map(MK_SH, (GLuint)r->a[0]);
        RX(glGetShaderiv)(sh, GL_COMPILE_STATUS, &ok);
        if (!ok && g_compile_fail++ < 5) {          // the driver's reason, and the source's first line (#version, extensions)
            char log[512] = { 0 }, src[160] = { 0 };
            RX(glGetShaderInfoLog)(sh, sizeof log, NULL, log);
            if (rp_fn[OP_glGetShaderSource]) RX(glGetShaderSource)(sh, sizeof src, NULL, src);
            for (char *c = src; *c; c++) if (*c == '\n') *c = ' ';
            fprintf(stderr, "glreplay: shader %u did not compile: %.400s [source: %.120s]\n", (unsigned)r->a[0], log, src);
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

// The driver: libEGL/libGLESv2 (GLREP_EGL/GLREP_GLES), a GLES 3 context on a WxH RGBA8 D24S8 pbuffer, made current.
static int open_driver(int w, int h, EGLint *maj, EGLint *min)
{
    const char *el = getenv("GLREP_EGL"), *gl = getenv("GLREP_GLES"), *pf = getenv("GLREP_PLATFORM");
    g_egl = dlopen(el && *el ? el : "libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!g_egl) g_egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    g_gles = dlopen(gl && *gl ? gl : "libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!g_gles) g_gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_egl || !g_gles) { fprintf(stderr, "glreplay: no EGL/GLES: %s\n", dlerror()); return -1; }
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
    if (g_dpy == EGL_NO_DISPLAY || !e_Initialize(g_dpy, maj, min)) { fprintf(stderr, "glreplay: eglInitialize failed\n"); return -1; }
    const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */, EGL_RED_SIZE, 8,
                          EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!e_ChooseConfig(g_dpy, ca, &cfg, 1, &n) || n < 1) { fprintf(stderr, "glreplay: no pbuffer RGBA8 D24S8 GLES3 config\n"); return -1; }
    e_BindAPI(EGL_OPENGL_ES_API);
    const EGLint sa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    g_surf = e_CreatePbufferSurface(g_dpy, cfg, sa);
    const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = e_CreateContext(g_dpy, cfg, EGL_NO_CONTEXT, xa);
    if (g_surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !e_MakeCurrent(g_dpy, g_surf, g_surf, ctx)) {
        fprintf(stderr, "glreplay: no pbuffer context\n"); return -1;
    }
    return 0;
}

// --info: what the driver tells an application (Unity picks its GLES path from these): EGL's client and display
// strings, every EGL config, the GL strings, the extensions one per line in the driver's order and the limits
// Unity reads. Two drivers' outputs are compared with diff (Mesa made to answer as libmali does).
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void print_info(EGLint maj, EGLint min)
{
    const char *(*q)(EGLDisplay, EGLint) = sym("eglQueryString");
    EGLBoolean (*getcfgs)(EGLDisplay, EGLConfig *, EGLint, EGLint *) = sym("eglGetConfigs");
    EGLBoolean (*cattr)(EGLDisplay, EGLConfig, EGLint, EGLint *) = sym("eglGetConfigAttrib");
    const GLubyte *(*gs)(GLenum) = sym("glGetString");
    const GLubyte *(*gsi)(GLenum, GLuint) = sym("glGetStringi");
    void (*gi)(GLenum, GLint *) = sym("glGetIntegerv");
    const char *s;
    printf("egl.version %d.%d\n", maj, min);
    if (q) {
        static const struct { EGLint e; const char *n; } es[] = { { EGL_VENDOR, "egl.vendor" }, { EGL_VERSION, "egl.version_string" },
                                                                  { EGL_CLIENT_APIS, "egl.client_apis" } };
        for (size_t i = 0; i < sizeof es / sizeof es[0]; i++) printf("%s %s\n", es[i].n, (s = q(g_dpy, es[i].e)) ? s : "(null)");
        for (int d = 0; d < 2; d++) {
            s = q(d ? g_dpy : EGL_NO_DISPLAY, EGL_EXTENSIONS);
            char *c = s ? strdup(s) : NULL, *sp = NULL;
            for (char *t = c ? strtok_r(c, " ", &sp) : NULL; t; t = strtok_r(NULL, " ", &sp)) printf("%s %s\n", d ? "egl.ext" : "egl.client_ext", t);
            if (!s) printf("%s (null)\n", d ? "egl.ext" : "egl.client_ext");
            free(c);
        }
    }
    EGLint nc = 0;
    if (getcfgs && cattr && getcfgs(g_dpy, NULL, 0, &nc) && nc > 0) {
        EGLConfig *cs = calloc(nc, sizeof *cs);
        getcfgs(g_dpy, cs, nc, &nc);
        static const struct { EGLint a; const char *n; } at[] = {
            { EGL_CONFIG_ID, "id" }, { EGL_RED_SIZE, "r" }, { EGL_GREEN_SIZE, "g" }, { EGL_BLUE_SIZE, "b" }, { EGL_ALPHA_SIZE, "a" },
            { EGL_DEPTH_SIZE, "depth" }, { EGL_STENCIL_SIZE, "stencil" }, { EGL_SAMPLES, "samples" }, { EGL_SAMPLE_BUFFERS, "sbuf" },
            { EGL_SURFACE_TYPE, "surface" }, { EGL_RENDERABLE_TYPE, "renderable" }, { EGL_CONFORMANT, "conformant" },
            { EGL_CONFIG_CAVEAT, "caveat" }, { EGL_NATIVE_VISUAL_ID, "visual" }, { EGL_COLOR_BUFFER_TYPE, "colorbuf" } };
        printf("egl.configs %d\n", nc);
        for (EGLint i = 0; i < nc; i++) {
            printf("egl.config");
            for (size_t k = 0; k < sizeof at / sizeof at[0]; k++) {
                EGLint v = 0;
                if (cattr(g_dpy, cs[i], at[k].a, &v)) printf(" %s=%s%x", at[k].n, at[k].a == EGL_CONFIG_ID ? "" : "0x", v);
            }
            printf("\n");
        }
        free(cs);
    }
    static const struct { GLenum e; const char *n; } gss[] = { { GL_VENDOR, "gl.vendor" }, { GL_RENDERER, "gl.renderer" },
                                                               { GL_VERSION, "gl.version" }, { GL_SHADING_LANGUAGE_VERSION, "gl.glsl" } };
    for (size_t i = 0; i < sizeof gss / sizeof gss[0]; i++) printf("%s %s\n", gss[i].n, (s = (const char *)gs(gss[i].e)) ? s : "(null)");
    GLint ne = 0;
    if (gi) gi(GL_NUM_EXTENSIONS, &ne);
    printf("gl.num_extensions %d\n", ne);
    char **ex = ne > 0 ? calloc(ne, sizeof *ex) : NULL;
    for (GLint i = 0; ex && gsi && i < ne; i++) ex[i] = strdup((s = (const char *)gsi(GL_EXTENSIONS, i)) ? s : "(null)");
    if (ex && gsi) {
        for (GLint i = 0; i < ne; i++) printf("gl.ext[%d] %s\n", i, ex[i]);
        qsort(ex, ne, sizeof *ex, cmp_str);              // sorted too: a diff that ignores the order
        for (GLint i = 0; i < ne; i++) { printf("gl.ext %s\n", ex[i]); free(ex[i]); }
    }
    free(ex);
    s = (const char *)gs(GL_EXTENSIONS);                 // the old single string (Unity reads this one on GLES)
    printf("gl.extensions_string %s\n", s ? s : "(null)");
    static const struct { GLenum e; const char *n; } lim[] = {
        { GL_MAX_TEXTURE_SIZE, "max_texture_size" }, { GL_MAX_CUBE_MAP_TEXTURE_SIZE, "max_cube_map_texture_size" },
        { GL_MAX_3D_TEXTURE_SIZE, "max_3d_texture_size" }, { GL_MAX_ARRAY_TEXTURE_LAYERS, "max_array_texture_layers" },
        { GL_MAX_RENDERBUFFER_SIZE, "max_renderbuffer_size" }, { GL_MAX_SAMPLES, "max_samples" },
        { GL_MAX_COLOR_ATTACHMENTS, "max_color_attachments" }, { GL_MAX_DRAW_BUFFERS, "max_draw_buffers" },
        { GL_MAX_VERTEX_ATTRIBS, "max_vertex_attribs" }, { GL_MAX_VERTEX_UNIFORM_VECTORS, "max_vertex_uniform_vectors" },
        { GL_MAX_FRAGMENT_UNIFORM_VECTORS, "max_fragment_uniform_vectors" }, { GL_MAX_VARYING_VECTORS, "max_varying_vectors" },
        { GL_MAX_TEXTURE_IMAGE_UNITS, "max_texture_image_units" }, { GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, "max_vertex_texture_image_units" },
        { GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, "max_combined_texture_image_units" }, { GL_MAX_UNIFORM_BLOCK_SIZE, "max_uniform_block_size" },
        { GL_MAX_UNIFORM_BUFFER_BINDINGS, "max_uniform_buffer_bindings" }, { GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, "ubo_offset_alignment" },
        { GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, "max_ssbo_bindings" }, { GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, "max_compute_invocations" },
        { GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, "max_compute_shared_memory" }, { GL_MAX_TEXTURE_LOD_BIAS, "max_texture_lod_bias" },
        { GL_NUM_PROGRAM_BINARY_FORMATS, "num_program_binary_formats" }, { GL_NUM_SHADER_BINARY_FORMATS, "num_shader_binary_formats" },
        { GL_NUM_COMPRESSED_TEXTURE_FORMATS, "num_compressed_texture_formats" } };
    for (size_t i = 0; gi && i < sizeof lim / sizeof lim[0]; i++) { GLint v = -1; gi(lim[i].e, &v); printf("gl.%s %d\n", lim[i].n, v); }
    GLint nf = 0;
    if (gi) gi(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &nf);
    if (gi && nf > 0 && nf < 4096) {
        GLint *f = calloc(nf, sizeof *f);
        gi(GL_COMPRESSED_TEXTURE_FORMATS, f);
        for (GLint i = 0; i < nf; i++) printf("gl.compressed_format 0x%x\n", f[i]);
        free(f);
    }
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    long from = -1, to = -1;
    int w = 720, h = 480;
    const char *hash_out = NULL, *hash_frames = NULL, *hash_dump = NULL;
    int hash_draws = 0, info = 0;
    size_t evict_kb = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--from") && i + 1 < argc) from = atol(argv[++i]);
        else if (!strcmp(argv[i], "--to") && i + 1 < argc) to = atol(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
        else if (!strcmp(argv[i], "--check")) g_check = 1;
        else if (!strcmp(argv[i], "--no-finish")) g_finish = 0;
        else if (!strcmp(argv[i], "--evict") && i + 1 < argc) evict_kb = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--hash") && i + 1 < argc) hash_out = argv[++i];
        else if (!strcmp(argv[i], "--hash-frames") && i + 1 < argc) hash_frames = argv[++i];
        else if (!strcmp(argv[i], "--hash-draws")) hash_draws = 1;
        else if (!strcmp(argv[i], "--hash-dump") && i + 1 < argc) hash_dump = argv[++i];
        else if (!strcmp(argv[i], "--info")) info = 1;
        else if (!strcmp(argv[i], "--zero-discard")) g_zero_discard = 1;
        else if (!strcmp(argv[i], "--show") && i + 1 < argc) sscanf(argv[++i], "%lu-%lu", &g_show_a, &g_show_b);
        else path = argv[i];
    }
    if (info) {                                          // no trace: the driver's strings, configs and limits
        EGLint maj = 0, min = 0;
        if (open_driver(w, h, &maj, &min) < 0) return 1;
        print_info(maj, min);
        return 0;
    }
    if (!path) { fprintf(stderr, "usage: glreplay --info | <trace.gz> [--from F] [--to F] [--size WxH] [--check] [--no-finish] [--hash <out> [--hash-frames A-B] [--hash-draws] [--hash-dump <dir>]] [--show A-B] [--evict KB]\n"); return 2; }
    time_init();
    classes();
    evict_init(evict_kb);
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
    EGLint maj = 0, min = 0;
    if (open_driver(w, h, &maj, &min) < 0) return 1;
    unsigned nfn = 0;
    for (unsigned i = 0; i < OP_N; i++) if ((rp_fn[i] = sym(glrec_name[i]))) nfn++;
    char last_frames[64];                                // --hash-frames last:N: the trace's last N frames
    if (hash_frames && !strncmp(hash_frames, "last:", 5)) {
        long n_ = atol(hash_frames + 5);
        snprintf(last_frames, sizeof last_frames, "%ld-%ld", nframes > n_ ? nframes - n_ : 0, nframes - 1);
        hash_frames = last_frames;
    }
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
    printf("frames measured: %lu (%ld - %ld), timer: %s%s", g_frames_measured, from, to - 1,
           g_counter ? "generic timer" : "clock_gettime", g_finish ? ", glFinish after each frame (untimed)" : "");
    if (g_evict) printf(", %zu KB of code and data run between calls (untimed)", g_evict >> 10);
    printf("\n");
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
    if (g_zero_discard) printf("--zero-discard: %lu fragment shaders changed\n", g_zero_discard_n);
    printf("\nnot replayable: names not seen made %lu, uniform locations not asked for %lu, attribute locations that moved %lu, "
           "calls from other threads %lu, entry points this driver lacks (window) %lu\n",
           g_unmapped_names, g_loc_miss, g_attrib_moved, g_other_thread_calls, missing);
    for (unsigned i = 0; i < OP_N; i++) if (g_unsupported[i]) printf("  unsupported %s: %lu\n", glrec_name[i], g_unsupported[i]);
    if (g_check) {
        unsigned long tot = 0;
        for (unsigned i = 0; i < OP_N; i++) if (g_op_err[i]) { printf("  GL errors in %s: %lu (first 0x%04x)\n", glrec_name[i], g_op_err[i], g_op_firsterr[i]); tot += g_op_err[i]; }
        printf("GL errors: %lu; programs that did not link: %lu; shaders that did not compile: %lu\n", tot, g_link_fail, g_compile_fail);
    }
    return 0;
}
