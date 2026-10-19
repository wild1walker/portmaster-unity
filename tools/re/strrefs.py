#!/usr/bin/env python3
"""Per-function .rodata string references in an ARM32 (ARM-mode, PIC) shared object.

Recognises the PIC idioms clang/gcc emit for Android:
   ldr rX, [pc, #imm]   ; literal = string - (P + 8)
   add rX, pc, rX       ; P = address of the add
 and   movw/movt rX + add rX, pc, rX.
Only functions whose referenced strings match FILTER (regex) are printed.

usage: python3 -I strrefs.py LIB.so LIB.sym.so [FILTER_REGEX] [--all]
Output: one block per function: "FUNC addr size demangled-name" followed by the strings.
"""
import sys, re, struct, bisect, subprocess
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection
import capstone


def main():
    lib, symf = sys.argv[1], sys.argv[2]
    filt = re.compile(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith('--') else None
    f = open(lib, 'rb')
    e = ELFFile(f)
    secs = {s.name: s for s in e.iter_sections()}
    text = secs['.text']
    tbase = text['sh_addr']
    tdata = text.data()
    ro = secs['.rodata']
    ro_lo = ro['sh_addr']
    rodata = ro.data()
    ro_hi = ro_lo + len(rodata)

    def word(addr):
        if tbase <= addr < tbase + len(tdata) - 3:
            return struct.unpack_from('<I', tdata, addr - tbase)[0]
        return None

    def cstr(addr):
        if not (ro_lo <= addr < ro_hi):
            return None
        o = addr - ro_lo
        end = rodata.find(b'\0', o, o + 400)
        if end < 0:
            return None
        s = rodata[o:end]
        if len(s) < 1 or any(c < 0x20 or c > 0x7e for c in s):
            return None
        return s.decode('ascii')

    sf = open(symf, 'rb')
    se = ELFFile(sf)
    funcs, maps = [], []
    for s in se.iter_sections():
        if isinstance(s, SymbolTableSection):
            for sym in s.iter_symbols():
                t = sym['st_info']['type']
                if t == 'STT_FUNC' and sym['st_size'] > 0:
                    funcs.append((sym['st_value'] & ~1, sym['st_size'], sym.name, sym['st_value'] & 1))
                elif sym.name in ('$a', '$t'):
                    maps.append((sym['st_value'], sym.name))
    funcs = sorted(set(funcs))
    maps.sort()
    mapaddrs = [a for a, _ in maps]
    # demangle names in one go
    names = [fn[2] for fn in funcs]
    try:
        p = subprocess.run(['arm-linux-gnueabihf-c++filt'], input='\n'.join(names), capture_output=True, text=True)
        dem = p.stdout.split('\n')
    except Exception:
        dem = names
    md_arm = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    md_thumb = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
    ldr_re = re.compile(r'^(r\d+|ip|lr|sb|sl|fp), \[pc, #(-?0x[0-9a-f]+|-?\d+)\]$')
    addpc_re = re.compile(r'^(r\d+|ip|lr|sb|sl|fp), pc, (r\d+|ip|lr|sb|sl|fp)$')
    addpc2_re = re.compile(r'^(r\d+|ip|lr|sb|sl|fp), (r\d+|ip|lr|sb|sl|fp), pc$')
    add3_re = re.compile(r'^(\w+), (\w+), (\w+)$')
    mov_re = re.compile(r'^(r\d+|ip|lr|sb|sl|fp), #(0x[0-9a-f]+|\d+)$')
    for idx, (addr, size, name, thumb) in enumerate(funcs):
        if not (tbase <= addr < tbase + len(tdata)):
            continue
        mi = bisect.bisect_right(mapaddrs, addr) - 1
        is_thumb = thumb or (mi >= 0 and maps[mi][1] == '$t')
        md = md_thumb if is_thumb else md_arm
        code = tdata[addr - tbase: addr - tbase + size]
        regs = {}
        absr = {}
        refs = []

        def insns():
            off = 0
            step = 2 if is_thumb else 4
            while off < len(code):
                n = 0
                for ins in md.disasm_lite(code[off:], addr + off):
                    n += ins[1]
                    yield ins
                off += n
                if off < len(code):
                    off += step  # undecodable word (literal pool data): skip it
        for ins in insns():
            a, sz, mn, ops = ins
            if mn.startswith('ldr') and mn in ('ldr', 'ldr.w'):
                m = ldr_re.match(ops)
                if m:
                    pcv = (a + (4 if is_thumb else 8)) & ~3
                    w = word(pcv + int(m.group(2), 0))
                    if w is not None:
                        regs[m.group(1)] = w
                    continue
            if mn in ('movw',):
                m = mov_re.match(ops)
                if m:
                    regs[m.group(1)] = int(m.group(2), 0)
                    continue
            if mn in ('movt',):
                m = mov_re.match(ops)
                if m and m.group(1) in regs:
                    regs[m.group(1)] = (regs[m.group(1)] & 0xffff) | (int(m.group(2), 0) << 16)
                    continue
            if mn == 'add':
                pcv = a + (4 if is_thumb else 8)
                m = addpc_re.match(ops) or addpc2_re.match(ops)
                if m:
                    src = m.group(2)
                    if src in regs:
                        v = (regs[src] + pcv) & 0xffffffff
                        absr[m.group(1)] = v
                        s = cstr(v)
                        if s is not None:
                            refs.append(s)
                    continue
                m = add3_re.match(ops)
                if m:
                    d, x, y = m.groups()
                    v = None
                    if x in absr and y in regs:
                        v = absr[x] + regs[y]
                    elif y in absr and x in regs:
                        v = absr[y] + regs[x]
                    if v is not None:
                        v &= 0xffffffff
                        s = cstr(v)
                        if s is not None:
                            refs.append(s)
                    continue
        if not refs:
            continue
        if filt and not any(filt.search(r) for r in refs):
            continue
        print('FUNC 0x%07x %5d %s' % (addr, size, dem[idx] if idx < len(dem) else name))
        seen = []
        for r in refs:
            if r not in seen:
                seen.append(r)
        for r in seen:
            print('    %r' % r)


main()
