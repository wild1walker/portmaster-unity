#!/usr/bin/env python3
"""List and extract files of an NSIS installer (LZMA or deflate), local or over HTTP.

Unity's TargetSupportInstaller .exe files are non-solid NSIS 3 (Unicode) installers: every file is
its own compressed block, at an offset the script's header gives, so a file is fetched with one
HTTP range request and the installer itself is never downloaded whole or stored. From 2020.3 the
installers are built with a large-installer NSIS variant: the first header is 36 bytes, script
entries have 8 parameters instead of 6 and file offsets are 64-bit (both layouts handled).
Solid installers (one LZMA stream for everything) are refused: use 7-Zip.

usage (tools/re/nsisrange.py, or python -m pmunity.nsis):
  nsisrange.py list    SRC                     # path<TAB>block offset<TAB>compressed size
  nsisrange.py extract SRC OUTDIR GLOB...      # extract matching paths (fnmatch, '/' separators)
  SRC: a local file or an http(s) URL (the server must honour Range requests)

Paths come from the script's SetOutPath / File commands; variables are written as $NAME
($INSTDIR, $_59_ for user variables, ...). Only what Unity's installers use is decoded.
"""
import sys, os, struct, lzma, zlib, fnmatch, urllib.request, time

FH_SIG = b'\xef\xbe\xad\xdeNullsoftInst'
VARS = ['$0', '$1', '$2', '$3', '$4', '$5', '$6', '$7', '$8', '$9',
        '$R0', '$R1', '$R2', '$R3', '$R4', '$R5', '$R6', '$R7', '$R8', '$R9',
        '$CMDLINE', '$INSTDIR', '$OUTDIR', '$EXEDIR', '$LANGUAGE', '$TEMP', '$PLUGINSDIR',
        '$EXEPATH', '$EXEFILE', '$HWNDPARENT', '$_CLICK', '$_OUTDIR']
EW_CREATEDIR, EW_EXTRACTFILE = 11, 20


class Source:
    def __init__(self, src):
        self.src = src
        self.url = src.startswith(('http://', 'https://'))
        self.fetched = 0
        if not self.url:
            self.f = open(src, 'rb')

    def read(self, off, n):
        if n <= 0:
            return b''               # an empty file's block (no valid Range for it)
        if not self.url:
            self.f.seek(off)
            return self.f.read(n)
        for attempt in range(6):
            try:
                req = urllib.request.Request(self.src, headers={'Range': 'bytes=%d-%d' % (off, off + n - 1)})
                with urllib.request.urlopen(req, timeout=120) as r:
                    if r.status != 206:
                        raise IOError('server ignored the Range request (status %d)' % r.status)
                    data = r.read()
                if len(data) != n:
                    raise IOError('short read %d of %d' % (len(data), n))
                self.fetched += n
                return data
            except (OSError, IOError) as ex:  # retry transient network errors
                if attempt == 5 or isinstance(ex, ValueError):
                    raise
                print('retry %d at %d: %s' % (attempt + 1, off, ex), file=sys.stderr)
                time.sleep(3 * (attempt + 1))


def lzma_decoder(blk):
    """(decompressor, bytes of block header used) for an NSIS LZMA block or stream start."""
    bcj = False
    if blk[0] in (0, 1):
        bcj, blk = blk[0] == 1, blk[1:]
    props, dsz = blk[0], struct.unpack_from('<I', blk, 1)[0]
    lc, r = props % 9, props // 9
    filt = [{'id': lzma.FILTER_LZMA1, 'dict_size': dsz, 'lc': lc, 'lp': r % 5, 'pb': r // 5}]
    if bcj:
        filt.insert(0, {'id': lzma.FILTER_X86})
    return lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filt), 5 + (1 if bcj else 0)


def decompress(blk):
    """One NSIS block body -> bytes (LZMA raw with props, optionally a BCJ flag byte; or deflate)."""
    if blk[:1] == b'\x5d' or (blk[:1] in (b'\0', b'\1') and blk[1:2] == b'\x5d'):
        dec, used = lzma_decoder(blk)
        return dec.decompress(blk[used:])
    return zlib.decompressobj(-15).decompress(blk)   # NSIS zlib = raw deflate (bzip2 unsupported)


class Nsis:
    def __init__(self, src):
        self.s = Source(src)
        head = self.s.read(0, 1 << 20)
        pos = 0
        while True:
            i = head.find(FH_SIG, pos)
            if i < 0 and len(head) < 8 << 20:
                head += self.s.read(len(head), len(head))
                continue
            if i < 0:
                raise SystemExit('no NSIS first header in the first 8 MiB')
            if (i - 4) % 512 == 0:
                break
            pos = i + 1
        self.fh = i - 4
        flags, _, _, _, _, self.hdr_len, self.total = struct.unpack_from('<7I', head, self.fh)
        fhsize = 28
        if head[self.fh + 28:self.fh + 36] == bytes(8) and head[self.fh + 39] & 0x80:
            fhsize = 36                      # large-installer variant: 64-bit data length
        w = struct.unpack_from('<I', head, self.fh + fhsize)[0]
        if not w & 0x80000000:
            raise SystemExit('solid installer: use 7-Zip (this tool needs per-file blocks)')
        n = w & 0x7fffffff
        p = self.fh + fhsize + 4
        blk = head[p:p + n] if p + n <= len(head) else self.s.read(p, n)
        self.h = decompress(blk)[:self.hdr_len]
        self.data_base = p + n
        self.total += fhsize - 28            # end of data relative to fh + 28 below
        self.blocks = [struct.unpack_from('<II', self.h, 4 + 8 * k) for k in range(8)]
        so = self.blocks[3][0]
        eo, en = self.blocks[2]
        self.entsize = (so - eo) // en if en else 28
        if self.entsize not in (28, 36):
            raise SystemExit('unknown script entry size %d' % self.entsize)
        self.unicode = self.h[so:so + 2] == b'\0\0' and self.h[so + 3:so + 4] == b'\0'
        # Unicode string codes: NSIS 3 uses 1 LANG, 2 SHELL, 3 VAR, 4 SKIP; the older Unicode
        # NSIS 2 fork 0xE000 SKIP, 0xE001 VAR, 0xE002 SHELL, 0xE003 LANG (Unity 2017.4 - 2019.4)
        self.codes = {}
        if self.unicode:
            words = struct.unpack_from('<%dH' % ((self.blocks[4][0] - so) // 2), self.h, so)
            new = sum(1 for w in words if 1 <= w <= 4)
            old = sum(1 for w in words if 0xE000 <= w <= 0xE003)
            self.codes = ({1: 'lang', 2: 'shell', 3: 'var', 4: 'skip'} if new > old else
                          {0xE000: 'skip', 0xE001: 'var', 0xE002: 'shell', 0xE003: 'lang'})

    def string(self, off):
        h, so = self.h, self.blocks[3][0]
        out = []
        if self.unicode:
            p = so + 2 * off
            while True:
                c = struct.unpack_from('<H', h, p)[0]
                p += 2
                if c == 0:
                    break
                code = self.codes.get(c)
                if code:
                    v = struct.unpack_from('<H', h, p)[0]
                    p += 2
                    if code == 'skip':   # literal next char
                        out.append(chr(v))
                        continue
                    idx = ((v >> 8) & 0x7f) << 7 | (v & 0x7f)
                    if code == 'var':
                        out.append(VARS[idx] if idx < len(VARS) else '$_%d_' % idx)
                    elif code == 'shell':
                        out.append('$SHELL%d' % (v & 0xff))
                    else:
                        out.append('$LANG%d' % idx)
                    continue
                out.append(chr(c))
        else:
            p = so + off
            while h[p]:
                c = h[p]
                p += 1
                if c in (252, 253, 254, 255):
                    v = h[p] | h[p + 1] << 8
                    idx = (h[p] & 0x7f) | (h[p + 1] & 0x7f) << 7
                    p += 2
                    if c == 252:
                        out.append(chr(h[p - 2]))
                        p -= 1
                    elif c == 253:
                        out.append(VARS[idx] if idx < len(VARS) else '$_%d_' % idx)
                    elif c == 254:
                        out.append('$SHELL%d' % (v & 0xff))
                    else:
                        out.append('$LANG%d' % idx)
                    continue
                out.append(chr(c))
        return ''.join(out)

    def files(self):
        eo, en = self.blocks[2]
        outdir = '$OUTDIR'
        seen = {}
        wide = self.entsize == 36
        for k in range(en):
            op, *pa = struct.unpack_from('<%dI' % (self.entsize // 4), self.h, eo + self.entsize * k)
            if op == EW_CREATEDIR and pa[1]:
                outdir = self.string(pa[0])
            elif op == EW_EXTRACTFILE:
                if wide:
                    pa[2] |= pa[3] << 32
                name = self.string(pa[1])
                path = name if ('\\' in name and name.startswith('$')) else outdir + '\\' + name
                path = path.replace('\\', '/')
                seen.setdefault(path, pa[2])
        offs = sorted(set(seen.values()))
        nxt = {o: (offs[j + 1] if j + 1 < len(offs) else None) for j, o in enumerate(offs)}
        end = self.fh + 28 + self.total - self.data_base
        for path, o in seen.items():
            e = nxt[o] if nxt[o] is not None else end
            yield path, o, e - o - 4   # compressed size (approximate for the last block: CRC tail)

    def extract(self, off):
        w = struct.unpack('<I', self.s.read(self.data_base + off, 4))[0]
        n = w & 0x7fffffff
        blk = self.s.read(self.data_base + off + 4, n)
        return decompress(blk) if w & 0x80000000 else blk


def main():
    cmd, src = sys.argv[1], sys.argv[2]
    ns = Nsis(src)
    if cmd == 'list':
        for path, o, n in sorted(ns.files()):
            print('%s\t%d\t%d' % (path, o, n))
    elif cmd == 'extract':
        outdir, globs = sys.argv[3], sys.argv[4:]
        rel = lambda p: p.lstrip('$').replace('$', '_')
        for path, o, n in sorted(ns.files()):
            if not any(fnmatch.fnmatchcase(path, g) for g in globs):
                continue
            data = ns.extract(o)
            dst = os.path.join(outdir, rel(path))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as f:
                f.write(data)
            print('%10d %s' % (len(data), path))
        print('fetched %d bytes' % ns.s.fetched, file=sys.stderr)
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
