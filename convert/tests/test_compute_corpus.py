"""Opt-in test of the compute kernel translation on Unity's own data and on games, read from local
paths (nothing of it is in the repository):

    PMUNITY_COMPUTE_CORPUS=<file>[:<file>...] [PMUNITY_COMPUTE_EXPORT=<dir>] \
        python -m unittest tests.test_compute_corpus -v                      (from convert/)

PMUNITY_COMPUTE_CORPUS: Unity serialized files with ComputeShader assets: the "unity default
resources" of a Linux or macOS player (they hold Unity's GLCore and GLES3 kernels of the same
compute shaders), or a game's .assets files (GLCore and Vulkan kernels only).

Checks, per ComputeShader with GLCore kernels:
  - it reads and writes back unchanged (UnityPy's typetree of the data's version);
  - with Unity's GLES3 variant (if any) taken out, compute.add_gles3 makes one again: at the same
    place in the variant list, with the same level, kernels and metadata, and every kernel
    declares the same uniform blocks, buffers, images, samplers and shared memory as Unity's
    (from 2019.1: earlier GLES3 kernels differ, see docs/shaders.md);
  - the file with the new variants writes and reads back;
  - PMUNITY_COMPUTE_EXPORT=<dir>: every translated kernel as <dir>/<n>.comp with an index.tsv,
    for runtime/host/shadercheck (compile + link with a GLES 3.1+ driver, e.g. Mesa).
The run prints, per file, the kernels translated and the ones left out with the reason.
"""
import copy
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity.shaders import compute as CS

CORPUS = [p for p in os.environ.get('PMUNITY_COMPUTE_CORPUS', '').split(os.pathsep) if p]
EXPORT = os.environ.get('PMUNITY_COMPUTE_EXPORT')


def declarations(code):
    """A kernel's global declarations (before main), temporaries and preprocessor lines aside."""
    head = code.split('void main()')[0]
    return [ln.strip() for ln in head.splitlines()
            if ln.strip() and not re.match(r'\s*(precision\b|#)', ln) and
            not re.match(r'(highp |mediump |lowp )?(vec|ivec|uvec|bvec|float|int|uint|bool)[234]? u_xlat', ln.strip())]


def _strip_code(v):
    v = copy.deepcopy(v)
    v['targetRenderer'] = v['targetLevel'] = None
    for k in v['kernels']:
        for kv in CS.kernel_variants(k):
            kv['code'] = None
    return v


@unittest.skipUnless(CORPUS, 'set PMUNITY_COMPUTE_CORPUS')
class ComputeCorpusTest(unittest.TestCase):
    def test_corpus(self):
        import UnityPy
        n_export = 0
        rows = []
        if EXPORT:
            os.makedirs(EXPORT, exist_ok=True)
        for path in CORPUS:
            env = UnityPy.load(path)
            unity = env.file.unity_version
            stats = {'shaders': 0, 'kernels': 0, 'left out': 0, 'reference': 0}
            saved = {}
            for obj in env.objects:
                if obj.type.name != 'ComputeShader':
                    continue
                raw = obj.get_raw_data()
                t = obj.read_typetree()
                name = t['m_Name']
                with self.subTest(file=path, shader=name):
                    obj.save_typetree(t)
                    self.assertEqual(obj.get_raw_data(), raw, 'does not write back unchanged')
                    rs = [v['targetRenderer'] for v in t['variants']]
                    if CS.RENDERER_GLCORE not in rs:
                        continue
                    ref = t['variants'][rs.index(CS.RENDERER_GLES3)] if CS.RENDERER_GLES3 in rs else None
                    t2 = copy.deepcopy(t)
                    t2['variants'] = [v for v in t2['variants'] if v['targetRenderer'] != CS.RENDERER_GLES3]
                    r = CS.add_gles3(t2, unity, precision='default-samplers')
                    stats['shaders'] += 1
                    stats['kernels'] += r.kernels
                    stats['left out'] += len(r.failed)
                    for kn, why in r.failed:
                        print(f'  {os.path.basename(path)}: {name}: {kn}: {why}')
                    if r.status != 'converted':
                        continue
                    rs2 = [v['targetRenderer'] for v in t2['variants']]
                    ours = t2['variants'][rs2.index(CS.RENDERER_GLES3)]
                    if ref is not None and not r.failed:
                        stats['reference'] += 1
                        self.assertEqual(rs2, rs, 'place in the variant list')
                        self.assertEqual(ours['targetLevel'], ref['targetLevel'], 'level')
                        self.assertEqual(_strip_code(ours), _strip_code(ref), 'kernels and metadata')
                        if tuple(map(int, unity.split('.')[:2])) >= (2019, 1):
                            for ka, kb in zip(ours['kernels'], ref['kernels']):
                                for va, vb in zip(CS.kernel_variants(ka), CS.kernel_variants(kb)):
                                    self.assertEqual(declarations(bytes(va['code']).decode()),
                                                     declarations(bytes(vb['code']).decode()), ka['name'])
                    obj.save_typetree(t2)
                    saved[obj.path_id] = rs2
                    if EXPORT:
                        for k in ours['kernels']:
                            for kv in CS.kernel_variants(k):
                                with open(os.path.join(EXPORT, f'{n_export:04d}.comp'), 'w') as f:
                                    f.write(bytes(kv['code']).rstrip(b'\0').decode())
                                rows.append(f'{n_export:04d}\t{name}\t{os.path.basename(path)}\tkernel {k["name"]}\t')
                                n_export += 1
            if saved:
                # the file with the new variants writes and reads back
                env2 = UnityPy.load(env.file.save())
                for pid, rs2 in saved.items():
                    v = env2.file.objects[pid].read_typetree()['variants']
                    self.assertEqual([x['targetRenderer'] for x in v], rs2, f'{path}: {pid} read back')
            print(f'{path}: Unity {unity}: {stats["shaders"]} compute shaders with GLCore kernels, '
                  f'{stats["kernels"]} kernels translated, {stats["left out"]} left out, '
                  f'{stats["reference"]} compared with Unity\'s GLES3 variant')
        if EXPORT:
            with open(os.path.join(EXPORT, 'index.tsv'), 'w') as f:
                f.write('\n'.join(rows) + '\n')


if __name__ == '__main__':
    unittest.main()
