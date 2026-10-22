"""StreamingAssets for code written for a desktop build (pmunity/streaming.py, android-data's
--streaming-assets): bundles anywhere in StreamingAssets (2022.3's padded block layout, LZMA
blocks), Addressables catalog CRCs, the folder beside the APK, the port check's suggestion and the
port's setup. Hand-made data only.   python -m unittest discover -s tests   (from convert/)"""
import base64
import json
import lzma
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
from pmunity import android_data as A
from pmunity import port as P
from pmunity import prefs as PR
from pmunity import streaming as ST
import test_port as TP


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    TP.put(path, data)


def read(path):
    with open(path) as f:
        return f.read()


def sf_head(version='2022.3.62f2', platform=24):
    """The start of a SerializedFile of format 22 (48-byte header), platform after the version."""
    meta = version.encode() + b'\0' + struct.pack('<I', platform) + b'\0' * 64
    return struct.pack('>IIII', 0, 0, 22, 0) + b'\0' * 4 + struct.pack('>IQQQ', 100, 1000, 4096, 0) + meta


def bundle(files, fmt=8, block=48, comp=(3,), pad=True):
    """A UnityFS bundle: format 8 with BlockInfoNeedPaddingAtStart (0x200: the blocks start
    16-aligned after the blocks info, as Unity 2022.3 writes them) or format 6; blocks of `block`
    bytes compressed in turn with `comp` (3 LZ4HC, 1 LZMA, 0 stored)."""
    import lz4.block
    data = b''.join(d for _, d in files)
    blocks, raw = [], b''
    for k, i in enumerate(range(0, len(data), block)):
        plain, c = data[i:i + block], comp[k % len(comp)]
        if c == 3:
            z = lz4.block.compress(plain, mode='high_compression', store_size=False)
        elif c == 1:
            f = {'id': lzma.FILTER_LZMA1, 'dict_size': 1 << 16, 'lc': 3, 'lp': 0, 'pb': 2}
            z = bytes([(2 * 5 + 0) * 9 + 3]) + struct.pack('<I', 1 << 16) + lzma.compress(plain, lzma.FORMAT_RAW, filters=[f])
        else:
            z = plain
        blocks.append((len(plain), len(z), c))
        raw += z
    info = b'\0' * 16 + struct.pack('>i', len(blocks)) + b''.join(struct.pack('>IIH', *b) for b in blocks)
    info += struct.pack('>i', len(files))
    off = 0
    for name, d in files:
        info += struct.pack('>qqI', off, len(d), 4) + name.encode() + b'\0'
        off += len(d)
    ci = lz4.block.compress(info, store_size=False)
    head = b'UnityFS\0' + struct.pack('>I', fmt) + b'5.x.x\0' + b'2022.3.62f2\0'
    flags = 0x42 | (0x200 if pad and fmt >= 7 else 0)
    n = len(head) + 20
    hpad = (-n % 16) if fmt >= 7 else 0
    body = ci + (b'\0' * (-(n + hpad + len(ci)) % 16) if flags & 0x200 else b'')
    size = n + hpad + len(body) + len(raw)
    return head + struct.pack('>qIII', size, len(ci), len(info), flags) + b'\0' * hpad + body + raw


def plain_of(path):
    b = A.Bundle(path)
    with open(path, 'rb') as f:
        return b''.join(b.read_block(f, i)[1] for i in range(len(b.blocks)))


class BundleTest(unittest.TestCase):
    def check(self, data, *methods):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, 'x.bundle')
            put(p, data)
            before = plain_of(p)
            self.assertEqual(ST.bundle_crc(p), zlib.crc32(before))
            res = A.patch_bundle_platform(p)
            self.assertEqual({(r[0], r[1]) for r in res if r[2] != 'already'}, {('CAB-a', 24), ('CAB-b', 24)})
            self.assertTrue(set(methods) & {r[2] for r in res}, res)
            self.assertEqual(A.bundle_platforms(p), [('CAB-a', 13), ('CAB-b', 13)])
            after = plain_of(p)
            self.assertEqual(len(after), len(before))
            self.assertEqual([i for i in range(len(after)) if after[i] != before[i]],
                             [i for i in range(len(after)) if (before[i], after[i]) == (24, 13)])
            self.assertEqual(ST.bundle_crc(p), zlib.crc32(after))

    def files(self):
        return [('CAB-a', sf_head() + bytes(range(40))), ('CAB-b', sf_head() + b'z' * 30)]

    def test_padded_2022_layout(self):
        self.check(bundle(self.files(), comp=(0,)), 'literal')

    def test_lz4_padded(self):
        self.check(bundle(self.files(), comp=(3,)), 'literal', 'reencode')

    def test_lzma_blocks(self):
        # BuildAssetBundleOptions.None: LZMA; the block that holds the byte becomes LZ4HC
        self.check(bundle(self.files(), comp=(1,), block=1 << 12), 'rewrite')
        self.check(bundle(self.files(), comp=(3, 1), fmt=6), 'rewrite')        # (mixed blocks, format 6)


def catalog(crcs, extra_records=1):
    """An Addressables catalog: entries pointing at AssetBundleRequestOptions JSON records (one per
    CRC) with other records around them."""
    extra, entries = bytearray(), []

    def record(js):
        off = len(extra)
        asm, cls = b'Unity.ResourceManager', b'UnityEngine.ResourceManagement.ResourceProviders.AssetBundleRequestOptions'
        b = js.encode('utf-16-le')
        extra.extend(bytes([7, len(asm)]) + asm + bytes([len(cls)]) + cls + struct.pack('<i', len(b)) + b)
        return off

    for i in range(extra_records):
        extra.extend(b'\x04' + struct.pack('<i', 1234 + i))        # an Int32 record nobody points at
    for k, c in enumerate(crcs):
        off = record(f'{{"m_Hash":"h{k}","m_Crc":{c},"m_BundleName":"b{k}","m_BundleSize":{100 + k}}}')
        entries.append((k, 1, -1, 0, off, k, 0))
        entries.append((k + 100, 1, -1, 0, off, k, 0))           # (two entries, one record)
    entries.append((999, 0, -1, 0, -1, 0, 0))                      # no data
    ent = struct.pack('<i', len(entries)) + b''.join(struct.pack('<7i', *e) for e in entries)
    c = {'m_LocatorId': 'AddressablesMainContentCatalog', 'm_InternalIds': ['x'], 'm_EntryDataString': base64.b64encode(ent).decode(),
         'm_ExtraDataString': base64.b64encode(bytes(extra)).decode(), 'm_resourceTypes': []}
    return json.dumps(c, separators=(',', ':')).encode()


class CatalogTest(unittest.TestCase):
    def crcs_in(self, text):
        c = json.loads(text)
        return [json.loads(js)['m_Crc'] for _, _, _, js in reversed(ST._records(c)[2])]

    def test_replace(self):
        old = [1133350037, 77, 2372109071, 0]
        text = catalog(old)
        new = {1133350037: 5, 77: 4000000000, 2372109071: 2372109070}
        out, n = ST.catalog_crcs(text, new)
        self.assertEqual(n, 3)
        self.assertEqual(self.crcs_in(out), [5, 4000000000, 2372109070, 0])
        a, b = json.loads(text), json.loads(out)
        self.assertEqual({k for k in a if a[k] != b[k]}, {'m_EntryDataString', 'm_ExtraDataString'})
        # every record but the CRC as it was (names, sizes, the records nobody points at)
        ra, rb = ST._records(a)[2], ST._records(b)[2]
        for x, y in zip(ra, rb):
            self.assertEqual(json.loads(x[3])['m_BundleName'], json.loads(y[3])['m_BundleName'])
        self.assertEqual(base64.b64decode(b['m_ExtraDataString'])[:5], b'\x04' + struct.pack('<i', 1234))
        # the text around the two strings stays byte for byte
        self.assertEqual(out.replace(b['m_ExtraDataString'].encode(), b'').replace(b['m_EntryDataString'].encode(), b''),
                         text.replace(a['m_ExtraDataString'].encode(), b'').replace(a['m_EntryDataString'].encode(), b''))

    def test_nothing_to_do(self):
        text = catalog([10, 20])
        self.assertEqual(ST.catalog_crcs(text, {30: 40}), (text, 0))

    def test_detect(self):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, 'catalog.json')
            put(p, catalog([0, 0]))
            self.assertFalse(ST.catalog_has_crcs(p))
            put(p, catalog([0, 99]))
            self.assertTrue(ST.catalog_has_crcs(p))
            put(p, b'{"not": "a catalog"}')
            self.assertFalse(ST.catalog_has_crcs(p))
        self.assertTrue(ST.is_catalog('aa/catalog.json') and ST.is_catalog('aa/catalog_2024.json'))
        self.assertTrue(ST.is_catalog('aa/catalog.bin'))
        self.assertFalse(ST.is_catalog('aa/settings.json') or ST.is_catalog('x/aa/catalog.json'))

    def test_windows_paths(self):
        # a Windows build's {RuntimePath}\StandaloneWindows64\<bundle> ids get '/', byte for byte in a binary catalog
        rp = b'{UnityEngine.AddressableAssets.Addressables.RuntimePath}'
        binc = (b'\x05\x00' + rp + b'\\StandaloneWindows64\\locales(en)_all.bundle' + b'uP\\\x01\x02' +
                rp + b'/StandaloneLinux64/a.bundle' + struct.pack('<I', 123456789) + b'\\')
        out, n = ST.catalog_fix_paths(binc, True)
        self.assertEqual((n, len(out)), (1, len(binc)))
        self.assertIn(rp + b'/StandaloneWindows64/locales(en)_all.bundleuP\\', out)      # bytes after it kept
        self.assertTrue(out.endswith(struct.pack('<I', 123456789) + b'\\'))
        js = b'{"m_InternalIds":["' + rp + b'\\\\StandaloneWindows64\\\\a.bundle","Assets\\\\x.prefab"]}'
        out, n = ST.catalog_fix_paths(js, False)
        self.assertEqual(n, 1)
        self.assertEqual(json.loads(out)['m_InternalIds'], [rp.decode() + '/StandaloneWindows64/a.bundle', 'Assets\\x.prefab'])
        # binary catalogs: each CRC found exactly once is replaced
        data = b'abc' + struct.pack('<I', 111) + b'def' + struct.pack('<I', 222) * 2
        out, n = ST.catalog_bin_crcs(data, {111: 333, 222: 444, 555: 666})
        self.assertEqual(n, 1)
        self.assertEqual(out, b'abc' + struct.pack('<I', 333) + b'def' + struct.pack('<I', 222) * 2)
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, 'catalog.json')
            put(p, js)
            self.assertTrue(ST.catalog_needs_fix(p))                   # Windows paths, no CRCs
            put(p, b'{"m_InternalIds":["' + rp + b'/StandaloneLinux64/a.bundle"]}')
            self.assertFalse(ST.catalog_needs_fix(p))
            self.assertTrue(ST.catalog_needs_fix(os.path.join(t, 'catalog.bin')) is False)    # (missing)
            put(os.path.join(t, 'catalog.bin'), data)
            self.assertTrue(ST.catalog_needs_fix(os.path.join(t, 'catalog.bin')))
            self.assertEqual(ST.fix_catalog(os.path.join(t, 'catalog.bin'), binc, {123456789: 7})[1:], (1, 1))


class PlaceTest(unittest.TestCase):
    def test_modes(self):
        with tempfile.TemporaryDirectory() as t:
            tree, z = os.path.join(t, 'tree'), os.path.join(t, 'zip')
            files = []
            for rel in ('config.json', 'levels/a.lvl', 'aa/catalog.json'):
                p = os.path.join(tree, 'assets', rel)
                put(p, rel)
                files.append(('assets/' + rel, p))
            log = []
            self.assertEqual(ST.place('both', files, z, A.clone, log.append), files)
            self.assertEqual(read(os.path.join(z, 'Game_Data', 'StreamingAssets', 'levels', 'a.lvl')), 'levels/a.lvl')
            self.assertIn('beside the APK in Game_Data/StreamingAssets/, and in the OBB', log[-1])
            put(os.path.join(z, 'Game_Data', 'StreamingAssets', 'stale.txt'), 'old')
            self.assertEqual(ST.place('loose', files[:1], z, A.clone, log.append), [])
            self.assertEqual(sorted(os.listdir(os.path.join(z, 'Game_Data', 'StreamingAssets'))), ['config.json'])
            self.assertIn('(not in the OBB or APK)', log[-1])
            self.assertEqual(ST.place('obb', files, z, A.clone, log.append), files)
            self.assertFalse(os.path.exists(os.path.join(z, 'Game_Data')))

    def test_data_folder(self):
        with tempfile.TemporaryDirectory() as t:
            for rel in ('Managed/a.dll', 'StreamingAssets/s.json', 'GI/x', 'mods/m1/info.txt', 'config/settings.ini'):
                put(os.path.join(t, 'Data', rel), rel)
            items = ST.data_folder_items(os.path.join(t, 'Data'), 'Game/Game_Data')
            self.assertEqual([(d, a) for d, _, a in items], [('Game/Game_Data/config/settings.ini', 'link'),
                                                            ('Game/Game_Data/mods/m1/info.txt', 'link')])

    def test_asmpatch_command(self):
        self.assertEqual(ST.asmpatch_command("'/x/asmpatch' --only=", 'both'), "'/x/asmpatch' --only= --with=streaming-assets")
        self.assertEqual(ST.asmpatch_command('asmpatch', 'loose'), 'asmpatch --with=streaming-assets-all')
        self.assertEqual(ST.asmpatch_command('asmpatch', 'obb'), 'asmpatch')


class PortCheckTest(unittest.TestCase):
    def rep(self, t, sa_mb=1, io=True, would=None):
        d = TP.fake_game(os.path.join(t, 'g'))
        if sa_mb is not None:
            p = os.path.join(d, 'StreamingAssets', 'levels.json')
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'wb') as f:
                f.truncate(int(sa_mb * 2**20))
        if io:
            TP.write_assembly(os.path.join(d, 'Managed', 'Levels.dll'), 'Levels', [('', 'Levels')],
                              [('UnityEngine', 'Application'), ('System.IO', 'File')], ['Load'],
                              [(1, 'get_streamingAssetsPath'), (2, 'ReadAllText')])
        rep = {'backend': 'mono', 'data': d, 'notes': [], 'suggest': {}}
        if would is not None:
            rep['asmpatch_would'] = would
        ST.port_check(rep)
        return rep

    def test_names_only(self):
        with tempfile.TemporaryDirectory() as t:
            r = self.rep(t)
            self.assertEqual(r['suggest']['android_data']['streaming_assets'], 'both')
            self.assertIn('Levels', r['notes'][0])
            self.assertEqual(ST.code_reads(os.path.join(r['data'], 'Managed')), ['Levels'])

    def test_asmpatch_decides(self):
        with tempfile.TemporaryDirectory() as t:
            r = self.rep(t, would=['rewired-android'])               # asmpatch found no read reaching System.IO
            self.assertNotIn('android_data', r['suggest'])
            self.assertFalse(r['streaming_assets']['io'])
        with tempfile.TemporaryDirectory() as t:
            r = self.rep(t, io=False, would=['streaming-assets', 'streaming-assets-all'])
            self.assertEqual(r['suggest']['android_data']['streaming_assets'], 'both')

    def test_data_path_note(self):
        with tempfile.TemporaryDirectory() as t:
            r = self.rep(t, io=False, would=['data-path'])
            self.assertTrue(any('android_data.data_path = true' in n for n in r['notes']))
            self.assertNotIn('android_data', r['suggest'])

    def test_big_and_none(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertEqual(self.rep(t, sa_mb=300)['suggest']['android_data']['streaming_assets'], 'loose')
        with tempfile.TemporaryDirectory() as t:
            r = self.rep(t, sa_mb=None)
            self.assertEqual(r['suggest'], {})
            self.assertNotIn('streaming_assets', r)

    def test_config(self):
        cfg = P.build_config('g', None, {'android_data': {'streaming_assets': 'both'}})
        self.assertTrue(P.needs_asmpatch(cfg))
        self.assertIn("STREAMING_ASSETS=both", P.setup_env(cfg))
        self.assertIn("DATA_PATH=0", P.setup_env(cfg))
        dp = P.build_config('g', None, {'android_data': {'data_path': True}})
        self.assertTrue(P.needs_asmpatch(dp))
        self.assertIn("DATA_PATH=1", P.setup_env(dp))
        P.validate(cfg)
        cfg['android_data']['streaming_assets'] = 'apk'
        with self.assertRaises(SystemExit):
            P.validate(cfg)
        self.assertFalse(P.needs_asmpatch(P.build_config('g', None, {})))


@unittest.skipUnless(shutil.which('bash'), 'needs bash')
class SetupTest(unittest.TestCase):
    """The android route's setup with streaming_assets: android-data gets --streaming-assets and
    asmpatch (--only= when the port has asmpatch off: only the patch android-data adds runs), and
    a second install keeps what the game wrote into Game_Data."""

    def test_setup(self):
        with tempfile.TemporaryDirectory() as t:
            game = os.path.join(t, 'src', 'My Game')
            TP.fake_game(game, name='My Game')
            cp = os.path.join(t, 'cfg.json')
            put(cp, json.dumps({'port': {'title': 'My Game'}, 'android_data': {'streaming_assets': 'both'}}))
            out = P.new('mygame', game, cp, os.path.join(t, 'port'), asmpatch='/nonexistent-asmpatch', log=lambda m: None)
            self.assertIn('STREAMING_ASSETS=both', read(os.path.join(out, 'mygame', 'setup', 'setup.env')))
            g = os.path.join(t, 'ports', 'mygame')
            shutil.copytree(os.path.join(out, 'mygame'), g)
            put(os.path.join(g, 'setup', 'asmpatch'), '#!/bin/sh\n')
            shutil.copytree(game, os.path.join(g, 'gamedata', 'My Game'))
            put(os.path.join(g, 'Game_Data', 'mods', 'mine.txt'), 'the player\'s')     # (from an earlier install)
            put(os.path.join(g, 'Game_Data', 'StreamingAssets', 'old.json'), 'old')
            pyb, pm = TP.stub_tools(t)
            # the stand-in android-data also writes Game_Data/StreamingAssets into zip/
            py = os.path.join(t, 'stub', 'python', 'bin', 'python3.12')
            s = read(py).replace('echo apk > "$out/zip/$name.apk"',
                                        'echo apk > "$out/zip/$name.apk"; mkdir -p "$out/zip/Game_Data/StreamingAssets"; '
                                        'echo new > "$out/zip/Game_Data/StreamingAssets/levels.json"')
            put(py, s)
            import tarfile
            with tarfile.open(pyb, 'w:gz') as tf:
                tf.add(os.path.join(t, 'stub', 'python'), 'python')
            shutil.copy(pyb, os.path.join(g, 'setup', 'python.tar.gz'))
            log = os.path.join(t, 'pm.log')
            e = dict(os.environ, GAMEDIR=g, controlfolder=pm, DEVICE_ARCH='aarch64', STUB_LOG=log,
                     STUB_RUNTIME='.NET 4.x (libmonobdwgc-2.0)', STUB_VERSION='2019.4.8f1', STUB_FAIL='')
            r = subprocess.run(['bash', os.path.join(g, 'setup', 'patchscript')], env=e, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            text = read(log)
            self.assertIn('--streaming-assets both', text)
            self.assertIn("--asmpatch '" + os.path.join(g, 'setup', 'asmpatch') + "' --only=", text)
            self.assertEqual(read(os.path.join(g, 'Game_Data', 'StreamingAssets', 'levels.json')).strip(), 'new')
            self.assertFalse(os.path.exists(os.path.join(g, 'Game_Data', 'StreamingAssets', 'old.json')))
            self.assertEqual(read(os.path.join(g, 'Game_Data', 'mods', 'mine.txt')), 'the player\'s')


LINUX_PREFS = """<unity_prefs version_major="1" version_minor="1">
	<pref name="Language" type="string">VVM=</pref>
	<pref name="Screenmanager Resolution Height" type="int">1080</pref>
	<pref name="Screenmanager Resolution Width" type="int">1920</pref>
	<pref name="MusicVolume" type="float">0.75</pref>
	<pref name="Save &amp; Slot" type="string">eyJsZXZlbCI6M30=</pref>
</unity_prefs>
"""


class PrefsTest(unittest.TestCase):
    """PlayerPrefs: Unity's Linux XML (as its player writes it) <-> ua_host's SharedPreferences file."""

    def test_round_trip(self):
        with tempfile.TemporaryDirectory() as t:
            pc = os.path.join(t, 'prefs')
            put(pc, LINUX_PREFS)
            hh = os.path.join(t, 'data', 'shared_prefs', 'com.pmunity.g.v2.playerprefs.prefs')
            put(hh, 'i\t__UNITY_PLAYERPREFS_VERSION__\t1\ni\tScreenmanager Resolution Width\t720\n'
                    's\tName\ttab\there%25\n')
            n, kept = PR.copy(pc, hh)
            self.assertEqual((n, kept), (3, 2))
            got = {k: (t_, v) for k, t_, v in PR.read(hh)}
            self.assertEqual(got['Language'], ('s', 'US'))
            self.assertEqual(got['Save & Slot'], ('s', '{"level":3}'))
            self.assertEqual(got['MusicVolume'], ('f', 0.75))
            self.assertEqual(got['Screenmanager Resolution Width'], ('i', 720))   # (the handheld's own)
            self.assertEqual(got['__UNITY_PLAYERPREFS_VERSION__'], ('i', 1))
            self.assertEqual(got['Name'], ('s', 'tab\there%'))
            self.assertNotIn('Screenmanager Resolution Height', got)
            # and back to a new Linux file
            back = os.path.join(t, 'back', 'prefs')
            PR.copy(hh, back, keep_all=True)
            self.assertTrue(read(back).startswith('<unity_prefs'))
            again = {k: (t_, v) for k, t_, v in PR.read(back)}
            self.assertEqual(again, got)

    def test_cli(self):
        with tempfile.TemporaryDirectory() as t:
            pc = os.path.join(t, 'prefs')
            put(pc, LINUX_PREFS)
            r = subprocess.run([sys.executable, '-m', 'pmunity', 'prefs', 'show', pc], capture_output=True, text=True,
                               cwd=os.path.join(HERE, '..'))
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("string Save & Slot = '{\"level\":3}'", r.stdout)
            put(pc, '<other/>')
            r = subprocess.run([sys.executable, '-m', 'pmunity', 'prefs', 'copy', pc, os.path.join(t, 'x.prefs')],
                               capture_output=True, text=True, cwd=os.path.join(HERE, '..'))
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('not a Unity prefs file', r.stderr)


if __name__ == '__main__':
    unittest.main()
