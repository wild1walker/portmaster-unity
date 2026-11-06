// Fill census of traced frames (glfill.c): UA_GL_TRACE=<s> UA_GL_TRACE_FILL=1, called by gltrace.c.
#ifndef GLFILL_H
#define GLFILL_H
int glfill_init(void);                  // at gltrace_table(): 1 when UA_GL_TRACE_FILL=1 and GL has what it needs
void glfill_attach(unsigned prog, unsigned shader);   // glAttachShader: the program's sources
void glfill_ready(void);                // a traced frame begins (the render thread, its context current)
void glfill_frame_start(void);
void glfill_frame_end(long frame);      // the traced frame's census
int glfill_begin(unsigned fb);          // before a draw: 1 = make the same draw now (counted), then glfill_end()
void glfill_end(void);
void glfill_covered(unsigned fb);       // a clear, invalidation or blit of the draw framebuffer's colour
#endif
