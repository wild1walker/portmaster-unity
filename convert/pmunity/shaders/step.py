"""Conversion step `shaders`: GLES3 programs for every Shader in a game's data (convert.py).

Files searched: the serialized files (globalgamemanagers, level*, *.assets), Resources/
unity_builtin_extra, and asset bundles under StreamingAssets whose names match `bundles` (a
regular expression; bundles are opened only to look for shaders, and rewritten only when one
changes). The player's own "unity default resources" comes with the Android player and already
has GLES3 programs.
"""
import os
import re

from ..unity_io import Loaded
from . import convert as C


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


def run(job, precision='highp', replace=False, bundles=r'.', log_each=False, optimize=None):
    """Returns the per-shader results."""
    results = []
    changed_files = 0
    for rel in shader_files(job, bundles):
        if job.is_done('shaders', rel):
            continue
        lf = Loaded(job.path_in(rel))
        n = 0
        for pid, o in lf.objects('Shader'):
            t = o.read_typetree()
            r = C.add_gles3(t, precision=precision, replace=replace,
                            optimize=C.glsl.OPTIMIZE if optimize is None else optimize)
            r.file = rel
            results.append(r)
            if r.status == 'converted':
                o.save_typetree(t)
                n += 1
            if r.status == 'skipped' or log_each:
                job.log(f'shaders: {rel}: {r}')
            if r.guessed_layout:
                job.log(f'shaders: {rel}: {r.name}: program layout of an unverified Unity version (guessed, checked by round trip)')
        if n:
            job.write(rel, lf.save())
            changed_files += 1
        lf.close()
        job.mark_done('shaders', rel)
    conv = [r for r in results if r.status == 'converted']
    job.log(f'shaders: {len(conv)} shaders given GLES3 programs ({sum(r.programs for r in conv)} programs, '
            f'{sum(r.twins for r in conv)} subprograms) in {changed_files} files; '
            f'{sum(r.status == "skipped" for r in results)} not translatable; precision {precision}; '
            f'rewrites: {sum(len(r.notes) for r in conv)} stages ({", ".join(sorted({n.split(" ", 1)[1].rsplit(" ", 1)[0] for r in conv for n in r.notes})) or "none"})')
    return results
