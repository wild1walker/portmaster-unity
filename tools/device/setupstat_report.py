#!/usr/bin/env python3
"""setupstat_report.py <samples.jsonl> <name>=<t0>:<t1> ...: setupstat.py's samples summed up per
interval (epoch seconds, or 'start' / 'end'): the wall time, the cores' busy and iowait shares, the
FUSE daemon's CPU, the card's MB read and written and its busy share, MemAvailable's low, the
dirty and writeback memory's high, the clock and temperature range; then each watched process
that ran: its CPU in cores (its busiest threads' shares), peak VmHWM and RssAnon, threads, the
bytes and calls of its reads and writes."""
import json, sys

HZ = 100
S = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]


def at(t):
    if t == 'start':
        return S[0]['t']
    if t == 'end':
        return S[-1]['t']
    return float(t)


for spec in sys.argv[2:]:
    name, rng = spec.split('=')
    t0, t1 = (at(x) for x in rng.split(':'))
    w = [s for s in S if t0 <= s['t'] <= t1]
    if len(w) < 2:
        print(f'{name}: no samples')
        continue
    a, b = w[0], w[-1]
    dt = b['t'] - a['t']
    cores = []
    for c in ('cpu0', 'cpu1', 'cpu2', 'cpu3'):
        x, y = a['cpu'].get(c), b['cpu'].get(c)
        if not x or not y:
            cores.append('?')
            continue
        d = [q - p for p, q in zip(x, y)]
        tot = sum(d) or 1
        busy = (d[0] + d[1] + d[2] + d[5] + d[6]) / tot
        cores.append(f'{busy * 100:.0f}%/{d[4] / tot * 100:.0f}io')
    allc = [q - p for p, q in zip(a['cpu']['cpu'], b['cpu']['cpu'])]
    tot = sum(allc) or 1
    busy_all = (allc[0] + allc[1] + allc[2] + allc[5] + allc[6]) / tot * 4
    sysc = (allc[2] + allc[5] + allc[6]) / tot * 4
    # processes: CPU by pid between its first and last sample in the window
    first, last = {}, {}
    for s in w:
        for p in s['p']:
            k = (p['pid'], p['comm'])
            first.setdefault(k, (s, p))
            last[k] = (s, p)
    procs = []
    for k, (s1, p1) in first.items():
        s2, p2 = last[k]
        cpu = (p2['cpu'] - p1['cpu']) / HZ
        if cpu < 0.5 and k[1] != 'pmu':
            continue
        hwm = max((p.get('VmHWM', 0) for s in w for p in s['p'] if p['pid'] == k[0]), default=0)
        anon = max((p.get('RssAnon', 0) for s in w for p in s['p'] if p['pid'] == k[0]), default=0)
        thr = max((p.get('thr', 0) for s in w for p in s['p'] if p['pid'] == k[0]), default=0)
        rd = (p2.get('rchar', 0) - p1.get('rchar', 0)) / 1e6
        wr = (p2.get('wchar', 0) - p1.get('wchar', 0)) / 1e6
        scr = p2.get('syscr', 0) - p1.get('syscr', 0)
        scw = p2.get('syscw', 0) - p1.get('syscw', 0)
        # the busiest thread's share
        tc = {t: p2['tcpu'].get(t, 0) - p1['tcpu'].get(t, 0) for t in p2.get('tcpu', {})}
        top = sorted(tc.values(), reverse=True)[:5]
        tops = '/'.join(f'{v / HZ / dt * 100:.0f}' for v in top)
        procs.append(f'{k[1]}[{p1.get("arg", "")[:28]}] {cpu / dt:.2f} cores (threads {tops} %), '
                     f'VmHWM {hwm // 1024} MB, RssAnon {anon // 1024} MB, thr {thr}, rchar {rd:.0f} MB '
                     f'({scr} calls), wchar {wr:.0f} MB ({scw} calls)')
    fuse = (b.get('fuse', 0) - a.get('fuse', 0)) / HZ / dt
    dr = (b['disk'][0] - a['disk'][0]) * 512 / 1e6
    dw = (b['disk'][1] - a['disk'][1]) * 512 / 1e6
    dbusy = (b['disk'][2] - a['disk'][2]) / 1000 / dt
    avail = min(s['mem']['MemAvailable'] for s in w) // 1024
    dirty = max(s['mem']['Dirty'] + s['mem'].get('Writeback', 0) for s in w) // 1024
    swap = min(s['mem']['SwapFree'] for s in w) // 1024
    print(f'== {name}: {dt:.0f} s; CPU {busy_all:.2f} of 4 cores busy (kernel {sysc:.2f}), per core {" ".join(cores)}; '
          f'FUSE daemon {fuse:.2f} cores; card read {dr:.0f} MB ({dr / dt:.1f} MB/s), written {dw:.0f} MB '
          f'({dw / dt:.1f} MB/s), busy {dbusy * 100:.0f} %; MemAvailable low {avail} MB, dirty+writeback high {dirty} MB, swap free low {swap} MB; '
          f'clock {min(s["mhz"] for s in w)}-{max(s["mhz"] for s in w)} MHz, {min(s["temp"] for s in w)}-{max(s["temp"] for s in w)} C')
    for p in procs:
        print('   ' + p)
