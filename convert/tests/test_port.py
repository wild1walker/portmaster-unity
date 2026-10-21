"""pmunity port: the check (on synthetic games: a fake build folder, .NET assemblies written here),
the config, the generated port folder, the release zip, and the generated setup run with stand-in
tools (offline; nothing of a real game or of Unity)."""
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import unittest
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity import port as P


# ---------------------------------------------------------------- a .NET assembly, written here

def write_assembly(path, name, typedefs=(), typerefs=(), methods=(), memberrefs=(), pinvokes=()):
    """A minimal PE/CLI file with metadata tables: Module, TypeRef, TypeDef, MethodDef, MemberRef,
    ModuleRef, ImplMap, Assembly. memberrefs: (typeref index from 1, name); pinvokes: (method
    index from 1, module, entry)."""
    strings = bytearray(b'\0')
    sidx = {}

    def s(x):
        if x not in sidx:
            sidx[x] = len(strings)
            strings.extend(x.encode() + b'\0')
        return sidx[x]

    modules = []
    for _, mod, _ in pinvokes:
        if mod not in modules:
            modules.append(mod)
    rows = {}
    rows[0x00] = [struct.pack('<HHHHH', 0, s(name + '.dll'), 0, 0, 0)]
    rows[0x01] = [struct.pack('<HHH', 0, s(n), s(ns)) for ns, n in typerefs]
    rows[0x02] = [struct.pack('<IHHHHH', 0, s(n), s(ns), 0, 1, 1) for ns, n in typedefs]
    rows[0x06] = [struct.pack('<IHHHHH', 0, 0, 0, s(m), 0, 1) for m in methods]
    rows[0x0A] = [struct.pack('<HHH', (tr << 3) | 1, s(m), 0) for tr, m in memberrefs]
    rows[0x1A] = [struct.pack('<H', s(m)) for m in modules]
    rows[0x1C] = [struct.pack('<HHHH', 0, (mi << 1) | 1, s(e), modules.index(mod) + 1) for mi, mod, e in pinvokes]
    rows[0x20] = [struct.pack('<IHHHHIHHH', 0x8004, 1, 0, 0, 0, 0, 0, s(name), 0)]
    rows = {t: r for t, r in rows.items() if r}
    valid = sum(1 << t for t in rows)
    tables = struct.pack('<IBBBBQQ', 0, 2, 0, 0, 1, valid, 0)
    tables += b''.join(struct.pack('<I', len(rows[t])) for t in sorted(rows))
    tables += b''.join(b''.join(rows[t]) for t in sorted(rows))
    while len(tables) % 4:
        tables += b'\0'
    while len(strings) % 4:
        strings += b'\0'
    version = b'v4.0.30319\0\0'
    streams = [(b'#~', tables), (b'#Strings', bytes(strings))]
    hdr_len = 16 + len(version) + 4 + sum(8 + ((len(n) + 4) & ~3) for n, _ in streams)
    md = bytearray(b'BSJB' + struct.pack('<HHII', 1, 1, 0, len(version)) + version + struct.pack('<HH', 0, len(streams)))
    off = hdr_len
    for n, data in streams:
        md += struct.pack('<II', off, len(data)) + n + b'\0' * (((len(n) + 4) & ~3) - len(n))
        off += len(data)
    for _, data in streams:
        md += data
    va, raw = 0x2000, 0x200
    cli = struct.pack('<IHHII', 72, 2, 5, va + 72, len(md)) + b'\0' * (72 - 16)
    section = cli + bytes(md)
    pe = bytearray(b'MZ' + b'\0' * 0x3A + struct.pack('<I', 0x80))
    pe += b'\0' * (0x80 - len(pe))
    pe += b'PE\0\0' + struct.pack('<HHIIIHH', 0x14C, 1, 0, 0, 0, 224, 0x2102)
    opt = bytearray(224)
    struct.pack_into('<H', opt, 0, 0x10B)
    struct.pack_into('<II', opt, 96 + 14 * 8, va, 72)
    pe += opt
    pe += b'.text\0\0\0' + struct.pack('<IIII', len(section), va, len(section), raw) + b'\0' * 16
    pe += b'\0' * (raw - len(pe))
    pe += section
    with open(path, 'wb') as f:
        f.write(pe)


def put(path, data):
    with open(path, 'wb' if isinstance(data, bytes) else 'w') as f:
        f.write(data)


def write_sf(path, version):
    """A SerializedFile header (format 17) naming a Unity version: enough for the version reader."""
    head = struct.pack('>IIII', 0, 0, 17, 0) + b'\0\0\0\0' + version.encode() + b'\0' + struct.pack('<I', 24)
    with open(path, 'wb') as f:
        f.write(head + b'\0' * 64)


def fake_game(root, version='2019.4.8f1', pointer=True, rewired=False, name='Game'):
    """<root>/<name>.x86_64 (an ELF header) + <name>_Data with globalgamemanagers and Managed/."""
    os.makedirs(os.path.join(root, f'{name}_Data', 'Managed'), exist_ok=True)
    with open(os.path.join(root, f'{name}.x86_64'), 'wb') as f:
        f.write(b'\x7fELF\x02\x01\x01' + b'\0' * 9 + struct.pack('<HH', 3, 0x3E) + b'\0' * 64)
    d = os.path.join(root, f'{name}_Data')
    write_sf(os.path.join(d, 'globalgamemanagers'), version)
    trefs = [('UnityEngine', 'Input'), ('UnityEngine', 'MonoBehaviour'), ('UnityEngine.UI', 'InputField')]
    mrefs = [(1, 'GetMouseButtonDown'), (1, 'get_mousePosition'), (1, 'GetKeyDown')] if pointer else [(1, 'GetAxis')]
    tdefs = [('', 'Menu')] + ([('Rewired', 'InputManager')] if rewired else [])
    write_assembly(os.path.join(d, 'Managed', 'Assembly-CSharp.dll'), 'Assembly-CSharp', tdefs, trefs,
                   ['Update', 'OnMouseDown', 'Start'], mrefs, [(3, 'steam_api', 'SteamAPI_Init')])
    with open(os.path.join(d, 'Managed', 'UnityEngine.dll'), 'wb') as f:
        f.write(b'MZ')                          # (platform assemblies are not read)
    return d


class AssemblyTest(unittest.TestCase):
    def test_reader(self):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, 'A.dll')
            write_assembly(p, 'A', [('N', 'T'), ('', 'U')], [('UnityEngine', 'Input'), ('TMPro', 'TMP_InputField')],
                           ['Update', 'OnMouseDown'], [(1, 'GetMouseButton'), (2, 'set_text')],
                           [(1, 'fmodstudio', 'FMOD_Studio_System_Create'), (2, 'libc', 'getpid')])
            a = P.Assembly(p)
            self.assertEqual(a.name, 'A')
            self.assertEqual(a.typedefs, [('N', 'T'), ('', 'U')])
            self.assertEqual(a.typerefs, [('UnityEngine', 'Input'), ('TMPro', 'TMP_InputField')])
            self.assertEqual(a.methods, ['Update', 'OnMouseDown'])
            self.assertEqual(a.memberrefs, [('UnityEngine', 'Input', 'GetMouseButton'), ('TMPro', 'TMP_InputField', 'set_text')])
            self.assertEqual(a.modulerefs, ['fmodstudio', 'libc'])
            self.assertEqual(a.pinvokes, [('fmodstudio', 'FMOD_Studio_System_Create'), ('libc', 'getpid')])

    def test_not_assembly(self):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, 'x.dll')
            put(p, b'\x7fELF' + b'\0' * 100)
            with self.assertRaises(P.NotAssembly):
                P.Assembly(p)

    def test_survey(self):
        with tempfile.TemporaryDirectory() as t:
            d = fake_game(t, rewired=True)
            s = P.survey_assemblies(os.path.join(d, 'Managed'))
            self.assertEqual(s['assemblies'], ['Assembly-CSharp'])
            self.assertIn('UnityEngine', s['libraries'])
            self.assertEqual(s['legacy']['mouse'], 2)
            self.assertEqual(s['legacy']['keys'], 1)
            self.assertEqual(s['mouse_messages'], ['OnMouseDown'])
            self.assertIn('UI InputField', s['text_input'])
            self.assertEqual(s['pinvoke'], {'steam_api': ['Assembly-CSharp']})
            self.assertTrue(s['rewired'].startswith('Assembly-CSharp.dll (source edition)'))
            self.assertEqual(P.input_verdict(s), 'pointer')
            s['selects'] = 2
            self.assertEqual(P.input_verdict(s), 'mixed')


class CheckTest(unittest.TestCase):
    def test_builds(self):
        with tempfile.TemporaryDirectory() as t:
            d = fake_game(os.path.join(t, 'g'))
            bs = P.find_builds(t)
            self.assertEqual(len(bs), 1)
            self.assertEqual((bs[0]['build'], bs[0]['arch'], bs[0]['backend'], bs[0]['exe']), ('linux', 'x86_64', 'mono', 'Game.x86_64'))
            self.assertEqual(P.find_builds(d)[0]['data'], os.path.abspath(d))
            app = os.path.join(t, 'M.app', 'Contents')
            os.makedirs(os.path.join(app, 'Resources', 'Data', 'Managed'))
            os.makedirs(os.path.join(app, 'MacOS'))
            put(os.path.join(app, 'MacOS', 'M'), b'\xcf\xfa\xed\xfe' + b'\0' * 32)
            write_sf(os.path.join(app, 'Resources', 'Data', 'globalgamemanagers'), '2018.4.1f1')
            b = P.find_builds(os.path.join(t, 'M.app'))[0]
            self.assertEqual(b['build'], 'macos')
            self.assertEqual(b['backend'], 'unknown')       # (no assemblies)

    def test_check_routes(self):
        with tempfile.TemporaryDirectory() as t:
            fake_game(t, pointer=True, rewired=True)
            rep = P.check(t, asmpatch='/nonexistent-asmpatch')
            self.assertEqual(rep['unity_version'], '2019.4.8f1')
            self.assertEqual(rep['route'], 'android')
            self.assertEqual(rep['shader_source'], 'glcore')
            self.assertEqual(rep['suggest']['android_data']['asmpatch'], 'auto')
            self.assertEqual(rep['suggest']['runtime']['UA_POINTER'], '1')
            box = next(r for r in rep['routes'] if r['route'] == 'box64')
            self.assertTrue(box['ok'])
            out = io.StringIO()
            P.print_report(rep, out)
            self.assertIn('recommended route: android', out.getvalue())

    def test_unsupported(self):
        with tempfile.TemporaryDirectory() as t:
            d = fake_game(t, version='5.5.4f1')
            rep = P.check(t, asmpatch='/nonexistent-asmpatch')
            self.assertFalse(next(r for r in rep['routes'] if r['route'] == 'android')['ok'])
            self.assertEqual(rep['route'], 'box64')         # (a Linux x86-64 build)
            shutil.rmtree(os.path.join(d, 'Managed'))
            os.makedirs(os.path.join(d, 'il2cpp_data'))
            put(os.path.join(t, 'Game.x86_64'), b'\x7fELF\x02\x01\x01' + b'\0' * 9 + struct.pack('<HH', 3, 3))
            rep = P.check(t)
            self.assertEqual(rep['backend'], 'il2cpp')
            self.assertIsNone(rep['route'])
            self.assertIn('scripting backend il2cpp', rep['unsupported'])


class ConfigTest(unittest.TestCase):
    def test_toml_round_trip(self):
        cfg = P.build_config('mygame', None, {'game': {'unity_versions': ['2020.3.1f1']},
                                              'runtime': {'UA_POINTER': '1'},
                                              'convert': {'shaders': {'from': ['d3d11']}},
                                              'questions': [{'id': 'TEXTURE_QUALITY', 'question': 'Q "x"?',
                                                             'options': [['medium', 'Medium'], ['fast', 'Fast']],
                                                             'default': 'medium', 'convert_set': 'textures.quality'}]})
        P.validate(cfg)
        back = tomllib.loads(P.dump_toml(cfg))
        self.assertEqual(P.merge(P.DEFAULTS, back), cfg)
        self.assertEqual(cfg['android_data']['package'], 'com.pmunity.mygame')
        self.assertEqual(cfg['port']['title'], 'mygame')

    def test_validate(self):
        bad = P.build_config('Bad Name', None, {'game': {'unity_versions': ['x']}, 'runtime': {'ua-x': '1'},
                                                'questions': [{'id': 'q', 'question': '?', 'options': [['a b', 'A']]}]})
        with self.assertRaises(SystemExit) as cm:
            P.validate(bad)
        msg = str(cm.exception)
        for part in ('port.name', 'runtime.ua-x', 'id', 'plain values', 'unity_versions'):
            self.assertIn(part, msg)

    def test_shell_files(self):
        cfg = P.build_config('g', None, {'game': {'unity_versions': ['2020.3.1f1'], 'data': 'My Game_Data'},
                                         'runtime': {'UA_KEYMAP': "start:escape,x:space", 'UA_POINTER': '1'},
                                         'android_data': {'args': ['--rewired-joysticks', '2']}})
        with tempfile.TemporaryDirectory() as t:
            put(os.path.join(t, 'setup.env'), P.setup_env(cfg))
            put(os.path.join(t, 'port.env'), P.port_env(cfg))
            out = subprocess.run(['bash', '-c', f'. {t}/setup.env; UA_POINTER=0; . {t}/port.env; '
                                  'echo "$GAME_DATA|${ANDROID_DATA_ARGS[1]}|$UA_KEYMAP|$UA_POINTER"'],
                                 capture_output=True, text=True, check=True).stdout.strip()
            self.assertEqual(out, 'My Game_Data|2|start:escape,x:space|0')

    def test_questions_lua(self):
        lua = P.questions_lua([{'id': 'A', 'question': 'say "hi"', 'options': [['x', 'X'], ['y', 'Y']]}])
        self.assertIn('id = "A"', lua)
        self.assertIn('question = "say \\"hi\\""', lua)
        self.assertIn('{ "x", "X" },', lua)


def stub_tools(t):
    """A stand-in for the setup's Python (pmunity commands that log and make what the setup reads)
    packed as python.tar.gz, and for PortMaster's 7-Zip (unzip)."""
    py = os.path.join(t, 'stub', 'python', 'bin', 'python3.12')
    os.makedirs(os.path.dirname(py), exist_ok=True)
    with open(py, 'w') as f:
        f.write(r'''#!/bin/bash
if [ "$1" = -c ]; then case "$2" in *scripting_runtime*) echo "$STUB_RUNTIME";; esac; exit 0; fi
shift 2; echo "pm $*" >> "$STUB_LOG"
[ "$1" = "$STUB_FAIL" ] && exit 1
case "$1" in
  version) echo "$STUB_VERSION (supported)";;
  player) d=$4; L=$d/Variations/mono/Release; mkdir -p "$L/Libs/armeabi-v7a" "$L/MonoLibs/armeabi-v7a"
          for f in libmain.so libunity.so; do echo x > "$L/Libs/armeabi-v7a/$f"; done
          for f in libmono.so libmonobdwgc-2.0.so libMonoPosixHelper.so; do echo x > "$L/MonoLibs/armeabi-v7a/$f"; done;;
  unpack) rm "$2/data.unity3d"; touch "$2/globalgamemanagers";;
  android-data) while [ $# -gt 0 ]; do case "$1" in --out) out=$2;; --name) name=$2;; --src) src=$2;; esac; shift; done
          mkdir -p "$out/zip/assets/bin/Data/Managed/etc"; echo apk > "$out/zip/$name.apk"
          echo '{}' > "$out/android_data_report.json"; rm -rf "$src"/*;;
esac
''')
    os.chmod(py, 0o755)
    with tarfile.open(os.path.join(t, 'python.tar.gz'), 'w:gz') as tf:
        tf.add(os.path.join(t, 'stub', 'python'), 'python')
    sz = os.path.join(t, 'pm', '7zzs.aarch64')
    os.makedirs(os.path.dirname(sz), exist_ok=True)
    with open(sz, 'w') as f:
        f.write('#!/bin/bash\n# 7zz x -y -o<dir> <archive>\necho "7zz $*" >> "$STUB_LOG"; o=${3#-o}; unzip -q -o "$4" -d "$o"\n')
    os.chmod(sz, 0o755)
    return os.path.join(t, 'python.tar.gz'), os.path.dirname(sz)


@unittest.skipUnless(shutil.which('bash'), 'needs bash')
class NewAndSetupTest(unittest.TestCase):
    def make(self, t, cfg_extra=None, version='2019.4.8f1'):
        game = os.path.join(t, 'src', 'My Game')
        fake_game(game, version=version, name='My Game')
        cfg = {'port': {'title': 'My Game', 'porter': ['someone']},
               'questions': [{'id': 'TEXTURE_QUALITY', 'question': 'Texture quality?',
                              'options': [['medium', 'Medium'], ['fast', 'Fast']], 'default': 'medium',
                              'convert_set': 'textures.quality'},
                             {'id': 'CURSOR', 'question': 'Cursor?', 'options': [['1', 'On'], ['0', 'Off']],
                              'env': 'UA_POINTER_CURSOR'}]}
        cfg = P.merge(cfg, cfg_extra or {})
        cp = os.path.join(t, 'cfg.json')
        with open(cp, 'w') as f:
            json.dump(cfg, f)
        out = P.new('mygame', game, cp, os.path.join(t, 'port'), asmpatch='/nonexistent-asmpatch', log=lambda m: None)
        return game, out

    def test_new_files(self):
        with tempfile.TemporaryDirectory() as t:
            _, out = self.make(t)
            with open(os.path.join(out, '.port_files')) as f:
                files = set(f.read().split('\n')) - {''}
            for f in ('My Game.sh', 'mygame/setup/patchscript', 'mygame/setup/setup.env', 'mygame/setup/convert.json',
                      'mygame/setup/questions.lua', 'mygame/setup.txt', 'mygame/port.json', 'mygame/port.env',
                      'mygame/README.md', 'mygame/gamedata/PUT_GAME_FILES_HERE.txt', 'mygame/mygame.gptk',
                      'README.md', 'package.sh', 'port.build.json'):
                self.assertIn(f, files)
            for f in files:
                text = open(os.path.join(out, f), errors='replace').read()
                if f != 'port.build.json':          # (it records route.json)
                    self.assertNotIn('@@', text, f)
                if f.endswith(('.sh', 'patchscript')):
                    subprocess.run(['bash', '-n', os.path.join(out, f)], check=True)
                    self.assertTrue(os.access(os.path.join(out, f), os.X_OK), f)
            pj = json.load(open(os.path.join(out, 'mygame', 'port.json')))
            self.assertEqual(pj['items'], ['My Game.sh', 'mygame'])
            self.assertEqual(pj['name'], 'mygame.zip')
            launcher = open(os.path.join(out, 'My Game.sh')).read()
            self.assertIn('GAMEDIR="/$directory/ports/mygame"', launcher)
            self.assertIn('PATCHER_QUESTIONS', launcher)
            build = json.load(open(os.path.join(out, 'port.build.json')))
            self.assertEqual(build['config']['game']['data'], 'My Game_Data')
            self.assertEqual(build['config']['runtime'], {'UA_POINTER': '1'})
            # again, without questions: their files go
            self.make(t, {'questions': []})
            self.assertFalse(os.path.exists(os.path.join(out, 'mygame', 'setup', 'questions.lua')))

    def test_package(self):
        with tempfile.TemporaryDirectory() as t:
            _, out = self.make(t, {'build': {'commands': ['echo tool > "$BUILD/tool" && chmod +x "$BUILD/tool"'],
                                             'outputs': {'setup/files/tool': '{build}/tool'}}})
            rt = os.path.join(t, 'rt')
            for d in ('runtime/host/out', 'runtime/media/out/licenses'):
                os.makedirs(os.path.join(rt, d))
            put(os.path.join(rt, 'runtime/host/out/ua_host'), 'host')
            put(os.path.join(rt, 'runtime/media/out/licenses/libvpx.copyright'), 'BSD')
            pyb = os.path.join(t, 'py.tar.gz')
            with tarfile.open(pyb, 'w:gz') as tf:
                for n, data in (('python/lib/python3.12/LICENSE.txt', b'PSF'),
                                ('python/lib/python3.12/site-packages/a-1.dist-info/LICENSE', b'MIT a')):
                    ti = tarfile.TarInfo(n)
                    ti.size = len(data)
                    tf.addfile(ti, io.BytesIO(data))
            z = P.package(out, os.path.join(t, 'rel'), rt, log=lambda m: None, pybundle=pyb, no_build=True)
            with zipfile.ZipFile(z) as zf:
                names = set(zf.namelist())
                modes = {i.filename: i.external_attr >> 16 & 0o777 for i in zf.infolist()}
                lic = zf.read('mygame/licenses/LICENSE.setup-python.txt').decode()
            for n in ('My Game.sh', 'mygame/ua_host', 'mygame/setup/python.tar.gz', 'mygame/setup/pmunity/__main__.py',
                      'mygame/setup/pmunity/shaders/glsl.py', 'mygame/setup/pmunity/video_decode_android.shader',
                      'mygame/licenses/LICENSE.libvpx.txt', 'mygame/licenses/LICENSE.portmaster-unity.txt',
                      'mygame/licenses/LICENSE.libhybris-Apache-2.0.txt', 'mygame/setup/patchscript'):
                self.assertIn(n, names)
            self.assertFalse(any('port_template' in n or '__pycache__' in n for n in names))
            self.assertNotIn('mygame/setup/asmpatch', names)        # (the port does not use it)
            self.assertEqual(modes['My Game.sh'], 0o755)
            self.assertEqual(modes['mygame/ua_host'], 0o755)
            self.assertEqual(modes['mygame/setup/patchscript'], 0o755)
            self.assertEqual(modes['mygame/setup/files/tool'], 0o755)          # (a build output)
            self.assertIn('MIT a', lic)
            self.assertNotIn('port.toml', ' '.join(names))

    def run_setup(self, t, out, env):
        """The generated launcher's setup part: patchscript with GAMEDIR etc., stand-in tools."""
        pyb, pm = stub_tools(t)
        g = os.path.join(t, 'ports', 'mygame')
        if not os.path.isdir(g):
            shutil.copytree(os.path.join(out, 'mygame'), g)
        if os.path.isdir(os.path.join(g, 'setup')):
            shutil.copy(pyb, os.path.join(g, 'setup', 'python.tar.gz'))
        log = os.path.join(t, 'pm.log')
        e = dict(os.environ, GAMEDIR=g, controlfolder=pm, DEVICE_ARCH='aarch64', STUB_LOG=log,
                 STUB_RUNTIME='.NET 4.x (libmonobdwgc-2.0)', STUB_VERSION='2019.4.8f1', STUB_FAIL='')
        e.update(env)
        r = subprocess.run(['bash', os.path.join(g, 'setup', 'patchscript')], env=e, capture_output=True, text=True)
        return g, r, (P._read(log) if os.path.exists(log) else '')

    def test_setup_folder(self):
        with tempfile.TemporaryDirectory() as t:
            game, out = self.make(t)
            g = os.path.join(t, 'ports', 'mygame')
            shutil.copytree(os.path.join(out, 'mygame'), g)
            shutil.copytree(game, os.path.join(g, 'gamedata', 'My Game'))
            with open(os.path.join(g, 'setup.txt'), 'a') as f:
                f.write('cursor=0\n')
            # first run: the conversion fails (interrupted), the second continues from there
            g, r, log = self.run_setup(t, out, {'TEXTURE_QUALITY': 'fast', 'STUB_FAIL': 'convert'})
            self.assertIn('PATCH_FAIL_MSG:Converting', r.stdout)
            g, r, log = self.run_setup(t, out, {'TEXTURE_QUALITY': 'medium'})
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual(log.count('pm player fetch 2019.4.8f1'), 1)
            self.assertIn('--set textures.quality=fast', log)            # (the first answer is kept)
            self.assertIn('--name MyGame --package com.pmunity.mygame', log)
            self.assertNotIn('--asmpatch', log)
            self.assertTrue(os.path.isfile(os.path.join(g, '.setup_done')))
            self.assertEqual(sorted(os.listdir(os.path.join(g, 'lib'))), ['libMonoPosixHelper.so', 'libmain.so',
                                                                         'libmonobdwgc-2.0.so', 'libunity.so'])
            self.assertTrue(os.path.isfile(os.path.join(g, 'MyGame.apk')))
            self.assertEqual(P._read(os.path.join(g, 'answers.env')).strip(), "export UA_POINTER_CURSOR='0'")
            self.assertEqual(os.listdir(os.path.join(g, 'gamedata')), ['PUT_GAME_FILES_HERE.txt'])
            self.assertFalse(os.path.exists(os.path.join(g, 'setup')))
            self.assertIn('setup finished', P._read(os.path.join(g, 'patchlog.txt')))

    def test_setup_archive_and_checks(self):
        with tempfile.TemporaryDirectory() as t:
            game, out = self.make(t, {'android_data': {'asmpatch': 'auto'}})
            g = os.path.join(t, 'ports', 'mygame')
            shutil.copytree(os.path.join(out, 'mygame'), g)
            put(os.path.join(g, 'setup', 'asmpatch'), '#!/bin/sh\n')
            z = os.path.join(g, 'gamedata', 'game-linux.zip')
            with zipfile.ZipFile(z, 'w') as zf:
                for r_, _, fs in os.walk(game):
                    for n in fs:
                        zf.write(os.path.join(r_, n), os.path.relpath(os.path.join(r_, n), os.path.dirname(game)))
            # another Unity version than the port's: refused
            g, r, log = self.run_setup(t, out, {'STUB_VERSION': '2019.4.9f1'})
            self.assertIn('PATCH_FAIL_MSG:This port is made for My Game built with Unity 2019.4.8f1; '
                          'your copy was built with Unity 2019.4.9f1', r.stdout)
            self.assertIn('7zz x -y -o', log)
            shutil.rmtree(os.path.join(g, 'gamedata', '.setup'))
            g, r, log = self.run_setup(t, out, {'STUB_RUNTIME': '.NET 3.5 (libmono)'})
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("--asmpatch '" + os.path.join(g, 'setup', 'asmpatch') + "'", log)
            self.assertIn('--set textures.quality=medium', log)          # (the default)
            self.assertNotIn('libmonobdwgc-2.0.so', os.listdir(os.path.join(g, 'lib')))
            self.assertFalse(os.path.isfile(z))                          # (the archive is removed once built)
            self.assertIn('removing game-linux.zip: not needed to play', P._read(os.path.join(g, 'patchlog.txt')))
            self.assertIn('keep_installers=0', P._read(os.path.join(g, 'setup.txt')))


class RoutesTest(unittest.TestCase):
    def test_templates(self):
        rts = P.routes()
        self.assertIn('android', rts)
        for r in rts.values():
            for dst, spec in r['files'].items():
                P.template_path(r, spec if isinstance(spec, str) else spec['template'])
        with self.assertRaises(SystemExit):
            P.render('@@NOPE@@', {})




class DecodedTexturesTest(unittest.TestCase):
    def test_textures_step_suggested_when_bc_decodes_large(self):
        from pmunity import port as PT
        big = {'notes': [], 'data_survey': {'decoded_mb': 600.0, 'textures': {}}}
        PT.suggest_config(big)
        self.assertIn('textures', big['suggest']['convert']['steps'])
        small = {'notes': [], 'data_survey': {'decoded_mb': 20.0, 'textures': {}}}
        PT.suggest_config(small)
        self.assertNotIn('steps', small['suggest']['convert'])


if __name__ == '__main__':
    unittest.main()
