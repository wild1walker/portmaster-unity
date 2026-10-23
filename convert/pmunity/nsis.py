#!/usr/bin/env python3
"""List and extract files of an NSIS installer (LZMA or deflate), local or over HTTP.

Unity's TargetSupportInstaller .exe files are NSIS (Unicode) installers. Up to 2023.2 they are
non-solid: every file is its own compressed block, at an offset the script's header gives, so a
file is fetched with one HTTP range request and the installer itself is never downloaded whole or
stored. From 2020.3 they are built with a large-installer NSIS variant: the first header is 36
bytes, script entries have 8 parameters instead of 6 and file offsets are 64-bit. Unity 6's
installers (an NSIS of 2025, first-header flag 0x40) cut LZMA data into chunks of 2 MiB
(uncompressed), each its own LZMA stream behind a 3-byte compressed length:
  - per-file blocks (e.g. 6000.0.58, 6000.1, 6000.2, 6000.3.11): 8-byte block words (top bit:
    compressed), the chunks of a block end with a zero length;
  - solid (e.g. 6000.0.85, 6000.3.26, 6000.4 - 6000.6): the first header holds the 64-bit
    uncompressed size, then the header comes in chunks of its own (one for a player installer,
    two for the Windows editor installer's 3.4 MB header) and all files follow as one stream of 2 MiB
    chunks (each file: 8-byte length + data, at its offset in that stream). A file is read by
    walking the chunk lengths (3 bytes per 2 MiB chunk, one small range request each on a
    kept-alive connection) to the chunks it spans.
Old-style solid installers (one LZMA stream for everything) are refused: use 7-Zip.

usage (tools/re/nsisrange.py, or python -m pmunity.nsis):
  nsisrange.py list    SRC                     # path<TAB>block offset<TAB>compressed size
  nsisrange.py extract SRC OUTDIR GLOB...      # extract matching paths (fnmatch, '/' separators)
  SRC: a local file or an http(s) URL (the server must honour Range requests)

Paths come from the script's SetOutPath / File commands; variables are written as $NAME
($INSTDIR, $_59_ for user variables, ...). Only what Unity's installers use is decoded.
"""
import sys, os, struct, lzma, zlib, fnmatch, urllib.request, urllib.parse, http.client, time

FH_SIG = b'\xef\xbe\xad\xdeNullsoftInst'
VARS = ['$0', '$1', '$2', '$3', '$4', '$5', '$6', '$7', '$8', '$9',
        '$R0', '$R1', '$R2', '$R3', '$R4', '$R5', '$R6', '$R7', '$R8', '$R9',
        '$CMDLINE', '$INSTDIR', '$OUTDIR', '$EXEDIR', '$LANGUAGE', '$TEMP', '$PLUGINSDIR',
        '$EXEPATH', '$EXEFILE', '$HWNDPARENT', '$_CLICK', '$_OUTDIR']
EW_CREATEDIR, EW_EXTRACTFILE = 11, 20


class Source:
    """Random access to a local file or an http(s) URL (Range requests on one kept-alive
    connection, reopened after errors; redirects followed)."""

    def __init__(self, src):
        self.src = src
        self.url = src.startswith(('http://', 'https://'))
        self.fetched = 0
        self.requests = 0
        self.conn = None
        if not self.url:
            self.f = open(src, 'rb')

    def _get(self, url, rng, depth=0):
        u = urllib.parse.urlsplit(url)
        # the environment's proxy (https_proxy / http_proxy, as urllib and curl use it): https through
        # a CONNECT tunnel, http with the whole URL as the request's target
        proxy = None if urllib.request.proxy_bypass(u.hostname or '') else urllib.request.getproxies().get(u.scheme)
        if self.conn is None or self.conn_host != (u.scheme, u.netloc):
            if self.conn is not None:
                self.conn.close()
            cls = http.client.HTTPSConnection if u.scheme == 'https' else http.client.HTTPConnection
            if proxy:
                p = urllib.parse.urlsplit(proxy if '://' in proxy else 'http://' + proxy)
                self.conn = cls(p.hostname, p.port or 8080, timeout=120)
                if u.scheme == 'https':
                    self.conn.set_tunnel(u.hostname, u.port or 443)
            else:
                self.conn = cls(u.netloc, timeout=120)
            self.conn_host = (u.scheme, u.netloc)
        path = u.path + ('?' + u.query if u.query else '')
        if proxy and u.scheme == 'http':
            path = url
        self.conn.request('GET', path, headers={'Range': rng, 'User-Agent': 'pmunity'})
        r = self.conn.getresponse()
        data = r.read()
        if r.status in (301, 302, 303, 307, 308) and depth < 5:
            return self._get(urllib.parse.urljoin(url, r.getheader('Location')), rng, depth + 1)
        if r.status != 206:
            raise IOError('server ignored the Range request (status %d)' % r.status)
        return data

    def close(self):
        if not self.url:
            self.f.close()
        elif self.conn is not None:
            self.conn.close()
            self.conn = None

    def read(self, off, n):
        if n <= 0:
            return b''               # an empty file's block (no valid Range for it)
        if not self.url:
            self.f.seek(off)
            return self.f.read(n)
        for attempt in range(6):
            try:
                data = self._get(self.src, 'bytes=%d-%d' % (off, off + n - 1))
                if len(data) != n:
                    raise IOError('short read %d of %d' % (len(data), n))
                self.fetched += n
                self.requests += 1
                return data
            except (OSError, IOError, http.client.HTTPException) as ex:  # retry transient network errors
                if self.conn is not None:
                    self.conn.close()
                    self.conn = None
                if attempt == 5:
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


CHUNK = 2 << 20          # Unity 6 installers: uncompressed size of every LZMA chunk but a stream's last


def dechunk(blk):
    """A Unity 6 block body: LZMA chunks, each a 3-byte compressed length and an LZMA stream, up to
    a zero length (or the end of the body)."""
    out, p = [], 0
    while p + 3 <= len(blk):
        n = int.from_bytes(blk[p:p + 3], 'little')
        if n == 0:
            break
        out.append(decompress(blk[p + 3:p + 3 + n]))
        p += 3 + n
    return b''.join(out)


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
        self.chunked = bool(flags & 0x40)    # Unity 6: LZMA in 2 MiB chunks (module doc)
        self.solid = False
        self.cache = {}
        get = lambda p, n: head[p:p + n] if p + n <= len(head) else self.s.read(p, n)
        if self.chunked:
            self.usize = struct.unpack_from('<Q', head, self.fh + 28)[0]
            if self.usize:                   # solid: header chunks, then the data stream's chunks
                self.solid = True
                p, h = self.fh + 36, b''
                while len(h) < 8 + self.hdr_len:  # (an editor installer's header takes more than one)
                    n = int.from_bytes(get(p, 3), 'little')
                    if n == 0:
                        break
                    blk = get(p, 3 + n + 3)
                    h += decompress(blk[3:3 + n])
                    p += 3 + n
                if len(h) < 8 or struct.unpack_from('<Q', h, 0)[0] != self.hdr_len or len(h) != 8 + self.hdr_len:
                    raise SystemExit('unknown Unity 6 installer layout (header chunk)')
                self.h = h[8:]
                self.chunk_pos = [p]          # compressed position of each data chunk found so far
                self.chunk_len = [int.from_bytes(blk[3 + n:], 'little')]
                self.end = self.fh + 28 + self.total
                fhsize = 36
            else:
                w = struct.unpack_from('<Q', head, self.fh + 36)[0]
                n = w & 0x7fffffffffffffff
                blk = get(self.fh + 44, n)
                self.h = (dechunk(blk) if w >> 63 else blk)[:self.hdr_len]
                self.data_base = self.fh + 44 + n
                fhsize = 36
        else:
            fhsize = 28
            if head[self.fh + 28:self.fh + 36] == bytes(8) and head[self.fh + 39] & 0x80:
                fhsize = 36                      # large-installer variant: 64-bit data length
            w = struct.unpack_from('<I', head, self.fh + fhsize)[0]
            if not w & 0x80000000:
                raise SystemExit('solid installer: use 7-Zip (this tool needs per-file blocks)')
            n = w & 0x7fffffff
            p = self.fh + fhsize + 4
            blk = get(p, n)
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

    def close(self):
        self.s.close()

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
        if self.solid:
            end = self.usize
        else:
            end = self.fh + 28 + self.total - self.data_base
        word = 8 if self.chunked else 4
        for path, o in seen.items():
            e = nxt[o] if nxt[o] is not None else end
            # compressed size (solid: uncompressed; approximate for the last block: CRC tail)
            yield path, o, e - o - word

    def _chunk(self, k):
        """Data chunk k of a solid Unity 6 installer, uncompressed (the last two kept)."""
        if k in self.cache:
            return self.cache[k]
        while len(self.chunk_pos) <= k:          # walk the chunk lengths up to chunk k
            p, n = self.chunk_pos[-1], self.chunk_len[-1]
            if n == 0 or p + 3 + n >= self.end:
                raise SystemExit('offset beyond the installer data')
            q = p + 3 + n
            self.chunk_pos.append(q)
            self.chunk_len.append(int.from_bytes(self.s.read(q, 3), 'little'))
        p, n = self.chunk_pos[k], self.chunk_len[k]
        blk = self.s.read(p + 3, n + 3 if p + 6 + n <= self.end else n)
        if len(self.chunk_pos) == k + 1 and len(blk) == n + 3:
            self.chunk_pos.append(p + 3 + n)
            self.chunk_len.append(int.from_bytes(blk[n:], 'little'))
        data = decompress(blk[:n])
        if len(self.cache) >= 2:
            self.cache.pop(min(self.cache))
        self.cache[k] = data
        return data

    def _stream(self, off, n):
        out = []
        while n > 0:
            k, r = divmod(off, CHUNK)
            c = self._chunk(k)[r:r + n]
            if not c:
                raise SystemExit('short chunk %d' % k)
            out.append(c)
            off += len(c)
            n -= len(c)
        return b''.join(out)

    def extract(self, off):
        if self.solid:
            n = struct.unpack('<Q', self._stream(off, 8))[0]
            return self._stream(off + 8, n)
        if self.chunked:
            w = struct.unpack('<Q', self.s.read(self.data_base + off, 8))[0]
            n = w & 0x7fffffffffffffff
            blk = self.s.read(self.data_base + off + 8, n)
            return dechunk(blk) if w >> 63 else blk
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
        print('fetched %d bytes in %d requests' % (ns.s.fetched, ns.s.requests), file=sys.stderr)
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
