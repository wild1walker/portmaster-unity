#!/usr/bin/env python3
"""Call tree + string refs for functions of an ARM32 .so using the unstripped .sym.so.

usage: python3 -I calltree.py LIB.so SYM.so DEPTH NAME_OR_ADDR...
  NAME_OR_ADDR: hex address (0x...) or a substring of the demangled name (first match).
Prints, for each function: callees (bl/blx imm, PLT stubs resolved to imported symbol
names) and the C strings it references (PIC ldr/add pc idiom), recursing DEPTH levels.
"""
import sys, re, struct, bisect, subprocess
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection
from elftools.elf.relocation import RelocationSection
import capstone

lib, symf, depth = sys.argv[1], sys.argv[2], int(sys.argv[3])
targets = sys.argv[4:]
f = open(lib, 'rb'); e = ELFFile(f)
secs = {s.name: s for s in e.iter_sections()}
text = secs['.text']; tbase = text['sh_addr']; tdata = text.data()
ro = secs['.rodata']; ro_lo = ro['sh_addr']; rodata = ro.data()
plt = secs['.plt']; plt_lo = plt['sh_addr']; plt_hi = plt_lo + plt['sh_size']
# PLT: 20-byte header, 12-byte entries in .rel.plt order
dynsym = secs['.dynsym']
pltnames = []
for r in secs['.rel.plt'].iter_relocations():
    pltnames.append(dynsym.get_symbol(r['r_info_sym']).name)

syms = []
se = ELFFile(open(symf, 'rb'))
maps = []
for s in se.iter_sections():
    if isinstance(s, SymbolTableSection):
        for sym in s.iter_symbols():
            if sym['st_info']['type'] == 'STT_FUNC' and sym['st_value'] and sym['st_size']:
                syms.append((sym['st_value'] & ~1, sym['st_size'], sym.name))
            elif sym.name in ('$a', '$t'):
                maps.append((sym['st_value'], sym.name))
syms = sorted(set(syms)); maps.sort()
addrs = [s[0] for s in syms]; mapaddrs = [m[0] for m in maps]
p = subprocess.run(['arm-linux-gnueabihf-c++filt'], input='\n'.join(s[2] for s in syms), capture_output=True, text=True)
dem = p.stdout.split('\n')


def name_at(a):
    if plt_lo <= a < plt_hi:
        i = (a - plt_lo - 20) // 12
        return 'PLT:' + (pltnames[i] if 0 <= i < len(pltnames) else '?')
    i = bisect.bisect_right(addrs, a) - 1
    if i >= 0 and syms[i][0] <= a < syms[i][0] + syms[i][1]:
        off = a - syms[i][0]
        return dem[i] + ('+0x%x' % off if off else '')
    return '0x%x' % a


def cstr(a):
    if not (ro_lo <= a < ro_lo + len(rodata)):
        return None
    o = a - ro_lo; end = rodata.find(b'\0', o, o + 300)
    s = rodata[o:end] if end > 0 else b''
    if len(s) < 1 or any(c < 0x20 or c > 0x7e for c in s):
        return None
    return s.decode()


md = {False: capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM),
      True: capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)}
ldr_re = re.compile(r'^(\w+), \[pc, #(-?0x[0-9a-f]+|-?\d+)\]$')
add3_re = re.compile(r'^(\w+), (\w+), (\w+)$')
add_re = re.compile(r'^(\w+), pc, (\w+)$')
bl_re = re.compile(r'^#(0x[0-9a-f]+)$')


def analyse(idx):
    a0, size, _ = syms[idx]
    mi = bisect.bisect_right(mapaddrs, a0) - 1
    th = mi >= 0 and maps[mi][1] == '$t'
    code = tdata[a0 - tbase: a0 - tbase + size]
    regs, calls, strs, absr = {}, [], [], {}
    off = 0
    while off < len(code):
        n = 0
        for a, sz, mn, ops in md[th].disasm_lite(code[off:], a0 + off):
            n += sz
            if mn in ('ldr', 'ldr.w'):
                m = ldr_re.match(ops)
                if m:
                    pa = ((a + (4 if th else 8)) & ~3) + int(m.group(2), 0)
                    if tbase <= pa < tbase + len(tdata) - 3:
                        regs[m.group(1)] = struct.unpack_from('<I', tdata, pa - tbase)[0]
            elif mn == 'add':
                pcv = a + (4 if th else 8)
                m = add_re.match(ops)
                v = None
                if m and m.group(2) in regs:
                    v = (regs[m.group(2)] + pcv) & 0xffffffff
                    absr[m.group(1)] = v
                else:
                    m = add3_re.match(ops)
                    if m:
                        d, x, y = m.groups()
                        if x in absr and y in regs:
                            v = (absr[x] + regs[y]) & 0xffffffff
                        elif y in absr and x in regs:
                            v = (absr[y] + regs[x]) & 0xffffffff
                if v is not None:
                    s = cstr(v)
                    if s is not None and s not in strs:
                        strs.append(s)
            elif mn in ('bl', 'blx', 'b') or (mn.startswith('b') and len(mn) <= 3 and mn != 'bic'):
                m = bl_re.match(ops)
                if m:
                    t = int(m.group(1), 16)
                    if mn == 'b' or len(mn) == 3 and mn not in ('blx',):
                        # plain/conditional branch: only count tail calls outside this function
                        if a0 <= t < a0 + size:
                            continue
                    nm = name_at(t)
                    if nm not in calls:
                        calls.append(nm)
        off += n
        if off < len(code):
            off += 2 if th else 4
    return calls, strs


def find(t):
    if t.startswith('0x'):
        a = int(t, 16)
        i = bisect.bisect_right(addrs, a) - 1
        return i
    for i, d in enumerate(dem[:len(syms)]):
        if d.startswith(t) or d == t:
            return i
    for i, d in enumerate(dem[:len(syms)]):
        if t in d:
            return i
    return None


seen = set()


def walk(idx, lvl, ind):
    calls, strs = analyse(idx)
    print('%s%s @0x%x' % (ind, dem[idx], syms[idx][0]))
    if strs:
        print('%s    strings: %s' % (ind, ' | '.join(repr(s) for s in strs)))
    for c in calls:
        print('%s    -> %s' % (ind, c))
    if lvl < depth:
        for c in calls:
            if c.startswith('PLT:') or '+0x' in c or c.startswith('0x'):
                continue
            j = find(c)
            if j is not None and j not in seen:
                seen.add(j)
                walk(j, lvl + 1, ind + '    ')


for t in targets:
    i = find(t)
    if i is None:
        print('not found', t)
        continue
    seen.add(i)
    walk(i, 0, '')
    print()
