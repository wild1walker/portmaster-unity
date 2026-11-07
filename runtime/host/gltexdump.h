// gltexdump.c: a 2D texture as the GPU samples it, written to <dir>/tex<id>.ppm and .a.pgm (UA_GL_TRACE_DUMPTEX)
#ifndef GLTEXDUMP_H
#define GLTEXDUMP_H
void gltexdump(unsigned tex, int w, int h, const char *dir);
#endif
