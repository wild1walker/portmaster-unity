#!/usr/bin/env python3
"""Writes glrec_gen.h (runtime/host/glrec.c's wrappers and replay) from Khronos' GLES headers.

    python3 glrec_gen.py <dir with gl32.h and gl2ext.h> > glrec_gen.h

(the ua-host-armhf image has them: /usr/include/GLES3/gl32.h, /usr/include/GLES2/gl2ext.h.)
Every GLES 3.2 entry point, and each extension entry point a Unity player names (NAMES below: the
GL names in the libunity.so of 15 players, 5.6 - 6000.6), gets a wrapper. A wrapper either records
the call into glrec.c's command buffer (calls that only set state or queue GPU work: their arguments
by value, the memory a pointer argument points at copied), or submits everything recorded so far
and calls through (anything that returns a value, writes through a pointer, or reads memory whose
size is not known here). The class of each function is decided below; glrec.c has the rest."""
import re, sys, os

# extension entry points Unity players name (core GLES 3.2 functions are all wrapped)
NAMES = set('''
glBeginQueryEXT glBlendBarrierKHR glBlendBarrierNV glBlendEquationSeparateiEXT glBlendEquationiEXT
glBlendFuncSeparateiEXT glBlendFunciEXT glColorMaskiEXT glCopyImageSubDataEXT glCopyImageSubDataOES
glDebugMessageCallbackKHR glDebugMessageControlKHR glDebugMessageInsertKHR glDeleteQueriesEXT
glDiscardFramebufferEXT glDrawArraysInstancedEXT glDrawElementsBaseVertexEXT glDrawElementsBaseVertexOES
glDrawElementsInstancedBaseVertexEXT glDrawElementsInstancedBaseVertexOES glDrawElementsInstancedEXT
glDrawRangeElementsBaseVertexEXT glDrawRangeElementsBaseVertexOES glEndQueryEXT
glFramebufferTexture2DMultisampleEXT glFramebufferTexture2DMultisampleIMG glFramebufferTextureEXT
glFramebufferTextureMultisampleMultiviewOVR glFramebufferTextureMultiviewOVR glGenQueriesEXT
glGetObjectLabelEXT glGetObjectLabelKHR glGetQueryObjectui64vEXT glGetQueryObjectuivEXT glGetQueryivEXT
glLabelObjectEXT glMapBufferOES glObjectLabelKHR glPatchParameteriEXT glPatchParameteriOES
glPopDebugGroupKHR glPopGroupMarkerEXT glPushDebugGroupKHR glPushGroupMarkerEXT glQueryCounterEXT
glRenderbufferStorageMultisampleEXT glRenderbufferStorageMultisampleIMG glTexBufferEXT
glTexPageCommitmentEXT glTexStorage2DEXT glTexStorage3DEXT glTextureViewEXT glTextureViewOES
glUnmapBufferOES glBindVertexArrayOES glDeleteVertexArraysOES glGenVertexArraysOES glIsVertexArrayOES
glGetProgramBinaryOES glProgramBinaryOES glInsertEventMarkerEXT glMultiDrawArraysEXT glMultiDrawElementsEXT
glMultiDrawElementsBaseVertexEXT glMultiDrawElementsBaseVertexOES glDrawArraysInstancedBaseInstanceEXT
glDrawElementsInstancedBaseInstanceEXT glDrawElementsInstancedBaseVertexBaseInstanceEXT glTexStorage1DEXT
glBufferStorageEXT glGetGraphicsResetStatusEXT glGetGraphicsResetStatusKHR glFlushMappedBufferRangeEXT
glMapBufferRangeEXT glClipControlEXT glPolygonOffsetClampEXT glFramebufferFetchBarrierEXT
glFramebufferPixelLocalStorageSizeEXT glClearPixelLocalStorageuiEXT glTexStorage3DMultisampleOES
glCopyTextureLevelsAPPLE glResolveMultisampleFramebufferAPPLE glRenderbufferStorageMultisampleAPPLE
glDiscardFramebufferEXT glQueryCounterEXT glGetQueryObjecti64vEXT glGetQueryObjectivEXT
'''.split())

# by value and void, but must not be deferred: submit first
FLUSH_SCALAR = set('''glFinish glLinkProgram glCompileShader glValidateProgram glDeleteShader glDeleteProgram
glAttachShader glDetachShader glReleaseShaderCompiler glProgramParameteri glUseProgramStages
glActiveShaderProgram glBindProgramPipeline glDispatchCompute glDispatchComputeIndirect glMemoryBarrier
glMemoryBarrierByRegion glBeginTransformFeedback glEndTransformFeedback glPauseTransformFeedback
glResumeTransformFeedback glBindTransformFeedback glBeginQuery glEndQuery glBeginQueryEXT glEndQueryEXT
glQueryCounterEXT glTexImage2D glTexImage3D glCopyTexImage2D glGenerateMipmap
glTexStorage2D glTexStorage3D glTexStorage2DMultisample glTexStorage3DMultisample glTexStorage2DEXT
glTexStorage3DEXT glTexStorage1DEXT glTexStorage3DMultisampleOES glRenderbufferStorage
glRenderbufferStorageMultisample glRenderbufferStorageMultisampleEXT glRenderbufferStorageMultisampleIMG
glRenderbufferStorageMultisampleAPPLE glTexBuffer glTexBufferRange glTexBufferEXT glTextureViewEXT
glTextureViewOES glTexPageCommitmentEXT glBindImageTexture glCopyImageSubData glCopyImageSubDataEXT
glCopyImageSubDataOES glCopyBufferSubData
glDebugMessageControl glDebugMessageControlKHR glFramebufferFetchBarrierEXT
glFramebufferPixelLocalStorageSizeEXT glResolveMultisampleFramebufferAPPLE glCopyTextureLevelsAPPLE
glDeleteSync glWaitSync glPatchParameteri glPatchParameteriEXT glPatchParameteriOES glMinSampleShading
glBufferStorageEXT'''.split())
# (texture definitions and copies between objects submit first: few a frame, and the frame graph
#  treats them as an end of what it knows. glDispatchCompute: writes the analysis cannot see.)

# returns a name and depends on nothing recorded: called at once, nothing submitted
IMMEDIATE = set('''glGenTextures glGenBuffers glGenFramebuffers glGenRenderbuffers glGenVertexArrays
glGenVertexArraysOES glGenSamplers glGenQueries glGenQueriesEXT glGenTransformFeedbacks
glGenProgramPipelines glCreateShader glCreateProgram glGetString glGetStringi glGetShaderPrecisionFormat
glGetGraphicsResetStatus glGetGraphicsResetStatusEXT glGetGraphicsResetStatusKHR'''.split())

# pointer arguments whose memory is copied when the call is recorded: {function: {param: C size}}
COPY = {}
for n, k in (('1', 1), ('2', 2), ('3', 3), ('4', 4)):
    for t in ('f', 'i', 'ui'):
        COPY['glUniform%s%sv' % (n, t)] = {'value': 'count * %d * 4' % k}
        COPY['glProgramUniform%s%sv' % (n, t)] = {'value': 'count * %d * 4' % k}
        COPY['glProgramUniform%s%svEXT' % (n, t)] = {'value': 'count * %d * 4' % k}
for m, k in (('2', 4), ('3', 9), ('4', 16), ('2x3', 6), ('3x2', 6), ('2x4', 8), ('4x2', 8), ('3x4', 12), ('4x3', 12)):
    COPY['glUniformMatrix%sfv' % m] = {'value': 'count * %d * 4' % k}
    COPY['glProgramUniformMatrix%sfv' % m] = {'value': 'count * %d * 4' % k}
    COPY['glProgramUniformMatrix%sfvEXT' % m] = {'value': 'count * %d * 4' % k}
COPY.update({
    'glBufferSubData': {'data': 'size'},
    'glBufferData': {'data': 'data ? size : 0'},
    'glInvalidateFramebuffer': {'attachments': 'numAttachments * 4'},
    'glInvalidateSubFramebuffer': {'attachments': 'numAttachments * 4'},
    'glDiscardFramebufferEXT': {'attachments': 'numAttachments * 4'},
    'glDrawBuffers': {'bufs': 'n * 4'},
    'glClearBufferfv': {'value': '(buffer == 0x1800 ? 4 : 1) * 4'},
    'glClearBufferiv': {'value': '(buffer == 0x1800 ? 4 : 1) * 4'},
    'glClearBufferuiv': {'value': '16'},
    'glVertexAttrib1fv': {'v': '4'}, 'glVertexAttrib2fv': {'v': '8'}, 'glVertexAttrib3fv': {'v': '12'},
    'glVertexAttrib4fv': {'v': '16'}, 'glVertexAttribI4iv': {'v': '16'}, 'glVertexAttribI4uiv': {'v': '16'},
    'glTexParameteriv': {'params': '(pname == 0x1004 ? 4 : 1) * 4'},
    'glTexParameterfv': {'params': '(pname == 0x1004 ? 4 : 1) * 4'},
    'glTexParameterIiv': {'params': '(pname == 0x1004 ? 4 : 1) * 4'},
    'glTexParameterIuiv': {'params': '(pname == 0x1004 ? 4 : 1) * 4'},
    'glSamplerParameteriv': {'param': '(pname == 0x1004 ? 4 : 1) * 4'},
    'glSamplerParameterfv': {'param': '(pname == 0x1004 ? 4 : 1) * 4'},
    'glSamplerParameterIiv': {'param': '(pname == 0x1004 ? 4 : 1) * 4'},
    'glSamplerParameterIuiv': {'param': '(pname == 0x1004 ? 4 : 1) * 4'},
    'glDeleteTextures': {'textures': 'n * 4'}, 'glDeleteBuffers': {'buffers': 'n * 4'},
    'glDeleteFramebuffers': {'framebuffers': 'n * 4'}, 'glDeleteRenderbuffers': {'renderbuffers': 'n * 4'},
    'glDeleteVertexArrays': {'arrays': 'n * 4'}, 'glDeleteVertexArraysOES': {'arrays': 'n * 4'},
    'glDeleteSamplers': {'samplers': 'count * 4'}, 'glDeleteQueries': {'ids': 'n * 4'},
    'glDeleteQueriesEXT': {'ids': 'n * 4'}, 'glDeleteTransformFeedbacks': {'ids': 'n * 4'},
    # debug groups and markers (a string: its length, or up to its end)
    'glPushDebugGroup': {'message': 'length < 0 ? strlen(message) + 1 : (size_t)length'},
    'glPushDebugGroupKHR': {'message': 'length < 0 ? strlen(message) + 1 : (size_t)length'},
    'glPushGroupMarkerEXT': {'marker': 'length <= 0 ? strlen(marker) + 1 : (size_t)length'},
    'glInsertEventMarkerEXT': {'marker': 'length <= 0 ? strlen(marker) + 1 : (size_t)length'},
})

# draws: indices (an offset into the element buffer, or client memory: copied) and client vertex arrays
DRAWS = {   # name: index of the indices parameter or None, the count and type parameters
    'glDrawArrays': None, 'glDrawArraysInstanced': None, 'glDrawArraysInstancedEXT': None,
    'glDrawArraysInstancedBaseInstanceEXT': None, 'glDrawArraysIndirect': None, 'glDrawElementsIndirect': None,
    'glDrawElements': 'indices', 'glDrawElementsInstanced': 'indices', 'glDrawElementsInstancedEXT': 'indices',
    'glDrawRangeElements': 'indices', 'glDrawElementsBaseVertex': 'indices', 'glDrawElementsBaseVertexEXT': 'indices',
    'glDrawElementsBaseVertexOES': 'indices', 'glDrawRangeElementsBaseVertex': 'indices',
    'glDrawRangeElementsBaseVertexEXT': 'indices', 'glDrawRangeElementsBaseVertexOES': 'indices',
    'glDrawElementsInstancedBaseVertex': 'indices', 'glDrawElementsInstancedBaseVertexEXT': 'indices',
    'glDrawElementsInstancedBaseVertexOES': 'indices', 'glDrawElementsInstancedBaseInstanceEXT': 'indices',
    'glDrawElementsInstancedBaseVertexBaseInstanceEXT': 'indices',
}
# recorded with a pointer kept as a number (an offset into a bound buffer; glrec.c's hook knows
# whether it is client memory and submits first then)
POINTER_AS_VALUE = {'glVertexAttribPointer': 'pointer', 'glVertexAttribIPointer': 'pointer',
                    'glDrawArraysIndirect': 'indirect', 'glDrawElementsIndirect': 'indirect'}
# calls glrec.c follows (its vertex array shadow: whether a draw reads client memory), before the
# call is recorded; a nonzero result while recording submits what is recorded and calls through
HOOKS = {
    'glBindVertexArray': 'glrec_vao_hook(rec_, array)', 'glBindVertexArrayOES': 'glrec_vao_hook(rec_, array)',
    'glBindBuffer': 'glrec_buffer_hook(rec_, target, buffer)',
    'glVertexAttribPointer': 'glrec_vap_hook(rec_, index)', 'glVertexAttribIPointer': 'glrec_vap_hook(rec_, index)',
    'glEnableVertexAttribArray': 'glrec_attrib_hook(rec_, index, 1)', 'glDisableVertexAttribArray': 'glrec_attrib_hook(rec_, index, 0)',
}
for _d, _i in DRAWS.items():
    HOOKS[_d] = 'glrec_draw_hook(rec_, %d, %d)' % (_i is not None, 'Indirect' in _d)
# (deleting buffers or vertex arrays, and GLES 3.1's separate vertex formats, change what the shadow
#  knows: submitted first, the shadow then asks the driver again)
FLUSH_SCALAR |= set('''glDeleteBuffers glDeleteVertexArrays glDeleteVertexArraysOES glVertexAttribBinding
glBindVertexBuffer glVertexAttribFormat glVertexAttribIFormat glVertexBindingDivisor'''.split())
for _n in ('glDeleteBuffers', 'glDeleteVertexArrays', 'glDeleteVertexArraysOES'):
    COPY.pop(_n, None)
# calls glrec.c follows after they ran (any thread): which programs' shaders read the depth/stencil
# buffer through framebuffer fetch, and Unity's program binary cache
POST = {
    'glShaderSource': 'glrec_shader_source(shader, count, string, length)',
    'glAttachShader': 'glrec_attach_shader(program, shader)',
    'glLinkProgram': 'glrec_link_program(program)',
    'glProgramBinary': 'glrec_program_binary(program)', 'glProgramBinaryOES': 'glrec_program_binary(program)',
    'glGetIntegerv': 'glrec_get_integerv(pname, data)',
    'glDeleteProgram': 'glrec_delete_program(program)',
}

PTR_TYPES = {'GLsync', 'GLDEBUGPROC', 'GLDEBUGPROCKHR', 'GLeglImageOES', 'GLeglClientBufferEXT'}
FLOAT_TYPES = {'GLfloat', 'GLclampf'}

PROTO = re.compile(r'GL_APICALL\s+(.+?)\s*GL_APIENTRY\s+(gl\w+)\s*\((.*?)\);', re.S)


def parse(path):
    out = []
    for m in PROTO.finditer(open(path).read()):
        ret, name, params = ' '.join(m.group(1).split()), m.group(2), ' '.join(m.group(3).split())
        ps = []
        if params and params != 'void':
            for p in params.split(','):
                p = p.strip()
                mm = re.match(r'(.*?)(\w+)$', p)
                ps.append((mm.group(1).strip(), mm.group(2)))
        out.append((ret, name, ps))
    return out


def is_ptr(t):
    return '*' in t or t.replace('const ', '').strip() in PTR_TYPES


def slot_in(t, n):
    b = t.replace('const ', '').strip()
    if b in FLOAT_TYPES:
        return 'rec_fbits(%s)' % n
    if is_ptr(t):
        return '(uint64_t)(uintptr_t)%s' % n
    if b in ('GLuint64', 'GLuint64EXT'):
        return '(uint64_t)%s' % n
    return '(uint64_t)(int64_t)%s' % n


def slot_out(t, i):
    b = t.replace('const ', '').strip()
    if b in FLOAT_TYPES:
        return 'rec_bitsf(a[%d])' % i
    if is_ptr(t):
        return '(%s)(uintptr_t)a[%d]' % (t, i)
    if b in ('GLuint64', 'GLuint64EXT'):
        return '(%s)a[%d]' % (b, i)
    return '(%s)(int64_t)a[%d]' % (b, i)


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '/usr/include'
    core = parse(os.path.join(d, 'gl32.h'))
    ext = parse(os.path.join(d, 'gl2ext.h'))
    funcs, seen = [], set()
    for ret, name, ps in core + [f for f in ext if f[1] in NAMES]:
        if name in seen:
            continue
        seen.add(name)
        funcs.append((ret, name, ps))
    funcs.sort(key=lambda f: f[1])
    classes = {}
    for ret, name, ps in funcs:
        ptrs = [p for p in ps if is_ptr(p[0])]
        if name in IMMEDIATE:
            c = 'imm'
        elif name in FLUSH_SCALAR:
            c = 'flush'
        elif ret != 'void':
            c = 'flush'
        elif name in DRAWS:
            c = 'rec'
        elif name in COPY:
            c = 'rec'
        elif not ptrs or all(p[1] == POINTER_AS_VALUE.get(name) for p in ptrs):
            c = 'rec'
        else:
            c = 'flush'
        if name.startswith(('glGet', 'glIs', 'glCheck', 'glMap', 'glUnmap', 'glFlushMapped', 'glReadPixels', 'glReadnPixels',
                            'glClientWaitSync', 'glFenceSync')):
            c = 'imm' if name in IMMEDIATE else 'flush'
        classes[name] = c
    w = sys.stdout.write
    w('// Generated by glrec_gen.py from Khronos\' GLES 3.2 headers (gl32.h, gl2ext.h): do not edit.\n')
    w('// %d entry points: %d recorded, %d submit first, %d called at once.\n' % (
        len(funcs), sum(c == 'rec' for c in classes.values()), sum(c == 'flush' for c in classes.values()),
        sum(c == 'imm' for c in classes.values())))
    w('enum {\n')
    for ret, name, ps in funcs:
        w('    OP_%s,\n' % name)
    w('    OP_N\n};\n')
    w('static const char *const __attribute__((unused)) glrec_name[OP_N] = {\n')
    for ret, name, ps in funcs:
        w('    "%s",\n' % name)
    w('};\n')
    w('static const unsigned char __attribute__((unused)) glrec_class[OP_N] = {   // 1 recorded, 2 submits first, 3 at once\n')
    for ret, name, ps in funcs:
        w('    %d,   // %s\n' % ({'rec': 1, 'flush': 2, 'imm': 3}[classes[name]], name))
    w('};\n')
    for ret, name, ps in funcs:
        w('typedef %s (*T_%s)(%s);\n' % (ret, name, ', '.join(t for t, n in ps) or 'void'))
    w('\n#ifndef GLREC_OPS_ONLY\n')
    w('static void *glrec_next[OP_N];\n')
    w('#define NX(f) ((T_##f)glrec_next[OP_##f])\n\n')
    for ret, name, ps in funcs:
        hasf = any(p[0].replace('const ', '').strip() in FLOAT_TYPES and not is_ptr(p[0]) for p in ps)
        decl = ', '.join('%s %s' % (t, n) for t, n in ps) or 'void'
        args = ', '.join(n for t, n in ps)
        abi = 'SO_ABI ' if hasf else ''
        c = classes[name]
        rv = 'return ' if ret != 'void' else ''
        if c == 'imm':
            w('static %s%s w_%s(%s) { glrec_count(OP_%s); %sNX(%s)(%s); }\n' % (abi, ret, name, decl, name, rv, name, args))
            continue
        if c == 'flush' and name in POST:
            assert ret == 'void'
            w('static %s%s w_%s(%s) { glrec_submit_for(OP_%s); NX(%s)(%s); %s; }\n' % (abi, ret, name, decl, name, name, args, POST[name]))
            continue
        if c == 'flush':
            w('static %s%s w_%s(%s) { glrec_submit_for(OP_%s); %sNX(%s)(%s); }\n' % (abi, ret, name, decl, name, rv, name, args))
            continue
        w('static %s%s w_%s(%s)\n{\n    int rec_ = glrec_recording();\n' % (abi, ret, name, decl))
        if name in HOOKS:
            # (the hook follows the call either way; nonzero while recording: submit and call through)
            w('    if (%s && rec_) { glrec_submit_for(OP_%s); rec_ = 0; }\n' % (HOOKS[name], name))
        w('    if (!rec_) { glrec_count(OP_%s); NX(%s)(%s); return; }\n' % (name, name, args))
        cp = COPY.get(name, {})
        sl = []
        for i, (t, n) in enumerate(ps):
            if n in cp:
                sl.append('rec_a[%d] = glrec_blob(%s, (size_t)(%s));' % (i, n, cp[n]))
            elif DRAWS.get(name) == n:
                sl.append('rec_a[%d] = glrec_indices(%s, count, type);' % (i, n))
            else:
                sl.append('rec_a[%d] = %s;' % (i, slot_in(t, n)))
        if ps:
            w('    uint64_t *rec_a = glrec_cmd(OP_%s, %d);\n    %s\n}\n' % (name, len(ps), ' '.join(sl)))
        else:
            w('    glrec_cmd(OP_%s, 0);\n}\n' % name)
    # replay
    w('\n// one recorded call, to the next layer (blobs are pointers again)\n')
    w('static void glrec_call(unsigned op, const uint64_t *a)\n{\n    switch (op) {\n')
    for ret, name, ps in funcs:
        if classes[name] != 'rec':
            continue
        types = ', '.join(t for t, n in ps) or 'void'
        cp = COPY.get(name, {})
        outs = []
        for i, (t, n) in enumerate(ps):
            if n in cp or DRAWS.get(name) == n:
                outs.append('(%s)glrec_ptr(a[%d])' % (t, i))
            else:
                outs.append(slot_out(t, i))
        w('    case OP_%s: NX(%s)(%s); break;\n' % (name, name, ', '.join(outs)))
    w('    default: break;\n    }\n}\n\n')
    w('static so_shim glrec_shims[OP_N + 1] = {\n')
    for ret, name, ps in funcs:
        w('    { "%s", (void *)w_%s },\n' % (name, name))
    w('    { NULL, NULL },\n};\n')
    w('#endif\n')


if __name__ == '__main__':
    main()
