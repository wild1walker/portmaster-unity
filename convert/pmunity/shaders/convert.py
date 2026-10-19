"""GLES3 programs for Shader assets that only carry OpenGL Core (and Metal / Vulkan) programs.

Unity's Android player renders with GLES3 and only runs GPU programs of the GLES types. A game
built for macOS or Linux has OpenGL Core programs (GLSL 1.50) instead, which are close to GLSL ES
3.00 (glsl.py). For every Shader:

  1. the GLCore platform blob is copied as a GLES3Plus blob (platform 9, the one Unity's GLES3
     renderer reads), each entry used by a GLCore32 subprogram translated to GLSL ES 3.00 and
     retyped GLES3 (gpu program type 4); entries nothing uses that way are copied unchanged so the
     blob indices stay the same;
  2. every GLCore32 SerializedSubProgram in the parsed form gets a GLES3 twin: a copy with
     m_GpuProgramType 4 and the same m_BlobIndex. Parameters, bindings, keywords and channels are
     the GLCore ones, which the GL and GLES backends read the same way (both bind by name).

GLCore41 / GLCore43 programs (desktop-only variants) get no twin. A shader with a program that
cannot be translated (glsl.Unsupported) gets no GLES3 programs at all, so Unity does not mix
translated and missing variants; the report names it.
"""
import copy

from . import blob as B
from . import glsl

STAGES = ('progVertex', 'progFragment', 'progGeometry', 'progHull', 'progDomain')
GLES_TYPES = (B.GPU_GLES31AEP, B.GPU_GLES31, B.GPU_GLES3, B.GPU_GLES)


def subprograms(tree):
    """Every SerializedSubProgram list of the shader: (pass, stage name, list)."""
    for ss in tree['m_ParsedForm']['m_SubShaders']:
        for p in ss['m_Passes']:
            for st in STAGES:
                prog = p.get(st)
                if prog and prog.get('m_SubPrograms') is not None:
                    yield p, st, prog['m_SubPrograms']


class Result:
    def __init__(self, name):
        self.name = name
        self.status = 'unchanged'       # converted | unchanged | skipped
        self.why = ''
        self.programs = 0               # translated blob entries
        self.twins = 0                  # GLES3 subprograms added
        self.guessed_layout = False
        self.notes = []                 # rewrites applied (glsl.OPTIMIZE)

    def __repr__(self):
        return f'{self.name}: {self.status} {self.why}'.rstrip()


def add_gles3(tree, precision='highp', replace=False, translate=None, optimize=glsl.OPTIMIZE):
    """Adds GLES3 programs to one Shader typetree in place. Returns a Result.
    replace: a shader that already has GLES programs gets them rebuilt from its GLCore ones
    (otherwise it is left alone). translate: function(code bytes) -> code bytes, default glsl.program_to_es with
    `precision` and the `optimize` rewrites."""
    name = tree['m_ParsedForm']['m_Name']
    r = Result(name)
    plats = list(tree['platforms'])
    if B.PLATFORM_GLCORE not in plats:
        r.why = 'no OpenGL Core programs'
        return r
    if B.PLATFORM_GLES3PLUS in plats and not replace:
        r.why = 'already has GLES3 programs'
        return r
    blobs = B.ShaderBlobs(tree)
    entries = blobs.entries(B.PLATFORM_GLCORE)
    used = sorted({sp['m_BlobIndex'] for _, _, lst in subprograms(tree) for sp in lst
                   if sp['m_GpuProgramType'] == B.GPU_GLCORE32})
    if not used:
        r.why = 'no GLCore32 programs'
        return r
    tr = translate or (lambda code: glsl.program_to_es(code.decode('utf-8'), precision, optimize, r.notes).encode('utf-8'))
    out = [e.copy() if e is not None else None for e in entries]
    for i in used:
        e = out[i]
        if e is None:
            continue
        r.guessed_layout |= e.guessed
        if e.code:
            try:
                e.code = tr(e.code)
            except glsl.Unsupported as ex:
                r.status, r.why = 'skipped', f'program {i}: {ex}'
                return r
            r.programs += 1
        e.gpu_type = B.GPU_GLES3
    # commit: blob, then the twins (any older GLES subprograms go first when replacing)
    blobs.remove(B.PLATFORM_GLES3PLUS)
    blobs.set_entries(B.PLATFORM_GLES3PLUS, out)
    blobs.store()
    for _, _, lst in subprograms(tree):
        if replace:
            lst[:] = [sp for sp in lst if sp['m_GpuProgramType'] not in GLES_TYPES]
        twins = []
        for sp in lst:
            if sp['m_GpuProgramType'] == B.GPU_GLCORE32:
                t = copy.deepcopy(sp)
                t['m_GpuProgramType'] = B.GPU_GLES3
                twins.append(t)
        lst.extend(twins)
        r.twins += len(twins)
    r.status = 'converted'
    return r


def gles3_programs(tree):
    """[(blob index, code str)] of the shader's GLES3 programs referenced by GLES3 subprograms."""
    if B.PLATFORM_GLES3PLUS not in tree['platforms']:
        return []
    entries = B.ShaderBlobs(tree).entries(B.PLATFORM_GLES3PLUS)
    idx = sorted({sp['m_BlobIndex'] for _, _, lst in subprograms(tree) for sp in lst
                  if sp['m_GpuProgramType'] == B.GPU_GLES3})
    return [(i, entries[i].code.decode('utf-8')) for i in idx if entries[i] is not None and entries[i].code]
