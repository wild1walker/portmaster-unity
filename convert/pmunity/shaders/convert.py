"""GLES3 programs for Shader assets that only carry OpenGL Core (and Metal / Vulkan) programs.

Unity's Android player renders with GLES3 and only runs GPU programs of the GLES types. A game
built for macOS or Linux has OpenGL Core programs (GLSL 1.50) instead, which are close to GLSL ES
3.00 (glsl.py). For every Shader:

  1. the GLCore platform blob is copied as a GLES3Plus blob (platform 9, the one Unity's GLES3
     renderer reads), each entry used by a GLCore32 subprogram translated to GLSL ES 3.00 and
     retyped GLES3 (gpu program type 4); entries nothing uses that way (and, from Unity 2021.2,
     the parameter entries) are copied unchanged so the blob indices stay the same;
  2. every GLCore32 subprogram in the parsed form gets a GLES3 twin: a copy with
     m_GpuProgramType 4 and the same m_BlobIndex (2021.2+: the same parameter entry too).
     Parameters, bindings, keywords and channels are the GLCore ones, which the GL and GLES
     backends read the same way (both bind by name). This is what Unity's own builds hold: in
     the built-in resources of every LTS line 2017.4 - 2022.3, which carry both platforms, each
     GLES3 subprogram equals the GLCore one of the same variant but for the type, and the two
     blobs differ only in the code and type of the code entries;
  3. Unity 2020.1+: each pass with twins lists the GLES3 platform in m_Platforms; 2021.2+: the
     Shader's stageCounts gets the GLES3 platform's (as GLCore's, 1).

Where the subprograms are: up to Unity 2021.1 in each program's m_SubPrograms (a hardware tier
field each); from 2021.2 in m_PlayerSubPrograms, a list per hardware tier (desktop builds use the
last, tier-independent one) with the parallel m_ParameterBlobIndices (subprogram_lists()).

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
# the GPU program types whose code and parameter entries are in a platform's blob
PLATFORM_TYPES = {
    B.PLATFORM_GLCORE: (B.GPU_GLCORE32, B.GPU_GLCORE41, B.GPU_GLCORE43),
    B.PLATFORM_GLES3PLUS: (B.GPU_GLES31AEP, B.GPU_GLES31, B.GPU_GLES3),
    B.PLATFORM_GLES20: (B.GPU_GLES,),
    B.PLATFORM_D3D11: tuple(range(15, 23)),
}


def subprogram_lists(tree):
    """Every list of subprograms of the shader: (pass, stage name, list, parameter blob indices).
    Up to 2021.1 a program's m_SubPrograms (parameter indices None); from 2021.2 each hardware
    tier's m_PlayerSubPrograms list and the m_ParameterBlobIndices list parallel to it."""
    for ss in tree['m_ParsedForm']['m_SubShaders']:
        for p in ss['m_Passes']:
            for st in STAGES:
                prog = p.get(st)
                if not prog:
                    continue
                if prog.get('m_SubPrograms') is not None:
                    yield p, st, prog['m_SubPrograms'], None
                players = prog.get('m_PlayerSubPrograms')
                if players:
                    pbi = prog['m_ParameterBlobIndices']
                    for tier, lst in enumerate(players):
                        yield p, st, lst, pbi[tier]


def subprograms(tree):
    """Every list of subprograms of the shader: (pass, stage name, list)."""
    for p, st, lst, _ in subprogram_lists(tree):
        yield p, st, lst


def param_indices(tree, platform):
    """The blob indices of a platform's parameter entries (2021.2+; empty before)."""
    types = PLATFORM_TYPES.get(platform, ())
    return {pi[i] for _, _, lst, pi in subprogram_lists(tree) if pi is not None
            for i, sp in enumerate(lst) if sp['m_GpuProgramType'] in types}


def program_indices(tree, platform):
    """The blob indices of a platform's program entries (named by its subprograms' m_BlobIndex)."""
    types = PLATFORM_TYPES.get(platform, ())
    return {sp['m_BlobIndex'] for _, _, lst in subprograms(tree) for sp in lst if sp['m_GpuProgramType'] in types}


def platform_entries(tree, platform, blobs=None):
    """A platform's blob entries of a Shader typetree: Entry (programs), ParamEntry (2021.2+
    parameters), RawEntry (what no subprogram names and does not parse as a program) or None."""
    return (blobs or B.ShaderBlobs(tree)).entries(platform, param_indices(tree, platform), program_indices(tree, platform))


def add_platform(p, platform):
    """Lists a shader platform in a pass's m_Platforms (Unity 2020.1+), in ascending order."""
    plats = p.get('m_Platforms')
    if isinstance(plats, list) and platform not in plats:
        plats.append(platform)
        plats.sort()


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
        self.dropped = 0                # VR-only variants left out (not translatable)
        self.es31 = 0                   # programs that need GLSL ES 3.10 (GLES 3.1 programs, type 3)
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
    entries = platform_entries(tree, B.PLATFORM_GLCORE, blobs)
    used = sorted({sp['m_BlobIndex'] for _, _, lst in subprograms(tree) for sp in lst
                   if sp['m_GpuProgramType'] == B.GPU_GLCORE32})
    if not used:
        r.why = ('no GLCore32 programs ' + r.why).strip()
        return r
    tr = translate or (lambda code: glsl.program_to_es(code.decode('utf-8'), precision, optimize, r.notes).encode('utf-8'))
    tr31 = None if translate else \
        (lambda code: glsl.program_to_es(code.decode('utf-8'), precision, optimize, r.notes, es='310').encode('utf-8'))
    out = [e.copy() if e is not None else None for e in entries]
    dropped = set()                     # VR-only programs that do not translate: no twins
    es31 = set()                        # programs translated as GLES 3.1 ones
    for i in used:
        e = out[i]
        if e is None:
            continue
        if not isinstance(e, B.Entry):
            raise ValueError(f'{name}: blob entry {i} is both a program and parameters')
        r.guessed_layout |= e.guessed
        if e.code:
            try:
                try:
                    e.code = tr(e.code)
                except glsl.NeedsES31:
                    if tr31 is None:
                        raise
                    e.code = tr31(e.code)
                    es31.add(i)
            except glsl.Unsupported as ex:
                if any(k in VR_KEYWORDS for k in e.keywords + (e.local_keywords or [])):
                    dropped.add(i)      # e.g. STEREO_INSTANCING_ON writes gl_Layer (not in ES 3.00)
                    out[i] = entries[i].copy()
                    r.dropped += 1
                    continue
                r.status, r.why = 'skipped', f'program {i}: {ex}'
                r.changed = bool(replace and B.PLATFORM_GLES3PLUS in plats)
                return r
            r.programs += 1
        e.gpu_type = B.GPU_GLES3
    # the variants (per pass) whose program was left out: none of their subprograms get a twin;
    # those made GLES 3.1 programs: all their subprograms (and entries) are typed GLES 3.1
    skip = {(id(p), _variant_key(sp)) for p, _, lst in subprograms(tree) for sp in lst
            if sp['m_GpuProgramType'] == B.GPU_GLCORE32 and sp['m_BlobIndex'] in dropped}
    raised = {(id(p), _variant_key(sp)) for p, _, lst in subprograms(tree) for sp in lst
              if sp['m_GpuProgramType'] == B.GPU_GLCORE32 and sp['m_BlobIndex'] in es31}
    for p, _, lst in subprograms(tree):
        for sp in lst:
            if sp['m_GpuProgramType'] == B.GPU_GLCORE32 and (id(p), _variant_key(sp)) in raised:
                if out[sp['m_BlobIndex']] is not None:
                    out[sp['m_BlobIndex']].gpu_type = B.GPU_GLES31
    r.es31 = len(es31)
    # commit: the blob, then the twins
    stages = blobs.stage_counts.get(B.PLATFORM_GLCORE) if blobs.stage_counts else None
    blobs.set_entries(B.PLATFORM_GLES3PLUS, out, blobs.segments(B.PLATFORM_GLCORE), stages)
    blobs.store()
    for p, _, lst, pbi in subprogram_lists(tree):
        twins = []
        for i, sp in enumerate(lst):
            if sp['m_GpuProgramType'] == B.GPU_GLCORE32 and (id(p), _variant_key(sp)) not in skip:
                t = copy.deepcopy(sp)
                t['m_GpuProgramType'] = B.GPU_GLES31 if (id(p), _variant_key(sp)) in raised else B.GPU_GLES3
                twins.append((t, None if pbi is None else pbi[i]))
        for t, pi in twins:
            lst.append(t)
            if pbi is not None:
                pbi.append(pi)
        if twins:
            add_platform(p, B.PLATFORM_GLES3PLUS)
        r.twins += len(twins)
    r.status = 'converted'
    return r


def _strip_gles(tree):
    """Removes a shader's GLES3Plus blob and every GLES subprogram (tree edited in place)."""
    blobs = B.ShaderBlobs(tree)
    blobs.remove(B.PLATFORM_GLES3PLUS)
    blobs.store()
    for p, _, lst, pbi in subprogram_lists(tree):
        keep = [i for i, sp in enumerate(lst) if sp['m_GpuProgramType'] not in GLES_TYPES]
        if pbi is not None:
            pbi[:] = [pbi[i] for i in keep]
        lst[:] = [lst[i] for i in keep]
        plats = p.get('m_Platforms')
        if isinstance(plats, list) and B.PLATFORM_GLES3PLUS in plats:
            plats.remove(B.PLATFORM_GLES3PLUS)


def gles3_programs(tree):
    """[(blob index, code str)] of the shader's GLES3 programs referenced by GLES3 subprograms."""
    if B.PLATFORM_GLES3PLUS not in tree['platforms']:
        return []
    entries = platform_entries(tree, B.PLATFORM_GLES3PLUS)
    idx = sorted({sp['m_BlobIndex'] for _, _, lst in subprograms(tree) for sp in lst
                  if sp['m_GpuProgramType'] in (B.GPU_GLES3, B.GPU_GLES31)})
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
    """Keywords (2019.1 - 2021.1: global and local) and hardware tier of a subprogram."""
    kws = tuple(tuple(sorted(sp[k])) for k in ('m_KeywordIndices', 'm_GlobalKeywordIndices', 'm_LocalKeywordIndices') if k in sp)
    return kws, sp.get('m_ShaderHardwareTier')


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
            bad = dxbc.check_parsed_form(dxbc.Bindings.parse(e.rest, e.version), sp, names)
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
        translate = lambda vs, ps: dxbc.translate_program(translator, (vs.code, vs.rest), (ps.code, ps.rest), vs.version)
    blobs = B.ShaderBlobs(tree)
    entries = platform_entries(tree, B.PLATFORM_D3D11, blobs)
    newer = sorted({e.version for e in entries if isinstance(e, B.Entry) and e.version not in dxbc.SERIALIZERS})
    if newer:
        r.status = 'skipped'
        r.why = (f'Direct3D 11 programs of serializer {newer[0]} (Unity 2019.1+): not supported yet (only Unity '
                 f'2017.3 - 2018.4 builds; newer GL programs need constant buffer layouts the D3D11 ones do not give)')
        r.changed = bool(replace and B.PLATFORM_GLES3PLUS in plats)
        return r
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
        r.why = (r.failed[0][1] if r.failed else f'only VR-only variants, none translatable ({r.dropped})' if r.dropped
                 else 'no Direct3D 11 vertex/pixel programs')
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
                                    dxbc.gl_rest(dxbc.Bindings.parse(e.rest, e.version).source_map))
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
