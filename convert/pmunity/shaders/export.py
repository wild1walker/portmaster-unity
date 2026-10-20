"""Export a game's GLES3 programs as one file per stage, for compile checks and GPU cost tests.

    python -m pmunity shaders-export <data> <dir>

Writes <dir>/<nnnn>.vert and <nnnn>.frag (each the source Unity hands the driver for that stage;
identical programs once) and <dir>/index.tsv: number, shader, file, blob index, keywords. The
runtime's tools/shadercheck compiles and links every pair with the device's own driver.
"""
import hashlib
import os

from ..unity_io import Loaded
from . import blob as B
from . import convert as C
from . import glsl
from .step import shader_files


# Unity 2019+ programs define their uniform and block layout qualifiers through a macro the player
# switches off where the GLSL version lacks them (libunity looks for this line); GLSL ES 3.00 has
# no `layout(location)` on uniforms nor `layout(binding)` on blocks
_UNIFORM_LOCATION = '#define UNITY_SUPPORTS_UNIFORM_LOCATION 1\n'


def as_driver_sees(src):
    """One stage's source as the player hands it to a GLES 3.0 driver (for compile checks)."""
    if src.lstrip().startswith('#version 300 es') and _UNIFORM_LOCATION in src:
        src = src.replace(_UNIFORM_LOCATION, '#define UNITY_SUPPORTS_UNIFORM_LOCATION 0\n')
    return src


def export(job, out_dir, bundles=r'.'):
    os.makedirs(out_dir, exist_ok=True)
    seen = {}
    rows = []
    for rel in shader_files(job, bundles):
        lf = Loaded(job.path_in(rel))
        for pid, o in lf.objects('Shader'):
            t = o.read_typetree()
            if B.PLATFORM_GLES3PLUS not in t['platforms']:
                continue
            entries = C.platform_entries(t, B.PLATFORM_GLES3PLUS)
            name = t['m_ParsedForm']['m_Name']
            for i, code in C.gles3_programs(t):
                h = hashlib.sha1(code.encode()).hexdigest()
                if h in seen:
                    continue
                n = seen[h] = len(seen)
                for stage, src in glsl.stage_sources(code).items():
                    ext = {'VERTEX': 'vert', 'FRAGMENT': 'frag'}.get(stage, stage.lower())
                    with open(os.path.join(out_dir, f'{n:04d}.{ext}'), 'w') as f:
                        f.write(as_driver_sees(src))
                rows.append(f'{n:04d}\t{name}\t{rel}\t{i}\t{" ".join(entries[i].keywords)}')
        lf.close()
    with open(os.path.join(out_dir, 'index.tsv'), 'w') as f:
        f.write('\n'.join(rows) + '\n')
    job.log(f'shaders-export: {len(rows)} distinct GLES3 programs in {out_dir}')
    return len(rows)
