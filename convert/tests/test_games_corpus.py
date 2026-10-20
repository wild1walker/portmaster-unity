"""Opt-in check of the shader conversion on games' data, read from local paths (nothing of it is in
the repository; run Python with -I on data you downloaded):

    PMUNITY_GAMES=<Game>_Data[:<Game>_Data...] python -m unittest tests.test_games_corpus -v   (from convert/)

Per data folder: the Unity version is read and supported; in every serialized file (and asset
bundle under StreamingAssets) every Shader with OpenGL Core programs has blobs that read and write
back byte for byte (segments included) and converts in memory, nothing written, GLCore41/43
programs included as the shaders step does by default (step.glcore4x_for;
PMUNITY_GAMES_GLCORE4X=0 turns them off). A shader, or a SubShader's GLCore41/43 variants, may be
left out only for GLSL with no ES equivalent (reported per game, with the shader names when
PMUNITY_GAMES_VERBOSE=1); every converted program is GLSL ES 3.00 (or 3.10, typed GLES 3.1), and
every GLES subprogram names a translated entry of its own type. PMUNITY_GAMES_EXPORT=<dir>: the
GLES3 programs of each game go to <dir>/<n>/ (with an index.tsv) for runtime/host/shadercheck.
"""
import collections
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity import unity_version as UV
from pmunity.job import Job
from pmunity.shaders import blob as B
from pmunity.shaders import convert as C
from pmunity.shaders import glsl as G
from pmunity.shaders import export as X
from pmunity.shaders.step import shader_files, glcore4x_for

GAMES = [p for p in os.environ.get('PMUNITY_GAMES', '').split(os.pathsep) if p]
_QUALS = r'(?:(?:flat|smooth|centroid|invariant|readonly|writeonly|coherent|restrict|volatile)\s+)*'
_DECL = re.compile(r'^\s*(%s)(uniform|in|out|buffer)\s+(\w+)(?:\s+(\w+))?\s*(\[\d*\])?\s*[;{]' % _QUALS, re.M)


def _reason(why):
    """What a program was left out for, without the program and stage."""
    return re.sub(r'^.*?\b(VERTEX|FRAGMENT): ', '', why)


def interface(code):
    """{stage: sorted [(qualifiers, storage, type or block, name, array)]} of a GL program: its
    uniforms, blocks, inputs, varyings and outputs, whatever their layout, location and precision."""
    out = {}
    for st, src in G.stage_sources(code).items():
        src = G._code_lines(G._strip_comments(src))
        src = re.sub(r'\blayout\s*\([^)]*\)|\bUNITY_(?:LOCATION|BINDING)\(\d+\)|\b(?:highp|mediump|lowp)\b', ' ', src)
        out[st] = sorted((' '.join(m.group(1).split()), m.group(2), m.group(3), m.group(4) or '', m.group(5) or '')
                         for m in _DECL.finditer(src))
    return out
EXPORT = os.environ.get('PMUNITY_GAMES_EXPORT')
FORCE4X = {'1': True, '0': False}.get(os.environ.get('PMUNITY_GAMES_GLCORE4X', ''))
VERBOSE = os.environ.get('PMUNITY_GAMES_VERBOSE') == '1'


@unittest.skipUnless(GAMES, 'set PMUNITY_GAMES to game data folders')
class GamesCorpusTest(unittest.TestCase):
    def test_games(self):
        from pmunity.unity_io import Loaded
        summary = []
        for gi, data in enumerate(GAMES):
            v = UV.check(UV.of_data(data))
            job = Job.reader(data, {'version': 1}, log=lambda m: None)
            c = collections.Counter()
            seen = set()
            rows = []
            for rel in shader_files(job):
                lf = Loaded(job.path_in(rel))
                gl4 = glcore4x_for(job.path_in(rel), FORCE4X)
                for _, o in lf.objects('Shader'):
                    t = o.read_typetree()
                    if B.PLATFORM_GLCORE not in t['platforms']:
                        continue
                    sb = B.ShaderBlobs(t)
                    es = C.platform_entries(t, B.PLATFORM_GLCORE, sb)
                    back = B.build_segments(es, sb.segments(B.PLATFORM_GLCORE)) if sb.nested else None
                    if back is not None:
                        self.assertEqual(back, sb.platforms[B.PLATFORM_GLCORE], f'{data} {rel}')
                    self.assertFalse(any(isinstance(e, B.Entry) and e.guessed for e in es), f'{data}: layout guessed')
                    r = C.add_gles3(t, glcore4x=gl4)
                    name = t['m_ParsedForm']['m_Name']
                    c[r.status] += 1
                    if r.status == 'converted':
                        c['programs'] += r.programs
                        c['of them GLCore41/43'] += r.programs4x
                        c['GLES 3.1 programs'] += r.es31
                    c['VR-only variants left out'] += r.dropped
                    c['empty GLES3 blobs (stripped shaders)'] += r.empty
                    if r.status == 'skipped':
                        c['left out: ' + _reason(r.why)] += 1
                        if VERBOSE:
                            print(f'{v} {rel}: {name}: left out: {r.why}', file=sys.stderr)
                        continue
                    for si, why in r.failed:
                        c['SubShaders left out: ' + _reason(why)] += 1
                        if VERBOSE:
                            print(f'{v} {rel}: {name}: SubShader {si} left out: {why}', file=sys.stderr)
                    if r.status != 'converted':
                        continue
                    ours = C.platform_entries(t, B.PLATFORM_GLES3PLUS)
                    for i, code in C.gles3_programs(t):      # the same interface as the GL program
                        self.assertEqual(interface(code), interface(es[i].code.decode()), f'{v} {name} entry {i}')
                        c['programs with the GL interface'] += 1
                    for _, _, lst in C.subprograms(t):
                        for sp in lst:
                            if sp['m_GpuProgramType'] in (B.GPU_GLES3, B.GPU_GLES31):
                                e = ours[sp['m_BlobIndex']]
                                self.assertEqual(e.gpu_type, sp['m_GpuProgramType'], f'{v} {name}')
                    for i, code in C.gles3_programs(t):
                        self.assertRegex(code, r'^#ifdef VERTEX\n#version 3[01]0 es\n')
                        if EXPORT and code not in seen:
                            d = os.path.join(EXPORT, str(gi))
                            os.makedirs(d, exist_ok=True)
                            for st, src in G.stage_sources(code).items():
                                with open(os.path.join(d, f'{len(seen):04d}.{"vert" if st == "VERTEX" else "frag"}'), 'w') as f:
                                    f.write(X.as_driver_sees(src))
                            rows.append(f'{len(seen):04d}\t{name}\t{rel}\t{i}\t{" ".join(ours[i].keywords)}')
                            seen.add(code)
                lf.close()
            if EXPORT and rows:
                with open(os.path.join(EXPORT, str(gi), 'index.tsv'), 'w') as f:
                    f.write('\n'.join(rows) + '\n')
            self.assertGreater(c['converted'], 0, data)
            summary.append(f'{v}: ' + ', '.join(f'{k} {n}' for k, n in sorted(c.items())))
        print('\n' + '\n'.join(summary), file=sys.stderr)


if __name__ == '__main__':
    unittest.main()
