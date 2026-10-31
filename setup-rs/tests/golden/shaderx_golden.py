"""Golden data for setup-rs/crates/shaderx: the shaders step's own per-shader work (shaders/step.py:
the files it searches, convert.add_gles3 on each Shader) on a game's data, saved for the crate's
golden tests (crates/shaderx/tests/golden.rs, which read the folder from $SHADERX_GOLDEN).

    python shaderx_golden.py <data dir> <config.json> <out dir> [--converted <data dir>] [--work <dir>]

The out folder gets
  index.json        one row per Shader object: file, path id, the content hashes of its input, the
                    step's output (null: the step leaves it as it is) and, with --converted, the same
                    object in that folder (a convert run's output: the reference the step wrote)
  obj/<hash>.json   a Shader's fields the step reads or writes (name, platforms, offsets, lengths,
                    stageCounts, every pass's m_Platforms and subprogram lists, whole subprograms),
                    with the step's result for outputs; obj/<hash>.blob: its compressedBlob
  programs.jsonl    every distinct program translation (glsl.program_to_es through convert's cache):
                    code in, precision, rewrites, ES version, code out or the Unsupported raised, notes
  lz4.jsonl         made-up inputs and lz4.block.compress(mode='high_compression') of each
  log.txt           the step's summary lines for the shaders seen

Each output is checked here to differ from its input only in the fields saved (the Rust step returns
those); a row says so in `unmodeled` otherwise. Only the 'glcore' source is covered.
"""
import argparse
import copy
import hashlib
import json
import os
import random
import re
import shutil
import sys

from pmunity.shaders import blob as B
from pmunity.shaders import convert as C
from pmunity.shaders import glsl
from pmunity.shaders import step
from pmunity.unity_io import Loaded, NotOneSerializedFile

LISTS = ('m_SubPrograms', 'm_PlayerSubPrograms', 'm_ParameterBlobIndices')
BLOB_FIELDS = ('platforms', 'offsets', 'compressedLengths', 'decompressedLengths', 'stageCounts')


class _Job:
    """What step.shader_files needs of a job (read-only)."""

    def __init__(self, src):
        self.src = os.path.abspath(src)

    def list(self, pattern_dir, predicate):
        d = os.path.join(self.src, pattern_dir)
        if not os.path.isdir(d):
            return []
        return sorted(os.path.join(pattern_dir, n) if pattern_dir else n
                      for n in os.listdir(d) if predicate(n) and os.path.isfile(os.path.join(d, n)))


def modeled(t):
    """The fields of a Shader tree the step reads or changes (compressedBlob apart)."""
    out = {'name': t['m_ParsedForm']['m_Name']}
    for k in BLOB_FIELDS:
        if k in t:
            out[k] = t[k]
    subs = []
    for ss in t['m_ParsedForm']['m_SubShaders']:
        passes = []
        for p in ss['m_Passes']:
            q = {}
            if 'm_Platforms' in p:
                q['m_Platforms'] = p['m_Platforms']
            for st in C.STAGES:
                prog = p.get(st)
                if prog is None:
                    continue
                q[st] = {k: prog[k] for k in LISTS if k in prog}
            passes.append(q)
        subs.append(passes)
    out['subshaders'] = subs
    return out


def unmodeled(t):
    """The tree without the fields modeled() saves."""
    t = copy.deepcopy(t)
    for k in BLOB_FIELDS + ('compressedBlob',):
        t.pop(k, None)
    for ss in t['m_ParsedForm']['m_SubShaders']:
        for p in ss['m_Passes']:
            p.pop('m_Platforms', None)
            for st in C.STAGES:
                prog = p.get(st)
                if isinstance(prog, dict):
                    for k in LISTS:
                        prog.pop(k, None)
    return t


def _plain(v):
    if isinstance(v, (bytes, bytearray)):
        return list(v)
    if isinstance(v, tuple):
        return list(v)
    raise TypeError(type(v))


def save(objdir, fields, blob):
    """Content-addressed: returns the hash."""
    js = json.dumps(fields, sort_keys=True, default=_plain).encode()
    h = hashlib.sha1(js + b'\0' + blob).hexdigest()[:20]
    p = os.path.join(objdir, h)
    if not os.path.exists(p + '.json'):
        with open(p + '.json', 'wb') as f:
            f.write(js)
        with open(p + '.blob', 'wb') as f:
            f.write(blob)
    return h


def result_fields(r):
    return {'name': r.name, 'status': r.status, 'changed': r.changed, 'why': r.why, 'programs': r.programs,
            'twins': r.twins, 'guessed_layout': r.guessed_layout, 'notes': list(r.notes), 'dropped': r.dropped,
            'es31': r.es31, 'programs4x': r.programs4x, 'empty': r.empty,
            'failed': [[si, w] for si, w in r.failed]}


def lz4_cases(path):
    """Inputs that reach LZ4HC's corners: short ones, runs (its pattern analysis), repeats at every
    distance up to and past 64 KB, text-like data."""
    import lz4.block
    rnd = random.Random(1234)
    cases = [b'', b'a', b'abc' * 4, b'\0' * 12, b'\0' * 13, b'\0' * 100, bytes(range(256)) * 3]
    for n in list(range(0, 40)) + [64, 65, 100, 1000, 4096]:
        cases.append(bytes(rnd.randrange(4) for _ in range(n)))
    words = [b'vec4', b'float', b'u_xlat0', b' = ', b';\n', b'texture(', b'_MainTex', b'.xyzw', b'0.5', b'    ']
    for n in (100, 2000, 70000, 200000):
        cases.append(b''.join(rnd.choice(words) for _ in range(n // 5)))
    for run in (3, 4, 5, 8, 15, 16, 17, 255, 300, 70000):
        cases.append(b'xy' + b'\x55' * run + b'tail' + b'\x55' * (run // 2) + b'z' * 20)
    for d in (1, 2, 3, 4, 7, 8, 100, 65534, 65535, 65536, 65537, 100000):
        base = bytes(rnd.randrange(256) for _ in range(min(d, 70000)))
        cases.append((base * (2 + 140000 // max(len(base), 1)))[:140000 + d % 7])
    cases.append(bytes(rnd.randrange(256) for _ in range(300000)))
    cases.append(b''.join(bytes([rnd.randrange(3)]) * rnd.randrange(1, 40) for _ in range(20000)))
    with open(path, 'w') as f:
        for c in cases:
            z = lz4.block.compress(c, mode='high_compression', store_size=False)
            f.write(json.dumps({'in': c.hex(), 'out': z.hex()}) + '\n')
    return len(cases)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('data')
    ap.add_argument('config')
    ap.add_argument('out')
    ap.add_argument('--converted')
    ap.add_argument('--work')
    a = ap.parse_args()
    with open(a.config) as f:
        cfg = json.load(f)
    c = cfg.get('shaders', {})
    sources = c.get('from', ['glcore'])
    sources = sources.split(',') if isinstance(sources, str) else sources
    if list(sources) != ['glcore']:
        sys.exit(f'only the glcore source is covered (config: {sources})')
    precision = c.get('precision', 'highp')
    replace = c.get('replace', False)
    bundles = c.get('bundles', '.')
    opt = tuple(c.get('optimize', ('const-arrays', 'unroll')))
    glcore4x = c.get('glcore4x')
    objdir = os.path.join(a.out, 'obj')
    os.makedirs(objdir, exist_ok=True)
    work = a.work or os.path.join(a.out, 'work')
    job = _Job(a.data)
    rows, results, changed_files = [], [], 0
    for rel in step.shader_files(job, bundles):
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(work, exist_ok=True)
        try:
            lf = Loaded(os.path.join(job.src, rel), spill_dir=work)
        except NotOneSerializedFile as ex:
            print(f'{rel}: left as it is ({ex})', flush=True)
            continue
        gl4 = step.glcore4x_for(rel, glcore4x)
        n = 0
        file_rows = {}
        for pid, o in lf.objects('Shader'):
            t = o.read_typetree()
            src = next((s for s in sources if step.SOURCES[s] in t['platforms']), sources[0])
            assert src == 'glcore'
            tin = copy.deepcopy(t)
            hin = save(objdir, modeled(tin), bytes(tin['compressedBlob']))
            r = C.add_gles3(t, precision=precision, replace=replace, optimize=opt, glcore4x=gl4)
            r.glcore4x = gl4
            r.file = rel
            r.platforms = tuple(t['platforms'])
            results.append(r)
            row = {'file': rel, 'path_id': pid, 'in': hin, 'out': None, 'glcore4x': gl4,
                   'result': result_fields(r)}
            if r.status == 'converted' or r.changed:
                n += 1
                fields = modeled(t)
                row['out'] = save(objdir, fields, bytes(t['compressedBlob']))
                if unmodeled(t) != unmodeled(tin):
                    row['unmodeled'] = True
            elif t != tin:
                row['unmodeled'] = True          # (the step would not save it, but the tree changed)
            file_rows[pid] = row
            rows.append(row)
        if n:
            changed_files += 1
        lf.close()
        if a.converted and os.path.exists(os.path.join(a.converted, rel)):
            shutil.rmtree(work, ignore_errors=True)
            os.makedirs(work, exist_ok=True)
            cf = Loaded(os.path.join(a.converted, rel), spill_dir=work)
            for pid, o in cf.objects('Shader'):
                t = o.read_typetree()
                h = save(objdir, modeled(t), bytes(t['compressedBlob']))
                if pid in file_rows:
                    file_rows[pid]['converted'] = h
                else:
                    rows.append({'file': rel, 'path_id': pid, 'in': None, 'out': None, 'converted': h})
            cf.close()
        print(f'{rel}: {len(file_rows)} shaders, {n} changed', flush=True)
    shutil.rmtree(work, ignore_errors=True)
    with open(os.path.join(a.out, 'index.json'), 'w') as f:
        json.dump({'precision': precision, 'replace': replace, 'optimize': list(opt), 'glcore4x': glcore4x,
                   'changed_files': changed_files, 'rows': rows}, f, indent=0)
    with open(os.path.join(a.out, 'programs.jsonl'), 'w') as f:
        for (code, prec, optimize, es), (res, notes) in C._ES.items():
            rec = {'code': code.decode('utf-8'), 'precision': prec, 'optimize': list(optimize), 'es': es,
                   'notes': notes}
            if isinstance(res, Exception):
                rec['error'] = [type(res).__name__, str(res)]
            else:
                rec['out'] = res.decode('utf-8')
            f.write(json.dumps(rec) + '\n')
    nl = lz4_cases(os.path.join(a.out, 'lz4.jsonl'))
    conv = [r for r in results if r.status == 'converted']
    log = (f'shaders: {len(conv)} shaders given GLES3 programs ({sum(r.programs for r in conv)} programs, '
           f'{sum(r.twins for r in conv)} subprograms) in {changed_files} files; '
           f'{sum(r.status == "skipped" for r in results)} not translatable; precision {precision}; '
           f'rewrites: {sum(len(r.notes) for r in conv)} stages ({", ".join(sorted({n.split(" ", 1)[1].rsplit(" ", 1)[0] for r in conv for n in r.notes})) or "none"})')
    with open(os.path.join(a.out, 'log.txt'), 'w') as f:
        f.write(log + '\n')
    print(log)
    print(f'{len(rows)} rows, {len(C._ES)} distinct translations, {nl} lz4 cases, '
          f'{sum(1 for r in rows if r.get("unmodeled"))} with unmodeled changes')


if __name__ == '__main__':
    main()
