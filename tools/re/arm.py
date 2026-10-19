#!/usr/bin/env python3
"""Tiny ARM (A32) helper for libunity.so: string xrefs + annotated disassembly.
usage: arm.py xref <string>       -> functions referencing the string
       arm.py dis <sym|0xaddr> [n] -> annotated objdump of a function
       arm.py callers <sym|0xaddr> -> functions with bl to that address
"""
import sys, struct, subprocess, bisect, re, os
import numpy as np
from elftools.elf.elffile import ELFFile

# the player package (unpacked from Unity's Android Build Support installer): PMUNITY_ANDROID_PLAYER
BASE = os.path.join(os.environ.get('PMUNITY_ANDROID_PLAYER', '.'), 'Variations', 'mono', 'Release')
LIB = os.environ.get('ARMLIB', BASE + '/Libs/armeabi-v7a/libunity.so')
SYM = os.environ.get('ARMSYM', BASE + '/Symbols/armeabi-v7a/libunity.sym.so')
HERE = os.environ.get('ARMCACHE', os.path.dirname(os.path.abspath(__file__)))

f = open(LIB, 'rb')
elf = ELFFile(f)
segs = [(s['p_vaddr'], s['p_offset'], s['p_filesz']) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
data = open(LIB, 'rb').read()

def v2o(v):
    for va, off, sz in segs:
        if va <= v < va + sz:
            return off + v - va
    return None

def cstr(v):
    o = v2o(v)
    if o is None:
        return None
    e = data.find(b'\0', o, o + 400)
    if e < 0:
        return None
    s = data[o:e]
    if len(s) < 2 or any(c < 9 or c > 126 for c in s):
        return None
    return s.decode('latin1')

text = elf.get_section_by_name('.text')
TA, TS = text['sh_addr'], text['sh_size']
TO = v2o(TA)
words = np.frombuffer(data[TO:TO + TS - (TS % 4)], dtype='<u4')

# symbols
symcache = os.path.join(HERE, 'syms.tsv')
if not os.path.exists(symcache):
    se = ELFFile(open(SYM, 'rb'))
    out = []
    for sec in se.iter_sections():
        if sec['sh_type'] in ('SHT_SYMTAB', 'SHT_DYNSYM'):
            for s in sec.iter_symbols():
                if s['st_info']['type'] == 'STT_FUNC' and s['st_value']:
                    out.append((s['st_value'] & ~1, s['st_size'], s.name))
    out = sorted(set(out))
    with open(symcache, 'w') as w:
        for a, sz, n in out:
            w.write('%x\t%d\t%s\n' % (a, sz, n))
syms = []
for line in open(symcache):
    a, sz, n = line.rstrip('\n').split('\t')
    syms.append((int(a, 16), int(sz), n))
saddr = [s[0] for s in syms]
_dem = {}
import os as _os
_dp = _os.path.join(HERE, 'syms_dem.tsv')
if _os.path.exists(_dp):
    _mangled = [l.rstrip('\n').split('\t')[2] for l in open(symcache)]
    _demd = [l.rstrip('\n').split('\t', 2)[2] for l in open(_dp)]
    _dem = dict(zip(_mangled, _demd))

def demangle(names):
    todo = [n for n in names if n not in _dem]
    if todo:
        p = subprocess.run(['c++filt', '-n'], input='\n'.join(todo), capture_output=True, text=True)
        for a, b in zip(todo, p.stdout.split('\n')):
            _dem[a] = b
    return [_dem.get(n, n) for n in names]

def symat(a):
    i = bisect.bisect_right(saddr, a) - 1
    if i < 0:
        return None, 0
    s = syms[i]
    return s, a - s[0]

def symname(a):
    s, off = symat(a)
    if not s:
        return hex(a)
    n = demangle([s[2]])[0]
    return n + ('+0x%x' % off if off else '')

def find_sym(q):
    if q.startswith('0x'):
        a = int(q, 16)
        s, off = symat(a)
        return s
    cands = [s for s in syms if s[2] == q]
    if not cands:
        dn = demangle([s[2] for s in syms])
        cands = [s for s, d in zip(syms, dn) if d == q or d.startswith(q + '(')]
    if not cands:
        dn = demangle([s[2] for s in syms])
        cands = [s for s, d in zip(syms, dn) if q in d]
    return cands[0] if cands else None

def str_xrefs(target):
    """find (ldr literal, add rX, pc, rX) pairs producing target address"""
    res = []
    n = len(words)
    idx = np.arange(n, dtype=np.int64)
    addr = TA + idx * 4
    w = words.astype(np.int64)
    # A = target - 8 - W must be in text and an 'add rd, pc, rm' (cond AL)
    A = (target - 8 - w) & 0xffffffff
    ok = (A >= TA) & (A < TA + TS) & ((A & 3) == 0) & (np.abs(A - addr) < 8192)
    for i in np.nonzero(ok)[0]:
        a = int(A[i])
        ins = int(words[(a - TA) // 4])
        if (ins & 0x0fff0000) != 0x008f0000 or (ins & 0x0ff0) != 0:
            continue
        rm = ins & 0xf
        P = int(addr[i])
        found = False
        for b in range(a - 4, max(TA, a - 4096), -4):
            li = int(words[(b - TA) // 4])
            # ldr rt, [pc, #+/-imm12]  cond 0101 U001 1111 Rt imm12
            if (li & 0x0f7f0000) == 0x051f0000 and ((li >> 12) & 0xf) == rm:
                off = li & 0xfff
                la = b + 8 + (off if li & 0x00800000 else -off)
                if la == P:
                    found = True
                break
        if found:
            res.append(a)
    # also absolute pointers (in data) to the string
    return res

def absptr_xrefs(target):
    res = []
    pat = struct.pack('<I', target)
    for va, off, sz in segs:
        seg = data[off:off + sz]
        i = seg.find(pat)
        while i >= 0:
            if i % 4 == 0:
                res.append(va + i)
            i = seg.find(pat, i + 1)
    return res

def find_string(s):
    b = s.encode() + b'\0'
    out = []
    i = data.find(b)
    while i >= 0:
        # convert offset to vaddr
        for va, off, sz in segs:
            if off <= i < off + sz:
                out.append(va + i - off)
        i = data.find(b, i + 1)
    return out

def dis(start, end):
    p = subprocess.run(['objdump', '-d', '--no-show-raw-insn', '--start-address=0x%x' % start,
                        '--stop-address=0x%x' % end, LIB], capture_output=True, text=True)
    lines = p.stdout.split('\n')
    regs = {}
    out = []
    lit = {}
    for ln in lines:
        m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', ln)
        if not m:
            continue
        a = int(m.group(1), 16)
        op, args = m.group(2), m.group(3)
        args = re.sub(r'<[^>]*>', '', args)
        note = ''
        mm = re.match(r'(r\d+|r1[0-2]|lr|ip), \[pc, #(-?0x[0-9a-f]+|-?\d+)\]', args)
        if op.startswith('ldr') and mm:
            off = int(mm.group(2), 0)
            la = a + 8 + off
            o = v2o(la)
            if o is not None:
                val = struct.unpack('<I', data[o:o + 4])[0]
                regs[mm.group(1)] = val
                note = '=0x%x' % val
                st = cstr(val)
                if st:
                    note += ' "%s"' % st
        mm = re.match(r'(r\d+|lr|ip), pc, (r\d+|lr|ip)$', args)
        if op == 'add' and mm and mm.group(2) in regs:
            v = (regs[mm.group(2)] + a + 8) & 0xffffffff
            st = cstr(v)
            note = '-> 0x%x' % v + (' "%s"' % st if st else '')
            s, off = symat(v)
            if not st and s and off == 0:
                note += ' &' + symname(v)
        mm = re.match(r'(?:0x)?([0-9a-f]+)', args)
        if op in ('bl', 'blx', 'b', 'beq', 'bne') and mm and op != 'b' or (op == 'b' and mm):
            try:
                t = int(mm.group(1), 16)
                if op in ('bl', 'blx') or not (start <= t < end):
                    note = symname(t)
                    if TA <= t < TA + TS:
                        pass
                    else:
                        note = plt_name(t) or note
            except Exception:
                pass
        out.append('%8x: %-6s %-28s %s' % (a, op, args.strip(), note))
    return out

_plt = None
def plt_name(t):
    global _plt
    if _plt is None:
        _plt = {}
        relplt = elf.get_section_by_name('.rel.plt')
        dsym = elf.get_section_by_name('.dynsym')
        plt = elf.get_section_by_name('.plt')
        # ARM PLT: 20-byte header then 12-byte entries in rel.plt order
        pa = plt['sh_addr'] + 20
        for i, r in enumerate(relplt.iter_relocations()):
            _plt[pa + i * 12] = dsym.get_symbol(r['r_info_sym']).name
    return _plt.get(t)

def callers(target):
    res = []
    for i in range(len(words)):
        pass
    # vectorized BL decode: cond 1110 1011 imm24
    w = words.astype(np.int64)
    isbl = (w & 0x0f000000) == 0x0b000000
    imm = w & 0xffffff
    imm = np.where(imm & 0x800000, imm - 0x1000000, imm)
    addr = TA + np.arange(len(words), dtype=np.int64) * 4
    tgt = addr + 8 + imm * 4
    for i in np.nonzero(isbl & (tgt == target))[0]:
        res.append(int(addr[i]))
    # blx imm (thumb) skip
    return res

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'xref':
        for sa in find_string(sys.argv[2]):
            print('string @ 0x%x' % sa)
            for a in str_xrefs(sa):
                print('  code 0x%x in %s' % (a, symname(a)))
            for a in absptr_xrefs(sa):
                print('  ptr  0x%x in %s' % (a, symname(a)))
    elif cmd == 'xrefa':
        sa = int(sys.argv[2], 16)
        for a in str_xrefs(sa):
            print('  code 0x%x in %s' % (a, symname(a)))
        for a in absptr_xrefs(sa):
            print('  ptr  0x%x in %s' % (a, symname(a)))
    elif cmd == 'dis':
        s = find_sym(sys.argv[2])
        if not s:
            sys.exit('no sym')
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 0
        size = s[1] or 0x400
        if n:
            size = n
        print('== %s @0x%x size %d' % (demangle([s[2]])[0], s[0], s[1]))
        print('\n'.join(dis(s[0], s[0] + size)))
    elif cmd == 'callers':
        s = find_sym(sys.argv[2])
        t = s[0]
        print('== callers of %s' % demangle([s[2]])[0])
        for a in callers(t):
            print('  0x%x in %s' % (a, symname(a)))
    elif cmd == 'sym':
        for s in syms:
            d = demangle([s[2]])[0]
            if re.search(sys.argv[2], d):
                print('%x %d %s' % (s[0], s[1], d))
