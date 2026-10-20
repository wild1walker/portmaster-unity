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

A Windows build has Direct3D 11 programs instead (add_gles3_d3d11, dxbc.py): one blob entry per
stage, translated a variant (vertex + pixel shader) at a time with Unity's HLSLcc.
"""
import copy

from . import blob as B
from . import dxbc
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
        self.status = 'unchanged'       # converted | unchanged | skipped (converted: the tree changed)
        self.changed = False            # skipped, but older GLES programs were removed (save the tree)
        self.why = ''
        self.programs = 0               # translated blob entries
        self.twins = 0                  # GLES3 subprograms added
        self.guessed_layout = False
        self.notes = []                 # rewrites applied (glsl.OPTIMIZE)
        self.source = 'glcore'          # the platform translated: glcore | d3d11
        self.dropped = 0                # D3D11: VR-only variants left out (not translatable)
        self.failed = []                # D3D11: (subshader, why) of SubShaders left without GLES3 programs

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
    if B.PLATFORM_GLES3PLUS in plats:
        if not replace:
            r.why = 'already has GLES3 programs'
            return r
        _strip_gles(tree)          # rebuilt below, or (no translatable programs) left without any
        r.status, r.why = 'converted', '(old GLES programs removed)'
    blobs = B.ShaderBlobs(tree)
    entries = blobs.entries(B.PLATFORM_GLCORE)
    used = sorted({sp['m_BlobIndex'] for _, _, lst in subprograms(tree) for sp in lst
                   if sp['m_GpuProgramType'] == B.GPU_GLCORE32})
    if not used:
        r.why = ('no GLCore32 programs ' + r.why).strip()
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
                r.changed = bool(replace and B.PLATFORM_GLES3PLUS in plats)
                return r
            r.programs += 1
        e.gpu_type = B.GPU_GLES3
    # commit: the blob, then the twins
    blobs.set_entries(B.PLATFORM_GLES3PLUS, out)
    blobs.store()
    for _, _, lst in subprograms(tree):
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


def _strip_gles(tree):
    """Removes a shader's GLES3Plus blob and every GLES subprogram (tree edited in place)."""
    blobs = B.ShaderBlobs(tree)
    blobs.remove(B.PLATFORM_GLES3PLUS)
    blobs.store()
    for _, _, lst in subprograms(tree):
        lst[:] = [sp for sp in lst if sp['m_GpuProgramType'] not in GLES_TYPES]


def gles3_programs(tree):
    """[(blob index, code str)] of the shader's GLES3 programs referenced by GLES3 subprograms."""
    if B.PLATFORM_GLES3PLUS not in tree['platforms']:
        return []
    entries = B.ShaderBlobs(tree).entries(B.PLATFORM_GLES3PLUS)
    idx = sorted({sp['m_BlobIndex'] for _, _, lst in subprograms(tree) for sp in lst
                  if sp['m_GpuProgramType'] == B.GPU_GLES3})
    return [(i, entries[i].code.decode('utf-8')) for i in idx if entries[i] is not None and entries[i].code]


# ---------------------------------------------------------------- Direct3D 11 (Windows builds)

# variants only VR rendering asks for (stereo instancing writes gl_Layer, which ES 3.00 lacks):
# one of these that cannot be translated is left out; Unity never enables them on a handheld
VR_KEYWORDS = ('STEREO_INSTANCING_ON', 'STEREO_MULTIVIEW_ON', 'UNITY_SINGLE_PASS_STEREO', 'STEREO_CUBEMAP_RENDER_ON')
# SerializedSubProgram fields a GL subprogram leaves empty (its backend finds everything by name)
_D3D_ONLY_LISTS = ('m_VectorParams', 'm_MatrixParams', 'm_TextureParams', 'm_BufferParams', 'm_ConstantBuffers',
                   'm_ConstantBufferBindings', 'm_UAVParams', 'm_Samplers')
_OTHER_STAGES = ('progGeometry', 'progHull', 'progDomain')


def _variant_key(sp):
    return tuple(sorted(sp['m_KeywordIndices'])), sp.get('m_ShaderHardwareTier')


def gl_twin(sp, blob_index):
    """The GLES3 subprogram of a D3D11 one: Unity's GL subprograms have no parameters, bindings or
    vertex channels (the GL backends find uniforms, textures and inputs by name), only the source
    map, keywords, tier and requirements (compared with Unity's own GLES3 subprograms of the same
    shaders: identical but for the type and blob index)."""
    t = copy.deepcopy(sp)
    t['m_GpuProgramType'] = B.GPU_GLES3
    t['m_BlobIndex'] = blob_index
    for k in _D3D_ONLY_LISTS:
        if isinstance(t.get(k), list):
            t[k] = []
    ch = t.get('m_Channels')
    if isinstance(ch, dict) and isinstance(ch.get('m_Channels'), list):
        ch['m_Channels'] = []
    return t


def _check_bindings(p, *stages):
    """None if each (subprogram, entry)'s binding info agrees with the subprogram, else why not."""
    names = {i: n for n, i in p.get('m_NameIndices') or []}
    for sp, e in stages:
        if e is None or not e.code:
            return 'empty program entry'
        try:
            bad = dxbc.check_parsed_form(dxbc.Bindings.parse(e.rest), sp, names)
        except dxbc.Unsupported as ex:
            return str(ex)
        if bad:
            return 'binding info disagrees with the parsed form (' + ', '.join(bad[:3]) + ')'
    return None


def add_gles3_d3d11(tree, translator=None, precision='highp', replace=False, optimize=glsl.OPTIMIZE, translate=None):
    """Adds GLES3 programs translated from the shader's Direct3D 11 ones (tree edited in place).
    Returns a Result.

    Every pass's D3D11 vertex subprograms are paired with the pixel subprogram of the same
    variant (keywords and hardware tier); each pair becomes one GL program (dxbc.translate_program,
    then glsl.program_to_es as for a GLCore program), stored as Unity stores GL programs: the
    program in the vertex subprogram's entry, an entry without code for the fragment one, both
    retyped GLES3 with gl_twin() subprograms. The GLES3 blob keeps the D3D11 blob's indices (as in
    Unity's builds); a vertex entry shared by variants with different pixel shaders gets more.

    A SubShader with a variant that cannot be translated (a geometry or tessellation stage, UAVs,
    an untranslatable program) gets no GLES3 programs, so Unity uses the next SubShader or the
    Fallback, as for a SubShader an Android build has no programs for; VR-only variants
    (VR_KEYWORDS) that cannot be translated are left out instead (Result.dropped).
    translator: a dxbc.Translator; translate: function(vs Entry, ps Entry) -> GL program text
    (default dxbc.translate_program with translator)."""
    name = tree['m_ParsedForm']['m_Name']
    r = Result(name)
    r.source = 'd3d11'
    plats = list(tree['platforms'])
    if B.PLATFORM_D3D11 not in plats:
        r.why = 'no Direct3D 11 programs'
        return r
    if B.PLATFORM_GLES3PLUS in plats:
        if not replace:
            r.why = 'already has GLES3 programs'
            return r
        _strip_gles(tree)
        r.status, r.why = 'converted', '(old GLES programs removed)'
    if translate is None:
        if translator is None:
            raise ValueError('add_gles3_d3d11 needs a translator or a translate function')
        translate = lambda vs, ps: dxbc.translate_program(translator, (vs.code, vs.rest), (ps.code, ps.rest))
    blobs = B.ShaderBlobs(tree)
    entries = blobs.entries(B.PLATFORM_D3D11)
    cache = {}                          # (vs index, ps index) -> GLES program text | Unsupported

    def program(vi, pi):
        if (vi, pi) not in cache:
            vs, ps = entries[vi], entries[pi]
            try:
                if vs is None or ps is None or not vs.code or not ps.code:
                    raise dxbc.Unsupported('empty program entry')
                r.guessed_layout |= vs.guessed or ps.guessed
                notes = []
                gl = translate(vs, ps)
                cache[(vi, pi)] = glsl.program_to_es(gl, precision, optimize, notes)
                r.notes += notes
            except (dxbc.Unsupported, glsl.Unsupported) as ex:
                cache[(vi, pi)] = ex
        return cache[(vi, pi)]

    # plan: per SubShader, the (pass, vs, ps) variants to twin; any failure drops the SubShader
    plan = []
    for si, ss in enumerate(tree['m_ParsedForm']['m_SubShaders']):
        todo, why = [], None
        for pi_, p in enumerate(ss['m_Passes']):
            vlist = (p.get('progVertex') or {}).get('m_SubPrograms') or []
            flist = (p.get('progFragment') or {}).get('m_SubPrograms') or []
            vss = [sp for sp in vlist if sp['m_GpuProgramType'] in dxbc.VERTEX_TYPES]
            pss = {}
            for sp in flist:
                if sp['m_GpuProgramType'] in dxbc.PIXEL_TYPES:
                    pss.setdefault(_variant_key(sp), sp)
            others = {_variant_key(sp) for st in _OTHER_STAGES
                      for sp in ((p.get(st) or {}).get('m_SubPrograms') or [])
                      if sp['m_GpuProgramType'] in dxbc.OTHER_STAGE_TYPES}
            for vsp in vss:
                k = _variant_key(vsp)
                psp = pss.get(k)
                e = entries[vsp['m_BlobIndex']]
                kws = e.keywords if e is not None else []
                if psp is None:
                    err = 'no pixel shader for this variant'
                elif k in others:
                    err = 'geometry or tessellation stage (not in GLSL ES 3.00)'
                else:
                    err = _check_bindings(p, (vsp, entries[vsp['m_BlobIndex']]), (psp, entries[psp['m_BlobIndex']]))
                    if err is None:
                        res = program(vsp['m_BlobIndex'], psp['m_BlobIndex'])
                        err = str(res) if isinstance(res, Exception) else None
                if err is None:
                    todo.append((p, vsp, psp))
                elif any(kw in VR_KEYWORDS for kw in kws):
                    r.dropped += 1
                elif why is None:
                    why = f'pass {pi_} variant {" ".join(kws) or "(no keywords)"}: {err}'
        if why is None and todo:
            plan.append((si, todo))
        elif why is not None:
            r.failed.append((si, why))
    if not plan:
        r.status = 'skipped'
        r.why = r.failed[0][1] if r.failed else 'no Direct3D 11 vertex/pixel programs'
        r.changed = bool(replace and B.PLATFORM_GLES3PLUS in plats)
        return r

    # the GLES3 blob: D3D11 indices kept (entries without code where nothing GL uses them)
    out = [None if e is None else B.Entry(e.version, B.GPU_GLES3, [0] * len(e.stats), e.keywords, b'', dxbc.gl_rest(0))
           for e in entries]
    vslot, used = {}, set()
    twins = []                          # (subprogram list, twin)
    for si, todo in plan:
        for p, vsp, psp in todo:
            vi, fi = vsp['m_BlobIndex'], psp['m_BlobIndex']
            if (vi, fi) not in vslot:
                slot = vi if vi not in used else len(out)
                if slot == len(out):
                    out.append(None)
                used.add(slot)
                vslot[(vi, fi)] = slot
                e = entries[vi]
                out[slot] = B.Entry(e.version, B.GPU_GLES3, [0] * len(e.stats), e.keywords, cache[(vi, fi)].encode('utf-8'),
                                    dxbc.gl_rest(vsp['m_Channels']['m_SourceMap']))
                r.programs += 1
            used.add(fi)
            twins.append((p['progVertex']['m_SubPrograms'], gl_twin(vsp, vslot[(vi, fi)])))
            twins.append((p['progFragment']['m_SubPrograms'], gl_twin(psp, fi)))
    # commit: the blob, then the twins (each fragment subprogram once, however many vertex ones share it)
    blobs.set_entries(B.PLATFORM_GLES3PLUS, out)
    blobs.store()
    seen = set()
    for lst, t in twins:
        key = (id(lst), t['m_BlobIndex'], _variant_key(t))
        if key in seen:
            continue
        seen.add(key)
        lst.append(t)
        r.twins += 1
    r.status = 'converted'
    if r.failed:
        r.why = (r.why + ' ' if r.why else '') + '; '.join(f'SubShader {si} left out: {w}' for si, w in r.failed)
    return r
