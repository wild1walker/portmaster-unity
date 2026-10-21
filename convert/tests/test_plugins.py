"""Tests of the plugin survey (pmunity/plugins.py, clrmeta.py): names and SDKs, argument classes for
the softfp thunks, the plugin map and plugins.map, ELF facts of plugin files, FMOD versions.
    python -m unittest tests.test_plugins   (from convert/)
Opt-in on games' data (local paths; nothing of it is in the repository):
    PMUNITY_GAMES=<Game>_Data[:...] python -m unittest tests.test_plugins -v
reads every assembly's P/Invokes and checks the report and map are well formed."""
import os
import re
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from pmunity import clrmeta as C
from pmunity import plugins as P

GAMES = [p for p in os.environ.get('PMUNITY_GAMES', '').split(os.pathsep) if p]


class FakeAsm:
    """typedef_layout for a few hand-made types (row -> (base, fields, size, flags))."""
    def __init__(self, types):
        self.t = types

    def typedef_layout(self, row):
        return self.t[row]


class FakeRes:
    def __init__(self, asm):
        self.asm = asm

    def find(self, asm, tab, row):
        return (self.asm, row) if tab == C.TYPEDEF else (None, row)


def prim(e):
    return ('prim', e)


class Names(unittest.TestCase):
    def test_norm(self):
        self.assertEqual(P.norm('libfmodstudio.so.13'), 'fmodstudio')
        self.assertEqual(P.norm('Plugins/x86_64/libsteam_api.so'), 'steam_api')
        self.assertEqual(P.norm('steam_api64.dll'), 'steam_api64')
        self.assertEqual(P.norm('discord_game_sdk.bundle'), 'discord_game_sdk')

    def test_sdks(self):
        for name, sdk in [('fmodstudio', 'fmod'), ('fmodstudioL', 'fmod'), ('fmod', 'fmod'), ('steam_api64', 'steamworks'),
                          ('libsteam_api', 'steamworks'), ('CSteamworks', 'csteamworks'), ('discord_game_sdk', 'discord-game-sdk'),
                          ('discord-rpc', 'discord-rpc'), ('GalaxyCSharpGlue', 'galaxy'), ('EOSSDK-Win64-Shipping', 'eos'),
                          ('AkSoundEngine', 'wwise'), ('sqlite3', 'sqlite'), ('e_sqlite3', 'sqlite'), ('libSDL2-2.0.so.0', 'sdl2')]:
            self.assertEqual(P.sdk_of(name)[0], sdk, name)
        self.assertIsNone(P.sdk_of('MyGamePlugin'))
        self.assertTrue(P.SYSTEM.match('libc') and P.SYSTEM.match('__Internal') and P.WINDOWS.search('kernel32.dll'))

    def test_fmod_version(self):
        self.assertEqual(P.fmod_version_str(0x00020223), '2.02.23')
        self.assertEqual(P.fmod_version_str(0x00011013), '1.10.13')   # BCD digits, as FMOD prints them


class ArgumentClasses(unittest.TestCase):
    def setUp(self):
        TD = C.TYPEDEF
        self.asm = FakeAsm({
            1: ('System.Enum', [('value__', prim(C.E_I4))], None, 0),                       # an enum
            2: ('System.ValueType', [('x', prim(C.E_R4)), ('y', prim(C.E_R4)), ('z', prim(C.E_R4))], None, 0),   # VECTOR
            3: ('System.ValueType', [('data1', prim(C.E_U4)), ('data2', prim(C.E_U4))], None, 0),               # PARAMETER_ID
            4: ('System.ValueType', [('a', prim(C.E_I4)), ('d', prim(C.E_R8))], None, 0),   # mixed, 8-aligned
            5: ('System.ValueType', [('p', ('valuetype', 'V', (TD, 2))), ('v', ('valuetype', 'V', (TD, 2)))], None, 0),  # 6 floats
            6: ('System.Enum', [('value__', prim(C.E_I8))], None, 0),
            7: ('System.ValueType', [('a', prim(C.E_R8)), ('b', prim(C.E_R8))], None, 0),
        })
        self.res = FakeRes(self.asm)

    def tok(self, t):
        return P.abi_token(self.res, self.asm, t)

    def test_prims(self):
        self.assertEqual(self.tok(prim(C.E_VOID)), 'v')
        self.assertEqual(self.tok(prim(C.E_I4)), 'i')
        self.assertEqual(self.tok(prim(C.E_BOOL)), 'i')
        self.assertEqual(self.tok(prim(C.E_R4)), 'f')
        self.assertEqual(self.tok(prim(C.E_R8)), 'd')
        self.assertEqual(self.tok(prim(C.E_I8)), 'l')
        self.assertEqual(self.tok(prim(C.E_STRING)), 'i')
        self.assertEqual(self.tok(('byref', prim(C.E_R4))), 'i')          # out float: a pointer
        self.assertEqual(self.tok(('class', 'Delegate', None)), 'i')

    def test_structs(self):
        TD = C.TYPEDEF
        self.assertEqual(self.tok(('valuetype', 'RESULT', (TD, 1))), 'i')
        self.assertEqual(self.tok(('valuetype', 'LongEnum', (TD, 6))), 'l')
        self.assertEqual(self.tok(('valuetype', 'VECTOR', (TD, 2))), 'F3')
        self.assertEqual(self.tok(('valuetype', 'PARAMETER_ID', (TD, 3))), 's2')
        self.assertEqual(self.tok(('valuetype', 'Mixed', (TD, 4))), 'S4')
        self.assertEqual(self.tok(('valuetype', 'TwoVectors', (TD, 5))), 's6')   # 6 floats: not a VFP candidate
        self.assertEqual(self.tok(('valuetype', 'Two doubles', (TD, 7))), 'D2')
        self.assertEqual(self.tok(('valuetype', 'UnityEngine.Vector3', (C.TYPEREF, 'UnityEngine.Vector3'))), 'F3')
        self.assertEqual(self.tok(('valuetype', 'Somewhere.Unknown', (C.TYPEREF, 'Somewhere.Unknown'))), '?')

    def test_needs_thunk(self):
        self.assertTrue(P.needs_thunk(('i', ['i', 'f'])))
        self.assertTrue(P.needs_thunk(('f', ['i'])))
        self.assertTrue(P.needs_thunk(('i', ['F3'])))
        self.assertFalse(P.needs_thunk(('i', ['i', 's2', 'l'])))


def report(modules, sigs=(), fmod=None):
    rep = {'modules': modules, 'files': [], 'fmod': fmod, 'sigs': list(sigs)}
    rep['map'] = P.default_map(rep)
    return rep


class Map(unittest.TestCase):
    def mods(self):
        return {
            'fmodstudio': {'use': 'lib', 'entries': 3, 'note': ''},
            'steam_api': {'use': 'stub', 'stub': 'steam', 'entries': 900, 'note': ''},
            'sqlite3': {'use': 'host', 'lib': 'libsqlite3.so.0', 'entries': 70, 'note': ''},
            'SDL2': {'use': 'keep', 'entries': 50, 'note': ''},
            'libc': {'use': 'system', 'entries': 2, 'note': ''},
            'GalaxyCSharpGlue': {'use': 'none', 'entries': 800, 'note': ''},
        }

    def test_defaults(self):
        rep = report(self.mods(), sigs=[('fmodstudio', 'FMOD_Studio_VCA_SetVolume', 'i', ['i', 'f']),
                                        ('steam_api', 'SteamAPI_X', 'f', ['i']),
                                        ('sqlite3', 'sqlite3_bind_double', 'i', ['i', 'i', 'd'])])
        txt = P.map_text(rep, rep['map'])
        lines = [l for l in txt.splitlines() if not l.startswith('#')]
        self.assertEqual(lines, ['map fmodstudio lib libfmodstudio.so', 'map GalaxyCSharpGlue none',
                                 'map sqlite3 host libsqlite3.so.0', 'map steam_api stub steam',
                                 'sig fmodstudio FMOD_Studio_VCA_SetVolume i i,f', 'sig sqlite3 sqlite3_bind_double i i,i,d'])
        self.assertNotIn('SDL2', txt)                    # left to Mono's dllmap and the loader

    def test_port_map(self):
        rep = report(self.mods())
        m = P.merge_map(rep, {'plugins': {'SDL2': {'use': 'host', 'lib': 'libSDL2-2.0.so.0'}, 'sqlite3': 'none',
                                          'discord_game_sdk': {'use': 'stub'}}})
        txt = P.map_text(rep, m)
        self.assertIn('map SDL2 host libSDL2-2.0.so.0', txt)
        self.assertIn('map sqlite3 none', txt)
        self.assertIn('map discord_game_sdk stub discord_game_sdk', txt)
        with self.assertRaises(ValueError):
            P.merge_map(rep, {'x': {'use': 'maybe'}})
        with self.assertRaises(ValueError):
            P.map_text(rep, {'x': {'use': 'stub', 'stub': 'nope'}})

    def test_summary(self):
        rep = report(self.mods(), fmod={'header_version': '2.02.23', 'supply': {
            'version': '2.02.23', 'android': {'files': ['libfmod.so', 'libfmodstudio.so'], 'abi': 'armeabi-v7a', 'to': 'lib/'}}})
        s = P.summary(rep)
        self.assertTrue(any(l.startswith('steam_api (Steamworks') or l.startswith('steam_api (unknown') for l in s))
        self.assertTrue(any('FMOD 2.02.23' in l and 'libfmodstudio.so' in l for l in s))
        self.assertFalse(any(l.startswith('libc ') or l.startswith('SDL2 ') for l in s))


def tiny_elf(machine, needed, note=None, is64=False):
    """An ELF shared object with one PT_LOAD, a PT_DYNAMIC (DT_NEEDED..., DT_STRTAB, DT_SONAME) and an
    optional PT_NOTE."""
    strtab = b'\0'
    offs = []
    for n in needed + ['libtest.so']:
        offs.append(len(strtab))
        strtab += n.encode() + b'\0'
    ehsz, phsz = (64, 56) if is64 else (52, 32)
    nph = 3 if note else 2
    dyn_off = ehsz + nph * phsz
    ents = [(1, o) for o in offs[:-1]] + [(14, offs[-1]), (5, 0), (0, 0)]
    ent = (lambda t, v: struct.pack('<qQ', t, v)) if is64 else (lambda t, v: struct.pack('<iI', t, v))
    dyn = b''.join(ent(t, v) for t, v in ents)
    str_off = dyn_off + len(dyn)
    dyn = b''.join(ent(t, str_off if t == 5 else v) for t, v in ents)
    note_off = str_off + len(strtab)
    notedata = (struct.pack('<III', 8, 4, 1) + b'Android\0' + b'\x1e\0\0\0') if note else b''
    total = note_off + len(notedata)
    if is64:
        eh = b'\x7fELF\x02\x01\x01' + b'\0' * 9 + struct.pack('<HHIQQQIHHHHHH', 3, machine, 1, 0, ehsz, 0, 0, ehsz, phsz, nph, 64, 0, 0)
        ph = lambda t, o, s: struct.pack('<IIQQQQQQ', t, 4, o, o, o, s, s, 8)
    else:
        eh = b'\x7fELF\x01\x01\x01' + b'\0' * 9 + struct.pack('<HHIIIIIHHHHHH', 3, machine, 1, 0, ehsz, 0, 0, ehsz, phsz, nph, 40, 0, 0)
        ph = lambda t, o, s: struct.pack('<IIIIIIII', t, o, o, o, s, s, 4, 4)
    phs = ph(1, 0, total) + ph(2, dyn_off, len(dyn)) + (ph(4, note_off, len(notedata)) if note else b'')
    return eh + phs + dyn + strtab + notedata


class Elf(unittest.TestCase):
    def check(self, data, **want):
        with tempfile.NamedTemporaryFile(suffix='.so', delete=False) as f:
            f.write(data)
        try:
            info = P.elf_info(f.name)
        finally:
            os.unlink(f.name)
        for k, v in want.items():
            self.assertEqual(info[k], v, k)

    def test_android_and_glibc(self):
        self.check(tiny_elf(40, ['libc.so', 'libm.so']), android=True, arch='arm', soname='libtest.so', needed=['libc.so', 'libm.so'])
        self.check(tiny_elf(40, ['libc.so.6', 'libm.so.6']), android=False, arch='arm')
        self.check(tiny_elf(183, ['libdl.so.2'], note=True, is64=True), android=True, arch='arm64')
        self.check(tiny_elf(62, ['libc.so.6'], is64=True), android=False, arch='x86-64')


@unittest.skipUnless(GAMES, 'PMUNITY_GAMES not set')
class GamesCorpus(unittest.TestCase):
    def test_survey(self):
        for data in GAMES:
            with self.subTest(data=data):
                rep = P.survey(data)
                self.assertNotIn('unreadable_assemblies', rep, data)
                for name, e in rep['modules'].items():
                    self.assertTrue(e['entries'] > 0 and e['use'] in ('lib', 'host', 'stub', 'none', 'system', 'keep'), name)
                for name, entry, ret, args in rep['sigs']:
                    self.assertTrue(re.fullmatch(r'[vilfd]', ret) and all(re.fullmatch(r'[ilfd]|[FD][1-4]|[sS]\d+', a) for a in args), entry)
                txt = P.map_text(rep, rep['map'])
                self.assertTrue(txt.startswith('# plugins.map'))
                if rep['fmod']:
                    self.assertRegex(rep['fmod']['header_version'] or '', r'^\d+\.\d\d\.\d\d$')


if __name__ == '__main__':
    unittest.main()
