#!/usr/bin/env python3
"""The rows of runtime/host/unityhooks.c for one Unity player: each hooked function's offset, size,
instruction set and the FNV-1a 64 hash of its bytes, read from the player's libunity.so with Unity's
symbol file for it (`pmunity player fetch <version> --symbols`: Variations/<backend>/Release/Symbols/
<abi>/libunity.sym.so).

    python3 tools/re/unityhooks.py <libunity.so> <libunity.sym.so> [--unity <version abi>]

It checks what the runtime's hook relies on, and prints no row where a check fails:
  - the symbol file belongs to this library (the same GNU build id, when the library has one; else
    the function's bytes must at least be in the library's text);
  - the bytes the jump replaces (8 in ARM state, 16 in A64) are instructions that can run anywhere
    else: no pc-relative operand, no branch, no literal load;
  - nothing in the function branches into those bytes after the first instruction.
The function's meaning (its arguments, the enum values the hook compares) is read by hand for each
version before a row is added; the hash then ties the row to exactly that code. Thumb functions are
not hooked (the ARM players' functions here are in ARM state).
Pure Python, no Unity code is printed."""
import argparse
import struct
import sys

HOOKS = [
    # (mangled-name prefix, short name)
    ('_Z25AdjustThreadingModeForAPI', 'AdjustThreadingModeForAPI'),
]


class Elf:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        if d[:4] != b'\x7fELF':
            raise SystemExit(f'{path}: not an ELF file')
        self.is64 = d[4] == 2
        if self.is64:
            self.machine, = struct.unpack_from('<H', d, 0x12)
            phoff, = struct.unpack_from('<Q', d, 0x20)
            shoff, = struct.unpack_from('<Q', d, 0x28)
            phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', d, 0x36)
        else:
            self.machine, = struct.unpack_from('<H', d, 0x12)
            phoff, = struct.unpack_from('<I', d, 0x1c)
            shoff, = struct.unpack_from('<I', d, 0x20)
            phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', d, 0x2a)
        self.segs, self.notes = [], []
        for i in range(phnum):
            o = phoff + i * phentsize
            if self.is64:
                t, fl, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, o)
            else:
                t, off, va, pa, fs, ms, fl, al = struct.unpack_from('<IIIIIIII', d, o)
            if t == 1:
                self.segs.append((va, off, fs, fl))
            elif t == 4:
                self.notes.append((off, fs))
        self.secs = []
        for i in range(shnum):
            o = shoff + i * shentsize
            if self.is64:
                v = struct.unpack_from('<IIQQQQIIQQ', d, o)
            else:
                v = struct.unpack_from('<IIIIIIIIII', d, o)
            self.secs.append(v)       # name, type, flags, addr, off, size, link, info, align, entsize

    def build_id(self):
        d = self.d
        srcs = list(self.notes) + [(s[4], s[5]) for s in self.secs if s[1] == 7]
        for off, size in srcs:
            p = off
            while p + 12 <= off + size:
                nsz, dsz, typ = struct.unpack_from('<III', d, p)
                name = d[p + 12:p + 12 + nsz]
                desc_at = p + 12 + ((nsz + 3) & ~3)
                if typ == 3 and name[:3] == b'GNU':
                    return d[desc_at:desc_at + dsz].hex()
                p = desc_at + ((dsz + 3) & ~3)
        return None

    def file_off(self, va, n):
        for v, o, fs, fl in self.segs:
            if v <= va and va + n <= v + fs:
                return o + va - v, fl
        return None, 0

    def funcs(self):
        d = self.d
        for s in self.secs:
            if s[1] != 2:                       # SHT_SYMTAB
                continue
            stroff = self.secs[s[6]][4]
            ent = s[9]
            for i in range(s[5] // ent):
                o = s[4] + i * ent
                if self.is64:
                    st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', d, o)
                else:
                    st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from('<IIIBBH', d, o)
                if st_info & 15 != 2 or not st_value or st_shndx == 0:
                    continue
                e = d.index(b'\0', stroff + st_name)
                yield d[stroff + st_name:e].decode('utf-8', 'replace'), st_value, st_size


def fnv1a(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xffffffffffffffff
    return h


def arm_movable(w):
    """an ARM-state instruction that computes the same wherever it runs: no branch, no pc operand"""
    cond = w >> 28
    if cond == 0xf:
        return False                            # unconditional space (blx imm, pld, ...)
    if (w & 0x0e000000) == 0x0a000000:
        return False                            # b / bl
    if (w & 0x0ffffff0) == 0x012fff10 or (w & 0x0ffffff0) == 0x012fff30:
        return False                            # bx / blx reg
    if (w & 0x0e000000) == 0x08000000:          # ldm / stm: pc in the list or as base
        return not (w & 0x8000) and ((w >> 16) & 15) != 15
    regs = [(w >> 16) & 15, (w >> 12) & 15, w & 15]
    if (w & 0x0c000000) == 0x04000000:          # ldr / str immediate or register
        regs = [(w >> 16) & 15, (w >> 12) & 15] + ([w & 15] if w & 0x02000000 else [])
    return 15 not in regs


def a64_movable(w):
    """an A64 instruction that computes the same wherever it runs"""
    if (w & 0x1f000000) == 0x10000000:
        return False                            # adr / adrp
    if (w & 0x3b000000) == 0x18000000:
        return False                            # ldr (literal), ldrsw, prfm literal
    if (w & 0x7c000000) == 0x14000000:
        return False                            # b / bl
    if (w & 0x7e000000) == 0x34000000 or (w & 0x7e000000) == 0x36000000:
        return False                            # cbz / cbnz / tbz / tbnz
    if (w & 0xff000010) == 0x54000000:
        return False                            # b.cond
    if (w & 0xfe000000) == 0xd6000000:
        return False                            # br / blr / ret
    return True


def branches_into(code, start, isa, lo, hi):
    """offsets in the function whose direct branch target lies in [lo, hi)"""
    hits = []
    for i in range(0, len(code) - 3, 4):
        w, = struct.unpack_from('<I', code, i)
        pc = start + i
        t = None
        if isa == 'a':
            if (w & 0x0e000000) == 0x0a000000 and w >> 28 != 0xf:
                imm = w & 0xffffff
                imm = imm - (1 << 24) if imm & 0x800000 else imm
                t = pc + 8 + imm * 4
        else:
            if (w & 0x7c000000) == 0x14000000:
                imm = w & 0x3ffffff
                t = pc + 4 * (imm - (1 << 26) if imm & 0x2000000 else imm)
            elif (w & 0xff000010) == 0x54000000 or (w & 0x7e000000) == 0x34000000:
                imm = (w >> 5) & 0x7ffff
                t = pc + 4 * (imm - (1 << 19) if imm & 0x40000 else imm)
            elif (w & 0x7e000000) == 0x36000000:
                imm = (w >> 5) & 0x3fff
                t = pc + 4 * (imm - (1 << 14) if imm & 0x2000 else imm)
        if t is not None and lo <= t < hi:
            hits.append(pc)
    return hits


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('lib')
    ap.add_argument('sym')
    ap.add_argument('--unity', default='?', help='the player, for the row (e.g. "6000.0.61f1 armeabi-v7a")')
    a = ap.parse_args(argv)
    lib, sym = Elf(a.lib), Elf(a.sym)
    bl, bs = lib.build_id(), sym.build_id()
    if bl and bs and bl != bs:
        raise SystemExit(f'the symbol file is not this library\'s: build id {bs} against {bl}')
    funcs = {}
    for name, val, size in sym.funcs():
        for pre, short in HOOKS:
            if name.startswith(pre):
                funcs.setdefault(short, []).append((val, size, name))
    ok = 0
    for pre, short in HOOKS:
        got = funcs.get(short, [])
        if len(got) != 1:
            print(f'// {short}: {len(got)} symbols ({", ".join(g[2] for g in got) or "none"}): no row', file=sys.stderr)
            continue
        val, size, name = got[0]
        if lib.is64:
            isa, addr = 'x', val
        else:
            isa, addr = ('t' if val & 1 else 'a'), val & ~1
        if isa == 't':
            print(f'// {short}: a Thumb function: no row', file=sys.stderr)
            continue
        off, fl = lib.file_off(addr, size)
        if off is None or not (fl & 1):
            print(f'// {short}: +{addr:#x} is not in the library\'s code: no row', file=sys.stderr)
            continue
        code = lib.d[off:off + size]
        moved = 8 if isa == 'a' else 16
        words = struct.unpack_from(f'<{moved // 4}I', code, 0)
        mov = arm_movable if isa == 'a' else a64_movable
        bad = [i for i, w in enumerate(words) if not mov(w)]
        if bad:
            print(f'// {short}: instruction {bad[0]} of the entry cannot be moved: no row', file=sys.stderr)
            continue
        into = branches_into(code, addr, isa, addr + 4, addr + moved)
        if into:
            print(f'// {short}: branches into the entry from {", ".join(hex(x) for x in into)}: no row', file=sys.stderr)
            continue
        print(f'    {{ "{a.unity}", "{short}", {addr:#x}, {size:#x}, ISA_{"ARM" if isa == "a" else "A64"}, '
              f'{fnv1a(code):#018x}ull }},')
        ok += 1
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
