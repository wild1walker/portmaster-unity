"""Compares what `python -m pmunity convert` and `pmu convert` left: two run folders, each with
the converted data and the run's stdout, stderr and exit status (status, out.txt, err.txt).

    python convert_compare.py <py run> <rs run> [<data folder name>...]
    python convert_compare.py --tree <data folder> <data folder>     (the trees alone)

Equal means: the same exit status; the same stdout, the timings ("[   1.2s]") and the run
folder's own path apart; the same stderr unless the Python's is a traceback (pmu prints the
error's message); every data folder the same tree, byte for byte, empty folders included, the
journal (.pmunity.json) byte for byte except the order of audio.switch, which the Python writes
in its set's hash order (it changes from run to run). Prints what differs; exit status 1 then.
"""
import json
import os
import re
import sys


def norm(text, run):
    text = text.replace(os.path.abspath(run), '<run>')
    return re.sub(r'^\[ *[0-9.]+s\]', '[t]', text, flags=re.M)


def files(top):
    out = {}
    for d, dirs, names in os.walk(top):
        rel = os.path.relpath(d, top)
        if not dirs and not names:
            out[rel + '/'] = None
        for n in names:
            p = os.path.join(d, n)
            out[os.path.normpath(os.path.join(rel, n))] = p
    return out


def journal_same(a, b):
    if a == b:
        return True
    ja, jb = json.loads(a), json.loads(b)
    if ja != jb:
        return False
    sa, sb = (j.get('audio', {}).get('switch') for j in (ja, jb))
    for j in (ja, jb):
        if 'audio' in j:
            j['audio']['switch'] = dict(sorted(j['audio']['switch'].items()))
    return json.dumps(ja) == json.dumps(jb) and sa == sb


def compare_trees(a_top, b_top, label, bad):
    fa, fb = files(a_top), files(b_top)
    for rel in sorted(set(fa) | set(fb)):
        if rel not in fa or rel not in fb:
            bad.append(f'{label}/{rel}: only the {"second" if rel in fb else "first"} has it')
            continue
        if fa[rel] is None:
            continue
        with open(fa[rel], 'rb') as x, open(fb[rel], 'rb') as y:
            same, at = True, 0
            while True:
                p, q = x.read(1 << 20), y.read(1 << 20)
                if p != q:
                    same = False
                    at += next((i for i in range(min(len(p), len(q))) if p[i] != q[i]), min(len(p), len(q)))
                    break
                if not p:
                    break
                at += len(p)
        if os.path.basename(rel) == '.pmunity.json':
            a, b = open(fa[rel]).read(), open(fb[rel]).read()
            if not journal_same(a, b):
                bad.append(f'{label}/{rel} differs:\n  first  {a[:2000]}\n  second {b[:2000]}')
        elif not same:
            bad.append(f'{label}/{rel} differs ({os.path.getsize(fa[rel])} / {os.path.getsize(fb[rel])} bytes, first at {at})')


def main():
    if sys.argv[1] == '--tree':
        bad = []
        compare_trees(sys.argv[2], sys.argv[3], '.', bad)
        for b in bad:
            print(b)
        print(f'{len(files(sys.argv[2]))} paths compared, {len(bad)} differ')
        return 1 if bad else 0
    py, rs = sys.argv[1:3]
    folders = sys.argv[3:] or ['Data']
    bad = []
    st = [open(os.path.join(r, 'status')).read().strip() for r in (py, rs)]
    if st[0] != st[1]:
        bad.append(f'exit status: python {st[0]}, pmu {st[1]}')
    for name in ('out.txt', 'err.txt'):
        a, b = (norm(open(os.path.join(r, name), errors='replace').read(), r) for r in (py, rs))
        if name == 'err.txt' and 'Traceback' in a:
            continue
        if a != b:
            bad.append(f'{name} differs:\n--- python\n{a}--- pmu\n{b}')
    for folder in folders:
        compare_trees(os.path.join(py, folder), os.path.join(rs, folder), folder, bad)
    for b in bad:
        print(b)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
