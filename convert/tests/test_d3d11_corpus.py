"""Opt-in test of the Direct3D 11 path on real Unity data, read from local paths (nothing of it is
in the repository):

    PMUNITY_D3D11_CORPUS=<file>[:<file>...] python -m unittest tests.test_d3d11_corpus -v   (from convert/)

Each file is a Unity serialized file whose shaders have Direct3D 11 programs, e.g. the "unity
default resources" of a Unity Windows player (the Windows support installer's
Variations/win64_*nondevelopment_*/Data/Resources/; Unity 2017.4 - 2022.3). That file also has
Unity's own OpenGL Core and GLES3 programs of the same shaders, compiled from the same HLSL, which
the results are compared with. Needs pmhlslcc (tools/hlslcc/build.sh, or $PMHLSLCC).

Checks:
  - every D3D11 entry's binding info (2021.2+ with its parameter entry) parses and writes back
    byte for byte (dxbc.Bindings);
  - every shader converts; every variant that is not VR-only translates;
  - for each such variant with a GLCore program in the file: the translated GL program (before
    the ES rules) declares the same uniforms, vertex inputs, varyings, outputs and samplers as
    Unity's GLCore program, its code is the same up to the names of temporaries, and (2019.1+)
    the GL binding info written for it (dxbc.gl_bindings) is Unity's (with the program's shared
    parameters put back; the same set of constant buffers, members and bindings);
  - with Unity's GLES3 programs in the file: every GLES3 subprogram twin equals Unity's own GLES3
    subprogram of that variant but for the blob index, the GLES3 programs declare the same
    interface (precision qualifiers aside) and (2019.1+) carry the same binding info;
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
# editor-only shaders with variants GLSL ES has nothing for (2022.3: a sampler2DMSArray variant)
EDITOR_ONLY = ('Hidden/FrameDebuggerRenderTargetDisplay',)

_DECL = re.compile(r'^(?:(?:layout\(location = (\d+)\)|UNITY_LOCATION\(\d+\)) )?(uniform|in|out)\s+(?:(?:highp|mediump|lowp)\s+)?(\w+)\s+(\w+)(\[\d+\])?;', re.M)


def flat(src):
    """A stage with its uniform blocks written as loose uniforms (Unity's GL compile keeps some
    constant buffers as blocks, e.g. those of shaders that declare them for every API; the D3D11
    path writes every member as a uniform; Xhlslcc_UnusedX padding left out)."""
    out, inside = [], False
    for l in src.splitlines():
        if re.match(r'^(?:UNITY_BINDING\(\d+\) )?(?:layout\(std140\) )?uniform \w+ \{$', l):
            inside = True
            if out and out[-1] == '#if HLSLCC_ENABLE_UNIFORM_BUFFERS':
                out.pop()
            continue
        if inside:
            if re.match(r'^(#if HLSLCC_ENABLE_UNIFORM_BUFFERS|#endif)$', l):
                continue
            if l.startswith('};'):
                inside = False
                continue
            l = re.sub(r'^\s*(?:UNITY_UNIFORM\s+)?', 'uniform \t', l)
            if 'Xhlslcc_UnusedX' in l:
                continue
        out.append(l)
    return '\n'.join(out)


def interface(code):
    """{stage: sorted [(storage, type, name, array, location)]} of a GL program (precision aside)."""
    return {st: sorted((m.group(2), m.group(3), m.group(4), m.group(5) or '', m.group(1) or '')
                       for m in _DECL.finditer(flat(src)))
            for st, src in G.stage_sources(code).items()}


def body(src):
    """A stage's code after its declarations, every temporary called T (Unity's GL compiles see
    `fixed`/`half` and give such values their own temporaries, u_xlat10_0, u_xlat16_0, where the
    D3D11 bytecode reuses one: the operations and operands are what is compared)."""
    lines = [l for l in flat(src).splitlines() if l.strip() and not _DECL.match(l) and not l.startswith('#')
             and not re.match(r'\s*(?:(?:highp|mediump|lowp)\s+)?\w+\s+u_xlat\w+(\[\d+\])?;', l)
             and not l.startswith('precision ')]
    return [re.sub(r'\bu_xlat\w+', 'T', l) for l in lines]


def canon(b):
    """A binding info compared as Unity's merge of shared parameters leaves it: constant buffers
    in order with their members as a set, bindings as a set."""
    return ([(c.name, c.size, sorted(p.key() for p in c.params)) for c in b.cbuffers],
            sorted(x.key() for x in b.bindings))


def line_of(path):
    from pmunity.unity_io import Loaded
    lf = Loaded(path)
    v = getattr(lf.sf, 'unity_version', None)
    lf.close()
    return v


def variants(tree, entries):
    """(pass, tier, vertex sp, its parameter index, pixel sp, its parameter index, keywords,
    {type: sp of those keywords}) of every D3D11 variant (the vertex and pixel subprograms Unity
    runs together, convert._variant_pairs)."""
    for ss in tree['m_ParsedForm']['m_SubShaders']:
        for p in ss['m_Passes']:
            vl, fl = C._tier_lists(p.get('progVertex')), C._tier_lists(p.get('progFragment'))
            for t, (vlist, vpbi) in enumerate(vl):
                flist, fpbi = fl[t] if t < len(fl) else ([], None)
                vss = [(sp, vpbi[j] if vpbi is not None else None) for j, sp in enumerate(vlist) if sp['m_GpuProgramType'] in D.VERTEX_TYPES]
                pss = [(sp, fpbi[j] if fpbi is not None else None) for j, sp in enumerate(flist) if sp['m_GpuProgramType'] in D.PIXEL_TYPES]
                others = {}
                for j, sp in enumerate(vlist):
                    if sp['m_GpuProgramType'] in (B.GPU_GLCORE32, B.GPU_GLES3):
                        others[(sp['m_GpuProgramType'], C._keyset(sp), sp.get('m_ShaderHardwareTier'))] = \
                            (sp, vpbi[j] if vpbi is not None else None)
                for vsp, vpi, psp, ppi, keys in C._variant_pairs(vss, pss):
                    tier = vsp.get('m_ShaderHardwareTier')
                    yield p, t, vsp, vpi, psp, ppi, keys, {ty: others.get((ty, keys, tier)) for ty in (B.GPU_GLCORE32, B.GPU_GLES3)}


@unittest.skipUnless(CORPUS, 'set PMUNITY_D3D11_CORPUS to Unity files with Direct3D 11 programs')
class D3D11CorpusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from pmunity.unity_io import Loaded
        cls.trees = []
        for path in CORPUS:
            lf = Loaded(path)
            ver = getattr(lf.sf, 'unity_version', None)
            cls.trees += [(path, ver, o.read_typetree()) for _, o in lf.objects('Shader')]
            lf.close()
        cls.trees = [(p, v, t) for p, v, t in cls.trees if B.PLATFORM_D3D11 in t['platforms']]
        cls.translator = D.Translator()
        cls.stats = {}

    @classmethod
    def tearDownClass(cls):
        cls.translator.close()
        print('\n' + ', '.join(f'{k}: {v}' for k, v in sorted(cls.stats.items())), file=sys.stderr)

    def count(self, key, n=1):
        self.stats[key] = self.stats.get(key, 0) + n

    def entries(self, t, plat):
        if plat not in t['platforms']:
            return None
        return C.platform_entries(t, plat)

    def test_bindings_round_trip(self):
        for path, ver, t in self.trees:
            es = self.entries(t, B.PLATFORM_D3D11)
            for p, st, lst, pbi in C.subprogram_lists(t):
                for j, sp in enumerate(lst):
                    if sp['m_GpuProgramType'] not in range(15, 23):
                        continue
                    e = es[sp['m_BlobIndex']]
                    pe = es[pbi[j]] if pbi is not None else None
                    b = D.entry_bindings(e, pe)
                    self.assertEqual(b.serialize(), e.rest + (pe.data if pe is not None else b''))
                    self.count('D3D11 subprograms')

    def test_translation_matches_unity(self):
        for path, ver, t in self.trees:
            name = t['m_ParsedForm']['m_Name']
            line = C.unity_of(t, ver)
            d3d, glc, gles = (self.entries(t, pl) for pl in (B.PLATFORM_D3D11, B.PLATFORM_GLCORE, B.PLATFORM_GLES3PLUS))
            for p, tier, vsp, vpi, psp, ppi, keys, gl in variants(t, d3d):
                vs = d3d[vsp['m_BlobIndex']]
                kws = vs.keywords + (vs.local_keywords or [])
                vr = any(k in C.VR_KEYWORDS for k in kws)
                label = f'{name} {" ".join(kws) or "-"}'
                self.assertIsNotNone(psp, label)
                ps = d3d[psp['m_BlobIndex']]
                names = {i: n for n, i in p['m_NameIndices']}
                vb = D.with_common(D.entry_bindings(vs, d3d[vpi] if vpi is not None else None),
                                   p['progVertex'].get('m_CommonParameters'), names)
                pb = D.with_common(D.entry_bindings(ps, d3d[ppi] if ppi is not None else None),
                                   p['progFragment'].get('m_CommonParameters'), names)
                try:
                    ours = D.translate_program(self.translator, (vs.code, vb), (ps.code, pb), vs.version, line)
                    es = G.program_to_es(ours)
                except G.NeedsES31:
                    es = G.program_to_es(ours, es='310')
                except (D.Unsupported, G.Unsupported) as ex:
                    self.assertTrue(vr or name in EDITOR_ONLY, f'{label}: {ex}')
                    self.count('VR-only variants not translatable' if vr else 'editor-only variants not translatable')
                    continue
                self.count('variants translated')
                if vr:
                    continue                        # D3D11 and GL compile VR variants differently
                arrays = {q.name: q.array for b in (vb, pb) for cb in b.cbuffers for q in cb.params}
                info = D.gl_bindings(ours, line, vb.source_map, arrays) if line >= (2019, 1) else None
                for ty, ref, entries_ in ((B.GPU_GLCORE32, 'GLCore', glc), (B.GPU_GLES3, 'GLES3', gles)):
                    if entries_ is None or gl[ty] is None:
                        continue
                    usp, upi = gl[ty]
                    ue = entries_[usp['m_BlobIndex']]
                    unity = ue.code.decode()
                    mine, theirs = interface(es if ty == B.GPU_GLES3 else ours), interface(unity)
                    if re.search(r'^(?:UNITY_BINDING\(\d+\) )?uniform \w+ \{', unity, re.M):
                        # Unity's has uniform blocks, which declare unused members too: ours declares a part of them
                        for st in mine:
                            u = lambda d: {x for x in d[st] if x[0] == 'uniform'}
                            self.assertLessEqual(u(mine), u(theirs), f'{label} {st} ({ref})')
                            mine[st] = [x for x in mine[st] if x[0] != 'uniform']
                            theirs[st] = [x for x in theirs[st] if x[0] != 'uniform']
                    self.assertEqual(mine, theirs, f'{label} ({ref})')
                    if ty == B.GPU_GLCORE32:
                        for st, src in G.stage_sources(ours).items():
                            self.assertEqual(body(src), body(G.stage_sources(unity)[st]), f'{label} {st}')
                        self.count('same as Unity GLCore (up to temporaries)' if ours != unity else 'identical to Unity GLCore')
                    else:
                        self.count('same interface as Unity GLES3')
                    if info is not None and not re.search(r'^(?:UNITY_BINDING\(\d+\) )?uniform \w+ \{', unity, re.M):
                        ub = D.with_common(D.entry_bindings(ue, entries_[upi] if upi is not None else None),
                                           p['progVertex'].get('m_CommonParameters'), names)
                        self.assertEqual(canon(info), canon(ub), f'{label} ({ref} binding info)')
                        self.count(f'binding info same as Unity {ref}')

    def test_converted_subprograms_match_unity(self):
        for path, ver, t in self.trees:
            t = copy.deepcopy(t)
            ref = {}
            if B.PLATFORM_GLES3PLUS in t['platforms']:
                ues = self.entries(t, B.PLATFORM_GLES3PLUS)
                for p, st, lst, pbi in C.subprogram_lists(t):
                    for j, sp in enumerate(lst):
                        if sp['m_GpuProgramType'] == B.GPU_GLES3:
                            e = ues[sp['m_BlobIndex']]
                            b = D.entry_bindings(e, ues[pbi[j]] if pbi is not None else None) if e.code else None
                            ref[(id(lst), C._variant_key(sp))] = (sp, b)
            r = C.add_gles3_d3d11(t, self.translator, replace=True, unity=ver)
            if t['m_ParsedForm']['m_Name'] in EDITOR_ONLY:
                continue
            self.assertEqual(r.status, 'converted', r)
            self.assertFalse(r.failed, r)
            self.count('shaders converted')
            self.count('GLES3 programs', r.programs)
            self.count('VR-only variants left out', r.dropped)
            progs = dict(C.gles3_programs(t))
            es = self.entries(t, B.PLATFORM_GLES3PLUS)
            for p, st, lst, pbi in C.subprogram_lists(t):
                for j, sp in enumerate(lst):
                    if sp['m_GpuProgramType'] not in (B.GPU_GLES3, B.GPU_GLES31):
                        continue
                    if st == 'progVertex':
                        self.assertIn(sp['m_BlobIndex'], progs)
                    want = ref.get((id(lst), C._variant_key(sp)))
                    if want is None:
                        continue
                    a, b = dict(sp), dict(want[0])
                    a.pop('m_BlobIndex'), b.pop('m_BlobIndex')
                    e = es[sp['m_BlobIndex']]
                    if any(k in C.VR_KEYWORDS for k in e.keywords + (e.local_keywords or [])):
                        continue
                    if want[1] is not None and any(c.name not in ('', '$Globals') for c in want[1].cbuffers):
                        continue                    # Unity's GL program uses uniform blocks (ours: loose uniforms)
                    self.assertEqual(a, b, t['m_ParsedForm']['m_Name'])
                    self.count('subprograms same as Unity GLES3')
                    if want[1] is not None and e.code:
                        ours = D.entry_bindings(e, es[pbi[j]] if pbi is not None else None)
                        self.assertEqual(ours.serialize(), want[1].serialize(), t['m_ParsedForm']['m_Name'])
                        self.count('GLES3 binding info same as Unity')
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
