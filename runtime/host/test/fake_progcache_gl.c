// A stand-in libGLESv2.so.2 for test_progcache.c (glhooks.c's UA_GL_PROGCACHE): shaders keep their source,
// programs their attached shaders and attribute bindings. A link makes the program's "executable", an id
// from its shaders' types and sources (in type order) and its bindings; the binary is that id and the
// driver build's tag; glProgramBinary accepts only its own build's binaries. Counts links and binary loads.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define GL_LINK_STATUS 0x8B82
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#define GL_SHADER_TYPE 0x8B4F
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define FMT 0x8f61
static struct { unsigned type; char src[256]; } sh[64];
static struct { unsigned sh[4]; int n; char binds[256]; uint64_t exe; int linked; } pr[64];
static unsigned nsh = 1, npr = 1;
int fake_links, fake_binary_loads, fake_build = 7;
static uint64_t mix(uint64_t h, const void *p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= ((const unsigned char *)p)[i]; h *= 1099511628211ull; } return h; }
unsigned glCreateShader(unsigned type) { sh[nsh].type = type; sh[nsh].src[0] = 0; return nsh++; }
unsigned glCreateProgram(void) { memset(&pr[npr], 0, sizeof pr[npr]); return npr++; }
void glShaderSource(unsigned s, int count, const char *const *str, const int *len)
{
    sh[s].src[0] = 0;
    for (int i = 0; i < count; i++) strncat(sh[s].src, str[i], len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]));
}
void glCompileShader(unsigned s) { (void)s; }
void glAttachShader(unsigned p, unsigned s) { pr[p].sh[pr[p].n++] = s; }
void glDetachShader(unsigned p, unsigned s) { for (int i = 0; i < pr[p].n; i++) if (pr[p].sh[i] == s) pr[p].sh[i] = pr[p].sh[--pr[p].n]; }
void glBindAttribLocation(unsigned p, unsigned i, const char *n) { char b[64]; snprintf(b, sizeof b, "%u=%s;", i, n); strcat(pr[p].binds, b); }
void glTransformFeedbackVaryings(unsigned p, int n, const char *const *v, unsigned m) { (void)p; (void)n; (void)v; (void)m; }
void glProgramParameteri(unsigned p, unsigned pname, int v) { (void)p; (void)pname; (void)v; }
void glDeleteShader(unsigned s) { (void)s; }
void glDeleteProgram(unsigned p) { (void)p; }
// the executable a link makes: the shaders by type (vertex 0x8B31 after fragment 0x8B30), then the bindings
uint64_t fake_exe_of(unsigned p)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned t = 0x8B30; t <= 0x8B31; t++)
        for (int i = 0; i < pr[p].n; i++) if (sh[pr[p].sh[i]].type == t) h = mix(h, sh[pr[p].sh[i]].src, strlen(sh[pr[p].sh[i]].src));
    return mix(h, pr[p].binds, strlen(pr[p].binds));
}
void glLinkProgram(unsigned p) { fake_links++; pr[p].exe = fake_exe_of(p); pr[p].linked = 1; }
uint64_t fake_exe(unsigned p) { return pr[p].linked ? pr[p].exe : 0; }
void glGetProgramiv(unsigned p, unsigned pname, int *v)
{
    if (pname == GL_LINK_STATUS) *v = pr[p].linked;
    else if (pname == GL_PROGRAM_BINARY_LENGTH) *v = pr[p].linked ? 12 : 0;
}
void glGetShaderiv(unsigned s, unsigned pname, int *v) { if (pname == GL_SHADER_TYPE) *v = (int)sh[s].type; }
void glGetProgramBinary(unsigned p, int size, int *len, unsigned *fmt, void *b)
{
    if (size < 12 || !pr[p].linked) { if (len) *len = 0; return; }
    memcpy(b, &pr[p].exe, 8); memcpy((char *)b + 8, &fake_build, 4); *len = 12; *fmt = FMT;
}
void glProgramBinary(unsigned p, unsigned fmt, const void *b, int len)
{
    fake_binary_loads++;
    int build;
    memcpy(&build, (const char *)b + 8, 4);
    pr[p].linked = fmt == FMT && len == 12 && build == fake_build;
    if (pr[p].linked) memcpy(&pr[p].exe, b, 8);
}
const unsigned char *glGetString(unsigned name)
{
    // (a driver update that keeps its version string: the binary's own tag refuses the old binary)
    return (const unsigned char *)(name == GL_RENDERER ? "Fake-G31" : "Fake ES 3.2");
}
