// The frame graph of glrec.c (UA_GL_REC=2): a held frame's passes, what each reads and writes,
// and the equivalent stream a tiler does with less work.
//
// A tiler renders a pass (the commands between two framebuffer changes) tile by tile in on-chip
// memory: it loads each attachment the pass does not clear or invalidate first, and stores each
// one at the pass's end. Knowing the whole frame, the host can see which of those loads, stores and
// passes have no effect on anything the frame (or a later one) can read, and leave them out:
//
//   dead pass     a pass (not the window's) whose every result is overwritten whole (cleared or
//                 invalidated) later in the held frame before anything reads it: its draws,
//                 clears and blits are not submitted (its state calls are);
//   end inval     an attachment nothing reads again before it is overwritten whole later in the
//                 held frame (or, for the window's depth/stencil, before the swap: EGL leaves the
//                 window's ancillary buffers undefined after it): invalidated at the pass's end,
//                 so it is not stored;
//   start inval   a depth/stencil buffer a pass loads but does not test, and that is dead after
//                 the pass as above: invalidated at the pass's start, so it is not loaded;
//   clear sink    a pass that only clears, followed (after passes that touch none of its
//                 attachments) by a pass on the same framebuffer: its clears are made at that
//                 pass's start instead, with the clear state they had (put back after), so one
//                 pass begins with a clear and the clears' own pass is gone.
//
// What a pass reads: the textures its draws' programs sample (the program's sampler uniforms and
// the textures bound to those units; a program's samplers are read from the driver once and its
// sampler uniforms followed from then on), blit and copy sources, the depth/stencil buffer where a
// draw tests it (depth test with a function other than ALWAYS/NEVER, stencil test with one, or a
// stencil operation that reads the old value), and its own colour attachments where it loads them
// (whether its draws cover every pixel is not known: a loaded colour buffer counts as read). A
// depth/stencil buffer a pass loads without testing it is passed through. What the held calls do
// not show is asked from the driver once per held frame (its state is then that of the frame's
// start): the bound framebuffers, texture units, program and fixed-function state, and each
// framebuffer's attachments (bound for the question and back). A held frame with anything this
// does not follow (a draw without a program, a sampler unit over 31, multiview attachments) is
// submitted unchanged; nothing is left out while a query, transform feedback, image or shader
// storage buffer binding could see the draws (side effects). Where the driver lets shaders read the
// depth/stencil buffer (ARM_shader_framebuffer_fetch_depth_stencil), a draw whose program may do so
// reads it: a program whose shaders' sources name gl_LastFragDepth/gl_LastFragStencil, or one this
// did not see compiled from source (Unity's program binary cache is then turned off, as
// UA_SHADER_DUMP does, so that it compiles every program from source).
//   UA_GL_REC_OPT=dead,end,start,sink (default all) picks the changes; UA_GL_REC_VERBOSE=<n> logs
//   the decisions of the first <n> held frames.
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#define GLREC_PART 1
#include "glrec_gen.h"
#include "glrec_fg.h"

static int o_dead = 1, o_end = 1, o_start = 1, o_sink = 1;
static int g_verbose;
static int g_ds_fetch = -1;        // the driver lets shaders read depth/stencil (-1: not known yet)

static void (*d_GetIntegerv)(GLenum, GLint *);
static void (*d_GetFloatv)(GLenum, GLfloat *);
static void (*d_GetBooleanv)(GLenum, GLboolean *);
static GLboolean (*d_IsEnabled)(GLenum);
static void (*d_BindFramebuffer)(GLenum, GLuint);
static void (*d_GetFbAttachment)(GLenum, GLenum, GLenum, GLint *);
static void (*d_ActiveTexture)(GLenum);
static void (*d_GetProgramiv)(GLuint, GLenum, GLint *);
static void (*d_GetActiveUniform)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *);
static GLint (*d_GetUniformLocation)(GLuint, const GLchar *);
static void (*d_GetUniformiv)(GLuint, GLint, GLint *);

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

// ---- resources: an attachment's image (kind, object, level, layer); reads match the object
enum { RK_TEX = 1, RK_RB, RK_WIN_C, RK_WIN_D, RK_WIN_S };
static inline uint64_t rkey(uint64_t kind, GLuint name, unsigned level, unsigned layer)
{ return kind << 56 | (uint64_t)(level & 0xff) << 48 | (uint64_t)(layer & 0xffff) << 32 | name; }
static inline uint64_t robj(uint64_t k) { return k & (0xffull << 56 | 0xffffffffull); }
static inline int is_window(uint64_t k) { unsigned t = (unsigned)(k >> 56); return t >= RK_WIN_C; }

// ---- framebuffers: their attachments, asked from the driver once and followed from then on
// (every attachment change is a held call; a held frame that deletes textures, renderbuffers or
// framebuffers is submitted unchanged and the table forgotten, as at eglMakeCurrent)
#define NSLOT 6                    // colour 0-3, depth, stencil
enum { S_DEPTH = 4, S_STENCIL = 5 };
typedef struct { GLuint fb; unsigned ver; int bad; uint64_t att[NSLOT]; } fbi_t;
#define NFBI 256
static fbi_t g_fbi[NFBI];
static int g_nfbi, g_fbi_reset;
static int g_win_known, g_win_d, g_win_s;
static GLint g_start_draw_fb;

static fbi_t *fbi(GLuint fb)
{
    for (int i = 0; i < g_nfbi; i++) if (g_fbi[i].fb == fb) return &g_fbi[i];
    if (g_nfbi >= NFBI) { g_fbi_reset = 1; return NULL; }
    fbi_t *f = &g_fbi[g_nfbi++];
    memset(f, 0, sizeof *f);
    f->fb = fb;
    if (fb == 0) {
        if (!g_win_known) {
            GLint t = 0;
            d_BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            d_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &t); g_win_d = t != GL_NONE;
            t = 0;
            d_GetFbAttachment(GL_DRAW_FRAMEBUFFER, GL_STENCIL, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &t); g_win_s = t != GL_NONE;
            d_BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)g_start_draw_fb);
            g_win_known = 1;
        }
        f->att[0] = rkey(RK_WIN_C, 0, 0, 0);
        if (g_win_d) f->att[S_DEPTH] = rkey(RK_WIN_D, 0, 0, 0);
        if (g_win_s) f->att[S_STENCIL] = rkey(RK_WIN_S, 0, 0, 0);
        return f;
    }
    static const GLenum slot[NSLOT] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3,
                                        GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT };
    d_BindFramebuffer(GL_DRAW_FRAMEBUFFER, fb);
    for (int s = 0; s < NSLOT; s++) {
        GLint type = 0, name = 0, level = 0, layer = 0, face = 0;
        d_GetFbAttachment(GL_DRAW_FRAMEBUFFER, slot[s], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        if (type == GL_TEXTURE) {
            d_GetFbAttachment(GL_DRAW_FRAMEBUFFER, slot[s], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
            d_GetFbAttachment(GL_DRAW_FRAMEBUFFER, slot[s], GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
            d_GetFbAttachment(GL_DRAW_FRAMEBUFFER, slot[s], GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER, &layer);
            d_GetFbAttachment(GL_DRAW_FRAMEBUFFER, slot[s], GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE, &face);
            if (face) layer = face - GL_TEXTURE_CUBE_MAP_POSITIVE_X;
            f->att[s] = rkey(RK_TEX, (GLuint)name, (unsigned)level, (unsigned)layer);
        } else if (type == GL_RENDERBUFFER) {
            d_GetFbAttachment(GL_DRAW_FRAMEBUFFER, slot[s], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
            f->att[s] = rkey(RK_RB, (GLuint)name, 0, 0);
        } else if (type != GL_NONE) f->bad = 1;
    }
    d_BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)g_start_draw_fb);
    return f;
}

static int slots_of(GLenum a, int window, int *s)
{
    if (window) {
        if (a == GL_COLOR) { s[0] = 0; return 1; }
        if (a == GL_DEPTH) { s[0] = S_DEPTH; return 1; }
        if (a == GL_STENCIL) { s[0] = S_STENCIL; return 1; }
        return 0;
    }
    if (a >= GL_COLOR_ATTACHMENT0 && a < GL_COLOR_ATTACHMENT0 + 4) { s[0] = (int)(a - GL_COLOR_ATTACHMENT0); return 1; }
    if (a == GL_DEPTH_ATTACHMENT) { s[0] = S_DEPTH; return 1; }
    if (a == GL_STENCIL_ATTACHMENT) { s[0] = S_STENCIL; return 1; }
    if (a == GL_DEPTH_STENCIL_ATTACHMENT) { s[0] = S_DEPTH; s[1] = S_STENCIL; return 2; }
    return 0;
}

// ---- programs: their samplers (location, texture target, unit), followed across held frames
enum { T2D, TCUBE, T3D, T2DA, TEXT, TMS, TCA, NTGT };
static const GLenum tgt_binding[NTGT] = { GL_TEXTURE_BINDING_2D, GL_TEXTURE_BINDING_CUBE_MAP, GL_TEXTURE_BINDING_3D,
                                          GL_TEXTURE_BINDING_2D_ARRAY, GL_TEXTURE_BINDING_EXTERNAL_OES,
                                          GL_TEXTURE_BINDING_2D_MULTISAMPLE, GL_TEXTURE_BINDING_CUBE_MAP_ARRAY };
static int tgt_index(GLenum t)
{
    switch (t) {
    case GL_TEXTURE_2D: return T2D; case GL_TEXTURE_CUBE_MAP: return TCUBE; case GL_TEXTURE_3D: return T3D;
    case GL_TEXTURE_2D_ARRAY: return T2DA; case GL_TEXTURE_EXTERNAL_OES: return TEXT;
    case GL_TEXTURE_2D_MULTISAMPLE: return TMS; case GL_TEXTURE_CUBE_MAP_ARRAY: return TCA;
    default: return -1;
    }
}
static int sampler_tgt(GLenum type)
{
    switch (type) {
    case GL_SAMPLER_2D: case GL_SAMPLER_2D_SHADOW: case GL_INT_SAMPLER_2D: case GL_UNSIGNED_INT_SAMPLER_2D: return T2D;
    case GL_SAMPLER_CUBE: case GL_SAMPLER_CUBE_SHADOW: case GL_INT_SAMPLER_CUBE: case GL_UNSIGNED_INT_SAMPLER_CUBE: return TCUBE;
    case GL_SAMPLER_3D: case GL_INT_SAMPLER_3D: case GL_UNSIGNED_INT_SAMPLER_3D: return T3D;
    case GL_SAMPLER_2D_ARRAY: case GL_SAMPLER_2D_ARRAY_SHADOW: case GL_INT_SAMPLER_2D_ARRAY: case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY: return T2DA;
    case GL_SAMPLER_EXTERNAL_OES: return TEXT;
    case GL_SAMPLER_2D_MULTISAMPLE: case GL_INT_SAMPLER_2D_MULTISAMPLE: case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE: return TMS;
    case GL_SAMPLER_CUBE_MAP_ARRAY: case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW: case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
    case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY: return TCA;
    case GL_SAMPLER_BUFFER: case GL_INT_SAMPLER_BUFFER: case GL_UNSIGNED_INT_SAMPLER_BUFFER: return -2;   // (a buffer: no image)
    default: return -1;
    }
}
// which programs may read depth/stencil through framebuffer fetch (from any thread's calls)
#define NSH 8192
typedef struct { GLuint name; unsigned char used, fetch, nsh, linked; GLuint sh[6]; } shprog_t;
static shprog_t g_sh[NSH], g_pr[NSH];
static pthread_mutex_t g_shlock = PTHREAD_MUTEX_INITIALIZER;
static shprog_t *sp_find(shprog_t *t, GLuint name, int add)
{
    unsigned i = (name * 2654435761u) % NSH;
    for (unsigned k = 0; k < NSH; k++, i = (i + 1) % NSH) {
        if (t[i].used && t[i].name == name) return &t[i];
        if (!t[i].used) {
            if (!add) return NULL;
            memset(&t[i], 0, sizeof t[i]);
            t[i].used = 1; t[i].name = name;
            return &t[i];
        }
    }
    return NULL;
}
void fg_note_shader(GLuint sh, int fetch)
{
    pthread_mutex_lock(&g_shlock);
    shprog_t *e = sp_find(g_sh, sh, 1);
    if (e) e->fetch = (unsigned char)(fetch != 0);
    pthread_mutex_unlock(&g_shlock);
}
void fg_note_attach(GLuint prog, GLuint sh)
{
    pthread_mutex_lock(&g_shlock);
    shprog_t *e = sp_find(g_pr, prog, 1);
    if (e && e->nsh < 6) e->sh[e->nsh++] = sh;
    else if (e) e->nsh = 0xff;                           // (too many: unknown)
    pthread_mutex_unlock(&g_shlock);
}
void fg_note_link(GLuint prog, int binary)
{
    pthread_mutex_lock(&g_shlock);
    shprog_t *e = sp_find(g_pr, prog, 1);
    if (e) {
        int f = binary || e->nsh == 0xff || e->nsh == 0;
        for (int i = 0; !f && i < e->nsh; i++) { shprog_t *s = sp_find(g_sh, e->sh[i], 0); if (!s || s->fetch) f = 1; }
        e->fetch = (unsigned char)f; e->linked = 1;
    }
    pthread_mutex_unlock(&g_shlock);
}
void fg_note_delete(GLuint prog)
{
    pthread_mutex_lock(&g_shlock);
    shprog_t *e = sp_find(g_pr, prog, 0);
    if (e) { e->nsh = 0; e->linked = 0; e->fetch = 0; }     // (the name may come back: its shaders start over)
    pthread_mutex_unlock(&g_shlock);
}
static int prog_dsfetch(GLuint prog)
{
    pthread_mutex_lock(&g_shlock);
    shprog_t *e = sp_find(g_pr, prog, 0);
    int f = !e || !e->linked || e->fetch;
    pthread_mutex_unlock(&g_shlock);
    return f;
}
// the driver's extension (a context is current on the calling thread)
static void check_ext(void)
{
    if (g_ds_fetch >= 0) return;
    const GLubyte *(*gs)(GLenum) = NULL;
    void *h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (h) *(void **)&gs = dlsym(h, "glGetString");
    const char *ext = gs ? (const char *)gs(GL_EXTENSIONS) : NULL;
    if (!ext) return;
    g_ds_fetch = strstr(ext, "GL_ARM_shader_framebuffer_fetch_depth_stencil") != NULL;
}
int fg_sources_wanted(void) { check_ext(); return g_ds_fetch == 1; }

#define PSMP 32
typedef struct { GLuint prog; unsigned char used, bad, n, dsf; struct { GLint loc; signed char tgt; unsigned char unit; } s[PSMP]; } prog_t;
#define NPROG 1024
static prog_t g_prog[NPROG];
static int g_nprog;

static prog_t *prog_info(GLuint p)
{
    unsigned i = (p * 2654435761u) % NPROG;
    for (unsigned k = 0; k < NPROG; k++, i = (i + 1) % NPROG) {
        if (g_prog[i].used && g_prog[i].prog == p) return &g_prog[i];
        if (!g_prog[i].used) break;
    }
    if (g_nprog > NPROG / 2) { memset(g_prog, 0, sizeof g_prog); g_nprog = 0; i = (p * 2654435761u) % NPROG; }
    while (g_prog[i].used) i = (i + 1) % NPROG;
    prog_t *r = &g_prog[i];
    memset(r, 0, sizeof *r);
    r->prog = p; r->used = 1; g_nprog++;
    if (!p) { r->bad = 1; return r; }
    r->dsf = g_ds_fetch != 0 && prog_dsfetch(p);
    GLint nu = 0;
    d_GetProgramiv(p, GL_ACTIVE_UNIFORMS, &nu);
    for (GLint u = 0; u < nu; u++) {
        char name[128]; GLsizei len = 0; GLint size = 0; GLenum type = 0;
        d_GetActiveUniform(p, (GLuint)u, sizeof name - 8, &len, &size, &type, name);
        int t = sampler_tgt(type);
        if (t == -1) continue;
        if (t == -2) continue;
        if (len > 3 && !strcmp(name + len - 3, "[0]")) name[len - 3] = 0;
        for (GLint e = 0; e < size; e++) {
            char el[140];
            if (size > 1) snprintf(el, sizeof el, "%s[%d]", name, e); else snprintf(el, sizeof el, "%s", name);
            GLint loc = d_GetUniformLocation(p, el), unit = 0;
            if (loc < 0) continue;
            d_GetUniformiv(p, loc, &unit);
            if (r->n >= PSMP || unit < 0 || unit >= 32) { r->bad = 1; continue; }
            r->s[r->n].loc = loc; r->s[r->n].tgt = (signed char)t; r->s[r->n].unit = (unsigned char)unit; r->n++;
        }
    }
    return r;
}
static void prog_uniform(GLuint p, GLint loc, GLsizei count, const GLint *v)
{
    if (!p || loc < 0) return;
    prog_t *r = prog_info(p);
    for (GLsizei c = 0; c < count; c++)
        for (int i = 0; i < r->n; i++)
            if (r->s[i].loc == loc + c) {
                if (v[c] < 0 || v[c] >= 32) r->bad = 1; else r->s[i].unit = (unsigned char)v[c];
            }
}

// ---- the walk's state (what the held frame's calls set; the rest from the driver)
typedef struct {
    GLboolean scissor, discard, cmask[4]; GLint box[4]; GLboolean dmask; GLuint smask[2];
    GLfloat ccol[4], cdepth; GLint cstencil;
} clearstate;
typedef struct {
    GLuint draw_fb, read_fb, prog; int active;
    GLuint tex[32][NTGT]; unsigned char tex_known[32];
    GLboolean depth_test, stencil_test;
    GLenum depth_func, sfunc[2], sop[2][3];
    int cmaski;
    clearstate cs;
    unsigned csver;
} wstate;
static wstate s;
static int g_start_active;           // the driver's active texture unit (the held frame's start's)

static void state_from_driver(void)
{
    memset(&s, 0, sizeof s);
    GLint v = 0;
    d_GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &v); s.draw_fb = (GLuint)v; g_start_draw_fb = v;
    d_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &v); s.read_fb = (GLuint)v;
    d_GetIntegerv(GL_CURRENT_PROGRAM, &v); s.prog = (GLuint)v;
    d_GetIntegerv(GL_ACTIVE_TEXTURE, &v); s.active = v - GL_TEXTURE0;
    s.depth_test = d_IsEnabled(GL_DEPTH_TEST); s.stencil_test = d_IsEnabled(GL_STENCIL_TEST);
    s.cs.scissor = d_IsEnabled(GL_SCISSOR_TEST); s.cs.discard = d_IsEnabled(GL_RASTERIZER_DISCARD);
    d_GetIntegerv(GL_DEPTH_FUNC, &v); s.depth_func = (GLenum)v;
    d_GetIntegerv(GL_STENCIL_FUNC, &v); s.sfunc[0] = (GLenum)v;
    d_GetIntegerv(GL_STENCIL_BACK_FUNC, &v); s.sfunc[1] = (GLenum)v;
    static const GLenum ops[2][3] = { { GL_STENCIL_FAIL, GL_STENCIL_PASS_DEPTH_FAIL, GL_STENCIL_PASS_DEPTH_PASS },
                                      { GL_STENCIL_BACK_FAIL, GL_STENCIL_BACK_PASS_DEPTH_FAIL, GL_STENCIL_BACK_PASS_DEPTH_PASS } };
    for (int f = 0; f < 2; f++) for (int o = 0; o < 3; o++) { d_GetIntegerv(ops[f][o], &v); s.sop[f][o] = (GLenum)v; }
    d_GetIntegerv(GL_STENCIL_WRITEMASK, &v); s.cs.smask[0] = (GLuint)v;
    d_GetIntegerv(GL_STENCIL_BACK_WRITEMASK, &v); s.cs.smask[1] = (GLuint)v;
    d_GetBooleanv(GL_COLOR_WRITEMASK, s.cs.cmask);
    d_GetBooleanv(GL_DEPTH_WRITEMASK, &s.cs.dmask);
    d_GetIntegerv(GL_SCISSOR_BOX, s.cs.box);
    d_GetFloatv(GL_COLOR_CLEAR_VALUE, s.cs.ccol);
    d_GetFloatv(GL_DEPTH_CLEAR_VALUE, &s.cs.cdepth);
    d_GetIntegerv(GL_STENCIL_CLEAR_VALUE, &s.cs.cstencil);
}

static GLuint tex_bound(int unit, int t)
{
    if (!(s.tex_known[unit] & (1u << t))) {
        GLint v = 0;
        d_ActiveTexture(GL_TEXTURE0 + (GLenum)unit);
        d_GetIntegerv(tgt_binding[t], &v);
        d_ActiveTexture(GL_TEXTURE0 + (GLenum)g_start_active);          // (the driver's: the held frame's start's)
        s.tex[unit][t] = (GLuint)v;
        s.tex_known[unit] |= (unsigned char)(1u << t);
    }
    return s.tex[unit][t];
}

// ---- passes
typedef struct {
    GLuint fb; fbi_t *f; unsigned ver;
    uint64_t att[NSLOT];
    size_t k_first, k_last;
    unsigned full, written, tested, inval_end;        // over slots
    int ndraws, nclears, nother;
    uint32_t r0, nr, w0, nw;                           // reads / other writes in g_rw
    clearstate cs_first; unsigned csver_first, csver_last;
    int dead, sunk;
} pass_t;
static pass_t *g_pass;
static size_t g_npass, g_cappass;
static uint64_t *g_rw;                 // read and write keys
static size_t g_nrw, g_caprw;
static size_t *g_koff;                 // command k -> word offset
static size_t g_capk;
static unsigned char *g_skip;
static size_t g_capskip;
typedef struct { size_t k; int after; int kind; unsigned slots; size_t pass; } insert_t;   // kind 0 inval, 1 sunk clears
static insert_t *g_ins;
static size_t g_nins, g_capins;
static pass_t *g_cur;
static int g_opaque;
static int g_side;                     // shader storage / images seen: side effects possible
static int g_queries, g_tf;            // queries / transform feedback active

static void *grow(void *p, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) return p;
    size_t c = *cap ? *cap : 256;
    while (c < need) c *= 2;
    void *n = realloc(p, c * elem);
    if (!n) { fprintf(stderr, "[glrec] out of memory\n"); abort(); }
    *cap = c;
    return n;
}

static pass_t *work(size_t k)
{
    if (!g_cur) {
        g_pass = grow(g_pass, &g_cappass, g_npass + 1, sizeof *g_pass);
        pass_t *p = &g_pass[g_npass++];
        memset(p, 0, sizeof *p);
        p->fb = s.draw_fb;
        p->f = fbi(s.draw_fb);
        if (!p->f || p->f->bad) g_opaque = 1;
        if (p->f) { p->ver = p->f->ver; memcpy(p->att, p->f->att, sizeof p->att); }
        p->k_first = k;
        p->cs_first = s.cs; p->csver_first = s.csver;
        p->r0 = (uint32_t)g_nrw;
        g_cur = p;
    }
    g_cur->k_last = k;
    g_cur->csver_last = s.csver;
    return g_cur;
}
static void add_rw(pass_t *p, uint64_t key, int write)
{
    g_rw = grow(g_rw, &g_caprw, g_nrw + 1, sizeof *g_rw);
    g_rw[g_nrw++] = key | (write ? 1ull << 55 : 0);
    p->nr++;
}
static int started(const pass_t *p) { return p->ndraws || p->nother; }
static unsigned present(const pass_t *p) { unsigned m = 0; for (int i = 0; i < NSLOT; i++) if (p->att[i]) m |= 1u << i; return m; }
static void wrote(pass_t *p, unsigned slots) { p->written |= slots & present(p); p->inval_end &= ~slots; }

static void clear_slots(pass_t *p, unsigned slots)
{
    slots &= present(p);
    if (!slots || s.cs.discard) return;
    unsigned full = 0, w = 0;
    int all_c = s.cs.cmask[0] && s.cs.cmask[1] && s.cs.cmask[2] && s.cs.cmask[3] && !s.cmaski;
    for (int i = 0; i < 4; i++) if (slots & (1u << i)) {
        if (all_c) full |= 1u << i;
        if (s.cs.cmask[0] || s.cs.cmask[1] || s.cs.cmask[2] || s.cs.cmask[3] || s.cmaski) w |= 1u << i;
    }
    if ((slots & (1u << S_DEPTH)) && s.cs.dmask) { full |= 1u << S_DEPTH; w |= 1u << S_DEPTH; }
    if ((slots & (1u << S_STENCIL)) && (s.cs.smask[0] & 0xff) == 0xff && (s.cs.smask[1] & 0xff) == 0xff) full |= 1u << S_STENCIL;
    if ((slots & (1u << S_STENCIL)) && ((s.cs.smask[0] | s.cs.smask[1]) & 0xff)) w |= 1u << S_STENCIL;
    if (!s.cs.scissor && !started(p)) p->full |= full;
    wrote(p, w);
}

static int ds_read_now(void)
{
    // (bit 0: depth values read, bit 1: stencil values read)
    int r = 0;
    if (s.depth_test && s.depth_func != GL_ALWAYS && s.depth_func != GL_NEVER) r |= 1;
    if (s.stencil_test) {
        for (int f = 0; f < 2; f++) {
            if (s.sfunc[f] != GL_ALWAYS && s.sfunc[f] != GL_NEVER) r |= 2;
            for (int o = 0; o < 3; o++) {
                GLenum op = s.sop[f][o];
                if (op == GL_INCR || op == GL_DECR || op == GL_INCR_WRAP || op == GL_DECR_WRAP || op == GL_INVERT) r |= 2;
            }
        }
    }
    return r;
}

static int is_draw(unsigned op)
{
    switch (op) {
    case OP_glDrawArrays: case OP_glDrawElements: case OP_glDrawArraysInstanced: case OP_glDrawElementsInstanced:
    case OP_glDrawArraysInstancedEXT: case OP_glDrawElementsInstancedEXT: case OP_glDrawRangeElements:
    case OP_glDrawElementsBaseVertex: case OP_glDrawElementsBaseVertexEXT: case OP_glDrawElementsBaseVertexOES:
    case OP_glDrawRangeElementsBaseVertex: case OP_glDrawRangeElementsBaseVertexEXT: case OP_glDrawRangeElementsBaseVertexOES:
    case OP_glDrawElementsInstancedBaseVertex: case OP_glDrawElementsInstancedBaseVertexEXT:
    case OP_glDrawElementsInstancedBaseVertexOES: case OP_glDrawArraysInstancedBaseInstanceEXT:
    case OP_glDrawElementsInstancedBaseInstanceEXT: case OP_glDrawElementsInstancedBaseVertexBaseInstanceEXT:
    case OP_glDrawArraysIndirect: case OP_glDrawElementsIndirect:
        return 1;
    default:
        return 0;
    }
}

static void draw(size_t k)
{
    pass_t *p = work(k);
    if (s.cs.discard) return;
    p->ndraws++;
    prog_t *pi = prog_info(s.prog);
    if (pi->bad) { g_opaque = 1; return; }
    if (pi->dsf) p->tested |= 1u << S_DEPTH | 1u << S_STENCIL;     // (its shaders may read them)
    for (int i = 0; i < pi->n; i++) {
        GLuint t = tex_bound(pi->s[i].unit, pi->s[i].tgt);
        if (t) add_rw(p, rkey(RK_TEX, t, 0, 0), 0);
    }
    int rd = ds_read_now();
    if (rd & 1) p->tested |= 1u << S_DEPTH;
    if (rd & 2) p->tested |= 1u << S_STENCIL;
    unsigned w = 0;
    if (s.cs.cmask[0] || s.cs.cmask[1] || s.cs.cmask[2] || s.cs.cmask[3] || s.cmaski) w |= 0xf;
    if (s.depth_test && s.cs.dmask) w |= 1u << S_DEPTH;
    if (s.stencil_test) {
        for (int f = 0; f < 2; f++)
            for (int o = 0; o < 3; o++)
                if (s.sop[f][o] != GL_KEEP && (s.cs.smask[f] & 0xff)) w |= 1u << S_STENCIL;
    }
    wrote(p, w);
}

static void attach(GLenum target, GLenum attachment, uint64_t key, int bad)
{
    GLuint fb = target == GL_READ_FRAMEBUFFER ? s.read_fb : s.draw_fb;
    fbi_t *f = fbi(fb);
    if (!f || fb == 0) { g_opaque = 1; return; }
    int sl[2], n = slots_of(attachment, 0, sl);
    for (int i = 0; i < n; i++) f->att[sl[i]] = key;
    if (bad) f->bad = 1;
    f->ver++;
    if (fb == s.draw_fb) g_cur = NULL;
}

static void invalidate(size_t k, GLenum target, GLsizei n, const GLenum *a)
{
    if (target == GL_READ_FRAMEBUFFER && s.read_fb != s.draw_fb) return;
    pass_t *p = work(k);
    unsigned m = 0;
    for (GLsizei i = 0; a && i < n; i++) { int sl[2], c = slots_of(a[i], p->fb == 0, sl); for (int j = 0; j < c; j++) m |= 1u << sl[j]; }
    m &= present(p);
    if (!started(p)) p->full |= m;
    else p->inval_end |= m;
}

static void blit(size_t k, GLbitfield mask)
{
    pass_t *p = work(k);
    p->nother++;
    fbi_t *rf = fbi(s.read_fb);
    if (!rf || rf->bad) { g_opaque = 1; return; }
    unsigned w = 0;
    if (mask & GL_COLOR_BUFFER_BIT) { for (int i = 0; i < 4; i++) if (rf->att[i]) add_rw(p, rf->att[i], 0); w |= 0xf; }
    if (mask & GL_DEPTH_BUFFER_BIT) { if (rf->att[S_DEPTH]) add_rw(p, rf->att[S_DEPTH], 0); w |= 1u << S_DEPTH; }
    if (mask & GL_STENCIL_BUFFER_BIT) { if (rf->att[S_STENCIL]) add_rw(p, rf->att[S_STENCIL], 0); w |= 1u << S_STENCIL; }
    wrote(p, w);
}

// a texture upload (between passes): a pass of its own that writes part of the texture, so that
// nothing is moved across it or taken for overwritten by it
#define FB_UPLOAD 0xffffffffu
static void upload(size_t k, GLenum target)
{
    int t = tgt_index(target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? GL_TEXTURE_CUBE_MAP : target);
    if (t < 0 || s.active < 0 || s.active >= 32) { g_opaque = 1; return; }
    GLuint tex = tex_bound(s.active, t);
    GLuint draw_fb = s.draw_fb;
    g_cur = NULL;
    g_pass = grow(g_pass, &g_cappass, g_npass + 1, sizeof *g_pass);
    pass_t *p = &g_pass[g_npass++];
    memset(p, 0, sizeof *p);
    p->fb = FB_UPLOAD; p->k_first = p->k_last = k; p->r0 = (uint32_t)g_nrw; p->nother = 1;
    if (tex) add_rw(p, rkey(RK_TEX, tex, 0, 0), 1);
    s.draw_fb = draw_fb;
}

static void copy_tex(size_t k, GLenum target)
{
    pass_t *p = work(k);
    p->nother++;
    fbi_t *rf = fbi(s.read_fb);
    if (!rf || rf->bad) { g_opaque = 1; return; }
    for (int i = 0; i < 4; i++) if (rf->att[i]) add_rw(p, rf->att[i], 0);
    int t = tgt_index(target == GL_TEXTURE_CUBE_MAP_POSITIVE_X || (target > GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
                      ? GL_TEXTURE_CUBE_MAP : target);
    if (t < 0 || s.active < 0 || s.active >= 32) { g_opaque = 1; return; }
    GLuint dst = tex_bound(s.active, t);
    if (dst) add_rw(p, rkey(RK_TEX, dst, 0, 0), 1);
}

#define A(i) (a[i])
#define I(i) ((GLint)(int64_t)a[i])
#define U(i) ((GLuint)a[i])
#define F(i) (fbits(a[i]))
static float fbits(uint64_t v) { uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }

static void walk_cmd(size_t k, unsigned op, const uint64_t *a, const fg_frame *fr)
{
    switch (op) {
    case OP_glBindFramebuffer:
        if (U(0) == GL_FRAMEBUFFER || U(0) == GL_DRAW_FRAMEBUFFER) { if (U(1) != s.draw_fb) g_cur = NULL; s.draw_fb = U(1); }
        if (U(0) == GL_FRAMEBUFFER || U(0) == GL_READ_FRAMEBUFFER) s.read_fb = U(1);
        break;
    case OP_glFramebufferTexture2D: case OP_glFramebufferTexture2DMultisampleEXT: case OP_glFramebufferTexture2DMultisampleIMG: {
        GLenum tt = U(2);
        unsigned layer = tt >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && tt <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? tt - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0;
        attach(U(0), U(1), U(3) ? rkey(RK_TEX, U(3), (unsigned)I(4), layer) : 0, 0);
        break;
    }
    case OP_glFramebufferTextureLayer: attach(U(0), U(1), U(2) ? rkey(RK_TEX, U(2), (unsigned)I(3), (unsigned)I(4)) : 0, 0); break;
    case OP_glFramebufferTexture: case OP_glFramebufferTextureEXT: attach(U(0), U(1), U(2) ? rkey(RK_TEX, U(2), (unsigned)I(3), 0xffff) : 0, 0); break;
    case OP_glFramebufferRenderbuffer: attach(U(0), U(1), U(3) ? rkey(RK_RB, U(3), 0, 0) : 0, 0); break;
    case OP_glFramebufferTextureMultiviewOVR: case OP_glFramebufferTextureMultisampleMultiviewOVR: attach(U(0), U(1), 0, 1); break;
    case OP_glDrawBuffers: { fbi_t *f = fbi(s.draw_fb); if (f) f->ver++; g_cur = NULL; break; }
    case OP_glDeleteTextures: case OP_glDeleteRenderbuffers: case OP_glDeleteFramebuffers:
        g_opaque = 1; g_fbi_reset = 1;
        break;
    case OP_glReadBuffer: { fbi_t *f = fbi(s.read_fb); if (f) f->ver++; break; }
    case OP_glActiveTexture: s.active = (int)(U(0) - GL_TEXTURE0); break;
    case OP_glBindTexture: {
        int t = tgt_index(U(0));
        if (t >= 0 && s.active >= 0 && s.active < 32) { s.tex[s.active][t] = U(1); s.tex_known[s.active] |= (unsigned char)(1u << t); }
        break;
    }
    case OP_glUseProgram: s.prog = U(0); break;
    case OP_glUniform1i: { GLint v = I(1); prog_uniform(s.prog, I(0), 1, &v); break; }
    case OP_glUniform1iv: prog_uniform(s.prog, I(0), I(1), (const GLint *)fr->ptr(a[2])); break;
    case OP_glProgramUniform1i: { GLint v = I(2); prog_uniform(U(0), I(1), 1, &v); break; }
    case OP_glProgramUniform1iv: prog_uniform(U(0), I(1), I(2), (const GLint *)fr->ptr(a[3])); break;
    case OP_glEnable: case OP_glDisable: {
        GLboolean on = op == OP_glEnable;
        switch (U(0)) {
        case GL_DEPTH_TEST: s.depth_test = on; break;
        case GL_STENCIL_TEST: s.stencil_test = on; break;
        case GL_SCISSOR_TEST: s.cs.scissor = on; s.csver++; break;
        case GL_RASTERIZER_DISCARD: s.cs.discard = on; s.csver++; break;
        }
        break;
    }
    case OP_glDepthFunc: s.depth_func = U(0); break;
    case OP_glDepthMask: s.cs.dmask = (GLboolean)U(0); s.csver++; break;
    case OP_glStencilFunc: s.sfunc[0] = s.sfunc[1] = U(0); break;
    case OP_glStencilFuncSeparate:
        if (U(0) != GL_BACK) s.sfunc[0] = U(1);
        if (U(0) != GL_FRONT) s.sfunc[1] = U(1);
        break;
    case OP_glStencilOp: for (int f = 0; f < 2; f++) for (int o = 0; o < 3; o++) s.sop[f][o] = U(o); break;
    case OP_glStencilOpSeparate:
        for (int o = 0; o < 3; o++) { if (U(0) != GL_BACK) s.sop[0][o] = U(o + 1); if (U(0) != GL_FRONT) s.sop[1][o] = U(o + 1); }
        break;
    case OP_glStencilMask: s.cs.smask[0] = s.cs.smask[1] = U(0); s.csver++; break;
    case OP_glStencilMaskSeparate:
        if (U(0) != GL_BACK) s.cs.smask[0] = U(1);
        if (U(0) != GL_FRONT) s.cs.smask[1] = U(1);
        s.csver++;
        break;
    case OP_glColorMask: for (int i = 0; i < 4; i++) s.cs.cmask[i] = (GLboolean)U(i); s.csver++; break;
    case OP_glColorMaski: case OP_glColorMaskiEXT: s.cmaski = 1; s.csver++; break;
    case OP_glScissor: for (int i = 0; i < 4; i++) s.cs.box[i] = I(i); s.csver++; break;
    case OP_glClearColor: for (int i = 0; i < 4; i++) s.cs.ccol[i] = F(i); s.csver++; break;
    case OP_glClearDepthf: s.cs.cdepth = F(0); s.csver++; break;
    case OP_glClearStencil: s.cs.cstencil = I(0); s.csver++; break;
    case OP_glBindBufferBase: case OP_glBindBufferRange:
        if (U(0) == GL_SHADER_STORAGE_BUFFER || U(0) == GL_ATOMIC_COUNTER_BUFFER) g_side = 1;
        break;
    case OP_glClear: {
        pass_t *p = work(k);
        p->nclears++;
        GLbitfield m = U(0);
        clear_slots(p, (m & GL_COLOR_BUFFER_BIT ? 0xfu : 0) | (m & GL_DEPTH_BUFFER_BIT ? 1u << S_DEPTH : 0) |
                       (m & GL_STENCIL_BUFFER_BIT ? 1u << S_STENCIL : 0));
        break;
    }
    case OP_glClearBufferfv: case OP_glClearBufferiv: case OP_glClearBufferuiv: case OP_glClearBufferfi: {
        pass_t *p = work(k);
        p->nclears++;
        GLenum b = U(0);
        GLint d = I(1);
        unsigned sl = b == GL_COLOR ? (d >= 0 && d < 4 ? 1u << d : 0) : b == GL_DEPTH ? 1u << S_DEPTH :
                      b == GL_STENCIL ? 1u << S_STENCIL : b == GL_DEPTH_STENCIL ? (1u << S_DEPTH | 1u << S_STENCIL) : 0;
        if (b == GL_COLOR) { fbi_t *f = p->f; if (f && f->ver) { wrote(p, 0xf); break; } }   // (draw buffers may map it elsewhere)
        clear_slots(p, sl);
        break;
    }
    case OP_glInvalidateFramebuffer: case OP_glDiscardFramebufferEXT:
        invalidate(k, U(0), I(1), (const GLenum *)fr->ptr(a[2]));
        break;
    case OP_glInvalidateSubFramebuffer: { pass_t *p = work(k); p->nother++; wrote(p, present(p)); break; }
    case OP_glBlitFramebuffer: blit(k, U(8)); break;
    case OP_glCopyTexSubImage2D: copy_tex(k, U(0)); break;
    case OP_glCopyTexSubImage3D: copy_tex(k, U(0)); break;
    case OP_glTexSubImage2D: case OP_glTexSubImage3D: case OP_glCompressedTexSubImage2D: case OP_glCompressedTexSubImage3D:
        upload(k, U(0));
        break;
    default:
        if (is_draw(op)) draw(k);
        break;
    }
}

// is the resource dead after pass i: overwritten whole before anything reads it?
static int dead_after(size_t i, uint64_t key, int at_swap)
{
    for (size_t j = i + 1; j < g_npass; j++) {
        pass_t *q = &g_pass[j];
        if (q->dead) continue;
        for (uint32_t r = 0; r < q->nr; r++) {
            uint64_t k = g_rw[q->r0 + r];
            if (!(k >> 55 & 1) && robj(k & ~(1ull << 55)) == robj(key)) return 0;
        }
        int touched = 0, full = 1, through = 1;
        for (int sl = 0; sl < NSLOT; sl++) {
            if (q->att[sl] != key) continue;
            touched = 1;
            unsigned b = 1u << sl;
            if (!(q->full & b)) full = 0;
            if (q->full & b) continue;
            if (sl < 4) return 0;                    // a colour buffer loaded: read
            if (q->tested & b) return 0;             // depth/stencil tested
            if (!(q->inval_end & b)) through = 0;
        }
        if (!touched) continue;
        if (full) return 1;
        if (through == 1) return 1;                  // loaded untested, invalidated at the end
    }
    return at_swap && (key >> 56 == RK_WIN_D || key >> 56 == RK_WIN_S);
}

// does the pass itself read this image (a blit or copy from its own framebuffer, a texture it draws into)?
static int reads_own(const pass_t *p, uint64_t key)
{
    for (uint32_t r = 0; r < p->nr; r++) {
        uint64_t k = g_rw[p->r0 + r];
        if (!(k >> 55 & 1) && robj(k) == robj(key)) return 1;
    }
    return 0;
}

static unsigned long st_windows, st_opaque, st_passes, st_dead, st_end, st_end_ds, st_start, st_sunk;

static void add_insert(size_t k, int after, int kind, unsigned slots, size_t pass)
{
    g_ins = grow(g_ins, &g_capins, g_nins + 1, sizeof *g_ins);
    g_ins[g_nins++] = (insert_t){ k, after, kind, slots, pass };
}
static int ins_cmp(const void *x, const void *y)
{
    const insert_t *a = x, *b = y;
    if (a->k != b->k) return a->k < b->k ? -1 : 1;
    if (a->after != b->after) return a->after - b->after;
    return a->kind < b->kind ? 1 : a->kind > b->kind ? -1 : 0;   // (sunk clears before invalidates of the same point)
}

static void analyse(const fg_frame *fr)
{
    size_t nk = 0;
    for (size_t off = 0; off < fr->ncmd; nk++) off += (size_t)(fr->cmd[off] >> 32) + 1;
    g_koff = grow(g_koff, &g_capk, nk + 1, sizeof *g_koff);
    g_skip = grow(g_skip, &g_capskip, nk + 1, 1);
    memset(g_skip, 0, nk);
    g_npass = 0; g_nrw = 0; g_nins = 0; g_cur = NULL; g_opaque = 0;
    state_from_driver();
    g_start_active = s.active;
    size_t k = 0;
    for (size_t off = 0; off < fr->ncmd; k++) {
        uint64_t h = fr->cmd[off];
        unsigned op = (unsigned)(h & 0xffffffffu);
        g_koff[k] = off;
        walk_cmd(k, op, fr->cmd + off + 1, fr);
        off += (size_t)(h >> 32) + 1;
    }
    g_koff[k] = fr->ncmd;
    st_windows++; st_passes += g_npass;
    if (g_opaque) { st_opaque++; return; }
    int side = g_side || g_queries || g_tf;
    // dead passes, until nothing more dies (a pass whose only reader died dies too)
    for (int changed = 1; changed && o_dead && !side;) {
        changed = 0;
        for (size_t i = 0; i < g_npass; i++) {
            pass_t *p = &g_pass[i];
            if (p->dead || p->fb == 0 || p->fb == FB_UPLOAD || !(p->ndraws || p->nclears || p->nother)) continue;
            unsigned outs = (p->written | p->full) & present(p);
            int ok = outs || p->nw;
            for (int sl = 0; ok && sl < NSLOT; sl++) if ((outs >> sl & 1) && !dead_after(i, p->att[sl], fr->at_swap)) ok = 0;
            for (uint32_t r = 0; ok && r < p->nr; r++) {
                uint64_t w = g_rw[p->r0 + r];
                if ((w >> 55 & 1) && !dead_after(i, w & ~(1ull << 55), fr->at_swap)) ok = 0;
            }
            if (ok) { p->dead = 1; changed = 1; st_dead++; }
        }
    }
    for (size_t i = 0; i < g_npass; i++) {
        pass_t *p = &g_pass[i];
        if (!p->dead) continue;
        for (size_t kk = p->k_first; kk <= p->k_last; kk++) {
            unsigned op = (unsigned)(fr->cmd[g_koff[kk]] & 0xffffffffu);
            switch (op) {
            case OP_glClear: case OP_glClearBufferfv: case OP_glClearBufferiv: case OP_glClearBufferuiv: case OP_glClearBufferfi:
            case OP_glInvalidateFramebuffer: case OP_glDiscardFramebufferEXT: case OP_glInvalidateSubFramebuffer:
            case OP_glBlitFramebuffer: case OP_glCopyTexSubImage2D: case OP_glCopyTexSubImage3D:
                g_skip[kk] = 1; break;
            default:
                if (is_draw(op)) g_skip[kk] = 1;
            }
        }
    }
    // clear-only passes made at the start of the next pass on their framebuffer
    for (size_t i = 0; o_sink && i < g_npass; i++) {
        pass_t *p = &g_pass[i];
        if (p->dead || p->ndraws || p->nother || !p->nclears || p->csver_first != p->csver_last || s.cmaski) continue;
        size_t j;
        for (j = i + 1; j < g_npass; j++) {
            pass_t *q = &g_pass[j];
            if (q->dead) continue;
            int touch = 0;
            for (int a = 0; a < NSLOT && !touch; a++) {
                if (!p->att[a]) continue;
                for (int b = 0; b < NSLOT; b++) if (q->att[b] && robj(q->att[b]) == robj(p->att[a])) touch = 1;
                for (uint32_t r = 0; r < q->nr; r++) if (robj(g_rw[q->r0 + r] & ~(1ull << 55)) == robj(p->att[a])) touch = 1;
            }
            if (touch) break;
        }
        if (j >= g_npass) continue;
        pass_t *q = &g_pass[j];
        // (not into another pass that only clears: that one may be sunk itself)
        if (q->fb != p->fb || q->f != p->f || q->ver != p->ver || !(q->ndraws || q->nother)) continue;
        for (size_t kk = p->k_first; kk <= p->k_last; kk++) {
            unsigned op = (unsigned)(fr->cmd[g_koff[kk]] & 0xffffffffu);
            if (op == OP_glClear || op == OP_glClearBufferfv || op == OP_glClearBufferiv || op == OP_glClearBufferuiv || op == OP_glClearBufferfi)
                g_skip[kk] = 1;
        }
        add_insert(q->k_first, 0, 1, 0, i);
        p->sunk = 1; st_sunk++;
    }
    // invalidates at a pass's end and start
    for (size_t i = 0; i < g_npass; i++) {
        pass_t *p = &g_pass[i];
        if (p->dead || p->sunk) continue;
        unsigned end = 0, start = 0;
        for (int sl = 0; sl < NSLOT; sl++) {
            uint64_t key = p->att[sl];
            unsigned b = 1u << sl;
            if (!key || key >> 56 == RK_WIN_C || (p->inval_end & b)) continue;
            if (sl < S_DEPTH && !o_end) continue;
            int d = dead_after(i, key, fr->at_swap);
            if (!d) continue;
            if (o_end) end |= b;
            if (sl >= S_DEPTH && o_start && !(p->full & b) && !(p->tested & b) && !reads_own(p, key)) start |= b;
        }
        // (a packed depth/stencil buffer is one image: both its slots or neither)
        if (p->att[S_DEPTH] && p->att[S_DEPTH] == p->att[S_STENCIL]) {
            unsigned ds = 1u << S_DEPTH | 1u << S_STENCIL;
            if ((end & ds) != ds) end &= ~ds;
            if ((start & ds) != ds) start &= ~ds;
        }
        if (end) { add_insert(p->k_last, 1, 0, end, i); st_end++; if (end & (1u << S_DEPTH | 1u << S_STENCIL)) st_end_ds++; }
        if (start) { add_insert(p->k_first, 0, 0, start, i); st_start++; }
    }
    if (g_nins > 1) qsort(g_ins, g_nins, sizeof *g_ins, ins_cmp);
}

// ---- submitting the stream with the changes
#define NXT(f) ((T_##f)fr->next[OP_##f])
static void emit_inval(const fg_frame *fr, const pass_t *p, unsigned slots)
{
    GLenum a[NSLOT];
    GLsizei n = 0;
    for (int sl = 0; sl < NSLOT; sl++) {
        if (!(slots >> sl & 1)) continue;
        if (p->fb == 0) a[n++] = sl < 4 ? GL_COLOR : sl == S_DEPTH ? GL_DEPTH : GL_STENCIL;
        else if (sl < 4) a[n++] = GL_COLOR_ATTACHMENT0 + (GLenum)sl;
        else if (p->att[S_DEPTH] == p->att[S_STENCIL]) { if (sl == S_DEPTH) a[n++] = GL_DEPTH_STENCIL_ATTACHMENT; }
        else a[n++] = sl == S_DEPTH ? GL_DEPTH_ATTACHMENT : GL_STENCIL_ATTACHMENT;
    }
    if (n) NXT(glInvalidateFramebuffer)(GL_DRAW_FRAMEBUFFER, n, a);
}
static void set_clearstate(const fg_frame *fr, const clearstate *c)
{
    if (c->scissor) NXT(glEnable)(GL_SCISSOR_TEST); else NXT(glDisable)(GL_SCISSOR_TEST);
    if (c->discard) NXT(glEnable)(GL_RASTERIZER_DISCARD); else NXT(glDisable)(GL_RASTERIZER_DISCARD);
    NXT(glScissor)(c->box[0], c->box[1], c->box[2], c->box[3]);
    NXT(glColorMask)(c->cmask[0], c->cmask[1], c->cmask[2], c->cmask[3]);
    NXT(glDepthMask)(c->dmask);
    NXT(glStencilMaskSeparate)(GL_FRONT, c->smask[0]);
    NXT(glStencilMaskSeparate)(GL_BACK, c->smask[1]);
    NXT(glClearColor)(c->ccol[0], c->ccol[1], c->ccol[2], c->ccol[3]);
    NXT(glClearDepthf)(c->cdepth);
    NXT(glClearStencil)(c->cstencil);
}
static void emit_sunk(const fg_frame *fr, const pass_t *p, const pass_t *q)
{
    set_clearstate(fr, &p->cs_first);
    for (size_t kk = p->k_first; kk <= p->k_last; kk++) {
        const uint64_t *c = fr->cmd + g_koff[kk];
        unsigned op = (unsigned)(c[0] & 0xffffffffu);
        if (op == OP_glClear || op == OP_glClearBufferfv || op == OP_glClearBufferiv || op == OP_glClearBufferuiv || op == OP_glClearBufferfi)
            fr->call(op, c + 1);
    }
    set_clearstate(fr, &q->cs_first);
}

static void replay(const fg_frame *fr)
{
    size_t ii = 0, k = 0;
    for (size_t off = 0; off < fr->ncmd; k++) {
        const uint64_t *c = fr->cmd + off;
        for (; ii < g_nins && g_ins[ii].k == k && !g_ins[ii].after; ii++) {
            insert_t *in = &g_ins[ii];
            if (in->kind == 0) emit_inval(fr, &g_pass[in->pass], in->slots);
            else {
                // (the pass whose start this is)
                size_t j;
                for (j = 0; j < g_npass && g_pass[j].k_first != k; j++) ;
                if (j < g_npass) emit_sunk(fr, &g_pass[in->pass], &g_pass[j]);
            }
        }
        if (!g_skip[k]) fr->call((unsigned)(c[0] & 0xffffffffu), c + 1);
        for (; ii < g_nins && g_ins[ii].k == k && g_ins[ii].after; ii++)
            if (g_ins[ii].kind == 0) emit_inval(fr, &g_pass[g_ins[ii].pass], g_ins[ii].slots);
        off += (size_t)(c[0] >> 32) + 1;
    }
}

static unsigned long g_logged;
static void verbose(const fg_frame *fr)
{
    if (g_logged >= (unsigned long)g_verbose) return;
    g_logged++;
    fprintf(stderr, "[glrec] held frame: %zu passes%s%s\n", g_npass, g_opaque ? ", not followed: submitted unchanged" : "",
            fr->at_swap ? "" : " (submitted before the swap)");
    for (size_t i = 0; i < g_npass; i++) {
        pass_t *p = &g_pass[i];
        unsigned end = 0, start = 0, sink = 0;
        for (size_t n = 0; n < g_nins; n++) {
            if (g_ins[n].pass != i) continue;
            if (g_ins[n].kind == 1) sink = 1; else if (g_ins[n].after) end |= g_ins[n].slots; else start |= g_ins[n].slots;
        }
        fprintf(stderr, "[glrec]   %zu fb%u draws %d clears %d other %d full %02x written %02x tested %02x inval %02x%s%s%s%s%s\n",
                i, p->fb, p->ndraws, p->nclears, p->nother, p->full, p->written, p->tested, p->inval_end,
                p->dead ? " DEAD" : "", p->sunk ? " SUNK" : "", sink ? " +clears" : "",
                end ? " end-inval" : "", start ? " start-inval" : "");
    }
}

void fg_submit(fg_frame *fr)
{
    double t0 = now();
    analyse(fr);
    if (g_verbose) verbose(fr);
    fr->analysis_s = now() - t0;
    if (g_opaque) { g_nins = 0; memset(g_skip, 0, g_capskip); }
    replay(fr);
    if (g_fbi_reset) { g_nfbi = 0; g_fbi_reset = 0; }
}

void fg_flush_call(const char *n)
{
    if (!strcmp(n, "glLinkProgram") || !strcmp(n, "glProgramBinary") || !strcmp(n, "glProgramBinaryOES") ||
        !strcmp(n, "glDeleteProgram") || !strcmp(n, "glUseProgramStages")) { memset(g_prog, 0, sizeof g_prog); g_nprog = 0; }
    else if (!strcmp(n, "glBindImageTexture")) g_side = 1;
    else if (!strcmp(n, "glBeginQuery") || !strcmp(n, "glBeginQueryEXT")) g_queries++;
    else if (!strcmp(n, "glEndQuery") || !strcmp(n, "glEndQueryEXT")) { if (g_queries > 0) g_queries--; }
    else if (!strcmp(n, "glBeginTransformFeedback") || !strcmp(n, "glResumeTransformFeedback")) g_tf = 1;
    else if (!strcmp(n, "glEndTransformFeedback") || !strcmp(n, "glPauseTransformFeedback")) g_tf = 0;
    else if (!strcmp(n, "eglMakeCurrent")) { g_win_known = 0; g_nfbi = 0; }
}

void fg_swap(void) {}

void fg_log(unsigned long frames, double fg_s)
{
    double f = frames ? (double)frames : 1;
    fprintf(stderr, "[glrec]   frame graph: %.2f held frames a frame (%.2f not followed), %.1f passes; a frame: %.2f dead passes left out, "
            "%.2f end invalidates (%.2f depth/stencil), %.2f start invalidates, %.2f clear passes sunk; graph %.3f ms a frame\n",
            st_windows / f, st_opaque / f, st_passes / f, st_dead / f, st_end / f, st_end_ds / f, st_start / f, st_sunk / f, fg_s * 1000 / f);
    st_windows = st_opaque = st_passes = st_dead = st_end = st_end_ds = st_start = st_sunk = 0;
}

int fg_init(void)
{
    void *h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) return -1;
#define D(v, n) *(void **)&v = dlsym(h, n)
    D(d_GetIntegerv, "glGetIntegerv"); D(d_GetFloatv, "glGetFloatv"); D(d_GetBooleanv, "glGetBooleanv"); D(d_IsEnabled, "glIsEnabled");
    D(d_BindFramebuffer, "glBindFramebuffer"); D(d_GetFbAttachment, "glGetFramebufferAttachmentParameteriv");
    D(d_ActiveTexture, "glActiveTexture"); D(d_GetProgramiv, "glGetProgramiv"); D(d_GetActiveUniform, "glGetActiveUniform");
    D(d_GetUniformLocation, "glGetUniformLocation"); D(d_GetUniformiv, "glGetUniformiv");
#undef D
    if (!d_GetIntegerv || !d_GetFloatv || !d_GetBooleanv || !d_IsEnabled || !d_BindFramebuffer || !d_GetFbAttachment ||
        !d_ActiveTexture || !d_GetProgramiv || !d_GetActiveUniform || !d_GetUniformLocation || !d_GetUniformiv) return -1;
    const char *o = getenv("UA_GL_REC_OPT");
    if (o && *o) {
        o_dead = strstr(o, "dead") != NULL; o_end = strstr(o, "end") != NULL;
        o_start = strstr(o, "start") != NULL; o_sink = strstr(o, "sink") != NULL;
    }
    const char *v = getenv("UA_GL_REC_VERBOSE");
    g_verbose = v ? atoi(v) : 0;
    // (the extension list needs a context: read at the first held frame)
    return 0;
}

// the depth/stencil fetch extension: read once a context is current (from glrec.c's first frame)
void fg_context(void)
{
    check_ext();
    if (g_ds_fetch < 0) g_ds_fetch = 1;                  // (no extension list: assume shaders can)
    fprintf(stderr, "[glrec] frame graph: %s%s%s%s%s\n", o_dead ? "dead passes, " : "", o_end ? "end invalidates, " : "",
            o_start ? "start invalidates, " : "", o_sink ? "clear passes sunk" : "",
            g_ds_fetch ? "; shaders can read depth/stencil here: programs not seen compiled from source count as reading it" : "");
}
