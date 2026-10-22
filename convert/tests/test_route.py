"""pmunity route: the route detector on synthetic builds (headers only, made here; no game files)."""
import json
import os
import struct
import tempfile
import unittest

from pmunity import route as R



def sf_header(version, target, fmt=17):
    """The first bytes of a serialized file: format, Unity version, build target."""
    p = 48 if fmt >= 22 else 20
    head = bytearray(p)
    struct.pack_into('>I', head, 8, fmt)
    return bytes(head) + version.encode() + b'\0' + struct.pack('<i', target) + bytes(64)


def elf64(needed=(), versions=(), machine=62, interp=True, extra=b''):
    """A minimal x86_64 ELF: PT_INTERP, DT_NEEDED entries, GNU version needs (one library)."""
    dynstr = bytearray(b'\0')
    offs = {}
    for n in list(needed) + list(versions):
        offs[n] = len(dynstr)
        dynstr += n.encode() + b'\0'
    interp_s = b'/lib64/ld-linux-x86-64.so.2\0'
    dyn = b''.join(struct.pack('<qQ', 1, offs[n]) for n in needed) + struct.pack('<qQ', 0, 0)
    vn = b''
    if versions:
        vn = struct.pack('<HHIII', 1, len(versions), offs[needed[0]] if needed else 0, 16, 0)
        for i, v in enumerate(versions):
            vn += struct.pack('<IHHII', 0, 0, 2 + i, offs[v], 16 if i < len(versions) - 1 else 0)
    o_interp, o_dynstr, o_dyn, o_vn, o_sh = 0x100, 0x200, 0x300, 0x400, 0x500
    img = bytearray(o_sh + 64 * 4)
    img[0:16] = b'\x7fELF\x02\x01\x01' + bytes(9)
    struct.pack_into('<HHI', img, 16, 2, machine, 1)
    struct.pack_into('<QQQ', img, 24, 0, 64, o_sh)
    struct.pack_into('<HHHHHH', img, 52, 64, 56, 1 if interp else 0, 64, 4, 0)
    if interp:
        struct.pack_into('<IIQQQQQQ', img, 64, 3, 4, o_interp, 0, 0, len(interp_s), len(interp_s), 1)
    img[o_interp:o_interp + len(interp_s)] = interp_s
    img[o_dynstr:o_dynstr + len(dynstr)] = dynstr
    img[o_dyn:o_dyn + len(dyn)] = dyn
    img[o_vn:o_vn + len(vn)] = vn
    for i, (t, off, size) in enumerate([(3, o_dynstr, len(dynstr)), (6, o_dyn, len(dyn)), (0x6ffffffe, o_vn, len(vn))]):
        struct.pack_into('<IIQQQQIIQQ', img, o_sh + 64 * (i + 1), 0, t, 0, 0, off, size, 1, 0, 8, 0)
    return bytes(img) + extra


def pe(machine=0x8664):
    h = bytearray(0x200)
    h[0:2] = b'MZ'
    struct.pack_into('<I', h, 0x3c, 0x80)
    h[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<H', h, 0x84, machine)
    return bytes(h)


class Build:
    """A game folder under a temporary directory."""

    def __init__(self, test):
        self.tmp = tempfile.TemporaryDirectory()
        test.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name

    def file(self, rel, data=b''):
        p = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(data)
        return p

    def dir(self, rel):
        os.makedirs(os.path.join(self.root, rel), exist_ok=True)


def linux_build(t, version='2021.3.20f1', il2cpp=False, plugins=(), fmt=22,
                player=b'force-glcore\0force-vulkan\0GLX_EXT_create_context_es2_profile\0',
                exe_machine=62, name='Game'):
    b = Build(t)
    b.file(f'{name}_Data/globalgamemanagers', sf_header(version, 24, fmt))
    b.file(f'{name}.x86_64' if exe_machine == 62 else f'{name}.x86',
           elf64(['libdl.so.2', 'libc.so.6'], ['GLIBC_2.2.5', 'GLIBC_2.34'], machine=exe_machine))
    b.file('UnityPlayer.so', elf64(['libstdc++.so.6', 'libgcc_s.so.1', 'libm.so.6'], ['GLIBC_2.15'], interp=False,
                                   extra=player + b'libGL.so.1\0libX11.so.6\0libasound.so.2\0libGLESv1_CM.so.1\0'))
    if il2cpp:
        b.file('GameAssembly.so', elf64(['libc.so.6'], interp=False))
        b.dir(f'{name}_Data/il2cpp_data/Metadata')
    else:
        b.file(f'{name}_Data/Managed/Assembly-CSharp.dll', pe(0x14c))
        b.dir(f'{name}_Data/MonoBleedingEdge/x86_64')
    for p in plugins:
        b.file(f'{name}_Data/Plugins/{p}', elf64(['libc.so.6'], interp=False))
    return b


class RouteTest(unittest.TestCase):
    def test_binaries(self):
        b = Build(self)
        self.assertEqual(R.binary_kind(b.file('a', elf64())), ('elf', 'x86_64'))
        self.assertEqual(R.binary_kind(b.file('b', elf64(machine=3))), ('elf', 'x86'))
        self.assertEqual(R.binary_kind(b.file('c.dll', pe())), ('pe', 'x86_64'))
        self.assertEqual(R.binary_kind(b.file('d', b'\xcf\xfa\xed\xfe' + struct.pack('<i', 0x0100000c) + bytes(32))),
                         ('macho', 'arm64'))
        self.assertIsNone(R.binary_kind(b.file('e', b'text')))
        e = R.elf_info(b.file('f', elf64(['libGL.so.1', 'libc.so.6'], ['GLIBC_2.17', 'GLIBC_2.4', 'GLIBCXX_3.4.21'])))
        self.assertEqual(e['needed'], ['libGL.so.1', 'libc.so.6'])
        self.assertEqual((e['glibc'], e['glibcxx']), ('GLIBC_2.17', 'GLIBCXX_3.4.21'))
        self.assertEqual(e['interp'], '/lib64/ld-linux-x86-64.so.2')
        self.assertEqual(R.elf_info(b.file('g', b'\x7fELF')), {})

    def test_mono_linux_fits_android(self):
        b = linux_build(self, '2021.3.20f1', plugins=['lib_burst_generated.so', 'libsteam_api.so'])
        f = R.examine(R.find_data_dirs(b.root)[0])
        self.assertEqual((f['unity'], f['platform'], f['backend']), ('2021.3.20f1', 'linux', 'mono'))
        self.assertEqual(f['route'], 'android')
        self.assertTrue(f['box64']['fits'])
        self.assertTrue(any('Burst' in c for c in f['android']['caveats']))
        self.assertEqual(f['player']['renderers'], ['glcore', 'vulkan', 'gles'])
        self.assertEqual(R.gl_mode(f), 'gles')
        libs = f['libraries']
        self.assertIn('libGL.so.1', libs['native'])
        self.assertIn('libgcc_s.so.1', libs['x64lib'])
        self.assertNotIn('libGLESv1_CM.so.1', libs['native'] + libs['emulated'])    # optional SDL renderer
        self.assertNotIn('libc.so.6', libs['native'])                               # glibc: the device's

    def test_il2cpp_goes_box64(self):
        b = linux_build(self, '2022.3.4f1', il2cpp=True)
        f = R.examine(R.find_data_dirs(b.root)[0])
        self.assertEqual((f['backend'], f['route']), ('il2cpp', 'box64'))
        self.assertTrue(any('IL2CPP' in x for x in f['android']['blockers']))
        self.assertEqual(f['game_assembly']['arch'], 'x86_64')

    def test_out_of_range_versions(self):
        for v, fmt in (('5.5.4f1', 17), ('5.4.6f3', 15), ('6000.7.0b5', 23)):
            b = linux_build(self, v, fmt=fmt)
            f = R.examine(R.find_data_dirs(b.root)[0])
            self.assertEqual(f['unity'], v)                 # format 23: read past unity_version's range
            self.assertEqual(f['route'], 'box64', v)
            self.assertTrue(any('cover' in x for x in f['android']['blockers']), v)

    def test_x86_fmod_and_gl2(self):
        b = linux_build(self, '5.3.2p3', fmt=15, exe_machine=3, player=b'force-glcore\0force-opengl\0',
                        plugins=['x86/libfmod.so'])
        f = R.examine(R.find_data_dirs(b.root)[0])
        self.assertIn('opengl2', f['player']['renderers'])
        self.assertEqual(R.gl_mode(f), 'gl4es')           # 5.3: the legacy OpenGL 2 renderer
        self.assertTrue(any('32-bit' in c for c in f['box64']['caveats']))
        self.assertTrue(any('FMOD' in c for c in f['android']['caveats']))
        # unknown x86-only plugins block the Android route
        b = linux_build(self, '2019.4.1f1', fmt=21, plugins=['x86_64/libmystery.so'])
        f = R.examine(R.find_data_dirs(b.root)[0])
        self.assertFalse(f['android']['fits'])
        self.assertEqual(f['route'], 'box64')

    def test_other_platforms(self):
        b = Build(self)
        b.file('Game.app/Contents/Resources/Data/globalgamemanagers', sf_header('2019.4.40f1', 2))
        b.dir('Game.app/Contents/Resources/Data/Managed')
        b.dir('Game.app/Contents/MonoBleedingEdge')
        f = R.examine(R.find_data_dirs(b.root)[0])
        self.assertEqual((f['platform'], f['route']), ('macos', 'android'))
        self.assertTrue(f['box64']['fits'])                 # the same version's Linux player
        w = Build(self)
        w.file('Game_Data/globalgamemanagers', sf_header('2020.3.1f1', 19, 22))
        w.file('Game.exe', pe())
        w.file('GameAssembly.dll', pe())
        f = R.examine(R.find_data_dirs(w.root)[0])
        self.assertEqual((f['platform'], f['backend'], f['route']), ('windows', 'il2cpp', None))
        m = Build(self)
        m.file('Game_Data/globalgamemanagers', sf_header('2020.3.1f1', 19, 22))
        m.dir('Game_Data/Managed')
        f = R.examine(R.find_data_dirs(m.root)[0])
        self.assertEqual(f['route'], 'android')             # Direct3D 11 programs translated (2019.1+ too)
        self.assertTrue(any('Direct3D' in x for x in f['android']['caveats']))
        self.assertFalse(any('winbcl' in x for x in f['android']['caveats']))
        self.assertFalse(f['box64']['fits'])                # no Linux player for Direct3D data
        n = Build(self)
        n.file('Game_Data/globalgamemanagers', sf_header('2021.3.28f1', 19, 22))
        n.dir('Game_Data/Managed')
        f = R.examine(R.find_data_dirs(n.root)[0])
        self.assertEqual(f['route'], 'android')
        self.assertTrue(any('winbcl' in x for x in f['android']['caveats']))   # unityjit-win32 libraries
        # a compressed build (data.unity3d only): the platform from its executable
        c = Build(self)
        c.file('Game_Data/data.unity3d', b'UnityFS\0\0\0\0\x065.x.x\x002021.3.28f1\0' + bytes(64))
        c.dir('Game_Data/Managed')
        c.file('Game.exe', pe())
        f = R.examine(R.find_data_dirs(c.root)[0])
        self.assertEqual((f['platform'], f['compressed'], f['route']), ('windows', True, 'android'))
        self.assertTrue(any('Direct3D' in x for x in f['android']['caveats']))

    def test_launcher(self):
        b = linux_build(self, '2022.3.4f1', il2cpp=True, name='My Game')
        f = R.examine(R.find_data_dirs(b.root)[0])
        text = R.launcher(f, port='mygame')
        self.assertNotRegex(text, r'@@[A-Z0-9_]+@@')
        self.assertIn('GAMEDIR="/$directory/ports/mygame"', text)
        self.assertIn('# PORTMASTER: mygame.zip, My Game.sh', text)
        with tempfile.TemporaryDirectory() as d:
            t = os.path.join(d, 't.in')
            with open(t, 'w') as fh:
                fh.write('@@TITLE@@ @@NOPE@@')
            with self.assertRaises(ValueError):
                R.launcher(f, t)
        # game.env: what the setup finds, for the launcher (paths relative to gamedata/)
        env = dict(l.split('=', 1) for l in R.game_env(f, b.root).splitlines())
        self.assertEqual(env['EXE'], "'My Game.x86_64'")
        self.assertEqual((env['GAME_DIR'], env['DATA'], env['GL_MODE']), ('.', "'My Game_Data'", 'gles'))

    def test_gl_modes(self):
        # GLES mode needs the converter's versions; else desktop GL (mesa) or gl4es (GL 2 players)
        for v, fmt, player, want in (('2019.4.1f1', 21, b'force-glcore\0GLX_EXT_create_context_es2_profile\0', 'gles'),
                                     ('6000.7.0b5', 23, b'force-glcore\0GLX_EXT_create_context_es2_profile\0', 'mesa'),
                                     ('5.3.2p3', 15, b'force-glcore\0force-opengl\0', 'gl4es')):
            b = linux_build(self, v, fmt=fmt, player=player)
            self.assertEqual(R.gl_mode(R.examine(R.find_data_dirs(b.root)[0])), want, v)

    def test_port_hooks(self):
        b = linux_build(self, '2022.3.4f1', il2cpp=True)
        rep = {'data': R.find_data_dirs(b.root)[0], 'routes': [{'route': 'box64', 'ok': True, 'why': [], 'needs': []}]}
        R.port_check(rep)
        self.assertTrue(any('GL mode gles' in n for n in rep['routes'][0]['needs']))

        class Ctx:
            def __init__(self, route, out):
                self.cfg = {'port': {'route': route, 'name': 'g', 'title': 'G', 'setup_time': 'a minute'},
                            'game': {'what': 'the Linux build of G'}}
                self.out, self.written = out, {}

            def write(self, rel, data, mode=0o644):
                self.written[rel] = data

        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, 'g'))
            with open(os.path.join(d, 'g', 'port.json'), 'w') as fh:
                json.dump({'attr': {'desc': '', 'runtime': [], 'reqs': ['armhf']}}, fh)
            with open(os.path.join(d, 'g', 'port.env'), 'w') as fh:
                fh.write('# header\n[ -n "${PMU_GL+x}" ] || export PMU_GL=mesa\n')
            c = Ctx('box64', d)
            R.port_new(c)
            self.assertIn('"graphics_apis": [11, 17]', c.written['g/setup/gles.json'])
            pj = json.loads(c.written['g/port.json'])['attr']
            self.assertEqual((pj['runtime'], pj['reqs'], pj['min_glibc']), (['weston_pkg_0.2.squashfs'], [], '2.29'))
            self.assertIn('export PMU_GL=mesa', c.written['g/port.env'])
            self.assertIn('#PMU_BOX64=default', c.written['g/port.env'])
            c = Ctx('android', d)
            R.port_new(c)
            self.assertEqual(c.written, {})

    def test_cli(self):
        b = linux_build(self, '2022.3.4f1', il2cpp=True)
        import contextlib
        import io
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            R.main([b.root, '--json'])
        self.assertEqual(json.loads(out.getvalue())['route'], 'box64')
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            R.main([b.root])
        self.assertIn('route: box64', out.getvalue())
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            R.main([b.root, '--env'])
        self.assertIn("EXE=Game.x86_64", out.getvalue())


if __name__ == '__main__':
    unittest.main()
