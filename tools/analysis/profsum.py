#!/usr/bin/env python3
"""Sum up a UA_PROF recording (runtime/host/prof.c writes <game folder>/prof-<n>.txt):

    profsum.py prof-1.txt [--syms DIR] [--lib FILE]... [--thread NAME] [--top N]
                          [--frames] [--slow MS] [--callers TEXT] [--offsets]

Per sampled thread: how its time splits (on a CPU, waiting for one, blocked and in which call), and
for the time on a CPU: by module (Unity's engine, Mono's runtime, managed code, libc, the GLES
driver, ...), by function ("self": where the pc was; "on the stack": the function is anywhere in the
sample, so callers add up), by managed method (the innermost JIT-compiled frame: the method and the
engine/runtime calls it makes), and who waits in the blocked samples. Then every thread's CPU time
over the recording and each core's use.

  --syms DIR   Unity's symbol files (Variations/mono/Release/Symbols/armeabi-v7a from
               `pmunity player fetch --symbols`): names libunity.so, libmonobdwgc-2.0.so, libmain.so
               (default: unitysyms.py's sources for libunity.so: $UA_SYMS, $UA_LIBUNITY_SYM)
  --lib FILE   another file with symbols (runtime/host/out/ua_host, a library copied from the
               device); glibc libraries are otherwise named by the profile's own dladdr names
  --thread     only this thread (default: all, UnityMain first)
  --frames     UnityMain's time on a CPU per frame (from its frame times and samples)
  --slow MS    only samples in UnityMain frames longer than MS ms (the frames that miss a vsync)
  --callers T  the callers of functions whose name contains T, most frequent first
  --offsets    keep offsets in managed method names (hot spots inside a method)
  --exact      only the callers that were unwound (not those found by scanning) for "on the
               stack", the managed method and --callers
Callers are unwound exactly through code with unwind tables (32-bit ARM EHABI; AArch64 frame
pointers) and found by scanning the stack above that (Mono's JIT code has no tables): scanned
entries can be stale, so without --exact "on the stack" is an upper bound."""
import argparse, bisect, collections, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from elfsyms import ElfNames  # noqa: E402

SYSCALLS = {
    'armhf': {3: 'read', 4: 'write', 54: 'ioctl', 142: 'select', 158: 'sched_yield', 162: 'nanosleep', 168: 'poll',
              179: 'rt_sigsuspend', 240: 'futex', 252: 'epoll_wait', 265: 'clock_nanosleep', 335: 'pselect6',
              336: 'ppoll', 346: 'epoll_pwait', 407: 'clock_nanosleep_time64', 414: 'ppoll_time64',
              422: 'futex_time64', 0xfffe: '(page fault)', 0xffff: '?'},
    'arm64': {22: 'epoll_pwait', 29: 'ioctl', 63: 'read', 64: 'write', 72: 'pselect6', 73: 'ppoll', 98: 'futex',
              101: 'nanosleep', 115: 'clock_nanosleep', 124: 'sched_yield', 133: 'rt_sigsuspend',
              0xfffe: '(page fault)', 0xffff: '?'},
}
GLES = ('libmali', 'libGLES', 'libEGL', 'libMali', 'mali_', 'libgpu', 'libvulkan')
LIBC = ('libc.so', 'libc-', 'ld-linux', 'libm.so', 'libpthread', 'libstdc++', 'libgcc_s', 'libdl', 'librt')
MONO = ('libmonobdwgc', 'libmono.so', 'libmonosgen')


class Profile:
    def __init__(self, path):
        self.meta, self.threads, self.modules, self.jits, self.syms = {}, {}, [], [], {}
        self.cpu, self.stat, self.stacks, self.seq = [], [], {}, collections.defaultdict(list)
        self.frames = {'swap': [], 'main': []}
        self.version = 1
        with open(path, errors='replace') as f:
            lines = f.read().splitlines()
        for line in lines:
            p = line.split()
            if not p:
                continue
            k = p[0]
            if k == 'stack':
                # version 1: no <exact> field, every caller scanned; 2: the first <exact> unwound, the
                # rest scanned; 3: scanned callers written ~<address>
                v2 = self.version >= 2
                words = p[9 if v2 else 8:]
                rets = [int(x.lstrip('~'), 16) for x in words]
                exact = int(p[8]) if v2 else 0
                if self.version >= 3:
                    scanned = [x.startswith('~') for x in words]
                else:
                    scanned = [i >= exact for i in range(len(words))]
                self.stacks[int(p[1])] = dict(count=int(p[2]), thread=int(p[3]), state=p[4], sysnr=int(p[5], 16),
                                              pc=int(p[6], 16), lr=int(p[7], 16), exact=exact, rets=rets, scanned=scanned)
            elif k == 'seq':
                t = int(p[1])
                for e in p[2:]:
                    us, sid = e.split(':')
                    self.seq[t].append((int(us), int(sid)))
            elif k == 'module':
                self.modules.append((int(p[1], 16), int(p[2], 16), int(p[3], 16), p[4]))
            elif k == 'jit':
                a = int(p[1], 16)
                self.jits.append((a, a + int(p[2], 16), ' '.join(p[3:])))
            elif k == 'sym':
                self.syms[int(p[1], 16)] = (p[2], int(p[3], 16))
            elif k == 'thread':
                self.threads[int(p[1])] = dict(tid=int(p[2]), name=p[3], samples=int(p[4]), dropped=int(p[5]))
            elif k == 'cpu':
                self.cpu.append(dict(tid=int(p[1]), name=p[2], run=int(p[3]), wait=int(p[4]), slices=int(p[5]),
                                     ut=int(p[6]), st=int(p[7])))
            elif k == 'stat':
                self.stat.append((p[1], [int(x) for x in p[2:]]))
            elif k in self.frames:
                self.frames[k] += [int(x) for x in p[1:]]
            elif k == 'depth':
                self.meta['depth'] = int(p[1])
            elif k in ('arch', 'hz', 'secs', 'signal', 'mono', 'ua_prof'):
                self.meta[k] = p[1]
                if k == 'ua_prof':
                    self.version = int(p[1])
        self.modules.sort()
        self.jits.sort()
        self.mod_lo = [m[0] for m in self.modules]
        self.jit_lo = [j[0] for j in self.jits]
        self.hz = int(self.meta.get('hz', 0) or 0)
        self.secs = float(self.meta.get('secs', 0) or 0)
        self.arch = self.meta.get('arch', 'armhf')

    def module(self, a):
        i = bisect.bisect_right(self.mod_lo, a) - 1
        if i >= 0 and a < self.modules[i][1]:
            return self.modules[i]
        return None

    def jit(self, a):
        i = bisect.bisect_right(self.jit_lo, a) - 1
        if i >= 0 and a < self.jits[i][1]:
            return self.jits[i]
        return None


class Namer:
    """address -> (category, name)"""
    def __init__(self, prof, elf, offsets=False):
        self.p, self.elf, self.offsets, self.cache = prof, elf, offsets, {}

    def __call__(self, a):
        if a in self.cache:
            return self.cache[a]
        r = self._name(a)
        self.cache[a] = r
        return r

    def _name(self, a):
        p = self.p
        j = p.jit(a)
        if j:
            n = j[2]
            if self.offsets:
                n += f'+0x{a - j[0]:x}'
            return ('managed', n)
        m = p.module(a)
        if not m:
            return ('anonymous code (Mono trampolines, unnamed JIT)', 'anon')
        mod, off = m[3], a - m[2]
        cat = category(mod)
        f = self.elf(mod, off) if self.elf else None
        if f:
            return (cat, f'{f}' if mod in ('libunity.so',) else f'{mod}:{f}')
        if a in p.syms:
            s, _ = p.syms[a]
            return (cat, f'{mod}:{s}')
        return (cat, f'{mod}+0x{off:x}')


def category(mod):
    if mod == 'libunity.so':
        return 'Unity engine (libunity)'
    if mod.startswith(MONO):
        return 'Mono runtime (GC, JIT, icall wrappers)'
    if mod.startswith(GLES):
        return 'GLES driver'
    if mod.startswith(LIBC):
        return 'libc, libm, libstdc++'
    if mod.startswith('ua_host'):
        return 'ua_host'
    if mod.startswith('libmain'):
        return 'libmain'
    return mod


def sample_frames(p, s, exact_only):
    """pc, then the callers: lr when the unwind did not already use it (32-bit ARM's unwinder starts
    from lr; AArch64's frame chain starts at the caller's caller), the unwound ones, then (unless
    exact_only) the scanned ones"""
    use_lr = s['lr'] and not (p.arch == 'armhf' and s['exact'])
    rets = [r for r, sc in zip(s['rets'], s['scanned']) if not sc] if exact_only else s['rets']
    return [s['pc']] + ([s['lr']] if use_lr else []) + rets


def pct(n, d):
    return f'{100.0 * n / d:5.1f}%' if d else '  -  '


def frame_of(times, us):
    """index of the frame (between frame times) that holds us, or None"""
    i = bisect.bisect_right(times, us)
    return i - 1 if 0 < i < len(times) else None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('profile')
    ap.add_argument('--syms', action='append', default=[])
    ap.add_argument('--lib', action='append', default=[])
    ap.add_argument('--thread')
    ap.add_argument('--top', type=int, default=15)
    ap.add_argument('--frames', action='store_true')
    ap.add_argument('--slow', type=float)
    ap.add_argument('--callers')
    ap.add_argument('--offsets', action='store_true')
    ap.add_argument('--exact', action='store_true')
    a = ap.parse_args(argv)
    p = Profile(a.profile)
    elf = ElfNames(dirs=a.syms, files=a.lib)
    name = Namer(p, elf, a.offsets)
    sysnames = SYSCALLS.get(p.arch, SYSCALLS['armhf'])
    period_ms = 1000.0 / p.hz if p.hz else 0
    swaps, mains = p.frames['swap'], p.frames['main']
    print(f'{os.path.basename(a.profile)}: {p.secs:.1f} s at {p.hz} Hz ({p.arch}), Mono names: {p.meta.get("mono", "?")}')
    if p.secs:
        def fr(t):
            if len(t) < 2:
                return f'{len(t)} frames'
            d = sorted(t[i + 1] - t[i] for i in range(len(t) - 1))
            return (f'{len(t) / p.secs:.1f} fps, frame {sum(d) / len(d) / 1000:.1f} ms avg / {d[len(d) // 2] / 1000:.1f} median / '
                    f'{d[min(len(d) - 1, int(len(d) * 0.99))] / 1000:.1f} p99, {sum(1 for x in d if x > 25000)} over 25 ms')
        print(f'  presented: {fr(swaps)}')
        print(f'  UnityMain: {fr(mains)}')
    order = sorted(p.threads, key=lambda i: (p.threads[i]['name'] != 'UnityMain', i))
    main_idx = next((i for i in order if p.threads[i]['name'] == 'UnityMain'), None)
    slow_ids = None
    if a.slow and main_idx is not None and len(mains) > 1:
        slow = {i for i in range(len(mains) - 1) if (mains[i + 1] - mains[i]) / 1000 > a.slow}
        print(f'  --slow {a.slow:g}: {len(slow)} of {len(mains) - 1} UnityMain frames are longer')
        slow_ids = {}
        for t, seq in p.seq.items():
            c = collections.Counter()
            for us, sid in seq:
                f = frame_of(mains, us)
                if f is not None and f in slow:
                    c[sid] += 1
            slow_ids[t] = c
    cpu_by_tid = {c['tid']: c for c in p.cpu}
    for ti in order:
        th = p.threads[ti]
        if a.thread and th['name'] != a.thread:
            continue
        stacks = []
        for sid, s in p.stacks.items():
            if s['thread'] != ti:
                continue
            n = s['count'] if slow_ids is None else slow_ids.get(ti, {}).get(sid, 0)
            if n:
                stacks.append((n, s))
        total = sum(n for n, _ in stacks)
        print(f'\n== {th["name"]} (tid {th["tid"]}): {total} samples' + (f', {th["dropped"]} dropped (buffer full)' if th['dropped'] else ''))
        if not total:
            continue
        c = cpu_by_tid.get(th['tid'])
        if c and p.secs:
            print(f'  schedstat: on a CPU {c["run"] / 1e6:.0f} ms ({pct(c["run"] / 1e9, p.secs)} of the time), waiting for a CPU '
                  f'{c["wait"] / 1e6:.0f} ms ({pct(c["wait"] / 1e9, p.secs).strip()}), {c["slices"]} slices; '
                  f'user {c["ut"] * 10} ms, kernel {c["st"] * 10} ms')
        states = collections.Counter()
        for n, s in stacks:
            st = s['state']
            if st in 'SD':
                st = f'{st} {sysnames.get(s["sysnr"], hex(s["sysnr"]))}'
            states[st] += n
        print('  state: ' + ', '.join(f'{k} {pct(v, total).strip()}' for k, v in states.most_common()) +
              '   (R on a CPU, Q runnable but its sample came late: no CPU or a long kernel path, S/D blocked in <call>)')
        run = [(n, s) for n, s in stacks if s['state'] == 'R' and s['pc']]
        nrun = sum(n for n, _ in run)
        if nrun:
            mods, selfc, incl, managed, mself = (collections.Counter() for _ in range(5))
            unwound = udepth = 0
            callers = collections.Counter()
            for n, s in run:
                cat, fn = name(s['pc'])
                mods[cat] += n
                selfc[fn] += n
                if cat == 'managed':
                    mself[fn] += n
                frames = sample_frames(p, s, a.exact)
                names = [name(x) for x in frames]
                if s['exact']:
                    unwound += n
                    udepth += n * sum(1 for sc in s['scanned'] if not sc)
                for fnn in {x[1] for x in names}:
                    incl[fnn] += n
                inner = next((x[1] for x in names if x[0] == 'managed'), None)
                managed[inner or '(no managed frame: engine or runtime alone)'] += n
                if a.callers:
                    seq = [x[1] for x in names]
                    for i, x in enumerate(seq):
                        if a.callers in x:
                            nxt = [y for y in seq[i + 1:] if y != x][:4]
                            callers[' < '.join(nxt) or '(none)'] += n
                            break
            print(f'  on a CPU ({nrun} samples, {nrun * period_ms:.0f} ms): {pct(unwound, nrun).strip()} unwound'
                  + (f' ({udepth / unwound:.1f} callers on average), the rest scanned' if unwound else ', all scanned') + '; by module:')
            for k, v in mods.most_common():
                print(f'    {pct(v, nrun)}  {k}')
            print('  self (the pc):')
            for k, v in selfc.most_common(a.top):
                print(f'    {pct(v, nrun)}  {k}')
            print('  on the stack (inclusive):')
            for k, v in incl.most_common(a.top * 2):
                print(f'    {pct(v, nrun)}  {k}')
            if mself:
                print('  managed code itself (pc in JIT code):')
                for k, v in mself.most_common(a.top):
                    print(f'    {pct(v, nrun)}  {k}')
            if any(not k.startswith('(no managed') for k in managed):
                print('  by innermost managed method (the method and the engine/runtime calls it makes):')
                for k, v in managed.most_common(a.top):
                    print(f'    {pct(v, nrun)}  {k}')
            if a.callers:
                print(f'  callers of *{a.callers}*:')
                for k, v in callers.most_common(a.top):
                    print(f'    {v:5d}  {k}')
        blocked = [(n, s) for n, s in stacks if s['state'] in 'SD']
        nb = sum(n for n, _ in blocked)
        if nb:
            waits = collections.Counter()
            for n, s in blocked:
                frames = [s['pc']] + s['rets']
                names = [name(x) for x in frames if x]
                who = next((x[1] for x in names if not x[0].startswith('libc')), names[0][1] if names else
                           '(no stack: the call was not readable, e.g. under QEMU)')
                waits[f'{sysnames.get(s["sysnr"], hex(s["sysnr"]))} from {who}'] += n
            print(f'  blocked ({nb} samples, {nb * period_ms:.0f} ms): who waits')
            for k, v in waits.most_common(a.top):
                print(f'    {pct(v, nb)}  {k}')
    if a.frames and main_idx is not None and len(mains) > 1:
        busy = collections.Counter()
        for us, sid in p.seq.get(main_idx, []):
            f = frame_of(mains, us)
            if f is not None and p.stacks[sid]['state'] in 'RQ':
                busy[f] += 1
        rows = sorted(((mains[i + 1] - mains[i]) / 1000, busy[i] * period_ms) for i in range(len(mains) - 1))
        b = sorted(r[1] for r in rows)
        print(f'\n== UnityMain per frame ({len(rows)} frames): on a CPU (R+Q) {sum(b) / len(b):.1f} ms avg, '
              f'{b[len(b) // 2]:.1f} median, {b[int(len(b) * 0.9)]:.1f} p90, {b[-1]:.1f} max')
        hist = collections.Counter(min(int(x // 4) * 4, 60) for x in b)
        for k in sorted(hist):
            print(f'    {k:2d}-{k + 4:<3d} ms {hist[k]:5d} {"#" * min(60, hist[k] * 60 // max(hist.values()))}')
    if p.cpu and p.secs:
        print('\n== threads by CPU time over the recording (schedstat):')
        tot = sum(c['run'] for c in p.cpu)
        for c in sorted(p.cpu, key=lambda c: -c['run'])[:12]:
            print(f'    {100 * c["run"] / 1e9 / p.secs:5.1f}% of a core  {c["name"]} (waited for a CPU {c["wait"] / 1e6:.0f} ms)')
        print(f'    total {100 * tot / 1e9 / p.secs:.0f}% of a core')
    if p.stat:
        print('== cores (/proc/stat): busy (user/system), iowait')
        for name_, v in p.stat:
            if name_ == 'cpu' or len(v) < 7:
                continue
            tot = sum(v)
            if tot:
                print(f'    {name_}: {pct(tot - v[3] - v[4], tot)} busy ({pct(v[0] + v[1], tot).strip()} user, '
                      f'{pct(v[2] + v[5] + v[6], tot).strip()} system), iowait {pct(v[4], tot).strip()}')


if __name__ == '__main__':
    main()
