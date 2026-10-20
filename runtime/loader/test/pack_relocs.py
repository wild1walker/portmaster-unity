#!/usr/bin/env python3
"""Rewrites the dynamic relocations of a shared library the way newer Android toolchains emit
them, for the loader's tests (GNU ld 2.34 writes neither; lld does with
--pack-dyn-relocs=android+relr). ELF32 ARM (REL) or ELF64 AArch64 (RELA), little-endian.

  pack_relocs.py [--relr] [--aps2] [--android-relr] [--truncate] in.so out.so

  --relr          relative relocations -> DT_RELR (their addends written in place)
  --aps2          the rest of DT_REL / DT_RELA -> Android's packed "APS2" table
                  (DT_ANDROID_REL / DT_ANDROID_RELA)
  --android-relr  RELR under Android's pre-standard tags (DT_ANDROID_RELR...)
  --truncate      cut the packed table short: a broken library

The new tables go where the old one was (they are smaller); the dynamic section's spare
DT_NULL slots take the new tags. The APS2 groups use every flag combination bionic decodes:
by offset step + info, by info only, ungrouped; addends per relocation, per group, or none
(reset to 0).
"""
import struct
import sys

DT = dict(RELA=7, RELASZ=8, RELAENT=9, REL=17, RELSZ=18, RELENT=19, RELRSZ=35, RELR=36, RELRENT=37,
          RELACOUNT=0x6ffffff9, RELCOUNT=0x6ffffffa,
          ANDROID_REL=0x6000000f, ANDROID_RELSZ=0x60000010, ANDROID_RELA=0x60000011, ANDROID_RELASZ=0x60000012,
          ANDROID_RELR=0x6fffe000, ANDROID_RELRSZ=0x6fffe001, ANDROID_RELRENT=0x6fffe003)
BY_INFO, BY_OFFSET_DELTA, BY_ADDEND, HAS_ADDEND = 1, 2, 4, 8


def sleb(v, w):
    """signed LEB128 of v taken as a w-byte word (bionic's decoder wraps at the word size)"""
    bits = 8 * w
    v &= (1 << bits) - 1
    if v >= 1 << (bits - 1):
        v -= 1 << bits
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        if (v == 0 and not b & 0x40) or (v == -1 and b & 0x40):
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def aps2(relocs, rela, w):
    n = len(relocs)
    deltas, prev = [], 0
    for r in relocs:
        deltas.append(r[0] - prev)
        prev = r[0]

    def run(i, same_step):
        j = i + 1
        while j < n and relocs[j][1] == relocs[i][1] and (not same_step or deltas[j] == deltas[i]):
            j += 1
        return j - i

    groups, i = [], 0
    while i < n:
        a = run(i, True)
        if a >= 3:
            groups.append([BY_INFO | BY_OFFSET_DELTA, i, a])
            i += a
            continue
        b = run(i, False)
        if b >= 2:
            groups.append([BY_INFO, i, b])
            i += b
            continue
        if groups and groups[-1][0] == 0 and groups[-1][1] + groups[-1][2] == i:
            groups[-1][2] += 1
        else:
            groups.append([0, i, 1])
        i += 1

    out = bytearray(b'APS2') + sleb(n, w) + sleb(0, w)
    addend = 0
    for flags, start, size in groups:
        g = relocs[start:start + size]
        adds = [r[2] for r in g]
        if rela and any(adds):
            flags |= HAS_ADDEND | (BY_ADDEND if len(set(adds)) == 1 else 0)
        out += sleb(size, w) + sleb(flags, w)
        if flags & BY_OFFSET_DELTA:
            out += sleb(deltas[start], w)
        if flags & BY_INFO:
            out += sleb(g[0][1], w)
        if flags & BY_ADDEND:
            out += sleb(adds[0] - addend, w)
            addend = adds[0]
        elif not flags & HAS_ADDEND:
            addend = 0
        for k, r in enumerate(g):
            if not flags & BY_OFFSET_DELTA:
                out += sleb(deltas[start + k], w)
            if not flags & BY_INFO:
                out += sleb(r[1], w)
            if flags & HAS_ADDEND and not flags & BY_ADDEND:
                out += sleb(r[2] - addend, w)
                addend = r[2]
    return bytes(out)


def relr(offsets, w):
    """addresses (even words) and bitmaps of the following 31 / 63 words (odd words)"""
    nbits = 8 * w - 1
    out, i = [], 0
    while i < len(offsets):
        out.append(offsets[i])
        where = offsets[i] + w
        i += 1
        while True:
            bitmap = 0
            while i < len(offsets):
                d = offsets[i] - where
                if d < 0 or d >= nbits * w or d % w:
                    break
                bitmap |= 1 << (d // w)
                i += 1
            if not bitmap:
                break
            out.append(bitmap << 1 | 1)
            where += nbits * w
    return out


def main():
    opts = {a for a in sys.argv[1:] if a.startswith('--')}
    files = [a for a in sys.argv[1:] if not a.startswith('--')]
    data = bytearray(open(files[0], 'rb').read())
    is64 = data[4] == 2
    w = 8 if is64 else 4
    word = '<Q' if is64 else '<I'
    machine = struct.unpack_from('<H', data, 0x12)[0]
    relative = 1027 if machine == 183 else 23            # R_AARCH64_RELATIVE, R_ARM_RELATIVE
    if is64:
        phoff, = struct.unpack_from('<Q', data, 0x20)
        phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    else:
        phoff, = struct.unpack_from('<I', data, 0x1c)
        phentsize, phnum = struct.unpack_from('<HH', data, 0x2a)
    phs = []
    for i in range(phnum):
        o = phoff + i * phentsize
        if is64:
            t, _, off, va, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', data, o)
        else:
            t, off, va, _, filesz, memsz, _, _ = struct.unpack_from('<IIIIIIII', data, o)
        phs.append((t, off, va, filesz, memsz))

    def file_offset(va, size):
        for t, off, pva, filesz, _ in phs:
            if t == 1 and pva <= va and va + size <= pva + filesz:
                return off + va - pva
        return None

    _, dynoff, _, dynsz, _ = next(p for p in phs if p[0] == 2)
    dfmt = '<qQ' if is64 else '<iI'
    capacity = dynsz // (2 * w)
    dyn = []
    for k in range(capacity):
        tag, val = struct.unpack_from(dfmt, data, dynoff + k * 2 * w)
        if tag == 0:
            break
        dyn.append((tag, val))
    d = dict(dyn)
    rela = DT['RELA'] in d
    tab_va, tab_sz = (d[DT['RELA']], d[DT['RELASZ']]) if rela else (d[DT['REL']], d[DT['RELSZ']])
    efmt = ('<QQq' if rela else '<QQ') if is64 else ('<IIi' if rela else '<II')
    ent = struct.calcsize(efmt)
    tab_off = file_offset(tab_va, tab_sz)
    relocs = []
    for k in range(tab_sz // ent):
        f = struct.unpack_from(efmt, data, tab_off + k * ent)
        relocs.append((f[0], f[1], f[2] if rela else 0))

    rest, relr_offsets = relocs, []
    if '--relr' in opts:
        rest = []
        for r in relocs:
            o = file_offset(r[0], w)
            if (r[1] & (0xffffffff if is64 else 0xff)) == relative and r[0] % w == 0 and o is not None:
                if rela:
                    struct.pack_into(word, data, o, r[2] & ((1 << 8 * w) - 1))
                relr_offsets.append(r[0])
            else:
                rest.append(r)
    blob = aps2(rest, rela, w) if '--aps2' in opts and rest else b''
    if '--truncate' in opts:
        blob = blob[:len(blob) // 2]
    plain = b'' if '--aps2' in opts else b''.join(struct.pack(efmt, *(r if rela else r[:2])) for r in rest)
    rr = relr(sorted(relr_offsets), w)
    out = plain + blob
    out += b'\0' * (-len(out) % w)
    relr_va = tab_va + len(out)
    out += b''.join(struct.pack(word, x) for x in rr)
    if len(out) > tab_sz:
        sys.exit('pack_relocs: new tables (%d bytes) do not fit in %d' % (len(out), tab_sz))
    data[tab_off:tab_off + tab_sz] = out + b'\0' * (tab_sz - len(out))

    table = (DT['RELA'], DT['RELASZ'], DT['RELAENT']) if rela else (DT['REL'], DT['RELSZ'], DT['RELENT'])
    new = []
    for tag, val in dyn:
        if tag in (DT['RELACOUNT'], DT['RELCOUNT']):
            continue
        if tag in table:
            if not plain:
                continue
            if tag == table[1]:
                val = len(plain)
        new.append((tag, val))
    if blob:
        new += [(DT['ANDROID_RELA' if rela else 'ANDROID_REL'], tab_va), (DT['ANDROID_RELASZ' if rela else 'ANDROID_RELSZ'], len(blob))]
    if rr:
        p = 'ANDROID_' if '--android-relr' in opts else ''
        new += [(DT[p + 'RELR'], relr_va), (DT[p + 'RELRSZ'], len(rr) * w), (DT[p + 'RELRENT'], w)]
    if len(new) >= capacity:
        sys.exit('pack_relocs: no spare slots in the dynamic section')
    for k in range(capacity):
        struct.pack_into(dfmt, data, dynoff + k * 2 * w, *(new[k] if k < len(new) else (0, 0)))
    open(files[1], 'wb').write(data)
    print('%s: %d relocations -> %d RELR words, %d APS2 bytes, %d plain' % (
        files[1].split('/')[-1], len(relocs), len(rr), len(blob), len(plain) // ent))


if __name__ == '__main__':
    main()
