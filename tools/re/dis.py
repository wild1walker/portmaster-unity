#!/usr/bin/env python3
"""Disassemble functions of an Android .so by name, with the symbols of its .sym.so.
ARM (ARM and Thumb, from the symbol's low bit) and AArch64.

usage: python3 -I dis.py LIB.so SYM.so NAME_REGEX [MAX_FUNCS]
       python3 -I dis.py LIB.so SYM.so str:STRING_REGEX [MAX_FUNCS]   (AArch64: functions whose
                                                     ADRP+ADD string references match)
Prints each matching function (demangled name if c++filt is around) and its instructions,
with PC-relative literal loads resolved to C strings where they point at one.
"""
import sys, re, bisect, subprocess, struct
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection
import capstone


def main():
    lib, symf, rx = sys.argv[1], sys.argv[2], sys.argv[3]
    by_str = rx.startswith('str:')
    rx = re.compile(rx[4:] if by_str else rx)
    maxf = int(sys.argv[4]) if len(sys.argv) > 4 else 5
    e = ELFFile(open(lib, 'rb'))
    a64 = e['e_machine'] == 'EM_AARCH64'
    text = e.get_section_by_name('.text')
    tbase, tdata = text['sh_addr'], text.data()
    segs = [(s['p_vaddr'], s['p_filesz'], s['p_offset']) for s in e.iter_segments() if s['p_type'] == 'PT_LOAD']
    raw = open(lib, 'rb').read()

    def cstr(addr):
        for va, fs, off in segs:
            if va <= addr < va + fs:
                o = off + addr - va
                end = raw.find(b'\0', o, o + 200)
                s = raw[o:end]
                if end > o and all(0x20 <= c < 0x7f for c in s):
                    return s.decode()
        return None

    funcs, maps = [], []
    for s in ELFFile(open(symf, 'rb')).iter_sections():
        if isinstance(s, SymbolTableSection):
            for sym in s.iter_symbols():
                if sym['st_info']['type'] == 'STT_FUNC' and sym['st_size']:
                    funcs.append((sym['st_value'], sym['st_size'], sym.name))
                elif sym.name in ('$a', '$t', '$x', '$d'):
                    maps.append((sym['st_value'], sym.name))
    maps.sort()
    names = [f[2] for f in funcs]
    try:
        dem = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True).stdout.split('\n')
        if len(dem) < len(names):
            raise OSError
    except OSError:
        try:
            dem = subprocess.run(['arm-linux-gnueabihf-c++filt'], input='\n'.join(names), capture_output=True,
                                 text=True).stdout.split('\n')
        except OSError:
            dem = names
    fsorted = sorted(((a & ~1), dd) for (a, _, _), dd in zip(funcs, dem))
    faddrs = [a for a, _ in fsorted]

    def fname(t):
        i = bisect.bisect_right(faddrs, t) - 1
        if i >= 0 and faddrs[i] == t:
            return fsorted[i][1][:100]
        return None

    n = 0
    for (addr, size, name), d in zip(funcs, dem):
        if not by_str and not (rx.search(d) or rx.search(name)):
            continue
        thumb = (not a64) and bool(addr & 1)       # ARM ELF: bit 0 of a function symbol = Thumb
        addr &= ~1
        if not tbase <= addr < tbase + len(tdata):
            continue
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM) if a64 else \
            capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM)
        lines = ['FUNC 0x%x %d %s%s' % (addr, size, d, ' [thumb]' if thumb else '')]
        code = tdata[addr - tbase:addr - tbase + size]
        adrp = {}
        hit = not by_str
        for ins in md.disasm(code, addr):
            note = ''
            m = re.match(r'(\w+), \[pc, #(-?0x[0-9a-f]+|-?\d+)\]', ins.op_str)
            if not a64 and ins.mnemonic.startswith('ldr') and m:
                pc = ((ins.address + (4 if thumb else 8)) & ~3) + int(m.group(2), 0)
                if tbase <= pc < tbase + len(tdata) - 3:
                    note = ' ; =0x%x' % struct.unpack_from('<I', tdata, pc - tbase)[0]
            if a64 and ins.mnemonic == 'adrp':
                m = re.match(r'(\w+), #(0x[0-9a-f]+)', ins.op_str)
                if m:
                    adrp[m.group(1)] = int(m.group(2), 16)
            if a64 and ins.mnemonic == 'add':
                m = re.match(r'(\w+), (\w+), #(0x[0-9a-f]+|\d+)', ins.op_str)
                if m and m.group(2) in adrp:
                    v = adrp[m.group(2)] + int(m.group(3), 0)
                    s = cstr(v)
                    note = ' ; 0x%x%s' % (v, ' "%s"' % s if s else '')
                    if by_str and s and rx.search(s):
                        hit = True
            if ins.mnemonic in ('bl', 'blx', 'b', 'b.w', 'bl.w') and ins.op_str.startswith('#'):
                fn = fname(int(ins.op_str[1:], 16))
                if fn:
                    note = ' ; ' + fn
            lines.append('  %x: %-8s %s%s' % (ins.address, ins.mnemonic, ins.op_str, note))
        if not hit:
            continue
        print('\n'.join(lines))
        n += 1
        if n >= maxf:
            break


main()
