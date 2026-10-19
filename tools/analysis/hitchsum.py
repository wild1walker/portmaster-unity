#!/usr/bin/env python3
"""Summarize UA_HITCH_TRACE output (runtime/host/hitches.c): hitchsum.py <log> [--each] [--top N]

For every frame that was sampled, and for all of them together: the functions the sampled thread
was in most often, counted once per sample wherever they appear on the stack ("in", so callers
add up) and as the innermost named frame ("top"). libunity.so offsets are named from the
release player's libunity.sym.so (unitysyms.py: UA_LIBUNITY_SYM=<path>); other modules stay
module+offset.
--each prints the per-frame tables too (default: only frames of 200 ms or more get one line).
--state <name>: only frames in that boss state ("loading" for loads); --callers <function>: the
frames that called it, most frequent first.
--prof: the UA_PROF_EVERY samples instead ("[prof]" lines, all frames), per boss state: where the
sampled thread spends its time (innermost frame) and what is on its stack (inclusive)."""
import collections, os, re, sys

from unitysyms import namer
name = namer()

SAMPLE = re.compile(r'\[hitch-sample\] \+(\d+) (\S+) pc (\S+) lr (\S+) \|(.*)')
PROF = re.compile(r'\[prof\] (\S+) pc (\S+) lr (\S+) \|(.*)')
DONE = re.compile(r'\[hitch-sample\] frame of (\d+) ms, (\d+) samples')
# The game port's own log lines that say where the game is (groups: scene, time, state, end) and
# when it loads; the defaults are Cuphead's helper (portmaster-cuphead). Other ports: set
# UA_STATE_RE / UA_LOADING_RE.
BENCH = re.compile(os.environ.get('UA_STATE_RE', r'\[cupheadport bench\] (\w+?)(?:Level)? t=(\d+) state=(\S+) hp=\S+ x=\S+( WON)?'))
LOADING = re.compile(os.environ.get('UA_LOADING_RE', r'\[cupheadport\] (code|font|shader) warmup for |\[cupheadport\] memory trim \(scene|\[cupheadport bench\] next:'))

def prof_main(path, want, top_n, thread):
    per = collections.OrderedDict()
    where = 'loading'
    for line in open(path, errors='replace'):
        if LOADING.search(line): where = 'loading'
        m = BENCH.search(line)
        if m:
            where = 'loading' if m.group(4) else m.group(1) + '/' + m.group(3)
            continue
        m = PROF.search(line)
        if not m or (thread and m.group(1) != thread): continue
        if want and want not in where: continue
        st = [name(t) for t in [m.group(2)] + m.group(4).split()]
        d = per.setdefault(where, [0, collections.Counter(), collections.Counter()])
        d[0] += 1; d[1][st[0]] += 1; d[2].update(set(st))
    for where, (n, top, inc) in per.items():
        print(f'== {where}: {n} samples')
        print('  innermost:')
        for k, c in top.most_common(top_n): print(f'    {100 * c / n:5.1f}%  {k}')
        print('  on the stack:')
        for k, c in inc.most_common(top_n * 2): print(f'    {100 * c / n:5.1f}%  {k}')

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if '--prof' in sys.argv:
        want = sys.argv[sys.argv.index('--state') + 1] if '--state' in sys.argv else None
        top_n = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 15
        thread = sys.argv[sys.argv.index('--thread') + 1] if '--thread' in sys.argv else 'UnityMain'
        return prof_main(args[0], want, top_n, thread)
    each = '--each' in sys.argv
    top_n = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 12
    want = sys.argv[sys.argv.index('--state') + 1] if '--state' in sys.argv else None
    callers_of = sys.argv[sys.argv.index('--callers') + 1] if '--callers' in sys.argv else None
    frames, cur, where = [], [], 'loading'
    for line in open(args[0], errors='replace'):
        if LOADING.search(line): where = 'loading'
        m = BENCH.search(line)
        if m:
            where = 'loading' if m.group(4) else m.group(1) + '/' + m.group(3)
            continue
        m = SAMPLE.search(line)
        if m:
            stack = [m.group(3)] + m.group(5).split()
            cur.append((m.group(2), [name(t) for t in stack]))
            continue
        m = DONE.search(line)
        if m:
            if want is None or want in where:
                frames.append((int(m.group(1)), cur, where))
            cur = []
    tot_in, tot_top = collections.Counter(), collections.Counter()
    nsamp = 0
    callers = collections.Counter()
    for ms, samples, wh in frames:
        if callers_of:
            for thread, st in samples:
                for i, n in enumerate(st):
                    if callers_of in n:
                        callers[' < '.join(st[i + 1:i + 5])] += 1
                        break
        f_in, f_top = collections.Counter(), collections.Counter()
        for thread, st in samples:
            seen = set(st)
            f_in.update(seen); f_top[st[0]] += 1
        tot_in.update(f_in); tot_top.update(f_top); nsamp += len(samples)
        if each or ms >= 200:
            print(f'{wh:<24} frame {ms} ms, {len(samples)} samples: ' + '; '.join(f'{n} {c}' for n, c in f_top.most_common(3)))
            if each:
                for n, c in f_in.most_common(top_n): print(f'    in {c:3d}  {n}')
    if callers_of:
        print(f'\ncallers of {callers_of}:')
        for n, c in callers.most_common(top_n): print(f'  {c:4d}  {n}')
    print(f'\n{len(frames)} frames, {nsamp} samples')
    print('innermost:')
    for n, c in tot_top.most_common(top_n): print(f'  {100 * c / max(nsamp, 1):5.1f}%  {n}')
    print('on the stack:')
    for n, c in tot_in.most_common(top_n * 2): print(f'  {100 * c / max(nsamp, 1):5.1f}%  {n}')

if __name__ == '__main__':
    main()
