// Draws merged (UA_GL_MERGE=1, with UA_GL_REC recording; off by default): neighbouring draws of one
// program that differ only in their geometry, one texture and the renderer's own uniforms are made as
// one draw.
//
// A 2D game's sprites come in depth order, one draw each where Unity cannot batch them: Hollow
// Knight's Greenpath alternates between two atlases and two programs (lit and unlit), and a sprite that
// is not batched carries its own transform. On Mali's driver each draw costs the thread that makes it
// 30 - 70 us whatever it draws (docs/profiling.md, "The render thread's draws"); a held frame
// (glrec.c) shows which neighbours could be one.
//
// The shaders. The game's programs stay as the game compiles them. Each gets a twin, linked from
// rewritten sources (GLSL ES 3.00 and later), that only merged draws use:
//   vertex   an attribute `ua_v` at location 15: 1 + the draw's index in the batch + 16 x its
//            texture. Each use of a renderer uniform (UA_GL_MERGE_UNIFORMS: the object's matrices,
//            its sprite colour and flip) becomes `(ua_m ? ua_pd_<name>[draw] : <name>)`: the value
//            itself, selected, never computed; the batch's values are in the arrays. A flat int
//            `ua_sel` carries the texture to the fragment shader.
//   fragment with a `uniform sampler2D _MainTex` used only as `texture(_MainTex, uv)`: three more
//            samplers (`layout(binding = 31, 30, 29)`) and `texture(_MainTex, uv)` read through a
//            function that samples the one ua_sel names. ua_sel is the same for the three vertices of
//            each triangle and so for every fragment of a quad: implicit derivatives as before.
// The game's draws never see the twins (a program the twin does not link for is not merged); before a
// merged draw the twin gets the game's uniforms the game changed since the last one (the replay
// follows the game's uniform calls and reads them back from the game's program).
//
// The draws. In each held frame a scan follows the state its calls set (and asks the driver what the
// frame did not set: the state at its start) and finds runs of draws with nothing between them but
// geometry (buffer binds, maps and writes, vertex attribute pointers), texture binds and uniforms;
// each draw of a run: indexed triangles of one program, its vertices in one buffer that the frame
// wrote, the same layout, the same textures but _MainTex (four at most in a run), the same values of
// every uniform but the renderer ones, at most 8 draws and 65,535 vertices. The run's draws are made
// as one at its last draw's place: their vertices copied byte for byte into a buffer of the host's,
// their indices in the same order (each triangle drawn in its draw's order, so blending, depth and
// stencil see what they saw), then the state the last draw left set back. Nothing else of the frame
// changes.
//
//   UA_GL_MERGE=1              on (needs UA_GL_REC=1, thread or auto recording; off by default)
//   UA_GL_MERGE_MAX=<n>        draws in a run, 2 - 8 (default 8)
//   UA_GL_MERGE_RUNS=0         a measurement: the twins made, the frames replayed as they are (no scan)
//   UA_GL_MERGE_REWRITE=vs|fs  a measurement: only these shaders rewritten (default both)
//   UA_GL_MERGE_UNIFORMS=<names>  the renderer uniforms (default below)
//   UA_GL_MERGE_VERBOSE=<n>[@<m>]  <n> frames' runs and what ended each (after the first <m>), logged
//   UA_GL_MERGE=check          as 1, and in every UA_GL_MERGE_CHECK_EVERY'th (30) frame each run that writes no
//                              depth or stencil is made twice from the same picture: merged, and as the game's
//                              own draws with the game's program; the two compared byte for byte
//                              ("[glmerge] check: ..."), the game's kept
// Unity's program binary cache is off while this is on (glrec.c): a program it loads from its cache has no
// sources here, so no twin.
#define _GNU_SOURCE
#include <ctype.h>
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#define GLREC_PART 1
#include "glrec_gen.h"
#include "glrec_fg.h"
#include "glmerge.h"

#define SEL_LOC 15                  // ua_v's attribute location
#define MAXRUN 8                    // (each renderer uniform's array has MAXRUN entries: kept small, the driver copies them)
#define NTEX 4                      // _MainTex and three more
static const int g_xunit[NTEX] = { -1, 31, 30, 29 };

static int g_on, g_max = MAXRUN, g_verbose, g_verbose_skip, g_check, g_check_every = 30, g_sample_all, g_runs_off, g_rw_vs = 1, g_rw_fs = 1;
static const char *g_names_default =
    "hlslcc_mtx4x4unity_ObjectToWorld,hlslcc_mtx4x4unity_WorldToObject,_RendererColor,_Flip";
static char g_names[1024];
static void *const *g_next;         // the next layer's entry points (glrec.c's table)
#define N(f) ((T_##f)g_next[OP_##f])

// statistics since the last log line
static unsigned long g_frames, g_draws, g_merged_draws, g_runs, g_end[16];
static unsigned long g_rewritten_vs, g_rewritten_fs, g_skipped_vs, g_skipped_fs, g_relinks, g_progs_ok;
static double g_scan_s;
static unsigned long g_q_uni, g_q_tex, g_q_attr;   // driver questions the scan asked
static const char *const g_end_name[16] = { "another call", "a draw that cannot be merged", "another program", "the layout",
    "a texture", "a uniform", "the run is full", "the vertices", "a state call", "the frame's end", "a texture's parameters",
    "a write the scan cannot follow", "", "", "", "" };
enum { E_CALL, E_DRAW, E_PROG, E_LAYOUT, E_TEX, E_UNI, E_FULL, E_VERTS, E_STATE, E_END, E_TEXPARAM, E_WRITE };

int glmerge_on(void) { return g_on && g_next; }

int glmerge_init(void)
{
    const char *e = getenv("UA_GL_MERGE");
    g_on = e && *e && strcmp(e, "0");
    if (!g_on) return 0;
    g_check = !strcmp(e, "check");
    g_sample_all = (e = getenv("UA_GL_MERGE_SAMPLE")) && !strcmp(e, "all");
    g_runs_off = (e = getenv("UA_GL_MERGE_RUNS")) && !strcmp(e, "0");
    if ((e = getenv("UA_GL_MERGE_REWRITE")) && *e) { g_rw_vs = strstr(e, "vs") != NULL; g_rw_fs = strstr(e, "fs") != NULL; }
    if ((e = getenv("UA_GL_MERGE_CHECK_EVERY")) && atoi(e) > 0) g_check_every = atoi(e);
    if ((e = getenv("UA_GL_MERGE_MAX")) && atoi(e) >= 2 && atoi(e) <= MAXRUN) g_max = atoi(e);
    if ((e = getenv("UA_GL_MERGE_VERBOSE"))) { g_verbose = atoi(e); const char *at = strchr(e, '@'); if (at) g_verbose_skip = atoi(at + 1); }
    snprintf(g_names, sizeof g_names, "%s", (e = getenv("UA_GL_MERGE_UNIFORMS")) && *e ? e : g_names_default);
    return 1;
}
void glmerge_set_next(void *const *next) { g_next = next; }

// ======================================================================= the sources
typedef struct { char *p; size_t n, cap; } sbuf;
static void sb_put(sbuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n + 1) c *= 2;
        char *q = realloc(b->p, c);
        if (!q) { free(b->p); b->p = NULL; b->cap = 0; b->n = 0; return; }
        b->p = q; b->cap = c;
    }
    if (!b->p) return;
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void sb_str(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }
static void sb_fmt(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void sb_fmt(sbuf *b, const char *fmt, ...)
{
    char t[512]; va_list ap; va_start(ap, fmt); int n = vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    if (n > 0) sb_put(b, t, (size_t)n < sizeof t ? (size_t)n : sizeof t - 1);
}
static int idch(int c) { return isalnum(c) || c == '_'; }
// the next whole-word occurrence of w at or after p (outside comments: Unity's sources have none in code)
static const char *word(const char *p, const char *w)
{
    size_t n = strlen(w);
    for (const char *q = strstr(p, w); q; q = strstr(q + 1, w))
        if ((q == p || !idch((unsigned char)q[-1])) && !idch((unsigned char)q[n])) return q;
    return NULL;
}
// "#version 3x0 es": its number, else 0
static int es_version(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    int v = 0;
    if (sscanf(s, "#version %d es", &v) == 1 && v >= 300) return v;
    return 0;
}
// the position of main's body ("void main(" ... "{"), and of "void main" itself
static const char *main_at(const char *s, const char **body)
{
    const char *m = word(s, "main");
    while (m) {
        const char *b = m - 1;
        while (b > s && (*b == ' ' || *b == '\t')) b--;
        if (b - 3 >= s && !strncmp(b - 3, "void", 4) && (b - 4 < s || !idch((unsigned char)b[-4]))) {
            const char *q = m + 4;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '(') {
                const char *c = strchr(q, ')');
                const char *o = c ? strchr(c, '{') : NULL;
                if (o) { *body = o + 1; return b - 3; }
            }
        }
        m = word(m + 1, "main");
    }
    return NULL;
}
// a global `uniform <type> <name>[n];` declaration (type float/vec2/vec3/vec4; n 0 for none): its line
typedef struct { const char *at, *end; int comps, n; char prec[32]; } udecl;
static int find_udecl(const char *s, const char *name, udecl *d)
{
    for (const char *q = word(s, name); q; q = word(q + 1, name)) {
        const char *ls = q;
        while (ls > s && ls[-1] != '\n') ls--;
        const char *le = strchr(q, ';');
        if (!le) return 0;
        const char *nl = strchr(ls, '\n');
        if (nl && nl < le) continue;                       // (one declaration a line, as hlslcc writes them)
        char line[256];
        size_t len = (size_t)(le - ls);
        if (len >= sizeof line) continue;
        memcpy(line, ls, len); line[len] = 0;
        char t1[32], t2[32], t3[32], nm[128]; int n = 0;
        memset(d, 0, sizeof *d);
        const char *ty;
        int k = sscanf(line, " uniform %31s %31s %127[^ \t[;]", t1, t2, nm);
        if (k == 3 && (!strcmp(t1, "highp") || !strcmp(t1, "mediump") || !strcmp(t1, "lowp")) && !strcmp(nm, name)) {
            snprintf(d->prec, sizeof d->prec, "%s", t1); ty = t2;
        } else if (sscanf(line, " uniform %31s %127[^ \t[;]", t3, nm) == 2 && !strcmp(nm, name)) {
            ty = t3;
        } else continue;
        d->comps = !strcmp(ty, "float") ? 1 : !strcmp(ty, "vec2") ? 2 : !strcmp(ty, "vec3") ? 3 : !strcmp(ty, "vec4") ? 4 : 0;
        if (!d->comps) return 0;
        const char *br = strchr(line, '[');
        if (br) { if (sscanf(br, "[%d]", &n) != 1 || n <= 0 || n > 4 || d->comps != 4) return 0; }
        d->n = n; d->at = ls; d->end = le + 1;
        return 1;
    }
    return 0;
}

// the vertex shader: ua_v, ua_sel, and the renderer uniforms through the batch's arrays
static char *rewrite_vs(const char *src)
{
    int ver = es_version(src);
    if (!ver || strstr(src, "ua_")) return NULL;
    const char *body, *mn = main_at(src, &body);
    if (!mn) return NULL;
    struct { char name[96]; udecl d; } pd[16];
    int npd = 0;
    char names[1024]; snprintf(names, sizeof names, "%s", g_names);
    for (char *t = strtok(names, ", "); t && npd < 16; t = strtok(NULL, ", ")) {
        udecl d;
        if (!find_udecl(src, t, &d) || d.at > mn) continue;
        // every use after its declaration: name[<digit>] for an array, the name alone otherwise
        int ok = 1;
        for (const char *q = word(d.end, t); q && ok; q = word(q + 1, t)) {
            const char *a = q + strlen(t);
            if (d.n) ok = a[0] == '[' && isdigit((unsigned char)a[1]) && a[2] == ']' && a[1] - '0' < d.n;
        }
        if (!ok) continue;
        snprintf(pd[npd].name, sizeof pd[npd].name, "%s", t); pd[npd].d = d; npd++;
    }
    sbuf b = { 0 };
    // declarations before main; uses replaced from the first declaration on
    const char *p = src;
    sb_put(&b, p, (size_t)(mn - p));
    sb_fmt(&b, "layout(location = %d) in highp float ua_v;\nflat out mediump int ua_sel;\nbool ua_m;\nint ua_d;\n", SEL_LOC);
    static const char *const vt[5] = { "", "float", "vec2", "vec3", "vec4" };
    for (int i = 0; i < npd; i++)
        sb_fmt(&b, "uniform %s %s ua_pd_%s[%d];\n", pd[i].d.prec, vt[pd[i].d.comps], pd[i].name, MAXRUN * (pd[i].d.n ? pd[i].d.n : 1));
    sb_put(&b, mn, (size_t)(body - mn));
    sb_str(&b, "\n    { int ua_i = int(ua_v); ua_m = ua_i > 0; ua_d = (ua_i - 1) & 15; ua_sel = ua_m ? (ua_i - 1) >> 4 : 0; }\n");
    // the body with each use replaced
    p = body;
    for (;;) {
        const char *best = NULL; int bi = -1;
        for (int i = 0; i < npd; i++) {
            const char *q = word(p, pd[i].name);
            if (q && (!best || q < best)) { best = q; bi = i; }
        }
        if (!best) { sb_str(&b, p); break; }
        sb_put(&b, p, (size_t)(best - p));
        const char *nm = pd[bi].name;
        size_t nl = strlen(nm);
        if (pd[bi].d.n) {
            int k = best[nl + 1] - '0';
            sb_fmt(&b, "(ua_m ? ua_pd_%s[ua_d * %d + %d] : %s[%d])", nm, pd[bi].d.n, k, nm, k);
            p = best + nl + 3;
        } else {
            sb_fmt(&b, "(ua_m ? ua_pd_%s[ua_d] : %s)", nm, nm);
            p = best + nl;
        }
    }
    // uses before main (in other functions) were left as they are: a program with them is not merged
    for (int i = 0; i < npd; i++) {
        const char *q = word(pd[i].d.end, pd[i].name);
        if (q && q < mn) { free(b.p); return NULL; }
    }
    return b.p;
}

// the fragment shader: _MainTex read through the sampler ua_sel names
static char *rewrite_fs(const char *src)
{
    int ver = es_version(src);
    if (ver < 310 || strstr(src, "ua_")) return NULL;          // (layout(binding) for samplers: 3.10)
    const char *body, *mn = main_at(src, &body);
    if (!mn) return NULL;
    const char *d = word(src, "_MainTex");
    if (!d || d > mn) return NULL;
    const char *ls = d;
    while (ls > src && ls[-1] != '\n') ls--;
    const char *le = strchr(d, ';');
    if (!le) return NULL;
    char line[256]; size_t len = (size_t)(le - ls);
    if (len >= sizeof line) return NULL;
    memcpy(line, ls, len); line[len] = 0;
    if (!strstr(line, "uniform") || !word(line, "sampler2D") || strchr(line, '[')) return NULL;
    // the sampler's precision: its own, the default statement's, else lowp (GLSL ES's default for sampler2D)
    char prec[16] = "lowp";
    if (word(line, "highp")) strcpy(prec, "highp"); else if (word(line, "mediump")) strcpy(prec, "mediump");
    else if (word(line, "lowp")) strcpy(prec, "lowp");
    else {
        for (const char *q = strstr(src, "precision"); q && q < mn; q = strstr(q + 1, "precision")) {
            char p1[16], p2[32];
            if (sscanf(q, "precision %15s %31[^;]", p1, p2) == 2 && !strcmp(p2, "sampler2D")) snprintf(prec, sizeof prec, "%s", p1);
        }
    }
    // every other use: texture(_MainTex, ...)
    int uses = 0;
    for (const char *q = word(le, "_MainTex"); q; q = word(q + 1, "_MainTex")) {
        const char *b = q - 1;
        while (b > src && (*b == ' ' || *b == '\t')) b--;
        if (*b != '(' || b - 7 < src || strncmp(b - 7, "texture", 7) || idch((unsigned char)b[-8])) return NULL;
        const char *a = q + 8;
        while (*a == ' ' || *a == '\t') a++;
        if (*a != ',') return NULL;
        uses++;
    }
    if (!uses) return NULL;
    sbuf o = { 0 };
    sb_put(&o, src, (size_t)(mn - src));
    sb_str(&o, "flat in mediump int ua_sel;\n");
    for (int t = 1; t < NTEX; t++) sb_fmt(&o, "layout(binding = %d) uniform %s sampler2D ua_t%d;\n", g_xunit[t], prec, t);
    if (g_sample_all)
        sb_fmt(&o, "%s vec4 ua_tex(highp vec2 c) { %s vec4 a = texture(_MainTex, c), b = texture(ua_t1, c), d = texture(ua_t2, c), e = texture(ua_t3, c); "
                   "return ua_sel == 1 ? b : ua_sel == 2 ? d : ua_sel == 3 ? e : a; }\n", prec, prec);
    else
        sb_fmt(&o, "%s vec4 ua_tex(highp vec2 c) { if (ua_sel == 1) return texture(ua_t1, c); if (ua_sel == 2) return texture(ua_t2, c); "
                   "if (ua_sel == 3) return texture(ua_t3, c); return texture(_MainTex, c); }\n", prec);
    // main and the rest, each texture(_MainTex, -> ua_tex(
    const char *p = mn;
    for (const char *q = word(p, "_MainTex"); q; q = word(p, "_MainTex")) {
        const char *b = q - 1;
        while (*b == ' ' || *b == '\t') b--;
        const char *a = q + 8;
        while (*a == ' ' || *a == '\t') a++;
        sb_put(&o, p, (size_t)(b - 7 - p));
        sb_str(&o, "ua_tex(");
        p = a + 1;                                           // (after the comma)
    }
    sb_str(&o, p);
    return o.p;
}

// ======================================================================= shaders and programs
#define PD_MAX 8
#define PD_LOCS 256                         // (uniform locations the scan follows by number)
typedef struct {
    GLuint name; unsigned char used, ok, ntex;
    GLuint twin;                           // the program from the rewritten sources, for merged draws only
    GLint loc_main;                        // the game's program's _MainTex
    int nsmp; struct { GLint loc; int unit, tgt; } smp[16];    // its samplers (units: their uniforms' values; target slot)
    int npd; struct { GLint loc[4]; GLint arr; int n, comps, fs; char name[160]; } pd[PD_MAX];   // renderer uniforms: the game's
                                           // program's locations, the twin's array
    GLuint vs, fs;                         // attached (the last of each kind)
    unsigned uver;                         // (the scan: bumped by each change of a uniform that is not a renderer one)
    unsigned char pdmap[PD_LOCS];          // a location -> 1 + (renderer uniform << 3 | element), 0 for any other
    float pdval[PD_MAX][4][4];             // their values: every uniform call of the render thread passes the scan,
    unsigned char pdknown[PD_MAX][4];      // the rest asked once (a link resets them)
    float (*uval)[4]; unsigned char *uknown;   // every other uniform's value by location, the same way
    GLint *tloc; GLenum *utype;            // each location's twin location and type (to copy it there)
    uint32_t dirty[PD_LOCS / 32];          // set by the game since the twin last had them (the replay's view)
} prog_t;
#define NPRG 1024
static prog_t g_prg[NPRG];
static prog_t *prg(GLuint p, int add)
{
    unsigned i = (p * 2654435761u) % NPRG;
    for (unsigned k = 0; k < NPRG; k++, i = (i + 1) % NPRG) {
        if (g_prg[i].used && g_prg[i].name == p) return &g_prg[i];
        if (!g_prg[i].used) {
            if (!add) return NULL;
            g_prg[i] = (prog_t){ .name = p, .used = 1, .loc_main = -1 };
            return &g_prg[i];
        }
    }
    return NULL;
}

// glShaderSource: the game's source compiled as it is; the rewritten one kept for the program's twin
typedef struct { GLuint sh; unsigned char used, vs; char *orig, *rw; } shd_t;
#define NSHD 1024
static shd_t g_shd[NSHD];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static shd_t *shd(GLuint s, int add)
{
    unsigned i = (s * 2654435761u) % NSHD;
    for (unsigned k = 0; k < NSHD; k++, i = (i + 1) % NSHD) {
        if (g_shd[i].used && g_shd[i].sh == s) return &g_shd[i];
        if (!g_shd[i].used) {
            if (!add) return NULL;
            g_shd[i] = (shd_t){ .sh = s, .used = 1 };
            return &g_shd[i];
        }
    }
    return NULL;
}
char *glmerge_source(GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len)
{
    if (!g_on || !str || !g_next) return NULL;
    size_t tot = 0;
    for (GLsizei i = 0; i < count; i++) if (str[i]) tot += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    char *src = malloc(tot + 1), *o = src;
    if (!src) return NULL;
    for (GLsizei i = 0; i < count; i++) {
        if (!str[i]) continue;
        size_t n = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
        memcpy(o, str[i], n); o += n;
    }
    *o = 0;
    GLint type = 0;
    N(glGetShaderiv)(sh, GL_SHADER_TYPE, &type);
    char *r = type == GL_VERTEX_SHADER ? (g_rw_vs ? rewrite_vs(src) : NULL) : type == GL_FRAGMENT_SHADER ? (g_rw_fs ? rewrite_fs(src) : NULL) : NULL;
    pthread_mutex_lock(&g_lock);
    shd_t *s = shd(sh, 1);
    if (s) { free(s->orig); free(s->rw); s->vs = type == GL_VERTEX_SHADER; s->orig = src; s->rw = r; }
    else { free(src); free(r); }
    pthread_mutex_unlock(&g_lock);
    if (type == GL_VERTEX_SHADER) { if (r) g_rewritten_vs++; else g_skipped_vs++; }
    else if (type == GL_FRAGMENT_SHADER) { if (r) g_rewritten_fs++; else g_skipped_fs++; }
    return NULL;
}
void glmerge_attach(GLuint prog, GLuint sh)
{
    if (!g_on) return;
    pthread_mutex_lock(&g_lock);
    shd_t *s = shd(sh, 0);
    prog_t *p = prg(prog, 1);
    if (p && s) { if (s->vs) p->vs = sh; else p->fs = sh; }
    pthread_mutex_unlock(&g_lock);
}

static int tgt_slot(GLenum t);
// a sampler type's texture target slot (0 2D, 1 3D, 2 cube, 3 2D array), -1 for another sampler, -2 for no sampler
static int sampler_slot(GLenum t)
{
    switch (t) {
    case GL_SAMPLER_2D: case GL_SAMPLER_2D_SHADOW: case GL_INT_SAMPLER_2D: case GL_UNSIGNED_INT_SAMPLER_2D: return 0;
    case GL_SAMPLER_3D: case GL_INT_SAMPLER_3D: case GL_UNSIGNED_INT_SAMPLER_3D: return 1;
    case GL_SAMPLER_CUBE: case GL_SAMPLER_CUBE_SHADOW: case GL_INT_SAMPLER_CUBE: case GL_UNSIGNED_INT_SAMPLER_CUBE: return 2;
    case GL_SAMPLER_2D_ARRAY: case GL_SAMPLER_2D_ARRAY_SHADOW: case GL_INT_SAMPLER_2D_ARRAY: case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY: return 3;
    }
    return -1;
}
static int sampler_type(GLenum t)
{
    return t == GL_SAMPLER_2D || t == GL_SAMPLER_3D || t == GL_SAMPLER_CUBE || t == GL_SAMPLER_2D_SHADOW || t == GL_SAMPLER_2D_ARRAY ||
           t == GL_SAMPLER_2D_ARRAY_SHADOW || t == GL_SAMPLER_CUBE_SHADOW || t == GL_INT_SAMPLER_2D || t == GL_INT_SAMPLER_3D ||
           t == GL_INT_SAMPLER_CUBE || t == GL_INT_SAMPLER_2D_ARRAY || t == GL_UNSIGNED_INT_SAMPLER_2D || t == GL_UNSIGNED_INT_SAMPLER_3D ||
           t == GL_UNSIGNED_INT_SAMPLER_CUBE || t == GL_UNSIGNED_INT_SAMPLER_2D_ARRAY || t == GL_SAMPLER_EXTERNAL_OES ||
           t == GL_SAMPLER_2D_MULTISAMPLE || t == GL_SAMPLER_BUFFER;
}
static GLuint compile_one(GLenum type, const char *src)
{
    GLuint sh = N(glCreateShader)(type);
    const GLchar *one[1] = { src };
    N(glShaderSource)(sh, 1, one, NULL);
    N(glCompileShader)(sh);
    GLint ok = 0;
    N(glGetShaderiv)(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { N(glDeleteShader)(sh); return 0; }
    return sh;
}
// the twin: the rewritten vertex shader with the rewritten fragment shader (else the game's), the
// game's attribute locations
static GLuint link_twin(GLuint game, const char *vsrc, const char *fsrc)
{
    GLuint vs = compile_one(GL_VERTEX_SHADER, vsrc), fs = compile_one(GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs) { if (vs) N(glDeleteShader)(vs); if (fs) N(glDeleteShader)(fs); return 0; }
    GLuint r = N(glCreateProgram)();
    N(glAttachShader)(r, vs); N(glAttachShader)(r, fs);
    GLint na = 0;
    N(glGetProgramiv)(game, GL_ACTIVE_ATTRIBUTES, &na);
    for (GLint i = 0; i < na; i++) {
        char name[128]; GLsizei len = 0; GLint size = 0; GLenum type = 0;
        N(glGetActiveAttrib)(game, (GLuint)i, sizeof name, &len, &size, &type, name);
        GLint loc = N(glGetAttribLocation)(game, name);
        if (loc >= 0) N(glBindAttribLocation)(r, (GLuint)loc, name);
    }
    N(glLinkProgram)(r);
    GLint ok = 0;
    N(glGetProgramiv)(r, GL_LINK_STATUS, &ok);
    N(glDetachShader)(r, vs); N(glDetachShader)(r, fs); N(glDeleteShader)(vs); N(glDeleteShader)(fs);
    if (!ok || N(glGetAttribLocation)(r, "ua_v") != SEL_LOC) { N(glDeleteProgram)(r); return 0; }
    return r;
}
// what the scan and the merged draws need: the game's samplers and renderer uniforms, the twin's arrays,
// and where each of the game's uniforms is in the twin
static void introspect(prog_t *p)
{
    GLuint g = p->name, t = p->twin;
    p->ok = 0; p->ntex = 1; p->nsmp = 0; p->npd = 0; p->loc_main = -1;
    memset(p->pdmap, 0, sizeof p->pdmap); memset(p->pdknown, 0, sizeof p->pdknown);
    if (!p->uval) { p->uval = malloc(sizeof *p->uval * PD_LOCS); p->uknown = malloc(PD_LOCS); p->tloc = malloc(sizeof(GLint) * PD_LOCS); p->utype = malloc(sizeof(GLenum) * PD_LOCS); }
    if (!p->uval || !p->uknown || !p->tloc || !p->utype) return;
    memset(p->uknown, 0, PD_LOCS);
    for (int i = 0; i < PD_LOCS; i++) { p->tloc[i] = -1; p->utype[i] = 0; }
    memset(p->dirty, 0xff, sizeof p->dirty);
    if (!t) return;
    GLint nb = 0;
    N(glGetProgramiv)(g, GL_ACTIVE_UNIFORM_BLOCKS, &nb);
    if (nb) return;                                               // (uniform blocks: their bindings are not copied)
    shd_t *fs = p->fs ? shd(p->fs, 0) : NULL;
    // the twin's extra samplers and renderer arrays
    GLint nu = 0;
    N(glGetProgramiv)(t, GL_ACTIVE_UNIFORMS, &nu);
    for (GLint u = 0; u < nu; u++) {
        char name[160]; GLsizei len = 0; GLint size = 0; GLenum type = 0;
        N(glGetActiveUniform)(t, (GLuint)u, sizeof name - 8, &len, &size, &type, name);
        if (len > 3 && !strcmp(name + len - 3, "[0]")) name[len - 3] = 0;
        if (!strncmp(name, "ua_t", 4) && sampler_type(type)) {
            if (name[4] >= '1' && name[4] <= '3' && name[4] - '0' + 1 > p->ntex) p->ntex = (unsigned char)(name[4] - '0' + 1);
            continue;
        }
        if (strncmp(name, "ua_pd_", 6) || p->npd >= PD_MAX) continue;
        const char *orig = name + 6;
        int per = size / MAXRUN;
        int comps = type == GL_FLOAT ? 1 : type == GL_FLOAT_VEC2 ? 2 : type == GL_FLOAT_VEC3 ? 3 : type == GL_FLOAT_VEC4 ? 4 : 0;
        if (!comps || per < 1 || per > 4 || size != per * MAXRUN) return;
        typeof(p->pd[0]) *d = &p->pd[p->npd];
        memset(d, 0, sizeof *d);
        d->n = per; d->comps = comps;
        snprintf(d->name, sizeof d->name, "%s", orig);
        d->arr = N(glGetUniformLocation)(t, name);
        int all = d->arr >= 0;
        for (int k = 0; k < per && all; k++) {
            char el[200];
            if (per > 1) snprintf(el, sizeof el, "%s[%d]", orig, k); else snprintf(el, sizeof el, "%s", orig);
            d->loc[k] = N(glGetUniformLocation)(g, el);           // (the game's program's)
            all = d->loc[k] >= 0 && d->loc[k] < PD_LOCS;
        }
        if (!all) continue;                                       // (one the game's program does not use)
        for (int k = 0; k < per; k++) p->pdmap[d->loc[k]] = (unsigned char)(1 + (p->npd << 3 | k));
        // a renderer uniform the fragment shader reads too: must be the same for the whole run
        d->fs = !fs || !fs->orig || word(fs->orig, orig) != NULL;
        p->npd++;
    }
    // the game's uniforms: samplers, and where each is in the twin
    N(glGetProgramiv)(g, GL_ACTIVE_UNIFORMS, &nu);
    for (GLint u = 0; u < nu; u++) {
        char name[160]; GLsizei len = 0; GLint size = 0; GLenum type = 0;
        N(glGetActiveUniform)(g, (GLuint)u, sizeof name - 8, &len, &size, &type, name);
        if (len > 3 && !strcmp(name + len - 3, "[0]")) name[len - 3] = 0;
        for (GLint e = 0; e < size; e++) {
            char el[180];
            if (size > 1) snprintf(el, sizeof el, "%s[%d]", name, e); else snprintf(el, sizeof el, "%s", name);
            GLint loc = N(glGetUniformLocation)(g, el);
            if (loc < 0) continue;
            if (loc >= PD_LOCS) return;                           // (a location the tables do not reach)
            p->tloc[loc] = N(glGetUniformLocation)(t, el); p->utype[loc] = type;
            if (sampler_type(type)) {
                int slot = sampler_slot(type);
                if (size != 1 || p->nsmp >= 16 || slot < 0) return;   // (sampler arrays, external and buffer textures: not followed)
                GLint unit = 0;
                N(glGetUniformiv)(g, loc, &unit);
                if (!strcmp(name, "_MainTex") && slot == 0) p->loc_main = loc;
                p->smp[p->nsmp].loc = loc; p->smp[p->nsmp].unit = unit; p->smp[p->nsmp].tgt = slot; p->nsmp++;
            }
        }
    }
    if (p->ntex > 1 && p->loc_main < 0) p->ntex = 1;
    p->ok = 1;
    g_progs_ok++;
}

// after glLinkProgram (the render thread, with the context): the game's program as the game linked it, and its twin
void glmerge_linked(GLuint prog)
{
    if (!g_on || !g_next) return;
    pthread_mutex_lock(&g_lock);
    prog_t *p = prg(prog, 1);
    if (!p) { pthread_mutex_unlock(&g_lock); return; }
    if (p->twin) { N(glDeleteProgram)(p->twin); p->twin = 0; }
    GLint ok = 0;
    N(glGetProgramiv)(prog, GL_LINK_STATUS, &ok);
    shd_t *v = p->vs ? shd(p->vs, 0) : NULL, *f = p->fs ? shd(p->fs, 0) : NULL;
    if (ok && v && v->rw && f && f->orig) {
        p->twin = f->rw ? link_twin(prog, v->rw, f->rw) : 0;
        if (!p->twin) { if (f->rw) g_relinks++; p->twin = link_twin(prog, v->rw, f->orig); }   // (the fragment shader as the game's)
    }
    introspect(p);
    pthread_mutex_unlock(&g_lock);
}
void glmerge_binary(GLuint prog)
{
    if (!g_on || !g_next) return;
    pthread_mutex_lock(&g_lock);
    prog_t *p = prg(prog, 1);
    if (p) { if (p->twin) { N(glDeleteProgram)(p->twin); p->twin = 0; } introspect(p); }   // (no sources: no twin)
    pthread_mutex_unlock(&g_lock);
}
void glmerge_deleted(GLuint prog)
{
    if (!g_on) return;
    pthread_mutex_lock(&g_lock);
    prog_t *p = prg(prog, 0);
    if (p) { p->ok = 0; if (p->twin && g_next) { N(glDeleteProgram)(p->twin); p->twin = 0; } }
    pthread_mutex_unlock(&g_lock);
}
// a sampler uniform set (glUniform1i on the bound program): the unit the scan reads
static void prog_sampler_set(prog_t *p, GLint loc, GLsizei count, const GLint *v)
{
    for (GLsizei c = 0; c < count; c++)
        for (int i = 0; i < p->nsmp; i++) if (p->smp[i].loc == loc + c) p->smp[i].unit = v[c];
}

// ======================================================================= the scan's state
#define A_(i) (a[i])
#define I(i) ((GLint)(int64_t)a[i])
#define U(i) ((GLuint)a[i])
static float fbits(uint64_t v) { uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }

#define NATTR 16
typedef struct { GLuint buf; GLint size; GLenum type; GLboolean norm, integer; GLsizei stride; uintptr_t off; } attr_t;
typedef struct { GLuint name; uint32_t known, enabled_known, enabled; attr_t a[NATTR]; GLuint ebo; int ebo_known, used; } svao_t;
#define NSVAO 8
typedef struct {
    GLuint prog; int prog_known;
    int active, active_known;
    GLuint tex[32][4]; uint32_t tex_known[4];
    GLuint smp[32]; uint32_t smp_known;
    GLuint array; int array_known;
    GLuint vao; int vao_known;
    svao_t v[NSVAO]; int nv;
    GLuint cw; int cw_known;              // GL_COPY_WRITE_BUFFER (our upload's target)
    GLuint bp[8]; uint32_t bp_known;      // other binding points (maps: which buffer)
} sstate;
static sstate S;
static void st_reset(void) { memset(&S, 0, sizeof S); }
static GLuint cur_prog(void)
{
    if (!S.prog_known) { GLint v = 0; N(glGetIntegerv)(GL_CURRENT_PROGRAM, &v); S.prog = (GLuint)v; S.prog_known = 1; }
    return S.prog;
}
static int cur_active(void)
{
    if (!S.active_known) { GLint v = 0; N(glGetIntegerv)(GL_ACTIVE_TEXTURE, &v); S.active = v - GL_TEXTURE0; S.active_known = 1; }
    return S.active;
}
static GLint g_start_active;              // (the driver's active unit while the scan asks: the frame's start's)
static const GLenum g_tbind[4] = { GL_TEXTURE_BINDING_2D, GL_TEXTURE_BINDING_3D, GL_TEXTURE_BINDING_CUBE_MAP, GL_TEXTURE_BINDING_2D_ARRAY };
static GLuint tex_at(int unit, int slot)
{
    if (unit < 0 || unit >= 32 || slot < 0 || slot > 3) return 0;
    if (!(S.tex_known[slot] & 1u << unit)) {
        GLint v = 0;
        N(glActiveTexture)(GL_TEXTURE0 + (GLenum)unit);
        N(glGetIntegerv)(g_tbind[slot], &v);
        N(glActiveTexture)(GL_TEXTURE0 + (GLenum)g_start_active);
        S.tex[unit][slot] = (GLuint)v; S.tex_known[slot] |= 1u << unit; g_q_tex++;
    }
    return S.tex[unit][slot];
}
static GLuint smp_at(int unit)
{
    if (unit < 0 || unit >= 32) return 0;
    if (!(S.smp_known & 1u << unit)) {
        GLint v = 0;
        N(glActiveTexture)(GL_TEXTURE0 + (GLenum)unit);
        N(glGetIntegerv)(GL_SAMPLER_BINDING, &v);
        N(glActiveTexture)(GL_TEXTURE0 + (GLenum)g_start_active);
        S.smp[unit] = (GLuint)v; S.smp_known |= 1u << unit; g_q_tex++;
    }
    return S.smp[unit];
}
static GLuint cur_array(void)
{
    if (!S.array_known) { GLint v = 0; N(glGetIntegerv)(GL_ARRAY_BUFFER_BINDING, &v); S.array = (GLuint)v; S.array_known = 1; }
    return S.array;
}
static GLuint g_start_vao;
static svao_t *cur_vao(void)
{
    if (!S.vao_known) { GLint v = 0; N(glGetIntegerv)(GL_VERTEX_ARRAY_BINDING, &v); S.vao = (GLuint)v; S.vao_known = 1; g_start_vao = S.vao; }
    for (int i = 0; i < S.nv; i++) if (S.v[i].name == S.vao) return &S.v[i];
    if (S.nv == NSVAO) { memmove(&S.v[0], &S.v[1], sizeof S.v[0] * (NSVAO - 1)); S.nv--; }
    svao_t *v = &S.v[S.nv++];
    memset(v, 0, sizeof *v);
    v->name = S.vao; v->used = 1;
    return v;
}
// what the frame did not set of the bound vertex array: the driver's (its state at the frame's start; bound for the question)
static int g_vao_bound_q;                 // (the scan bound another vertex array in the driver to ask)
static void vao_ask_bind(svao_t *v)
{
    GLuint want = v->name;
    if (want != g_start_vao) { N(glBindVertexArray)(want); g_vao_bound_q = 1; }
}
static void vao_ask_done(svao_t *v) { if (v->name != g_start_vao) N(glBindVertexArray)(g_start_vao); }
static int attr_enabled(svao_t *v, int i)
{
    if (!(v->enabled_known & 1u << i)) {
        vao_ask_bind(v);
        GLint e = 0;
        N(glGetVertexAttribiv)((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &e);
        vao_ask_done(v);
        if (e) v->enabled |= 1u << i; else v->enabled &= ~(1u << i);
        v->enabled_known |= 1u << i; g_q_attr++;
    }
    return (v->enabled >> i) & 1;
}
static const attr_t *attr_of(svao_t *v, int i)
{
    if (!(v->known & 1u << i)) {
        vao_ask_bind(v);
        GLint b = 0, sz = 0, ty = 0, nm = 0, st = 0, in = 0; void *ptr = NULL;
        N(glGetVertexAttribiv)((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &b);
        N(glGetVertexAttribiv)((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &sz);
        N(glGetVertexAttribiv)((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &ty);
        N(glGetVertexAttribiv)((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &nm);
        N(glGetVertexAttribiv)((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &st);
        N(glGetVertexAttribiv)((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &in);
        N(glGetVertexAttribPointerv)((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
        vao_ask_done(v);
        v->a[i] = (attr_t){ (GLuint)b, sz, (GLenum)ty, (GLboolean)(nm != 0), (GLboolean)(in != 0), st, (uintptr_t)ptr };
        v->known |= 1u << i; g_q_attr++;
    }
    return &v->a[i];
}
static GLuint vao_ebo(svao_t *v)
{
    if (!v->ebo_known) {
        vao_ask_bind(v);
        GLint e = 0; N(glGetIntegerv)(GL_ELEMENT_ARRAY_BUFFER_BINDING, &e);
        vao_ask_done(v);
        v->ebo = (GLuint)e; v->ebo_known = 1;
    }
    return v->ebo;
}
static const GLenum g_bpt[8] = { GL_COPY_WRITE_BUFFER, GL_COPY_READ_BUFFER, GL_UNIFORM_BUFFER, GL_PIXEL_UNPACK_BUFFER,
                                 GL_PIXEL_PACK_BUFFER, GL_TRANSFORM_FEEDBACK_BUFFER, GL_SHADER_STORAGE_BUFFER, GL_DRAW_INDIRECT_BUFFER };
static const GLenum g_bpb[8] = { GL_COPY_WRITE_BUFFER_BINDING, GL_COPY_READ_BUFFER_BINDING, GL_UNIFORM_BUFFER_BINDING, GL_PIXEL_UNPACK_BUFFER_BINDING,
                                 GL_PIXEL_PACK_BUFFER_BINDING, GL_TRANSFORM_FEEDBACK_BUFFER_BINDING, GL_SHADER_STORAGE_BUFFER_BINDING, GL_DRAW_INDIRECT_BUFFER_BINDING };
static int bp_index(GLenum t) { for (int i = 0; i < 8; i++) if (g_bpt[i] == t) return i; return -1; }
static GLuint bound_buffer(GLenum t, int *known)
{
    *known = 1;
    if (t == GL_ARRAY_BUFFER) return cur_array();
    if (t == GL_ELEMENT_ARRAY_BUFFER) return vao_ebo(cur_vao());
    int i = bp_index(t);
    if (i < 0) { *known = 0; return 0; }
    if (!(S.bp_known & 1u << i)) { GLint v = 0; N(glGetIntegerv)(g_bpb[i], &v); S.bp[i] = (GLuint)v; S.bp_known |= 1u << i; }
    return S.bp[i];
}

// ---- what the frame wrote into buffers (the bytes are in its blobs)
static int g_poison_w;                     // (the write log could not grow: nothing found this frame)
typedef struct { GLuint buf; size_t off, len; const unsigned char *p; long prev; } wr_t;   // p NULL: unknown bytes there
static wr_t *g_wr; static size_t g_nwr, g_capwr;
#define NWH 1024
static struct { GLuint buf; unsigned gen; long last; } g_wh[NWH];  // each buffer's last write this frame
static unsigned g_wgen = 1;
static long *wh_slot(GLuint buf, int add)
{
    unsigned i = (buf * 2654435761u) % NWH;
    for (unsigned k = 0; k < NWH; k++, i = (i + 1) % NWH) {
        if (g_wh[i].gen == g_wgen && g_wh[i].buf == buf) return &g_wh[i].last;
        if (g_wh[i].gen != g_wgen) {
            if (!add) return NULL;
            g_wh[i].gen = g_wgen; g_wh[i].buf = buf; g_wh[i].last = -1;
            return &g_wh[i].last;
        }
    }
    return NULL;
}
static void wr_add(GLuint buf, size_t off, size_t len, const void *p)
{
    if (g_nwr == g_capwr) {
        size_t c = g_capwr ? g_capwr * 2 : 256;
        wr_t *n = realloc(g_wr, c * sizeof *n);
        if (!n) { g_poison_w = 1; return; }
        g_wr = n; g_capwr = c;
    }
    long *l = wh_slot(buf, 1);
    if (!l) { g_poison_w = 1; return; }
    g_wr[g_nwr] = (wr_t){ buf, off, len, p, *l };
    *l = (long)g_nwr++;
}
// the bytes [off, off + len) of buf as the frame wrote them at this point, or NULL
static const unsigned char *wr_find(GLuint buf, size_t off, size_t len)
{
    if (g_poison_w) return NULL;
    long *l = wh_slot(buf, 0);
    for (long i = l ? *l : -1; i >= 0; i = g_wr[i].prev) {
        wr_t *w = &g_wr[i];
        if (off + len <= w->off || off >= w->off + w->len) continue;     // (no overlap)
        if (!w->p || off < w->off || off + len > w->off + w->len) return NULL;          // (unknown, or only part of it)
        return w->p + (off - w->off);
    }
    return NULL;
}
typedef struct { uint32_t id; GLuint buf; size_t off, len; } mapr_t;
static mapr_t g_maps[16]; static int g_nmaps;

// ---- uniforms: the renderer ones' values (in the program's entry); any other change bumps the program's version
static prog_t *g_lastp;                    // (the last program a uniform call named)
static prog_t *prg_fast(GLuint prog)
{
    if (g_lastp && g_lastp->used && g_lastp->name == prog) return g_lastp;
    prog_t *p = prg(prog, 0);
    if (p) g_lastp = p;
    return p;
}
static void uni_set(GLuint prog, GLint loc, GLsizei count, int comps, const float *v)
{
    if (!prog || loc < 0) return;
    prog_t *p = prg_fast(prog);
    if (!p || !p->ok) return;
    int bumped = 0;
    for (GLsizei c = 0; c < count; c++) {
        GLint l = loc + c;
        unsigned m = l < PD_LOCS ? p->pdmap[l] : 0;
        if (!m) {
            // (the same value again changes nothing; one the scan has not seen is asked once)
            if (bumped) continue;
            if (l >= PD_LOCS || !p->uval || !p->uknown) { p->uver++; bumped = 1; continue; }
            float x[4] = { 0, 0, 0, 0 };
            memcpy(x, v + c * comps, (size_t)comps * 4);
            if (!p->uknown[l]) { N(glGetUniformfv)(prog, l, p->uval[l]); for (int q = comps; q < 4; q++) p->uval[l][q] = 0; p->uknown[l] = 1; g_q_uni++; }
            if (memcmp(p->uval[l], x, 16)) { p->uver++; bumped = 1; }
            memcpy(p->uval[l], x, 16);
            continue;
        }
        int i = (int)(m - 1) >> 3, k = (int)(m - 1) & 7;
        float x[4] = { 0, 0, 0, 0 };
        memcpy(x, v + c * comps, (size_t)comps * 4);
        // (one the fragment shader reads too must stay the same over a run: a change is a change of version)
        if (p->pd[i].fs && (!p->pdknown[i][k] || memcmp(p->pdval[i][k], x, 16)) && !bumped) { p->uver++; bumped = 1; }
        memcpy(p->pdval[i][k], x, 16); p->pdknown[i][k] = 1;
    }
}
static const float *pd_get(prog_t *p, int i, int k)
{
    if (!p->pdknown[i][k]) { N(glGetUniformfv)(p->name, p->pd[i].loc[k], p->pdval[i][k]); p->pdknown[i][k] = 1; g_q_uni++; }
    return p->pdval[i][k];
}

// ======================================================================= runs
typedef struct {
    size_t at;                             // the draw's command (word offset)
    GLuint prog; prog_t *p;
    GLuint vbuf; GLsizei stride; uint32_t amask; attr_t at_[NATTR];   // its vertex attributes (offsets as set)
    uintptr_t base;                        // the lowest attribute offset
    const unsigned char *vdata;            // its vertices' bytes [vmin, vmax]
    unsigned vmin, vmax;
    const void *idx; GLenum itype; GLsizei icount; GLuint ebo; uintptr_t ioff;
    GLuint tmain, smain; uint64_t thash;   // _MainTex's texture and sampler; the others' hash
    GLuint tfix[16]; int ntfix;            // the others' textures
    unsigned uver; int active;             // its program's uniform version; the active texture unit
    size_t ipos;                           // (in a run) its first index in the run's indices
    GLuint array_after;                    // GL_ARRAY_BUFFER as the draw left it
    float pdv[PD_MAX][4][4];               // its renderer uniforms
} cand_t;
static cand_t *g_c; static size_t g_nc, g_capc;
typedef struct { size_t first, n; size_t voff, soff, ioff, nidx, nverts; GLuint tex[NTEX]; int ntex; float *pd; } run_t;
static run_t *g_run; static size_t g_nrun, g_caprun;
static unsigned char *g_up; static size_t g_nup, g_capup;   // the frame's merged geometry (one upload)
static GLuint g_mb;                        // the host's buffer for it
static size_t *g_skip; static size_t g_nskip, g_capskip;    // draws made by a run's last draw (sorted)

static void *grow(void *p, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) return p;
    size_t c = *cap ? *cap : 64;
    while (c < need) c *= 2;
    void *n = realloc(p, c * elem);
    if (!n) return NULL;
    *cap = c;
    return n;
}
static size_t up_put(const void *d, size_t n, size_t align)
{
    size_t at = (g_nup + align - 1) & ~(align - 1);
    unsigned char *q = grow(g_up, &g_capup, at + n, 1);
    if (!q) return (size_t)-1;
    g_up = q;
    if (d) memcpy(g_up + at, d, n);              // (else the caller fills it)
    g_nup = at + n;
    return at;
}

static const uint64_t *g_fcmd;            // the frame being scanned
static const uint64_t *f_cmd_args(size_t at) { return g_fcmd + at + 1; }
static unsigned f_cmd_op(size_t at) { return (unsigned)(g_fcmd[at] & 0xffffffffu); }
static int g_vframe;                       // (verbose: frames logged)
static int g_cur_end = -1;
static void vlog_end(int why) { if (why >= 0 && why < 16) g_end[why]++; g_cur_end = why; }

static size_t isize(GLenum t) { return t == GL_UNSIGNED_BYTE ? 1 : t == GL_UNSIGNED_SHORT ? 2 : t == GL_UNSIGNED_INT ? 4 : 0; }
static unsigned iread(const void *p, GLenum t, size_t i)
{
    if (t == GL_UNSIGNED_BYTE) return ((const unsigned char *)p)[i];
    if (t == GL_UNSIGNED_SHORT) { uint16_t v; memcpy(&v, (const unsigned char *)p + 2 * i, 2); return v; }
    uint32_t v; memcpy(&v, (const unsigned char *)p + 4 * i, 4); return v;
}

// a draw the scan can merge, filled in c; else 0 (and why)
static int candidate(unsigned op, const uint64_t *a, const fg_frame *f, cand_t *c, int *why)
{
    *why = E_DRAW;
    GLenum mode, type; GLsizei count; uint64_t ix; GLint basev = 0;
    if (op == OP_glDrawElements) { mode = U(0); count = I(1); type = U(2); ix = a[3]; }
    else if (op == OP_glDrawRangeElements) { mode = U(0); count = I(3); type = U(4); ix = a[5]; }
    else if (op == OP_glDrawElementsBaseVertex || op == OP_glDrawElementsBaseVertexOES || op == OP_glDrawElementsBaseVertexEXT) {
        mode = U(0); count = I(1); type = U(2); ix = a[3]; basev = I(4);
    } else return 0;
    if (mode != GL_TRIANGLES || count <= 0 || !isize(type)) return 0;
    memset(c, 0, offsetof(cand_t, pdv));
    c->prog = cur_prog();
    c->p = prg(c->prog, 0);
    if (!c->p || !c->p->ok) return 0;
    svao_t *v = cur_vao();
    if (attr_enabled(v, SEL_LOC)) return 0;
    // indices: the frame's bytes of the element buffer, or client memory the recorder copied
    c->ebo = vao_ebo(v); c->itype = type; c->icount = count;
    if (c->ebo) {
        c->ioff = (uintptr_t)ix;
        c->idx = wr_find(c->ebo, (size_t)c->ioff, (size_t)count * isize(type));
        if (!c->idx) { *why = E_VERTS; return 0; }
    } else c->idx = f->ptr(ix);
    if (!c->idx) return 0;
    unsigned mn = ~0u, mx = 0;
    if (type == GL_UNSIGNED_SHORT && !((uintptr_t)c->idx & 1)) {          // (Unity's: 16-bit, aligned)
        const uint16_t *q = c->idx;
        for (GLsizei i = 0; i < count; i++) { unsigned x = q[i]; mn = x < mn ? x : mn; mx = x > mx ? x : mx; }
    } else
        for (GLsizei i = 0; i < count; i++) { unsigned x = iread(c->idx, type, (size_t)i); if (x < mn) mn = x; if (x > mx) mx = x; }
    if (basev < 0 && (unsigned)(-basev) > mn) return 0;
    mn += (unsigned)basev; mx += (unsigned)basev;
    if (mx - mn >= 65535) return 0;
    c->vmin = mn; c->vmax = mx;
    // the vertex attributes: one buffer, one stride
    int first = 1;
    for (int i = 0; i < NATTR; i++) {
        if (i == SEL_LOC || !attr_enabled(v, i)) continue;
        const attr_t *t = attr_of(v, i);
        if (!t->buf) return 0;
        GLsizei st = t->stride;
        if (!st) return 0;                        // (tightly packed single attributes: not Unity's)
        if (first) { c->vbuf = t->buf; c->stride = st; c->base = t->off; first = 0; }
        else if (t->buf != c->vbuf || st != c->stride) return 0;
        if (t->off < c->base) c->base = t->off;
        c->at_[i] = *t;
        c->amask |= 1u << i;
    }
    if (first) return 0;
    for (int i = 0; i < NATTR; i++) if ((c->amask >> i & 1) && c->at_[i].off - c->base >= (uintptr_t)c->stride) return 0;
    size_t vlen = (size_t)(mx - mn + 1) * (size_t)c->stride;
    c->vdata = wr_find(c->vbuf, (size_t)c->base + (size_t)mn * (size_t)c->stride, vlen);
    if (!c->vdata) { *why = E_VERTS; return 0; }
    // textures: _MainTex's (may change within a run), every other sampler's (may not)
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < c->p->nsmp; i++) {
        int unit = c->p->smp[i].unit;
        if (unit < 0 || unit >= 29) return 0;     // (units 29 - 31 are the host's)
        if (c->p->smp[i].loc == c->p->loc_main) { c->tmain = tex_at(unit, 0); c->smain = smp_at(unit); continue; }
        GLuint tx = tex_at(unit, c->p->smp[i].tgt);
        if (c->ntfix < 16) c->tfix[c->ntfix++] = tx;
        h = (h ^ tx) * 1099511628211ull; h = (h ^ smp_at(unit)) * 1099511628211ull; h = (h ^ (unsigned)unit) * 1099511628211ull;
    }
    c->thash = h;
    c->uver = c->p->uver;
    c->active = cur_active();
    // the renderer uniforms' values
    for (int i = 0; i < c->p->npd; i++)
        for (int k = 0; k < c->p->pd[i].n; k++) {
            memcpy(c->pdv[i][k], pd_get(c->p, i, k), 16);
        }
    c->array_after = cur_array();
    return 1;
}
static int same_layout(const cand_t *x, const cand_t *y)
{
    if (x->amask != y->amask || x->stride != y->stride) return 0;
    for (int i = 0; i < NATTR; i++) {
        if (!(x->amask >> i & 1)) continue;
        const attr_t *a = &x->at_[i], *b = &y->at_[i];
        if (a->size != b->size || a->type != b->type || a->norm != b->norm || a->integer != b->integer ||
            a->off - x->base != b->off - y->base) return 0;
    }
    return 1;
}
// the renderer uniforms the fragment shader reads too: equal over the run
static int same_fs_uniforms(const cand_t *x, const cand_t *y)
{
    for (int i = 0; i < x->p->npd; i++) if (x->p->pd[i].fs && memcmp(x->pdv[i], y->pdv[i], (size_t)x->p->pd[i].n * 16)) return 0;
    return 1;
}

// the run being built
static size_t g_rfirst; static size_t g_rn; static GLuint g_rtex[NTEX]; static int g_rntex; static size_t g_rverts;
static void run_close(int why)
{
    if (!g_rn) return;
    vlog_end(why);
    if (g_rn < 2) { g_rn = 0; return; }
    run_t *r = grow(g_run, &g_caprun, g_nrun + 1, sizeof *g_run);
    if (!r) { g_rn = 0; return; }
    g_run = r;
    run_t *R = &g_run[g_nrun];
    memset(R, 0, sizeof *R);
    R->first = g_rfirst; R->n = g_rn;
    // texture 0 is the last draw's: _MainTex's unit holds it when the run is made
    cand_t *last = &g_c[g_rfirst + g_rn - 1];
    R->ntex = 0;
    R->tex[R->ntex++] = last->tmain;
    for (int t = 0; t < g_rntex; t++) if (g_rtex[t] != last->tmain) R->tex[R->ntex++] = g_rtex[t];
    // the geometry: each draw's vertices in order, its indices moved to them; ua_v a byte a vertex
    size_t nv = 0, ni = 0;
    for (size_t k = 0; k < g_rn; k++) { cand_t *c = &g_c[g_rfirst + k]; nv += c->vmax - c->vmin + 1; ni += (size_t)c->icount; }
    size_t vo = up_put(NULL, nv * (size_t)last->stride, 16), so = up_put(NULL, nv * 4, 4), io = up_put(NULL, ni * 2, 4);
    if (vo == (size_t)-1 || so == (size_t)-1 || io == (size_t)-1) { g_rn = 0; return; }
    size_t pos = 0, ip = 0;
    for (size_t k = 0; k < g_rn; k++) {
        cand_t *c = &g_c[g_rfirst + k];
        size_t n = c->vmax - c->vmin + 1;
        memcpy(g_up + vo + pos * (size_t)c->stride, c->vdata, n * (size_t)c->stride);
        int sel = 0;
        for (int t = 0; t < R->ntex; t++) if (R->tex[t] == c->tmain) sel = t;
        float sv = (float)(1 + (int)k + 16 * sel);
        for (size_t q = 0; q < n; q++) memcpy(g_up + so + 4 * (pos + q), &sv, 4);
        GLint basev = 0;
        const uint64_t *a = f_cmd_args(c->at);
        unsigned op = f_cmd_op(c->at);
        if (op == OP_glDrawElementsBaseVertex || op == OP_glDrawElementsBaseVertexOES || op == OP_glDrawElementsBaseVertexEXT) basev = I(4);
        unsigned shift = (unsigned)basev - c->vmin + (unsigned)pos;
        uint16_t *out = (uint16_t *)(void *)(g_up + io) + ip;                 // (io is 4-aligned)
        if (c->itype == GL_UNSIGNED_SHORT && !((uintptr_t)c->idx & 1)) {
            const uint16_t *q = c->idx;
            for (GLsizei i = 0; i < c->icount; i++) out[i] = (uint16_t)(q[i] + shift);
        } else
            for (GLsizei i = 0; i < c->icount; i++) out[i] = (uint16_t)(iread(c->idx, c->itype, (size_t)i) + shift);
        c->ipos = ip;
        pos += n; ip += (size_t)c->icount;
    }
    R->voff = vo; R->soff = so; R->ioff = io; R->nidx = ni; R->nverts = nv;
    // the renderer uniforms: draw k's at [k]
    prog_t *p = last->p;
    size_t tot = 0;
    for (int i = 0; i < p->npd; i++) tot += (size_t)p->pd[i].n * MAXRUN * 4;
    R->pd = tot ? calloc(tot, sizeof(float)) : NULL;
    if (tot && !R->pd) { g_rn = 0; return; }
    size_t o = 0;
    for (int i = 0; i < p->npd; i++) {
        int n = p->pd[i].n, cps = p->pd[i].comps;
        for (size_t k = 0; k < g_rn; k++)
            for (int e = 0; e < n; e++) memcpy(R->pd + o + ((size_t)k * (size_t)n + (size_t)e) * (size_t)cps, g_c[g_rfirst + k].pdv[i][e], (size_t)cps * 4);
        o += (size_t)n * MAXRUN * (size_t)cps;
    }
    for (size_t k = 0; k + 1 < g_rn; k++) {
        size_t *s = grow(g_skip, &g_capskip, g_nskip + 1, sizeof *g_skip);
        if (!s) break;
        g_skip = s; g_skip[g_nskip++] = g_c[g_rfirst + k].at;
    }
    g_nrun++;
    g_runs++; g_merged_draws += g_rn;
    g_rn = 0;
}

// ======================================================================= the walk
static int run_reads(GLuint t)
{
    if (!g_rn) return 0;
    for (int i = 0; i < g_rntex; i++) if (g_rtex[i] == t) return 1;
    const cand_t *c = &g_c[g_rfirst];
    for (int i = 0; i < c->ntfix; i++) if (c->tfix[i] == t) return 1;
    return 0;
}
static int g_prog_uniform_op(unsigned op)
{
    const char *n = op < OP_N ? glrec_name[op] : "";
    return !strncmp(n, "glUniform", 9) || !strncmp(n, "glProgramUniform", 16);
}
static int g_poison;                       // a write the scan cannot follow: no more candidates this frame
static int g_sel_set;                      // the game set ua_v's generic value: merging off for good
static void wr_unknown(GLuint buf) { wr_add(buf, 0, (size_t)1 << 30, NULL); }
static void tex_forget(void) { memset(S.tex_known, 0, sizeof S.tex_known); S.smp_known = 0; }

static int tgt_slot(GLenum t)
{
    return t == GL_TEXTURE_2D ? 0 : t == GL_TEXTURE_3D ? 1 : t == GL_TEXTURE_CUBE_MAP ? 2 : t == GL_TEXTURE_2D_ARRAY ? 3 : -1;
}

static void walk(size_t at, unsigned op, const uint64_t *a, const fg_frame *f)
{
    switch (op) {
    // ---- draws
    case OP_glDrawElements: case OP_glDrawRangeElements:
    case OP_glDrawElementsBaseVertex: case OP_glDrawElementsBaseVertexOES: case OP_glDrawElementsBaseVertexEXT: {
        g_draws++;
        size_t ci = g_nc;
        cand_t *c = grow(g_c, &g_capc, g_nc + 1, sizeof *g_c);
        if (!c) { run_close(E_CALL); return; }
        g_c = c; c = &g_c[ci];
        int why = E_DRAW;
        if (g_poison || g_sel_set || !candidate(op, a, f, c, &why)) { run_close(why); return; }
        c->at = at;
        if (g_rn) {
            cand_t *prev = &g_c[g_rfirst + g_rn - 1];
            int end = -1, room = c->p->ntex < NTEX ? c->p->ntex : NTEX, have = 0;
            for (int t = 0; t < g_rntex; t++) if (g_rtex[t] == c->tmain) have = 1;
            if (c->prog != prev->prog) end = E_PROG;
            else if (!same_layout(c, prev)) end = E_LAYOUT;
            else if (c->thash != prev->thash || c->smain != prev->smain) end = E_TEX;
            else if (c->uver != prev->uver || !same_fs_uniforms(c, prev)) end = E_UNI;
            else if ((int)g_rn >= g_max || g_rverts + (c->vmax - c->vmin + 1) > 65535) end = E_FULL;
            else if (!have && g_rntex >= room) end = E_TEX;
            if (end >= 0) run_close(end);
        }
        if (!g_rn) { g_rfirst = ci; g_rntex = 0; g_rverts = 0; }
        int have = 0;
        for (int t = 0; t < g_rntex; t++) if (g_rtex[t] == c->tmain) have = 1;
        if (!have) g_rtex[g_rntex++] = c->tmain;
        g_rverts += c->vmax - c->vmin + 1;
        g_rn++; g_nc++;
        return;
    }
    // ---- geometry: followed, a run goes on
    case OP_glBindBuffer: {
        GLenum t = U(0); GLuint b = U(1);
        if (t == GL_ARRAY_BUFFER) { S.array = b; S.array_known = 1; }
        else if (t == GL_ELEMENT_ARRAY_BUFFER) { svao_t *v = cur_vao(); v->ebo = b; v->ebo_known = 1; }
        else { int i = bp_index(t); if (i >= 0) { S.bp[i] = b; S.bp_known |= 1u << i; } }
        return;
    }
    case OP_glBindVertexArray: case OP_glBindVertexArrayOES:
        if (!S.vao_known) cur_vao();                 // (the frame's start's, asked first)
        S.vao = U(0); S.vao_known = 1;
        return;
    case OP_glVertexAttribPointer: case OP_glVertexAttribIPointer: {
        GLuint i = U(0);
        if (i >= NATTR) { run_close(E_STATE); return; }
        if (i == SEL_LOC) { g_sel_set = 1; run_close(E_STATE); return; }
        svao_t *v = cur_vao();
        if (op == OP_glVertexAttribPointer) v->a[i] = (attr_t){ cur_array(), I(1), U(2), (GLboolean)U(3), 0, I(4), (uintptr_t)a[5] };
        else v->a[i] = (attr_t){ cur_array(), I(1), U(2), 0, 1, I(3), (uintptr_t)a[4] };
        v->known |= 1u << i;
        return;
    }
    case OP_glEnableVertexAttribArray: case OP_glDisableVertexAttribArray: {
        GLuint i = U(0);
        if (i >= NATTR) { run_close(E_STATE); return; }
        if (i == SEL_LOC) { g_sel_set = 1; run_close(E_STATE); return; }
        svao_t *v = cur_vao();
        if (op == OP_glEnableVertexAttribArray) v->enabled |= 1u << i; else v->enabled &= ~(1u << i);
        v->enabled_known |= 1u << i;
        return;
    }
    case OP_glMapBufferRange: {
        int known; GLuint b = bound_buffer(U(0), &known);
        if (!known) { g_poison = 1; return; }
        if (g_nmaps < 16) g_maps[g_nmaps++] = (mapr_t){ (uint32_t)a[4], b, (size_t)(int64_t)a[1], (size_t)(int64_t)a[2] };
        else g_poison = 1;
        return;
    }
    case OP_glFlushMappedBufferRange: {
        for (int m = 0; m < g_nmaps; m++)
            if (g_maps[m].id == (uint32_t)a[3]) { wr_add(g_maps[m].buf, g_maps[m].off + (size_t)(int64_t)a[1], (size_t)(int64_t)a[2], a[4] ? f->ptr(a[4]) : NULL); return; }
        g_poison = 1;
        return;
    }
    case OP_glUnmapBuffer: {
        for (int m = 0; m < g_nmaps; m++)
            if (g_maps[m].id == (uint32_t)a[1]) {
                if (a[2]) wr_add(g_maps[m].buf, g_maps[m].off, (size_t)a[3], f->ptr(a[2]));
                g_maps[m] = g_maps[--g_nmaps];
                return;
            }
        g_poison = 1;
        return;
    }
    case OP_glBufferSubData: {
        int known; GLuint b = bound_buffer(U(0), &known);
        if (!known) { g_poison = 1; return; }
        wr_add(b, (size_t)(int64_t)a[1], (size_t)(int64_t)a[2], a[3] ? f->ptr(a[3]) : NULL);
        return;
    }
    case OP_glBufferData: {
        int known; GLuint b = bound_buffer(U(0), &known);
        if (!known) { g_poison = 1; return; }
        wr_unknown(b);
        if (a[2]) wr_add(b, 0, (size_t)(int64_t)a[1], f->ptr(a[2]));
        return;
    }
    case OP_glCopyBufferSubData: {
        int known; GLuint b = bound_buffer(U(1), &known);
        if (!known) g_poison = 1; else wr_unknown(b);
        run_close(E_CALL);
        return;
    }
    // ---- textures and samplers
    case OP_glActiveTexture: S.active = (int)(U(0) - GL_TEXTURE0); S.active_known = 1; return;
    case OP_glBindTexture: {
        int u = cur_active();
        if (u < 0 || u >= 32) { run_close(E_STATE); return; }
        if (u >= 29) { run_close(E_STATE); g_poison = 1; return; }      // (the host's units)
        int sl = tgt_slot(U(0));
        if (sl >= 0) { S.tex[u][sl] = U(1); S.tex_known[sl] |= 1u << u; }
        else run_close(E_STATE);
        return;
    }
    case OP_glBindSampler: { GLuint u = U(0); if (u < 32) { S.smp[u] = U(1); S.smp_known |= 1u << u; } return; }
    case OP_glTexParameteri: case OP_glTexParameterf: case OP_glTexParameteriv: case OP_glTexParameterfv: {
        // a texture the open run reads: the run ends (its draws read the old parameters); any other goes on
        int sl = tgt_slot(U(0)), u = cur_active();
        GLuint t = sl >= 0 && u >= 0 && u < 32 ? tex_at(u, sl) : 0;
        if (!t || run_reads(t)) run_close(E_TEXPARAM);
        return;
    }
    case OP_glSamplerParameteri: case OP_glSamplerParameterf: case OP_glSamplerParameteriv: case OP_glSamplerParameterfv:
        run_close(E_TEXPARAM);
        return;
    // ---- programs and uniforms
    case OP_glUseProgram: S.prog = U(0); S.prog_known = 1; return;
    case OP_glUniform1f: case OP_glUniform2f: case OP_glUniform3f: case OP_glUniform4f: {
        int n = op == OP_glUniform1f ? 1 : op == OP_glUniform2f ? 2 : op == OP_glUniform3f ? 3 : 4;
        float v[4] = { 0 };
        for (int i = 0; i < n; i++) v[i] = fbits(a[1 + i]);
        uni_set(cur_prog(), I(0), 1, n, v);
        return;
    }
    case OP_glUniform1fv: case OP_glUniform2fv: case OP_glUniform3fv: case OP_glUniform4fv: {
        int n = op == OP_glUniform1fv ? 1 : op == OP_glUniform2fv ? 2 : op == OP_glUniform3fv ? 3 : 4;
        uni_set(cur_prog(), I(0), I(1), n, (const float *)f->ptr(a[2]));
        return;
    }
    case OP_glProgramUniform4fv: uni_set(U(0), I(1), I(2), 4, (const float *)f->ptr(a[3])); return;
    case OP_glUniform1i: case OP_glUniform1iv: case OP_glProgramUniform1i: case OP_glProgramUniform1iv: {
        GLuint pr = op == OP_glUniform1i || op == OP_glUniform1iv ? cur_prog() : U(0);
        prog_t *p = prg(pr, 0);
        if (p) {
            if (op == OP_glUniform1i) { GLint v = I(1); prog_sampler_set(p, I(0), 1, &v); }
            else if (op == OP_glUniform1iv) prog_sampler_set(p, I(0), I(1), (const GLint *)f->ptr(a[2]));
            else if (op == OP_glProgramUniform1i) { GLint v = I(2); prog_sampler_set(p, I(1), 1, &v); }
            else prog_sampler_set(p, I(1), I(2), (const GLint *)f->ptr(a[3]));
            p->uver++;
        }
        return;
    }
    case OP_glPixelStorei: case OP_glPushDebugGroup: case OP_glPushDebugGroupKHR: case OP_glPopDebugGroup: case OP_glPopDebugGroupKHR:
    case OP_glInsertEventMarkerEXT: case OP_glPushGroupMarkerEXT: case OP_glPopGroupMarkerEXT:
        return;
    // ---- what the scan cannot see through
    case OP_glVertexAttrib1f: case OP_glVertexAttrib2f: case OP_glVertexAttrib3f: case OP_glVertexAttrib4f:
    case OP_glVertexAttrib1fv: case OP_glVertexAttrib2fv: case OP_glVertexAttrib3fv: case OP_glVertexAttrib4fv:
    case OP_glVertexAttribI4i: case OP_glVertexAttribI4ui: case OP_glVertexAttribI4iv: case OP_glVertexAttribI4uiv:
        if (U(0) == SEL_LOC) g_sel_set = 1;
        run_close(E_STATE);
        return;
    case OP_glDeleteBuffers: {
        const GLuint *b = (const GLuint *)f->ptr(a[1]);
        for (GLsizei i = 0; b && i < I(0); i++) wr_unknown(b[i]);
        run_close(E_CALL);
        S.array_known = 0; for (int i = 0; i < S.nv; i++) S.v[i].known = 0, S.v[i].ebo_known = 0;
        return;
    }
    case OP_glDeleteTextures: case OP_glDeleteSamplers: tex_forget(); run_close(E_CALL); return;
    case OP_glDeleteVertexArrays: case OP_glDeleteVertexArraysOES: case OP_glVertexAttribBinding: case OP_glBindVertexBuffer:
    case OP_glVertexAttribFormat: case OP_glVertexAttribIFormat: case OP_glVertexBindingDivisor:
        S.nv = 0; S.vao_known = 0; run_close(E_STATE);
        return;
    case OP_glBeginTransformFeedback: case OP_glDispatchCompute: case OP_glDispatchComputeIndirect: case OP_glReadPixels:
        g_poison_w = 1; g_poison = 1; run_close(E_CALL);
        return;
    default:
        if (g_prog_uniform_op(op)) { prog_t *p = prg(cur_prog(), 0); if (p) p->uver++; return; }
        run_close(E_CALL);
        return;
    }
}

// ======================================================================= the replay
static GLint g_cw_start;                   // GL_COPY_WRITE_BUFFER at the frame's start (the upload's target)
static GLuint g_xtex[NTEX], g_xsmp[NTEX];  // what the host bound on its units (29 - 31)
static int g_xknown, g_xact_dirty;

static void point_merged(const run_t *R, const cand_t *L)
{
    N(glBindBuffer)(GL_ARRAY_BUFFER, g_mb);
    for (int i = 0; i < NATTR; i++) {
        if (!(L->amask >> i & 1)) continue;
        const attr_t *t = &L->at_[i];
        const void *o = (const void *)(uintptr_t)(R->voff + (t->off - L->base));
        if (t->integer) N(glVertexAttribIPointer)((GLuint)i, t->size, t->type, t->stride, o);
        else N(glVertexAttribPointer)((GLuint)i, t->size, t->type, t->norm, t->stride, o);
    }
    N(glBindBuffer)(GL_ELEMENT_ARRAY_BUFFER, g_mb);
}
// the game's uniforms the twin does not have yet (the replay's view: what the game set since the last copy)
static unsigned long g_synced;
static void sync_twin(prog_t *p)
{
    for (int w = 0; w < PD_LOCS / 32; w++) {
        uint32_t m = p->dirty[w];
        if (!m) continue;
        p->dirty[w] = 0;
        for (; m; m &= m - 1) {
            int loc = w * 32 + __builtin_ctz(m);
            GLint tl = p->tloc[loc];
            if (tl < 0 || p->pdmap[loc]) continue;        // (none in the twin; a renderer uniform: the arrays)
            float f[16]; GLint iv[4];
            g_synced++;
            switch (p->utype[loc]) {
            case GL_FLOAT: N(glGetUniformfv)(p->name, loc, f); N(glUniform1fv)(tl, 1, f); break;
            case GL_FLOAT_VEC2: N(glGetUniformfv)(p->name, loc, f); N(glUniform2fv)(tl, 1, f); break;
            case GL_FLOAT_VEC3: N(glGetUniformfv)(p->name, loc, f); N(glUniform3fv)(tl, 1, f); break;
            case GL_FLOAT_VEC4: N(glGetUniformfv)(p->name, loc, f); N(glUniform4fv)(tl, 1, f); break;
            case GL_FLOAT_MAT2: N(glGetUniformfv)(p->name, loc, f); N(glUniformMatrix2fv)(tl, 1, GL_FALSE, f); break;
            case GL_FLOAT_MAT3: N(glGetUniformfv)(p->name, loc, f); N(glUniformMatrix3fv)(tl, 1, GL_FALSE, f); break;
            case GL_FLOAT_MAT4: N(glGetUniformfv)(p->name, loc, f); N(glUniformMatrix4fv)(tl, 1, GL_FALSE, f); break;
            case GL_INT: case GL_BOOL: N(glGetUniformiv)(p->name, loc, iv); N(glUniform1iv)(tl, 1, iv); break;
            case GL_INT_VEC2: case GL_BOOL_VEC2: N(glGetUniformiv)(p->name, loc, iv); N(glUniform2iv)(tl, 1, iv); break;
            case GL_INT_VEC3: case GL_BOOL_VEC3: N(glGetUniformiv)(p->name, loc, iv); N(glUniform3iv)(tl, 1, iv); break;
            case GL_INT_VEC4: case GL_BOOL_VEC4: N(glGetUniformiv)(p->name, loc, iv); N(glUniform4iv)(tl, 1, iv); break;
            default: if (sampler_type(p->utype[loc])) { N(glGetUniformiv)(p->name, loc, iv); N(glUniform1iv)(tl, 1, iv); } break;
            }
        }
    }
}
static void emit(const run_t *R)
{
    const cand_t *L = &g_c[R->first + R->n - 1];
    prog_t *p = L->p;
    N(glUseProgram)(p->twin);
    sync_twin(p);
    point_merged(R, L);
    N(glEnableVertexAttribArray)(SEL_LOC);
    N(glVertexAttribPointer)(SEL_LOC, 1, GL_FLOAT, GL_FALSE, 4, (const void *)(uintptr_t)R->soff);
    if (R->ntex > 1) {
        for (int t = 1; t < R->ntex; t++) {
            if (g_xknown & 1 << t && g_xtex[t] == R->tex[t] && g_xsmp[t] == L->smain) continue;
            N(glActiveTexture)(GL_TEXTURE0 + (GLenum)g_xunit[t]);
            N(glBindTexture)(GL_TEXTURE_2D, R->tex[t]);
            N(glBindSampler)((GLuint)g_xunit[t], L->smain);
            g_xtex[t] = R->tex[t]; g_xsmp[t] = L->smain; g_xknown |= 1 << t;
            g_xact_dirty = 1;
        }
        if (g_xact_dirty) { N(glActiveTexture)(GL_TEXTURE0 + (GLenum)L->active); g_xact_dirty = 0; }
    }
    size_t o = 0;
    for (int i = 0; i < p->npd; i++) {
        int n = p->pd[i].n, cps = p->pd[i].comps;
        GLsizei cnt = (GLsizei)((size_t)n * R->n);
        const float *v = R->pd + o;
        if (cps == 4) N(glUniform4fv)(p->pd[i].arr, cnt, v);
        else if (cps == 3) N(glUniform3fv)(p->pd[i].arr, cnt, v);
        else if (cps == 2) N(glUniform2fv)(p->pd[i].arr, cnt, v);
        else N(glUniform1fv)(p->pd[i].arr, cnt, v);
        o += (size_t)n * MAXRUN * (size_t)cps;
    }
    N(glDrawRangeElements)(GL_TRIANGLES, 0, (GLuint)(R->nverts - 1), (GLsizei)R->nidx, GL_UNSIGNED_SHORT, (const void *)(uintptr_t)R->ioff);
    N(glUseProgram)(p->name);
}
// the last draw's state again
static void emit_restore(const run_t *R)
{
    const cand_t *L = &g_c[R->first + R->n - 1];
    N(glDisableVertexAttribArray)(SEL_LOC);
    N(glBindBuffer)(GL_ELEMENT_ARRAY_BUFFER, L->ebo);
    GLuint cur = g_mb;
    for (int i = 0; i < NATTR; i++) {
        if (!(L->amask >> i & 1)) continue;
        const attr_t *t = &L->at_[i];
        if (t->buf != cur) { N(glBindBuffer)(GL_ARRAY_BUFFER, t->buf); cur = t->buf; }
        if (t->integer) N(glVertexAttribIPointer)((GLuint)i, t->size, t->type, t->stride, (const void *)t->off);
        else N(glVertexAttribPointer)((GLuint)i, t->size, t->type, t->norm, t->stride, (const void *)t->off);
    }
    if (cur != L->array_after) N(glBindBuffer)(GL_ARRAY_BUFFER, L->array_after);
}

// ---- UA_GL_MERGE=check
static unsigned long g_ck_runs, g_ck_diff, g_ck_px, g_ck_skip, g_ck_bytex[NTEX + 1][2];
static GLuint g_ckfb[3], g_ckrb[3];
static GLint g_ckw, g_ckh;
static unsigned char *g_ckb[2];
static int ck_fbos(GLint w, GLint h)
{
    if (g_ckfb[0] && g_ckw == w && g_ckh == h) return 1;
    if (g_ckfb[0]) { N(glDeleteFramebuffers)(3, g_ckfb); N(glDeleteRenderbuffers)(3, g_ckrb); free(g_ckb[0]); free(g_ckb[1]); }
    GLint prb = 0, pdf = 0;
    N(glGetIntegerv)(GL_RENDERBUFFER_BINDING, &prb); N(glGetIntegerv)(GL_DRAW_FRAMEBUFFER_BINDING, &pdf);
    N(glGenFramebuffers)(3, g_ckfb); N(glGenRenderbuffers)(3, g_ckrb);
    for (int i = 0; i < 3; i++) {
        N(glBindRenderbuffer)(GL_RENDERBUFFER, g_ckrb[i]);
        N(glRenderbufferStorage)(GL_RENDERBUFFER, GL_RGBA8, w, h);
        N(glBindFramebuffer)(GL_DRAW_FRAMEBUFFER, g_ckfb[i]);
        N(glFramebufferRenderbuffer)(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_ckrb[i]);
    }
    N(glBindRenderbuffer)(GL_RENDERBUFFER, (GLuint)prb); N(glBindFramebuffer)(GL_DRAW_FRAMEBUFFER, (GLuint)pdf);
    g_ckb[0] = malloc((size_t)w * h * 4); g_ckb[1] = malloc((size_t)w * h * 4);
    g_ckw = w; g_ckh = h;
    return g_ckb[0] && g_ckb[1];
}
static void ck_blit(GLuint from, GLuint to, const GLint *src, const GLint *dst)
{
    N(glBindFramebuffer)(GL_READ_FRAMEBUFFER, from); N(glBindFramebuffer)(GL_DRAW_FRAMEBUFFER, to);
    N(glBlitFramebuffer)(src[0], src[1], src[0] + src[2], src[1] + src[3], dst[0], dst[1], dst[0] + dst[2], dst[1] + dst[3], GL_COLOR_BUFFER_BIT, GL_NEAREST);
}
// a run checked, or 0 (it is then made as without the check)
static int emit_checked(const run_t *R)
{
    const cand_t *L = &g_c[R->first + R->n - 1];
    prog_t *p = L->p;
    GLint dfb = 0, rfb = 0, vp[4], samples = 0, rb = 0, sm = 0, dw = 0;
    N(glGetIntegerv)(GL_DRAW_FRAMEBUFFER_BINDING, &dfb); N(glGetIntegerv)(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    N(glGetIntegerv)(GL_VIEWPORT, vp); N(glGetIntegerv)(GL_SAMPLES, &samples);
    GLboolean dmask = 0; N(glGetBooleanv)(GL_DEPTH_WRITEMASK, &dmask);
    N(glGetIntegerv)(GL_STENCIL_WRITEMASK, &sm); N(glGetIntegerv)(GL_DRAW_BUFFER1, &dw);
    int dt = N(glIsEnabled)(GL_DEPTH_TEST), st = N(glIsEnabled)(GL_STENCIL_TEST), sc = N(glIsEnabled)(GL_SCISSOR_TEST);
    N(glBindFramebuffer)(GL_READ_FRAMEBUFFER, (GLuint)dfb);
    N(glGetIntegerv)(GL_READ_BUFFER, &rb);
    GLint rsz = 0, enc = 0;
    N(glGetFramebufferAttachmentParameteriv)(GL_READ_FRAMEBUFFER, dfb ? GL_COLOR_ATTACHMENT0 : GL_BACK, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &rsz);
    N(glGetFramebufferAttachmentParameteriv)(GL_READ_FRAMEBUFFER, dfb ? GL_COLOR_ATTACHMENT0 : GL_BACK, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &enc);
    N(glBindFramebuffer)(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    if ((dt && dmask) || (st && sm) || samples > 0 || dw != GL_NONE || rsz != 8 || enc != GL_LINEAR ||
        (rb != GL_COLOR_ATTACHMENT0 && rb != GL_BACK) || vp[2] <= 0 || vp[3] <= 0 || !ck_fbos(vp[2], vp[3])) { g_ck_skip++; return 0; }
    GLint z[4] = { 0, 0, vp[2], vp[3] };
    if (sc) N(glDisable)(GL_SCISSOR_TEST);
    ck_blit((GLuint)dfb, g_ckfb[0], vp, z);                         // the picture before
    N(glBindFramebuffer)(GL_DRAW_FRAMEBUFFER, (GLuint)dfb); N(glBindFramebuffer)(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    if (sc) N(glEnable)(GL_SCISSOR_TEST);
    emit(R);                                                         // merged
    if (sc) N(glDisable)(GL_SCISSOR_TEST);
    ck_blit((GLuint)dfb, g_ckfb[1], vp, z);
    ck_blit(g_ckfb[0], (GLuint)dfb, z, vp);                          // the picture before again
    N(glBindFramebuffer)(GL_DRAW_FRAMEBUFFER, (GLuint)dfb); N(glBindFramebuffer)(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    if (sc) N(glEnable)(GL_SCISSOR_TEST);
    // the game's draws, one by one, with its own program (from the run's geometry: the same bytes); the
    // renderer uniforms each draw's, the last one's left as the game set them
    N(glDisableVertexAttribArray)(SEL_LOC);
    point_merged(R, L);
    int umain = -1;
    for (int i = 0; i < p->nsmp; i++) if (p->smp[i].loc == p->loc_main) umain = p->smp[i].unit;
    for (size_t k = 0; k < R->n; k++) {
        const cand_t *c = &g_c[R->first + k];
        for (int i = 0; i < p->npd; i++)
            for (int e = 0; e < p->pd[i].n; e++) {
                char el[120];
                if (p->pd[i].n > 1) snprintf(el, sizeof el, "%s[%d]", p->pd[i].name, e); else snprintf(el, sizeof el, "%s", p->pd[i].name);
                GLint l = p->pd[i].loc[e];
                int cps = p->pd[i].comps;
                if (cps == 4) N(glUniform4fv)(l, 1, c->pdv[i][e]); else if (cps == 3) N(glUniform3fv)(l, 1, c->pdv[i][e]);
                else if (cps == 2) N(glUniform2fv)(l, 1, c->pdv[i][e]); else N(glUniform1fv)(l, 1, c->pdv[i][e]);
            }
        if (umain >= 0) { N(glActiveTexture)(GL_TEXTURE0 + (GLenum)umain); N(glBindTexture)(GL_TEXTURE_2D, c->tmain); }
        N(glDrawElements)(GL_TRIANGLES, c->icount, GL_UNSIGNED_SHORT, (const void *)(uintptr_t)(R->ioff + 2 * c->ipos));
    }
    if (umain >= 0) N(glBindTexture)(GL_TEXTURE_2D, L->tmain);
    N(glActiveTexture)(GL_TEXTURE0 + (GLenum)L->active);
    if (sc) N(glDisable)(GL_SCISSOR_TEST);
    ck_blit((GLuint)dfb, g_ckfb[2], vp, z);
    // compared
    N(glBindFramebuffer)(GL_READ_FRAMEBUFFER, g_ckfb[1]);
    N(glReadPixels)(0, 0, vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, g_ckb[0]);
    N(glBindFramebuffer)(GL_READ_FRAMEBUFFER, g_ckfb[2]);
    N(glReadPixels)(0, 0, vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, g_ckb[1]);
    N(glBindFramebuffer)(GL_DRAW_FRAMEBUFFER, (GLuint)dfb); N(glBindFramebuffer)(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    if (sc) N(glEnable)(GL_SCISSOR_TEST);
    unsigned long d = 0; long first = -1;
    const uint32_t *a = (const uint32_t *)g_ckb[0], *b = (const uint32_t *)g_ckb[1];
    for (long i = 0; i < (long)vp[2] * vp[3]; i++) if (a[i] != b[i]) { if (first < 0) first = i; d++; }
    g_ck_runs++;
    g_ck_bytex[R->ntex][d != 0]++;
    if (d) {
        g_ck_diff++; g_ck_px += d;
        const char *dd = getenv("UA_GL_MERGE_DUMP");
        if (dd && *dd && g_ck_diff <= 4) {          // (both pictures, and the one before, as PPM: merged, the game's)
            for (int w = 0; w < 2; w++) {
                char path[600]; snprintf(path, sizeof path, "%s/ck%lu-%s.ppm", dd, g_ck_diff, w ? "game" : "merged");
                FILE *o = fopen(path, "wb");
                if (!o) continue;
                fprintf(o, "P6\n%d %d\n255\n", vp[2], vp[3]);
                const unsigned char *px = g_ckb[w];
                for (long y = vp[3] - 1; y >= 0; y--) for (long x = 0; x < vp[2]; x++) fwrite(px + (y * vp[2] + x) * 4, 1, 3, o);
                fclose(o);
            }
        }
        if (g_ck_diff <= 6 && R->ntex > 1) {
            GLint act = 0; N(glGetIntegerv)(GL_ACTIVE_TEXTURE, &act);
            for (int t = 1; t < R->ntex; t++) {
                char nm[8]; snprintf(nm, sizeof nm, "ua_t%d", t);
                GLint loc = N(glGetUniformLocation)(p->twin, nm), unit = -1, b = -1, sb = -1, cur = 0;
                if (loc >= 0) N(glGetUniformiv)(p->twin, loc, &unit);
                N(glGetIntegerv)(GL_CURRENT_PROGRAM, &cur);
                N(glActiveTexture)(GL_TEXTURE0 + (GLenum)g_xunit[t]); N(glGetIntegerv)(GL_TEXTURE_BINDING_2D, &b); N(glGetIntegerv)(GL_SAMPLER_BINDING, &sb);
                N(glActiveTexture)((GLenum)act);
                fprintf(stderr, "[glmerge] check:   %s: unit %d, the host bound tex%d (sampler %d) on unit %d for tex%u; program bound %d; main unit %d holds tex%u\n",
                        nm, unit, b, sb, g_xunit[t], R->tex[t], cur, umain, L->tmain);
            }
        }
        if (g_ck_diff <= 20)
            fprintf(stderr, "[glmerge] check: a run of %zu draws (program %u, %d textures) differs in %lu pixels; first at (%ld, %ld): %08x merged, %08x the game's\n",
                    R->n, p->name, R->ntex, d, first % vp[2], first / vp[2], a[first], b[first]);
    }
    emit_restore(R);
    return 1;
}

// each op's kind for the replay's uniform following: 1 glUniform*, 2 glUniform*v / Matrix (count a[1]),
// 3 glProgramUniform*, 4 glProgramUniform*v (count a[2]), 5 glUseProgram
static unsigned char g_ukind[OP_N];
static int g_ukind_made;
static void ukind_make(void)
{
    for (unsigned op = 0; op < OP_N; op++) {
        const char *n = glrec_name[op];
        size_t l = strlen(n);
        int v = l > 0 && n[l - 1] == 'v';
        if (!strncmp(n, "glUniformBlockBinding", 21)) continue;
        if (!strncmp(n, "glProgramUniform", 16)) g_ukind[op] = v ? 4 : 3;
        else if (!strncmp(n, "glUniform", 9)) g_ukind[op] = v ? 2 : 1;
    }
    g_ukind[OP_glUseProgram] = 5;
    g_ukind_made = 1;
}
static void dirty_mark(GLuint prog, GLint loc, GLsizei count)
{
    if (!prog || loc < 0) return;
    prog_t *p = prg_fast(prog);
    if (!p || !p->ok) return;
    for (GLsizei c = 0; c < count && loc + c < PD_LOCS; c++) p->dirty[(loc + c) >> 5] |= 1u << ((loc + c) & 31);
}

static void frame_reset(void)
{
    st_reset();
    g_nwr = 0; g_nmaps = 0; g_poison_w = 0;
    if (++g_wgen == 0) { memset(g_wh, 0, sizeof g_wh); g_wgen = 1; }
    g_nc = 0; g_nrun = 0; g_nup = 0; g_nskip = 0; g_rn = 0; g_poison = 0;
}

void glmerge_submit(fg_frame *f)
{
    if (g_runs_off) {
        for (size_t i = 0; i < f->ncmd;) {
            uint64_t h = f->cmd[i];
            unsigned n = (unsigned)(h >> 32);
            f->call((unsigned)(h & 0xffffffffu), f->cmd + i + 1);
            i += n + 1;
        }
        g_frames++;
        return;
    }
    double t0 = 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); t0 = ts.tv_sec + ts.tv_nsec / 1e9;
    pthread_mutex_lock(&g_lock);                 // (the render thread's links change the programs' entries)
    frame_reset();
    g_fcmd = f->cmd;
    N(glGetIntegerv)(GL_ACTIVE_TEXTURE, &g_start_active); g_start_active -= GL_TEXTURE0;
    { GLint v = 0; N(glGetIntegerv)(GL_VERTEX_ARRAY_BINDING, &v); g_start_vao = (GLuint)v; }
    static unsigned long seen;
    int vb = g_verbose > 0 && seen++ >= (unsigned long)g_verbose_skip && g_vframe < g_verbose;
    // the scan
    for (size_t i = 0; i < f->ncmd;) {
        uint64_t h = f->cmd[i];
        unsigned op = (unsigned)(h & 0xffffffffu), n = (unsigned)(h >> 32);
        g_cur_end = -1;
        size_t before = g_nrun;
        walk(i, op, f->cmd + i + 1, f);
        if (vb && g_cur_end >= 0 && g_cur_end != E_DRAW && glrec_name[op])
            fprintf(stderr, "[glmerge] frame %d: a run ended at %s (%s)%s\n", g_vframe, glrec_name[op], g_end_name[g_cur_end],
                    g_nrun > before ? ", merged" : "");
        i += n + 1;
    }
    run_close(E_END);
    if (g_vao_bound_q) { N(glBindVertexArray)(g_start_vao); g_vao_bound_q = 0; }
    clock_gettime(CLOCK_MONOTONIC, &ts); g_scan_s += ts.tv_sec + ts.tv_nsec / 1e9 - t0;
    if (vb) { fprintf(stderr, "[glmerge] frame %d: %zu runs of %zu draws\n", g_vframe, g_nrun, g_nskip + g_nrun); g_vframe++; }
    // the merged geometry, then the frame with each run's draws made at its last
    if (g_nrun) {
        if (!g_mb) N(glGenBuffers)(1, &g_mb);
        N(glGetIntegerv)(GL_COPY_WRITE_BUFFER_BINDING, &g_cw_start);
        N(glBindBuffer)(GL_COPY_WRITE_BUFFER, g_mb);
        N(glBufferData)(GL_COPY_WRITE_BUFFER, (GLsizeiptr)g_nup, g_up, GL_STREAM_DRAW);
        N(glBindBuffer)(GL_COPY_WRITE_BUFFER, (GLuint)g_cw_start);
    }
    if (!g_ukind_made) ukind_make();
    GLint rp = 0;
    N(glGetIntegerv)(GL_CURRENT_PROGRAM, &rp);                    // (the frame's start's: nothing replayed yet)
    GLuint rprog = (GLuint)rp;
    size_t si = 0, ri = 0;
    for (size_t i = 0; i < f->ncmd;) {
        uint64_t h = f->cmd[i];
        unsigned op = (unsigned)(h & 0xffffffffu), n = (unsigned)(h >> 32);
        const uint64_t *a = f->cmd + i + 1;
        switch (g_ukind[op]) {
        case 1: dirty_mark(rprog, I(0), 1); break;
        case 2: dirty_mark(rprog, I(0), I(1)); break;
        case 3: dirty_mark(U(0), I(1), 1); break;
        case 4: dirty_mark(U(0), I(1), I(2)); break;
        case 5: rprog = U(0); break;
        }
        if (si < g_nskip && g_skip[si] == i) si++;
        else if (ri < g_nrun && g_c[g_run[ri].first + g_run[ri].n - 1].at == i) {
            if (!(g_check && g_frames % (unsigned long)g_check_every == 0 && emit_checked(&g_run[ri]))) { emit(&g_run[ri]); emit_restore(&g_run[ri]); }
            free(g_run[ri].pd); g_run[ri].pd = NULL; ri++;
        }
        else {
            if (op == OP_glBindTexture || op == OP_glBindSampler || op == OP_glDeleteTextures || op == OP_glDeleteSamplers) g_xknown = 0;
            f->call(op, f->cmd + i + 1);
        }
        i += n + 1;
    }
    for (; ri < g_nrun; ri++) { free(g_run[ri].pd); g_run[ri].pd = NULL; }
    pthread_mutex_unlock(&g_lock);
    g_frames++;
}

void glmerge_log(void)
{
    if (!g_on) return;
    if (!g_frames) {
        fprintf(stderr, "[glmerge] shaders: %lu vertex and %lu fragment shaders rewritten (%lu and %lu left as they are), %lu programs merge, %lu linked again\n",
                g_rewritten_vs, g_rewritten_fs, g_skipped_vs, g_skipped_fs, g_progs_ok, g_relinks);
        return;
    }
    double fr = (double)g_frames;
    unsigned long made = g_draws - g_merged_draws + g_runs;
    fprintf(stderr, "[glmerge]   the scan asked the driver a frame: %.1f uniforms, %.1f texture bindings, %.1f vertex attributes\n",
            g_q_uni / fr, g_q_tex / fr, g_q_attr / fr);
    g_q_uni = g_q_tex = g_q_attr = 0;
    fprintf(stderr, "[glmerge] %lu frames: %.1f draws a frame made as %.1f (%.1f runs of %.1f); the scan %.2f ms a frame; "
                    "shaders %lu + %lu rewritten, %lu programs merge, %lu linked again\n",
            g_frames, g_draws / fr, made / fr, g_runs / fr, g_runs ? (double)g_merged_draws / g_runs : 0, g_scan_s * 1000 / fr,
            g_rewritten_vs, g_rewritten_fs, g_progs_ok, g_relinks);
    if (g_synced) { fprintf(stderr, "[glmerge]   the game's uniforms copied to twins: %.1f a frame\n", g_synced / fr); g_synced = 0; }
    fprintf(stderr, "[glmerge]   runs ended by:");
    for (int i = 0; i < 12; i++) if (g_end[i]) fprintf(stderr, " %s %.1f;", g_end_name[i], g_end[i] / fr);
    fprintf(stderr, "%s\n", g_sel_set ? " (the game set ua_v's attribute: merging off)" : "");
    if (g_check) {
        fprintf(stderr, "[glmerge] check: %lu runs made twice, %lu differ (%lu pixels); %lu left unchecked; by textures:", g_ck_runs, g_ck_diff, g_ck_px, g_ck_skip);
        for (int t = 1; t <= NTEX; t++) fprintf(stderr, " %d: %lu the same, %lu differ;", t, g_ck_bytex[t][0], g_ck_bytex[t][1]);
        fprintf(stderr, "\n");
    }
    g_frames = g_draws = g_merged_draws = g_runs = 0; g_scan_s = 0; memset(g_end, 0, sizeof g_end);
}
