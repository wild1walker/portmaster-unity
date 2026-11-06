// A fake libGLESv2 for test_glspec.c: shaders keep their sources; a program's active uniforms are the
// `uniform` lines of its shaders (vec4 or float, location = index + 1, `const` ones are not
// uniforms), each program has its own values; a program binary is "SRC:" + its fragment shader's
// source (format 0x1234), anything else fails to link; draws record the bound program.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_SHADER_TYPE 0x8B4F
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_ACTIVE_UNIFORM_BLOCKS 0x8A36
#define GL_UNIFORM_BLOCK_INDEX 0x8A3A
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#define FMT 0x1234

#define N 64
static unsigned sh_type[N]; static char *sh_src[N]; static int nsh = 1;
typedef struct { int used, linked; unsigned att[4]; int natt; char *fs; char names[8][32]; int nu; float val[8][4]; } prog_t;
static prog_t pr[N]; static int npr = 1;
int fake_cur, fake_draw_prog;
float fake_draw_val[4];

unsigned glCreateShader(unsigned t) { sh_type[nsh] = t; return (unsigned)nsh++; }
void glShaderSource(unsigned s, int n, const char *const *str, const int *len)
{
    size_t t = 0;
    for (int i = 0; i < n; i++) t += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    free(sh_src[s]); sh_src[s] = malloc(t + 1);
    size_t o = 0;
    for (int i = 0; i < n; i++) { size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]); memcpy(sh_src[s] + o, str[i], l); o += l; }
    sh_src[s][o] = 0;
}
void glCompileShader(unsigned s) { (void)s; }
void glGetShaderiv(unsigned s, unsigned e, int *v) { *v = e == GL_SHADER_TYPE ? (int)sh_type[s] : e == GL_COMPILE_STATUS ? 1 : 0; }
void glGetShaderInfoLog(unsigned s, int n, int *l, char *b) { (void)s; if (n) b[0] = 0; if (l) *l = 0; }
void glDeleteShader(unsigned s) { (void)s; }
unsigned glCreateProgram(void) { pr[npr].used = 1; return (unsigned)npr++; }
void glAttachShader(unsigned p, unsigned s) { pr[p].att[pr[p].natt++] = s; }
void glDetachShader(unsigned p, unsigned s)
{ for (int i = 0; i < pr[p].natt; i++) if (pr[p].att[i] == s) pr[p].att[i] = pr[p].att[--pr[p].natt]; }
void glGetAttachedShaders(unsigned p, int max, int *n, unsigned *s) { *n = pr[p].natt < max ? pr[p].natt : max; memcpy(s, pr[p].att, sizeof(unsigned) * (size_t)*n); }
static void parse(prog_t *q, const char *src)
{
    for (const char *l = src; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        if (strncmp(l, "uniform", 7)) continue;
        const char *e = strchr(l, ';'), *b = e;
        while (b > l && b[-1] != ' ' && b[-1] != '\t') b--;
        if (q->nu < 8) { snprintf(q->names[q->nu], 32, "%.*s", (int)(e - b), b); q->nu++; }
    }
}
static void link_fs(prog_t *q, const char *fs) { q->nu = 0; free(q->fs); q->fs = strdup(fs); parse(q, fs); memset(q->val, 0, sizeof q->val); q->linked = 1; }
void glLinkProgram(unsigned p)
{
    for (int i = 0; i < pr[p].natt; i++) if (sh_type[pr[p].att[i]] == GL_FRAGMENT_SHADER) link_fs(&pr[p], sh_src[pr[p].att[i]]);
}
void glDeleteProgram(unsigned p) { pr[p].used = 0; }
void glUseProgram(unsigned p) { fake_cur = (int)p; }
void glGetIntegerv(unsigned e, int *v) { *v = e == GL_CURRENT_PROGRAM ? fake_cur : 0; }
void glGetProgramiv(unsigned p, unsigned e, int *v)
{
    *v = e == GL_LINK_STATUS ? pr[p].linked : e == GL_ACTIVE_UNIFORMS ? pr[p].nu :
         e == GL_PROGRAM_BINARY_LENGTH ? (pr[p].linked ? 4 + (int)strlen(pr[p].fs) : 0) : 0;
}
void glGetProgramInfoLog(unsigned p, int n, int *l, char *b) { (void)p; if (n) b[0] = 0; if (l) *l = 0; }
void glGetProgramBinary(unsigned p, int size, int *len, unsigned *fmt, void *b)
{
    int n = 4 + (int)strlen(pr[p].fs);
    if (size < n) { *len = 0; return; }
    memcpy(b, "SRC:", 4); memcpy((char *)b + 4, pr[p].fs, (size_t)n - 4);
    *len = n; *fmt = FMT;
}
void glProgramBinary(unsigned p, unsigned fmt, const void *b, int len)
{
    pr[p].linked = 0;
    if (fmt != FMT || len < 4 || memcmp(b, "SRC:", 4)) return;
    char *s = malloc((size_t)len - 3);
    memcpy(s, (const char *)b + 4, (size_t)len - 4); s[len - 4] = 0;
    link_fs(&pr[p], s);
    free(s);
}
void glGetActiveAttrib(unsigned p, unsigned i, int n, int *l, int *sz, unsigned *t, char *name) { (void)p; (void)i; (void)n; *l = 0; *sz = 0; *t = 0; name[0] = 0; }
int glGetAttribLocation(unsigned p, const char *n) { (void)p; (void)n; return -1; }
void glBindAttribLocation(unsigned p, unsigned i, const char *n) { (void)p; (void)i; (void)n; }
void glGetActiveUniform(unsigned p, unsigned i, int n, int *l, int *sz, unsigned *t, char *name)
{ snprintf(name, (size_t)n, "%s", pr[p].names[i]); *l = (int)strlen(name); *sz = 1; *t = 0x8B52; }
void glGetActiveUniformsiv(unsigned p, int n, const unsigned *i, unsigned e, int *v) { (void)p; (void)i; for (int k = 0; k < n; k++) v[k] = e == GL_UNIFORM_BLOCK_INDEX ? -1 : 0; }
int glGetUniformLocation(unsigned p, const char *name)
{ for (int i = 0; i < pr[p].nu; i++) if (!strcmp(pr[p].names[i], name)) return i + 1; return -1; }
void glGetActiveUniformBlockName(unsigned p, unsigned i, int n, int *l, char *b) { (void)p; (void)i; (void)n; *l = 0; b[0] = 0; }
unsigned glGetUniformBlockIndex(unsigned p, const char *n) { (void)p; (void)n; return 0xFFFFFFFFu; }
void glGetActiveUniformBlockiv(unsigned p, unsigned i, unsigned e, int *v) { (void)p; (void)i; (void)e; *v = 0; }
void glUniformBlockBinding(unsigned p, unsigned i, unsigned b) { (void)p; (void)i; (void)b; }
static void set(unsigned p, int loc, const float *v, int c) { if (loc >= 1 && loc <= 8) for (int i = 0; i < 4; i++) pr[p].val[loc - 1][i] = i < c ? v[i] : 0; }
void glProgramUniform4fv(unsigned p, int loc, int n, const float *v) { (void)n; set(p, loc, v, 4); }
void glProgramUniform1fv(unsigned p, int loc, int n, const float *v) { (void)n; set(p, loc, v, 1); }
void glUniform4fv(int loc, int n, const float *v) { (void)n; set((unsigned)fake_cur, loc, v, 4); }
void glUniform1fv(int loc, int n, const float *v) { (void)n; set((unsigned)fake_cur, loc, v, 1); }
void glUniform4f(int loc, float a, float b, float c, float d) { float v[4] = { a, b, c, d }; set((unsigned)fake_cur, loc, v, 4); }
// the values a program holds, for the test
float *fake_value(unsigned p, const char *name) { int l = glGetUniformLocation(p, name); return l > 0 ? pr[p].val[l - 1] : NULL; }
const char *fake_fs(unsigned p) { return pr[p].fs; }
void glDrawElements(unsigned m, int c, unsigned t, const void *i) { (void)m; (void)c; (void)t; (void)i; fake_draw_prog = fake_cur; }
// (the rest: present, unused here)
#define STUB(name, args) void name args {}
STUB(glProgramUniform2fv, (unsigned p, int l, int n, const float *v)) STUB(glProgramUniform3fv, (unsigned p, int l, int n, const float *v))
STUB(glProgramUniform1iv, (unsigned p, int l, int n, const int *v)) STUB(glProgramUniform2iv, (unsigned p, int l, int n, const int *v))
STUB(glProgramUniform3iv, (unsigned p, int l, int n, const int *v)) STUB(glProgramUniform4iv, (unsigned p, int l, int n, const int *v))
STUB(glProgramUniform1uiv, (unsigned p, int l, int n, const unsigned *v)) STUB(glProgramUniform2uiv, (unsigned p, int l, int n, const unsigned *v))
STUB(glProgramUniform3uiv, (unsigned p, int l, int n, const unsigned *v)) STUB(glProgramUniform4uiv, (unsigned p, int l, int n, const unsigned *v))
STUB(glProgramUniformMatrix2fv, (unsigned p, int l, int n, unsigned char t, const float *v))
STUB(glProgramUniformMatrix3fv, (unsigned p, int l, int n, unsigned char t, const float *v))
STUB(glProgramUniformMatrix4fv, (unsigned p, int l, int n, unsigned char t, const float *v))
STUB(glProgramUniformMatrix2x3fv, (unsigned p, int l, int n, unsigned char t, const float *v))
STUB(glProgramUniformMatrix3x2fv, (unsigned p, int l, int n, unsigned char t, const float *v))
STUB(glProgramUniformMatrix2x4fv, (unsigned p, int l, int n, unsigned char t, const float *v))
STUB(glProgramUniformMatrix4x2fv, (unsigned p, int l, int n, unsigned char t, const float *v))
STUB(glProgramUniformMatrix3x4fv, (unsigned p, int l, int n, unsigned char t, const float *v))
STUB(glProgramUniformMatrix4x3fv, (unsigned p, int l, int n, unsigned char t, const float *v))
STUB(glUniform2fv, (int l, int n, const float *v)) STUB(glUniform3fv, (int l, int n, const float *v))
STUB(glUniform1iv, (int l, int n, const int *v)) STUB(glUniform2iv, (int l, int n, const int *v))
STUB(glUniform3iv, (int l, int n, const int *v)) STUB(glUniform4iv, (int l, int n, const int *v))
STUB(glUniform1uiv, (int l, int n, const unsigned *v)) STUB(glUniform2uiv, (int l, int n, const unsigned *v))
STUB(glUniform3uiv, (int l, int n, const unsigned *v)) STUB(glUniform4uiv, (int l, int n, const unsigned *v))
STUB(glUniform1f, (int l, float a)) STUB(glUniform2f, (int l, float a, float b)) STUB(glUniform3f, (int l, float a, float b, float c))
STUB(glUniform1i, (int l, int a)) STUB(glUniform2i, (int l, int a, int b)) STUB(glUniform3i, (int l, int a, int b, int c))
STUB(glUniform4i, (int l, int a, int b, int c, int d))
STUB(glUniformMatrix2fv, (int l, int n, unsigned char t, const float *v)) STUB(glUniformMatrix3fv, (int l, int n, unsigned char t, const float *v))
STUB(glUniformMatrix4fv, (int l, int n, unsigned char t, const float *v)) STUB(glUniformMatrix2x3fv, (int l, int n, unsigned char t, const float *v))
STUB(glUniformMatrix3x2fv, (int l, int n, unsigned char t, const float *v)) STUB(glUniformMatrix2x4fv, (int l, int n, unsigned char t, const float *v))
STUB(glUniformMatrix4x2fv, (int l, int n, unsigned char t, const float *v)) STUB(glUniformMatrix3x4fv, (int l, int n, unsigned char t, const float *v))
STUB(glUniformMatrix4x3fv, (int l, int n, unsigned char t, const float *v))
STUB(glGenFramebuffers, (int n, unsigned *f)) STUB(glGenTextures, (int n, unsigned *t)) STUB(glBindTexture, (unsigned t, unsigned x))
STUB(glTexStorage2D, (unsigned t, int l, unsigned f, int w, int h)) STUB(glDeleteTextures, (int n, const unsigned *t))
STUB(glBindFramebuffer, (unsigned t, unsigned f)) STUB(glFramebufferTexture2D, (unsigned t, unsigned a, unsigned tt, unsigned x, int l))
unsigned char glIsEnabled(unsigned c) { (void)c; return 0; }
STUB(glEnable, (unsigned c)) STUB(glDisable, (unsigned c)) STUB(glClearBufferfv, (unsigned b, int d, const float *v))
STUB(glReadPixels, (int x, int y, int w, int h, unsigned f, unsigned t, void *p)) STUB(glGetBooleanv, (unsigned e, unsigned char *v))
STUB(glColorMask, (unsigned char r, unsigned char g, unsigned char b, unsigned char a)) STUB(glPixelStorei, (unsigned e, int v))
STUB(glBindBuffer, (unsigned t, unsigned b)) STUB(glDrawArrays, (unsigned m, int f, int c))
STUB(glDrawElementsBaseVertexOES, (unsigned m, int c, unsigned t, const void *i, int b))
