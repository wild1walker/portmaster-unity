#!/usr/bin/env python3
"""ELF helper for Android shared objects of both ABIs (armeabi-v7a, arm64-v8a, x86, x86_64):
every relocation table the Android linker reads (REL/RELA, PLT, packed "APS2" DT_ANDROID_REL(A),
RELR and DT_ANDROID_RELR), pointer reads with RELATIVE relocations applied, notes and build facts.

  from elfimage import Image
  im = Image('libunity.so')
  im.relocs()          -> list of (offset, type, symindex, addend, table)
  im.ptr(addr)         -> pointer-sized value at addr, relocated (RELATIVE: base 0)
  im.cstr(addr)        -> C string at addr (any loaded segment) or None
  im.facts()           -> dict: machine, class, relocation formats, note.android.ident, ...

usage: python3 -I elfimage.py LIB.so...   (prints facts() as JSON)
"""
import sys, struct, json, collections
from elftools.elf.elffile import ELFFile

DT = {'DT_NULL': 0, 'DT_NEEDED': 1, 'DT_PLTRELSZ': 2, 'DT_HASH': 4, 'DT_STRTAB': 5, 'DT_SYMTAB': 6,
      'DT_RELA': 7, 'DT_RELASZ': 8, 'DT_REL': 17, 'DT_RELSZ': 18, 'DT_PLTREL': 20, 'DT_TEXTREL': 22,
      'DT_JMPREL': 23, 'DT_FLAGS': 30, 'DT_RELRSZ': 35, 'DT_RELR': 36,
      'DT_ANDROID_REL': 0x6000000f, 'DT_ANDROID_RELSZ': 0x60000010, 'DT_ANDROID_RELA': 0x60000011,
      'DT_ANDROID_RELASZ': 0x60000012, 'DT_ANDROID_RELR': 0x6fffe000, 'DT_ANDROID_RELRSZ': 0x6fffe001,
      'DT_GNU_HASH': 0x6ffffef5, 'DT_VERSYM': 0x6ffffff0, 'DT_FLAGS_1': 0x6ffffffb,
      'DT_VERNEED': 0x6ffffffe, 'DT_VERDEF': 0x6ffffffc}
DTN = {v: k for k, v in DT.items()}

RELOC_NAMES = {
    'EM_ARM': {0: 'NONE', 2: 'ABS32', 3: 'REL32', 17: 'TLS_DTPMOD32', 18: 'TLS_DTPOFF32',
               19: 'TLS_TPOFF32', 20: 'COPY', 21: 'GLOB_DAT', 22: 'JUMP_SLOT', 23: 'RELATIVE',
               160: 'IRELATIVE'},
    'EM_AARCH64': {0: 'NONE', 257: 'ABS64', 1024: 'COPY', 1025: 'GLOB_DAT', 1026: 'JUMP_SLOT',
                   1027: 'RELATIVE', 1028: 'TLS_DTPMOD64', 1029: 'TLS_DTPREL64', 1030: 'TLS_TPREL64',
                   1031: 'TLSDESC', 1032: 'IRELATIVE'},
    'EM_386': {1: '32', 2: 'PC32', 6: 'GLOB_DAT', 7: 'JMP_SLOT', 8: 'RELATIVE', 14: 'TLS_TPOFF',
               35: 'TLS_DTPMOD32', 36: 'TLS_DTPOFF32', 42: 'IRELATIVE'},
    'EM_X86_64': {1: '64', 6: 'GLOB_DAT', 7: 'JUMP_SLOT', 8: 'RELATIVE', 16: 'DTPMOD64',
                  17: 'DTPOFF64', 18: 'TPOFF64', 37: 'IRELATIVE'},
}
RELATIVE = {'EM_ARM': 23, 'EM_AARCH64': 1027, 'EM_386': 8, 'EM_X86_64': 8}


def sleb(data, p):
    r = s = 0
    while True:
        b = data[p]; p += 1
        r |= (b & 0x7f) << s; s += 7
        if not b & 0x80:
            if b & 0x40:
                r -= 1 << s
            return r, p


class Image:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        self.e = ELFFile(self.f)
        self.mach = self.e['e_machine']
        self.is64 = self.e.elfclass == 64
        self.ps = 8 if self.is64 else 4
        self.pf = '<Q' if self.is64 else '<I'
        self.segs = [s for s in self.e.iter_segments() if s['p_type'] == 'PT_LOAD']
        self.tags = collections.OrderedDict()
        dyn = self.e.get_section_by_name('.dynamic')
        self.needed = []
        if dyn is None:
            for s in self.e.iter_segments():
                if s['p_type'] == 'PT_DYNAMIC':
                    dyn = s
        for t in dyn.iter_tags():
            d = t.entry.d_tag
            d = DT.get(d, d) if isinstance(d, str) else d
            if d == 1:
                self.needed.append(t.needed)
            else:
                self.tags.setdefault(d, t.entry.d_val)
        self._rel = None
        self._relmap = None

    # --- memory
    def read(self, addr, n):
        for s in self.segs:
            if s['p_vaddr'] <= addr and addr + n <= s['p_vaddr'] + s['p_filesz']:
                self.f.seek(s['p_offset'] + addr - s['p_vaddr'])
                return self.f.read(n)
        return None

    def cstr(self, addr, maxlen=512):
        b = self.read(addr, 1)
        if b is None:
            return None
        out = bytearray()
        while len(out) < maxlen:
            c = self.read(addr + len(out), 64)
            if c is None:
                c = self.read(addr + len(out), 1)
                if c is None:
                    return None
            i = c.find(b'\0')
            if i >= 0:
                out += c[:i]
                try:
                    return out.decode('ascii')
                except UnicodeDecodeError:
                    return None
            out += c
        return None

    # --- relocations
    def _table(self, addr_tag, size_tag, rela, name, out):
        if addr_tag not in self.tags or not self.tags.get(size_tag):
            return
        data = self.read(self.tags[addr_tag], self.tags[size_tag])
        if self.is64:
            ent = 24 if rela else 16
            for i in range(0, len(data) - ent + 1, ent):
                off, info = struct.unpack_from('<QQ', data, i)
                add = struct.unpack_from('<q', data, i + 16)[0] if rela else None
                out.append((off, info & 0xffffffff, info >> 32, add, name))
        else:
            ent = 12 if rela else 8
            for i in range(0, len(data) - ent + 1, ent):
                off, info = struct.unpack_from('<II', data, i)
                add = struct.unpack_from('<i', data, i + 8)[0] if rela else None
                out.append((off, info & 0xff, info >> 8, add, name))

    def _aps2(self, addr_tag, size_tag, rela, name, out):
        if addr_tag not in self.tags:
            return
        data = self.read(self.tags[addr_tag], self.tags[size_tag])
        if data[:4] != b'APS2':
            raise ValueError('%s: packed relocations without APS2' % self.path)
        p = 4
        count, p = sleb(data, p)
        off, p = sleb(data, p)
        info = add = 0
        n = 0
        while n < count:
            gsize, p = sleb(data, p)
            flags, p = sleb(data, p)
            step = 0
            if flags & 2:
                step, p = sleb(data, p)
            if flags & 1:
                info, p = sleb(data, p)
            if (flags & 12) == 12:
                d, p = sleb(data, p); add += d
            elif not flags & 8:
                add = 0
            for _ in range(gsize):
                if flags & 2:
                    off += step
                else:
                    d, p = sleb(data, p); off += d
                if not flags & 1:
                    info, p = sleb(data, p)
                if (flags & 12) == 8:
                    d, p = sleb(data, p); add += d
                if self.is64:
                    out.append((off & (2 ** 64 - 1), info & 0xffffffff, info >> 32, add if rela else None, name))
                else:
                    out.append((off & 0xffffffff, info & 0xff, info >> 8, add if rela else None, name))
                n += 1

    def _relr(self, addr_tag, size_tag, name, out):
        if addr_tag not in self.tags:
            return
        data = self.read(self.tags[addr_tag], self.tags[size_tag])
        ps, bits = self.ps, self.ps * 8 - 1
        words = struct.unpack('<%d%s' % (len(data) // ps, 'Q' if self.is64 else 'I'), data)
        where = 0
        for w in words:
            if not w & 1:
                where = w
                out.append((where, RELATIVE[self.mach], 0, None, name))
                where += ps
            else:
                b = w >> 1
                for k in range(bits):
                    if b & (1 << k):
                        out.append((where + k * ps, RELATIVE[self.mach], 0, None, name))
                where += bits * ps

    def relocs(self):
        if self._rel is None:
            out = []
            self._aps2(DT['DT_ANDROID_REL'], DT['DT_ANDROID_RELSZ'], False, 'android_rel', out)
            self._aps2(DT['DT_ANDROID_RELA'], DT['DT_ANDROID_RELASZ'], True, 'android_rela', out)
            self._relr(DT['DT_RELR'], DT['DT_RELRSZ'], 'relr', out)
            self._relr(DT['DT_ANDROID_RELR'], DT['DT_ANDROID_RELRSZ'], 'android_relr', out)
            self._table(DT['DT_REL'], DT['DT_RELSZ'], False, 'rel', out)
            self._table(DT['DT_RELA'], DT['DT_RELASZ'], True, 'rela', out)
            rela_plt = self.tags.get(DT['DT_PLTREL']) == DT['DT_RELA']
            self._table(DT['DT_JMPREL'], DT['DT_PLTRELSZ'], rela_plt, 'plt', out)
            self._rel = out
        return self._rel

    def ptr(self, addr):
        """Pointer at addr after relocation (load base 0); symbol relocations give None."""
        if self._relmap is None:
            self._relmap = {}
            rel = RELATIVE[self.mach]
            for off, t, s, a, _ in self.relocs():
                self._relmap[off] = (t, s, a)
        b = self.read(addr, self.ps)
        if b is None:
            return None
        v = struct.unpack(self.pf, b)[0]
        r = self._relmap.get(addr)
        if r is None:
            return v
        t, s, a = r
        if t == RELATIVE[self.mach]:
            return a if a is not None else v
        return None

    def section_words(self, name):
        """(base address, list of relocated pointer-sized words) of a section."""
        s = self.e.get_section_by_name(name)
        if s is None or s['sh_type'] == 'SHT_NOBITS':
            return None, []
        base = s['sh_addr']
        return base, [self.ptr(base + i) for i in range(0, s['sh_size'] - self.ps + 1, self.ps)]

    def reloc_name(self, t):
        return RELOC_NAMES.get(self.mach, {}).get(t, str(t))

    # --- facts
    def facts(self):
        e = self.e
        r = {'machine': self.mach, 'class': e.elfclass, 'needed': self.needed}
        r['dyn_tags'] = sorted(DTN.get(k, hex(k) if isinstance(k, int) else str(k)) for k in self.tags
                               if (isinstance(k, int) and k in DTN) or not isinstance(k, int))
        c = collections.Counter((tb, self.reloc_name(t)) for _, t, _, _, tb in self.relocs())
        r['relocs'] = {'%s %s' % k: v for k, v in sorted(c.items())}
        r['reloc_formats'] = sorted(set(tb for _, _, _, _, tb in self.relocs()))
        phd = []
        for s in e.iter_segments():
            phd.append(s['p_type'])
        r['phdrs'] = phd
        r['load_align'] = sorted(set(hex(s['p_align']) for s in self.segs))
        r['flags'] = {DTN[k]: hex(self.tags[k]) for k in (DT['DT_FLAGS'], DT['DT_FLAGS_1'], DT['DT_TEXTREL'])
                      if k in self.tags}
        r['hash'] = [n for n, k in (('gnu', DT['DT_GNU_HASH']), ('sysv', DT['DT_HASH'])) if k in self.tags]
        tls = None
        for s in e.iter_segments():
            if s['p_type'] == 'PT_TLS':
                tls = {'memsz': s['p_memsz'], 'align': s['p_align']}
        r['pt_tls'] = tls
        ds = e.get_section_by_name('.dynsym')
        ifunc, tlssyms, emutls = [], [], []
        for sym in ds.iter_symbols():
            t = sym['st_info']['type']
            if t in ('STT_LOOS', 'STT_GNU_IFUNC') and sym['st_shndx'] != 'SHN_UNDEF':
                ifunc.append(sym.name)
            if t == 'STT_TLS':
                tlssyms.append(sym.name)
            if sym.name.startswith('__emutls'):
                emutls.append(sym.name + ('(undef)' if sym['st_shndx'] == 'SHN_UNDEF' else ''))
        r['ifunc_exports'] = ifunc
        r['tls_symbols'] = tlssyms
        r['emutls_symbols'] = emutls[:20]
        # .note.android.ident: API level + NDK version (NDK r14+)
        for s in e.iter_sections():
            if s['sh_type'] != 'SHT_NOTE':
                continue
            d, p = s.data(), 0
            while p + 12 <= len(d):
                nsz, dsz, typ = struct.unpack_from('<III', d, p)
                name = d[p + 12:p + 12 + nsz].rstrip(b'\0')
                q = p + 12 + ((nsz + 3) & ~3)
                desc = d[q:q + dsz]
                p = q + ((dsz + 3) & ~3)
                if name == b'Android' and typ == 1 and len(desc) >= 4:
                    r['android_ident'] = {'api': struct.unpack_from('<I', desc)[0],
                                          'ndk': desc[4:68].split(b'\0')[0].decode('ascii', 'replace'),
                                          'ndk_build': desc[68:132].split(b'\0')[0].decode('ascii', 'replace')}
                elif name == b'GNU' and typ == 5:
                    r['gnu_property'] = desc.hex()
                elif name == b'GNU' and typ == 3:
                    r['build_id'] = True
        cm = e.get_section_by_name('.comment')
        if cm is not None:
            r['comment'] = sorted(set(x.decode('ascii', 'replace') for x in cm.data().split(b'\0') if x))[:6]
        r['sections'] = len(list(e.iter_sections()))
        return r


def main():
    for p in sys.argv[1:]:
        print(json.dumps({p: Image(p).facts()}, indent=1, default=str))


if __name__ == '__main__':
    main()
