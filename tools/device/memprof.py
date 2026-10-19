#!/usr/bin/env python3
"""Device-side: where a process's memory is, every <interval> s, from /proc/<pid>/smaps.
Usage: python3 memprof.py <pid> [interval] [top]   (until the process exits)
One line per sample: resident and swapped MB by mapping (anonymous memory, the GPU driver's
/dev/mali0 mappings, each file), plus the system's MemAvailable and swap use."""
import os, re, sys, time

pid = int(sys.argv[1]); iv = float(sys.argv[2]) if len(sys.argv) > 2 else 10; top = int(sys.argv[3]) if len(sys.argv) > 3 else 8
HDR = re.compile(r'^[0-9a-f]+-[0-9a-f]+ \S+ \S+ \S+ \d+\s*(.*)$')

def sample():
    rss, swap, cur = {}, {}, None
    try:
        for line in open(f'/proc/{pid}/smaps'):
            m = HDR.match(line)
            if m:
                n = m.group(1).strip() or '[anon]'
                if n.startswith('[anon:') or n.startswith('[stack'): n = '[anon]'
                cur = os.path.basename(n) if n.startswith('/') and not n.startswith('/dev/') else n
                continue
            if line.startswith('Rss:'):
                rss[cur] = rss.get(cur, 0) + int(line.split()[1])
            elif line.startswith('Swap:'):
                swap[cur] = swap.get(cur, 0) + int(line.split()[1])
    except OSError:
        return None
    return rss, swap

def meminfo():
    d = {}
    for l in open('/proc/meminfo'):
        k, v = l.split(':', 1); d[k] = int(v.split()[0])
    return d

while True:
    s = sample()
    if s is None: break
    rss, swap = s
    mi = meminfo()
    tot = sum(rss.values()) >> 10; tsw = sum(swap.values()) >> 10
    rows = sorted(rss.items(), key=lambda kv: -kv[1])[:top]
    print(f'[mem] {time.strftime("%T")} rss {tot} MB swap {tsw} MB | avail {mi["MemAvailable"] >> 10} MB, swap used {(mi["SwapTotal"] - mi["SwapFree"]) >> 10} MB | ' +
          '  '.join(f'{n} {v >> 10}' + (f'+{swap.get(n, 0) >> 10}sw' if swap.get(n, 0) >= 1024 else '') for n, v in rows), flush=True)
    time.sleep(iv)
