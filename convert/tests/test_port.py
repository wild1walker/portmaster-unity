"""pmunity port: the check (on synthetic games: a fake build folder, .NET assemblies written here),
the config, the generated port folder, the release zip, and the generated setup run with stand-in
tools (offline; nothing of a real game or of Unity)."""
import io
import json
import os
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import tomllib
import unittest
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pmunity import port as P
from test_apk import BID, make_apk, make_obb, manifest as apk_manifest


# ---------------------------------------------------------------- a .NET assembly, written here

def write_assembly(path, name, typedefs=(), typerefs=(), methods=(), memberrefs=(), pinvokes=(), version=1,
                   assemblyrefs=()):
    """A minimal PE/CLI file with metadata tables: Module, TypeRef, TypeDef, MethodDef, MemberRef,
    ModuleRef, ImplMap, Assembly (version <version>.0.0.0). memberrefs: (typeref index from 1,
    name); pinvokes: (method index from 1, module, entry)."""
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
    rows[0x20] = [struct.pack('<IHHHHIHHH', 0x8004, version, 0, 0, 0, 0, 0, s(name), 0)]
    rows[0x23] = [struct.pack('<HHHHIHHHH', 2, 1, 0, 0, 0, 0, s(a), 0, 0) for a in assemblyrefs]
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

    def test_hdrp_no_android_player_route(self):
        # HDRP does not render on the Android player's GLES3 device: no route on it fits, the arm64 ones
        # neither (Suvalf, 6000.3 HDRP with Vulkan shaders only, got mono-arm64 recommended)
        for v in ('2021.3.28f1', '6000.0.61f1'):
            rep = {'backend': 'mono', 'unity_version': v, 'unity_supported': True, 'shader_source': 'vulkan',
                   'hdrp': True, 'scripting_runtime': '.NET 4.x', 'build': 'linux', 'arch': 'x86_64'}
            P.decide_routes(rep)
            for name in ('android', 'il2cpp-arm64', 'mono-arm64'):
                r = next(r for r in rep['routes'] if r['route'] == name)
                self.assertFalse(r['ok'], (v, name))
                self.assertTrue(any('HDRP' in w for w in r['why']), (v, name))
            self.assertNotIn(rep['route'], ('android', 'il2cpp-arm64', 'mono-arm64'))

    def test_mouse_current_unchecked(self):
        # asmpatch --survey: the Input System's Mouse.current used with no null check -> pointer mode, even
        # where the UI is also selected for pad navigation (a handheld has no mouse unless pointer mode
        # presents one: Hey, Listen! threw NullReferenceException in CursorHandler.Update every frame)
        survey = ('  Mouse.current / Pointer.current: 4 reads (CursorHandler.Update, CompanionVisual.Update)\n'
                  '  used with no null check in 2 methods (CursorHandler.Update, CompanionVisual.Update): a handheld has no mouse unless pointer\n')
        for text, want in ((survey, '1'), ('  Mouse.current / Pointer.current: 1 reads (A.Update)\n', None)):
            rep = {'notes': [], 'code': {'input_system': True, 'legacy': {}}, 'input_verdict': 'mixed', 'asmpatch_survey': text}
            P.suggest_config(rep)
            self.assertEqual(rep['suggest']['runtime'].get('UA_POINTER'), want)
            notes = ' '.join(rep['notes'])
            if want:
                self.assertIn('Mouse.current with no null check (CursorHandler.Update, CompanionVisual.Update)', notes)
                self.assertNotIn('try the pad first', notes)
            else:
                self.assertIn('try the pad first', notes)

    def test_windows_class_libraries(self):
        # a Windows Mono build of 2021.2+ ships unityjit-win32 class libraries: the check names winbcl
        exe = bytearray(b'MZ' + b'\0' * 0x3A + struct.pack('<I', 0x80) + b'\0' * 0x40)
        exe[0x80:0x86] = b'PE\0\0' + struct.pack('<H', 0x8664)
        for marker, want in ((b'Interop+Kernel32 SetThreadErrorMode', True), (b'Interop+Sys', False)):
            with tempfile.TemporaryDirectory() as t:
                d = fake_game(t, version='2021.3.28f1')
                os.remove(os.path.join(t, 'Game.x86_64'))
                put(os.path.join(t, 'Game.exe'), bytes(exe) + b'\0' * 64)
                put(os.path.join(d, 'Managed', 'mscorlib.dll'), b'MZ' + marker)
                rep = P.check(t, asmpatch='/nonexistent-asmpatch', deep=False)
                self.assertEqual((rep['build'], rep['backend']), ('windows', 'mono'))
                android = next(r for r in rep['routes'] if r['route'] == 'android')
                self.assertEqual(any('winbcl' in n for n in android['needs']), want, android['needs'])

    def test_unity6_netstandard(self):
        # a Unity 6 build without netstandard.dll: the setup's winbcl adds Unity's facade (the check says so)
        for have in (False, True):
            with tempfile.TemporaryDirectory() as t:
                d = fake_game(t, version='6000.2.6f2')
                if have:
                    put(os.path.join(d, 'Managed', 'netstandard.dll'), b'MZ')
                rep = P.check(t, asmpatch='/nonexistent-asmpatch', deep=False)
                android = next(r for r in rep['routes'] if r['route'] == 'android')
                self.assertEqual(any('netstandard' in n for n in android['needs']), not have, android['needs'])

    def test_compressed_settings(self):
        # a compressed build: globalgamemanagers is read from inside data.unity3d (graphics APIs)
        from test_android_data import archive
        with tempfile.TemporaryDirectory() as t:
            d = fake_game(t)
            os.remove(os.path.join(d, 'globalgamemanagers'))
            put(os.path.join(d, 'data.unity3d'), archive([('globalgamemanagers', b'not a serialized file' * 4)]))
            r = P.survey_data(d, deep=False)
            self.assertTrue(any(n.startswith('globalgamemanagers: not read') for n in r['notes']), r['notes'])
            P.COMPRESSED_SURVEY_MAX, keep = 16, P.COMPRESSED_SURVEY_MAX
            try:
                self.assertEqual(P.survey_data(d, deep=False)['notes'], [])     # too big to open: not tried
            finally:
                P.COMPRESSED_SURVEY_MAX = keep

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


def roomy_path(t):
    """PATH with a `df` that reports a card with 64 GB free first (the IL2CPP setups' space checks
    then do not depend on this machine's own free space)."""
    d = os.path.join(t, 'roomy')
    os.makedirs(d, exist_ok=True)
    put(os.path.join(d, 'df'), '#!/bin/sh\necho "Filesystem 1048576-blocks Used Available Capacity Mounted on"\n'
                               'echo "card 65536 0 65536 0% /card"\n')
    os.chmod(os.path.join(d, 'df'), 0o755)
    return d + os.pathsep + os.environ.get('PATH', '')


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
  winbcl) if [ -n "$STUB_WINBCL" ]; then mkdir -p "$3"; for n in $STUB_WINBCL; do echo x > "$3/$n"; done; fi;;
  convert) if [ -n "$PMHLSLCC" ]; then echo "env PMHLSLCC=$PMHLSLCC" >> "$STUB_LOG"; fi;;
  il2cpp) out=$3; while [ $# -gt 0 ]; do case "$1" in --pkg) pkg=$2;; esac; shift; done
          L=$pkg/Variations/il2cpp/Release/Libs/arm64-v8a; mkdir -p "$out/lib/arm64-v8a" "$L"
          echo x > "$out/lib/arm64-v8a/libil2cpp.so"; for f in libmain.so libunity.so; do echo x > "$L/$f"; done;;
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
    def make(self, t, cfg_extra=None, version='2019.4.8f1', windows=False):
        game = os.path.join(t, 'src', 'My Game')
        d = fake_game(game, version=version, name='My Game')
        if windows:             # the Windows layout (My Game.exe beside the data), unityjit-win32 class libraries
            os.remove(os.path.join(game, 'My Game.x86_64'))
            exe = bytearray(b'MZ' + b'\0' * 0x3A + struct.pack('<I', 0x80) + b'\0' * 0x40)
            exe[0x80:0x86] = b'PE\0\0' + struct.pack('<H', 0x8664)
            put(os.path.join(game, 'My Game.exe'), bytes(exe) + b'\0' * 64)
            put(os.path.join(d, 'Managed', 'mscorlib.dll'), b'MZ Interop+Kernel32 SetThreadErrorMode')
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
                      'mygame/README.md', 'mygame/gamedata/PUT_GAME_FILES_HERE.txt', 'mygame/mygame.ini',
                      'mygame/gameinfo.xml', 'README.md', 'package.sh', 'port.build.json', 'testing_thread.txt'):
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
            self.assertEqual(P.launcher_warnings(launcher), [])
            # the README in PortMaster's layout: Notes (a thank-you first), Controls, Compile; no title
            readme = P._read(os.path.join(out, 'README.md'))
            self.assertTrue(readme.startswith('## Notes\n\nThanks to the makers of My Game'), readme[:80])
            heads = [l for l in readme.splitlines() if l.startswith('#')]
            self.assertEqual(heads, ['## Notes', '## Controls', '## Compile'])
            self.assertIn('| Select + Start | Quit |', readme)
            self.assertIn('runtime/build.sh', readme)
            self.assertIn('32-bit (armhf) userland', readme)
            self.assertNotIn('tools/asmpatch/build.sh', readme)           # (the port does not use it)
            self.assertNotIn('\u2014', readme)
            self.assertIn('| Select + Start | Quit |', P._read(os.path.join(out, 'mygame', 'README.md')))
            gi = P._read(os.path.join(out, 'mygame', 'gameinfo.xml'))
            self.assertIn('<path>./My Game.sh</path>', gi)
            self.assertIn('<image>./mygame/screenshot.png</image>', gi)
            tt = P._read(os.path.join(out, 'testing_thread.txt'))
            self.assertIn('Title: My Game', tt)
            self.assertIn(pj['attr']['inst'], tt)
            build = json.load(open(os.path.join(out, 'port.build.json')))
            self.assertEqual(build['config']['game']['data'], 'My Game_Data')
            self.assertEqual(build['config']['runtime'], {'UA_POINTER': '1'})
            # again, without questions: their files go
            self.make(t, {'questions': []})
            self.assertFalse(os.path.exists(os.path.join(out, 'mygame', 'setup', 'questions.lua')))

    def test_vulkan_port(self):
        """a Vulkan-only build (shaders.from = ["vulkan"]): pmspirvcross (aarch64) in the release with SPIRV-Cross's and
        SMOL-V's licences; other ports do not take it"""
        with tempfile.TemporaryDirectory() as t:
            game, out = self.make(t, {'convert': {'shaders': {'from': ['vulkan']}}}, version='2022.3.36f1')
            cfg = json.load(open(os.path.join(out, 'port.build.json')))['config']
            self.assertTrue(P.needs_pmspirvcross(cfg) and P.wants('pmspirvcross', cfg))
            self.assertFalse(P.needs_pmhlslcc(cfg))
            self.assertFalse(P.needs_pmspirvcross(P.merge(cfg, {'convert': {'shaders': {'from': ['glcore']}}})))
            rt = os.path.join(t, 'rt')
            for d in ('runtime/host/out', 'runtime/media/out/licenses', 'sc'):
                os.makedirs(os.path.join(rt, d))
            put(os.path.join(rt, 'runtime/host/out/ua_host'), 'host')
            put(os.path.join(rt, 'runtime/media/out/licenses/libvpx.copyright'), 'BSD')
            put(os.path.join(rt, 'sc', 'pmspirvcross'), 'elf')
            pyb = os.path.join(t, 'py.tar.gz')
            with tarfile.open(pyb, 'w:gz') as tf:
                ti = tarfile.TarInfo('python/lib/python3.12/LICENSE.txt')
                ti.size = 3
                tf.addfile(ti, io.BytesIO(b'PSF'))
            with self.assertRaises(SystemExit) as cm:   # (no licence beside the binary)
                P.package(out, os.path.join(t, 'rel'), rt, log=lambda m: None, pybundle=pyb, no_build=True,
                          pmspirvcross_bin=os.path.join(rt, 'sc', 'pmspirvcross'))
            self.assertIn('pmspirvcross.LICENSE.txt', str(cm.exception))
            put(os.path.join(rt, 'sc', 'pmspirvcross.LICENSE.txt'), 'Apache 2.0 SPIRV-Cross, MIT SMOL-V')
            z = P.package(out, os.path.join(t, 'rel'), rt, log=lambda m: None, pybundle=pyb, no_build=True,
                          pmspirvcross_bin=os.path.join(rt, 'sc', 'pmspirvcross'))
            with zipfile.ZipFile(z) as zf:
                modes = {i.filename: i.external_attr >> 16 & 0o777 for i in zf.infolist()}
                self.assertEqual(zf.read('mygame/licenses/LICENSE.pmspirvcross-SPIRV-Cross-SMOL-V.txt'),
                                 b'Apache 2.0 SPIRV-Cross, MIT SMOL-V')
            self.assertEqual(modes['mygame/setup/pmspirvcross'], 0o755)
            self.assertNotIn('mygame/setup/pmhlslcc', modes)
            with self.assertRaises(SystemExit) as cm:   # (the default place, not built)
                P.package(out, os.path.join(t, 'rel'), rt, log=lambda m: None, pybundle=pyb, no_build=True)
            self.assertIn('ARCH=arm64 tools/spirvcross/build.sh', str(cm.exception))

    def test_windows_port(self):
        """a Windows build of 2021.2+: Direct3D 11 shaders (pmhlslcc in the release, PMHLSLCC in the setup)
        and the Unix class libraries (pmunity winbcl in the setup, its files shipped with --managed-file)"""
        with tempfile.TemporaryDirectory() as t:
            game, out = self.make(t, version='2022.2.17f1', windows=True)
            build = json.load(open(os.path.join(out, 'port.build.json')))
            cfg = build['config']
            self.assertEqual((cfg['game']['build'], cfg['port']['route']), ('windows', 'android'))
            self.assertEqual(cfg['convert']['shaders']['from'], ['d3d11'])
            self.assertTrue(P.needs_pmhlslcc(cfg) and P.needs_winbcl(cfg))
            self.assertIn("WINBCL=auto", P._read(os.path.join(out, 'mygame', 'setup', 'setup.env')))
            self.assertIn('winbcl = "auto"', P.dump_toml(cfg))
            readme = P._read(os.path.join(out, 'README.md'))
            self.assertIn('ARCH=arm64 tools/hlslcc/build.sh', readme)
            self.assertIn('Windows-only class libraries', readme)
            self.assertIn(f'about {P.WINBCL_MB} MB of its Windows editor installer', P._read(os.path.join(out, 'mygame', 'README.md')))
            self.assertIn(f'(about\n{P.PLAYER_MB + P.WINBCL_MB} MB', P._read(os.path.join(out, 'mygame', 'gamedata', 'PUT_GAME_FILES_HERE.txt')))
            pj = json.load(open(os.path.join(out, 'mygame', 'port.json')))
            self.assertIn(f'player and class libraries from Unity, about {P.PLAYER_MB + P.WINBCL_MB} MB', pj['attr']['inst'])
            # before 2021.2 there is nothing to replace: the READMEs say the player only
            cfg19 = P.merge(cfg, {'game': {'unity_versions': ['2019.4.8f1']}})
            self.assertTrue(P.needs_pmhlslcc(cfg19))
            self.assertFalse(P.needs_winbcl(cfg19))
            self.assertEqual(P.download_more(cfg19), '')
            # the release: pmhlslcc (aarch64) with HLSLcc's licence
            rt = os.path.join(t, 'rt')
            for d in ('runtime/host/out', 'runtime/media/out/licenses', 'hl'):
                os.makedirs(os.path.join(rt, d))
            put(os.path.join(rt, 'runtime/host/out/ua_host'), 'host')
            put(os.path.join(rt, 'runtime/media/out/licenses/libvpx.copyright'), 'BSD')
            put(os.path.join(rt, 'hl', 'pmhlslcc'), 'elf')
            pyb = os.path.join(t, 'py.tar.gz')
            with tarfile.open(pyb, 'w:gz') as tf:
                ti = tarfile.TarInfo('python/lib/python3.12/LICENSE.txt')
                ti.size = 3
                tf.addfile(ti, io.BytesIO(b'PSF'))
            with self.assertRaises(SystemExit) as cm:   # (no licence beside the binary)
                P.package(out, os.path.join(t, 'rel'), rt, log=lambda m: None, pybundle=pyb, no_build=True,
                          pmhlslcc_bin=os.path.join(rt, 'hl', 'pmhlslcc'))
            self.assertIn('pmhlslcc.LICENSE.txt', str(cm.exception))
            put(os.path.join(rt, 'hl', 'pmhlslcc.LICENSE.txt'), 'MIT HLSLcc, BSD bstrlib')
            z = P.package(out, os.path.join(t, 'rel'), rt, log=lambda m: None, pybundle=pyb, no_build=True,
                          pmhlslcc_bin=os.path.join(rt, 'hl', 'pmhlslcc'))
            with zipfile.ZipFile(z) as zf:
                modes = {i.filename: i.external_attr >> 16 & 0o777 for i in zf.infolist()}
                self.assertEqual(zf.read('mygame/licenses/LICENSE.pmhlslcc-HLSLcc.txt'), b'MIT HLSLcc, BSD bstrlib')
            self.assertEqual(modes['mygame/setup/pmhlslcc'], 0o755)
            with self.assertRaises(SystemExit) as cm:   # (the default place, not built)
                P.package(out, os.path.join(t, 'rel'), rt, log=lambda m: None, pybundle=pyb, no_build=True)
            self.assertIn('ARCH=arm64 tools/hlslcc/build.sh', str(cm.exception))
            # the setup: winbcl with the player after the fetch, PMHLSLCC for the conversion, its files
            # shipped with --managed-file
            g = os.path.join(t, 'ports', 'mygame')
            shutil.copytree(os.path.join(out, 'mygame'), g)
            shutil.copytree(game, os.path.join(g, 'gamedata', 'My Game'))
            put(os.path.join(g, 'setup', 'pmhlslcc'), '#!/bin/sh\n')
            g, r, log = self.run_setup(t, out, {'STUB_VERSION': '2022.2.17f1', 'STUB_WINBCL': 'System.dll netstandard.dll'})
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            W, S = os.path.join(g, 'gamedata', '.setup'), os.path.join(g, 'setup')
            wb = next(l for l in log.splitlines() if l.startswith('pm winbcl '))
            self.assertEqual(wb, f"pm winbcl {g}/gamedata/My Game/My Game_Data {W}/bcl --version 2022.2.17f1 "
                                 f"--player {W}/player")
            self.assertIn(f'env PMHLSLCC={S}/pmhlslcc', log)
            ad = next(l for l in log.splitlines() if l.startswith('pm android-data '))
            self.assertIn(f'--managed-file {W}/bcl/System.dll --managed-file {W}/bcl/netstandard.dll', ad)
            self.assertLess(log.index('pm player fetch'), log.index('pm winbcl'))
            self.assertTrue(os.path.isfile(os.path.join(g, '.setup_done')))
            # winbcl = "off": not run
            shutil.rmtree(g)
            game, out = self.make(t, {'android_data': {'winbcl': 'off'}}, version='2022.2.17f1', windows=True)
            self.assertFalse(P.needs_winbcl(json.load(open(os.path.join(out, 'port.build.json')))['config']))
            shutil.copytree(os.path.join(out, 'mygame'), g)
            shutil.copytree(game, os.path.join(g, 'gamedata', 'My Game'))
            os.unlink(os.path.join(t, 'pm.log'))
            g, r, log = self.run_setup(t, out, {'STUB_VERSION': '2022.2.17f1', 'STUB_WINBCL': 'System.dll'})
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertNotIn('pm winbcl', log)
            self.assertNotIn('--managed-file', log)
            self.assertNotIn('PMHLSLCC', log)                # (no pmhlslcc in this copy of the port)

    def test_il2cpp_arm64_route(self):
        # the 64-bit route: offered by the check, its player defaults, a launcher without the 32-bit
        # userland, and a port.json PortMaster offers to 64-bit-only systems
        with tempfile.TemporaryDirectory() as t:
            game = os.path.join(t, 'src', 'My Game')
            fake_game(game, version='2019.4.8f1', name='My Game')
            rep = P.check(game, asmpatch='/nonexistent-asmpatch')
            r = next(x for x in rep['routes'] if x['route'] == 'il2cpp-arm64')
            self.assertTrue(r['ok'] and r['available'], r)
            self.assertEqual(rep['route'], 'android')           # (still the recommended one)
            _, out = self.make(t, {'port': {'route': 'il2cpp-arm64'}})
            env = open(os.path.join(out, 'mygame', 'setup', 'setup.env')).read()
            self.assertIn('PORT_ROUTE=il2cpp-arm64', env)
            self.assertIn('PLAYER_BACKEND=il2cpp', env)
            self.assertIn('PLAYER_ABI=arm64-v8a', env)
            launcher = open(os.path.join(out, 'My Game.sh')).read()
            self.assertNotIn('lib32', launcher)
            self.assertNotIn('SDL_AUDIODRIVER', launcher)
            self.assertIn('uname -m', launcher)
            setup = open(os.path.join(out, 'mygame', 'setup', 'patchscript')).read()
            self.assertIn('--on-device', setup)
            self.assertIn('--il2cpp "$W/il2cpp"', setup)
            self.assertNotIn('MonoLibs', setup)
            for f in ('My Game.sh', 'mygame/setup/patchscript'):
                subprocess.run(['bash', '-n', os.path.join(out, f)], check=True)
            pj = json.load(open(os.path.join(out, 'mygame', 'port.json')))
            self.assertNotIn('armhf', pj['attr']['reqs'])
            self.assertIn('IL2CPP', pj['attr']['inst'])
            readme = P._read(os.path.join(out, 'README.md'))
            self.assertIn('64-bit (aarch64) handheld', readme)
            self.assertIn("Unity's IL2CPP on the handheld", readme)
            self.assertIn('ARCH=arm64 runtime/build.sh', readme)
            self.assertNotIn('armhf', readme)
            self.assertIn('Needs a 64-bit (aarch64) handheld.', P._read(os.path.join(out, 'testing_thread.txt')))
            # (gameinfo.xml takes the route's description when port.toml has none)
            self.assertIn("<desc>My Game on Unity's 64-bit Android player", P._read(os.path.join(out, 'mygame', 'gameinfo.xml')))
            # a player set in port.toml wins over the route's default
            _, out = self.make(t, {'port': {'route': 'il2cpp-arm64'}, 'player': {'abi': 'armeabi-v7a'}})
            self.assertIn('PLAYER_ABI=armeabi-v7a', open(os.path.join(out, 'mygame', 'setup', 'setup.env')).read())

    def test_mono_arm64_route(self):
        # Mono on the 64-bit player: offered by the check for a .NET 4.x Mono build (not recommended
        # over the others), the IL2CPP player with its engine modules, android-data --mono64, the
        # release's Mono runtime of the game's line installed as lib/libil2cpp.so and beside it
        with tempfile.TemporaryDirectory() as t:
            game = os.path.join(t, 'src', 'My Game')
            fake_game(game, version='2019.4.8f1', name='My Game')
            rep = P.check(game, asmpatch='/nonexistent-asmpatch')
            r = next(x for x in rep['routes'] if x['route'] == 'mono-arm64')
            self.assertTrue(r['ok'] and r['available'], r)
            self.assertEqual(rep['route'], 'android')
            _, out = self.make(t, {'port': {'route': 'mono-arm64'}})
            env = open(os.path.join(out, 'mygame', 'setup', 'setup.env')).read()
            self.assertIn('PORT_ROUTE=mono-arm64', env)
            self.assertIn('PLAYER_BACKEND=il2cpp', env)
            self.assertIn('PLAYER_ABI=arm64-v8a', env)
            launcher = open(os.path.join(out, 'My Game.sh')).read()
            self.assertNotIn('lib32', launcher)
            self.assertIn('uname -m', launcher)
            setup = open(os.path.join(out, 'mygame', 'setup', 'patchscript')).read()
            self.assertIn('--abi "$PLAYER_ABI" --managed', setup)
            self.assertIn('--mono64', setup)
            self.assertIn('$S/mono64/$line', setup)
            self.assertNotIn('--on-device', setup)
            self.assertNotIn('MonoLibs', setup)
            for f in ('My Game.sh', 'mygame/setup/patchscript'):
                subprocess.run(['bash', '-n', os.path.join(out, f)], check=True)
            pj = json.load(open(os.path.join(out, 'mygame', 'port.json')))
            self.assertNotIn('armhf', pj['attr']['reqs'])
            self.assertIn("Unity's Mono", pj['attr']['desc'])
            self.assertIn('mono64', json.load(open(os.path.join(P.TEMPLATES, 'mono-arm64', 'route.json')))['package'])
            self.assertEqual(P.mono64_lines({'game': {'unity_versions': ['2019.4.8f1', '2019.4.40f1', '6000.0.61f1']}}),
                             ['2019.4', '6000.0'])

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
            P.write_png(os.path.join(out, 'screenshot.png'), 640, 480, bytes(640 * 480 * 3))   # (a stand-in capture)
            msgs = []
            z = P.package(out, os.path.join(t, 'rel'), rt, log=msgs.append, pybundle=pyb, no_build=True, submission=True)
            with zipfile.ZipFile(z) as zf:
                names = set(zf.namelist())
                modes = {i.filename: i.external_attr >> 16 & 0o777 for i in zf.infolist()}
                lic = zf.read('mygame/licenses/LICENSE.setup-python.txt').decode()
                self.assertEqual(zf.read('mygame/screenshot.png'), P._read(os.path.join(out, 'screenshot.png'), 'rb'))
            # PortMaster's files: gameinfo.xml and the screenshot in the port folder, the testing
            # thread's post beside the zip only; what is still missing is listed
            self.assertIn('mygame/gameinfo.xml', names)
            self.assertNotIn('mygame/cover.png', names)
            self.assertFalse(any('testing_thread' in n for n in names))
            self.assertTrue(os.path.isfile(os.path.join(t, 'rel', 'testing_thread.txt')))
            text = '\n'.join(msgs)
            self.assertIn('no cover', text)
            self.assertNotIn('no screenshot', text)
            self.assertIn('[controls] is empty', text)
            self.assertNotIn('standard lines', text)
            # --submission: the layout of PortMaster's repository (ports/mygame/ there)
            sub = os.path.join(t, 'rel', 'submission', 'mygame')
            top = sorted(os.listdir(sub))
            self.assertEqual(top, ['My Game.sh', 'README.md', 'gameinfo.xml', 'mygame', 'port.json', 'screenshot.png'])
            self.assertTrue(P._read(os.path.join(sub, 'README.md')).startswith('## Notes'))
            self.assertTrue(os.access(os.path.join(sub, 'My Game.sh'), os.X_OK))
            inner = set(os.listdir(os.path.join(sub, 'mygame')))
            self.assertFalse(inner & {'port.json', 'gameinfo.xml', 'screenshot.png'})
            self.assertTrue({'ua_host', 'setup', 'licenses', 'README.md'} <= inner)
            self.assertTrue(os.access(os.path.join(sub, 'mygame', 'ua_host'), os.X_OK))
            for n in ('My Game.sh', 'mygame/ua_host', 'mygame/setup/python.tar.gz', 'mygame/setup/pmunity/__main__.py',
                      'mygame/setup/pmunity/shaders/glsl.py', 'mygame/setup/pmunity/video_decode_android.shader',
                      'mygame/setup/pmunity/il2cpp_device_lock.py', 'mygame/setup/tools/il2cpp/ndk-clang.sh',
                      'mygame/licenses/LICENSE.libvpx.txt', 'mygame/licenses/LICENSE.portmaster-unity.txt',
                      'mygame/licenses/LICENSE.libhybris-Apache-2.0.txt', 'mygame/setup/patchscript'):
                self.assertIn(n, names)
            self.assertFalse(any('port_template' in n or '__pycache__' in n for n in names))
            self.assertNotIn('mygame/setup/asmpatch', names)        # (the port does not use it)
            self.assertNotIn('mygame/setup/pmhlslcc', names)        # (nor Direct3D 11 shaders)
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
        # (not the caller's PMHLSLCC: the setup sets it only for a port that ships pmhlslcc)
        e = dict({k: v for k, v in os.environ.items() if k != 'PMHLSLCC'}, GAMEDIR=g, controlfolder=pm,
                 DEVICE_ARCH='aarch64', STUB_LOG=log, STUB_RUNTIME='.NET 4.x (libmonobdwgc-2.0)',
                 STUB_VERSION='2019.4.8f1', STUB_FAIL='')
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

    def test_setup_il2cpp_arm64(self):
        """the il2cpp-arm64 setup: what changes the game's code (the port's assembly patches, asmpatch with
        the opt-in patches for StreamingAssets and dataPath read with System.IO) goes to the IL2CPP build,
        and android-data packs the same StreamingAssets and dataPath modes"""
        with tempfile.TemporaryDirectory() as t:
            put(os.path.join(t, 'mypatch.sh'), '#!/bin/sh\n')
            game, out = self.make(t, {'port': {'route': 'il2cpp-arm64'},
                                      'android_data': {'streaming_assets': 'both', 'data_path': True},
                                      'files': {'assembly_patches': [{'name': 'Assembly-CSharp.dll', 'files': ['mypatch.sh'],
                                                                      'command': 'sh {setup}/files/mypatch.sh {managed} {out}'}]}})
            self.assertIn('asmpatch?', json.load(open(os.path.join(P.TEMPLATES, 'il2cpp-arm64', 'route.json')))['package'])
            self.assertTrue(P.needs_asmpatch(json.load(open(os.path.join(out, 'port.build.json')))['config']))
            g = os.path.join(t, 'ports', 'mygame')
            shutil.copytree(os.path.join(out, 'mygame'), g)
            shutil.copytree(game, os.path.join(g, 'gamedata', 'My Game'))
            put(os.path.join(g, 'setup', 'asmpatch'), '#!/bin/sh\n')
            g, r, log = self.run_setup(t, out, {'PATH': roomy_path(t)})
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            il = next(l for l in log.splitlines() if l.startswith('pm il2cpp '))
            S = os.path.join(g, 'setup')
            self.assertIn(f"--assembly-patch Assembly-CSharp.dll sh {S}/files/mypatch.sh {{managed}} {{out}}", il)
            self.assertIn(f"--streaming-assets both --data-path --asmpatch '{S}/asmpatch' --only=", il)
            ad = next(l for l in log.splitlines() if l.startswith('pm android-data '))
            self.assertIn('--streaming-assets both --data-path', ad)
            for o in ('--asmpatch', '--managed-file', '--assembly-patch'):
                self.assertNotIn(o, ad)
            self.assertEqual(sorted(os.listdir(os.path.join(g, 'lib'))), ['libil2cpp.so', 'libmain.so', 'libunity.so'])
            self.assertTrue(os.path.isfile(os.path.join(g, '.setup_done')))
            # asmpatch = "auto" and the modes left at obb: asmpatch's own choice of patches, nothing opt-in
            shutil.rmtree(g)
            game, out = self.make(t, {'port': {'route': 'il2cpp-arm64'}, 'android_data': {'asmpatch': 'auto'}})
            shutil.copytree(os.path.join(out, 'mygame'), g)
            shutil.copytree(game, os.path.join(g, 'gamedata', 'My Game'))
            put(os.path.join(g, 'setup', 'asmpatch'), '#!/bin/sh\n')
            os.unlink(os.path.join(t, 'pm.log'))
            g, r, log = self.run_setup(t, out, {'PATH': roomy_path(t)})
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            il = next(l for l in log.splitlines() if l.startswith('pm il2cpp '))
            self.assertTrue(il.endswith(f"--streaming-assets obb --asmpatch '{os.path.join(g, 'setup', 'asmpatch')}'"), il)


@unittest.skipUnless(shutil.which('bash'), 'needs bash')
class ApkRouteTest(unittest.TestCase):
    """The apk route: the check of an Android build, the port it makes and ships, and its setup run
    with the real `pmunity apk` (the setup's Python is this one)."""

    def apk(self, t, sub='src', name='game.apk', abis=('arm64-v8a', 'armeabi-v7a'), backend='il2cpp', obb=False,
            pkg='com.test.game', extra=None):
        d = os.path.join(t, sub)
        os.makedirs(d, exist_ok=True)
        a = make_apk(os.path.join(d, name), apk_manifest(pkg=pkg, meta=(('unity.build-id', BID),)), abis=abis,
                     backend=backend, extra=extra)
        if obb:
            make_obb(os.path.join(d, os.path.splitext(name)[0] + '.main.obb'), BID)
        return a

    def runtime(self, t):
        rt = os.path.join(t, 'rt')
        for arch in ('', 'arm64/'):
            for d in (f'runtime/host/out/{arch}', f'runtime/media/out/{arch}licenses'):
                os.makedirs(os.path.join(rt, d), exist_ok=True)
            put(os.path.join(rt, f'runtime/host/out/{arch}ua_host'), f'host {arch}')
            put(os.path.join(rt, f'runtime/media/out/{arch}licenses/libvpx.copyright'), f'BSD {arch}')
        pyb = os.path.join(t, 'py.tar.gz')
        with tarfile.open(pyb, 'w:gz') as tf:
            ti = tarfile.TarInfo('python/lib/python3.12/LICENSE.txt')
            ti.size = 3
            tf.addfile(ti, io.BytesIO(b'PSF'))
        return rt, pyb

    def test_check(self):
        with tempfile.TemporaryDirectory() as t:
            self.apk(t, obb=True)
            rep = P.check(os.path.join(t, 'src'))
            self.assertEqual(rep['build'], 'android')
            self.assertEqual(rep['route'], 'apk')
            r = rep['routes'][0]
            self.assertTrue(r['ok'] and r['available'], r)
            self.assertIn('ships both', ' '.join(r['needs']))
            self.assertEqual(rep['unity_version'], '2021.3.4f1')
            self.assertEqual(rep['apk_info']['obbs'], {'main': 'main.7.com.test.game.obb'})
            self.assertEqual(rep['suggest']['player'], {'backend': 'il2cpp', 'abis': ['arm64-v8a', 'armeabi-v7a']})
            self.assertEqual(rep['suggest']['game'], {'package': 'com.test.game'})
            out = io.StringIO()
            P.print_report(rep, out)
            self.assertIn('package com.test.game, versionCode 7', out.getvalue())
            self.assertIn('recommended route: apk', out.getvalue())
            # a Mono build that reads only touches: pointer mode with touch events
            dll = os.path.join(t, 'Assembly-CSharp.dll')
            write_assembly(dll, 'Assembly-CSharp', [('', 'Game')], [('UnityEngine', 'Input')], ['Update'],
                           [(1, 'GetTouch'), (1, 'get_touchCount')])
            m = self.apk(t, 'mono', 'mono.apk', abis=('armeabi-v7a',), backend='mono',
                         extra={'assets/bin/Data/Managed/Assembly-CSharp.dll': P._read(dll, 'rb')})
            rep = P.check(m)
            self.assertEqual(rep['route'], 'apk')
            self.assertEqual(rep['suggest']['runtime'], {'UA_POINTER': 'touch'})
            self.assertIn('armeabi-v7a only', ' '.join(rep['routes'][0]['needs']))
            # refused, with the reason: x86 only
            x = self.apk(t, 'x86', 'x.apk', abis=('x86',))
            rep = P.check(x)
            self.assertIsNone(rep['route'])
            self.assertIn('x86', rep['unsupported'])
            # nothing Android: no game
            os.makedirs(os.path.join(t, 'empty'))
            with self.assertRaises(SystemExit):
                P.check(os.path.join(t, 'empty'))

    def test_new_and_package(self):
        with tempfile.TemporaryDirectory() as t:
            a = self.apk(t)
            out = P.new('mygame', a, None, os.path.join(t, 'port'), title='My Game', log=lambda m: None)
            files = set(P._read(os.path.join(out, '.port_files')).split('\n')) - {''}
            for f in ('My Game.sh', 'mygame/setup/patchscript', 'mygame/setup/setup.env',
                      'mygame/port.json', 'mygame/README.md', 'mygame/gamedata/PUT_GAME_FILES_HERE.txt', 'README.md',
                      'mygame/gameinfo.xml', 'testing_thread.txt'):
                self.assertIn(f, files)
            self.assertNotIn('mygame/setup/convert.json', files)
            for f in files:
                if f != 'port.build.json':
                    self.assertNotIn('@@', P._read(os.path.join(out, f)), f)
                if f.endswith(('.sh', 'patchscript')):
                    subprocess.run(['bash', '-n', os.path.join(out, f)], check=True)
            env = P._read(os.path.join(out, 'mygame', 'setup', 'setup.env'))
            for x in ('PORT_ROUTE=apk', 'GAME_PACKAGE=com.test.game', 'PLAYER_ABI=auto', 'GAME_BUILD=android',
                      "GAME_DATA='*.apk *.xapk *.apks *.aab *.zip'", 'UNITY_VERSIONS=2021.3.4f1'):
                self.assertIn(x, env)
            self.assertNotIn('PACKAGE=com.pmunity', env)
            toml = tomllib.loads(P._read(os.path.join(out, 'port.toml')))
            self.assertNotIn('convert', toml)
            self.assertNotIn('android_data', toml)
            self.assertEqual(toml['player']['abis'], ['arm64-v8a', 'armeabi-v7a'])
            launcher = P._read(os.path.join(out, 'My Game.sh'))
            self.assertEqual(P.launcher_warnings(launcher), [])
            for x in ('./ua_host64 "$GAMEDIR"', '/lib/ld-linux-armhf.so.3', '"abi"'):
                self.assertIn(x, launcher)
            self.assertNotRegex(launcher, r'(?m)^\s*(export\s+)?(SDL_AUDIODRIVER|UA_FRAME_RATE)=')
            pj = json.loads(P._read(os.path.join(out, 'mygame', 'port.json')))
            self.assertNotIn('armhf', pj['attr']['reqs'])
            self.assertIn('nothing is downloaded', pj['attr']['inst'])
            self.assertNotIn("downloads Unity's Android player", P._read(os.path.join(out, 'README.md')))
            self.assertIn('ARCH=arm64 runtime/build.sh', P._read(os.path.join(out, 'README.md')))
            # the release: both runtimes, each with its licences
            rt, pyb = self.runtime(t)
            z = P.package(out, os.path.join(t, 'rel'), rt, log=lambda m: None, pybundle=pyb, no_build=True)
            with zipfile.ZipFile(z) as zf:
                names = set(zf.namelist())
                self.assertEqual(zf.read('mygame/ua_host64'), b'host arm64/')
                self.assertEqual(zf.read('mygame/licenses/LICENSE.libvpx.ua_host64.txt'), b'BSD arm64/')
            for n in ('mygame/ua_host', 'mygame/licenses/LICENSE.libvpx.txt', 'mygame/setup/python.tar.gz',
                      'mygame/setup/pmunity/apk.py'):
                self.assertIn(n, names)
            # port new again from port.toml alone (the README's Compile section)
            P.new('mygame', None, None, out, log=lambda m: None)
            self.assertEqual(P._read(os.path.join(out, 'mygame', 'setup', 'setup.env')), env)
            # an armeabi-v7a-only build: a 32-bit userland (port.json), ua_host alone
            v7 = self.apk(t, 'v7', 'v7.apk', abis=('armeabi-v7a',))
            out2 = P.new('v7game', v7, None, os.path.join(t, 'port2'), log=lambda m: None)
            pj = json.loads(P._read(os.path.join(out2, 'v7game', 'port.json')))
            self.assertEqual(pj['attr']['reqs'], ['armhf'])
            self.assertNotIn('ARCH=arm64', P._read(os.path.join(out2, 'README.md')))
            z = P.package(out2, os.path.join(t, 'rel2'), rt, log=lambda m: None, pybundle=pyb, no_build=True)
            with zipfile.ZipFile(z) as zf:
                names = set(zf.namelist())
            self.assertIn('v7game/ua_host', names)
            self.assertNotIn('v7game/ua_host64', names)

    def setup_port(self, t, data, files=(), hosts=('ua_host', 'ua_host64'), cfg=None, reinstall=False):
        """A port made from the build `data`, installed as ports/mygame (over the installed one: as
        PortMaster installs it again) with `files` in gamedata/ and the given runtimes; the setup's
        Python is this one, pmunity this checkout's."""
        cp = os.path.join(t, 'cfg.json')
        with open(cp, 'w') as f:
            json.dump(P.merge({'port': {'title': 'My Game'}}, cfg or {}), f)
        out = P.new('mygame', data, cp, os.path.join(t, 'port'), log=lambda m: None)
        g = os.path.join(t, 'ports', 'mygame')
        if not reinstall:
            shutil.rmtree(g, ignore_errors=True)
        shutil.copytree(os.path.join(out, 'mygame'), g, dirs_exist_ok=True)
        for h in hosts:
            put(os.path.join(g, h), '#!/bin/sh\n')
        shutil.copytree(P.HERE, os.path.join(g, 'setup', 'pmunity'), dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns('__pycache__', 'port_template'))
        stub = os.path.join(t, 'stub', 'python', 'bin', 'python3.12')
        os.makedirs(os.path.dirname(stub), exist_ok=True)
        put(stub, f'#!/bin/sh\nexec {shlex.quote(sys.executable)} "$@"\n')
        os.chmod(stub, 0o755)
        with tarfile.open(os.path.join(g, 'setup', 'python.tar.gz'), 'w:gz') as tf:
            tf.add(os.path.join(t, 'stub', 'python'), 'python')
        for f in files:
            shutil.copy(f, os.path.join(g, 'gamedata'))
        return g

    def run_setup(self, g, env=None):
        e = dict(os.environ, GAMEDIR=g, controlfolder=os.path.join(os.path.dirname(g), 'PortMaster'),
                 DEVICE_ARCH='aarch64', PYTHONPATH='')
        e.update(env or {})
        return subprocess.run(['bash', os.path.join(g, 'setup', 'patchscript')], env=e, capture_output=True, text=True)

    @unittest.skipIf(os.path.exists('/lib/ld-linux-armhf.so.3'), 'a 32-bit userland here: the setup takes armeabi-v7a')
    def test_setup(self):
        with tempfile.TemporaryDirectory() as t:
            a = self.apk(t, obb=True)
            g = self.setup_port(t, a, [a, os.path.join(t, 'src', 'game.main.obb')])
            os.makedirs(os.path.join(g, 'gamedata', 'more'))
            put(os.path.join(g, 'gamedata', '._game.apk'), 'AppleDouble')           # (macOS's, on exFAT: ignored)
            shutil.move(os.path.join(g, 'gamedata', 'game.main.obb'), os.path.join(g, 'gamedata', 'more'))
            r = self.run_setup(g)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('the players this system runs, in this order: arm64', r.stdout)
            self.assertEqual(sorted(os.listdir(os.path.join(g, 'lib'))), ['libil2cpp.so', 'libmain.so', 'libunity.so'])
            self.assertEqual(P._read(os.path.join(g, 'lib', 'libunity.so'), 'rb'), b'\x7fELFarm64-v8a')
            for f in ('base.apk', 'main.7.com.test.game.obb', '.setup_done', 'answers.env'):
                self.assertTrue(os.path.isfile(os.path.join(g, f)), f)
            info = json.loads(P._read(os.path.join(g, 'android_package.json')))
            self.assertEqual((info['abi'], info['source'], info['main_obb']), ('arm64-v8a', 'apk', 'main.7.com.test.game.obb'))
            self.assertEqual(sorted(os.listdir(os.path.join(g, 'gamedata'))), ['._game.apk', 'PUT_GAME_FILES_HERE.txt'])
            self.assertFalse(os.path.exists(os.path.join(g, 'setup')))
            self.assertIn('setup finished', P._read(os.path.join(g, 'patchlog.txt')))
            # the setup again (the port installed anew, the marker deleted): the installed APK and OBB
            os.remove(os.path.join(g, '.setup_done'))
            self.setup_port(t, a, reinstall=True)
            r = self.run_setup(g)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('no package in gamedata/: the installed one again', r.stdout)
            self.assertTrue(os.path.isfile(os.path.join(g, 'main.7.com.test.game.obb')))
            self.assertTrue(os.path.isfile(os.path.join(g, 'base.apk')))

    def test_setup_checks(self):
        with tempfile.TemporaryDirectory() as t:
            a = self.apk(t)
            # another game's package; another Unity version; nothing in gamedata/
            other = self.apk(t, 'other', 'other.apk', pkg='com.other.game')
            g = self.setup_port(t, a, [other])
            r = self.run_setup(g)
            self.assertIn('PATCH_FAIL_MSG:This port is made for My Game (com.test.game); your files are com.other.game',
                          r.stdout)
            g = self.setup_port(t, a, [a], cfg={'game': {'unity_versions': ['2021.3.5f1']}})
            r = self.run_setup(g)
            self.assertIn('your copy was built with Unity 2021.3.4f1', r.stdout)
            g = self.setup_port(t, a)
            r = self.run_setup(g)
            self.assertIn('PATCH_FAIL_MSG:Put the Android build of My Game', r.stdout)
            # refused by pmunity apk, with its reason
            g = self.setup_port(t, a)
            put(os.path.join(g, 'gamedata', 'game.apk'), 'not a zip')
            r = self.run_setup(g)
            self.assertIn("PATCH_FAIL_MSG:The game's files cannot run here:", r.stdout)
            self.assertIn('not a zip archive', r.stdout)
            # the 32-bit player only, on a system without a 32-bit userland: said before anything else
            if not os.path.exists('/lib/ld-linux-armhf.so.3'):
                g = self.setup_port(t, a, [a], hosts=('ua_host',))
                r = self.run_setup(g)
                self.assertIn('needs a 32-bit (armhf) userland', r.stdout)
                self.assertNotIn('unpacking the setup tools', r.stdout)

    @unittest.skipIf(os.path.exists('/lib/ld-linux-armhf.so.3'), 'a 32-bit userland here: the setup takes armeabi-v7a')
    def test_mono_build_il2cpp(self):
        """player.il2cpp: a Mono build (armeabi-v7a only) also on 64-bit-only systems: the release ships
        ua_host64, port.json asks for no 32-bit userland, and the setup there has `pmunity apk --il2cpp
        --on-device` compile its code (pmunity il2cpp stubbed: it fails once, then the rerun goes on)"""
        with tempfile.TemporaryDirectory() as t:
            a = self.apk(t, abis=('armeabi-v7a',), backend='mono',
                         extra={'assets/bin/Data/Managed/Assembly-CSharp.dll': b'MZ', 'assets/bin/Data/boot.config': b''})
            rep = P.check(a)
            self.assertIn('player.il2cpp = true', ' '.join(rep['notes']))
            with self.assertRaises(SystemExit) as cm:          # (an IL2CPP build has nothing to compile)
                cp = os.path.join(t, 'c.json')
                with open(cp, 'w') as f:
                    json.dump({'player': {'il2cpp': True}}, f)
                P.new('il', self.apk(t, 'il', 'il.apk'), cp, os.path.join(t, 'port-il'), log=lambda m: None)
            self.assertIn('player.il2cpp', str(cm.exception))
            cfg = {'player': {'il2cpp': True}}
            g = self.setup_port(t, a, [a], cfg=cfg)
            out = os.path.join(t, 'port')
            self.assertIn('PLAYER_IL2CPP=1', P._read(os.path.join(out, 'mygame', 'setup', 'setup.env')))
            pj = json.loads(P._read(os.path.join(out, 'mygame', 'port.json')))
            self.assertNotIn('armhf', pj['attr']['reqs'])
            self.assertIn("compiles the game's code with Unity's IL2CPP", pj['attr']['inst'])
            self.assertIn('without a 32-bit userland the first launch compiles', P._read(os.path.join(out, 'README.md')))
            self.assertTrue(P.wants('ua_host64', json.load(open(os.path.join(out, 'port.build.json')))['config']))
            log = os.path.join(t, 'il2cpp.log')
            with open(os.path.join(g, 'setup', 'pmunity', 'il2cpp.py'), 'a') as f:
                f.write(f'''

def build(data, out, abi='arm64-v8a', home=None, pkg=None, jobs=None, **kw):   # (the test's stand-in)
    import json as J
    with open({log!r}, 'a') as f:
        f.write(J.dumps([sorted(os.listdir(os.path.join(data, 'Managed'))), out, home, pkg, kw['on_device']]) + '\\n')
    os.makedirs(os.path.join(out, 'partial'), exist_ok=True)
    if not os.path.exists(os.path.join(out, 'partial', 'once')):
        open(os.path.join(out, 'partial', 'once'), 'w').close()
        raise SystemExit('killed while compiling')
    L = os.path.join(pkg, 'Variations', 'il2cpp', 'Release', 'Libs', 'arm64-v8a')
    os.makedirs(L, exist_ok=True)
    for n in ('libmain.so', 'libunity.so'):
        open(os.path.join(L, n), 'w').write('player')
    for rel in ('lib/arm64-v8a/libil2cpp.so', 'Data/Managed/Metadata/global-metadata.dat'):
        os.makedirs(os.path.dirname(os.path.join(out, rel)), exist_ok=True)
        open(os.path.join(out, rel), 'wb').write(struct.pack('<II', 0xFAB11BAF, 29))
    return {{'unity': '2021.3.4f1', 'metadata_version': 29, 'libil2cpp_bytes': 8}}
''')
            r = self.run_setup(g, {'PATH': roomy_path(t)})
            self.assertIn("PATCH_FAIL_MSG:Compiling the game's code failed", r.stdout, r.stdout + r.stderr)
            self.assertTrue(os.path.isdir(os.path.join(g, 'gamedata', '.setup', 'apk', '.il2cpp', 'partial')))
            r = self.run_setup(g, {'PATH': roomy_path(t)})
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('the players this system runs, in this order: arm64', r.stdout)
            calls = [json.loads(x) for x in P._read(log).splitlines()]
            self.assertEqual(len(calls), 2)
            W = os.path.join(g, 'gamedata', '.setup')
            self.assertEqual(calls[1], [['Assembly-CSharp.dll'], os.path.join(W, 'apk', '.il2cpp'), os.path.join(W, 'il2cpp-home'),
                                        os.path.join(W, 'il2cpp-home', '2021.3.4f1', 'player-arm64-v8a'), True])
            self.assertEqual(sorted(os.listdir(os.path.join(g, 'lib'))), ['libil2cpp.so', 'libmain.so', 'libunity.so'])
            info = json.loads(P._read(os.path.join(g, 'android_package.json')))
            self.assertEqual((info['abi'], info['backend'], info['il2cpp']['from']), ('arm64-v8a', 'il2cpp', 'mono'))
            self.assertFalse(os.path.exists(W))

    def test_choose_abi_preferences(self):
        from pmunity import apk as A
        have = ['arm64-v8a', 'armeabi-v7a']
        self.assertEqual(A.choose_abi(have, 'armhf,arm64', None, print)[0], 'armeabi-v7a')
        self.assertEqual(A.choose_abi(have, 'arm64,armhf', None, print)[0], 'arm64-v8a')
        self.assertEqual(A.choose_abi(['arm64-v8a'], 'armhf,arm64', None, print)[0], 'arm64-v8a')
        with self.assertRaises(A.Refused):
            A.choose_abi(['armeabi-v7a'], 'arm64', None, print)
        with self.assertRaises(A.Refused):
            A.choose_abi(have, 'auto,arm64', None, print)


class RoutesTest(unittest.TestCase):
    def test_templates(self):
        rts = P.routes()
        self.assertIn('android', rts)
        for r in rts.values():
            for dst, spec in r['files'].items():
                P.template_path(r, spec if isinstance(spec, str) else spec['template'])
        with self.assertRaises(SystemExit):
            P.render('@@NOPE@@', {})




class PortMasterMetadataTest(unittest.TestCase):
    """port.json's fields, gameinfo.xml, the README's controls, the testing thread, the screenshot
    checks and the launcher's standard lines (PortMaster's submission files)."""

    def cfg(self, **over):
        cfg = P.build_config('mygame', None, P.merge({'game': {'unity_versions': ['2020.3.1f1']}}, over))
        P.validate(cfg)
        return cfg

    def test_port_json(self):
        cfg = self.cfg(port={'title': 'My Game', 'porter': 'someone', 'genres': ['Platformer', 'visual novel'],
                             'desc_md': '**bold**', 'reqs': ['2gb', '!lowres'],
                             'store': [{'name': 'Steam', 'gameurl': 'https://example.com/app/1'}]})
        a = P.port_json(cfg)['attr']
        self.assertEqual(list(a), ['title', 'porter', 'desc', 'desc_md', 'inst', 'inst_md', 'genres', 'image', 'rtr',
                                   'exp', 'runtime', 'store', 'availability', 'reqs', 'arch', 'min_glibc'])
        self.assertEqual(a['porter'], ['someone'])
        self.assertEqual(a['genres'], ['platformer', 'visual novel'])
        self.assertEqual((a['desc_md'], a['inst_md']), ('**bold**', None))
        self.assertEqual(a['store'], [{'name': 'Steam', 'gameurl': 'https://example.com/app/1', 'developerurl': ''}])
        self.assertEqual(a['availability'], 'paid')                 # (a store sells it)
        self.assertEqual(a['reqs'], ['armhf', '2gb', '!lowres'])
        self.assertIs(a['exp'], False)
        self.assertIs(a['rtr'], False)
        self.assertEqual(self.cfg()['port']['availability'], 'free')
        self.assertEqual(self.cfg(port={'availability': 'demo'})['port']['availability'], 'demo')

    def test_validate_metadata(self):
        bad = P.build_config('g', None, {'game': {'unity_versions': ['2020.3.1f1'], 'release_date': '29/09/2017'},
                                         'port': {'genres': ['shooter'], 'availability': 'maybe',
                                                  'store': ['https://example.com'], 'reqs': ['Not Lowres'],
                                                  'screenshot': 'shot.bmp'},
                                         'controls': {'A': ''}})
        with self.assertRaises(SystemExit) as cm:
            P.validate(bad)
        msg = str(cm.exception)
        for part in ("'shooter'", 'port.availability', 'port.store', "'Not Lowres'", 'release_date', 'port.screenshot',
                     'controls.A'):
            self.assertIn(part, msg)

    def test_gameinfo(self):
        import xml.etree.ElementTree as ET
        cfg = self.cfg(port={'title': 'My Game', 'genres': ['rpg'], 'desc': 'Short.'},
                       game={'developer': 'Studio & Co', 'release_date': '2017-09-29', 'description': 'A <long> one.'})
        xml = P.gameinfo_xml(cfg, 'cover.png')
        g = ET.fromstring(xml).find('game')
        self.assertEqual([c.tag for c in g], ['path', 'name', 'desc', 'releasedate', 'developer', 'genre', 'image'])
        self.assertEqual(g.findtext('path'), './My Game.sh')
        self.assertEqual(g.findtext('desc'), 'A <long> one.')
        self.assertEqual(g.findtext('releasedate'), '20170929T000000')
        self.assertEqual(g.findtext('developer'), 'Studio & Co')
        self.assertEqual(g.findtext('genre'), 'RPG')
        self.assertEqual(g.findtext('image'), './mygame/cover.png')
        self.assertTrue(xml.startswith('<?xml version="1.0" encoding="utf-8"?>\n<gameList>'))
        # the image: the cover when the port folder has one, else the screenshot
        with tempfile.TemporaryDirectory() as t:
            self.assertEqual(P.gameinfo_image(cfg, t), 'screenshot.png')
            P.write_png(os.path.join(t, 'cover.png'), 4, 3, bytes(36))
            self.assertEqual(P.gameinfo_image(cfg, t), 'cover.png')

    def test_controls_and_thanks(self):
        cfg = self.cfg(port={'title': 'G', 'store': [{'name': 'itch.io', 'gameurl': 'https://x.example/g',
                                                      'developerurl': 'https://x.example'}]},
                       game={'developer': 'X'}, controls={'D-Pad': 'Move', 'B': 'Jump | climb'})
        self.assertEqual(P.controls_rows(cfg), [('D-Pad', 'Move'), ('B', 'Jump | climb'), ('Select + Start', 'Quit')])
        md = P.controls_markdown(cfg)
        self.assertTrue(md.startswith('| Button | Action |\n|--|--|\n| D-Pad | Move |'))
        self.assertIn('| B | Jump \\| climb |', md)
        self.assertEqual(P.thanks_sentence(cfg), 'Thanks to [X](https://x.example) for creating G.')
        cfg['controls']['Select + Start'] = 'Back to the menu'
        self.assertEqual(P.controls_rows(cfg)[-1], ('Select + Start', 'Back to the menu'))
        self.assertIn("keeps its own gamepad controls", P.controls_markdown(self.cfg()))
        self.assertEqual(P.thanks_sentence(self.cfg(port={'thanks': 'Thanks!'})), 'Thanks!')

    def test_testing_thread(self):
        cfg = self.cfg(port={'title': 'My Game'}, game={'url': 'https://example.com/g'}, controls={'A': 'Jump'})
        tt = P.testing_thread_txt(cfg, 'Requires the game.')
        for part in ('mygame.zip', 'harbourmaster install', 'Title: My Game', 'URL: https://example.com/g',
                     'Instructions:\nRequires the game.\nNeeds a handheld with a 32-bit (armhf) userland',
                     '| A | Jump |', '| Select + Start | Quit |', 'ROCKNIX\n-> [ ] Libmali', '[ ] Knulli',
                     '[ ] 640x480'):
            self.assertIn(part, tt)
        self.assertNotIn('\u2014', tt)

    def test_images(self):
        with tempfile.TemporaryDirectory() as t:
            png = os.path.join(t, 'a.png')
            P.write_png(png, 3, 2, bytes(range(18)))
            self.assertEqual(P.image_size(png), (3, 2))
            import zlib
            d = P._read(png, 'rb')
            idat = d[d.index(b'IDAT') + 4:d.index(b'IEND') - 8]
            self.assertEqual(zlib.decompress(idat), b'\0' + bytes(range(9)) + b'\0' + bytes(range(9, 18)))
            jpg = os.path.join(t, 'b.jpg')
            put(jpg, b'\xff\xd8\xff\xe0\x00\x04xx\xff\xc0\x00\x11\x08\x01\xe0\x02\x80\x03' + bytes(20))
            self.assertEqual(P.image_size(jpg), (640, 480))
            self.assertIsNone(P.image_size(os.path.join(t, 'none.png')))
            # the screenshot checks: there, PNG or JPEG, 4:3 and at least 640x480
            cfg = self.cfg(port={'screenshot': 'b.jpg'})
            self.assertEqual(P.port_images(cfg, t), {'screenshot': (jpg, 'screenshot.jpg'), 'cover': None})
            w = '\n'.join(P.submission_warnings(cfg, t))
            self.assertNotIn('no screenshot', w)
            self.assertNotIn('the screenshot is', w)
            self.assertIn('no cover', w)
            P.write_png(os.path.join(t, 'screenshot.png'), 720, 480, bytes(720 * 480 * 3))
            self.assertIn('the screenshot is 720x480', '\n'.join(P.submission_warnings(self.cfg(), t)))

    def test_submission_warnings(self):
        cfg = P.build_config('my_game', None, {'game': {'unity_versions': ['2020.3.1f1']},
                                               'port': {'desc': 'Fast \u2014 and **bold**.', 'exp': True}})
        P.validate(cfg)
        with tempfile.TemporaryDirectory() as t:
            P.write_png(os.path.join(t, 'screenshot.png'), 640, 480, bytes(640 * 480 * 3))
            gi = P.gameinfo_xml(cfg, 'cover.png')
            w = '\n'.join(P.submission_warnings(cfg, t, P.port_json(cfg), gameinfo=gi))
        for part in ('port.porter is empty', 'port.json desc: plain text', 'an em dash', 'port.exp = true',
                     "port.name 'my_game'", 'game.publisher is empty', 'gameinfo.xml names cover.png'):
            self.assertIn(part, w)
        self.assertNotIn('port.desc is empty', w)
        self.assertNotIn('screenshot', w.replace('shows the screenshot', ''))

    def test_launchers(self):
        # every route's launcher starts with PortMaster's standard lines (the PORTMASTER line allowed)
        for name, r in P.routes().items():
            spec = r['files'].get('@@TITLE@@.sh')
            if spec:
                text = P._read(P.template_path(r, spec if isinstance(spec, str) else spec['template']))
                self.assertEqual(P.launcher_warnings(text), [], name)
        bad = P._read(P.template_path(P.routes()['android'], 'launcher.sh'))
        bad = bad.replace('\nget_controls\n', '\n').replace('$GPTOKEYB2 "ua_host"', '"$GPTOKEYB2" "ua_host"')
        bad = bad.replace('pm_finish', 'true') + 'export SDL_VIDEODRIVER=kmsdrm\n'
        w = '\n'.join(P.launcher_warnings(bad))
        for part in ('standard lines', 'quoted', 'SDL_VIDEODRIVER', 'pm_finish'):
            self.assertIn(part, w)

    def test_toml_round_trip(self):
        cfg = self.cfg(port={'store': [{'name': 'GOG', 'gameurl': 'https://example.com', 'developerurl': ''}]},
                       controls={'D-Pad': 'Move', 'L1 + R1': 'Map'})
        back = tomllib.loads(P.dump_toml(cfg))
        self.assertEqual(P.merge(P.DEFAULTS, back), cfg)
        self.assertEqual(list(back['controls']), ['D-Pad', 'L1 + R1'])

    def test_shot(self):
        import threading
        with tempfile.TemporaryDirectory() as t:
            px = bytes(range(256)) * 9                              # 32x24, RGB
            ppm = os.path.join(t, 'shot-1.ppm')
            put(ppm, b'P6\n# a comment\n32 24\n255\n' + px)
            out = P.shot(ppm, os.path.join(t, 'a.png'), log=lambda m: None)
            self.assertEqual(P.image_size(out), (32, 24))
            # a folder ua_host runs a game in: shot.req asks for the next frame
            def host():
                req = os.path.join(t, 'shot.req')
                for _ in range(100):
                    if os.path.exists(req):
                        os.remove(req)
                        put(os.path.join(t, 'shot-2.ppm'), b'P6\n4 3\n255\n' + bytes(36))
                        return
                    time.sleep(0.05)
            th = threading.Thread(target=host)
            th.start()
            msgs = []
            out = P.shot(t, os.path.join(t, 'b.png'), timeout=10, log=msgs.append)
            th.join()
            self.assertEqual(P.image_size(out), (4, 3))
            self.assertIn('from ' + os.path.join(t, 'shot-2.ppm'), msgs[0])
            self.assertIn('4:3, at least 640x480', msgs[0])
            put(os.path.join(t, 'c.ppm'), b'P5\n1 1\n255\n\0')
            with self.assertRaises(SystemExit):
                P.shot(os.path.join(t, 'c.ppm'), os.path.join(t, 'c.png'))


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
