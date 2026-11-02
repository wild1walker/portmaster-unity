"""il2cpp_compare.py <run a> <run b> [--noise <run c>]...: compares two runs of
tests/golden/il2cpp_device.sh (e.g. the Python's and pmu's): exit status, every file left (the
manifests: contents and modes), and the build's text records with what changes from run to run
anyway taken out (timings, the progress lines, the order parallel compiles finish in). --noise
<run c>: a run of the same implementation as <run a> (two Python runs): files that differ between
a and c differ from build to build anyway (libil2cpp's Runtime.cpp embeds __DATE__ and __TIME__)
and are reported as such, not as failures. Exit 0 when nothing else differs."""
import json
import os
import re
import sys


# names made from what changes from build to build: the link's result folder (a hash of its
# inputs, Runtime.cpp's object among them) and its response file
HASHED = [(re.compile(r'linkresult_[0-9A-F]{32}'), 'linkresult_<hash>'),
          (re.compile(r'/tmp/tmp[A-Za-z0-9_]{6}\.tmp'), '/tmp/tmp<name>.tmp')]


def unhash(t):
    for rx, rep in HASHED:
        t = rx.sub(rep, t)
    return t


CC_OBJ = re.compile(r' (\S+\.(?:cpp|c|cc)) -o (\S+\.o)\b')


def objects_by_source(run):
    """il2cpp's object cache names its objects by hashes, of the source's contents among other
    things: {object path under out/: 'out/.work/cache/<its source, under out/ or home/>.o'} from
    cc.log, so that runs whose generated C++ differs in a file (2020.x's metadata tables are not
    written the same each time) compare that file's object by its source."""
    out = {}
    text = ''
    for w in (('out', '.work'), ('out', '.il2cpp', '.work')):        # (il2cpp's, apk --il2cpp's)
        try:
            text += open(os.path.join(run, 'records', *w, 'cc.log'), encoding='utf-8', errors='replace').read()
        except OSError:
            pass
    for m in CC_OBJ.finditer(text):
        src, obj = m.group(1), m.group(2)
        i = obj.rfind('/out/')
        j = max(src.rfind('/out/'), src.rfind('/home/'))
        if i >= 0 and j >= 0:
            out[obj[i + 1:]] = 'out/.work/cache/<' + src[j + 1:] + '>.o'
    return out


def manifest(run):
    by_src = objects_by_source(run)
    out = {}
    with open(os.path.join(run, 'manifest')) as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('L '):
                path, target = line[2:].split(' -> ', 1)
                out[unhash(path)] = ('link', target)
            else:
                sha, mode, path = line.split(' ', 2)
                out[unhash(by_src.get(path, path))] = (sha, mode)
    return out


TIMING = [
    (re.compile(r'\b\d+(\.\d+)? (s|min|MB|MiB|GB)\b'), '<n> \\2'),
    (re.compile(r'\b\d+(\.\d+)? MB to download'), '<n> MB to download'),
    (re.compile(r'(CPU|peak|available|swap|RAM) \d+(\.\d+)?'), '\\1 <n>'),
    (re.compile(r'\(\d+ requests\)'), '(<n> requests)'),
]
PROGRESS = re.compile(r'^  (compiled \d+ of|converting: |retry \d+ at )')


def norm_text(name, text):
    lines = unhash(text).splitlines()
    if name in ('stderr', 'stdout'):
        lines = [l for l in lines if not PROGRESS.match(l)]
        out = []
        for l in lines:
            for rx, rep in TIMING:
                l = rx.sub(rep, l)
            l = re.sub(r'"build_seconds": \d+', '"build_seconds": <n>', l)
            out.append(l)
        return out
    if name == 'cc.log':
        # (the objects' names: hashes of what they are built from, see objects_by_source)
        return sorted(re.sub(r' -o \S+/cache/[0-9A-F]{32}\.o\b', ' -o <object>', l) for l in lines)
    if name in ('pmunity-done.jsonl', 'killed.log'):
        return sorted(lines)
    if name == 'il2cpp.log':
        # (il2cpp's own timings: the ten longest compiles and the totals)
        return [re.sub(r'Duration: \d+s\d+ms', 'Duration: <t>', re.sub(r'\d+ milliseconds', '<n> milliseconds', l))
                for l in lines if not l.startswith('\tTime Compile: ')]
    if name == 'build.log':
        # (blocks in the order the parallel commands finished; the timing of the last line)
        return sorted(re.sub(r', \d+ s$', ', <n> s', l) for l in lines)
    return lines


def norm_json(d):
    d = dict(d)
    d.pop('build_seconds', None)
    od = d.get('on_device')
    if isinstance(od, dict):
        od = dict(od)
        for k in ('peak_mb', 'cpu_seconds'):
            if isinstance(od.get(k), dict):
                od[k] = sorted(od[k])
        if isinstance(od.get('scheduler'), dict):
            s = dict(od['scheduler'])
            for k in ('most_at_once', 'waited_for_memory_s', 'memory_scale'):
                s[k] = '<varies>' if k in s else None
            od['scheduler'] = s
        od['mem_total_mb'] = '<machine>'
        d['on_device'] = od
    return d


def records(run):
    base = os.path.join(run, 'records')
    out = {}
    for r, _, fs in os.walk(base):
        for n in fs:
            p = os.path.join(r, n)
            out[os.path.relpath(p, base)] = p
    for n in ('stdout', 'stderr', 'status'):
        out[n] = os.path.join(run, n)
    return out


def main(argv):
    a, b = argv[0], argv[1]
    noise = [argv[i + 1] for i, x in enumerate(argv) if x == '--noise']
    fails = []
    ma, mb = manifest(a), manifest(b)
    only_a, only_b = sorted(set(ma) - set(mb)), sorted(set(mb) - set(ma))
    differ = sorted(p for p in set(ma) & set(mb) if ma[p] != mb[p])
    noisy = set()
    for c in noise:
        mc = manifest(c)
        noisy |= {p for p in set(ma) | set(mc) if ma.get(p) != mc.get(p)}
    same = len(set(ma) & set(mb)) - len(differ)
    print(f'manifest: {len(ma)} / {len(mb)} files, {same} the same')
    for p in only_a:
        (print if p in noisy else fails.append)(f'  only in {os.path.basename(a)}: {p}')
    for p in only_b:
        (print if p in noisy else fails.append)(f'  only in {os.path.basename(b)}: {p}')
    expected = [p for p in differ if p in noisy]
    for p in differ:
        if p not in noisy:
            fails.append(f'  differs: {p} {ma[p]} {mb[p]}')
    if expected:
        print(f'  {len(expected)} differ that also differ between the runs of one implementation:')
        for p in expected:
            print(f'    {p}')
    ra, rb = records(a), records(b)
    for name in sorted(set(ra) | set(rb)):
        if name not in ra or name not in rb:
            fails.append(f'  record only in one run: {name}')
            continue
        ta = open(ra[name], encoding='utf-8', errors='replace').read()
        tb = open(rb[name], encoding='utf-8', errors='replace').read()
        if name.endswith('android_package.json'):
            ja, jb = json.loads(ta), json.loads(tb)
            for j in (ja, jb):
                if isinstance(j.get('il2cpp'), dict):
                    j['il2cpp'].pop('build_seconds', None)
            if ja != jb:
                fails.append(f'  {name}: differs: {ja} {jb}')
            continue
        if name.endswith('il2cpp.json'):
            ja, jb = norm_json(json.loads(ta)), norm_json(json.loads(tb))
            if ja != jb:
                keys = sorted(k for k in set(ja) | set(jb) if ja.get(k) != jb.get(k))
                fails.append(f'  {name}: differs in {keys}: {[(k, ja.get(k), jb.get(k)) for k in keys]}')
            continue
        la, lb = norm_text(os.path.basename(name), ta), norm_text(os.path.basename(name), tb)
        if la != lb:
            import difflib
            d = list(difflib.unified_diff(la, lb, 'a/' + name, 'b/' + name, lineterm='', n=1))
            if any(p == 'out/' + name or name.endswith(p) for p in noisy) and not name.endswith(('.log', 'stderr', 'stdout')):
                print(f'  record {name} differs (also between runs of one implementation)')
                continue
            fails.append(f'  record {name} differs:\n' + '\n'.join(d[:60]))
    print(f'records: {len(set(ra) & set(rb))} compared')
    if fails:
        print('DIFFERENCES:')
        print('\n'.join(fails))
        return 1
    print('OK: the same' + (' (but for what differs between runs of one implementation)' if expected else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
