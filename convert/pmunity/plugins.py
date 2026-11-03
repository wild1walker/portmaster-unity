"""Native plugins of a Unity game and what the Android-player port gives each of them
(docs/plugins.md).

    python -m pmunity plugins <Game>_Data [--map OUT] [--plugin-map port.json] [--json OUT]

A desktop build ships its native plugins as x86/x86-64 (or macOS) libraries in <Game>_Data/Plugins,
useless on ARM. This module reads which libraries the game's managed code P/Invokes (ImplMap
records of every game assembly, clrmeta.py), with each call's argument types, matches them with
the files in Plugins/ and with known SDKs (FMOD, Steamworks, Discord, GOG Galaxy, EOS, Wwise, ...),
and says for each what the runtime does:

  stub    a built-in library (runtime/host) that answers as a PC without that service
          (Steam not running, Discord not running, ...); for Wwise a silent sound engine made of the
          game's own entry points (fn lines below)
  lib     an ARM build the porter or player supplies in the port's lib/ folder (FMOD: the exact
          FMOD Engine version is read from the game, see fmod_info)
  host    a library of the handheld's system (e.g. libsqlite3.so.0)
  none    dlopen fails: DllNotFoundException at the first call, as on a PC without that library
  system  libc and friends, mapped by the loader; Windows-only names fail as on a Linux PC

The runtime reads the result as `plugins.map` (written beside the APK by android-data): one `map`
line per DllImport name and one `sig` line per entry point whose arguments include float or double
values (on 32-bit ARM a glibc library expects them in VFP registers, Mono passes them in core
registers: ua_host builds a converting thunk from the line). A stub built from the game's own entry
points (Wwise, whose C# integration P/Invokes a different set in every version) gets one `fn` line
per entry point (its result and argument classes, stub_token) and `const` lines with the game's
AKRESULT and AkCallbackType values. The port can override any entry with
a JSON plugin map (--plugin-map; the "plugins" object of a port config):

  {"fmodstudio": {"use": "lib", "file": "libfmodstudio.so"},
   "sqlite3": {"use": "host", "lib": "libsqlite3.so.0"},
   "discord_game_sdk": {"use": "stub"}, "GalaxyCSharpGlue": {"use": "none"}}
"""
import argparse
import fnmatch
import json
import os
import re
import struct
import sys

from . import clrmeta as C

# --------------------------------------------------------------------------- what is known

# (id, DllImport name pattern (case-insensitive, without lib prefix / extension), title, default use,
#  stub id, note)
SDKS = [
    ('fmod', r'fmod(studio)?l?(64)?', 'FMOD Engine / FMOD Studio', 'lib', None,
     'audio middleware: supply FMOD\'s ARM libraries of the same version (see "fmod")'),
    ('steamworks', r'steam_api(64)?', 'Steamworks', 'stub', 'steam',
     'built-in stub: SteamAPI_Init fails as without a running Steam client'),
    ('steam-encryptedappticket', r'sdkencryptedappticket(64)?', 'Steamworks encrypted app tickets', 'none', None,
     'only reached after a successful SteamAPI_Init'),
    ('csteamworks', r'csteamworks', 'Steamworks.NET (CSteamworks glue, before 9.0)', 'stub', 'csteamworks',
     'built-in stub: Init fails as without a running Steam client'),
    ('discord-game-sdk', r'discord_game_sdk', 'Discord Game SDK', 'stub', 'discord_game_sdk',
     'built-in stub: DiscordCreate answers NotRunning, as without a Discord client'),
    ('discord-rpc', r'discord-rpc', 'Discord RPC', 'stub', 'discord-rpc',
     'built-in stub: never connects, as without a Discord client'),
    ('discord-partner-sdk', r'discord_partner_sdk', 'Discord Social SDK', 'none', None,
     'no stub yet: DllNotFoundException at the first call'),
    ('galaxy', r'galaxy(csharpglue|peer)?(64)?', 'GOG Galaxy SDK', 'none', None,
     'no stub: the SWIG glue throws DllNotFoundException/TypeInitializationException at its first call; '
     'GOG builds usually catch it (offline mode)'),
    ('eos', r'eossdk-.*', 'Epic Online Services', 'stub', 'eos',
     'built-in stub: EOS_Initialize succeeds, EOS_Platform_Create gives no platform (no network service)'),
    ('wwise', r'aksoundengine', 'Audiokinetic Wwise', 'stub', 'wwise',
     "built-in stub: a silent sound engine made of the game's own entry points (every call succeeds, the "
     "bank-loaded and end-of-event callbacks reach the game); an ARM AkSoundEngine of the game's Wwise "
     "version in lib/ plays the sound instead"),
    ('sqlite', r'(e_)?sqlite3?', 'SQLite', 'host', None,
     "the system's libsqlite3.so.0 (32-bit for the armv7 player), or an ARM build in lib/"),
    ('standalonefilebrowser', r'standalonefilebrowser', 'StandaloneFileBrowser (native file dialogs)', 'none', None,
     'no file dialogs on a handheld: DllNotFoundException when the game opens one'),
    ('xinput', r'xinputinterface(32|64)?', 'XInputDotNet', 'none', None, 'Windows-only'),
    ('rewired', r'rewired_.*', 'Rewired native helper', 'none', None, 'Rewired uses its Unity-input path on Android'),
    ('steamaudio', r'phonon', 'Steam Audio', 'lib', None, 'supply an ARM build of phonon'),
    ('sdl2', r'sdl2(-2\.0)?', 'SDL2', 'keep', None,
     "left to Mono's dllmap and the loader (a port maps it with use host libSDL2-2.0.so.0 or --dllmap)"),
    ('openal', r'(soft_oal|openal(32)?)', 'OpenAL', 'keep', None, "a port can map the system's libopenal.so.1 (use host)"),
    ('udev', r'udev', 'libudev (joystick enumeration, e.g. Rewired on Linux)', 'keep', None,
     "Rewired's Linux path: on the Android player see tools/asmpatch rewired-android"),
]
HOST_DEFAULT = {'sqlite': 'libsqlite3.so.0'}

# P/Invoke names that are the C library or the OS (the loader maps libc/libm/libdl/libz; Mono's own
# libraries come with the Android player; Windows and macOS names fail as on a Linux PC)
SYSTEM = re.compile(r'^(__internal|libc|c|libc\.so(\.6)?|libdl(\.so(\.2)?)?|dl|libm|m|libpthread|pthread|libz|z|zlib1?|'
                    r'monoposixhelper|system\.native|system\.net\.security\.native|system\.globalization\.native|'
                    r'system\.io\.compression\.native|libmono-btls-shared|mono-native)$', re.I)
WINDOWS = re.compile(r'(^(kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|ws2_32|winmm|comdlg32|crypt32|iphlpapi|'
                     r'winspool|ntdll|dwmapi|shcore|xinput[0-9_]*|dinput8|psapi|version|imm32|setupapi|hid|dbghelp|'
                     r'd3d\w*|dxgi|opengl32|msvcrt|ucrtbase)(\.dll|\.drv)?$)|\.dll$', re.I)
MACOS = re.compile(r'^/System/|^/usr/lib/|\.dylib$|\.framework|\.bundle$', re.I)

# assemblies that come with Unity / the runtime (their P/Invokes are Mono's and the engine's business)
FRAMEWORK = re.compile(r'^(mscorlib|netstandard|System|System\..*|Mono\.(Security|Posix|Cecil.*|CompilerServices.*|'
                       r'Simd|Tasklets|WebBrowser)|UnityEngine(\..*)?|Microsoft\..*|Boo\..*|UnityScript.*|mcs|'
                       r'nunit\.framework|ICSharpCode\..*)\.dll$', re.I)

# files of the Plugins folder that no managed P/Invoke names
FILE_NOTES = [
    (r'lib_burst_generated', 'Burst-compiled code: absent on ARM, Burst runs the managed versions of those jobs'),
    (r'screenselector', "the Linux player's own launcher dialog (not used by the game's code)"),
    (r'resonanceaudio', 'an FMOD DSP plugin (Google Resonance Audio): needed only if the FMOD settings load it; '
                        'supply its ARM build with FMOD\'s'),
    (r'(fmod|ak).*plugin|libfmod_.*|.*_fmod', 'an FMOD/Wwise plugin: supply its ARM build if the game loads it'),
    (r'^(lib)?(?-i:Ak[A-Z])\w+\.(so|dll|bundle)$', "a Wwise plugin (an effect or codec Wwise's sound engine loads): not used by the "
                                             "built-in stub; with an ARM sound engine in lib/ supply its ARM build too"),
]

ELF_MACHINE = {3: 'x86', 62: 'x86-64', 40: 'arm', 183: 'arm64'}


def norm(name):
    """A DllImport name without directory, 'lib' prefix and library extension: 'libfmodstudio.so.13' ->
    'fmodstudio'."""
    n = os.path.basename(name.replace('\\', '/'))
    n = re.sub(r'(\.so(\.\d+)*|\.dll|\.dylib|\.bundle)$', '', n, flags=re.I)
    if n.lower().startswith('lib') and len(n) > 3:
        n = n[3:]
    return n


def sdk_of(name):
    n = norm(name).lower()
    for s in SDKS:
        if re.fullmatch(s[1], n, re.I):
            return s
    return None


# --------------------------------------------------------------------------- argument classes

# by-value types the resolver cannot open (other assemblies than the game's): well-known layouts
KNOWN_VALUETYPES = {
    'System.IntPtr': 'i', 'System.UIntPtr': 'i', 'System.Boolean': 'i', 'System.Char': 'i', 'System.SByte': 'i',
    'System.Byte': 'i', 'System.Int16': 'i', 'System.UInt16': 'i', 'System.Int32': 'i', 'System.UInt32': 'i',
    'System.Int64': 'l', 'System.UInt64': 'l', 'System.Single': 'f', 'System.Double': 'd', 'System.Guid': 's4',
    'UnityEngine.Vector2': 'F2', 'UnityEngine.Vector3': 'F3', 'UnityEngine.Vector4': 'F4',
    'UnityEngine.Quaternion': 'F4', 'UnityEngine.Color': 'F4', 'UnityEngine.Rect': 'F4', 'UnityEngine.Color32': 's1',
    'UnityEngine.Vector2Int': 's2', 'UnityEngine.Vector3Int': 's3',
}


class Resolver:
    """Opens the assemblies of a Managed folder on demand to resolve by-value types."""

    def __init__(self, managed):
        self.managed = managed
        self.cache = {}

    def get(self, name):
        if name not in self.cache:
            p = os.path.join(self.managed, name + '.dll')
            try:
                self.cache[name] = C.Assembly(p) if os.path.isfile(p) else None
            except (C.NotAssembly, ValueError, IndexError, struct.error):
                self.cache[name] = None
        return self.cache[name]

    def find(self, asm, tab, row):
        """(assembly, typedef row) of a TypeDef/TypeRef of asm, following TypeRefs into other assemblies."""
        if tab == C.TYPEDEF:
            return asm, row
        if tab != C.TYPEREF:
            return None, None
        full = asm.typeref_name(row)
        scope = asm.typeref_scope(row)
        other = self.get(scope) if scope else None
        if other is None:
            return None, full
        for i, t in enumerate(other.types()):
            if t['full'] == full:
                return other, i + 1
        return None, full


def leaf_classes(res, asm, t, depth=0):
    """[(kind, size)] of the scalar leaves of type t passed by value: kind 'f' (float), 'd' (double),
    'i' (integer/pointer); None when unknown."""
    if depth > 8:
        return None
    k = t[0]
    if k == 'prim':
        e = t[1]
        if e == C.E_R4:
            return [('f', 4)]
        if e == C.E_R8:
            return [('d', 8)]
        if e in (C.E_I8, C.E_U8):
            return [('i', 8)]
        if e in (C.E_STRING, C.E_OBJECT, C.E_I, C.E_U):
            return [('i', 4)]
        return [('i', C.PRIM_SIZE.get(e, 4))]
    if k in ('ptr', 'byref', 'class', 'array', 'fnptr'):
        return [('i', 4)]
    if k == 'valuetype':
        name, ref = t[1], t[2]
        if ref is None:
            return None
        a, row = res.find(asm, *ref)
        if a is None:
            v = KNOWN_VALUETYPES.get(row if isinstance(row, str) else name)
            if v is None:
                return None
            if v in ('i', 'l', 'f', 'd'):
                return [({'l': 'i'}.get(v, v), 8 if v in ('l', 'd') else 4)]
            n = int(v[1:])
            return [('f', 4)] * n if v[0] == 'F' else [('i', 4)] * n
        base, fields, size, flags = a.typedef_layout(row)
        if base == 'System.Enum':
            for fname, ft in fields:
                if fname == 'value__' and ft:
                    return leaf_classes(res, a, ft, depth + 1)
            return [('i', 4)]
        out = []
        for fname, ft in fields:
            if ft is None:
                return None
            sub = leaf_classes(res, a, ft, depth + 1)
            if sub is None:
                return None
            out += sub
        if size and sum(s for _, s in out) < size:
            out.append(('i', size - sum(s for _, s in out)))       # explicit size / padding
        return out or [('i', 4)]
    return None


def abi_token(res, asm, t):
    """The argument class of one P/Invoke argument or result, as ua_host's thunks read it:
    i (word), l (two words, even-aligned), f (float), d (double), F<n>/D<n> (a struct of n floats /
    doubles, a VFP candidate), s<n>/S<n> (another struct of n words, word / doubleword aligned),
    v (void), ? (unknown)."""
    if t[0] == 'prim' and t[1] == C.E_VOID:
        return 'v'
    leaves = leaf_classes(res, asm, t)
    if leaves is None:
        return '?'
    if t[0] == 'prim' or (len(leaves) == 1 and t[0] != 'valuetype'):
        kind, size = leaves[0]
        return {'f': 'f', 'd': 'd'}.get(kind, 'l' if size == 8 else 'i')
    kinds = {k for k, _ in leaves}
    if t[0] == 'valuetype' and len(leaves) == 1 and kinds != {'f'} and kinds != {'d'}:
        return 'l' if leaves[0][1] == 8 else 'i'                     # an enum or a one-field struct
    if kinds == {'f'} and len(leaves) <= 4:
        return f'F{len(leaves)}'
    if kinds == {'d'} and len(leaves) <= 4:
        return f'D{len(leaves)}'
    size, align = 0, 4
    for k, s in leaves:
        a = 8 if s == 8 else min(4, s)
        align = max(align, a)
        size = (size + a - 1) // a * a + s
    words = (size + 3) // 4
    return f'S{(words + 1) // 2 * 2}' if align == 8 else f's{words}'


# the classes of a stub's entry points (fn lines): the integer kinds by sign, pointers and strings apart
STUB_PRIM = {C.E_VOID: 'v', C.E_BOOL: 'b', C.E_CHAR: 'u', C.E_I1: 'i', C.E_U1: 'u', C.E_I2: 'i', C.E_U2: 'u',
             C.E_I4: 'i', C.E_U4: 'u', C.E_I8: 'l', C.E_U8: 'l', C.E_R4: 'f', C.E_R8: 'd', C.E_STRING: 'z',
             C.E_I: 'p', C.E_U: 'p', C.E_OBJECT: 'p'}
STUB_VALUETYPES = {'System.IntPtr': 'p', 'System.UIntPtr': 'p', 'System.Runtime.InteropServices.HandleRef': 'p',
                   'System.Boolean': 'b', 'System.Char': 'u', 'System.SByte': 'i', 'System.Byte': 'u',
                   'System.Int16': 'i', 'System.UInt16': 'u', 'System.Int32': 'i', 'System.UInt32': 'u',
                   'System.Int64': 'l', 'System.UInt64': 'l', 'System.Single': 'f', 'System.Double': 'd'}


def stub_token(res, asm, t, depth=0):
    """The class of a P/Invoke argument or result for a stub built from the game's entry points
    (ua_host's wwise.c reads it): v b i (signed int, an enum) u (unsigned) l (64-bit) f d, p (a
    pointer: IntPtr, HandleRef, an array, a class or delegate), r (a ref/out pointer), z (a string),
    a struct by value as abi_token writes it, ? unknown."""
    k = t[0]
    if k == 'prim':
        return STUB_PRIM.get(t[1], '?')
    if k == 'byref':
        return 'r'
    if k in ('ptr', 'class', 'array', 'fnptr'):
        return 'p'
    if k != 'valuetype' or depth > 8:
        return '?'
    full = t[1]
    if t[2] is not None:
        a, row = res.find(asm, *t[2])
        if a is not None:
            full = a.types()[row - 1]['full']
            base, fields, size, flags = a.typedef_layout(row)
            if base == 'System.Enum':
                ft = next((ft for fname, ft in fields if fname == 'value__' and ft), None)
                return stub_token(res, a, ft, depth + 1) if ft else 'i'
        elif isinstance(row, str):
            full = row
    return STUB_VALUETYPES.get(full) or abi_token(res, asm, t)


SIGNATURE_STUBS = ('wwise',)                   # stubs made of the game's entry points: fn lines
STUB_CONSTS = {'wwise': ('AKRESULT', 'AkCallbackType')}     # and these enums of the game: const lines


def needs_thunk(sig):
    ret, params = sig
    return any(x[0] in 'fdFD' for x in [ret] + params)


# --------------------------------------------------------------------------- files

def elf_info(path):
    """{'format': 'elf', 'arch', 'soname', 'needed', 'android'} of an ELF file, or None."""
    try:
        with open(path, 'rb') as f:
            d = f.read(1 << 20)
            size = os.path.getsize(path)
            if size > len(d):
                f.seek(0)
                d = f.read()
    except OSError:
        return None
    if d[:4] != b'\x7fELF':
        return None
    is64 = d[4] == 2
    end = '<' if d[5] == 1 else '>'
    mach, = struct.unpack_from(end + 'H', d, 18)
    info = {'format': 'elf', 'arch': ELF_MACHINE.get(mach, str(mach)), 'soname': None, 'needed': [], 'android': False}
    try:
        if is64:
            phoff, = struct.unpack_from(end + 'Q', d, 32)
            phsz, phn = struct.unpack_from(end + 'HH', d, 54)
        else:
            phoff, = struct.unpack_from(end + 'I', d, 28)
            phsz, phn = struct.unpack_from(end + 'HH', d, 42)
        loads, dyn = [], None
        for i in range(phn):
            o = phoff + i * phsz
            if is64:
                pt, _, off, va, _, fsz = struct.unpack_from(end + 'IIQQQQ', d, o)
            else:
                pt, off, va, _, fsz = struct.unpack_from(end + 'IIIII', d, o)
            if pt == 1:
                loads.append((va, off, fsz))
            elif pt == 2:
                dyn = (off, fsz)
            elif pt == 4:                                  # PT_NOTE: Android's ABI note
                if b'Android' in d[off:off + fsz]:
                    info['android'] = True
        if dyn:
            ents, strtab = [], None
            w = 16 if is64 else 8
            for o in range(dyn[0], dyn[0] + dyn[1], w):
                tag, val = struct.unpack_from(end + ('qQ' if is64 else 'iI'), d, o)
                if tag == 0:
                    break
                ents.append((tag, val))
                if tag == 5:
                    strtab = val
            if strtab is not None:
                so = next((off + strtab - va for va, off, fsz in loads if va <= strtab < va + fsz), None)
                if so is not None:
                    def s(i):
                        return d[so + i:d.index(b'\0', so + i)].decode('latin-1')
                    info['needed'] = [s(v) for t, v in ents if t == 1]
                    info['soname'] = next((s(v) for t, v in ents if t == 14), None)
        if any(n in ('libc.so', 'liblog.so') for n in info['needed']):
            info['android'] = True
    except (struct.error, ValueError, IndexError):
        pass
    return info


def pe_version(path):
    """FileVersion / ProductVersion of a Windows DLL (VS_VERSIONINFO strings), or None."""
    try:
        d = open(path, 'rb').read()
    except OSError:
        return None
    if d[:2] != b'MZ':
        return None
    for key in ('ProductVersion', 'FileVersion'):
        k = (key + '\0').encode('utf-16-le')
        i = d.find(k)
        if i >= 0:
            j = i + len(k)
            j += (-j) % 4                                  # value is 32-bit aligned
            e = j
            while e + 1 < len(d) and d[e:e + 2] != b'\0\0':
                e += 2
            v = d[j:e].decode('utf-16-le', 'replace').strip()
            if v:
                return v
    return None


def plist_version(bundle):
    p = os.path.join(bundle, 'Contents', 'Info.plist')
    try:
        txt = open(p, 'rb').read().decode('utf-8', 'replace')
    except OSError:
        return None
    m = re.search(r'<key>CFBundle(?:ShortVersionString|Version)</key>\s*<string>([^<]+)</string>', txt)
    return m.group(1).strip() if m else None


def plugin_files(data):
    """[(relative path, abs path)] of the native libraries the build ships: <Data>/Plugins (all
    subfolders), and for a macOS app Contents/Plugins and Contents/Frameworks bundles."""
    out = []
    roots = [os.path.join(data, 'Plugins')]
    contents = os.path.dirname(os.path.dirname(os.path.abspath(data)))
    if os.path.basename(contents) == 'Contents':
        roots += [os.path.join(contents, 'Plugins'), os.path.join(contents, 'Frameworks')]
    for root in roots:
        if not os.path.isdir(root):
            continue
        for r, ds, fs in os.walk(root):
            ds.sort()
            for n in list(ds):
                if n.endswith('.bundle') or n.endswith('.framework'):
                    out.append((os.path.relpath(os.path.join(r, n), os.path.dirname(root)), os.path.join(r, n)))
                    ds.remove(n)
            for n in sorted(fs):
                if n.startswith('.') or not re.search(r'(\.so(\.\d+)*|\.dll|\.dylib)$', n, re.I):
                    continue
                out.append((os.path.relpath(os.path.join(r, n), os.path.dirname(root)), os.path.join(r, n)))
    return out


def describe_file(path):
    if os.path.isdir(path):
        v = plist_version(path)
        return {'format': 'macos-bundle', 'version': v}
    e = elf_info(path)
    if e:
        return e
    if path.lower().endswith('.dll'):
        with open(path, 'rb') as f:
            head = f.read(0x400)
        if head[:2] == b'MZ':
            try:
                C.Assembly(path)
                return {'format': 'managed'}
            except (C.NotAssembly, ValueError, IndexError, struct.error):
                pass
            return {'format': 'pe', 'version': pe_version(path)}
    if path.endswith('.dylib'):
        return {'format': 'mach-o'}
    return {'format': 'unknown'}


# --------------------------------------------------------------------------- FMOD

def fmod_version_str(n):
    return f'{n >> 16 & 0xffff:x}.{n >> 8 & 0xff:02x}.{n & 0xff:02x}'


# Linux SONAME suffix of each FMOD Engine minor line
FMOD_SONAME = {'10': '1.10', '11': '2.00', '12': '2.01', '13': '2.02', '14': '2.03'}


def fmod_info(constants, pinvokes, files):
    """What FMOD a game uses: the integration's header version (FMOD.VERSION.number, which the
    managed code passes to System_Create: the native library must be of the same major.minor line,
    and the same patch release is what was tested), the DllImport names and the desktop libraries."""
    num = next((v for k, v in constants.items() if k.endswith('FMOD.VERSION.number') and isinstance(v, int)), None)
    names = sorted({p['module'] for p in pinvokes if sdk_of(p['module']) and sdk_of(p['module'])[0] == 'fmod'})
    if num is None and not names:
        return None
    info = {'header_version': fmod_version_str(num) if num is not None else None,
            'header_version_hex': f'0x{num:08x}' if num is not None else None, 'dllimport_names': names}
    suffix = next((v for k, v in constants.items() if k.endswith('FMOD.VERSION.dllSuffix')), None)
    if suffix:
        info['dll_suffix'] = suffix
    native = []
    for rel, p in files:
        if not re.search(r'fmod', os.path.basename(rel), re.I):
            continue
        d = describe_file(p)
        e = {'file': rel, 'format': d.get('format'), 'arch': d.get('arch')}
        if d.get('soname'):
            e['soname'] = d['soname']
            m = re.search(r'\.so\.(\d+)', d['soname'])
            if m and m.group(1) in FMOD_SONAME:
                e['line'] = FMOD_SONAME[m.group(1)]
        if d.get('version'):
            e['version'] = d['version']
        if num is not None and not os.path.isdir(p):
            with open(p, 'rb') as f:
                e['contains_header_version'] = struct.pack('<I', num) in f.read()
        native.append(e)
    info['desktop_libraries'] = native
    ver = info['header_version']
    line = '.'.join(ver.split('.')[:2]) if ver else None
    lines = {e.get('line') for e in native if e.get('line')}
    if line and lines and line not in lines:
        info['warning'] = f'the managed wrapper says FMOD {ver}, the desktop library is of line {", ".join(sorted(lines))}'
    core_in_studio = names == ['fmodstudio']
    info['core_dllimport'] = 'fmodstudio' if core_in_studio else ('fmod' if 'fmod' in names else None)
    v = ver or '<version>'
    vv = v.replace('.', '')
    files_android = ['libfmod.so', 'libfmodstudio.so'] if 'fmodstudio' in names or core_in_studio else ['libfmod.so']
    info['supply'] = {
        'version': v,
        'android': {
            'files': files_android,
            'abi': 'armeabi-v7a for the armv7 Mono player, arm64-v8a for an arm64 player',
            'from': [f'FMOD Engine {v} for Android (fmodstudioapi{vv}android.tar.gz): api/core/lib/<abi>/libfmod.so, '
                     f'api/studio/lib/<abi>/libfmodstudio.so',
                     f'or FMOD for Unity {v} (fmodstudio{vv}.unitypackage): the Android libraries under '
                     f'Assets/Plugins/FMOD/.../android/.../<abi>/'],
            'to': 'lib/ (beside libunity.so)',
        },
        'linux': {
            'files': files_android,
            'from': f'FMOD Engine {v} for Linux (fmodstudioapi{vv}linux.tar.gz): api/core/lib/arm/ (armhf) or arm64/, '
                    f'api/studio/lib/arm/ or arm64/',
            'to': 'lib/ under those names (libfmod.so.<n>.<m> -> libfmod.so; ua_host loads a library\'s '
                  'DT_NEEDED siblings from lib/ first)',
            'note': 'alternative: output through ALSA/PulseAudio (not the runtime\'s audio path); on the armv7 '
                    'player calls with float arguments go through ua_host\'s softfp thunks (sig lines)',
        },
        'plugins': 'FMOD DSP plugins the game\'s FMOD settings load (e.g. libresonanceaudio.so) from the same release',
        'license': 'FMOD\'s licence applies (free for small developers; downloads need an FMOD account); '
                   'nothing of FMOD is redistributed with the port tools',
    }
    return info


# --------------------------------------------------------------------------- Wwise

# soundbank format version (a bank's BKHD chunk, SoundbanksInfo.xml's SoundbankVersion) -> Wwise release
WWISE_BANK_LINES = {88: '2013', 112: '2014.1', 113: '2015.1', 118: '2016.1', 120: '2016.2', 125: '2017.1',
                    128: '2017.2', 132: '2018.1', 134: '2019.1', 135: '2019.2', 140: '2021.1'}


def _elf_dynsyms(d):
    """{name: (file offset, ...)} of an ELF's dynamic symbols that are functions, via the section headers."""
    out = {}
    try:
        is64, end = d[4] == 2, '<' if d[5] == 1 else '>'
        shoff, = struct.unpack_from(end + ('Q' if is64 else 'I'), d, 0x28 if is64 else 0x20)
        shsz, shn = struct.unpack_from(end + 'HH', d, 0x3A if is64 else 0x2E)
        secs = []
        for i in range(shn):
            o = shoff + i * shsz
            if is64:
                _, typ, _, addr, off, size, link, _, _, entsz = struct.unpack_from(end + 'IIQQQQIIQQ', d, o)
            else:
                _, typ, _, addr, off, size, link, _, _, entsz = struct.unpack_from(end + 'IIIIIIIIII', d, o)
            secs.append((typ, addr, off, size, link, entsz))
        for typ, addr, off, size, link, entsz in secs:
            if typ != 11 or not entsz:                       # SHT_DYNSYM
                continue
            stroff = secs[link][2]
            for o in range(off, off + size, entsz):
                if is64:
                    name, info, _, shndx, value, _ = struct.unpack_from(end + 'IBBHQQ', d, o)
                else:
                    name, value, _, info, _, shndx = struct.unpack_from(end + 'IIIBBH', d, o)
                if not shndx or info & 0xf != 2:             # STT_FUNC
                    continue
                n = d[stroff + name:d.index(b'\0', stroff + name)].decode('latin-1')
                for t, a, so, sz, _, _ in secs:              # the address in the file
                    if t == 1 and a <= value < a + sz:       # SHT_PROGBITS
                        out[n] = so + value - a
    except (struct.error, ValueError, IndexError):
        pass
    return out


def _x86_constant(d, off, depth=0):
    """The int an x86 function returns as a constant: mov eax, imm32; ret, after following a jmp, or
    the first call (32-bit PIC code calls the function that does it)."""
    if off is None or depth > 2 or off + 8 > len(d):
        return None
    if d[off] == 0xb8 and d[off + 5] == 0xc3:
        return struct.unpack_from('<I', d, off + 1)[0]
    for i in range(off, min(off + 24, len(d) - 5)):
        if d[i] in (0xe9, 0xe8):                            # jmp / call rel32
            target = i + 5 + struct.unpack_from('<i', d, i + 1)[0]
            v = _x86_constant(d, target, depth + 1)
            if v is not None or d[i] == 0xe9:
                return v
    return None


def wwise_info(data):
    """What Wwise a game uses: the version of its desktop libAkSoundEngine (the constants its
    CSharp_GetMajorMinorVersion / CSharp_GetSubminorBuildVersion return, read from the x86 code of
    an ELF build; or a version string), and the soundbank format of its banks (their BKHD chunk,
    SoundbanksInfo.xml), which names the Wwise release that wrote them. None without Wwise."""
    libs = [(rel, p) for rel, p in plugin_files(data) if re.match(r'(lib)?aksoundengine\b', os.path.basename(rel), re.I)]
    banks = []
    sa = os.path.join(data, 'StreamingAssets')
    for r, ds, fs in os.walk(sa):
        ds.sort()
        banks += [os.path.join(r, n) for n in sorted(fs) if n.lower().endswith('.bnk')]
    if not libs and not banks:
        return None
    info = {'libraries': [rel for rel, _ in libs], 'version': None, 'banks': len(banks), 'bank_versions': [],
            'bank_folders': sorted({os.path.relpath(os.path.dirname(b), data) for b in banks})}
    for rel, p in libs:
        if os.path.isdir(p) or info['version']:
            continue
        try:
            d = open(p, 'rb').read()
        except OSError:
            continue
        m = re.search(rb'(?:Wwise|Ak)[^\0]{0,16}?v?(20[1-3]\d\.\d+\.\d+)(?:[^\0]{0,8}?[Bb]uild[ :]*(\d+))?', d)
        if m:
            info['version'] = m.group(1).decode() + (f' build {m.group(2).decode()}' if m.group(2) else '')
            info['version_from'] = rel + ' (a string)'
            continue
        if d[:4] != b'\x7fELF' or d[18] not in (3, 62):     # x86, x86-64
            continue
        syms = _elf_dynsyms(d)
        mm = _x86_constant(d, syms.get('CSharp_GetMajorMinorVersion'))
        sb = _x86_constant(d, syms.get('CSharp_GetSubminorBuildVersion'))
        if mm is not None and 2010 <= mm >> 16 <= 2040 and (mm & 0xffff) < 20:
            info['version'] = f'{mm >> 16}.{mm & 0xffff}' + (f'.{sb >> 16} build {sb & 0xffff}' if sb is not None else '')
            info['version_from'] = rel + ' (its code)'
    vers = set()
    for b in banks[:64]:
        try:
            with open(b, 'rb') as f:
                h = f.read(12)
        except OSError:
            continue
        if h[:4] == b'BKHD' and len(h) == 12:
            vers.add(struct.unpack_from('<I', h, 8)[0])
    for folder in info['bank_folders']:
        x = os.path.join(data, folder, 'SoundbanksInfo.xml')
        try:
            m = re.search(rb'SoundbankVersion="(\d+)"', open(x, 'rb').read(4096))
        except OSError:
            m = None
        if m:
            vers.add(int(m.group(1)))
    info['bank_versions'] = sorted(vers)
    lines = [WWISE_BANK_LINES.get(v) for v in info['bank_versions']]
    info['bank_lines'] = [ln or (f'2021.1 or later' if v > 140 else 'unknown') for ln, v in zip(lines, info['bank_versions'])]
    return info


def wwise_summary(w):
    """One line on a game's Wwise and what the port does with it."""
    ver = w.get('version')
    banks = ', '.join(f'version {v} (Wwise {ln})' for v, ln in zip(w['bank_versions'], w['bank_lines']))
    what = f"Wwise {ver or (w['bank_lines'][0] if w['bank_lines'] else '(version not read)')}"
    src = []
    if ver:
        src.append(w['version_from'])
    if w['banks']:
        src.append(f"{w['banks']} soundbanks in {', '.join(w['bank_folders'])}" + (f': {banks}' if banks else ''))
    return (f"{what} ({'; '.join(src)}): the port gives the game ua_host's built-in Wwise, a silent sound engine "
            f"made of the integration's own entry points (every call succeeds, banks load, events end at once, "
            f"the bank-loaded and end-of-event callbacks reach the game). Sound needs Wwise's sound engine for "
            f"ARM of {ver or 'the same version'} in the port's lib/ (Wwise's Android SDK: libAkSoundEngine.so, "
            f"an Audiokinetic account) or a sound backend (UA_WWISE_BACKEND, docs/plugins.md)")


def port_check(rep):
    """port check: Wwise's version and what the port does with it."""
    data = rep.get('data')
    if not data or not os.path.isdir(data) or rep.get('backend') not in ('mono', None):
        return
    if not any(re.fullmatch('(lib)?aksoundengine', norm(m), re.I) for m in (rep.get('code') or {}).get('pinvoke', {})):
        return
    w = wwise_info(data)
    if w:
        rep['wwise'] = w
        rep['notes'].append(wwise_summary(w))


# --------------------------------------------------------------------------- survey

def survey(data):
    """The plugin report of a desktop data folder: {'modules': {name: {...}}, 'files': [...],
    'fmod': {...} | None, 'map': {name: entry}, 'sigs': [(module, entry, ret, params)]}."""
    managed = os.path.join(data, 'Managed')
    res = Resolver(managed)
    pins, consts, bad = [], {}, []
    if os.path.isdir(managed):
        for n in sorted(os.listdir(managed)):
            if not n.endswith('.dll') or FRAMEWORK.match(n):
                continue
            a = res.get(n[:-4])
            if a is None:
                bad.append(n)
                continue
            try:
                for p in a.pinvokes():
                    p['assembly'] = n
                    sig = p.pop('sig')
                    p['abi'] = (abi_token(res, a, sig[0]), [abi_token(res, a, x) for x in sig[1]]) if sig else None
                    sdk = sdk_of(p['module'])
                    if sdk and sdk[4] in SIGNATURE_STUBS:
                        p['stub_abi'] = (stub_token(res, a, sig[0]), [stub_token(res, a, x) for x in sig[1]]) if sig else None
                    pins.append(p)
                consts.update(a.field_constants())
            except (ValueError, IndexError, struct.error) as ex:
                bad.append(f'{n}: {ex}')
    files = plugin_files(data)
    mods = {}
    for p in pins:
        m = mods.setdefault(p['module'], {'entries': {}, 'assemblies': set(), 'types': set()})
        m['entries'][p['entry']] = p['abi']
        m['assemblies'].add(p['assembly'])
        m['types'].add(p['type'])
    out = {}
    used_files = set()
    for name in sorted(mods, key=str.lower):
        m = mods[name]
        key = norm(name).lower()
        match = [rel for rel, path in files if norm(rel).lower() == key]
        used_files.update(match)
        sdk = sdk_of(name)
        e = {'entries': len(m['entries']), 'assemblies': sorted(m['assemblies']),
             'types': sorted(m['types'])[:8], 'desktop_files': match}
        floats = sorted(k for k, v in m['entries'].items() if v and any(x[0] in 'fdFD' for x in [v[0]] + v[1]))
        unknown = sorted(k for k, v in m['entries'].items() if v is None or '?' in [v[0]] + v[1])
        if floats:
            e['float_args'] = len(floats)
        if unknown:
            e['unknown_args'] = unknown[:10]
        if sdk:
            e.update(sdk=sdk[0], title=sdk[2], use=sdk[3], note=sdk[5])
            if sdk[4]:
                e['stub'] = sdk[4]
            if sdk[3] == 'host':
                e['lib'] = HOST_DEFAULT.get(sdk[0])
        elif SYSTEM.match(name):
            e.update(use='system', note='C library / Mono runtime library: provided')
        elif WINDOWS.search(name):
            e.update(use='system', note='Windows-only: calls fail as on a Linux PC')
        elif MACOS.search(name):
            e.update(use='system', note='macOS-only: calls fail as on a Linux PC')
        elif match:
            e.update(use='none', note='shipped as a desktop library only: DllNotFoundException at the first call '
                                      'unless an ARM build (Android, or Linux glibc) is put in lib/')
        else:
            e.update(use='none', note='not shipped with the game (a system library on the desktop?): '
                                      'DllNotFoundException at the first call unless the handheld has it')
        out[name] = e
    filerep = []
    for rel, path in files:
        d = describe_file(path)
        f = {'file': rel, 'format': d.get('format')}
        for k in ('arch', 'soname', 'version'):
            if d.get(k):
                f[k] = d[k]
        if rel in used_files:
            f['used_by'] = sorted(n for n, e in out.items() if rel in e['desktop_files'])
        elif d.get('format') != 'managed':
            f['note'] = next((t for rx, t in FILE_NOTES if re.search(rx, os.path.basename(rel), re.I)),
                             'no P/Invoke of the managed code names it (loaded by another plugin, or unused)')
        filerep.append(f)
    sigs = []
    for name in sorted(mods):
        for entry, abi in sorted(mods[name]['entries'].items()):
            if abi and needs_thunk(abi) and '?' not in [abi[0]] + abi[1]:
                sigs.append((name, entry, abi[0], abi[1]))
    # a stub made of the game's own entry points: every entry point's classes, and the game's constants
    fns, stub_consts = [], []
    for p in sorted((p for p in pins if 'stub_abi' in p), key=lambda p: (p['module'], p['entry'])):
        abi = p['stub_abi']
        if abi and '?' not in [abi[0]] + abi[1] and not any(f[:2] == (p['module'], p['entry']) for f in fns[-1:]):
            fns.append((p['module'], p['entry'], abi[0], abi[1]))
    for name in sorted({p['module'] for p in pins if 'stub_abi' in p}):
        enums = STUB_CONSTS.get(sdk_of(name)[4], ())
        for k in sorted(consts):
            parts = k.split('.')
            if len(parts) >= 2 and parts[-2] in enums and isinstance(consts[k], int) and not isinstance(consts[k], bool):
                stub_consts.append((name, '.'.join(parts[-2:]), consts[k]))
    rep = {'modules': out, 'files': filerep, 'fmod': fmod_info(consts, pins, files), 'sigs': sigs,
           'fns': fns, 'consts': stub_consts}
    if bad:
        rep['unreadable_assemblies'] = bad
    rep['map'] = default_map(rep)
    return rep


def default_map(rep):
    m = {}
    for name, e in rep['modules'].items():
        if e['use'] in ('system', 'keep'):
            continue
        ent = {'use': e['use']}
        if e['use'] == 'stub':
            ent['stub'] = e['stub']
        elif e['use'] == 'host':
            if not e.get('lib'):
                continue
            ent['lib'] = e['lib']
        elif e['use'] == 'lib':
            ent['file'] = 'lib' + norm(name) + '.so'
        m[name] = ent
    return m


def merge_map(rep, port_map):
    """The port's plugin map (JSON object, or a port config with a "plugins" object) over the defaults."""
    m = dict(rep['map'])
    pm = port_map.get('plugins', port_map) if isinstance(port_map, dict) else {}
    for name, ent in pm.items():
        if isinstance(ent, str):
            ent = {'use': ent}
        if not isinstance(ent, dict) or ent.get('use') not in ('lib', 'stub', 'host', 'none'):
            raise ValueError(f'plugin map: {name}: "use" must be lib, stub, host or none')
        m[name] = ent
    return m


STUBS = ('steam', 'csteamworks', 'discord_game_sdk', 'discord-rpc', 'eos', 'wwise')


def map_text(rep, pmap):
    """plugins.map for ua_host (runtime/host/plugins.c)."""
    lines = ['# plugins.map (pmunity android-data; docs/plugins.md): what the runtime gives each DllImport name',
             '#   map <name> lib <file in lib/> | stub <id> | host <system library> | none',
             '#   sig <name> <entry> <result> <args>: argument classes for the softfp thunks of glibc libraries']
    for name in sorted(pmap, key=str.lower):
        e = pmap[name]
        u = e['use']
        if re.search(r'\s', name):
            continue
        if u == 'lib':
            lines.append(f'map {name} lib {e.get("file") or "lib" + norm(name) + ".so"}')
        elif u == 'stub':
            st = e.get('stub') or (sdk_of(name) or [None] * 5)[4]
            if st not in STUBS:
                raise ValueError(f'plugin map: {name}: no stub {st!r} (stubs: {", ".join(STUBS)})')
            lines.append(f'map {name} stub {st}')
        elif u == 'host':
            lines.append(f'map {name} host {e["lib"]}')
        else:
            lines.append(f'map {name} none')
    libs = {n for n, e in pmap.items() if e['use'] in ('lib', 'host')}
    for name, entry, ret, params in rep['sigs']:
        if re.search(r'\s', name + entry) or name not in libs:
            continue
        lines.append(f'sig {name} {entry} {ret} {",".join(params) or "-"}')
    stubbed = {n for n, e in pmap.items() if e['use'] == 'stub' and
               (e.get('stub') or (sdk_of(n) or [None] * 5)[4]) in SIGNATURE_STUBS}
    fns = [f for f in rep.get('fns', []) if f[0] in stubbed and not re.search(r'\s', f[0] + f[1])]
    if fns:
        lines.append('#   fn <name> <entry> <result> <args>, const <name> <constant> <value>: a stub made of the game\'s '
                     'entry points (wwise.c)')
        for name, entry, ret, params in fns:
            lines.append(f'fn {name} {entry} {ret} {",".join(params) or "-"}')
        for name, k, v in rep.get('consts', []):
            if name in stubbed and not re.search(r'\s', k):
                lines.append(f'const {name} {k} {v}')
    return '\n'.join(lines) + '\n'


def summary(rep):
    """Short lines for the converter's log and warnings."""
    out = []
    for name, e in rep['modules'].items():
        if e['use'] in ('system', 'keep'):
            continue
        what = e.get('title') or 'unknown library'
        out.append(f'{name} ({what}, {e["entries"]} entry points): {e["use"]}'
                   + (f' {e["stub"]}' if e.get('stub') else '') + f' - {e["note"]}')
    f = rep.get('fmod')
    if f:
        s = f['supply']
        out.append(f'FMOD {f["header_version"]}: put {", ".join(s["android"]["files"])} of FMOD Engine {s["version"]} '
                   f'for Android ({s["android"]["abi"]}) in {s["android"]["to"]}' + (f'; {f["warning"]}' if f.get('warning') else ''))
        out[-1] = out[-1].replace(' in lib/ (beside libunity.so)', ' in the port\'s lib/, beside libunity.so')
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(prog='pmunity plugins', description=__doc__.split('\n\n')[0])
    ap.add_argument('data', help='the desktop <Game>_Data folder (or Contents/Resources/Data)')
    ap.add_argument('--json', help='write the full report here')
    ap.add_argument('--map', help='write plugins.map (the runtime\'s) here')
    ap.add_argument('--plugin-map', help="the port's plugin map (JSON) over the defaults")
    a = ap.parse_args(argv)
    rep = survey(a.data)
    pmap = merge_map(rep, json.load(open(a.plugin_map))) if a.plugin_map else rep['map']
    for line in summary(rep):
        print(line)
    for f in rep['files']:
        if f.get('note'):
            print(f'{f["file"]} ({f.get("arch") or f["format"]}): {f["note"]}')
    if any(e.get('sdk') == 'wwise' for e in rep['modules'].values()):
        w = wwise_info(a.data)
        if w:
            print(wwise_summary(w))
    if a.json:
        json.dump(dict(rep, map=pmap), open(a.json, 'w'), indent=1, default=sorted)
    if a.map:
        open(a.map, 'w').write(map_text(rep, pmap))


if __name__ == '__main__':
    main()
