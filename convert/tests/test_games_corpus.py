"""Opt-in check of the shader conversion on games' data, read from local paths (nothing of it is in
the repository; run Python with -I on data you downloaded):

    PMUNITY_GAMES=<Game>_Data[:<Game>_Data...] python -m unittest tests.test_games_corpus -v   (from convert/)

Per data folder: the Unity version is read and supported; in every serialized file (and asset
bundle under StreamingAssets) every Shader with OpenGL Core programs has blobs that read and write
back byte for byte (segments included) and converts in memory, nothing written. A shader may be
left out only for GLSL with no ES equivalent (reported per game); every converted program is
GLSL ES 3.00 (or 3.10, typed GLES 3.1). PMUNITY_GAMES_EXPORT=<dir>: the GLES3 programs of each game
go to <dir>/<n>/ for runtime/host/shadercheck.
"""
import collections
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity import unity_version as UV
from pmunity.job import Job
from pmunity.shaders import blob as B
from pmunity.shaders import convert as C
from pmunity.shaders import glsl as G
from pmunity.shaders import export as X
from pmunity.shaders.step import shader_files

GAMES = [p for p in os.environ.get('PMUNITY_GAMES', '').split(os.pathsep) if p]
EXPORT = os.environ.get('PMUNITY_GAMES_EXPORT')


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
            for rel in shader_files(job):
                lf = Loaded(job.path_in(rel))
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
                    r = C.add_gles3(t)
                    c[r.status] += 1
                    c['programs'] += r.programs
                    c['GLES 3.1 programs'] += r.es31
                    c['VR-only variants left out'] += r.dropped
                    if r.status == 'skipped':
                        c['left out: ' + r.why.split(': ', 1)[-1].split(' ', 1)[-1]] += 1
                        continue
                    for i, code in C.gles3_programs(t):
                        self.assertRegex(code, r'^#ifdef VERTEX\n#version 3[01]0 es\n')
                        if EXPORT and code not in seen:
                            d = os.path.join(EXPORT, str(gi))
                            os.makedirs(d, exist_ok=True)
                            for st, src in G.stage_sources(code).items():
                                with open(os.path.join(d, f'{len(seen):04d}.{"vert" if st == "VERTEX" else "frag"}'), 'w') as f:
                                    f.write(X.as_driver_sees(src))
                            seen.add(code)
                lf.close()
            self.assertGreater(c['converted'], 0, data)
            summary.append(f'{v}: ' + ', '.join(f'{k} {n}' for k, n in sorted(c.items())))
        print('\n' + '\n'.join(summary), file=sys.stderr)


if __name__ == '__main__':
    unittest.main()
