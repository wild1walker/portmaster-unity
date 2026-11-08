// Programs specialized on a uniform's value (UA_GL_SPEC=1|check; off by default).
//
// Unity's built-in lit shaders (Sprites/Diffuse, the surface shaders' ForwardBase pass) light every
// fragment with the main directional light, _LightColor0, a plain uniform. A 2D game that tints its
// scenes with ambient light only has no such light: Unity sets _LightColor0 to 0 and every fragment
// still does the light's arithmetic, multiplied by zero (Hollow Knight: its lit sprites shade 37 % of
// Greenpath's fragments; Mali-G31 offline compiler: 5.10 arithmetic cycles a fragment, 1.38 with the
// uniform a constant 0).
//
// So for each fragment shader whose source declares `uniform vec4 _LightColor0;` (outside a uniform
// block) a twin is compiled with the declaration replaced by `const vec4 _LightColor0 = vec4(0.0);`
// and linked with the same vertex shader and attribute locations: the compiler folds the light away.
// The twin is bound instead of the program while the game's value of the uniform is 0 in every
// component the shader reads; any other value binds the program again. The two give the same bits:
// the light's term is a finite value times 0 (+0 or -0), added to the ambient term, which leaves it
// unchanged.
//
// The game never sees the twin: glUseProgram(P) binds it in P's place, glGetIntegerv(GL_CURRENT_PROGRAM)
// reads P, every uniform the game sets on P is set on the twin too (its own location for the same
// name), and P's uniform block bindings with it. Unity's program binary cache keeps both: while this is
// on, the binary of P that glGetProgramBinary hands Unity carries the twin's binary after it (a header
// "UASP"), and glProgramBinary splits it again; a binary without the header loads as it is, without a
// twin. So that Unity's cache holds one kind or the other: when UA_GL_SPEC is turned on for a game's
// data (no data/cache/uaspec.on yet), or off again (the file still there), ua_host empties
// data/cache/UnityShaderCache once before the player starts: Unity compiles each program from its
// source once more and caches it anew (Hollow Knight: about 2.6 s of compiles spread over the
// first loads).
//
// UA_GL_SPEC=check: as 1, and the first draws of each twin are also made with the program, both into
// scratch textures, and the pictures compared byte for byte ("[glspec] check: <n> draws compared, <n>
// differ").
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64           // (readdir on 32-bit: large inode numbers)
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "glspec.h"

typedef unsigned GLenum; typedef unsigned GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned char GLboolean;
typedef unsigned GLbitfield;

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_SHADER_TYPE 0x8B4F
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_ACTIVE_UNIFORM_BLOCKS 0x8A36
#define GL_UNIFORM_BLOCK_INDEX 0x8A3A
#define GL_UNIFORM_BLOCK_BINDING 0x8A3F
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#define GL_INFO_LOG_LENGTH 0x8B84

static int g_on, g_check;
static long g_compared, g_differ;    // (check mode)
static long g_twins, g_twin_fail, g_binaries, g_stale, g_rebinds, g_twin_binds;

// the next layer's functions (the calls the game's are passed to)
#define F(ret, name, args) static ret (*r_##name) args;
F(void, ShaderSource, (GLuint, GLsizei, const char *const *, const GLint *)) F(void, LinkProgram, (GLuint))
F(void, UseProgram, (GLuint)) F(void, DeleteProgram, (GLuint)) F(void, GetIntegerv, (GLenum, GLint *))
F(void, GetProgramiv, (GLuint, GLenum, GLint *)) F(void, ProgramBinary, (GLuint, GLenum, const void *, GLsizei))
F(void, GetProgramBinary, (GLuint, GLsizei, GLsizei *, GLenum *, void *))
F(void, UniformBlockBinding, (GLuint, GLuint, GLuint))
F(void, Uniform1fv, (GLint, GLsizei, const float *)) F(void, Uniform2fv, (GLint, GLsizei, const float *))
F(void, Uniform3fv, (GLint, GLsizei, const float *)) F(void, Uniform4fv, (GLint, GLsizei, const float *))
F(void, Uniform1iv, (GLint, GLsizei, const GLint *)) F(void, Uniform2iv, (GLint, GLsizei, const GLint *))
F(void, Uniform3iv, (GLint, GLsizei, const GLint *)) F(void, Uniform4iv, (GLint, GLsizei, const GLint *))
F(void, Uniform1uiv, (GLint, GLsizei, const GLuint *)) F(void, Uniform2uiv, (GLint, GLsizei, const GLuint *))
F(void, Uniform3uiv, (GLint, GLsizei, const GLuint *)) F(void, Uniform4uiv, (GLint, GLsizei, const GLuint *))
F(void, Uniform1f, (GLint, float)) F(void, Uniform2f, (GLint, float, float)) F(void, Uniform3f, (GLint, float, float, float))
F(void, Uniform4f, (GLint, float, float, float, float))
F(void, Uniform1i, (GLint, GLint)) F(void, Uniform2i, (GLint, GLint, GLint)) F(void, Uniform3i, (GLint, GLint, GLint, GLint))
F(void, Uniform4i, (GLint, GLint, GLint, GLint, GLint))
F(void, UniformMatrix2fv, (GLint, GLsizei, GLboolean, const float *)) F(void, UniformMatrix3fv, (GLint, GLsizei, GLboolean, const float *))
F(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const float *))
F(void, UniformMatrix2x3fv, (GLint, GLsizei, GLboolean, const float *)) F(void, UniformMatrix3x2fv, (GLint, GLsizei, GLboolean, const float *))
F(void, UniformMatrix2x4fv, (GLint, GLsizei, GLboolean, const float *)) F(void, UniformMatrix4x2fv, (GLint, GLsizei, GLboolean, const float *))
F(void, UniformMatrix3x4fv, (GLint, GLsizei, GLboolean, const float *)) F(void, UniformMatrix4x3fv, (GLint, GLsizei, GLboolean, const float *))
F(void, ProgramUniform4fv, (GLuint, GLint, GLsizei, const float *))
F(void, DrawElements, (GLenum, GLsizei, GLenum, const void *)) F(void, DrawArrays, (GLenum, GLint, GLsizei))
F(void, DrawElementsBaseVertexOES, (GLenum, GLsizei, GLenum, const void *, GLint))
#undef F
// direct driver calls (queries, the twin's own setup)
#define F(ret, name, args) static ret (*q_##name) args;
F(GLuint, CreateShader, (GLenum)) F(void, CompileShader, (GLuint)) F(void, GetShaderiv, (GLuint, GLenum, GLint *))
F(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, char *)) F(void, DeleteShader, (GLuint)) F(GLuint, CreateProgram, (void))
F(void, AttachShader, (GLuint, GLuint)) F(void, DetachShader, (GLuint, GLuint)) F(void, GetAttachedShaders, (GLuint, GLsizei, GLsizei *, GLuint *))
F(void, BindAttribLocation, (GLuint, GLuint, const char *)) F(void, GetActiveAttrib, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *))
F(GLint, GetAttribLocation, (GLuint, const char *)) F(void, GetActiveUniform, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *))
F(void, GetActiveUniformsiv, (GLuint, GLsizei, const GLuint *, GLenum, GLint *)) F(GLint, GetUniformLocation, (GLuint, const char *))
F(void, GetActiveUniformBlockName, (GLuint, GLuint, GLsizei, GLsizei *, char *)) F(GLuint, GetUniformBlockIndex, (GLuint, const char *))
F(void, GetActiveUniformBlockiv, (GLuint, GLuint, GLenum, GLint *)) F(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *))
F(void, ProgramUniform1fv, (GLuint, GLint, GLsizei, const float *)) F(void, ProgramUniform2fv, (GLuint, GLint, GLsizei, const float *))
F(void, ProgramUniform3fv, (GLuint, GLint, GLsizei, const float *)) F(void, ProgramUniform4fv, (GLuint, GLint, GLsizei, const float *))
F(void, ProgramUniform1iv, (GLuint, GLint, GLsizei, const GLint *)) F(void, ProgramUniform2iv, (GLuint, GLint, GLsizei, const GLint *))
F(void, ProgramUniform3iv, (GLuint, GLint, GLsizei, const GLint *)) F(void, ProgramUniform4iv, (GLuint, GLint, GLsizei, const GLint *))
F(void, ProgramUniform1uiv, (GLuint, GLint, GLsizei, const GLuint *)) F(void, ProgramUniform2uiv, (GLuint, GLint, GLsizei, const GLuint *))
F(void, ProgramUniform3uiv, (GLuint, GLint, GLsizei, const GLuint *)) F(void, ProgramUniform4uiv, (GLuint, GLint, GLsizei, const GLuint *))
F(void, ProgramUniformMatrix2fv, (GLuint, GLint, GLsizei, GLboolean, const float *))
F(void, ProgramUniformMatrix3fv, (GLuint, GLint, GLsizei, GLboolean, const float *))
F(void, ProgramUniformMatrix4fv, (GLuint, GLint, GLsizei, GLboolean, const float *))
F(void, ProgramUniformMatrix2x3fv, (GLuint, GLint, GLsizei, GLboolean, const float *))
F(void, ProgramUniformMatrix3x2fv, (GLuint, GLint, GLsizei, GLboolean, const float *))
F(void, ProgramUniformMatrix2x4fv, (GLuint, GLint, GLsizei, GLboolean, const float *))
F(void, ProgramUniformMatrix4x2fv, (GLuint, GLint, GLsizei, GLboolean, const float *))
F(void, ProgramUniformMatrix3x4fv, (GLuint, GLint, GLsizei, GLboolean, const float *))
F(void, ProgramUniformMatrix4x3fv, (GLuint, GLint, GLsizei, GLboolean, const float *))
F(void, GetProgramBinaryQ, (GLuint, GLsizei, GLsizei *, GLenum *, void *))
F(void, ProgramBinaryQ, (GLuint, GLenum, const void *, GLsizei))
F(void, GetProgramivQ, (GLuint, GLenum, GLint *))
// (check mode)
F(void, GenFramebuffers, (GLsizei, GLuint *)) F(void, GenTextures, (GLsizei, GLuint *)) F(void, BindTexture, (GLenum, GLuint))
F(void, TexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) F(void, DeleteTextures, (GLsizei, const GLuint *))
F(void, BindFramebuffer, (GLenum, GLuint)) F(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
F(GLboolean, IsEnabled, (GLenum)) F(void, Enable, (GLenum)) F(void, Disable, (GLenum))
F(void, ClearBufferfv, (GLenum, GLint, const float *)) F(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))
F(void, GetBooleanv, (GLenum, GLboolean *)) F(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))
F(void, PixelStorei, (GLenum, GLint)) F(void, BindBuffer, (GLenum, GLuint)) F(void, UseProgramQ, (GLuint))
F(void, GetIntegerv_, (GLenum, GLint *))
#undef F

// ---- the rule: _LightColor0 a constant 0
static const char RULE_NAME[] = "_LightColor0";

// the source with the rule's declaration replaced; *used: the components the shader reads (bits xyzw)
static char *twin_source(const char *src, unsigned *used)
{
    const char *d = NULL;
    for (const char *p = src; (p = strstr(p, RULE_NAME)); p += sizeof RULE_NAME - 1) {
        const char *line = p;
        while (line > src && line[-1] != '\n') line--;
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        // `uniform [precision] vec4 _LightColor0;` at the line's start: a plain uniform (in a block the
        // members are indented, after a `uniform <Block> {` line)
        if (!strncmp(line, "uniform", 7) && memmem(line, n, "vec4", 4) && p[sizeof RULE_NAME - 1] == ';') { d = line; break; }
    }
    if (!d) return NULL;
    // what of it the shader reads: every later use followed by a swizzle of x, y, z, w (or none: all)
    *used = 0;
    const char *after = strchr(d, '\n');
    if (!after) return NULL;
    for (const char *p = after; (p = strstr(p, RULE_NAME)); p += sizeof RULE_NAME - 1) {
        const char *q = p + sizeof RULE_NAME - 1;
        if (*q != '.') { *used = 15; break; }
        for (q++; *q == 'x' || *q == 'y' || *q == 'z' || *q == 'w' || *q == 'r' || *q == 'g' || *q == 'b' || *q == 'a'; q++)
            *used |= *q == 'x' || *q == 'r' ? 1 : *q == 'y' || *q == 'g' ? 2 : *q == 'z' || *q == 'b' ? 4 : 8;
    }
    if (!*used) return NULL;                        // (declared, never read: nothing to gain)
    size_t pre = (size_t)(d - src), n = strlen(src);
    char *s = malloc(n + 64);
    if (!s) return NULL;
    memcpy(s, src, pre);
    size_t o = pre + (size_t)sprintf(s + pre, "const vec4 %s = vec4(0.0);", RULE_NAME);
    memcpy(s + o, after, strlen(after) + 1);
    return s;
}

// ---- programs
#define MAX_SH 65536
#define MAX_PROG 65536
#define MAX_LOC 4096
typedef struct {
    GLuint twin;
    GLint loc;                   // the program's location of the rule's uniform (-1: none)
    unsigned used;
    float val[4];                // its value, as the game set it (0 after a link)
    short *map;                  // the program's location -> the twin's (-1: none), MAX_LOC entries
    GLuint *blk; int nblk;       // the program's uniform block i -> the twin's
    int checked;                 // (check mode) draws compared
} prog_t;
static char **g_src;             // fragment shader -> its twin's source
static unsigned char *g_used;    // ... and the components of the uniform it reads
static prog_t **g_prog;
static GLuint g_game;            // the program the game bound
static GLuint g_bound;           // the one bound (the twin in its place)
static pthread_t g_thread;
static int g_have_thread;

static int match(const prog_t *p)
{
    for (int i = 0; i < 4; i++) if ((p->used & (1u << i)) && p->val[i] != 0.0f) return 0;
    return 1;
}
static void bind(void)
{
    prog_t *p = g_game < MAX_PROG ? g_prog[g_game] : NULL;
    GLuint want = p && p->twin && match(p) ? p->twin : g_game;
    if (want != g_bound) { r_UseProgram(want); g_bound = want; if (want != g_game) g_twin_binds++; }
}

static void drop(GLuint prog)
{
    prog_t *p = prog < MAX_PROG ? g_prog[prog] : NULL;
    if (!p) return;
    if (p->twin) r_DeleteProgram(p->twin);
    free(p->map); free(p->blk); free(p);
    g_prog[prog] = NULL;
}

// the twin linked (from source or binary): its locations and blocks against the program's
static void maps(GLuint prog, prog_t *p)
{
    p->map = malloc(MAX_LOC * sizeof *p->map);
    if (!p->map) return;
    memset(p->map, 0xff, MAX_LOC * sizeof *p->map);
    GLint nu = 0;
    q_GetProgramivQ(prog, GL_ACTIVE_UNIFORMS, &nu);
    for (GLint i = 0; i < nu; i++) {
        char name[160]; GLsizei l = 0; GLint sz = 0; GLenum t = 0; GLuint idx = (GLuint)i; GLint blk = -1;
        q_GetActiveUniform(prog, idx, sizeof name - 8, &l, &sz, &t, name);
        q_GetActiveUniformsiv(prog, 1, &idx, GL_UNIFORM_BLOCK_INDEX, &blk);
        if (blk >= 0) continue;
        char *br = strchr(name, '[');
        if (br) *br = 0;
        for (GLint e = 0; e < sz; e++) {
            char el[176];
            if (sz > 1) snprintf(el, sizeof el, "%s[%d]", name, e); else snprintf(el, sizeof el, "%s", name);
            GLint lo = q_GetUniformLocation(prog, el);
            if (lo >= 0 && lo < MAX_LOC) p->map[lo] = (short)q_GetUniformLocation(p->twin, el);
        }
    }
    p->loc = q_GetUniformLocation(prog, RULE_NAME);
    GLint nb = 0;
    q_GetProgramivQ(prog, GL_ACTIVE_UNIFORM_BLOCKS, &nb);
    if (nb > 0 && (p->blk = calloc((size_t)nb, sizeof *p->blk))) {
        p->nblk = nb;
        for (GLint i = 0; i < nb; i++) {
            char name[160]; GLsizei l = 0; GLint b = 0;
            q_GetActiveUniformBlockName(prog, (GLuint)i, sizeof name, &l, name);
            p->blk[i] = q_GetUniformBlockIndex(p->twin, name);
            q_GetActiveUniformBlockiv(prog, (GLuint)i, GL_UNIFORM_BLOCK_BINDING, &b);
            if (p->blk[i] != 0xFFFFFFFFu) r_UniformBlockBinding(p->twin, p->blk[i], (GLuint)b);
        }
    }
}

static void make_twin(GLuint prog)
{
    GLint ok = 0, n = 0;
    q_GetProgramivQ(prog, GL_LINK_STATUS, &ok);
    if (!ok) return;
    GLuint sh[8];
    q_GetAttachedShaders(prog, 8, &n, sh);
    GLuint vs = 0; const char *fs = NULL;
    for (GLint i = 0; i < n; i++) {
        GLint t = 0;
        q_GetShaderiv(sh[i], GL_SHADER_TYPE, &t);
        if (t == GL_FRAGMENT_SHADER) fs = sh[i] < MAX_SH ? g_src[sh[i]] : NULL; else vs = sh[i];
    }
    if (!fs || !vs) return;
    prog_t *p = calloc(1, sizeof *p);
    if (!p) return;
    for (GLint i = 0; i < n; i++) if (sh[i] < MAX_SH && g_src[sh[i]] == fs) p->used = g_used[sh[i]];
    GLuint f = q_CreateShader(GL_FRAGMENT_SHADER);
    const char *s = fs;
    r_ShaderSource(f, 1, &s, NULL);
    q_CompileShader(f);
    GLint st = 0;
    q_GetShaderiv(f, GL_COMPILE_STATUS, &st);
    if (!st) {
        char log[300] = ""; GLsizei l = 0;
        q_GetShaderInfoLog(f, sizeof log, &l, log);
        if (g_twin_fail++ < 5) fprintf(stderr, "[glspec] program %u: its twin does not compile: %s\n", prog, log);
        q_DeleteShader(f); free(p);
        return;
    }
    GLuint t = q_CreateProgram();
    q_AttachShader(t, vs); q_AttachShader(t, f);
    GLint na = 0;
    q_GetProgramivQ(prog, GL_ACTIVE_ATTRIBUTES, &na);
    for (GLint i = 0; i < na; i++) {
        char name[128]; GLsizei l = 0; GLint sz = 0; GLenum ty = 0;
        q_GetActiveAttrib(prog, (GLuint)i, sizeof name, &l, &sz, &ty, name);
        GLint loc = q_GetAttribLocation(prog, name);
        if (loc >= 0) q_BindAttribLocation(t, (GLuint)loc, name);
    }
    r_LinkProgram(t);
    q_DetachShader(t, vs); q_DetachShader(t, f); q_DeleteShader(f);
    q_GetProgramivQ(t, GL_LINK_STATUS, &st);
    if (!st) {
        char log[300] = ""; GLsizei l = 0;
        q_GetProgramInfoLog(t, sizeof log, &l, log);
        if (g_twin_fail++ < 5) fprintf(stderr, "[glspec] program %u: its twin does not link: %s\n", prog, log);
        r_DeleteProgram(t); free(p);
        return;
    }
    drop(prog);
    p->twin = t;
    g_prog[prog] = p;
    maps(prog, p);
    g_twins++;
}

static int mine(void) { return !g_have_thread || pthread_equal(pthread_self(), g_thread); }
// Unity's context moved to the calling thread (glrec.c's UA_GL_REC=thread)
void glspec_rethread(void) { if (g_have_thread) g_thread = pthread_self(); }

// ---- wrappers: sources, links, binaries
static void w_ShaderSource(GLuint sh, GLsizei n, const char *const *str, const GLint *len)
{
    r_ShaderSource(sh, n, str, len);
    if (sh >= MAX_SH) return;
    GLint t = 0;
    q_GetShaderiv(sh, GL_SHADER_TYPE, &t);
    free(g_src[sh]); g_src[sh] = NULL;
    if (t != GL_FRAGMENT_SHADER) return;
    size_t total = 0;
    for (GLsizei i = 0; i < n; i++) total += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    char *all = malloc(total + 1);
    if (!all) return;
    size_t o = 0;
    for (GLsizei i = 0; i < n; i++) { size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]); memcpy(all + o, str[i], l); o += l; }
    all[o] = 0;
    unsigned used = 0;
    g_src[sh] = twin_source(all, &used);
    g_used[sh] = (unsigned char)used;
    free(all);
}
static void w_LinkProgram(GLuint p)
{
    r_LinkProgram(p);
    if (p < MAX_PROG) { drop(p); make_twin(p); if (p == g_game) { g_bound = 0; bind(); } }
}
static void w_DeleteProgram(GLuint p)
{
    if (p == g_game && g_bound != p && mine()) { r_UseProgram(p); g_bound = p; }   // (so the game's deletion takes)
    drop(p);
    r_DeleteProgram(p);
}
static void w_UseProgram(GLuint p) { if (!mine()) { r_UseProgram(p); return; } g_game = p; bind(); }
static void w_GetIntegerv(GLenum e, GLint *v)
{
    r_GetIntegerv(e, v);
    if (e == GL_CURRENT_PROGRAM && mine() && g_bound != g_game && (GLuint)*v == g_bound) *v = (GLint)g_game;
}

// Unity's program binary cache: the twin's binary after the program's
#define MAGIC 0x50534155u           // "UASP"
typedef struct { unsigned magic, version, fmt, len, tfmt, tlen; } blob_t;
static GLint twin_len(prog_t *p, GLint *tlen)
{
    *tlen = 0;
    if (p && p->twin) q_GetProgramivQ(p->twin, GL_PROGRAM_BINARY_LENGTH, tlen);
    return *tlen;
}
static void w_GetProgramiv(GLuint prog, GLenum e, GLint *v)
{
    r_GetProgramiv(prog, e, v);
    if (e != GL_PROGRAM_BINARY_LENGTH || prog >= MAX_PROG || *v <= 0) return;
    GLint t;
    twin_len(g_prog[prog], &t);
    *v += (GLint)sizeof(blob_t) + t;
}
static void w_GetProgramBinary(GLuint prog, GLsizei size, GLsizei *len, GLenum *fmt, void *b)
{
    GLint plen = 0, tlen = 0;
    q_GetProgramivQ(prog, GL_PROGRAM_BINARY_LENGTH, &plen);
    prog_t *p = prog < MAX_PROG ? g_prog[prog] : NULL;
    twin_len(p, &tlen);
    GLsizei need = (GLsizei)sizeof(blob_t) + plen + tlen, got = 0, tgot = 0;
    if (!b || size < need) { if (len) *len = 0; return; }
    blob_t h = { MAGIC, 1, 0, 0, 0, 0 };
    unsigned char *o = b;
    r_GetProgramBinary(prog, plen, &got, &h.fmt, o + sizeof h);
    h.len = (unsigned)got;
    if (tlen) { q_GetProgramBinaryQ(p->twin, tlen, &tgot, &h.tfmt, o + sizeof h + got); h.tlen = (unsigned)tgot; }
    memcpy(o, &h, sizeof h);
    if (len) *len = (GLsizei)(sizeof h + got + tgot);
    if (fmt) *fmt = h.fmt;
}
static void w_ProgramBinary(GLuint prog, GLenum fmt, const void *b, GLsizei len)
{
    blob_t h;
    if (len < (GLsizei)sizeof h || (memcpy(&h, b, sizeof h), h.magic != MAGIC || h.version != 1 ||
                                    (GLsizei)(sizeof h + h.len + h.tlen) > len || h.fmt != fmt)) {
        r_ProgramBinary(prog, fmt, b, len);     // (the driver's own: no twin)
        if (prog < MAX_PROG) drop(prog);
        g_stale++;
        return;
    }
    const unsigned char *c = b;
    r_ProgramBinary(prog, h.fmt, c + sizeof h, (GLsizei)h.len);
    if (prog >= MAX_PROG) return;
    drop(prog);
    GLint ok = 0;
    q_GetProgramivQ(prog, GL_LINK_STATUS, &ok);
    if (!ok || !h.tlen) return;
    GLuint t = q_CreateProgram();
    q_ProgramBinaryQ(t, h.tfmt, c + sizeof h + h.len, (GLsizei)h.tlen);
    q_GetProgramivQ(t, GL_LINK_STATUS, &ok);
    if (!ok) { r_DeleteProgram(t); return; }
    prog_t *p = calloc(1, sizeof *p);
    if (!p) { r_DeleteProgram(t); return; }
    p->twin = t;
    p->used = 15;                // (the source is not here: every component must be 0)
    g_prog[prog] = p;
    maps(prog, p);
    g_binaries++;
    if (prog == g_game) { g_bound = 0; bind(); }
}
static void w_UniformBlockBinding(GLuint prog, GLuint idx, GLuint b)
{
    r_UniformBlockBinding(prog, idx, b);
    prog_t *p = prog < MAX_PROG ? g_prog[prog] : NULL;
    if (p && (GLint)idx < p->nblk && p->blk[idx] != 0xFFFFFFFFu) r_UniformBlockBinding(p->twin, p->blk[idx], b);
}

// ---- uniforms: set on the program the game bound (whichever is bound) and on its twin
static prog_t *cur_spec(void) { return mine() && g_game && g_game < MAX_PROG ? g_prog[g_game] : NULL; }
static GLint twin_loc(prog_t *p, GLint loc) { return loc >= 0 && loc < MAX_LOC ? p->map[loc] : -1; }
static void rule_set(prog_t *p, GLint loc, const float *v, int comps)
{
    if (loc != p->loc || p->loc < 0) return;
    int was = match(p);
    for (int i = 0; i < 4; i++) p->val[i] = i < comps ? v[i] : 0.0f;
    if (match(p) != was) { g_rebinds++; bind(); }
}
#define UNI_V(N, T, fn) \
    static void w_Uniform##N(GLint loc, GLsizei n, const T *v) \
    { \
        prog_t *p = cur_spec(); \
        if (!p) { r_Uniform##N(loc, n, v); return; } \
        q_ProgramUniform##N(g_game, loc, n, v); \
        GLint t = twin_loc(p, loc); \
        if (t >= 0) q_ProgramUniform##N(p->twin, t, n, v); \
        fn; \
    }
UNI_V(1fv, float, rule_set(p, loc, v, 1)) UNI_V(2fv, float, rule_set(p, loc, v, 2)) UNI_V(3fv, float, rule_set(p, loc, v, 3))
UNI_V(4fv, float, rule_set(p, loc, v, 4))
UNI_V(1iv, GLint, (void)0) UNI_V(2iv, GLint, (void)0) UNI_V(3iv, GLint, (void)0) UNI_V(4iv, GLint, (void)0)
UNI_V(1uiv, GLuint, (void)0) UNI_V(2uiv, GLuint, (void)0) UNI_V(3uiv, GLuint, (void)0) UNI_V(4uiv, GLuint, (void)0)
#undef UNI_V
#define UNI_M(N) \
    static void w_UniformMatrix##N(GLint loc, GLsizei n, GLboolean tr, const float *v) \
    { \
        prog_t *p = cur_spec(); \
        if (!p) { r_UniformMatrix##N(loc, n, tr, v); return; } \
        q_ProgramUniformMatrix##N(g_game, loc, n, tr, v); \
        GLint t = twin_loc(p, loc); \
        if (t >= 0) q_ProgramUniformMatrix##N(p->twin, t, n, tr, v); \
    }
UNI_M(2fv) UNI_M(3fv) UNI_M(4fv) UNI_M(2x3fv) UNI_M(3x2fv) UNI_M(2x4fv) UNI_M(4x2fv) UNI_M(3x4fv) UNI_M(4x3fv)
#undef UNI_M
// (the scalar float ones take floats: the player calls them with softfp on armhf, SO_ABI)
static void h_Uniform4f(GLint loc, float a, float b, float c, float d)
{
    prog_t *p = cur_spec();
    if (!p) { r_Uniform4f(loc, a, b, c, d); return; }
    float v[4] = { a, b, c, d };
    w_Uniform4fv(loc, 1, v);
}
static SO_ABI void w_Uniform4f(GLint loc, float a, float b, float c, float d) { h_Uniform4f(loc, a, b, c, d); }
static void h_Uniform3f(GLint loc, float a, float b, float c)
{ prog_t *p = cur_spec(); if (!p) { r_Uniform3f(loc, a, b, c); return; } float v[3] = { a, b, c }; w_Uniform3fv(loc, 1, v); }
static SO_ABI void w_Uniform3f(GLint loc, float a, float b, float c) { h_Uniform3f(loc, a, b, c); }
static void h_Uniform2f(GLint loc, float a, float b)
{ prog_t *p = cur_spec(); if (!p) { r_Uniform2f(loc, a, b); return; } float v[2] = { a, b }; w_Uniform2fv(loc, 1, v); }
static SO_ABI void w_Uniform2f(GLint loc, float a, float b) { h_Uniform2f(loc, a, b); }
static void h_Uniform1f(GLint loc, float a)
{ prog_t *p = cur_spec(); if (!p) { r_Uniform1f(loc, a); return; } w_Uniform1fv(loc, 1, &a); }
static SO_ABI void w_Uniform1f(GLint loc, float a) { h_Uniform1f(loc, a); }
static void w_Uniform1i(GLint loc, GLint a) { prog_t *p = cur_spec(); if (!p) { r_Uniform1i(loc, a); return; } w_Uniform1iv(loc, 1, &a); }
static void w_Uniform2i(GLint loc, GLint a, GLint b)
{ prog_t *p = cur_spec(); if (!p) { r_Uniform2i(loc, a, b); return; } GLint v[2] = { a, b }; w_Uniform2iv(loc, 1, v); }
static void w_Uniform3i(GLint loc, GLint a, GLint b, GLint c)
{ prog_t *p = cur_spec(); if (!p) { r_Uniform3i(loc, a, b, c); return; } GLint v[3] = { a, b, c }; w_Uniform3iv(loc, 1, v); }
static void w_Uniform4i(GLint loc, GLint a, GLint b, GLint c, GLint d)
{ prog_t *p = cur_spec(); if (!p) { r_Uniform4i(loc, a, b, c, d); return; } GLint v[4] = { a, b, c, d }; w_Uniform4iv(loc, 1, v); }
// (a game's own glProgramUniform4fv on a program with a twin: the twin too; the other forms are left
// to the program alone, which is safe only while games do not use them: Unity's GLES backend does not)
static void w_ProgramUniform4fv(GLuint prog, GLint loc, GLsizei n, const float *v)
{
    r_ProgramUniform4fv(prog, loc, n, v);
    prog_t *p = prog < MAX_PROG ? g_prog[prog] : NULL;
    if (!p) return;
    GLint t = twin_loc(p, loc);
    if (t >= 0) q_ProgramUniform4fv(p->twin, t, n, v);
    if (loc == p->loc) {
        int was = match(p);
        for (int i = 0; i < 4; i++) p->val[i] = v[i];
        if (match(p) != was && prog == g_game && mine()) { g_rebinds++; bind(); }
    }
}

// ---- UA_GL_SPEC=check: a twin's first draws also made with the program, both into scratch textures
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_RGBA8 0x8058
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_COLOR 0x1800
#define GL_VIEWPORT 0x0BA2
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PACK_ALIGNMENT 0x0D05
#define CHECK_DRAWS 32               // per program
static GLuint g_cfb, g_ctex[2];
static int g_cw, g_ch;
static unsigned char *g_cpix[2];

// draw() made with the twin and with the program into the two scratch textures, compared
static void check(void (*draw)(void *), void *arg)
{
    prog_t *p = cur_spec();
    if (!g_check || !p || g_bound != p->twin || p->checked >= CHECK_DRAWS) return;
    p->checked++;
    GLint vp[4], dfb = 0, rfb = 0, tex = 0, pbo = 0, al = 4;
    q_GetIntegerv_(GL_VIEWPORT, vp);
    int w = vp[0] + vp[2], h = vp[1] + vp[3];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    q_GetIntegerv_(GL_DRAW_FRAMEBUFFER_BINDING, &dfb); q_GetIntegerv_(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    q_GetIntegerv_(GL_TEXTURE_BINDING_2D, &tex); q_GetIntegerv_(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    q_GetIntegerv_(GL_PACK_ALIGNMENT, &al);
    if (!g_cfb) q_GenFramebuffers(1, &g_cfb);
    if (w > g_cw || h > g_ch) {
        if (g_ctex[0]) q_DeleteTextures(2, g_ctex);
        q_GenTextures(2, g_ctex);
        for (int i = 0; i < 2; i++) { q_BindTexture(GL_TEXTURE_2D, g_ctex[i]); q_TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h); }
        q_BindTexture(GL_TEXTURE_2D, (GLuint)tex);
        g_cw = w; g_ch = h;
        for (int i = 0; i < 2; i++) { free(g_cpix[i]); g_cpix[i] = malloc((size_t)w * h * 4); }
    }
    if (!g_cpix[0] || !g_cpix[1]) return;
    GLboolean dt = q_IsEnabled(GL_DEPTH_TEST), st = q_IsEnabled(GL_STENCIL_TEST), sc = q_IsEnabled(GL_SCISSOR_TEST), cm[4];
    q_GetBooleanv(GL_COLOR_WRITEMASK, cm);
    if (dt) q_Disable(GL_DEPTH_TEST);
    if (st) q_Disable(GL_STENCIL_TEST);
    if (pbo) q_BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    q_PixelStorei(GL_PACK_ALIGNMENT, 4);
    static const float zero[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 2; i++) {
        q_BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_cfb);
        q_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_ctex[i], 0);
        q_ColorMask(1, 1, 1, 1);
        if (sc) q_Disable(GL_SCISSOR_TEST);
        q_ClearBufferfv(GL_COLOR, 0, zero);
        if (sc) q_Enable(GL_SCISSOR_TEST);
        q_ColorMask(cm[0], cm[1], cm[2], cm[3]);
        q_UseProgramQ(i ? g_game : p->twin);
        draw(arg);
        q_BindFramebuffer(GL_READ_FRAMEBUFFER, g_cfb);
        q_ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_cpix[i]);
    }
    q_UseProgramQ(g_bound);
    q_BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dfb); q_BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    if (pbo) q_BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pbo);
    q_PixelStorei(GL_PACK_ALIGNMENT, al);
    if (dt) q_Enable(GL_DEPTH_TEST);
    if (st) q_Enable(GL_STENCIL_TEST);
    g_compared++;
    if (memcmp(g_cpix[0], g_cpix[1], (size_t)w * h * 4)) {
        size_t k = 0, n = (size_t)w * h * 4, bad = 0;
        for (size_t i = 0; i < n; i++) if (g_cpix[0][i] != g_cpix[1][i]) { if (!bad++) k = i; }
        if (g_differ++ < 8) fprintf(stderr, "[glspec] check: program %u's twin differs on %zu bytes (first at %zu: %d vs %d)\n",
                                    g_game, bad, k, g_cpix[0][k], g_cpix[1][k]);
    }
    if (g_compared % 50 == 0) fprintf(stderr, "[glspec] check: %ld draws compared, %ld differ\n", g_compared, g_differ);
}
typedef struct { GLenum m; GLsizei c; GLenum t; const void *i; GLint b, f; } drawargs;
static void d_elements(void *a) { drawargs *d = a; r_DrawElements(d->m, d->c, d->t, d->i); }
static void d_arrays(void *a) { drawargs *d = a; r_DrawArrays(d->m, d->f, d->c); }
static void d_bv(void *a) { drawargs *d = a; r_DrawElementsBaseVertexOES(d->m, d->c, d->t, d->i, d->b); }
static void w_DrawElements(GLenum m, GLsizei c, GLenum t, const void *i)
{ drawargs d = { m, c, t, i, 0, 0 }; check(d_elements, &d); r_DrawElements(m, c, t, i); }
static void w_DrawArrays(GLenum m, GLint f, GLsizei c) { drawargs d = { m, c, 0, NULL, 0, f }; check(d_arrays, &d); r_DrawArrays(m, f, c); }
static void w_DrawElementsBaseVertexOES(GLenum m, GLsizei c, GLenum t, const void *i, GLint b)
{ drawargs d = { m, c, t, i, b, 0 }; check(d_bv, &d); r_DrawElementsBaseVertexOES(m, c, t, i, b); }

void glspec_frame(void)
{
    if (!g_on) return;
    if (!g_have_thread) { g_thread = pthread_self(); g_have_thread = 1; }
    static long frames, every;
    if (!every) { const char *e = getenv("UA_GL_SPEC_LOG"); every = e && atol(e) > 0 ? atol(e) : 1800; }
    if (++frames % every == 0)
        fprintf(stderr, "[glspec] %ld frames: %ld twins made from source, %ld from Unity's cache (%ld cached without one), "
                        "%ld failed; twins bound %ld times, %ld switches by value%s\n", frames, g_twins, g_binaries, g_stale, g_twin_fail,
                g_twin_binds, g_rebinds, "");
    if (g_check && frames % every == 0) fprintf(stderr, "[glspec] check: %ld draws compared, %ld differ\n", g_compared, g_differ);
}

void *glspec_hard(const char *name)
{
    if (!g_on) return NULL;
    if (!strcmp(name, "glUniform1f")) return (void *)h_Uniform1f;
    if (!strcmp(name, "glUniform2f")) return (void *)h_Uniform2f;
    if (!strcmp(name, "glUniform3f")) return (void *)h_Uniform3f;
    if (!strcmp(name, "glUniform4f")) return (void *)h_Uniform4f;
    return NULL;
}

// UA_GL_SPEC turned on or off for this data: Unity's program binary cache emptied once (glspec.h)
static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *e;
    char p[1024];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(p); else unlink(p);
    }
    closedir(d);
    rmdir(path);
}
void glspec_cache(const char *cache_dir)
{
    const char *e = getenv("UA_GL_SPEC");
    int on = e && (!strcmp(e, "1") || !strcmp(e, "check"));
    char mark[1024], dir[1024];
    snprintf(mark, sizeof mark, "%s/uaspec.on", cache_dir);
    snprintf(dir, sizeof dir, "%s/UnityShaderCache", cache_dir);
    struct stat st;
    int marked = stat(mark, &st) == 0;
    if (on == marked) return;
    remove_tree(dir);
    if (on) { FILE *f = fopen(mark, "w"); if (f) { fputs("UA_GL_SPEC: Unity's program binaries carry their twins\n", f); fclose(f); } }
    else unlink(mark);
    fprintf(stderr, "[glspec] UA_GL_SPEC turned %s: Unity's program binary cache emptied (%s)\n", on ? "on" : "off", dir);
}

static so_shim g_shims[64];

static void *gl(void *(*next)(const char *), const char *name)
{
    void *p = next ? next(name) : NULL;
    if (p) return p;
    static void *h;
    if (!h) h = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    p = h ? dlsym(h, name) : NULL;
    if (!p) {
        void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        void *(*gpa)(const char *) = e ? (void *(*)(const char *))dlsym(e, "eglGetProcAddress") : NULL;
        if (gpa) p = gpa(name);
    }
    return p;
}

const so_shim *glspec_table(void *(*next)(const char *name))
{
    memset(g_shims, 0, sizeof g_shims);
    const char *e = getenv("UA_GL_SPEC");
    g_on = e && (!strcmp(e, "1") || !strcmp(e, "check"));
    g_check = e && !strcmp(e, "check");
    if (!g_on) return g_shims;
    int ok = 1;
#define Q(f, n) do { if (!(*(void **)&q_##f = gl(NULL, n))) ok = 0; } while (0)
    Q(CreateShader, "glCreateShader"); Q(CompileShader, "glCompileShader"); Q(GetShaderiv, "glGetShaderiv");
    Q(GetShaderInfoLog, "glGetShaderInfoLog"); Q(DeleteShader, "glDeleteShader"); Q(CreateProgram, "glCreateProgram");
    Q(AttachShader, "glAttachShader"); Q(DetachShader, "glDetachShader"); Q(GetAttachedShaders, "glGetAttachedShaders");
    Q(BindAttribLocation, "glBindAttribLocation"); Q(GetActiveAttrib, "glGetActiveAttrib"); Q(GetAttribLocation, "glGetAttribLocation");
    Q(GetActiveUniform, "glGetActiveUniform"); Q(GetActiveUniformsiv, "glGetActiveUniformsiv"); Q(GetUniformLocation, "glGetUniformLocation");
    Q(GetActiveUniformBlockName, "glGetActiveUniformBlockName"); Q(GetUniformBlockIndex, "glGetUniformBlockIndex");
    Q(GetActiveUniformBlockiv, "glGetActiveUniformBlockiv"); Q(GetProgramInfoLog, "glGetProgramInfoLog");
    Q(ProgramUniform1fv, "glProgramUniform1fv"); Q(ProgramUniform2fv, "glProgramUniform2fv"); Q(ProgramUniform3fv, "glProgramUniform3fv");
    Q(ProgramUniform4fv, "glProgramUniform4fv"); Q(ProgramUniform1iv, "glProgramUniform1iv"); Q(ProgramUniform2iv, "glProgramUniform2iv");
    Q(ProgramUniform3iv, "glProgramUniform3iv"); Q(ProgramUniform4iv, "glProgramUniform4iv"); Q(ProgramUniform1uiv, "glProgramUniform1uiv");
    Q(ProgramUniform2uiv, "glProgramUniform2uiv"); Q(ProgramUniform3uiv, "glProgramUniform3uiv"); Q(ProgramUniform4uiv, "glProgramUniform4uiv");
    Q(ProgramUniformMatrix2fv, "glProgramUniformMatrix2fv"); Q(ProgramUniformMatrix3fv, "glProgramUniformMatrix3fv");
    Q(ProgramUniformMatrix4fv, "glProgramUniformMatrix4fv"); Q(ProgramUniformMatrix2x3fv, "glProgramUniformMatrix2x3fv");
    Q(ProgramUniformMatrix3x2fv, "glProgramUniformMatrix3x2fv"); Q(ProgramUniformMatrix2x4fv, "glProgramUniformMatrix2x4fv");
    Q(ProgramUniformMatrix4x2fv, "glProgramUniformMatrix4x2fv"); Q(ProgramUniformMatrix3x4fv, "glProgramUniformMatrix3x4fv");
    Q(ProgramUniformMatrix4x3fv, "glProgramUniformMatrix4x3fv"); Q(GetProgramBinaryQ, "glGetProgramBinary");
    Q(ProgramBinaryQ, "glProgramBinary"); Q(GetProgramivQ, "glGetProgramiv");
    Q(GenFramebuffers, "glGenFramebuffers"); Q(GenTextures, "glGenTextures"); Q(BindTexture, "glBindTexture");
    Q(TexStorage2D, "glTexStorage2D"); Q(DeleteTextures, "glDeleteTextures"); Q(BindFramebuffer, "glBindFramebuffer");
    Q(FramebufferTexture2D, "glFramebufferTexture2D"); Q(IsEnabled, "glIsEnabled"); Q(Enable, "glEnable"); Q(Disable, "glDisable");
    Q(ClearBufferfv, "glClearBufferfv"); Q(ReadPixels, "glReadPixels"); Q(GetBooleanv, "glGetBooleanv"); Q(ColorMask, "glColorMask");
    Q(PixelStorei, "glPixelStorei"); Q(BindBuffer, "glBindBuffer"); Q(UseProgramQ, "glUseProgram"); Q(GetIntegerv_, "glGetIntegerv");
#undef Q
    int k = 0;
#define REQ(f, n) do { *(void **)&r_##f = gl(next, n); if (r_##f) g_shims[k++] = (so_shim){ n, (void *)w_##f }; else ok = 0; } while (0)
    REQ(ShaderSource, "glShaderSource"); REQ(LinkProgram, "glLinkProgram"); REQ(UseProgram, "glUseProgram");
    REQ(DeleteProgram, "glDeleteProgram"); REQ(GetIntegerv, "glGetIntegerv"); REQ(GetProgramiv, "glGetProgramiv");
    REQ(ProgramBinary, "glProgramBinary"); REQ(GetProgramBinary, "glGetProgramBinary");
    REQ(UniformBlockBinding, "glUniformBlockBinding");
    REQ(Uniform1fv, "glUniform1fv"); REQ(Uniform2fv, "glUniform2fv"); REQ(Uniform3fv, "glUniform3fv"); REQ(Uniform4fv, "glUniform4fv");
    REQ(Uniform1iv, "glUniform1iv"); REQ(Uniform2iv, "glUniform2iv"); REQ(Uniform3iv, "glUniform3iv"); REQ(Uniform4iv, "glUniform4iv");
    REQ(Uniform1uiv, "glUniform1uiv"); REQ(Uniform2uiv, "glUniform2uiv"); REQ(Uniform3uiv, "glUniform3uiv"); REQ(Uniform4uiv, "glUniform4uiv");
    REQ(Uniform1f, "glUniform1f"); REQ(Uniform2f, "glUniform2f"); REQ(Uniform3f, "glUniform3f"); REQ(Uniform4f, "glUniform4f");
    REQ(Uniform1i, "glUniform1i"); REQ(Uniform2i, "glUniform2i"); REQ(Uniform3i, "glUniform3i"); REQ(Uniform4i, "glUniform4i");
    REQ(UniformMatrix2fv, "glUniformMatrix2fv"); REQ(UniformMatrix3fv, "glUniformMatrix3fv"); REQ(UniformMatrix4fv, "glUniformMatrix4fv");
    REQ(UniformMatrix2x3fv, "glUniformMatrix2x3fv"); REQ(UniformMatrix3x2fv, "glUniformMatrix3x2fv");
    REQ(UniformMatrix2x4fv, "glUniformMatrix2x4fv"); REQ(UniformMatrix4x2fv, "glUniformMatrix4x2fv");
    REQ(UniformMatrix3x4fv, "glUniformMatrix3x4fv"); REQ(UniformMatrix4x3fv, "glUniformMatrix4x3fv");
    REQ(ProgramUniform4fv, "glProgramUniform4fv");
    if (g_check) { REQ(DrawElements, "glDrawElements"); REQ(DrawArrays, "glDrawArrays"); REQ(DrawElementsBaseVertexOES, "glDrawElementsBaseVertexOES"); }
#undef REQ
    g_src = calloc(MAX_SH, sizeof *g_src);
    g_prog = calloc(MAX_PROG, sizeof *g_prog);
    g_used = calloc(MAX_SH, 1);
    if (!ok || !g_src || !g_prog || !g_used) {
        fprintf(stderr, "[glspec] UA_GL_SPEC: the driver lacks what it needs (GLES 3.1); off\n");
        memset(g_shims, 0, sizeof g_shims);
        g_on = 0;
        return g_shims;
    }
    fprintf(stderr, "[glspec] programs with %s twinned with it a constant 0 (UA_GL_SPEC%s)\n", RULE_NAME, g_check ? "=check" : "");
    return g_shims;
}
