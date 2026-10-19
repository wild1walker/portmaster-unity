#!/usr/bin/env python3
"""Find JNINativeMethod tables {const char *name; const char *sig; void *fn;} in an
ARM32 shared object's data sections, and list JNI-looking strings.

usage: python3 -I jnitables.py LIB.so LIB.sym.so(optional, for function names)
"""
import sys, struct, bisect, re
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection


def load(path):
    f = open(path, 'rb')
    return f, ELFFile(f)


def main():
    lib = sys.argv[1]
    symf = sys.argv[2] if len(sys.argv) > 2 else None
    f, e = load(lib)
    secs = {s.name: s for s in e.iter_sections()}
    ro = secs['.rodata']
    ro_lo, ro_hi = ro['sh_addr'], ro['sh_addr'] + ro['sh_size']
    rodata = ro.data()
    tx = secs['.text']
    tx_lo, tx_hi = tx['sh_addr'], tx['sh_addr'] + tx['sh_size']

    def cstr(addr):
        if not (ro_lo <= addr < ro_hi):
            return None
        o = addr - ro_lo
        end = rodata.find(b'\0', o)
        try:
            return rodata[o:end].decode('ascii')
        except UnicodeDecodeError:
            return None

    names = []
    if symf:
        sf, se = load(symf)
        for s in se.iter_sections():
            if isinstance(s, SymbolTableSection):
                for sym in s.iter_symbols():
                    if sym['st_info']['type'] == 'STT_FUNC' and sym['st_value']:
                        names.append((sym['st_value'] & ~1, sym.name))
        names.sort()
    addrs = [a for a, _ in names]

    def fname(a):
        i = bisect.bisect_right(addrs, a & ~1) - 1
        if i >= 0 and addrs[i] == (a & ~1):
            return names[i][1]
        return '?'

    sigre = re.compile(r'^\(([ZBCSIJFDV]|L[\w/$]+;|\[)*\)([ZBCSIJFDV]|L[\w/$]+;|\[+([ZBCSIJFD]|L[\w/$]+;))$')
    for sname in ('.data', '.data.rel.ro', '.data.rel.ro.local'):
        if sname not in secs:
            continue
        s = secs[sname]
        d = s.data()
        base = s['sh_addr']
        i = 0
        while i + 12 <= len(d):
            n, g, fn = struct.unpack_from('<III', d, i)
            nm, sg = cstr(n), cstr(g)
            if nm and sg and sigre.match(sg) and tx_lo <= (fn & ~1) < tx_hi:
                # table start
                start = i
                rows = []
                while i + 12 <= len(d):
                    n, g, fn = struct.unpack_from('<III', d, i)
                    nm, sg = cstr(n), cstr(g)
                    if not (nm and sg and sigre.match(sg) and tx_lo <= (fn & ~1) < tx_hi):
                        break
                    rows.append((nm, sg, fn))
                    i += 12
                print('TABLE @0x%x (%s) entries=%d' % (base + start, sname, len(rows)))
                for nm, sg, fn in rows:
                    print('  %-36s %-60s 0x%07x %s' % (nm, sg, fn, fname(fn)))
                continue
            i += 4


main()
