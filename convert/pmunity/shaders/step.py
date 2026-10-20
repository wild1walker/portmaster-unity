"""Conversion step `shaders`: GLES3 programs for every Shader in a game's data (convert.py).

Files searched: the serialized files (globalgamemanagers, level*, *.assets), Resources/
unity_builtin_extra, and asset bundles under StreamingAssets whose names match `bundles` (a
regular expression; bundles are opened only to look for shaders, and rewritten only when one
changes). The player's own "unity default resources" comes with the Android player and already
has GLES3 programs.

`sources` names the desktop platforms to translate, in order of preference: 'glcore' (OpenGL
Core, macOS / Linux builds; the default) and 'd3d11' (Direct3D 11, Windows builds, with Unity's
HLSLcc: tools/hlslcc). A shader is translated from the first one it has.

`glcore4x`: also translate the GLCore41 / GLCore43 programs (`#pragma target 4.5` and up) and
give shaders whose variants were all stripped an empty GLES3 blob (convert.add_gles3). On by
default, for data of every Unity version; config `"shaders": {"glcore4x": false}` (or `pmunity
convert --set shaders.glcore4x=false`) keeps a port on the output the converter wrote before
(Cuphead's port, which was checked on the device that way, sets it).
"""
import os
import re

from ..unity_io import Loaded
from . import blob as B
from . import convert as C
from . import dxbc

SOURCES = {'glcore': B.PLATFORM_GLCORE, 'd3d11': B.PLATFORM_D3D11}


def glcore4x_for(path, glcore4x=None):
    """Whether the GLCore41/43 programs of a serialized file or bundle are translated (None: yes)."""
    return True if glcore4x is None else bool(glcore4x)


def shader_files(job, bundles=r'.'):
    files = job.list('', lambda n: n.endswith('.assets') or n == 'globalgamemanagers' or re.match(r'level\d+$', n))
    files += job.list('Resources', lambda n: n == 'unity_builtin_extra')
    if bundles:
        rx = re.compile(bundles)
        for d, _, names in os.walk(os.path.join(job.src, 'StreamingAssets')):
            rel = os.path.relpath(d, job.src)
            files += sorted(os.path.join(rel, n) for n in names
                            if rx.search(n) and not n.endswith(('.manifest', '.meta')) and _is_bundle(os.path.join(d, n)))
    return files


def _is_bundle(path):
    try:
        with open(path, 'rb') as f:
            return f.read(8) == b'UnityFS\0'
    except OSError:
        return False


def run(job, precision='highp', replace=False, bundles=r'.', log_each=False, optimize=None, sources=('glcore',),
        hlslcc=None, glcore4x=None):
    """Returns the per-shader results. hlslcc: the pmhlslcc binary (default dxbc.find_hlslcc());
    glcore4x: see the module docstring."""
    if glcore4x is None:
        glcore4x = (job.cfg.get('shaders') or {}).get('glcore4x')
    if glcore4x not in (None, True, False):
        raise ValueError(f'shaders.glcore4x: {glcore4x!r} (true or false)')
    for src in sources:
        if src not in SOURCES:
            raise ValueError(f'unknown shader source {src!r} (known: {", ".join(SOURCES)})')
    results = []
    changed_files = 0
    translator = None
    opt = C.glsl.OPTIMIZE if optimize is None else optimize
    for rel in shader_files(job, bundles):
        if job.is_done('shaders', rel):
            continue
        lf = Loaded(job.path_in(rel))
        gl4 = glcore4x_for(job.path_in(rel), glcore4x)
        n = 0
        for pid, o in lf.objects('Shader'):
            t = o.read_typetree()
            src = next((s for s in sources if SOURCES[s] in t['platforms']), sources[0])
            if src == 'd3d11':
                if translator is None:
                    translator = dxbc.Translator(hlslcc)
                r = C.add_gles3_d3d11(t, translator, precision=precision, replace=replace, optimize=opt,
                                      unity=getattr(lf.sf, 'unity_version', None))
            else:
                r = C.add_gles3(t, precision=precision, replace=replace, optimize=opt, glcore4x=gl4)
                r.glcore4x = gl4
            r.file = rel
            results.append(r)
            if r.status == 'converted' or r.changed:
                o.save_typetree(t)
                n += 1
            if r.status == 'skipped' or r.failed or log_each:
                job.log(f'shaders: {rel}: {r}')
            if r.guessed_layout:
                job.log(f'shaders: {rel}: {r.name}: program layout of an unverified Unity version (guessed, checked by round trip)')
        if n:
            job.write(rel, lf.save())
            changed_files += 1
        lf.close()
        job.mark_done('shaders', rel)
    if translator is not None:
        translator.close()
    conv = [r for r in results if r.status == 'converted']
    job.log(f'shaders: {len(conv)} shaders given GLES3 programs ({sum(r.programs for r in conv)} programs, '
            f'{sum(r.twins for r in conv)} subprograms) in {changed_files} files; '
            f'{sum(r.status == "skipped" for r in results)} not translatable; precision {precision}; '
            f'rewrites: {sum(len(r.notes) for r in conv)} stages ({", ".join(sorted({n.split(" ", 1)[1].rsplit(" ", 1)[0] for r in conv for n in r.notes})) or "none"})')
    es31 = sum(getattr(r, 'es31', 0) for r in conv)
    if es31:
        job.log(f'shaders: {es31} programs need GLSL ES 3.10 (multisampled textures, texture gather, bit-field functions, '
                f'storage buffers, binding points, images): GLES 3.1 programs, used where the device has OpenGL ES 3.1+')
    gl4 = [r for r in results if r.source == 'glcore' and getattr(r, 'glcore4x', False)]
    if gl4:
        sub = [w for r in gl4 if r.source == 'glcore' for w in r.failed]
        job.log(f'shaders: GLCore41/43 programs translated too: {sum(r.programs4x for r in gl4 if r.status == "converted")} '
                f'programs; {len(sub)} SubShaders left out (no ES equivalent); '
                f'{sum(r.empty for r in gl4)} shaders without programs (stripped) given an empty GLES3 blob')
    elif any(r.source == 'glcore' for r in results):
        job.log('shaders: GLCore41/43 programs not translated (glcore4x off)')
    vr = sum(r.dropped for r in conv if r.source == 'glcore')
    if vr:
        job.log(f'shaders: {vr} VR-only OpenGL Core variants not translatable (left out, never used on a handheld)')
    d3d = [r for r in results if r.source == 'd3d11']
    if d3d:
        job.log(f'shaders: from Direct3D 11: {sum(r.status == "converted" for r in d3d)} of {len(d3d)} shaders '
                f'({sum(r.programs for r in d3d if r.status == "converted")} programs); '
                f'{sum(len(r.failed) for r in d3d)} SubShaders left out; {sum(r.dropped for r in d3d)} VR-only variants and '
                f'{sum(r.dropped_instancing for r in d3d)} GPU instancing variants left out')
    return results
