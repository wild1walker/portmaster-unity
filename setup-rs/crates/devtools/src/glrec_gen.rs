//! `pmu-dev glrec-gen <dir with gl32.h and gl2ext.h> > glrec_gen.h`: runtime/host/glrec_gen.h
//! (runtime/host/glrec.c's wrappers and replay) from Khronos' GLES headers (was runtime/host/glrec_gen.py;
//! the same header but its first line). The ua-host-armhf image has them: /usr/include/GLES3/gl32.h,
//! /usr/include/GLES2/gl2ext.h (a folder with both: `cp` them side by side, or pass /usr/include and
//! the tool looks in GLES3/ and GLES2/ too).
//!
//! Every GLES 3.2 entry point, and each extension entry point a Unity player names (NAMES below: the
//! GL names in the libunity.so of 15 players, 5.6 - 6000.6), gets a wrapper. A wrapper either records
//! the call into glrec.c's command buffer (calls that only set state or queue GPU work: their arguments
//! by value, the memory a pointer argument points at copied), or submits everything recorded so far and
//! calls through (anything that returns a value, writes through a pointer, or reads memory whose size is
//! not known here). The class of each function is decided below; glrec.c has the rest. UA_GL_REC_FILE (a
//! trace of every call to a file, glrec.c) uses the tr_<name> writers below, and glreplay.c (GLREC_PART 3)
//! the reader that calls a driver with them again, names mapped.
use std::collections::{BTreeMap, HashMap, HashSet};
use std::fmt::Write as _;
use std::path::Path;

// extension entry points Unity players name (core GLES 3.2 functions are all wrapped)
const NAMES: &str = "
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
";

// by value and void, but must not be deferred: submit first
const FLUSH_SCALAR: &str = "glFinish glLinkProgram glCompileShader glValidateProgram glDeleteShader glDeleteProgram
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
glBufferStorageEXT
glDeleteBuffers glDeleteVertexArrays glDeleteVertexArraysOES glVertexAttribBinding
glBindVertexBuffer glVertexAttribFormat glVertexAttribIFormat glVertexBindingDivisor";
// (texture definitions and copies between objects submit first: few a frame, and the frame graph
//  treats them as an end of what it knows. glDispatchCompute: writes the analysis cannot see.
//  Deleting buffers or vertex arrays, and GLES 3.1's separate vertex formats, change what the shadow
//  knows: submitted first, the shadow then asks the driver again.)

// returns a name and depends on nothing recorded: called at once, nothing submitted
const IMMEDIATE: &str = "glGenTextures glGenBuffers glGenFramebuffers glGenRenderbuffers glGenVertexArrays
glGenVertexArraysOES glGenSamplers glGenQueries glGenQueriesEXT glGenTransformFeedbacks
glGenProgramPipelines glCreateShader glCreateProgram glGetString glGetStringi glGetShaderPrecisionFormat
glGetGraphicsResetStatus glGetGraphicsResetStatusEXT glGetGraphicsResetStatusKHR";

// hand-written in glrec.c (glrec_custom_<name>, and its replay glrec_replay_<name>): buffer maps,
// Unity's way of writing dynamic geometry (a map, writes, a flush of the range, an unmap per draw);
// fences (glFenceSync submits what is held first, so every fence the game waits on or deletes is in the
// driver's stream behind everything recorded before it; with UA_GL_REC=thread a fence is held too, the
// game given a stand-in until the GL thread has made it); deleting buffers (held with UA_GL_REC=thread)
const CUSTOM: &str = "glMapBufferRange glFlushMappedBufferRange glUnmapBuffer glFenceSync glClientWaitSync glDeleteSync
glGetSynciv glIsSync glWaitSync glDeleteBuffers glShaderSource";

// draws: the indices parameter (an offset into the element buffer, or client memory: copied), or none
const DRAWS: [(&str, bool); 21] = [
    ("glDrawArrays", false), ("glDrawArraysInstanced", false), ("glDrawArraysInstancedEXT", false),
    ("glDrawArraysInstancedBaseInstanceEXT", false), ("glDrawArraysIndirect", false), ("glDrawElementsIndirect", false),
    ("glDrawElements", true), ("glDrawElementsInstanced", true), ("glDrawElementsInstancedEXT", true),
    ("glDrawRangeElements", true), ("glDrawElementsBaseVertex", true), ("glDrawElementsBaseVertexEXT", true),
    ("glDrawElementsBaseVertexOES", true), ("glDrawRangeElementsBaseVertex", true),
    ("glDrawRangeElementsBaseVertexEXT", true), ("glDrawRangeElementsBaseVertexOES", true),
    ("glDrawElementsInstancedBaseVertex", true), ("glDrawElementsInstancedBaseVertexEXT", true),
    ("glDrawElementsInstancedBaseVertexOES", true), ("glDrawElementsInstancedBaseInstanceEXT", true),
    ("glDrawElementsInstancedBaseVertexBaseInstanceEXT", true),
];

// recorded with a pointer kept as a number (an offset into a bound buffer; glrec.c's hook knows
// whether it is client memory and submits first then)
const POINTER_AS_VALUE: [(&str, &str); 4] = [
    ("glVertexAttribPointer", "pointer"), ("glVertexAttribIPointer", "pointer"),
    ("glDrawArraysIndirect", "indirect"), ("glDrawElementsIndirect", "indirect"),
];

// calls glrec.c follows (its vertex array shadow: whether a draw reads client memory), before the
// call is recorded; a nonzero result while recording submits what is recorded and calls through
const HOOKS: [(&str, &str); 13] = [
    ("glBindVertexArray", "glrec_vao_hook(rec_, array)"), ("glBindVertexArrayOES", "glrec_vao_hook(rec_, array)"),
    ("glBindBuffer", "glrec_buffer_hook(rec_, target, buffer)"),
    ("glVertexAttribPointer", "glrec_vap_hook(rec_, index)"), ("glVertexAttribIPointer", "glrec_vap_hook(rec_, index)"),
    ("glEnableVertexAttribArray", "glrec_attrib_hook(rec_, index, 1)"), ("glDisableVertexAttribArray", "glrec_attrib_hook(rec_, index, 0)"),
    ("glBindBufferBase", "glrec_buffer_hook(rec_, target, buffer)"), ("glBindBufferRange", "glrec_buffer_hook(rec_, target, buffer)"),
    ("glPixelStorei", "glrec_pixelstore_hook(rec_, pname, param)"),
    // (texture uploads: nonzero for a format or type whose size glrec.c does not know)
    ("glTexSubImage2D", "glrec_pixels_hook(rec_, format, type)"), ("glTexSubImage3D", "glrec_pixels_hook(rec_, format, type)"),
    ("", ""),
];

// texture uploads' pixels: an offset into the bound unpack buffer, or client memory (copied, its
// size from the pixel store state)
const PIXELS: [(&str, &str, &str); 4] = [
    ("glTexSubImage2D", "pixels", "glrec_pixels(pixels, width, height, 1, format, type)"),
    ("glTexSubImage3D", "pixels", "glrec_pixels(pixels, width, height, depth, format, type)"),
    ("glCompressedTexSubImage2D", "data", "glrec_cpixels(data, imageSize)"),
    ("glCompressedTexSubImage3D", "data", "glrec_cpixels(data, imageSize)"),
];

// calls glrec.c follows after they ran (any thread): which programs' shaders read the depth/stencil
// buffer through framebuffer fetch, and Unity's program binary cache
const POST: [(&str, &str); 8] = [
    ("glAttachShader", "glrec_attach_shader(program, shader)"),
    ("glLinkProgram", "glrec_link_program(program)"),
    ("glProgramBinary", "glrec_program_binary(program, binary, length)"),
    ("glProgramBinaryOES", "glrec_program_binary(program, binary, length)"),
    ("glGetProgramBinary", "glrec_get_program_binary(program, bufSize, length, binary)"),
    ("glGetProgramBinaryOES", "glrec_get_program_binary(program, bufSize, length, binary)"),
    ("glGetIntegerv", "glrec_get_integerv(pname, data)"),
    ("glDeleteProgram", "glrec_delete_program(program)"),
];

// which object a GLuint (or GLuint array) parameter names, by its name (part 3 maps them)
const KIND: [(&str, &str); 18] = [
    ("texture", "TEX"), ("textures", "TEX"), ("buffer", "BUF"), ("buffers", "BUF"), ("framebuffer", "FBO"),
    ("framebuffers", "FBO"), ("renderbuffer", "RBO"), ("renderbuffers", "RBO"), ("array", "VAO"), ("arrays", "VAO"),
    ("sampler", "SMP"), ("samplers", "SMP"), ("program", "PROG"), ("shader", "SH"), ("pipeline", "PIPE"),
    ("pipelines", "PIPE"), ("id", "QRY"), ("ids", "QRY"),
];
const XFB_FUNCS: [&str; 4] = ["glBindTransformFeedback", "glGenTransformFeedbacks", "glDeleteTransformFeedbacks", "glIsTransformFeedback"];
// replayed as they are recorded by hand (glreplay.c): maps, fences, sources, locations
const TR_CUSTOM_REPLAY: [&str; 23] = [
    "glMapBufferRange", "glFlushMappedBufferRange", "glUnmapBuffer", "glFenceSync", "glClientWaitSync",
    "glWaitSync", "glDeleteSync", "glShaderSource", "glGetUniformLocation", "glGetUniformBlockIndex",
    "glGetAttribLocation", "glGetProgramResourceLocation", "glGetProgramResourceIndex",
    "glGetFragDataLocation", "glCreateShader", "glCreateProgram", "glCreateShaderProgramv",
    "glUseProgram", "glReadPixels", "glMapBufferOES", "glUnmapBufferOES", "glMapBufferRangeEXT",
    "glFlushMappedBufferRangeEXT",
];
const PTR_TYPES: [&str; 5] = ["GLsync", "GLDEBUGPROC", "GLDEBUGPROCKHR", "GLeglImageOES", "GLeglClientBufferEXT"];
const FLOAT_TYPES: [&str; 2] = ["GLfloat", "GLclampf"];

type Params = Vec<(String, String)>;
struct Func {
    ret: String,
    name: String,
    ps: Params,
}

fn words(s: &str) -> HashSet<&str> {
    s.split_whitespace().collect()
}

fn norm(s: &str) -> String {
    s.split_whitespace().collect::<Vec<_>>().join(" ")
}

fn is_word(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

/// The prototypes `GL_APICALL <ret> GL_APIENTRY gl<name> (<params>);` of a header, in order.
fn parse(text: &str) -> Vec<Func> {
    let mut out = Vec::new();
    let mut pos = 0;
    while let Some(i) = text[pos..].find("GL_APICALL") {
        let start = pos + i;
        pos = start + 1;
        let rest = &text[start + "GL_APICALL".len()..];
        // GL_APICALL\s+(.+?)\s*GL_APIENTRY\s+(gl\w+)\s*\((.*?)\);
        if !rest.starts_with(|c: char| c.is_whitespace()) {
            continue;
        }
        let mut found = None;
        let mut from = 0;
        while let Some(j) = rest[from..].find("GL_APIENTRY") {
            let k = from + j;
            from = k + 1;
            let ret = rest[..k].trim_start();
            if ret.trim_end().is_empty() {
                continue;
            }
            let after = &rest[k + "GL_APIENTRY".len()..];
            let a2 = after.trim_start();
            if a2.len() == after.len() || !a2.starts_with("gl") {
                continue;
            }
            let nlen = a2.find(|c: char| !is_word(c)).unwrap_or(a2.len());
            let name = &a2[..nlen];
            let a3 = a2[nlen..].trim_start();
            let Some(a4) = a3.strip_prefix('(') else { continue };
            let Some(e) = a4.find(");") else { continue };
            let consumed = text.len() - a4.len() + e + 2;
            found = Some((norm(ret), name.to_string(), norm(&a4[..e]), consumed));
            break;
        }
        let Some((ret, name, params, end)) = found else { continue };
        pos = end;
        let mut ps = Vec::new();
        if !params.is_empty() && params != "void" {
            for p in params.split(',') {
                let p = p.trim();
                let w = p.len() - p.chars().rev().take_while(|&c| is_word(c)).map(char::len_utf8).sum::<usize>();
                ps.push((p[..w].trim().to_string(), p[w..].to_string()));
            }
        }
        out.push(Func { ret, name, ps });
    }
    out
}

fn base(t: &str) -> String {
    t.replace("const ", "").trim().to_string()
}

fn is_ptr(t: &str) -> bool {
    t.contains('*') || PTR_TYPES.contains(&base(t).as_str())
}

fn slot_in(t: &str, n: &str) -> String {
    let b = base(t);
    if FLOAT_TYPES.contains(&b.as_str()) {
        format!("rec_fbits({n})")
    } else if is_ptr(t) {
        format!("(uint64_t)(uintptr_t){n}")
    } else if b == "GLuint64" || b == "GLuint64EXT" {
        format!("(uint64_t){n}")
    } else {
        format!("(uint64_t)(int64_t){n}")
    }
}

fn slot_out(t: &str, i: usize) -> String {
    let b = base(t);
    if FLOAT_TYPES.contains(&b.as_str()) {
        format!("rec_bitsf(a[{i}])")
    } else if is_ptr(t) {
        format!("({t})(uintptr_t)a[{i}]")
    } else if b == "GLuint64" || b == "GLuint64EXT" {
        format!("({b})a[{i}]")
    } else {
        format!("({b})(int64_t)a[{i}]")
    }
}

fn kind(n: &str) -> Option<&'static str> {
    KIND.iter().find(|(k, _)| *k == n).map(|(_, v)| *v)
}

fn tr_kind(name: &str, t: &str, n: &str) -> Option<&'static str> {
    let b = t.replace("const ", "").replace('*', "");
    if b.trim() != "GLuint" {
        return None;
    }
    match kind(n) {
        Some("QRY") if XFB_FUNCS.contains(&name) => Some("XFB"),
        k => k,
    }
}

/// skipped on replay: queries, reads, debug labels, program binaries (the driver's own)
fn replayed(name: &str) -> bool {
    const SKIP: [&str; 13] = [
        "glGet", "glIs", "glCheck", "glReadnPixels", "glDebugMessage", "glObjectLabel", "glObjectPtrLabel", "glLabelObject",
        "glPushDebugGroup", "glPopDebugGroup", "glPushGroupMarker", "glPopGroupMarker", "glInsertEventMarker",
    ];
    if SKIP.iter().any(|p| name.starts_with(p)) {
        return false;
    }
    !["glProgramBinary", "glProgramBinaryOES", "glReleaseShaderCompiler", "glFinish", "glFlush", "glGetError", "glValidateProgram", "glShaderBinary"]
        .contains(&name)
}

fn read_header(d: &Path, name: &str, sub: &str) -> Result<String, String> {
    for p in [d.join(name), d.join(sub).join(name)] {
        if let Ok(t) = std::fs::read_to_string(&p) {
            return Ok(t);
        }
    }
    Err(format!("{}: no {name} (nor {sub}/{name})", d.display()))
}

pub fn run(args: &[String]) -> Result<(), String> {
    let d = args.first().map(String::as_str).unwrap_or("/usr/include");
    let d = Path::new(d);
    let core = parse(&read_header(d, "gl32.h", "GLES3")?);
    let ext = parse(&read_header(d, "gl2ext.h", "GLES2")?);
    print!("{}", generate(core, ext));
    Ok(())
}

pub fn generate(core: Vec<Func>, ext: Vec<Func>) -> String {
    let names = words(NAMES);
    let flush_scalar = words(FLUSH_SCALAR);
    let immediate = words(IMMEDIATE);
    let custom = words(CUSTOM);
    let draws: HashMap<&str, bool> = DRAWS.into_iter().collect();
    let pav: HashMap<&str, &str> = POINTER_AS_VALUE.into_iter().collect();
    let hooks: HashMap<String, String> = HOOKS
        .iter()
        .filter(|(k, _)| !k.is_empty())
        .map(|(k, v)| (k.to_string(), v.to_string()))
        .chain(DRAWS.iter().map(|(d, i)| (d.to_string(), format!("glrec_draw_hook(rec_, {}, {})", *i as u8, d.contains("Indirect") as u8))))
        .collect();
    let pixels: HashMap<&str, (&str, &str)> = PIXELS.iter().map(|(f, p, e)| (*f, (*p, *e))).collect();
    let post: HashMap<&str, &str> = POST.into_iter().collect();
    // pointer arguments whose memory is copied when the call is recorded: {function: {param: C size}}
    let mut copy: HashMap<String, BTreeMap<String, String>> = HashMap::new();
    let one = |p: &str, e: String| BTreeMap::from([(p.to_string(), e)]);
    for (n, k) in [("1", 1), ("2", 2), ("3", 3), ("4", 4)] {
        for t in ["f", "i", "ui"] {
            for f in [format!("glUniform{n}{t}v"), format!("glProgramUniform{n}{t}v"), format!("glProgramUniform{n}{t}vEXT")] {
                copy.insert(f, one("value", format!("count * {k} * 4")));
            }
        }
    }
    for (m, k) in [("2", 4), ("3", 9), ("4", 16), ("2x3", 6), ("3x2", 6), ("2x4", 8), ("4x2", 8), ("3x4", 12), ("4x3", 12)] {
        for f in [format!("glUniformMatrix{m}fv"), format!("glProgramUniformMatrix{m}fv"), format!("glProgramUniformMatrix{m}fvEXT")] {
            copy.insert(f, one("value", format!("count * {k} * 4")));
        }
    }
    let tp = "(pname == 0x1004 ? 4 : 1) * 4";
    for (f, p, e) in [
        ("glBufferSubData", "data", "size"),
        ("glBufferData", "data", "data ? size : 0"),
        ("glInvalidateFramebuffer", "attachments", "numAttachments * 4"),
        ("glInvalidateSubFramebuffer", "attachments", "numAttachments * 4"),
        ("glDiscardFramebufferEXT", "attachments", "numAttachments * 4"),
        ("glDrawBuffers", "bufs", "n * 4"),
        ("glClearBufferfv", "value", "(buffer == 0x1800 ? 4 : 1) * 4"),
        ("glClearBufferiv", "value", "(buffer == 0x1800 ? 4 : 1) * 4"),
        ("glClearBufferuiv", "value", "16"),
        ("glVertexAttrib1fv", "v", "4"), ("glVertexAttrib2fv", "v", "8"), ("glVertexAttrib3fv", "v", "12"),
        ("glVertexAttrib4fv", "v", "16"), ("glVertexAttribI4iv", "v", "16"), ("glVertexAttribI4uiv", "v", "16"),
        ("glTexParameteriv", "params", tp), ("glTexParameterfv", "params", tp),
        ("glTexParameterIiv", "params", tp), ("glTexParameterIuiv", "params", tp),
        ("glSamplerParameteriv", "param", tp), ("glSamplerParameterfv", "param", tp),
        ("glSamplerParameterIiv", "param", tp), ("glSamplerParameterIuiv", "param", tp),
        ("glDeleteTextures", "textures", "n * 4"),
        ("glDeleteFramebuffers", "framebuffers", "n * 4"), ("glDeleteRenderbuffers", "renderbuffers", "n * 4"),
        ("glDeleteSamplers", "samplers", "count * 4"), ("glDeleteQueries", "ids", "n * 4"),
        ("glDeleteQueriesEXT", "ids", "n * 4"), ("glDeleteTransformFeedbacks", "ids", "n * 4"),
        // debug groups and markers (a string: its length, or up to its end)
        ("glPushDebugGroup", "message", "length < 0 ? strlen(message) + 1 : (size_t)length"),
        ("glPushDebugGroupKHR", "message", "length < 0 ? strlen(message) + 1 : (size_t)length"),
        ("glPushGroupMarkerEXT", "marker", "length <= 0 ? strlen(marker) + 1 : (size_t)length"),
        ("glInsertEventMarkerEXT", "marker", "length <= 0 ? strlen(marker) + 1 : (size_t)length"),
    ] {
        copy.insert(f.to_string(), one(p, e.to_string()));
    }
    // (glDeleteBuffers, glDeleteVertexArrays[OES]: submitted first, nothing copied)
    // ---- UA_GL_REC_FILE: pointer arguments written with the memory they point at
    let mut tr: HashMap<String, BTreeMap<String, String>> = HashMap::new();
    for (f, d) in &copy {
        tr.insert(f.clone(), d.iter().map(|(p, e)| (p.clone(), format!("tr_blob({p}, (size_t)({e}))"))).collect());
    }
    for (f, p) in [("glDeleteBuffers", "buffers"), ("glDeleteVertexArrays", "arrays"), ("glDeleteVertexArraysOES", "arrays")] {
        tr.insert(f.into(), one(p, format!("tr_blob({p}, (size_t)n * 4)")));
    }
    for f in ["glTexSubImage2D", "glTexImage2D"] {
        tr.insert(f.into(), one("pixels", "tr_pix(pixels, tr_pixsize(width, height, 1, format, type))".into()));
    }
    for f in ["glTexSubImage3D", "glTexImage3D"] {
        tr.insert(f.into(), one("pixels", "tr_pix(pixels, tr_pixsize(width, height, depth, format, type))".into()));
    }
    for f in ["glCompressedTexSubImage2D", "glCompressedTexSubImage3D", "glCompressedTexImage2D", "glCompressedTexImage3D"] {
        tr.insert(f.into(), one("data", "tr_pix(data, (size_t)(imageSize > 0 ? imageSize : 0))".into()));
    }
    for (d, i) in DRAWS {
        if i {
            tr.insert(d.into(), one("indices", "tr_idx(indices, count, type)".into()));
        }
    }
    for f in ["glVertexAttribPointer", "glVertexAttribIPointer"] {
        tr.insert(f.into(), one("pointer", "tr_vap(pointer)".into()));
    }
    for (f, p) in [
        ("glGetUniformLocation", "name"), ("glGetAttribLocation", "name"), ("glBindAttribLocation", "name"),
        ("glGetFragDataLocation", "name"), ("glGetUniformBlockIndex", "uniformBlockName"),
        ("glGetProgramResourceIndex", "name"), ("glGetProgramResourceLocation", "name"),
    ] {
        tr.insert(f.into(), one(p, format!("tr_str({p})")));
    }

    let mut funcs: Vec<Func> = Vec::new();
    let mut seen = HashSet::new();
    for f in core.into_iter().chain(ext.into_iter().filter(|f| names.contains(f.name.as_str()))) {
        if seen.insert(f.name.clone()) {
            funcs.push(f);
        }
    }
    funcs.sort_by(|a, b| a.name.cmp(&b.name));
    let mut classes: HashMap<String, &str> = HashMap::new();
    for f in &funcs {
        let name = f.name.as_str();
        let ptrs: Vec<&(String, String)> = f.ps.iter().filter(|p| is_ptr(&p.0)).collect();
        let mut c = if immediate.contains(name) {
            "imm"
        } else if custom.contains(name) {
            "custom"
        } else if pixels.contains_key(name) {
            "rec"
        } else if flush_scalar.contains(name) || f.ret != "void" {
            "flush"
        } else if draws.contains_key(name) || copy.contains_key(name) {
            "rec"
        } else if ptrs.is_empty() || ptrs.iter().all(|p| Some(p.1.as_str()) == pav.get(name).copied()) {
            "rec"
        } else {
            "flush"
        };
        const READS: [&str; 10] =
            ["glGet", "glIs", "glCheck", "glMap", "glUnmap", "glFlushMapped", "glReadPixels", "glReadnPixels", "glClientWaitSync", "glFenceSync"];
        if READS.iter().any(|p| name.starts_with(p)) {
            c = if immediate.contains(name) {
                "imm"
            } else if custom.contains(name) {
                "custom"
            } else {
                "flush"
            };
        }
        classes.insert(f.name.clone(), c);
    }
    let count = |k: &str| classes.values().filter(|c| **c == k).count();
    let mut w = String::new();
    w += "// Generated by pmu-dev glrec-gen from Khronos' GLES 3.2 headers (gl32.h, gl2ext.h): do not edit.\n";
    writeln!(
        w,
        "// {} entry points: {} recorded, {} submit first, {} called at once, {} by hand (glrec.c).",
        funcs.len(),
        count("rec"),
        count("flush"),
        count("imm"),
        count("custom")
    )
    .unwrap();
    w += "// GLREC_PART 1: the ops, names, classes, types and next layer; 2: the wrappers, the replay, the table.\n";
    w += "#if !defined(GLREC_PART) || GLREC_PART == 1\n";
    w += "enum {\n";
    for f in &funcs {
        writeln!(w, "    OP_{},", f.name).unwrap();
    }
    w += "    OP_N\n};\n";
    w += "static const char *const __attribute__((unused)) glrec_name[OP_N] = {\n";
    for f in &funcs {
        writeln!(w, "    \"{}\",", f.name).unwrap();
    }
    w += "};\n";
    w += "static const unsigned char __attribute__((unused)) glrec_class[OP_N] = {   // 1 recorded, 2 submits first, 3 at once, 4 by hand\n";
    for f in &funcs {
        let n = match classes[&f.name] {
            "rec" => 1,
            "flush" => 2,
            "imm" => 3,
            _ => 4,
        };
        writeln!(w, "    {n},   // {}", f.name).unwrap();
    }
    w += "};\n";
    for f in &funcs {
        let ts: Vec<&str> = f.ps.iter().map(|p| p.0.as_str()).collect();
        let ts = if ts.is_empty() { "void".to_string() } else { ts.join(", ") };
        writeln!(w, "typedef {} (*T_{})({ts});", f.ret, f.name).unwrap();
    }
    w += "static void *glrec_next[OP_N] __attribute__((unused));\n";
    w += "#define NX(f) ((T_##f)glrec_next[OP_##f])\n";
    w += "#endif\n\n#if !defined(GLREC_PART) || GLREC_PART == 2\n";
    w += "\n// UA_GL_REC_FILE: each call written after it ran (glrec.c's tr_* helpers; replayed by glreplay.c)\n";
    let decl_of = |ps: &Params| ps.iter().map(|(t, n)| format!("{t} {n}")).collect::<Vec<_>>().join(", ");
    let empty = BTreeMap::new();
    for f in &funcs {
        let (name, ps) = (f.name.as_str(), &f.ps);
        if classes[name] == "custom" {
            continue;
        }
        let mut decl = decl_of(ps);
        if f.ret != "void" {
            decl = if decl.is_empty() { format!("{} r_", f.ret) } else { format!("{decl}, {} r_", f.ret) };
        }
        let trd = tr.get(name).unwrap_or(&empty);
        let mut sl = Vec::new();
        for (i, (t, n)) in ps.iter().enumerate() {
            if let Some(e) = trd.get(n) {
                sl.push(format!("a_[{i}] = {e};"));
            } else if name.starts_with("glGen") && is_ptr(t) && ps.len() == 2 {
                let c = &ps[0].1;
                sl.push(format!("a_[{i}] = tr_blob({n}, (size_t)({c} > 0 ? {c} : 0) * 4);"));
            } else {
                sl.push(format!("a_[{i}] = {};", slot_in(t, n)));
            }
        }
        let rs = if f.ret != "void" { format!(" tr_ret({});", slot_in(&f.ret, "r_")) } else { String::new() };
        writeln!(
            w,
            "static void tr_{name}({}) {{ {}tr_begin(OP_{name}, {});{}{rs} tr_end(); }}",
            if decl.is_empty() { "void" } else { &decl },
            if sl.is_empty() { "" } else { "uint64_t *a_ = " },
            ps.len(),
            if sl.is_empty() { String::new() } else { format!(" {}", sl.join(" ")) }
        )
        .unwrap();
    }
    w += "#define TRC(f, ...) do { if (g_tr) tr_##f(__VA_ARGS__); } while (0)\n\n";
    for f in &funcs {
        let (name, ps, ret) = (f.name.as_str(), &f.ps, f.ret.as_str());
        let hasf = ps.iter().any(|p| FLOAT_TYPES.contains(&base(&p.0).as_str()) && !is_ptr(&p.0));
        let decl = {
            let d = decl_of(ps);
            if d.is_empty() { "void".to_string() } else { d }
        };
        let args = ps.iter().map(|p| p.1.as_str()).collect::<Vec<_>>().join(", ");
        let abi = if hasf { "" } else { "" };
        let c = classes[name];
        let rv = if ret != "void" { "return " } else { "" };
        let mut tra = vec![name.to_string()];
        tra.extend(ps.iter().map(|p| p.1.clone()));
        if ret != "void" {
            tra.push("r_".into());
        }
        let trargs = tra.join(", ");
        if c == "imm" {
            if ret == "void" {
                writeln!(w, "static {abi}{ret} w_{name}({decl}) {{ glrec_imm(OP_{name}); uint64_t t_ = glrec_t0(); NX({name})({args}); glrec_t1(OP_{name}, t_); TRC({trargs}); }}").unwrap();
            } else {
                writeln!(w, "static {abi}{ret} w_{name}({decl}) {{ glrec_imm(OP_{name}); uint64_t t_ = glrec_t0(); {ret} r_ = NX({name})({args}); glrec_t1(OP_{name}, t_); TRC({trargs}); return r_; }}").unwrap();
            }
            continue;
        }
        if c == "custom" {
            writeln!(w, "static {abi}{ret} w_{name}({decl}) {{ {rv}glrec_custom_{name}({args}); }}").unwrap();
            continue;
        }
        if c == "flush" {
            if let Some(p) = post.get(name) {
                assert_eq!(ret, "void");
                writeln!(w, "static {abi}{ret} w_{name}({decl}) {{ glrec_submit_for(OP_{name}); uint64_t t_ = glrec_t0(); NX({name})({args}); glrec_tadd(OP_{name}, t_); {p}; TRC({trargs}); }}").unwrap();
            } else if ret == "void" {
                writeln!(w, "static {abi}{ret} w_{name}({decl}) {{ glrec_submit_for(OP_{name}); uint64_t t_ = glrec_t0(); NX({name})({args}); glrec_tadd(OP_{name}, t_); TRC({trargs}); }}").unwrap();
            } else {
                writeln!(w, "static {abi}{ret} w_{name}({decl}) {{ glrec_submit_for(OP_{name}); uint64_t t_ = glrec_t0(); {ret} r_ = NX({name})({args}); glrec_tadd(OP_{name}, t_); TRC({trargs}); return r_; }}").unwrap();
            }
            continue;
        }
        write!(w, "static {abi}{ret} w_{name}({decl})\n{{\n    int rec_ = glrec_recording();\n").unwrap();
        if let Some(h) = hooks.get(name) {
            // (the hook follows the call either way; nonzero while recording: submit and call through)
            writeln!(w, "    if ({h} && rec_) {{ glrec_submit_for(OP_{name}); rec_ = 0; }}").unwrap();
        }
        writeln!(w, "    if (!rec_) {{ uint64_t t_ = glrec_t0(); NX({name})({args}); glrec_t1(OP_{name}, t_); TRC({trargs}); return; }}").unwrap();
        let cp = copy.get(name).unwrap_or(&empty);
        let mut sl = Vec::new();
        for (i, (t, n)) in ps.iter().enumerate() {
            if let Some(e) = cp.get(n) {
                sl.push(format!("rec_a[{i}] = glrec_blob({n}, (size_t)({e}));"));
            } else if draws.get(name) == Some(&true) && n == "indices" {
                sl.push(format!("rec_a[{i}] = glrec_indices({n}, count, type);"));
            } else if pixels.get(name).is_some_and(|p| p.0 == n) {
                sl.push(format!("rec_a[{i}] = {};", pixels[name].1));
            } else {
                sl.push(format!("rec_a[{i}] = {};", slot_in(t, n)));
            }
        }
        if ps.is_empty() {
            writeln!(w, "    glrec_cmd(OP_{name}, 0);\n}}").unwrap();
        } else {
            writeln!(w, "    uint64_t *rec_a = glrec_cmd(OP_{name}, {});\n    {}\n}}", ps.len(), sl.join(" ")).unwrap();
        }
    }
    // replay
    w += "\n// one recorded call, to the next layer (blobs are pointers again)\n";
    w += "static void glrec_call(unsigned op, const uint64_t *a)\n{\n    switch (op) {\n";
    for f in &funcs {
        let (name, ps) = (f.name.as_str(), &f.ps);
        if classes[name] == "custom" {
            writeln!(w, "    case OP_{name}: glrec_replay_{name}(a); break;").unwrap();
            continue;
        }
        if classes[name] != "rec" {
            continue;
        }
        let cp = copy.get(name).unwrap_or(&empty);
        let mut outs = Vec::new();
        for (i, (t, n)) in ps.iter().enumerate() {
            if cp.contains_key(n) || (draws.get(name) == Some(&true) && n == "indices") || pixels.get(name).is_some_and(|p| p.0 == n) {
                outs.push(format!("({t})glrec_ptr(a[{i}])"));
            } else {
                outs.push(slot_out(t, i));
            }
        }
        writeln!(w, "    case OP_{name}: NX({name})({}); break;", outs.join(", ")).unwrap();
    }
    w += "    default: break;\n    }\n}\n\n";
    w += "static so_shim glrec_shims[OP_N + 1] = {\n";
    for f in &funcs {
        writeln!(w, "    {{ \"{0}\", (void *)w_{0} }},", f.name).unwrap();
    }
    w += "    { NULL, NULL },\n};\n";
    w += "#endif\n";
    // part 3: glreplay.c's decoder for UA_GL_REC_FILE traces
    w += "\n#if defined(GLREC_PART) && GLREC_PART == 3\n";
    w += "// what replay does with each entry point: 0 skipped (a query, a read, a label), 1 below, 2 glreplay.c by hand\n";
    w += "static const unsigned char rp_how[OP_N] = {\n";
    for f in &funcs {
        let name = f.name.as_str();
        let h = if TR_CUSTOM_REPLAY.contains(&name) {
            2
        } else if replayed(name) && classes[name] != "custom" {
            1
        } else {
            0
        };
        writeln!(w, "    {h},   // {name}").unwrap();
    }
    w += "};\n";
    w += "static void rp_call(unsigned op, const uint64_t *a, uint64_t r)\n{\n    (void)r;\n    switch (op) {\n";
    for f in &funcs {
        let (name, ps) = (f.name.as_str(), &f.ps);
        if TR_CUSTOM_REPLAY.contains(&name) {
            writeln!(w, "    case OP_{name}: rp_custom_{name}(a, r); break;").unwrap();
            continue;
        }
        if !replayed(name) || classes[name] == "custom" {
            continue;
        }
        let gen = name.starts_with("glGen") && ps.len() == 2 && is_ptr(&ps[1].0);
        if gen {
            let k = tr_kind(name, &ps[1].0, &ps[1].1).unwrap_or_else(|| panic!("{name}"));
            writeln!(w, "    case OP_{name}: {{ GLsizei n_ = (GLsizei)(int64_t)a[0]; GLuint *o_ = rp_out(n_); RX({name})(n_, o_); rp_gen(MK_{k}, (const GLuint *)rp_ptr(a[1]), o_, n_); break; }}").unwrap();
            continue;
        }
        let prog_param = ps.iter().any(|p| p.1 == "program");
        let mut outs = Vec::new();
        for (i, (t, n)) in ps.iter().enumerate() {
            let b = base(t);
            let k = tr_kind(name, t, n);
            if is_ptr(t) && PTR_TYPES.contains(&b.as_str()) {
                outs.push(if b == "GLsync" { format!("({t})rp_sync(a[{i}])") } else { format!("({t})(uintptr_t)a[{i}]") });
            } else if is_ptr(t) {
                match kind(n) {
                    Some(kk) if name.starts_with("glDelete") => {
                        let kk = if kk == "QRY" && XFB_FUNCS.contains(&name) { "XFB" } else { kk };
                        outs.push(format!("({t})rp_maparr(MK_{kk}, rp_ptr(a[{i}]), (GLsizei)(int64_t)a[0])"));
                    }
                    _ => outs.push(format!("({t})rp_ptr(a[{i}])")),
                }
            } else if let Some(k) = k {
                outs.push(format!("rp_map(MK_{k}, (GLuint)a[{i}])"));
            } else if n == "location" && b == "GLint" && (name.starts_with("glUniform") || name.starts_with("glProgramUniform")) {
                outs.push(format!("rp_loc({}, (GLint)(int64_t)a[{i}])", if prog_param { "(GLuint)a[0]" } else { "rp_prog" }));
            } else if n == "uniformBlockIndex" {
                outs.push(format!("rp_ubi((GLuint)a[0], (GLuint)a[{i}])"));
            } else {
                outs.push(slot_out(t, i));
            }
        }
        writeln!(w, "    case OP_{name}: RX({name})({}); break;", outs.join(", ")).unwrap();
    }
    w += "    default: break;\n    }\n}\n#endif\n";
    w
}
