"""memlog.py <mem.log>: what a rehearsed setup's memory sampler saw (tools/port/rehearse.sh, REHEARSE_MEMLOG=1):
the container's peak (all of its memory, and the anonymous part, which a handheld without swap cannot give back),
each program's peak RSS and anonymous memory with the step it was in, and each step's peak. The log's lines,
tab-separated, sizes in MB:

    <time> <the container's> <its anonymous> <its page cache> <name/pid RSS/anonymous/peak RSS ...> <step> <last line>
"""
import sys


def main():
    path = sys.argv[1]
    peak_cg = peak_anon = (0, '', '')
    progs = {}      # name -> [peak anonymous, peak RSS, peak of the peak RSS, step, last line]
    steps = {}      # step -> [the container's anonymous memory, the program with the most, its anonymous]
    n = 0
    with open(path, errors='replace') as f:
        for line in f:
            col = line.rstrip('\n').split('\t')
            if len(col) < 7:
                continue
            try:
                cg, anon = int(col[1]), int(col[2])
            except ValueError:
                continue
            procs, step, last = col[4].split(), col[5].strip() or '(start)', col[6].strip()
            n += 1
            if cg > peak_cg[0]:
                peak_cg = (cg, step, last)
            if anon > peak_anon[0]:
                peak_anon = (anon, step, last)
            s = steps.setdefault(step, [0, '', 0])
            s[0] = max(s[0], anon)
            for i in range(0, len(procs) - 1, 2):
                name = procs[i].rsplit('/', 1)[0]
                try:
                    rss, a, hwm = (int(x) for x in procs[i + 1].split('/'))
                except ValueError:
                    continue
                p = progs.setdefault(name, [0, 0, 0, '', ''])
                if a > p[0]:
                    p[0], p[3], p[4] = a, step, last
                p[1], p[2] = max(p[1], rss), max(p[2], hwm)
                if a > s[2]:
                    s[1], s[2] = name, a
    if not n:
        print(f'memlog: no samples in {path}')
        return
    print(f'memory ({n} samples, {path}):')
    print(f'  the container at most {peak_cg[0]} MB (page cache included), in "{peak_cg[1]}"')
    print(f'  its anonymous memory at most {peak_anon[0]} MB, in "{peak_anon[1]}": {peak_anon[2]}')
    for name, (a, rss, hwm, step, last) in sorted(progs.items(), key=lambda kv: -kv[1][0])[:8]:
        print(f'  {name}: anonymous {a} MB, RSS {rss} MB (peak RSS {hwm} MB)' + (f'; most in "{step}": {last}' if a else ''))
    print("  each step: the container's anonymous memory at most (the program with the most)")
    for step, (a, name, pa) in steps.items():
        print(f'    {a:5d} MB  {step}' + (f' ({name} {pa} MB)' if name else ''))


if __name__ == '__main__':
    main()
