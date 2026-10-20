"""Opt-in test of the Direct3D 11 path on real Unity data, read from local paths (nothing of it is
in the repository):

    PMUNITY_D3D11_CORPUS=<file>[:<file>...] python -m unittest tests.test_d3d11_corpus -v   (from convert/)

Each file is a Unity serialized file whose shaders have Direct3D 11 programs, e.g. the "unity
default resources" of a Unity Windows player (Unity 2017.4.9f1: the Windows support installer's
Variations/win64_nondevelopment_mono/Data/Resources/). That file also has Unity's own OpenGL Core
and GLES3 programs of the same shaders, compiled from the same HLSL, which the results are
compared with. Needs pmhlslcc (tools/hlslcc/build.sh, or $PMHLSLCC).

Checks:
  - every D3D11 entry's binding info parses and writes back byte for byte (dxbc.Bindings);
  - every shader converts; every variant that is not VR-only translates;
  - for each such variant with a GLCore program in the file: the translated GL program (before
    the ES rules) declares the same uniforms, vertex inputs, varyings, outputs and samplers as
    Unity's GLCore program, and its code is the same up to the names of temporaries;
  - with Unity's GLES3 programs in the file: every GLES3 subprogram twin equals Unity's own GLES3
    subprogram of that variant but for the blob index, and the GLES3 programs declare the same
    interface (precision qualifiers aside);
  - PMUNITY_D3D11_EXPORT=<dir>: every GLES3 program is written as <n>.vert / <n>.frag with an
    index.tsv, for runtime/host/shadercheck (compile + link with a GLES driver, e.g. Mesa).
"""
import copy
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity.shaders import blob as B
from pmunity.shaders import convert as C
from pmunity.shaders import dxbc as D
from pmunity.shaders import glsl as G

CORPUS = [p for p in os.environ.get('PMUNITY_D3D11_CORPUS', '').split(os.pathsep) if p]
EXPORT = os.environ.get('PMUNITY_D3D11_EXPORT')

_DECL = re.compile(r'^(?:layout\(location = (\d+)\) )?(uniform|in|out)\s+(?:(?:highp|mediump|lowp)\s+)?(\w+)\s+(\w+)(\[\d+\])?;', re.M)


def interface(code):
    """{stage: sorted [(storage, type, name, array, location)]} of a GL program (precision aside)."""
    return {st: sorted((m.group(2), m.group(3), m.group(4), m.group(5) or '', m.group(1) or '')
                       for m in _DECL.finditer(src))
            for st, src in G.stage_sources(code).items()}


def body(src):
    """A stage's code after its declarations, every temporary called T (Unity's GL compiles see
    `fixed`/`half` and give such values their own temporaries, u_xlat10_0, u_xlat16_0, where the
    D3D11 bytecode reuses one: the operations and operands are what is compared)."""
    lines = [l for l in src.splitlines() if l.strip() and not _DECL.match(l) and not l.startswith('#')
             and not re.match(r'\s*(?:(?:highp|mediump|lowp)\s+)?\w+\s+u_xlat\w+(\[\d+\])?;', l)
             and not l.startswith('precision ')]
    return [re.sub(r'\bu_xlat\w+', 'T', l) for l in lines]


def variants(tree):
    """(pass, vertex subprogram, pixel subprogram, GLCore vertex subprogram or None,
    GLES3 vertex and fragment subprograms or None) per D3D11 variant."""
    for ss in tree['m_ParsedForm']['m_SubShaders']:
        for p in ss['m_Passes']:
            vl, fl = p['progVertex']['m_SubPrograms'], p['progFragment']['m_SubPrograms']
            by = lambda lst, types: {C._variant_key(sp): sp for sp in lst if sp['m_GpuProgramType'] in types}
            vs, ps, glv = by(vl, D.VERTEX_TYPES), by(fl, D.PIXEL_TYPES), by(vl, (B.GPU_GLCORE32,))
            esv, esf = by(vl, (B.GPU_GLES3,)), by(fl, (B.GPU_GLES3,))
            for k, sp in vs.items():
                yield p, sp, ps.get(k), glv.get(k), esv.get(k), esf.get(k)


@unittest.skipUnless(CORPUS, 'set PMUNITY_D3D11_CORPUS to Unity files with Direct3D 11 programs')
class D3D11CorpusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from pmunity.unity_io import Loaded
        cls.trees = []
        for path in CORPUS:
            lf = Loaded(path)
            cls.trees += [(path, o.read_typetree()) for _, o in lf.objects('Shader')]
            lf.close()
        cls.trees = [(p, t) for p, t in cls.trees if B.PLATFORM_D3D11 in t['platforms']]
        cls.translator = D.Translator()
        cls.stats = {}

    @classmethod
    def tearDownClass(cls):
        cls.translator.close()
        print('\n' + ', '.join(f'{k}: {v}' for k, v in sorted(cls.stats.items())), file=sys.stderr)

    def count(self, key, n=1):
        self.stats[key] = self.stats.get(key, 0) + n

    def test_bindings_round_trip(self):
        for path, t in self.trees:
            for e in B.ShaderBlobs(t).entries(B.PLATFORM_D3D11):
                if e is not None and e.code:
                    self.assertEqual(D.Bindings.parse(e.rest).serialize(), e.rest)
                    self.count('D3D11 entries')

    def test_translation_matches_unity(self):
        for path, t in self.trees:
            name = t['m_ParsedForm']['m_Name']
            sb = B.ShaderBlobs(t)
            d3d = sb.entries(B.PLATFORM_D3D11)
            glc = sb.entries(B.PLATFORM_GLCORE) if B.PLATFORM_GLCORE in t['platforms'] else None
            gles = sb.entries(B.PLATFORM_GLES3PLUS) if B.PLATFORM_GLES3PLUS in t['platforms'] else None
            for p, vsp, psp, glv, esv, esf in variants(t):
                vs = d3d[vsp['m_BlobIndex']]
                vr = any(k in C.VR_KEYWORDS for k in vs.keywords)
                label = f'{name} {" ".join(vs.keywords) or "-"}'
                self.assertIsNotNone(psp, label)
                ps = d3d[psp['m_BlobIndex']]
                try:
                    ours = D.translate_program(self.translator, (vs.code, vs.rest), (ps.code, ps.rest))
                    es = G.program_to_es(ours)
                except (D.Unsupported, G.Unsupported) as ex:
                    self.assertTrue(vr, f'{label}: {ex}')
                    self.count('VR-only variants not translatable')
                    continue
                self.count('variants translated')
                if vr:
                    continue                        # D3D11 and GL compile VR variants differently
                if glc is not None and glv is not None:
                    unity = glc[glv['m_BlobIndex']].code.decode()
                    self.assertEqual(interface(ours), interface(unity), label)
                    for st, src in G.stage_sources(ours).items():
                        self.assertEqual(body(src), body(G.stage_sources(unity)[st]), f'{label} {st}')
                    self.count('same as Unity GLCore (up to temporaries)' if ours != unity else 'identical to Unity GLCore')
                if gles is not None and esv is not None:
                    unity = gles[esv['m_BlobIndex']].code.decode()
                    self.assertEqual(interface(es), interface(unity), label)
                    self.count('same interface as Unity GLES3')

    def test_converted_subprograms_match_unity(self):
        for path, t in self.trees:
            t = copy.deepcopy(t)
            ref = {}
            if B.PLATFORM_GLES3PLUS in t['platforms']:
                for p, st, lst in C.subprograms(t):
                    for sp in lst:
                        if sp['m_GpuProgramType'] == B.GPU_GLES3:
                            ref[(id(lst), C._variant_key(sp))] = sp
            r = C.add_gles3_d3d11(t, self.translator, replace=True)
            self.assertEqual(r.status, 'converted', r)
            self.assertFalse(r.failed, r)
            self.count('shaders converted')
            self.count('GLES3 programs', r.programs)
            self.count('VR-only variants left out', r.dropped)
            progs = dict(C.gles3_programs(t))
            for p, st, lst in C.subprograms(t):
                for sp in lst:
                    if sp['m_GpuProgramType'] != B.GPU_GLES3:
                        continue
                    want = ref.get((id(lst), C._variant_key(sp)))
                    if want is not None:
                        a, b = dict(sp), dict(want)
                        a.pop('m_BlobIndex'), b.pop('m_BlobIndex')
                        self.assertEqual(a, b, t['m_ParsedForm']['m_Name'])
                        self.count('subprograms same as Unity GLES3')
                    if st == 'progVertex':
                        self.assertIn(sp['m_BlobIndex'], progs)
            if EXPORT:
                self._export(t, progs)

    def _export(self, t, progs):
        os.makedirs(EXPORT, exist_ok=True)
        idx = os.path.join(EXPORT, 'index.tsv')
        n = 0
        if os.path.exists(idx):
            with open(idx) as f:
                n = sum(1 for _ in f)
        with open(idx, 'a') as f:
            for i, code in sorted(progs.items()):
                for st, src in G.stage_sources(code).items():
                    with open(os.path.join(EXPORT, f'{n:04d}.{"vert" if st == "VERTEX" else "frag"}'), 'w') as out:
                        out.write(src)
                f.write(f'{n:04d}\t{t["m_ParsedForm"]["m_Name"]}\t-\t{i}\t\n')
                n += 1


if __name__ == '__main__':
    unittest.main()
