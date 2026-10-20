"""Conversion step `shaders`: GLES3 programs for every Shader in a game's data (convert.py).

Files searched: the serialized files (globalgamemanagers, level*, *.assets), Resources/
unity_builtin_extra, and asset bundles under StreamingAssets whose names match `bundles` (a
regular expression; bundles are opened only to look for shaders, and rewritten only when one
changes). The player's own "unity default resources" comes with the Android player and already
has GLES3 programs.

`sources` names the desktop platforms to translate, in order of preference: 'glcore' (OpenGL
Core, macOS / Linux builds; the default) and 'd3d11' (Direct3D 11, Windows builds, with Unity's
HLSLcc: tools/hlslcc). A shader is translated from the first one it has.
"""
import os
import re

from ..unity_io import Loaded
from . import blob as B
from . import convert as C
from . import dxbc

SOURCES = {'glcore': B.PLATFORM_GLCORE, 'd3d11': B.PLATFORM_D3D11}


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
        hlslcc=None):
    """Returns the per-shader results. hlslcc: the pmhlslcc binary (default dxbc.find_hlslcc())."""
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
        n = 0
        for pid, o in lf.objects('Shader'):
            t = o.read_typetree()
            src = next((s for s in sources if SOURCES[s] in t['platforms']), sources[0])
            if src == 'd3d11':
                if translator is None:
                    translator = dxbc.Translator(hlslcc)
                r = C.add_gles3_d3d11(t, translator, precision=precision, replace=replace, optimize=opt)
            else:
                r = C.add_gles3(t, precision=precision, replace=replace, optimize=opt)
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
    vr = sum(r.dropped for r in conv if r.source == 'glcore')
    if vr:
        job.log(f'shaders: {vr} VR-only OpenGL Core variants not translatable (left out, never used on a handheld)')
    d3d = [r for r in results if r.source == 'd3d11']
    if d3d:
        job.log(f'shaders: from Direct3D 11: {sum(r.status == "converted" for r in d3d)} of {len(d3d)} shaders '
                f'({sum(r.programs for r in d3d if r.status == "converted")} programs); '
                f'{sum(len(r.failed) for r in d3d)} SubShaders left out; {sum(r.dropped for r in d3d)} VR-only variants left out')
    return results
