#!/usr/bin/env python3
"""Small helpers for poking at an ARM32 .so (file vaddr == link address, base 0).
usage: peek.py LIB.so SYM.so cmd args...
  sym ADDR...          -> nearest symbol (demangled) for each address
  ptrstr ADDR...       -> read 32-bit word at ADDR, print it and the C string it points to
  str ADDR...          -> C string at ADDR
  words ADDR N         -> N words at ADDR
  find NAME_REGEX      -> symbols matching regex (addr size name)
"""
import sys, struct, bisect, re, subprocess
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection

lib, symf, cmd, args = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
f = open(lib, 'rb'); e = ELFFile(f)
segs = [(s['p_vaddr'], s['p_filesz'], s['p_offset']) for s in e.iter_segments() if s['p_type'] == 'PT_LOAD']


def read(addr, n):
    for va, fs, off in segs:
        if va <= addr < va + fs:
            f.seek(off + addr - va)
            return f.read(n)
    return None


def cstr(addr):
    b = read(addr, 512)
    if not b:
        return None
    return b.split(b'\0')[0].decode('latin1')


syms = []
se = ELFFile(open(symf, 'rb'))
for s in se.iter_sections():
    if isinstance(s, SymbolTableSection):
        for sym in s.iter_symbols():
            if sym['st_info']['type'] in ('STT_FUNC', 'STT_OBJECT') and sym['st_value']:
                syms.append((sym['st_value'] & ~1, sym['st_size'], sym.name))
syms.sort()
addrs = [s[0] for s in syms]


def dem(n):
    return subprocess.run(['arm-linux-gnueabihf-c++filt', n], capture_output=True, text=True).stdout.strip()


def near(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0:
        return '?'
    sa, sz, nm = syms[i]
    return '%s+0x%x (size %d)' % (dem(nm), a - sa, sz)


if cmd == 'sym':
    for a in args:
        a = int(a, 16)
        print('0x%x %s' % (a, near(a)))
elif cmd == 'ptrstr':
    for a in args:
        a = int(a, 16)
        w = struct.unpack('<I', read(a, 4))[0]
        print('0x%x -> 0x%x %r  [%s]' % (a, w, cstr(w), near(a)))
elif cmd == 'str':
    for a in args:
        print('0x%s %r' % (a, cstr(int(a, 16))))
elif cmd == 'words':
    a, n = int(args[0], 16), int(args[1])
    ws = struct.unpack('<%dI' % n, read(a, 4 * n))
    for i, w in enumerate(ws):
        s = cstr(w)
        print('0x%x: 0x%08x %s' % (a + 4 * i, w, repr(s) if s and s.isprintable() and len(s) > 1 else ''))
elif cmd == 'find':
    r = re.compile(args[0])
    for sa, sz, nm in syms:
        d = dem(nm) if r.search(nm) else None
        if d and r.search(d) or r.search(nm):
            print('0x%07x %6d %s' % (sa, sz, d or dem(nm)))
