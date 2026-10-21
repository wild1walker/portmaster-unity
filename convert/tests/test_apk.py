"""pmunity apk: manifests (binary AXML, an App Bundle's protobuf XML), ABI choice, OBB names,
containers, refusals. Hand-made archives only; PMUNITY_APK=<file> also imports a real build
(dry run) and prints the report."""
import io
import json
import os
import struct
import tempfile
import unittest
import zipfile

from pmunity import apk as A

NS = 'http://schemas.android.com/apk/res/android'
RES = {'versionCode': 0x0101021b, 'versionName': 0x0101021c, 'minSdkVersion': 0x0101020c,
       'targetSdkVersion': 0x01010270, 'name': 0x01010003, 'value': 0x01010024, 'glEsVersion': 0x01010281}


# ---- a binary AXML writer (what aapt writes, reduced to what the parser reads) ----------------

def axml(tree, utf8=False, strip_names=False):
    """tree: (tag, [(name, value)], [children]); values: str, int, bool."""
    strings, idx = [], {}

    def s(x):
        if x not in idx:
            idx[x] = len(strings)
            strings.append(x)
        return idx[x]
    # attribute names with resource ids first (the resource map covers indices 0..n-1)
    names = []

    def walk(t):
        for k, _ in t[1]:
            if k in RES and k not in names:
                names.append(k)
        for c in t[2]:
            walk(c)
    walk(tree)
    for k in names:
        if strip_names:
            strings.append('')                  # no name, only the resource id
        else:
            s(k)
    resmap = [RES[k] for k in names]
    s(NS); s('android')
    body = []

    def chunk(ctype, hsize, payload):
        return struct.pack('<HHI', ctype, hsize, 8 + len(payload)) + payload

    def element(t):
        tag, attrs, kids = t
        ab = b''
        for k, v in attrs:
            ns = 0xffffffff if k == 'package' or k == 'split' else s(NS)
            name = names.index(k) if k in RES else s(k)
            if isinstance(v, bool):
                raw, dt, data = 0xffffffff, 0x12, 0xffffffff if v else 0
            elif isinstance(v, int):
                raw, dt, data = 0xffffffff, 0x10, v & 0xffffffff
            else:
                raw = data = s(v)
                dt = 0x03
            ab += struct.pack('<IIIHBBI', ns, name, raw, 8, 0, dt, data)
        ext = struct.pack('<IIHHHHHH', 0xffffffff, s(tag), 20, 20, len(attrs), 0, 0, 0)
        body.append(chunk(0x0102, 16, struct.pack('<II', 1, 0xffffffff) + ext + ab))
        for c in kids:
            element(c)
        body.append(chunk(0x0103, 16, struct.pack('<IIII', 1, 0xffffffff, 0xffffffff, s(tag))))
    element(tree)
    # the string pool
    data, offs = b'', []
    for x in strings:
        offs.append(len(data))
        if utf8:
            e = x.encode('utf-8')
            data += bytes([len(x), len(e)]) + e + b'\0'
        else:
            data += struct.pack('<H', len(x)) + x.encode('utf-16-le') + b'\0\0'
    data += b'\0' * (-len(data) % 4)
    hdr = struct.pack('<IIIII', len(strings), 0, 0x100 if utf8 else 0, 28 + 4 * len(strings), 0)
    pool = struct.pack('<HHI', 0x0001, 28, 28 + 4 * len(strings) + len(data)) + hdr[:20] + struct.pack(f'<{len(offs)}I', *offs) + data
    rmap = chunk(0x0180, 8, struct.pack(f'<{len(resmap)}I', *resmap))
    ns = chunk(0x0100, 16, struct.pack('<IIII', 1, 0xffffffff, s('android'), s(NS)))
    inner = pool + rmap + ns + b''.join(body)
    return struct.pack('<HHI', 0x0003, 8, 8 + len(inner)) + inner


def manifest(pkg='com.test.game', code=7, name='1.2', min_sdk=22, meta=(), split=None, gles=None):
    attrs = [('versionCode', code), ('versionName', name), ('package', pkg)]
    if split:
        attrs.append(('split', split))
    kids = [('uses-sdk', [('minSdkVersion', min_sdk), ('targetSdkVersion', 30)], [])]
    if gles:
        kids.append(('uses-feature', [('glEsVersion', gles)], []))
    app = ('application', [], [('meta-data', [('name', k), ('value', v)], []) for k, v in meta] +
           [('activity', [('name', 'com.unity3d.player.UnityPlayerActivity')], [])])
    return ('manifest', attrs, kids + [app])


# ---- protobuf (an App Bundle's manifest) ------------------------------------------------------

def _vi(n):
    out = b''
    while True:
        b = n & 0x7f
        n >>= 7
        out += bytes([b | (0x80 if n else 0)])
        if not n:
            return out


def _f(num, payload):
    if isinstance(payload, int):
        return _vi(num << 3) + _vi(payload)
    if isinstance(payload, str):
        payload = payload.encode()
    return _vi((num << 3) | 2) + _vi(len(payload)) + payload


def proto_xml(tree):
    def el(t):
        tag, attrs, kids = t
        b = _f(3, tag)
        for k, v in attrs:
            a = _f(2, k)
            if isinstance(v, bool):
                a += _f(6, _f(7, _f(8, int(v))))
            elif isinstance(v, int):
                a += _f(3, str(v)) + _f(6, _f(7, _f(6, v)))
            else:
                a += _f(3, v)
            if k in RES:
                a += _f(5, RES[k])
            b += _f(4, a)
        for c in kids:
            b += _f(5, _f(1, el(c)))
        return b
    return _f(1, el(tree))


# ---- archives ---------------------------------------------------------------------------------

def sf_head(version, fmt=22):
    if fmt >= 22:
        h = struct.pack('>IIII', 0, 0, fmt, 0) + b'\0\0\0\0' + struct.pack('>IQQQ', 0, 0, 0, 0)
    else:
        h = struct.pack('>IIII', 0, 0, fmt, 0) + b'\0\0\0\0'
    return h + version.encode() + b'\0' + b'\0' * 64


def make_apk(path, mf, abis=('arm64-v8a', 'armeabi-v7a'), backend='il2cpp', version='2021.3.4f1', data=True,
             extra=None, metadata=True, axml_kw=None):
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr('AndroidManifest.xml', axml(mf, **(axml_kw or {})))
        for abi in abis:
            libs = ['libmain.so', 'libunity.so'] + (['libil2cpp.so'] if backend == 'il2cpp' else
                                                     ['libmonobdwgc-2.0.so'] if backend == 'mono' else [])
            for n in libs:
                z.writestr(f'lib/{abi}/{n}', b'\x7fELF' + abi.encode())
        if data:
            z.writestr('assets/bin/Data/globalgamemanagers', sf_head(version))
            if backend == 'il2cpp' and metadata:
                z.writestr('assets/bin/Data/Managed/Metadata/global-metadata.dat', struct.pack('<II', 0xFAB11BAF, 29))
            if backend == 'mono':
                z.writestr('assets/bin/Data/Managed/etc/mono/config', b'<configuration/>')
        for k, v in (extra or {}).items():
            z.writestr(k, v)
    return path


def make_obb(path, build_id):
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr('assets/' + build_id, b'')
        z.writestr('assets/bin/Data/sharedassets0.assets', b'x')
    return path


def rd(path, mode='rb'):
    with open(path, mode) as f:
        return f.read()


BID = '0123abcd-0000-1111-2222-333344445555'


class ManifestTest(unittest.TestCase):
    def check(self, mf):
        self.assertEqual(mf.package, 'com.test.game')
        self.assertEqual(mf.version_code, 7)
        self.assertEqual(mf.version_name, '1.2')
        self.assertEqual(mf.min_sdk, 22)
        self.assertEqual(mf.target_sdk, 30)
        self.assertEqual(mf.meta, {'unity.build-id': BID, 'unity.splash-enable': 'true', 'unity.splash-mode': '0'})
        self.assertIn('com.unity3d.player.UnityPlayerActivity', mf.activities)
        self.assertEqual(mf.gles, 0x30000)

    META = (('unity.build-id', BID), ('unity.splash-enable', True), ('unity.splash-mode', 0))

    def test_axml_utf16(self):
        self.check(A.Manifest(A.parse_axml(axml(manifest(meta=self.META, gles=0x30000)))))

    def test_axml_utf8(self):
        self.check(A.Manifest(A.parse_axml(axml(manifest(meta=self.META, gles=0x30000), utf8=True))))

    def test_axml_stripped_attribute_names(self):
        # obfuscated/stripped manifests keep only the resource ids
        self.check(A.Manifest(A.parse_axml(axml(manifest(meta=self.META, gles=0x30000), strip_names=True))))

    def test_proto(self):
        self.check(A.Manifest(A.parse_proto_xml(proto_xml(manifest(meta=self.META, gles=0x30000)))))

    def test_split_name(self):
        mf = A.Manifest(A.parse_axml(axml(manifest(split='config.arm64_v8a'))))
        self.assertEqual(mf.split, 'config.arm64_v8a')

    def test_not_axml(self):
        with self.assertRaises(ValueError):
            A.parse_axml(b'<manifest/>')


class ImportTest(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.d = self.td.name
        self.out = os.path.join(self.d, 'out')
        self.logs = []

    def tearDown(self):
        self.td.cleanup()

    def run_apk(self, inputs, **kw):
        return A.run(inputs, self.out, log=self.logs.append, **kw)

    def refused(self, inputs, text, **kw):
        with self.assertRaises(A.Refused) as cm:
            self.run_apk(inputs, **kw)
        self.assertIn(text, str(cm.exception))

    def test_il2cpp_arm64(self):
        p = make_apk(os.path.join(self.d, 'g.apk'), manifest(meta=(('unity.splash-mode', 0),)),
                     extra={'lib/arm64-v8a/libplugin.so': b'x'})
        r = self.run_apk([p], target='arm64')
        self.assertEqual((r['abi'], r['backend'], r['unity_version'], r['il2cpp_metadata']), ('arm64-v8a', 'il2cpp', '2021.3.4f1', 29))
        self.assertEqual(sorted(os.listdir(os.path.join(self.out, 'lib'))), ['libil2cpp.so', 'libmain.so', 'libplugin.so', 'libunity.so'])
        self.assertEqual(rd(os.path.join(self.out, 'lib', 'libunity.so')), b'\x7fELFarm64-v8a')
        self.assertEqual(rd(os.path.join(self.out, 'base.apk')), rd(p))
        pj = json.loads(rd(os.path.join(self.out, 'android_package.json')))
        self.assertEqual((pj['package'], pj['version_code'], pj['version_name'], pj['apk'], pj['min_sdk'], pj['source']),
                         ('com.test.game', 7, '1.2', 'base.apk', 22, 'apk'))
        self.assertEqual(pj['meta_data'], {'unity.splash-mode': '0'})
        self.assertEqual(pj['plugins'], ['libplugin.so'])
        self.assertFalse([n for n in os.listdir(self.out) if n.startswith('.apk-')])   # temp dir gone

    def test_armhf_target_and_override(self):
        p = make_apk(os.path.join(self.d, 'g.apk'), manifest())
        self.assertEqual(self.run_apk([p], target='armhf')['abi'], 'armeabi-v7a')
        self.assertEqual(self.run_apk([p], target='arm64', abi='armeabi-v7a')['abi'], 'armeabi-v7a')
        self.assertEqual(rd(os.path.join(self.out, 'lib', 'libunity.so')), b'\x7fELFarmeabi-v7a')

    def test_mono_v7a(self):
        p = make_apk(os.path.join(self.d, 'g.apk'), manifest(), abis=('armeabi-v7a', 'x86'), backend='mono',
                     version='2019.4.18f1')
        self.refused([p], 'needs a 32-bit (armhf) userland', target='arm64')
        r = self.run_apk([p], target='armhf')
        self.assertEqual((r['abi'], r['backend']), ('armeabi-v7a', 'mono'))
        self.assertTrue(os.path.isfile(os.path.join(self.out, 'assets/bin/Data/Managed/etc/mono/config')))

    def test_refusals(self):
        p = make_apk(os.path.join(self.d, 'x86.apk'), manifest(), abis=('x86',))
        self.refused([p], 'only for x86')
        p = make_apk(os.path.join(self.d, 'nolib.apk'), manifest(), abis=())
        self.refused([p], 'no native libraries')
        p = make_apk(os.path.join(self.d, 'v8only.apk'), manifest(), abis=('arm64-v8a',))
        self.refused([p], 'needs a 64-bit userland', target='armhf')
        p = make_apk(os.path.join(self.d, 'noscripting.apk'), manifest(), backend=None)
        self.refused([p], 'neither libil2cpp.so nor a Mono runtime')
        p2 = os.path.join(self.d, 'nounity.apk')
        with zipfile.ZipFile(p2, 'w') as z:
            z.writestr('AndroidManifest.xml', axml(manifest()))
            z.writestr('lib/arm64-v8a/libgame.so', b'x')
        self.refused([p2], 'not a Unity Android build')
        p = make_apk(os.path.join(self.d, 'old.apk'), manifest(), version='5.6.7f1')
        self.refused([p], 'Unity 5.6.7f1')
        self.assertEqual(self.run_apk([p], target='arm64', any_version=True)['unity_version'], '5.6.7f1')
        p = make_apk(os.path.join(self.d, 'nodata.apk'), manifest(), data=False)
        self.refused([p], 'give the .obb')
        p = make_apk(os.path.join(self.d, 'nometa.apk'), manifest(), metadata=False)
        self.refused([p], 'global-metadata.dat')
        p = make_apk(os.path.join(self.d, 'vr.apk'), manifest(), extra={'lib/arm64-v8a/libvrapi.so': b'x'})
        self.refused([p], 'a VR build')
        self.assertIn('notes', self.run_apk([p], target='arm64', force=True))
        p = make_apk(os.path.join(self.d, 'b.apk'), manifest())
        orig = A.player_settings
        A.player_settings = lambda z, n, log: {'graphics_apis': [21]}
        try:
            self.refused([p], 'no OpenGL ES')
        finally:
            A.player_settings = orig

    def test_obb_beside_and_build_id(self):
        d = os.path.join(self.d, 'build')
        os.makedirs(d)
        p = make_apk(os.path.join(d, 'Game.apk'), manifest(meta=(('unity.build-id', BID),)), data=False,
                     extra={'assets/bin/Data/Managed/Metadata/global-metadata.dat': struct.pack('<II', 0xFAB11BAF, 27)})
        make_obb(os.path.join(d, 'Game.main.obb'), BID)
        with zipfile.ZipFile(os.path.join(d, 'Game.main.obb'), 'a') as z:
            z.writestr('assets/bin/Data/globalgamemanagers', sf_head('2020.3.3f1'))
        r = self.run_apk([p], target='arm64')
        self.assertEqual(r['main_obb'], 'main.7.com.test.game.obb')
        self.assertEqual(r['build_id'], BID)
        self.assertEqual(r['unity_version'], '2020.3.3f1')          # read from the OBB
        self.assertTrue(os.path.isfile(os.path.join(self.out, 'main.7.com.test.game.obb')))
        # an OBB of another build
        other = make_obb(os.path.join(self.d, 'main.7.com.test.game.obb'), BID.replace('0123', '9999'))
        p2 = make_apk(os.path.join(self.d, 'G2.apk'), manifest(meta=(('unity.build-id', BID),)))
        self.refused([p2, other], 'belongs to another build')
        self.assertIn('main_obb', self.run_apk([p2, other], target='arm64', force=True))
        # another package's OBB
        wrong = make_obb(os.path.join(self.d, 'main.7.com.other.obb'), BID)
        self.refused([p2, wrong], 'an OBB of com.other')

    def test_obb_patch_and_version_code(self):
        p = make_apk(os.path.join(self.d, 'g.apk'), manifest(code=12))
        m = make_obb(os.path.join(self.d, 'main.11.com.test.game.obb'), BID)
        q = make_obb(os.path.join(self.d, 'x.patch.obb'), BID)
        r = self.run_apk([p, m, q], target='arm64')
        self.assertEqual((r['main_obb'], r['patch_obb']), ('main.12.com.test.game.obb', 'patch.12.com.test.game.obb'))

    def test_xapk(self):
        base = make_apk(os.path.join(self.d, 'base.apk'), manifest(meta=(('unity.build-id', BID),)), abis=())
        cfg = os.path.join(self.d, 'config.arm64_v8a.apk')
        with zipfile.ZipFile(cfg, 'w') as z:
            z.writestr('AndroidManifest.xml', axml(manifest(split='config.arm64_v8a')))
            for n in ('libmain.so', 'libunity.so', 'libil2cpp.so'):
                z.writestr(f'lib/arm64-v8a/{n}', b'split')
        obb = make_obb(os.path.join(self.d, 'o.obb'), BID)
        x = os.path.join(self.d, 'game.xapk')
        with zipfile.ZipFile(x, 'w') as z:
            z.write(base, 'com.test.game.apk')
            z.write(cfg, 'config.arm64_v8a.apk')
            z.write(obb, 'Android/obb/com.test.game/main.7.com.test.game.obb')
            z.writestr('manifest.json', json.dumps({'package_name': 'com.test.game'}))
            z.writestr('icon.png', b'')
        r = self.run_apk([x], target='arm64')
        self.assertEqual((r['abi'], r['main_obb']), ('arm64-v8a', 'main.7.com.test.game.obb'))
        self.assertEqual(rd(os.path.join(self.out, 'lib', 'libil2cpp.so')), b'split')
        # the libraries' split missing
        self.refused([base], 'split APK')

    def test_bundletool_apks(self):
        base = make_apk(os.path.join(self.d, 'base-master.apk'), manifest(), abis=())
        cfg = os.path.join(self.d, 'base-arm64_v8a.apk')
        with zipfile.ZipFile(cfg, 'w') as z:
            z.writestr('AndroidManifest.xml', axml(manifest(split='config.arm64_v8a')))
            for n in ('libmain.so', 'libunity.so', 'libil2cpp.so'):
                z.writestr(f'lib/arm64-v8a/{n}', b'split')
        standalone = make_apk(os.path.join(self.d, 'standalone-arm64_v8a.apk'), manifest())
        x = os.path.join(self.d, 'game.apks')
        with zipfile.ZipFile(x, 'w') as z:
            z.writestr('toc.pb', b'')
            z.write(base, 'splits/base-master.apk')
            z.write(cfg, 'splits/base-arm64_v8a.apk')
            z.write(standalone, 'standalones/standalone-arm64_v8a.apk')
        r = self.run_apk([x], target='arm64')
        self.assertEqual(r['abi'], 'arm64-v8a')
        self.assertEqual(rd(os.path.join(self.out, 'lib', 'libunity.so')), b'split')

    def test_asset_pack_split_merged(self):
        base = make_apk(os.path.join(self.d, 'base.apk'), manifest(), data=False, extra={'assets/bin/Data/x': b'base'})
        pack = os.path.join(self.d, 'UnityDataAssetPack.apk')
        with zipfile.ZipFile(pack, 'w') as z:
            z.writestr('AndroidManifest.xml', axml(manifest(split='UnityDataAssetPack')))
            z.writestr('assets/bin/Data/globalgamemanagers', sf_head('2022.3.10f1'))
            z.writestr('assets/bin/Data/Managed/Metadata/global-metadata.dat', struct.pack('<II', 0xFAB11BAF, 31))
            zi = zipfile.ZipInfo('assets/bin/Data/data.unity3d')
            z.writestr(zi, b'D' * 1000)
        r = self.run_apk([base, pack], target='arm64')
        self.assertEqual((r['unity_version'], r['il2cpp_metadata']), ('2022.3.10f1', 31))
        with zipfile.ZipFile(os.path.join(self.out, 'base.apk')) as z, open(os.path.join(self.out, 'base.apk'), 'rb') as f:
            names = z.namelist()
            self.assertIn('assets/bin/Data/globalgamemanagers', names)
            self.assertIn('lib/arm64-v8a/libunity.so', names)
            self.assertEqual(z.read('assets/bin/Data/x'), b'base')
            i = z.getinfo('assets/bin/Data/data.unity3d')
            f.seek(i.header_offset)
            h = f.read(30)
            n, e = struct.unpack_from('<HH', h, 26)
            self.assertEqual((i.header_offset + 30 + n + e) % 4, 0)     # stored data aligned

    def test_aab(self):
        aab = os.path.join(self.d, 'game.aab')
        with zipfile.ZipFile(aab, 'w') as z:
            z.writestr('BundleConfig.pb', b'')
            z.writestr('base/manifest/AndroidManifest.xml', proto_xml(manifest(meta=(('unity.build-id', BID),))))
            for n in ('libmain.so', 'libunity.so', 'libil2cpp.so'):
                z.writestr(f'base/lib/arm64-v8a/{n}', b'aab')
            z.writestr('base/dex/classes.dex', b'dex')
            z.writestr('base/root/unity_extra', b'r')
            z.writestr('base/assets/bin/Data/Managed/Metadata/global-metadata.dat', struct.pack('<II', 0xFAB11BAF, 29))
            z.writestr('UnityDataAssetPack/manifest/AndroidManifest.xml', proto_xml(
                ('manifest', [('package', 'com.test.game'), ('split', 'UnityDataAssetPack')],
                 [('dist:module', [], [('dist:delivery', [], [('dist:install-time', [], [])])])])))
            z.writestr('UnityDataAssetPack/assets/bin/Data/globalgamemanagers', sf_head('2021.3.20f1'))
        r = self.run_apk([aab], target='arm64')
        self.assertEqual((r['package'], r['version_code'], r['unity_version'], r['build_id']), ('com.test.game', 7, '2021.3.20f1', BID))
        with zipfile.ZipFile(os.path.join(self.out, 'base.apk')) as z:
            names = set(z.namelist())
        self.assertTrue({'assets/bin/Data/globalgamemanagers', 'unity_extra', 'lib/arm64-v8a/libunity.so'} <= names)
        self.assertNotIn('dex/classes.dex', names)

    def test_dry_run_and_bad_input(self):
        p = make_apk(os.path.join(self.d, 'g.apk'), manifest())
        r = self.run_apk([p], target='arm64', dry_run=True)
        self.assertEqual(r['abi'], 'arm64-v8a')
        self.assertFalse(os.path.exists(os.path.join(self.out, 'base.apk')))
        junk = os.path.join(self.d, 'junk.apk')
        with open(junk, 'wb') as f:
            f.write(b'not a zip')
        self.refused([junk], 'not a zip')
        z = os.path.join(self.d, 'other.zip')
        with zipfile.ZipFile(z, 'w') as zf:
            zf.writestr('readme.txt', 'x')
        self.refused([z], 'without an APK')
        evil = os.path.join(self.d, 'evil.xapk')
        with zipfile.ZipFile(evil, 'w') as zf:
            zf.write(p, '../../g.apk')
        self.refused([evil], 'unsafe path')

    @unittest.skipUnless(os.environ.get('PMUNITY_APK'), 'PMUNITY_APK=<a real APK> runs a dry import')
    def test_real(self):
        r = A.run(os.environ['PMUNITY_APK'].split(os.pathsep), self.out, dry_run=True, log=print)
        print(r)


if __name__ == '__main__':
    unittest.main()
