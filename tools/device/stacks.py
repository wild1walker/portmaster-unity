#!/usr/bin/env python3
"""Device-side: what each thread of a running ua_host is blocked in. For every thread (or the
ones whose name contains a filter) prints the current syscall and the return-address-looking
words on its stack that fall in a mapped .so, as <module>+<offset> (feed the libunity offsets to
host/sym.py on the Mac). Usage: python3 stacks.py <pid> [name-filter] [words]"""
import os, re, struct, sys

pid = int(sys.argv[1]); filt = sys.argv[2] if len(sys.argv) > 2 else ''
nwords = int(sys.argv[3]) if len(sys.argv) > 3 else 1024

maps = []
for l in open(f'/proc/{pid}/maps'):
    m = re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \d+\s*(.*)', l)
    if m and 'x' in m.group(3) and m.group(5):
        maps.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(4), 16), m.group(5)))
# module base = start of its first mapping (file offset 0)
bases = {}
for l in open(f'/proc/{pid}/maps'):
    m = re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \d+\s*(.*)', l)
    if m and m.group(5) and int(m.group(4), 16) == 0 and m.group(5) not in bases:
        bases[m.group(5)] = int(m.group(1), 16)

def where(v):
    for s, e, off, path in maps:
        if s <= v < e:
            base = bases.get(path, s - off)
            return f'{os.path.basename(path)}+0x{v - base:x}'
    return None

mem = open(f'/proc/{pid}/mem', 'rb', 0)
for t in sorted(os.listdir(f'/proc/{pid}/task'), key=int):
    name = open(f'/proc/{pid}/task/{t}/comm').read().strip()
    if filt and filt not in name:
        continue
    sc = open(f'/proc/{pid}/task/{t}/syscall').read().split()
    print(f'== {t} {name}: syscall {" ".join(sc[:4])}')
    if len(sc) < 3 or sc[0] == 'running':
        continue
    sp, pc = int(sc[-2], 16), int(sc[-1], 16)
    print(f'   pc {where(pc) or hex(pc)}')
    try:
        mem.seek(sp); data = mem.read(nwords * 4)
    except OSError as e:
        print('   stack:', e); continue
    shown = 0
    for i in range(len(data) // 4):
        v = struct.unpack_from('<I', data, i * 4)[0]
        w = where(v & ~1) if v > 0x10000 else None
        if w and not w.endswith(('+0x0',)):
            print(f'   sp+{i * 4:#06x} {w}')
            shown += 1
            if shown >= 24:
                break
