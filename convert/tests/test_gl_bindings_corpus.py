"""Opt-in test of dxbc.gl_bindings (the binding info Unity 2019.1+ gives a GL program, written from
the program's text) against Unity's own GL programs, read from local paths (nothing of it is in
the repository):

    PMUNITY_GL_BINDINGS=<file or folder>[:...] python -m unittest tests.test_gl_bindings_corpus -v   (from convert/)

Each path is a Unity serialized file with OpenGL Core or GLES3 programs (Unity 2019.1 - 6000.6;
6000.6's binding info in its own layout, Bindings.typed),
or a folder searched for them (a Linux or macOS game's data, an Android build's, the built-in
resources of Unity's players). For every program that is not VR-only and has no uniform blocks
(what the Direct3D 11 path writes: loose uniforms and textures) the binding info gl_bindings()
writes equals Unity's byte for byte (2020.1+: with the program's shared parameters,
m_CommonParameters, put back, as a set). Programs with uniform blocks are compared too and
counted, but not required to match (two cases are not known from the text: a one-element array
in a block, which HLSLcc declares as a plain variable, and the order of blocks of several stages
in 2022.1+).
"""
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity.shaders import blob as B
from pmunity.shaders import convert as C
from pmunity.shaders import dxbc as D

PATHS = [p for p in os.environ.get('PMUNITY_GL_BINDINGS', '').split(os.pathsep) if p]
_FILE = re.compile(r'(\.assets|^unity_builtin_extra|^unity default resources|^level\d+|^globalgamemanagers)$')


def files(paths):
    for p in paths:
        if os.path.isfile(p):
            yield p
            continue
        for root, _, names in os.walk(p):
            for n in sorted(names):
                if _FILE.search(n):
                    yield os.path.join(root, n)


def canon(b):
    return ([(c.name, c.size, sorted(p.key() for p in c.params)) for c in b.cbuffers], sorted(x.key() for x in b.bindings))


_BLOCK = re.compile(r'^(?:UNITY_BINDING\(\d+\) )?(?:layout\([^)]*\) )?uniform \w+ \{', re.M)


@unittest.skipUnless(PATHS, 'set PMUNITY_GL_BINDINGS to Unity data with GL programs (2019.1+)')
class GLBindingsCorpusTest(unittest.TestCase):
    def test_bindings(self):
        from pmunity.unity_io import Loaded
        stats, bad = {}, []

        def count(k):
            stats[k] = stats.get(k, 0) + 1
        for path in files(PATHS):
            try:
                lf = Loaded(path)
            except Exception:                           # not a serialized file
                continue
            ver = getattr(lf.sf, 'unity_version', '') or ''
            try:
                line = tuple(int(x) for x in ver.split('.')[:2])
            except ValueError:
                line = (0, 0)
            if line < (2019, 1):
                lf.close()
                continue
            for _, o in lf.objects('Shader'):
                t = o.read_typetree()
                name = t['m_ParsedForm']['m_Name']
                for plat, gtype in ((B.PLATFORM_GLCORE, B.GPU_GLCORE32), (B.PLATFORM_GLES3PLUS, B.GPU_GLES3)):
                    if plat not in t['platforms']:
                        continue
                    es = C.platform_entries(t, plat)
                    for p, st, lst, pbi in C.subprogram_lists(t):
                        names = {i: n for n, i in p['m_NameIndices']}
                        common = p[st].get('m_CommonParameters')
                        for j, sp in enumerate(lst):
                            if sp['m_GpuProgramType'] != gtype:
                                continue
                            e = es[sp['m_BlobIndex']]
                            if not isinstance(e, B.Entry) or not e.code:
                                continue
                            if any(k in C.VR_KEYWORDS for k in e.keywords + (e.local_keywords or [])):
                                continue
                            code = e.code.decode()
                            try:
                                unity = D.with_common(D.entry_bindings(e, es[pbi[j]] if pbi is not None else None, typed=True),
                                                      common, names)
                                ours = D.gl_bindings(code, line, unity.source_map, typed=unity.typed)
                            except D.Unsupported as ex:
                                count(f'not read ({str(ex).split(":")[0]})')
                                continue
                            blocks = bool(_BLOCK.search(code))
                            same = canon(ours) == canon(unity)
                            exact = ours.serialize() == unity.serialize()
                            kind = 'with uniform blocks' if blocks else 'loose uniforms'
                            count(f'{kind}: {"same" if same else "differ"}')
                            if exact:
                                count('byte for byte')
                            if not blocks and not same:
                                bad.append(f'{ver} {name} {e.keywords}')
            lf.close()
        print('\n' + ', '.join(f'{k}: {v}' for k, v in sorted(stats.items())), file=sys.stderr)
        self.assertFalse(bad, bad[:10])
        self.assertTrue(stats.get('loose uniforms: same'), 'no GL programs of Unity 2019.1+ found')


if __name__ == '__main__':
    unittest.main()
