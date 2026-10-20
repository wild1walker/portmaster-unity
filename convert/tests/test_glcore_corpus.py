"""Opt-in test of the OpenGL Core path on Unity's own data of any version, read from local paths
(nothing of it is in the repository):

    PMUNITY_GLCORE_CORPUS=<file>[:<file>...] [PMUNITY_GLCORE_ANDROID=<file>[:<file>...]] \
        python -m unittest tests.test_glcore_corpus -v                       (from convert/)

PMUNITY_GLCORE_CORPUS: Unity serialized files whose shaders have OpenGL Core programs, e.g. the
"unity default resources" of a Unity Linux player (the Linux support installer's
Variations/linux64_*_nondevelopment_mono/Data/Resources/). Unity ships one such file for its
desktop players, so it also holds Unity's own GLES3 programs of the same shaders, compiled from the
same source: the conversion is compared with them. PMUNITY_GLCORE_ANDROID: the same version's
Android player "unity default resources" (Unity's GLES3 programs as an Android build has them),
matched by the Unity version in the file header.

Checks, per file:
  - every GL and D3D11 platform blob parses (with the serializer's layout) and writes back to the
    same bytes, segmented blobs (2019.3+) included; the Shader writes back unchanged;
  - every shader converts (Unity's GLES3 programs removed first, then rebuilt from GLCore); only
    VR-only variants may be left out;
  - the GLES3 blob equals Unity's but for the code of the program entries: same entries, types,
    keywords, statistics, binding info, and (2021.2+) the same parameter entries;
  - every GLES3 program (VR-only variants aside) declares the same uniforms, vertex inputs,
    varyings, outputs and samplers as Unity's GLES3 program of the variant (precision aside),
    and as the Android player's;
  - the GLES3 subprograms equal Unity's (but for the VR-only variants left out); the passes'
    m_Platforms (2020.1+) and the Shader's stageCounts (2021.2+) equal Unity's;
  - PMUNITY_GLCORE_EXPORT=<dir>: every GLES3 program is written as <dir>/<n>.vert / <n>.frag with
    an index.tsv, for runtime/host/shadercheck (compile + link with a GLES driver, e.g. Mesa).
"""
import collections
import copy
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pmunity.shaders import blob as B
from pmunity.shaders import convert as C
from pmunity.shaders import glsl as G
from pmunity.shaders import export as X

CORPUS = [p for p in os.environ.get('PMUNITY_GLCORE_CORPUS', '').split(os.pathsep) if p]
ANDROID = [p for p in os.environ.get('PMUNITY_GLCORE_ANDROID', '').split(os.pathsep) if p]
EXPORT = os.environ.get('PMUNITY_GLCORE_EXPORT')

if CORPUS:
    from test_d3d11_corpus import interface


def unity_version(path):
    from pmunity import unity_version as V
    return V.of_file(path)


def is_vr(e):
    return any(k in C.VR_KEYWORDS for k in e.keywords + (e.local_keywords or []))


def load_shaders(path):
    from pmunity.unity_io import Loaded
    lf = Loaded(path)
    out = [o.read_typetree() for _, o in lf.objects('Shader')]
    lf.close()
    return out


def gles3_subprograms(tree):
    """Counter of (stage, subprogram, parameter index, blob index) of the GLES3 subprograms."""
    out = collections.Counter()
    for p, st, lst, pbi in C.subprogram_lists(tree):
        for i, sp in enumerate(lst):
            if sp['m_GpuProgramType'] == B.GPU_GLES3:
                out[(st, repr(sorted(sp.items())), None if pbi is None else pbi[i], sp['m_BlobIndex'])] += 1
    return out


@unittest.skipUnless(CORPUS, 'set PMUNITY_GLCORE_CORPUS to Unity files with OpenGL Core programs')
class GLCoreCorpusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files = [(p, str(unity_version(p)), load_shaders(p)) for p in CORPUS]
        cls.android = {str(unity_version(p)): load_shaders(p) for p in ANDROID}
        cls.stats = collections.Counter()
        cls.n_export = 0

    @classmethod
    def tearDownClass(cls):
        print('\n' + ', '.join(f'{k}: {v}' for k, v in sorted(cls.stats.items())), file=sys.stderr)

    def count(self, key, n=1):
        self.stats[key] += n

    def test_blobs_round_trip(self):
        for path, ver, trees in self.files:
            for t in trees:
                sb = B.ShaderBlobs(t)
                for plat in t['platforms']:
                    if plat not in C.PLATFORM_TYPES:
                        continue
                    es = C.platform_entries(t, plat, sb)
                    for e in es:
                        self.assertNotIsInstance(e, B.RawEntry, f'{ver} {t["m_ParsedForm"]["m_Name"]}')
                        if isinstance(e, B.Entry):
                            self.assertFalse(e.guessed, f'{ver} {t["m_ParsedForm"]["m_Name"]}: layout guessed')
                    if sb.nested:
                        self.assertEqual(B.build_segments(es, sb.segments(plat)), sb.platforms[plat])
                    else:      # Unity orders a block's entries its own way: compare the entries
                        self.assertEqual([e and e.serialize() for e in B.parse_blob(B.build_blob(es))],
                                         [e and e.serialize() for e in es])
                    self.count(f'{ver} blobs read and written back')
                t2 = copy.deepcopy(t)
                B.ShaderBlobs(t2).store()
                sb2 = B.ShaderBlobs(t2)
                self.assertEqual(sb2.platforms, sb.platforms)
                self.assertEqual(t2.get('stageCounts'), t.get('stageCounts'))

    def test_conversion_matches_unity(self):
        for path, ver, trees in self.files:
            ref_android = {t['m_ParsedForm']['m_Name']: t for t in self.android.get(ver, [])}
            for ref in trees:
                name = ref['m_ParsedForm']['m_Name']
                if B.PLATFORM_GLCORE not in ref['platforms']:
                    continue
                t = copy.deepcopy(ref)
                r = C.add_gles3(t, replace=True)
                label = f'{ver} {name}'
                if r.status == 'skipped':
                    # the shader has a non-VR variant with no ES 3.00 equivalent: Unity's own GLES3
                    # program of it does not compile either (e.g. sampler2DMS in 2022.3's
                    # FrameDebuggerRenderTargetDisplay); the conversion leaves the shader out
                    self.count(f'{ver} shaders left out ({r.why.split(": ", 1)[-1]})')
                    continue
                self.assertEqual(r.status, 'converted', label)
                self.count(f'{ver} shaders converted')
                self.count(f'{ver} VR-only variants left out', r.dropped)
                ours, unity = C.platform_entries(t, B.PLATFORM_GLES3PLUS), C.platform_entries(ref, B.PLATFORM_GLES3PLUS)
                self.assertEqual(len(ours), len(unity), label)
                android = None
                if name in ref_android:
                    android = C.platform_entries(ref_android[name], B.PLATFORM_GLES3PLUS)
                for i, (a, b) in enumerate(zip(ours, unity)):
                    if not isinstance(a, B.Entry):
                        self.assertEqual(a and a.serialize(), b and b.serialize(), f'{label} entry {i}')
                        if a is not None:
                            self.count(f'{ver} parameter entries same as Unity')
                        continue
                    if a.gpu_type == B.GPU_GLCORE32:          # a VR-only program left out
                        self.assertTrue(is_vr(a), f'{label} entry {i}')
                        continue
                    for f in ('version', 'gpu_type', 'stats', 'keywords', 'local_keywords', 'rest'):
                        self.assertEqual(getattr(a, f), getattr(b, f), f'{label} entry {i} {f}')
                    if not a.code:
                        continue
                    if is_vr(a):
                        self.count(f'{ver} VR-only programs translated')
                        continue
                    self.assertEqual(interface(a.code.decode()), interface(b.code.decode()), f'{label} entry {i}')
                    self.count(f'{ver} programs with Unity GLES3 interface')
                    if android is not None and i < len(android) and android[i] is not None and \
                            getattr(android[i], 'keywords', None) == a.keywords:
                        self.assertEqual(interface(a.code.decode()), interface(android[i].code.decode()), f'{label} entry {i} (Android)')
                        self.count(f'{ver} programs with Unity Android GLES3 interface')
                # subprograms: Unity's GLES3 ones but for the variants left out
                mine, want = gles3_subprograms(t), gles3_subprograms(ref)
                self.assertFalse(mine - want, label)
                missing = want - mine
                self.assertTrue(all(is_vr(unity[k[3]]) for k in missing), f'{label}: {list(missing)[:2]}')
                self.count(f'{ver} GLES3 subprograms same as Unity', sum(mine.values()))
                plats = lambda tr: [p.get('m_Platforms') for ss in tr['m_ParsedForm']['m_SubShaders'] for p in ss['m_Passes']]
                self.assertEqual(plats(t), plats(ref), label)
                self.assertEqual(t.get('stageCounts'), ref.get('stageCounts'), label)
                if EXPORT:
                    self._export(ver, t)

    def _export(self, ver, t):
        os.makedirs(EXPORT, exist_ok=True)
        with open(os.path.join(EXPORT, 'index.tsv'), 'a') as f:
            for i, code in C.gles3_programs(t):
                for st, src in G.stage_sources(code).items():
                    with open(os.path.join(EXPORT, f'{self.n_export:04d}.{"vert" if st == "VERTEX" else "frag"}'), 'w') as out:
                        out.write(X.as_driver_sees(src))
                f.write(f'{self.n_export:04d}\t{ver} {t["m_ParsedForm"]["m_Name"]}\t-\t{i}\t\n')
                type(self).n_export += 1


if __name__ == '__main__':
    unittest.main()
