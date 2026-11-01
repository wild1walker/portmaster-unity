"""Made-up inputs for the pkgzip golden tests (pkgzip.sh): installer packages in every form macpkg
reads, zips in every form `python -m zipfile -e` reads, and a built game for patch-obb. No game or
Unity data in them (the real inputs come from $PMU_TEST_DATA).

    python pkgzip_make.py pkgs <dir>
    python pkgzip_make.py gamepkg <dir> <install location> <bundle name> <path>...   (GOG-style game
                          installer whose payload holds these paths, for the DLC to merge into)
    python pkgzip_make.py zips <dir>
    python pkgzip_make.py game <dir> <apk name> <globalgamemanagers> <obb name> <bundle name>...
"""
import bz2
import gzip
import json
import lzma
import os
import struct
import sys
import zipfile
import zlib


def odc(entries):
    """odc cpio of (name, mode, data)."""
    out = b''
    for name, mode, data in entries + [('TRAILER!!!', 0, b'')]:
        n = name.encode() + b'\0'
        out += b'070707' + b'%06o%06o%06o%06o%06o%06o%06o%011o%06o%011o' % (
            0, 0, mode, 0, 0, 1, 0, 0, len(n), len(data)) + n + data
    return out


def newc(entries):
    """newc cpio of (name, mode, data), 4-byte aligned."""
    out = b''
    for name, mode, data in entries + [('TRAILER!!!', 0, b'')]:
        n = name.encode() + b'\0'
        out += b'070701' + b''.join(b'%08X' % v for v in (0, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(n), 0)) + n
        out += b'\0' * ((-len(out)) % 4) + data
        out += b'\0' * ((-len(out)) % 4)
    return out


def pbzx(data, piece=1 << 20, stored_every=3):
    """Apple's pbzx framing: xz chunks, every third one stored as is."""
    out = b'pbzx' + struct.pack('>Q', piece)
    for i in range(0, len(data), piece):
        c = data[i:i + piece]
        z = c if (i // piece) % stored_every == 2 else lzma.compress(c, format=lzma.FORMAT_XZ)
        out += struct.pack('>QQ', 0, len(z)) + z
    return out


def xar(members):
    """xar archive of (path, data, encoding) with encoding 'gzip' (zlib), 'bzip2', 'none' (stored,
    octet-stream), 'noenc' (no <encoding>) or 'nodata' (a file without <data>)."""
    heap, files = b'', {}
    for path, data, enc in members:
        z = {'gzip': zlib.compress, 'bzip2': bz2.compress}.get(enc, lambda d: d)(data)
        files[path] = (len(heap), len(z), len(data), enc)
        heap += z

    def tree(prefix):
        names = sorted({p[len(prefix):].split('/')[0] for p in files if p.startswith(prefix)})
        xml = ''
        for nm in names:
            p = prefix + nm
            if p in files:
                off, ln, size, enc = files[p]
                style = {'gzip': 'application/x-gzip', 'bzip2': 'application/x-bzip2',
                         'none': 'application/octet-stream'}.get(enc)
                e = f'<encoding style="{style}"/>' if style else ''
                d = '' if enc == 'nodata' else (f'<data><offset>{off}</offset><length>{ln}</length>'
                                               f'<size>{size}</size>{e}</data>')
                xml += f'<file><name>{nm}</name><name>{nm}</name><type>file</type>{d}</file>'
            else:
                xml += f'<file><name>{nm}</name><type>directory</type>{tree(p + "/")}</file>'
        return xml
    toc = (f'<?xml version="1.0" encoding="UTF-8"?><xar><toc><checksum style="sha1"><offset>0</offset>'
           f'<size>20</size></checksum>{tree("")}</toc></xar>').encode()
    tz = zlib.compress(toc)
    return b'xar!' + struct.pack('>HHQQI', 28, 1, len(tz), len(toc), 1) + tz + heap


def info(loc=None):
    return (b'<?xml version="1.0"?><pkg-info identifier="t" version="1"' +
            (b' install-location="' + loc.encode() + b'"' if loc is not None else b'') + b'/>')


def blob(n, seed):
    """n made-up bytes that compress somewhat."""
    out = bytearray()
    x = seed
    while len(out) < n:
        x = (x * 1103515245 + 12345) & 0x7fffffff
        out += bytes([(x >> 16) & 0xff]) * (1 + (x & 7))
    return bytes(out[:n])


F, D, L = 0o100644, 0o040755, 0o120777


def pkgs(d):
    os.makedirs(d, exist_ok=True)
    big = blob(3 << 20, 1)
    members = [('.', D, b''), ('./Game.app', D, b''), ('./Game.app/Contents', D, b''),
               ('./Game.app/Contents/big.bin', F, big), ('./Game.app/Contents/empty', F, b''),
               ('./Game.app/Contents/link', L, b'big.bin'), ('./Game.app/._big.bin', F, b'meta'),
               ('../escape', F, b'no'), ('/abs/file', F, b'no'), ('./Game.app/Contents/Empty Dir', D, b''),
               ('./Game.app/Contents/fifo', 0o010644, b''), ('./Game.app/Contents/x/../y.txt', F, b'y'),
               ('./Game.app/Contents/ünï €.txt', F, b'unicode')]
    w = lambda n, b: open(os.path.join(d, n), 'wb').write(b)
    # pbzx (xz + stored chunks) of a newc cpio, in a product archive with two components
    w('pbzx.pkg', xar([('Distribution', b'<installer-gui-script/>', 'gzip'),
                       ('a.pkg/PackageInfo', info('/Applications'), 'gzip'),
                       ('a.pkg/Payload', pbzx(newc(members)), 'none'),
                       ('b.pkg/PackageInfo', info('/'), 'gzip'),
                       ('b.pkg/Payload', gzip.compress(odc([('./usr/share/b.txt', F, b'b')])), 'none')]))
    # bzip2 xar encoding of a plain odc cpio; no install location (the root)
    w('bz2.pkg', xar([('PackageInfo', info(), 'gzip'), ('Payload', odc(members), 'bzip2')]))
    # gzip of two members and zero padding, inside xar's zlib encoding; Scripts without payload/
    two = gzip.compress(odc(members)[:5000]) + gzip.compress(odc(members)[5000:]) + b'\0' * 9
    w('gz2.pkg', xar([('PackageInfo', info('/opt'), 'gzip'), ('Payload', two, 'gzip'),
                      ('Scripts', gzip.compress(odc([('./preinstall', F, b'#!/bin/sh\n')])), 'none')]))
    # GOG style: Scripts/payload with a postinstall naming the bundle; a Payload without <data>
    post = b'#!/bin/bash\ngog_bundle_name="Game.app"\n'
    scr = odc([('.', D, b''), ('./payload', D, b''), ('./payload/Contents', D, b''),
               ('./payload/Contents/a', F, b'a' * 100), ('./payload/Contents/big', F, big),
               ('./payload/Contents/l', L, b'a'), ('./postinstall', F, post), ('./big-script', F, blob(2 << 20, 2))])
    w('gog.pkg', xar([('package.pkg/PackageInfo', info('/Applications'), 'gzip'),
                      ('package.pkg/Payload', b'', 'nodata'),
                      ('package.pkg/Scripts', gzip.compress(scr), 'none')]))
    # nothing to unpack (the "no files found" line), and a file that is not an installer
    w('nofiles.pkg', xar([('Distribution', b'<installer-gui-script/>', 'gzip'), ('Resources/en.lproj/x', b'x' * 5000, 'noenc')]))
    w('notxar.pkg', b'PK\x03\x04 not an installer')
    # an archive whose last bytes are missing, and one whose TOC is cut
    g = xar([('PackageInfo', info('/Applications'), 'gzip'), ('Payload', gzip.compress(odc(members)), 'none')])
    w('cut.pkg', g[:-100])
    w('cuttoc.pkg', g[:60])


def gamepkg(d, loc, bundle, paths):
    """A GOG-style game installer that installs `paths` (relative to the bundle) with made-up data."""
    os.makedirs(d, exist_ok=True)
    ents, dirs = [('.', D, b''), ('./payload', D, b'')], set()
    for i, p in enumerate(paths):
        parts = p.split('/')
        for k in range(1, len(parts)):
            q = '/'.join(parts[:k])
            if q not in dirs:
                dirs.add(q)
                ents.append(('./payload/' + q, D, b''))
        ents.append(('./payload/' + p, F, b'game copy %d' % i))
    ents += [('./payload/Contents/game-only.txt', F, b'game only'), ('./payload/Contents/._x', F, b'm'),
             ('./postinstall', F, f'#!/bin/bash\ngog_bundle_name="{bundle}"\n'.encode())]
    with open(os.path.join(d, 'fakegame.pkg'), 'wb') as f:
        f.write(xar([('Distribution', b'<installer-gui-script/>', 'gzip'),
                     ('package.pkg/PackageInfo', info(loc), 'gzip'),
                     ('package.pkg/Scripts', gzip.compress(odc(ents)), 'none')]))


def patch_name(path, old, new):
    """Replaces a name's bytes in a zip's local and central headers (same length)."""
    b = open(path, 'rb').read()
    assert len(old) == len(new) and b.count(old) == 2
    open(path, 'wb').write(b.replace(old, new))


def zips(d):
    os.makedirs(d, exist_ok=True)
    p = lambda n: os.path.join(d, n)
    big = blob(300_000, 3)
    with zipfile.ZipFile(p('methods.zip'), 'w') as z:
        for i, m in enumerate((zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED, zipfile.ZIP_BZIP2, zipfile.ZIP_LZMA)):
            z.writestr(zipfile.ZipInfo(f'm{m}/data.bin'), big[i:], compress_type=m)
            z.writestr(zipfile.ZipInfo(f'm{m}/empty'), b'', compress_type=m)
        z.writestr('dir/', b'')
        z.writestr('dir/sub/', b'')
        z.writestr('a/../b.txt', b'b')
        z.writestr('/abs/c.txt', b'c')
        z.writestr('./d/./e.txt', b'e')
        z.writestr('é ü/ñ.txt', b'unicode')
        z.writestr('dup.txt', b'first')
        z.writestr('dup.txt', b'second')
        zi = zipfile.ZipInfo('mode.sh')
        zi.external_attr = 0o755 << 16
        z.writestr(zi, b'#!/bin/sh\n')
    with zipfile.ZipFile(p('cp437.zip'), 'w') as z:
        z.writestr('n_XX.txt', b'cp437 name')
    patch_name(p('cp437.zip'), b'n_XX.txt', b'n_\x80\xe9.txt')
    data = open(p('methods.zip'), 'rb').read()
    open(p('prefixed.zip'), 'wb').write(b'#!/bin/sh\nexit 0\n' * 50 + data)     # (a self-extractor's stub)
    with zipfile.ZipFile(p('comment.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
        z.comment = b'an archive comment'
        z.writestr('x/y.txt', big)
    for name, m in (('crc_stored.zip', zipfile.ZIP_STORED), ('crc_deflated.zip', zipfile.ZIP_DEFLATED)):
        with zipfile.ZipFile(p(name), 'w', m) as z:
            z.writestr('ok.txt', b'fine')
            z.writestr('bad.bin', big)
            z.writestr('after.txt', b'not reached')
        b = bytearray(open(p(name), 'rb').read())
        with zipfile.ZipFile(p(name)) as z:
            i = z.getinfo('bad.bin')
            crc = struct.pack('<I', i.CRC)
        assert b.count(crc) == 2
        b = bytes(b).replace(crc, struct.pack('<I', i.CRC ^ 1))
        open(p(name), 'wb').write(b)
    open(p('notzip.zip'), 'wb').write(b'not a zip at all' * 10)
    with zipfile.ZipFile(p('exact64k.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('e.bin', big[:1 << 16])
        z.writestr('two.bin', big[:2 << 16])


def game(d, apk, ggm, obb, bundles):
    """A built game as android-data leaves one: android_package.json, the APK (globalgamemanagers)
    and a main OBB holding these bundle names (made-up data)."""
    os.makedirs(d, exist_ok=True)
    pk = {'apk': apk, 'package': 'com.example.game', 'version_code': 3, 'build_id': '0-1-2-3-4'}
    if obb:
        pk['main_obb'] = obb
    with open(os.path.join(d, 'android_package.json'), 'w') as f:
        json.dump(pk, f, indent=1)
    with zipfile.ZipFile(os.path.join(d, apk), 'w') as z:
        z.write(ggm, 'assets/bin/Data/globalgamemanagers')
        z.writestr('assets/bin/Data/Managed/x.dll', b'x')
    if obb:
        with zipfile.ZipFile(os.path.join(d, obb), 'w') as z:
            z.writestr('assets/0-1-2-3-4', b'')
            for n in bundles:
                z.writestr('assets/AssetBundles/' + n, b'bundle')


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[0] == 'pkgs':
        pkgs(a[1])
    elif a[0] == 'gamepkg':
        gamepkg(a[1], a[2], a[3], a[4:])
    elif a[0] == 'zips':
        zips(a[1])
    elif a[0] == 'game':
        game(a[1], a[2], a[3], a[4], a[5:])
