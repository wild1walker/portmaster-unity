// glreplay_hash: the pixel-diff side of glreplay (glreplay.c): each framebuffer's colour read back and hashed
// where a pass ends (a draw-framebuffer bind, the swap), and with --hash-draws after every draw, so two
// replays of one UA_GL_REC_FILE trace (two drivers, or one driver twice) can be compared picture by picture
// and a difference traced to the draw that made it (tools/analysis/pixdiff.py).
//
//   glreplay <trace.gz> --hash <out.txt> [--hash-frames A-B] [--hash-draws] [--hash-dump <dir>]
//
// A line of <out.txt>: "<frame> <call> <event> fb<game's name> <w>x<h> <kind> <hash>", event `bind` (the
// pass on that framebuffer ends: the game binds another), `swap` (the frame ends; fb0 is the window), `draw`
// (--hash-draws: after the draw, the framebuffer it drew into). <call> counts the trace's calls from its
// start, so the same line names the same point of both replays. Colour attachments 0 - 3 are read: a
// normalized one as RGBA / UNSIGNED_BYTE (the conversion every GLES driver must offer), a float one as RGBA /
// FLOAT, an integer one as RGBA_INTEGER / (UNSIGNED_)INT; <kind> says which. Depth and stencil are not
// readable in GLES and are not hashed (what they decide shows in the colour of later draws). The hash is
// FNV-1a 64 of the rows bottom to top. --hash-dump writes each readback of an 8-bit target as PPM
// (<frame>-<call>-<event>-fb<n>-a<attachment>.ppm, alpha dropped) for looking at a difference.
// Everything the readback changes (read framebuffer, read buffer, pack state) is put back.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES3/gl32.h>

#include "glreplay_hash.h"

static FILE *g_out;
static long g_from = 0, g_to = -1;
static int g_draws;
static const char *g_dump;
static void *(*g_sym)(const char *);
static unsigned char *g_buf; static size_t g_nbuf;
static unsigned long g_lines;

static void (*q_GetIntegerv)(GLenum, GLint *);
static void (*q_BindFramebuffer)(GLenum, GLuint);
static void (*q_ReadBuffer)(GLenum);
static void (*q_ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*q_PixelStorei)(GLenum, GLint);
static void (*q_BindBuffer)(GLenum, GLuint);
static void (*q_GetFbAttach)(GLenum, GLenum, GLenum, GLint *);
static void (*q_GetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);
static void (*q_GetRenderbufferParameteriv)(GLenum, GLenum, GLint *);
static void (*q_BindTexture)(GLenum, GLuint);
static void (*q_BindRenderbuffer)(GLenum, GLuint);
static GLenum (*q_GetError)(void);

int rph_init(const char *out, const char *frames, int draws, const char *dump, void *(*sym)(const char *))
{
    g_out = fopen(out, "w");
    if (!g_out) { fprintf(stderr, "glreplay: --hash: cannot write %s\n", out); return -1; }
    if (frames) {
        long a = 0, b = -1;
        if (sscanf(frames, "%ld-%ld", &a, &b) < 1) { fprintf(stderr, "glreplay: --hash-frames A-B\n"); return -1; }
        g_from = a; g_to = b >= 0 ? b + 1 : -1;
    }
    g_draws = draws; g_dump = dump; g_sym = sym;
#define S(v, n) *(void **)&v = sym(n)
    S(q_GetIntegerv, "glGetIntegerv"); S(q_BindFramebuffer, "glBindFramebuffer"); S(q_ReadBuffer, "glReadBuffer");
    S(q_ReadPixels, "glReadPixels"); S(q_PixelStorei, "glPixelStorei"); S(q_BindBuffer, "glBindBuffer");
    S(q_GetFbAttach, "glGetFramebufferAttachmentParameteriv"); S(q_GetTexLevelParameteriv, "glGetTexLevelParameteriv");
    S(q_GetRenderbufferParameteriv, "glGetRenderbufferParameteriv"); S(q_BindTexture, "glBindTexture");
    S(q_BindRenderbuffer, "glBindRenderbuffer"); S(q_GetError, "glGetError");
#undef S
    if (!q_GetIntegerv || !q_BindFramebuffer || !q_ReadBuffer || !q_ReadPixels || !q_PixelStorei || !q_BindBuffer || !q_GetFbAttach ||
        !q_GetRenderbufferParameteriv || !q_BindTexture || !q_BindRenderbuffer || !q_GetError) {
        fprintf(stderr, "glreplay: --hash: this driver lacks a call the readback needs\n");
        return -1;
    }
    fprintf(g_out, "# glreplay --hash: frame call event framebuffer size kind fnv1a64\n");
    return 0;
}

int rph_on(long frame) { return g_out && frame >= g_from && (g_to < 0 || frame < g_to); }
int rph_draws(void) { return g_draws; }

static uint64_t fnv(const unsigned char *p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// the size of attachment a of the bound read framebuffer (0 if none)
static int att_size(GLenum a, GLint *w, GLint *h, GLint *ctype)
{
    GLint type = GL_NONE, name = 0, level = 0, face = 0;
    q_GetFbAttach(GL_READ_FRAMEBUFFER, a, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    if (type == GL_NONE) return 0;
    q_GetFbAttach(GL_READ_FRAMEBUFFER, a, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
    q_GetFbAttach(GL_READ_FRAMEBUFFER, a, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, ctype);
    *w = *h = 0;
    if (type == GL_RENDERBUFFER) {
        GLint old = 0;
        q_GetIntegerv(GL_RENDERBUFFER_BINDING, &old);
        q_BindRenderbuffer(GL_RENDERBUFFER, (GLuint)name);
        q_GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, w);
        q_GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, h);
        q_BindRenderbuffer(GL_RENDERBUFFER, (GLuint)old);
    } else if (type == GL_TEXTURE && q_GetTexLevelParameteriv) {
        q_GetFbAttach(GL_READ_FRAMEBUFFER, a, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
        q_GetFbAttach(GL_READ_FRAMEBUFFER, a, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE, &face);
        GLenum tgt = face ? (GLenum)face : GL_TEXTURE_2D, bind = face ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
        GLint old = 0;
        q_GetIntegerv(face ? GL_TEXTURE_BINDING_CUBE_MAP : GL_TEXTURE_BINDING_2D, &old);
        q_BindTexture(bind, (GLuint)name);
        q_GetTexLevelParameteriv(tgt, level, GL_TEXTURE_WIDTH, w);
        q_GetTexLevelParameteriv(tgt, level, GL_TEXTURE_HEIGHT, h);
        q_BindTexture(bind, (GLuint)old);
        while (q_GetError()) {}                     // (an array or 3D texture: no 2D size here; skipped below)
    }
    return *w > 0 && *h > 0;
}

static void dump(const char *name, const unsigned char *p, int w, int h)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.ppm", g_dump, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--)
        for (int x = 0; x < w; x++) fwrite(p + ((size_t)y * w + x) * 4, 1, 3, f);
    fclose(f);
}

// the colour of the bound draw framebuffer (`game_fb` its name in the trace), or with window the window's,
// read back and hashed
void rph_read(long frame, unsigned long call, const char *event, unsigned game_fb, int window, int win_w, int win_h)
{
    GLint draw = 0, read = 0, pack = 0, align = 4, rowlen = 0, skipr = 0, skipp = 0, rbuf = GL_BACK;
    if (!window) q_GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    q_GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    q_GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    q_GetIntegerv(GL_PACK_ALIGNMENT, &align);
    q_GetIntegerv(GL_PACK_ROW_LENGTH, &rowlen);
    q_GetIntegerv(GL_PACK_SKIP_ROWS, &skipr);
    q_GetIntegerv(GL_PACK_SKIP_PIXELS, &skipp);
    q_BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)draw);
    q_GetIntegerv(GL_READ_BUFFER, &rbuf);
    if (pack) q_BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    q_PixelStorei(GL_PACK_ALIGNMENT, 1); q_PixelStorei(GL_PACK_ROW_LENGTH, 0);
    q_PixelStorei(GL_PACK_SKIP_ROWS, 0); q_PixelStorei(GL_PACK_SKIP_PIXELS, 0);
    for (int a = 0; a < (draw ? 4 : 1); a++) {
        GLint w = win_w, h = win_h, ct = GL_UNSIGNED_NORMALIZED;
        GLenum att = GL_COLOR_ATTACHMENT0 + (GLenum)a;
        if (draw && !att_size(att, &w, &h, &ct)) continue;
        GLenum fmt = GL_RGBA, type = GL_UNSIGNED_BYTE;
        const char *kind = "rgba8";
        size_t px = 4;
        if (ct == GL_FLOAT) { type = GL_FLOAT; kind = "rgba32f"; px = 16; }
        else if (ct == GL_INT) { fmt = GL_RGBA_INTEGER; type = GL_INT; kind = "rgba32i"; px = 16; }
        else if (ct == GL_UNSIGNED_INT) { fmt = GL_RGBA_INTEGER; type = GL_UNSIGNED_INT; kind = "rgba32ui"; px = 16; }
        size_t n = (size_t)w * (size_t)h * px;
        if (n > g_nbuf) { free(g_buf); g_nbuf = n; g_buf = malloc(n); }
        q_ReadBuffer(draw ? att : GL_BACK);
        memset(g_buf, 0, n);
        q_ReadPixels(0, 0, w, h, fmt, type, g_buf);
        GLenum e = q_GetError();
        while (q_GetError()) {}
        uint64_t hsh = e ? 0 : fnv(g_buf, n, 1469598103934665603ull);
        fprintf(g_out, "%ld %lu %s fb%u%s %dx%d %s %016llx%s\n", frame, call, event, game_fb, a ? (a == 1 ? ".1" : a == 2 ? ".2" : ".3") : "",
                w, h, kind, (unsigned long long)hsh, e ? " readerror" : "");
        g_lines++;
        if (g_dump && !e && px == 4) {
            char nm[200];
            snprintf(nm, sizeof nm, "%ld-%lu-%s-fb%u-a%d", frame, call, event, game_fb, a);
            dump(nm, g_buf, w, h);
        }
    }
    q_ReadBuffer((GLenum)rbuf);
    q_PixelStorei(GL_PACK_ALIGNMENT, align); q_PixelStorei(GL_PACK_ROW_LENGTH, rowlen);
    q_PixelStorei(GL_PACK_SKIP_ROWS, skipr); q_PixelStorei(GL_PACK_SKIP_PIXELS, skipp);
    if (pack) q_BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    q_BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
    while (q_GetError()) {}
}

void rph_close(void)
{
    if (!g_out) return;
    fclose(g_out);
    fprintf(stderr, "glreplay: --hash: %lu readbacks hashed\n", g_lines);
    g_out = NULL;
}
