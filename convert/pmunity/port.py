"""`python -m pmunity port`: a PortMaster port of a Unity game without writing scripts.

    python -m pmunity port check <game> [--json] [--asmpatch CMD] [--no-scan]
    python -m pmunity port new <name> --data <game> [--config port.toml|.json] [--out DIR]
                               [--route R] [--title T] [--player PKG] [--force]
    python -m pmunity port package <port dir> [--out DIR] [--pybundle FILE] [--runtime DIR]
                                   [--ua-host FILE] [--asmpatch-bin FILE] [--no-build]
    python -m pmunity port routes

<game> is the folder of a desktop build (the one with the executable and <Game>_Data), a macOS
<Game>.app, or the data folder itself; nothing in it is run or changed.

`check` reports what the game is (Unity version, scripting backend, platform build, compressed or
not), what it needs on the handheld (input stacks, mouse-only menus, text input, video, native
plugins, FMOD, store clients) and the route that fits: Unity's Android player (32-bit Mono, this
repository's runtime), IL2CPP to arm64, the x86-64 player under box64, or why none does.

`new` writes the port's folder: the PortMaster launcher, the setup that runs on the handheld on
the first launch (PortMaster's patcher: fetch Unity's player, convert the player's own copy of the
game, pack it, install it), port.json, READMEs, and the settings it was made with (port.toml, the
porter's file; port.build.json, everything resolved). `package` turns that folder into the
release zip: the runtime (ua_host), the converter, the setup's Python, asmpatch when the port uses
it, and every licence. docs/porting.md is the porter's guide; the templates are in
port_template/<route>/ (route.json says what a route renders and ships).

Extension points (for other parts of pmunity, without editing this file): a module
pmunity.<name> for a name in HOOK_MODULES (or $PMUNITY_PORT_HOOKS) may define
  port_check(report)        add to a check's report: report['notes'], report['routes'] (route
                            candidates), report['suggest'] (port config values)
  port_new(ctx)             add files to a port being written (ctx.write, ctx.cfg, ctx.port_dir)
  port_package(ctx)         add files to a release being packed (ctx.add, ctx.license, ctx.cfg)
and a route is a folder port_template/<route>/ with a route.json (docs/porting.md, "Routes").
"""
import argparse
import copy
import fnmatch
import hashlib
import importlib
import importlib.util
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import zipfile

from . import unity_version as UV

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATES = os.path.join(HERE, 'port_template')
REPO = os.path.dirname(os.path.dirname(HERE))          # the portmaster-unity checkout
HOOK_MODULES = ('route', 'box64', 'il2cpp', 'apk', 'services', 'plugins', 'inputtext', 'osk', 'windows', 'streaming')


# =========================================================================== .NET metadata
# Just enough of ECMA-335 (II.22-24) to list what an assembly defines and references: type
# definitions, type and member references, P/Invoke modules, assembly references. Read only.

# table number -> columns: ints are fixed sizes in bytes; 's' string, 'g' guid, 'b' blob heap
# index; ('t', n) an index into table n; ('c', name) a coded index
_CODED = {
    'TypeDefOrRef': (0x02, 0x01, 0x1B),
    'HasConstant': (0x04, 0x08, 0x17),
    'HasCustomAttribute': (0x06, 0x04, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x00, 0x0E, 0x17, 0x14, 0x11, 0x1A, 0x1B,
                           0x20, 0x23, 0x26, 0x27, 0x28, 0x2A, 0x2C, 0x2B),
    'HasFieldMarshal': (0x04, 0x08),
    'HasDeclSecurity': (0x02, 0x06, 0x20),
    'MemberRefParent': (0x02, 0x01, 0x1A, 0x06, 0x1B),
    'HasSemantics': (0x14, 0x17),
    'MethodDefOrRef': (0x06, 0x0A),
    'MemberForwarded': (0x04, 0x06),
    'Implementation': (0x26, 0x23, 0x27),
    'CustomAttributeType': (None, None, 0x06, 0x0A, None),
    'ResolutionScope': (0x00, 0x1A, 0x23, 0x01),
    'TypeOrMethodDef': (0x02, 0x06),
}
_TABLES = {
    0x00: (2, 's', 'g', 'g', 'g'),                                  # Module
    0x01: (('c', 'ResolutionScope'), 's', 's'),                     # TypeRef
    0x02: (4, 's', 's', ('c', 'TypeDefOrRef'), ('t', 0x04), ('t', 0x06)),   # TypeDef
    0x03: (('t', 0x04),),                                           # FieldPtr
    0x04: (2, 's', 'b'),                                            # Field
    0x05: (('t', 0x06),),                                           # MethodPtr
    0x06: (4, 2, 2, 's', 'b', ('t', 0x08)),                         # MethodDef
    0x07: (('t', 0x08),),                                           # ParamPtr
    0x08: (2, 2, 's'),                                              # Param
    0x09: (('t', 0x02), ('c', 'TypeDefOrRef')),                     # InterfaceImpl
    0x0A: (('c', 'MemberRefParent'), 's', 'b'),                     # MemberRef
    0x0B: (2, ('c', 'HasConstant'), 'b'),                           # Constant
    0x0C: (('c', 'HasCustomAttribute'), ('c', 'CustomAttributeType'), 'b'),
    0x0D: (('c', 'HasFieldMarshal'), 'b'),
    0x0E: (2, ('c', 'HasDeclSecurity'), 'b'),
    0x0F: (2, 4, ('t', 0x02)),                                      # ClassLayout
    0x10: (4, ('t', 0x04)),                                         # FieldLayout
    0x11: ('b',),                                                   # StandAloneSig
    0x12: (('t', 0x02), ('t', 0x14)),                               # EventMap
    0x13: (('t', 0x14),),                                           # EventPtr
    0x14: (2, 's', ('c', 'TypeDefOrRef')),                          # Event
    0x15: (('t', 0x02), ('t', 0x17)),                               # PropertyMap
    0x16: (('t', 0x17),),                                           # PropertyPtr
    0x17: (2, 's', 'b'),                                            # Property
    0x18: (2, ('t', 0x06), ('c', 'HasSemantics')),                  # MethodSemantics
    0x19: (('t', 0x02), ('c', 'MethodDefOrRef'), ('c', 'MethodDefOrRef')),  # MethodImpl
    0x1A: ('s',),                                                   # ModuleRef
    0x1B: ('b',),                                                   # TypeSpec
    0x1C: (2, ('c', 'MemberForwarded'), 's', ('t', 0x1A)),          # ImplMap
    0x1D: (4, ('t', 0x04)),                                         # FieldRVA
    0x1E: (4, 4),                                                   # EncLog
    0x1F: (4,),                                                     # EncMap
    0x20: (4, 2, 2, 2, 2, 4, 'b', 's', 's'),                        # Assembly
    0x21: (4,),
    0x22: (4, 4, 4),
    0x23: (2, 2, 2, 2, 4, 'b', 's', 's', 'b'),                      # AssemblyRef
    0x24: (4, ('t', 0x23)),
    0x25: (4, 4, 4, ('t', 0x23)),
    0x26: (4, 's', 'b'),                                            # File
    0x27: (4, 4, 's', 's', ('c', 'Implementation')),                # ExportedType
    0x28: (4, 4, 's', ('c', 'Implementation')),                     # ManifestResource
    0x29: (('t', 0x02), ('t', 0x02)),                               # NestedClass
    0x2A: (2, 2, ('c', 'TypeOrMethodDef'), 's'),                    # GenericParam
    0x2B: (('c', 'MethodDefOrRef'), 'b'),                           # MethodSpec
    0x2C: (('t', 0x2A), ('c', 'TypeDefOrRef')),                     # GenericParamConstraint
}


class NotAssembly(Exception):
    pass


class Assembly:
    """The names a .NET assembly defines and references (no code is loaded or run).

    name, typedefs [(namespace, name)], typerefs [(namespace, name)], methods [name],
    memberrefs [(class namespace, class name, member)], modulerefs [name], pinvokes [(module, entry)],
    assemblyrefs [name]."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        self.path = path
        self._parse()

    # ---- PE and the metadata root
    def _rva(self, rva):
        for va, vsize, raw, rsize in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return raw + rva - va
        raise NotAssembly('RVA outside the sections')

    def _parse(self):
        d = self.data
        if d[:2] != b'MZ' or len(d) < 0x40:
            raise NotAssembly('not a PE file')
        pe, = struct.unpack_from('<I', d, 0x3C)
        if d[pe:pe + 4] != b'PE\0\0':
            raise NotAssembly('no PE signature')
        nsec, = struct.unpack_from('<H', d, pe + 6)
        optsize, = struct.unpack_from('<H', d, pe + 20)
        opt = pe + 24
        magic, = struct.unpack_from('<H', d, opt)
        dd = opt + (96 if magic == 0x10B else 112)
        cli_rva, cli_size = struct.unpack_from('<II', d, dd + 14 * 8)
        if not cli_rva:
            raise NotAssembly('no CLI header (a native library)')
        sec = opt + optsize
        self.sections = []
        for i in range(nsec):
            vsize, va, rsize, raw = struct.unpack_from('<IIII', d, sec + 40 * i + 8)
            self.sections.append((va, vsize, raw, rsize))
        cli = self._rva(cli_rva)
        md_rva, _ = struct.unpack_from('<II', d, cli + 8)
        md = self._rva(md_rva)
        if d[md:md + 4] != b'BSJB':
            raise NotAssembly('no metadata root')
        vlen, = struct.unpack_from('<I', d, md + 12)
        p = md + 16 + vlen
        nstreams, = struct.unpack_from('<H', d, p + 2)
        p += 4
        self.streams = {}
        for _ in range(nstreams):
            off, size = struct.unpack_from('<II', d, p)
            e = d.index(b'\0', p + 8)
            name = d[p + 8:e].decode('ascii')
            self.streams[name] = (md + off, size)
            p = (e + 4) & ~3
        self._tables()

    def _str(self, i):
        off, size = self.streams.get('#Strings', (0, 0))
        if not i or i >= size:
            return ''
        e = self.data.index(b'\0', off + i)
        return self.data[off + i:e].decode('utf-8', 'replace')

    # ---- the #~ (or uncompressed #-) table stream
    def _tables(self):
        d = self.data
        off, _ = self.streams.get('#~') or self.streams.get('#-') or (None, 0)
        if off is None:
            raise NotAssembly('no metadata tables')
        heaps = d[off + 6]
        valid, = struct.unpack_from('<Q', d, off + 8)
        p = off + 24
        rows = {}
        for t in range(64):
            if valid >> t & 1:
                rows[t], = struct.unpack_from('<I', d, p)
                p += 4
        if heaps & 0x40:
            p += 4                                      # extra data
        unknown = [t for t in rows if t not in _TABLES]
        if unknown:
            raise NotAssembly(f'unknown metadata tables {unknown}')
        sz = {'s': 4 if heaps & 1 else 2, 'g': 4 if heaps & 2 else 2, 'b': 4 if heaps & 4 else 2}

        def coded(name):
            tabs = _CODED[name]
            bits = (len(tabs) - 1).bit_length()
            return 2 if max(rows.get(t, 0) for t in tabs if t is not None) < 1 << (16 - bits) else 4

        def colsize(c):
            if isinstance(c, int):
                return c
            if isinstance(c, str):
                return sz[c]
            return (2 if rows.get(c[1], 0) < 1 << 16 else 4) if c[0] == 't' else coded(c[1])

        self.rows, self.layout = rows, {}
        for t in sorted(rows):
            cols = [colsize(c) for c in _TABLES[t]]
            self.layout[t] = (p, cols, sum(cols))
            p += rows[t] * sum(cols)

    def table(self, t):
        """The rows of table t as tuples of ints."""
        if t not in self.layout:
            return []
        p, cols, width = self.layout[t]
        fmt = {1: 'B', 2: 'H', 4: 'I'}
        out = []
        for r in range(self.rows[t]):
            vals, q = [], p + r * width
            for c in cols:
                vals.append(struct.unpack_from('<' + fmt[c], self.data, q)[0])
                q += c
            out.append(tuple(vals))
        return out

    # ---- what the port check wants
    @property
    def name(self):
        a = self.table(0x20)
        return self._str(a[0][7]) if a else os.path.splitext(os.path.basename(self.path))[0]

    @property
    def typedefs(self):
        return [(self._str(r[2]), self._str(r[1])) for r in self.table(0x02)]

    @property
    def typerefs(self):
        return [(self._str(r[2]), self._str(r[1])) for r in self.table(0x01)]

    @property
    def methods(self):
        return [self._str(r[3]) for r in self.table(0x06)]

    @property
    def modulerefs(self):
        return [self._str(r[0]) for r in self.table(0x1A)]

    @property
    def assemblyrefs(self):
        return [self._str(r[6]) for r in self.table(0x23)]

    @property
    def memberrefs(self):
        trefs, tdefs = self.typerefs, self.typedefs
        out = []
        for cls, name, _ in self.table(0x0A):
            tag, idx = cls & 7, cls >> 3
            owner = ('', '')
            if tag == 1 and 0 < idx <= len(trefs):
                owner = trefs[idx - 1]
            elif tag == 0 and 0 < idx <= len(tdefs):
                owner = tdefs[idx - 1]
            elif tag == 2:
                owner = ('<module>', self.modulerefs[idx - 1] if 0 < idx <= len(self.modulerefs) else '')
            out.append((owner[0], owner[1], self._str(name)))
        return out

    @property
    def pinvokes(self):
        mods = self.modulerefs
        return [(mods[r[3] - 1] if 0 < r[3] <= len(mods) else '?', self._str(r[2])) for r in self.table(0x1C)]


def is_platform_assembly(name):
    """Unity's and the class libraries' assemblies (the rest is the game's own code and the
    libraries it ships, e.g. Rewired): the same rule as tools/asmpatch."""
    return (name in ('mscorlib', 'netstandard') or name.startswith(('System', 'Mono.', 'UnityEngine.', 'Unity.', 'Microsoft.'))
            or name == 'UnityEngine')


# =========================================================================== finding the build

def is_data_dir(d):
    return any(os.path.isfile(os.path.join(d, n)) for n in ('globalgamemanagers', 'mainData', 'data.unity3d'))


def _exe_kind(path):
    """('linux'|'windows'|'macos', arch) of an executable's header, or None. Read only."""
    try:
        with open(path, 'rb') as f:
            h = f.read(4096)
    except OSError:
        return None
    if h[:4] == b'\x7fELF' and len(h) > 20:
        if h[16] != 2 and h[16] != 3:                   # ET_EXEC / ET_DYN
            return None
        m, = struct.unpack_from('<H', h, 18)
        return 'linux', {0x3E: 'x86_64', 0x03: 'x86', 0xB7: 'aarch64', 0x28: 'armhf'}.get(m, hex(m))
    if h[:2] == b'MZ' and len(h) > 0x40:
        pe, = struct.unpack_from('<I', h, 0x3C)
        if pe + 6 <= len(h) and h[pe:pe + 4] == b'PE\0\0':
            m, = struct.unpack_from('<H', h, pe + 4)
            return 'windows', {0x8664: 'x86_64', 0x14C: 'x86', 0xAA64: 'aarch64'}.get(m, hex(m))
    if h[:4] in (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe'):
        return 'macos', 'universal' if h[:4] == b'\xca\xfe\xba\xbe' else 'x86_64/arm64'
    return None


def describe_build(data):
    """What kind of desktop build a data folder belongs to: platform, executable, architecture,
    scripting backend."""
    data = os.path.abspath(data)
    b = {'data': data, 'build': 'unknown', 'exe': None, 'arch': None}
    parent = os.path.dirname(data)
    if data.endswith(os.path.join('Contents', 'Resources', 'Data')):
        app = os.path.dirname(os.path.dirname(parent))
        b.update(build='macos', root=app)
        macos = os.path.join(app, 'Contents', 'MacOS')
        for n in sorted(os.listdir(macos)) if os.path.isdir(macos) else []:
            k = _exe_kind(os.path.join(macos, n))
            if k:
                b.update(exe=n, arch=k[1])
                break
        il2cpp = os.path.isfile(os.path.join(app, 'Contents', 'Frameworks', 'GameAssembly.dylib'))
    else:
        b['root'] = parent
        base = os.path.basename(data)[:-5] if data.endswith('_Data') else None
        cands = []
        if base:
            cands = [base + e for e in ('.x86_64', '.x86', '.x64', '.exe', '')]
        cands += sorted(n for n in os.listdir(parent) if n not in cands)
        for n in cands:
            p = os.path.join(parent, n)
            if os.path.isfile(p) and not n.endswith(('.so', '.dll', '.debug', '.dylib', '.txt')):
                k = _exe_kind(p)
                if k:
                    b.update(build=k[0], exe=n, arch=k[1])
                    break
        if b['build'] == 'unknown':
            if os.path.isfile(os.path.join(parent, 'UnityPlayer.dll')):
                b['build'] = 'windows'
            elif os.path.isfile(os.path.join(parent, 'UnityPlayer.so')):
                b['build'] = 'linux'
        il2cpp = any(os.path.isfile(os.path.join(parent, n)) for n in ('GameAssembly.so', 'GameAssembly.dll'))
    managed = os.path.join(data, 'Managed')
    has_managed = os.path.isdir(managed) and any(n.endswith('.dll') for n in os.listdir(managed))
    il2cpp = il2cpp or os.path.isdir(os.path.join(data, 'il2cpp_data'))
    b['backend'] = 'mono' if has_managed and not il2cpp else 'il2cpp' if il2cpp else 'unknown'
    b['compressed'] = not os.path.isfile(os.path.join(data, 'globalgamemanagers')) and \
        os.path.isfile(os.path.join(data, 'data.unity3d'))
    return b


def find_builds(path, depth=4):
    """Desktop builds (describe_build) under path: a data folder, a .app, a game folder."""
    path = os.path.abspath(path)
    if os.path.isfile(path):
        return []
    if is_data_dir(path):
        return [describe_build(path)]
    if path.endswith('.app') and is_data_dir(os.path.join(path, 'Contents', 'Resources', 'Data')):
        return [describe_build(os.path.join(path, 'Contents', 'Resources', 'Data'))]
    out = []
    base = path.count(os.sep)
    for r, ds, fs in os.walk(path):
        ds[:] = sorted(x for x in ds if not x.startswith('.') and x != '__MACOSX')
        if is_data_dir(r):
            out.append(describe_build(r))
            ds[:] = []
            continue
        if r.count(os.sep) - base >= depth:
            ds[:] = []
    return out


def android_packages(path, depth=4):
    """Android builds (.apk files) under path, for `pmunity apk` (the Android-build route)."""
    if os.path.isfile(path):
        return [path] if path.lower().endswith('.apk') else []
    out = []
    base = os.path.abspath(path).count(os.sep)
    for r, ds, fs in os.walk(path):
        ds[:] = sorted(x for x in ds if not x.startswith('.'))
        out += [os.path.join(r, n) for n in sorted(fs) if n.lower().endswith('.apk')]
        if r.count(os.sep) - base >= depth:
            ds[:] = []
    return out


def dir_size(d):
    total = 0
    for r, _, fs in os.walk(d):
        for n in fs:
            try:
                total += os.lstat(os.path.join(r, n)).st_size
            except OSError:
                pass
    return total


# =========================================================================== what the code uses

# (label, matcher on (namespace, name) of referenced/defined types)
_SERVICES = [
    ('Steamworks', lambda ns, n: ns == 'Steamworks' and n in ('SteamAPI', 'SteamClient', 'SteamUser') or
     ns.startswith('Facepunch.Steamworks')),
    ('GOG Galaxy', lambda ns, n: ns.startswith('Galaxy.Api')),
    ('Discord', lambda ns, n: ns.startswith(('DiscordRPC', 'Discord')) and n in ('DiscordRpcClient', 'Discord', 'DiscordRpc')),
    ('Epic Online Services', lambda ns, n: ns.startswith('Epic.OnlineServices')),
    ('FMOD Studio', lambda ns, n: ns in ('FMODUnity', 'FMOD.Studio') or (ns == '' and n == 'FMODStudioSystem')),
    ('Wwise', lambda ns, n: n in ('AkSoundEngine', 'AkInitializer')),
    ('CRIWARE', lambda ns, n: ns.startswith('CriWare') or n.startswith('CriAtom')),
    ('Unity Ads / IAP / Analytics', lambda ns, n: ns.startswith(('UnityEngine.Advertisements', 'UnityEngine.Purchasing'))),
    ('Unity Netcode / Mirror / Photon networking', lambda ns, n: ns.startswith(('Mirror', 'Photon', 'Unity.Netcode'))),
]
_TEXT_TYPES = {('UnityEngine.UI', 'InputField'): 'UI InputField', ('TMPro', 'TMP_InputField'): 'TextMeshPro InputField',
               ('UnityEngine', 'TouchScreenKeyboard'): 'TouchScreenKeyboard',
               ('UnityEngine.UIElements', 'TextField'): 'UI Toolkit TextField'}
_LEGACY = {
    'axes/buttons': ('GetAxis', 'GetAxisRaw', 'GetButton', 'GetButtonDown', 'GetButtonUp'),
    'keys': ('GetKey', 'GetKeyDown', 'GetKeyUp', 'get_anyKey', 'get_anyKeyDown', 'get_inputString'),
    'mouse': ('GetMouseButton', 'GetMouseButtonDown', 'GetMouseButtonUp', 'get_mousePosition', 'get_mouseScrollDelta'),
    'touch': ('get_touchCount', 'GetTouch', 'get_touches'),
}
_MOUSE_MSGS = ('OnMouseDown', 'OnMouseUp', 'OnMouseUpAsButton', 'OnMouseEnter', 'OnMouseExit', 'OnMouseOver', 'OnMouseDrag')
_POINTER_IFACES = ('IPointerClickHandler', 'IPointerDownHandler', 'IPointerUpHandler', 'IPointerEnterHandler',
                   'IPointerExitHandler', 'IDragHandler', 'IBeginDragHandler', 'IScrollHandler')
_NAV_IFACES = ('ISubmitHandler', 'ISelectHandler', 'IMoveHandler', 'ICancelHandler')
# P/Invoke modules every desktop build has (Unity's own, the C library) and those the runtime answers
_PINVOKE_BENIGN = re.compile(r'^(__Internal|libc|libc\.so\.6|libdl|kernel32|user32|gdi32|shell32|ole32|advapi32|dwmapi|'
                             r'msvcrt|psapi|winmm|ntdll|libSystem|libobjc|/usr/lib/libobjc|MonoPosixHelper|libMonoPosixHelper|'
                             r'System\.Native|libSystem\.Native|System\.Globalization\.Native|libmono.*|mono.*|'
                             r'SDL2|libSDL2.*|X11|libX11.*|libudev.*|udev)(\.dll|\.so.*|\.dylib)?$', re.I)


def survey_assemblies(managed):
    """The game's own code (not Unity's or the class libraries): input stacks, text input, video,
    P/Invoke modules, store/audio/network services. From names only: what the assemblies define
    and reference (the same questions as `asmpatch --survey`, without .NET)."""
    s = {'assemblies': [], 'libraries': [], 'unreadable': [], 'rewired': None, 'incontrol': False,
         'input_system': False, 'legacy': {k: 0 for k in _LEGACY}, 'mouse_messages': [], 'pointer_handlers': 0,
         'navigation': 0, 'selects': 0, 'text_input': [], 'video': [], 'pinvoke': {}, 'services': [],
         'cursor': [], 'screen': []}
    if not os.path.isdir(managed):
        return s
    names = sorted(n for n in os.listdir(managed) if n.endswith('.dll'))
    s['libraries'] = [n[:-4] for n in names if is_platform_assembly(n[:-4])]
    s['input_system'] = 'Unity.InputSystem.dll' in names
    services = set()
    for n in names:
        if is_platform_assembly(n[:-4]):
            continue
        try:
            a = Assembly(os.path.join(managed, n))
            tdefs, trefs, mrefs, methods = a.typedefs, a.typerefs, a.memberrefs, a.methods
        except (NotAssembly, struct.error, ValueError, IndexError) as ex:
            s['unreadable'].append(f'{n}: {ex}')
            continue
        s['assemblies'].append(n[:-4])
        types = set(tdefs) | set(trefs)
        for ns, tn in tdefs:
            if ns == 'Rewired' and tn == 'InputManager' or tn == 'InputManager_Base' and ns == 'Rewired':
                ed = 'source edition' if n.startswith('Assembly-CSharp') else 'DLL edition'
                s['rewired'] = s['rewired'] or f'{n} ({ed})'
            if ns.startswith('InControl') and tn in ('InputManager', 'InControlManager'):
                s['incontrol'] = True
        if any(ns.startswith('InControl') for ns, _ in trefs):
            s['incontrol'] = True
        for (ns, tn), label in _TEXT_TYPES.items():
            if (ns, tn) in types and label not in s['text_input']:
                s['text_input'].append(label)
        for ns, tn in types:
            if ns == 'UnityEngine.Video' and tn == 'VideoPlayer' and 'VideoPlayer' not in s['video']:
                s['video'].append('VideoPlayer')
            if tn == 'MovieTexture' and 'MovieTexture' not in s['video']:
                s['video'].append('MovieTexture')
            if ns == 'UnityEngine.EventSystems' and tn in _POINTER_IFACES:
                s['pointer_handlers'] += 1
            if ns == 'UnityEngine.EventSystems' and tn in _NAV_IFACES:
                s['navigation'] += 1
            for label, match in _SERVICES:
                if match(ns, tn):
                    services.add(label)
        for cns, cn, m in mrefs:
            if (cns, cn) == ('UnityEngine', 'Input'):
                for g, ms in _LEGACY.items():
                    if m in ms:
                        s['legacy'][g] += 1
                if m == 'get_inputString' and 'Input.inputString' not in s['text_input']:
                    s['text_input'].append('Input.inputString')
            if m in ('SetSelectedGameObject', 'set_firstSelectedGameObject') and cn == 'EventSystem' or \
                    m == 'Select' and cns == 'UnityEngine.UI':
                s['selects'] += 1
            if (cns, cn) == ('UnityEngine', 'Cursor') and m not in s['cursor']:
                s['cursor'].append(m)
            if (cns, cn) == ('UnityEngine', 'Screen') and m in ('SetResolution', 'set_fullScreen', 'set_fullScreenMode') \
                    and m not in s['screen']:
                s['screen'].append(m)
            if (cns, cn) == ('UnityEngine', 'Handheld') and m == 'PlayFullScreenMovie' and 'Handheld' not in s['video']:
                s['video'].append('Handheld.PlayFullScreenMovie')
        s['mouse_messages'] += sorted({m for m in methods if m in _MOUSE_MSGS})
        for mod, entry in a.pinvokes:
            s['pinvoke'].setdefault(mod, set()).add(n[:-4])
    s['mouse_messages'] = sorted(set(s['mouse_messages']))
    s['pinvoke'] = {m: sorted(v) for m, v in sorted(s['pinvoke'].items())}
    s['services'] = sorted(services)
    if 'Rewired_Core.dll' in names and not s['rewired']:
        s['rewired'] = 'Rewired_Core.dll (no InputManager found)'
    return s


def input_verdict(s):
    """pointer | mixed | pad | none, as asmpatch --survey's verdict line."""
    pointer = s['legacy']['mouse'] > 0 or s['mouse_messages'] or s['pointer_handlers'] > 0
    nav = s['selects'] > 0 or s['navigation'] > 0
    pad = s['rewired'] or s['incontrol'] or s['legacy']['axes/buttons'] > 0 or nav
    if pointer and not nav:
        return 'pointer'
    if pointer:
        return 'mixed'
    return 'pad' if pad else 'none'


def find_asmpatch(explicit=None):
    """The asmpatch command for this machine: --asmpatch, $PMUNITY_ASMPATCH, a build in
    tools/asmpatch/out, or None."""
    if explicit:
        return explicit
    if os.environ.get('PMUNITY_ASMPATCH'):
        return os.environ['PMUNITY_ASMPATCH']
    out = os.path.join(REPO, 'tools', 'asmpatch', 'out')
    import platform
    if sys.platform.startswith('linux') and platform.machine() in ('aarch64', 'arm64') and \
            os.access(os.path.join(out, 'linux-arm64', 'asmpatch'), os.X_OK):
        return shlex.quote(os.path.join(out, 'linux-arm64', 'asmpatch'))
    dll = os.path.join(out, 'any', 'asmpatch.dll')
    if os.path.isfile(dll):
        dotnet = shutil.which('dotnet') or os.path.expanduser('~/.dotnet/dotnet')
        if os.access(dotnet, os.X_OK):
            return f'{shlex.quote(dotnet)} {shlex.quote(dll)}'
    return None


def asmpatch_survey(cmd, managed):
    """(survey text, patches that would apply) from asmpatch, or (None, None)."""
    try:
        sv = subprocess.run(shlex.split(cmd) + ['--survey', managed], capture_output=True, text=True, timeout=300)
        ls = subprocess.run(shlex.split(cmd) + ['--list', managed], capture_output=True, text=True, timeout=300)
    except (OSError, subprocess.TimeoutExpired):
        return None, None
    if sv.returncode != 0:
        return None, None
    would = [m.group(1) for m in re.finditer(r'^would\s+([\w-]+):', ls.stdout, re.M)]
    return sv.stdout, would


# =========================================================================== what the data holds

_GFX = {0: 'OpenGL2', 1: 'Direct3D9', 2: 'Direct3D11', 4: 'Null', 8: 'OpenGLES2', 11: 'OpenGLES3', 16: 'Metal',
        17: 'OpenGLCore', 18: 'Direct3D12', 21: 'Vulkan'}
_KNOWN_PLUGINS = [
    (r'steam_api', 'Steamworks: the runtime has a stub (Steam not running: SteamAPI_Init fails, most games go on)'),
    (r'fmod', 'FMOD: needs an ARM (armhf) build of FMOD; without it the game is silent or stops'),
    (r'_burst_generated', 'Burst: falls back to the managed code (slower, same results)'),
    (r'discord', 'Discord: not on the handheld (calls fail)'),
    (r'galaxy', 'GOG Galaxy: not on the handheld (the game must go on without it)'),
    (r'AkSoundEngine|wwise', 'Wwise: needs an ARM build of Wwise'),
    (r'cri_ware|criware', 'CRIWARE: needs an ARM build'),
    (r'EOSSDK', 'Epic Online Services: not on the handheld'),
]


def _plugin_files(build):
    """Native libraries a build ships (Plugins/, the macOS bundle's PlugIns/)."""
    out = []
    dirs = [os.path.join(build['data'], 'Plugins')]
    if build['build'] == 'macos':
        dirs.append(os.path.join(build['root'], 'Contents', 'PlugIns'))
    for d in dirs:
        for r, ds, fs in os.walk(d):
            for n in fs + [x for x in ds if x.endswith('.bundle')]:
                if n.endswith(('.so', '.dll', '.dylib', '.bundle')) or '.so.' in n:
                    out.append(os.path.relpath(os.path.join(r, n), build['data']))
            ds[:] = [x for x in ds if not x.endswith('.bundle')]
    return sorted(out)


def plugin_notes(names):
    out, seen = [], {}
    for n in names:                             # one line per library (Plugins/x86 and x86_64 alike)
        seen.setdefault(os.path.basename(n), []).append(n)
    names = [v[0] for v in seen.values()]
    for n in names:
        b = os.path.basename(n)
        if re.match(r'ScreenSelector\.', b):
            continue                            # (the Linux player's resolution dialog: not used)
        note = next((t for rx, t in _KNOWN_PLUGINS if re.search(rx, b, re.I)), 'not shipped: a game that calls it needs an ARM build')
        out.append((n, note))
    return out


def survey_data(data, deep=True):
    """From the serialized files (UnityPy, read only): player settings, the scripts scenes use
    (input modules, text fields), video, texture formats, scenes."""
    r = {'product': None, 'company': None, 'graphics_apis': None, 'color_space': None, 'active_input': None,
         'screen': None, 'scenes': None, 'scripts_used': {}, 'video_players': 0, 'video_clips': {},
         'movie_textures': 0, 'textures': {}, 'notes': []}
    try:
        import UnityPy
    except ImportError:
        r['notes'].append('UnityPy is not installed: settings and scenes not read (pip install -r convert/requirements.txt)')
        return r
    ggm = os.path.join(data, 'globalgamemanagers')
    if os.path.isfile(ggm):
        try:
            env = UnityPy.load(ggm)
            for o in env.objects:
                n = o.type.name
                if n == 'PlayerSettings':
                    t = o.read_typetree()
                    r['product'], r['company'] = t.get('productName'), t.get('companyName')
                    r['screen'] = [t.get('defaultScreenWidth'), t.get('defaultScreenHeight')]
                    r['color_space'] = {0: 'gamma', 1: 'linear'}.get(t.get('m_ActiveColorSpace'))
                    if 'activeInputHandler' in t:
                        r['active_input'] = {0: 'Input Manager', 1: 'Input System package', 2: 'both'}.get(t['activeInputHandler'])
                elif n == 'BuildSettings':
                    t = o.read_typetree()
                    r['graphics_apis'] = list(t.get('m_GraphicsAPIs') or [])
                    r['scenes'] = len(t.get('scenes') or t.get('levels') or t.get('m_Scenes') or [])
        except Exception as ex:                       # (layouts of an unknown version)
            r['notes'].append(f'globalgamemanagers: not read ({type(ex).__name__}: {ex})')
    if not deep:
        return r
    # scripts: MonoScripts (globalgamemanagers.assets), and which of them scenes and assets use
    scripts = {}
    files = sorted(n for n in os.listdir(data) if re.match(r'^(level\d+|sharedassets\d+\.assets|resources\.assets|'
                                                          r'globalgamemanagers\.assets)$', n))
    loaded = {}
    for n in files:
        try:
            loaded[n] = UnityPy.load(os.path.join(data, n))
        except Exception as ex:
            r['notes'].append(f'{n}: not read ({type(ex).__name__})')
    for n, env in loaded.items():
        for o in env.objects:
            if o.type.name == 'MonoScript':
                try:
                    t = o.read_typetree()
                    scripts[(n, o.path_id)] = (t.get('m_Namespace') or '', t.get('m_ClassName') or '', t.get('m_AssemblyName') or '')
                except Exception:
                    pass
    used = {}
    for n, env in loaded.items():
        sf = env.file
        for o in env.objects:
            tn = o.type.name
            try:
                if tn == 'MonoBehaviour':
                    mb = o.read(check_read=False)
                    fid, pid = mb.m_Script.m_FileID, mb.m_Script.m_PathID
                    src = n if fid == 0 else os.path.basename(sf.externals[fid - 1].path)
                    k = scripts.get((src, pid))
                    if k:
                        used[k] = used.get(k, 0) + 1
                elif tn == 'VideoPlayer':
                    r['video_players'] += 1
                elif tn == 'MovieTexture':
                    r['movie_textures'] += 1
                elif tn == 'Texture2D':
                    t = o.read(check_read=False)
                    f = t.m_TextureFormat
                    try:
                        from UnityPy.enums import TextureFormat
                        f = TextureFormat(int(f)).name
                    except (ValueError, TypeError, ImportError):
                        f = getattr(f, 'name', str(f))
                    st = r['textures'].setdefault(f, [0, 0])
                    st[0] += 1
                    st[1] += int(getattr(t, 'm_CompleteImageSize', 0) or 0)
            except Exception:
                continue
    r['scripts_used'] = {f'{ns}.{cn}' if ns else cn: c for (ns, cn, _), c in sorted(used.items())}
    try:
        from . import video
        r['video_clips'] = video.clip_codecs(data)
    except Exception as ex:
        r['notes'].append(f'video clips: not read ({type(ex).__name__})')
    return r


_INPUT_MODULES = {
    'UnityEngine.EventSystems.StandaloneInputModule': 'Unity UI, Input Manager (pad navigation and mouse)',
    'UnityEngine.InputSystem.UI.InputSystemUIInputModule': 'Unity UI, Input System package',
    'Rewired.Integration.UnityUI.RewiredStandaloneInputModule': 'Unity UI, Rewired',
    'Rewired.Integration.UnityUI.RewiredEventSystem': 'Rewired event system',
    'InControl.InControlInputModule': 'Unity UI, InControl',
    'UnityEngine.EventSystems.TouchInputModule': 'Unity UI, touch',
}


# =========================================================================== the check

ROUTE_TEXT = {
    'android': "Unity's Android player (armv7, Mono) on the handheld's 32-bit userland: this repository's runtime",
    'il2cpp-arm64': 'the game compiled with IL2CPP into an arm64 Android player (for 64-bit-only systems; `pmunity il2cpp`, coming)',
    'box64': "the game's own x86-64 Linux player under box64 (tools/box64, `pmunity route`)",
    'apk': "the game's own Android build (`pmunity apk`)",
}


def _hook_modules():
    names = list(HOOK_MODULES) + [m for m in os.environ.get('PMUNITY_PORT_HOOKS', '').split(',') if m]
    out = []
    for n in names:
        full = n if '.' in n else f'{__package__}.{n}'
        try:
            if importlib.util.find_spec(full) is None:
                continue
            out.append(importlib.import_module(full))
        except Exception:
            continue
    return out


def _call_hooks(fn, arg):
    for m in _hook_modules():
        f = getattr(m, fn, None)
        if callable(f):
            try:
                f(arg)
            except Exception as ex:
                if isinstance(arg, dict):
                    arg.setdefault('notes', []).append(f'{m.__name__}.{fn}: {type(ex).__name__}: {ex}')
                else:
                    raise


def decide_routes(rep):
    """Route candidates [{route, ok, why, needs}] and the recommended one."""
    b, routes = rep, []
    v = rep.get('unity_version')
    supported = rep.get('unity_supported')
    mono = b.get('backend') == 'mono'
    gl = rep.get('shader_source')
    # Android player, Mono
    why, needs = [], []
    if not mono:
        why.append(f"scripting backend {b.get('backend')}: the game's code is compiled for its desktop platform "
                   f"(no managed assemblies for the Mono player)")
    if v and not supported:
        why.append(f'Unity {v}: the converter supports {UV.FIRST[0]}.{UV.FIRST[1]} - {UV.LAST[0]}.{UV.LAST[1]}')
    if gl is None:
        why.append('no shader programs the converter translates: ' + (rep.get('shader_why') or 'unknown graphics APIs'))
    elif gl == 'd3d11':
        needs.append('Direct3D 11 shaders: `shaders.from = ["d3d11"]` (tools/hlslcc; pmhlslcc for the handheld)')
    if b.get('compressed'):
        needs.append('a compressed build: the setup unpacks it first (pmunity unpack)')
    routes.append({'route': 'android', 'ok': not why, 'why': why, 'needs': needs})
    # IL2CPP to arm64 (64-bit only systems)
    routes.append({'route': 'il2cpp-arm64', 'ok': False, 'available': False,
                   'why': ['not available yet (il2cpp-build: `pmunity il2cpp`)'] + ([] if mono else ['needs a Mono build']),
                   'needs': []})
    # box64: the game's own x86-64 Linux player
    why = []
    if b.get('build') != 'linux':
        why.append(f"needs the game's Linux build (this one is {b.get('build')})")
    elif b.get('arch') not in ('x86_64', None):
        why.append(f"the Linux build is {b.get('arch')} (box64 runs x86-64; box86 would be needed)")
    routes.append({'route': 'box64', 'ok': not why, 'available': os.path.isfile(os.path.join(TEMPLATES, 'box64', 'route.json')),
                   'why': why, 'needs': ['box64 and the player\'s x86-64 libraries on the handheld (tools/box64)'] if not why else []})
    rep['routes'] = routes
    _call_hooks('port_check', rep)              # (other modules may refine or add routes)
    best = next((r for r in rep['routes'] if r['ok'] and r.get('available', True)), None) or \
        next((r for r in rep['routes'] if r['ok']), None)
    rep['route'] = best['route'] if best else None
    if not best:
        rep['unsupported'] = '; '.join(w for r in rep['routes'] for w in r['why'][:1]) or 'no route fits'
    return rep


def check(path, asmpatch=None, deep=True, log=None):
    """The port check's report (a dict; print_report formats it)."""
    log = log or (lambda m: None)
    rep = {'path': os.path.abspath(path), 'notes': [], 'warnings': [], 'suggest': {}, 'other_builds': []}
    builds = find_builds(path)
    if not builds:
        apks = android_packages(path)
        if apks:
            rep.update(build='android', apk=apks[0], backend=None, routes=[{'route': 'apk', 'ok': False, 'why': [
                'an Android build: `pmunity apk` (the Android-build route) reads it; not part of this check yet'], 'needs': []}])
            _call_hooks('port_check', rep)
            rep['route'] = next((r['route'] for r in rep['routes'] if r['ok']), None)
            if not rep['route']:
                rep['unsupported'] = rep['routes'][0]['why'][0]
            return rep
        raise SystemExit(f'{path}: no Unity game here (no folder with globalgamemanagers, mainData or data.unity3d)')
    # prefer a Mono build with OpenGL Core shaders (Linux, macOS), then any Mono build
    order = {'linux': 0, 'macos': 1, 'windows': 2}
    builds.sort(key=lambda b: (b['backend'] != 'mono', order.get(b['build'], 3)))
    b = builds[0]
    rep['other_builds'] = [{k: x[k] for k in ('data', 'build', 'backend')} for x in builds[1:]]
    rep.update(b)
    rep['size_mb'] = int(dir_size(b['root']) / 2 ** 20 + 0.5)
    rep['data_mb'] = int(dir_size(b['data']) / 2 ** 20 + 0.5)
    try:
        v = UV.of_data(b['data'])
        rep['unity_version'] = str(v)
        rep['unity_line'] = f'{v.major}.{v.minor}'
        rep['unity_supported'] = UV.FIRST <= v.line <= UV.LAST
    except (UV.Unsupported, ValueError, OSError) as ex:
        rep['unity_version'], rep['unity_supported'] = None, False
        rep['warnings'].append(f'Unity version not read: {ex}')
        v = None
    if v is not None and b['backend'] == 'mono':
        from .android_data import scripting_runtime
        try:
            rep['scripting_runtime'] = scripting_runtime(b['data'], v)
        except Exception:
            rep['scripting_runtime'] = None
    log('reading the game\'s settings and scenes')
    d = survey_data(b['data'], deep and not b['compressed']) if not b['compressed'] else \
        {'notes': ['a compressed build: settings and scenes are read after `pmunity unpack`'], 'graphics_apis': None,
         'scripts_used': {}, 'textures': {}, 'video_clips': {}, 'video_players': 0, 'movie_textures': 0}
    rep['data_survey'] = d
    rep['notes'] += d.get('notes', [])
    rep['product'] = d.get('product')
    # shaders: OpenGL Core programs (macOS, Linux) or Direct3D 11 (Windows)
    apis = d.get('graphics_apis')
    rep['graphics_apis'] = [_GFX.get(a, str(a)) for a in apis] if apis is not None else None
    if apis is None:
        rep['shader_source'] = {'windows': 'd3d11'}.get(b['build'], 'glcore')
        if b['compressed'] or b['backend'] != 'mono':
            pass
        else:
            rep['warnings'].append('graphics APIs not read: assuming ' + rep['shader_source'])
    elif 17 in apis:
        rep['shader_source'] = 'glcore'
    elif 2 in apis:
        rep['shader_source'] = 'd3d11'
    else:
        rep['shader_source'] = None
        rep['shader_why'] = (f'the build has {", ".join(rep["graphics_apis"]) or "no"} shaders only; '
                             f'the converter translates OpenGL Core (macOS/Linux builds made with OpenGL Core) and Direct3D 11')
    # the code
    managed = os.path.join(b['data'], 'Managed')
    s = survey_assemblies(managed) if b['backend'] == 'mono' else None
    rep['code'] = s
    if s is not None:
        rep['input_verdict'] = input_verdict(s)
        cmd = find_asmpatch(asmpatch)
        if cmd:
            log('asmpatch --survey')
            text, would = asmpatch_survey(cmd, managed)
            if text:
                rep['asmpatch_survey'] = text
                rep['asmpatch_would'] = would
                m = re.search(r'^verdict: (.*)$', text, re.M)
                if m:
                    rep['input_verdict_text'] = m.group(1)
    # plugins
    rep['plugins'] = plugin_notes(_plugin_files(b))
    decide_routes(rep)
    suggest_config(rep)
    return rep


def suggest_config(rep):
    """Port config values the check suggests (port new starts from them)."""
    s, d = rep.get('code') or {}, rep.get('data_survey') or {}
    sug = rep.setdefault('suggest', {})
    rt = sug.setdefault('runtime', {})
    ad = sug.setdefault('android_data', {})
    cv = sug.setdefault('convert', {})
    notes = rep['notes']
    if rep.get('shader_source') == 'd3d11':
        cv.setdefault('shaders', {})['from'] = ['d3d11']
    if s.get('rewired'):
        ad['asmpatch'] = 'auto'
        rt.setdefault('UA_PAD_NAME', 'Xbox Wireless Controller')
        notes.append(f"Rewired ({s['rewired']}): asmpatch's rewired-android patch (Rewired then reads the pad "
                     f"through Unity's Android input); UA_PAD_NAME names the pad as one Rewired's Android "
                     f"definitions know")
    else:
        ad['asmpatch'] = 'off'
    verdict = rep.get('input_verdict')
    if verdict == 'pointer':
        rt['UA_POINTER'] = '1'
        notes.append('menus look mouse-only (the code reads the mouse and selects no UI element for pad '
                     'navigation): pointer mode, UA_POINTER=1; if the game draws its own cursor, also '
                     'UA_POINTER_CURSOR=0')
    elif verdict == 'mixed':
        notes.append('reads the mouse and also selects UI elements: try the pad first; UA_POINTER=1 '
                     '(with UA_POINTER_TOGGLE) where menus need the mouse')
    if s.get('legacy', {}).get('keys'):
        notes.append(f"reads keyboard keys (Input.GetKey, {s['legacy']['keys']} references): map pad buttons "
                     f"with UA_KEYMAP (e.g. start:escape) where the pad has no binding")
    if s.get('input_system') or d.get('active_input') in ('Input System package', 'both'):
        notes.append(f"Input System package ({d.get('active_input') or 'Unity.InputSystem.dll'}): its desktop "
                     f"build has no Android pad layouts (docs/input.md); pads may need more than this")
    if s.get('text_input') or any(k.endswith('InputField') for k in d.get('scripts_used', {})):
        used = [k for k in d.get('scripts_used', {}) if k.endswith(('InputField', 'TextField'))]
        notes.append('text input (' + ', '.join(s.get('text_input', []) + [f'{k} in scenes' for k in used]) +
                     '): typing needs an on-screen keyboard or a default text')
    if d.get('color_space') == 'linear':
        notes.append('linear colour space: with the SDL display, UA_DISPLAY_SRGB=1 (docs/runtime.md)')
    vids = d.get('video_clips') or {}
    if vids or d.get('video_players'):
        notes.append(f"video: {d.get('video_players', 0)} VideoPlayers in the main data, clips {vids or 'none there'}: "
                     f"android-data adds Hidden/VideoDecodeAndroid (keep android_data.video_shader on); VP8/VP9 "
                     f"and H.264 decode built in, AAC/HEVC/MP3 only with the system's libavcodec (else silent)")
    elif 'VideoPlayer' in s.get('video', []):
        notes.append('the code uses VideoPlayer, but the main data has no clips or players (videos from bundles '
                     'or URLs, or none at all): keep android_data.video_shader on')
    if d.get('movie_textures') or 'MovieTexture' in s.get('video', []):
        rep['warnings'].append('MovieTexture (the legacy movie player): Unity\'s Android player has none; those videos do not play')
    pl = rep.get('plugins') or []
    if pl:
        notes.append('native plugins: ' + '; '.join(f'{os.path.basename(n)}: {t}' for n, t in pl))
    pinv = {m: a for m, a in (s.get('pinvoke') or {}).items() if not _PINVOKE_BENIGN.match(m)}
    if pinv:
        notes.append('P/Invoke into ' + ', '.join(f'{m} ({", ".join(a)})' for m, a in pinv.items()) +
                     ': each needs an ARM library or a stub')
    if s.get('services'):
        notes.append('services: ' + ', '.join(s['services']))
    tex = d.get('textures') or {}
    bc = sum(v[1] for k, v in tex.items() if k.startswith(('DXT', 'BC')))
    crunched = sum(v[0] for k, v in tex.items() if k.endswith('Crunched'))
    if bc > 64 << 20:
        notes.append(f'{bc / 2**20:.0f} MB of BC/DXT textures (main data): the handheld GPU cannot sample them, '
                     f'so Unity decodes them to RGBA when loading (4-8x the memory). The textures step '
                     f'(convert.steps += "textures") re-encodes them as ASTC: less memory, slightly different '
                     f'pixels, a longer setup; off by default')
    if crunched:
        notes.append(f'{crunched} crunched textures: as ASTC (the textures step) they take their full size on the '
                     f'card (several times the crunched size)')
    if rep.get('size_mb', 0) > 600:
        notes.append(f"{rep['size_mb']} MB: the handheld's setup needs about 2-3 times that free while it runs")
    return sug


def print_report(rep, out=sys.stdout):
    w = lambda s='': print(s, file=out)
    w(f"game: {rep.get('product') or os.path.basename(rep.get('data') or rep['path'])}")
    if rep.get('build') == 'android':
        w(f"  Android build: {rep['apk']}")
    else:
        w(f"  data: {rep['data']} ({rep.get('data_mb')} MB; the build: {rep.get('size_mb')} MB)")
        w(f"  build: {rep['build']}" + (f", {rep['exe']} ({rep['arch']})" if rep.get('exe') else '') +
          (', compressed (data.unity3d)' if rep.get('compressed') else ''))
        w(f"  Unity {rep.get('unity_version')}" + ('' if rep.get('unity_supported') else ' (outside the converter\'s range)') +
          f", {rep.get('backend')}" + (f", {rep['scripting_runtime']}" if rep.get('scripting_runtime') else ''))
        if rep.get('graphics_apis') is not None:
            w(f"  graphics APIs: {', '.join(rep['graphics_apis']) or 'none listed'} -> shaders from {rep.get('shader_source')}")
        ds = rep.get('data_survey') or {}
        if ds.get('scenes') is not None:
            w(f"  scenes: {ds['scenes']}; screen {ds.get('screen')}; colour space {ds.get('color_space')}"
              + (f"; active input handling: {ds['active_input']}" if ds.get('active_input') else ''))
        s = rep.get('code')
        if s:
            w(f"  code: {', '.join(s['assemblies'][:8])}{', ...' if len(s['assemblies']) > 8 else ''}")
            lg = ', '.join(f'{k} {v}' for k, v in s['legacy'].items() if v)
            w(f"  input: Rewired {s['rewired'] or 'no'}; InControl {'yes' if s['incontrol'] else 'no'}; "
              f"Input System {'yes' if s['input_system'] else 'no'}; legacy Input: {lg or 'none'}; "
              f"OnMouse* {len(s['mouse_messages'])}; UI pointer handlers {s['pointer_handlers']}; "
              f"UI selection calls {s['selects']}")
            mods = [_INPUT_MODULES[k] for k in ds.get('scripts_used', {}) if k in _INPUT_MODULES]
            if mods:
                w(f"  UI input modules in scenes: {', '.join(mods)}")
            w(f"  verdict: {rep.get('input_verdict_text') or rep.get('input_verdict')}")
        tex = ds.get('textures') or {}
        if tex:
            top = sorted(tex.items(), key=lambda kv: -kv[1][1])[:5]
            w('  textures (main data): ' + ', '.join(f'{k} {v[0]} ({v[1] / 2**20:.0f} MB)' for k, v in top))
    for o in rep.get('other_builds', []):
        w(f"  also: {o['build']} build ({o['backend']}) at {o['data']}")
    w()
    w('routes:')
    for r in rep.get('routes', []):
        mark = 'yes' if r['ok'] else 'no '
        avail = '' if r.get('available', True) else ' (templates not there yet)'
        w(f"  [{mark}] {r['route']}{avail}: {ROUTE_TEXT.get(r['route'], '')}")
        for x in r['why']:
            w(f'        - {x}')
        for x in r.get('needs', []):
            w(f'        + {x}')
    w()
    if rep.get('route'):
        w(f"recommended route: {rep['route']}")
    else:
        w(f"unsupported: {rep.get('unsupported')}")
    for x in rep.get('warnings', []):
        w(f'warning: {x}')
    for x in rep.get('notes', []):
        w(f'- {x}')
    if rep.get('suggest'):
        sg = {k: v for k, v in rep['suggest'].items() if v}
        if sg:
            w()
            w('port config (port new starts from these): ' + json.dumps(sg, sort_keys=True))
    if rep.get('asmpatch_survey'):
        w()
        w('asmpatch --survey:')
        for line in rep['asmpatch_survey'].splitlines():
            w('  ' + line)


# =========================================================================== the port config

NAME_RX = re.compile(r'^[a-z0-9][a-z0-9._-]*$')
ID_RX = re.compile(r'^[A-Z][A-Z0-9_]*$')
VALUE_RX = re.compile(r'^[A-Za-z0-9._:+-]+$')
ENV_RX = re.compile(r'^[A-Z_][A-Z0-9_]*$')

DEFAULTS = {
    'port': {'name': '', 'title': '', 'route': 'android', 'porter': [], 'desc': '', 'inst': '', 'genres': [],
             'availability': 'free', 'store': [], 'setup_time': '', 'exp': True, 'ready_marker': '.setup_done'},
    'game': {'data': '*_Data', 'build': 'linux', 'unity_versions': [], 'what': '', 'size_mb': 0, 'setup_space_mb': 0},
    'player': {'backend': 'mono', 'abi': 'armeabi-v7a', 'url': '', 'sha256': {}},
    'convert': {'steps': ['shaders', 'settings', 'audio'], 'settings': {'graphics_apis': [11]}},
    'android_data': {'asmpatch': 'off', 'video_shader': True, 'swappy': 'off', 'streaming_assets': 'obb', 'data_path': False,
                     'name': '', 'package': '', 'args': []},
    'runtime': {},
    'questions': [],
    'files': {'plugins': [], 'managed': [], 'assembly_patches': [], 'hooks': {}, 'extra': []},
    'build': {'commands': [], 'outputs': {}, 'licenses': {}},
}
HOOK_POINTS = ('game', 'pre_convert', 'post_convert', 'post_pack', 'post_install')


def merge(base, over):
    """Deep merge of config dicts: tables merge, everything else is replaced."""
    out = copy.deepcopy(base)
    for k, v in (over or {}).items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = merge(out[k], v)
        else:
            out[k] = copy.deepcopy(v)
    return out


def load_config(path):
    with open(path, 'rb') as f:
        raw = f.read()
    if path.endswith('.json'):
        return json.loads(raw)
    import tomllib
    return tomllib.loads(raw.decode('utf-8'))


def title_case(s):
    return re.sub(r'\s+', ' ', s.replace('_', ' ')).strip()


def build_config(name, rep, user):
    """The resolved config: DEFAULTS <- what the check found <- the porter's config."""
    cfg = merge(DEFAULTS, {})
    if rep:
        b = rep.get('build')
        data_name = 'Data' if b == 'macos' else os.path.basename(rep['data'])
        product = rep.get('product') or (os.path.basename(rep['data'])[:-5] if rep['data'].endswith('_Data') else name)
        found = {
            'port': {'title': title_case(product), 'route': rep.get('route') or 'android'},
            'game': {'data': data_name, 'build': b if b in ('linux', 'macos', 'windows') else 'linux',
                     'unity_versions': [rep['unity_version']] if rep.get('unity_version') else [],
                     'size_mb': rep.get('size_mb', 0)},
        }
        cfg = merge(cfg, found)
        cfg = merge(cfg, rep.get('suggest') or {})
    cfg = merge(cfg, user or {})
    p, g, ad = cfg['port'], cfg['game'], cfg['android_data']
    p['name'] = p.get('name') or name
    p['title'] = p['title'] or title_case(name)
    if not g['what']:
        kind = {'linux': 'Linux', 'macos': 'macOS', 'windows': 'Windows'}.get(g['build'], g['build'])
        vs = ', '.join(g['unity_versions'])
        g['what'] = f"the {kind} build of {p['title']}" + (f' (Unity {vs})' if vs else '') + \
            (': the installed .app or the macOS installers (.pkg)' if g['build'] == 'macos' else
             ': its folder, or the archive (.zip, .7z, .tar.gz) it came in')
    ad['name'] = ad['name'] or (re.sub(r'[^A-Za-z0-9]', '', p['title']) or 'Game')
    ad['package'] = ad['package'] or 'com.pmunity.' + re.sub(r'[^a-z0-9]', '', p['name'].lower())
    if not p['setup_time']:
        mb = g.get('size_mb') or 0
        slow = 'textures' in cfg['convert'].get('steps', [])
        p['setup_time'] = ('about an hour' if mb > 300 else 'a quarter of an hour') if slow else \
            ('about a quarter of an hour' if mb > 1000 else 'a few minutes')
    return cfg


def validate(cfg):
    p = cfg['port']
    errs = []
    if not NAME_RX.match(p['name']):
        errs.append(f"port.name {p['name']!r}: lowercase letters, digits, '.', '_', '-' (it is the folder in ports/)")
    if not p['title'] or '/' in p['title'] or p['title'].startswith('.'):
        errs.append(f"port.title {p['title']!r}: the launcher is '<title>.sh'")
    for k in cfg['runtime']:
        if not ENV_RX.match(k):
            errs.append(f'runtime.{k}: not an environment variable name')
    for q in cfg['questions']:
        if not ID_RX.match(q.get('id', '')):
            errs.append(f"questions: id {q.get('id')!r} must be like TEXTURE_QUALITY")
        opts = q.get('options') or []
        if not opts or any(not VALUE_RX.match(str(o[0])) for o in opts):
            errs.append(f"questions.{q.get('id')}: options [[value, label], ...] with plain values")
        if q.get('default') and q['default'] not in [o[0] for o in opts]:
            errs.append(f"questions.{q.get('id')}: default {q['default']!r} is not an option")
        if q.get('env') and not ENV_RX.match(q['env']):
            errs.append(f"questions.{q.get('id')}: env {q['env']!r} is not a variable name")
    a = cfg['android_data']['asmpatch']
    if not (a in ('auto', 'off') or isinstance(a, list) and all(VALUE_RX.match(x) for x in a)):
        errs.append(f'android_data.asmpatch {a!r}: auto, off or a list of patch names')
    if cfg['android_data'].get('streaming_assets', 'obb') not in ('obb', 'both', 'loose'):
        errs.append(f"android_data.streaming_assets {cfg['android_data']['streaming_assets']!r}: obb, both or loose")
    for k in cfg['files'].get('hooks', {}):
        if k not in HOOK_POINTS:
            errs.append(f"files.hooks.{k}: one of {', '.join(HOOK_POINTS)}")
    if not re.match(r'^\.[A-Za-z0-9._-]+$', p.get('ready_marker', '')):
        errs.append(f"port.ready_marker {p.get('ready_marker')!r}: a dot file name, e.g. .setup_done")
    for dst in cfg['build'].get('outputs', {}):
        if dst.startswith('/') or '..' in dst.split('/'):
            errs.append(f'build.outputs: {dst!r} must be a path inside the port folder')
    for v in cfg['game']['unity_versions']:
        if not re.match(r'^\d{4}\.\d+\.\d+[abfpx]\d+$', v):
            errs.append(f'game.unity_versions: {v!r} is not a Unity version')
    if errs:
        raise SystemExit('port config:\n  ' + '\n  '.join(errs))


def _toml_value(v):
    if isinstance(v, bool):
        return 'true' if v else 'false'
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, str):
        return json.dumps(v, ensure_ascii=False)
    if isinstance(v, (list, tuple)):
        return '[' + ', '.join(_toml_value(x) for x in v) + ']'
    if isinstance(v, dict):
        return '{' + ', '.join(f'{_toml_key(k)} = {_toml_value(x)}' for k, x in v.items()) + '}'
    raise TypeError(type(v))


def _toml_key(k):
    return k if re.match(r'^[A-Za-z0-9_-]+$', k) else json.dumps(k)


CONFIG_DOC = {
    'port': '# the port: its folder in ports/ (name), the menu entry and launcher "<title>.sh" (title), the\n'
            '# route (port check recommends one), port.json fields, the patcher\'s time estimate',
    'game': '# what the player provides: the data folder the setup looks for (data, a glob), the build\n'
            '# kind (linux | macos | windows), the Unity versions the setup accepts (empty: any the\n'
            '# converter supports), how READMEs and messages describe it (what), its size in MB',
    'player': "# Unity's Android player: backend/abi; url = the installer (`pmunity player url <version>`; empty:\n"
              "# looked up on the handheld); sha256 = {\"<file>\" = \"<hash>\"} checked after the fetch",
    'convert': '# pmunity convert\'s config (each step\'s DEFAULTS); "textures" re-encodes BC/DXT as ASTC\n'
               '# (less memory, a longer setup, slightly different pixels: off unless you add it)',
    'android_data': '# pmunity android-data: asmpatch = auto (run tools/asmpatch; it changes only what it\n'
                    '# finds, e.g. Rewired) | off | ["patch", ...]; streaming_assets = obb | both | loose (code that\n'
                    '# reads StreamingAssets as files: also/only beside the APK, docs/android-data.md); data_path =\n'
                    '# true (files read or written at Application.dataPath); name = the APK\'s, package (keep it:\n'
                    '# it names the PlayerPrefs file); args = more options',
    'runtime': "# ua_host's environment (docs/runtime.md, docs/input.md), written to port.env:\n"
               "# e.g. UA_POINTER = \"1\", UA_POINTER_CURSOR = \"0\", UA_KEYMAP = \"start:escape\", UA_PAD_NAME",
    'build': '# commands `port package` runs in this folder first (env PMUNITY, PORT_SRC, BUILD), the files\n'
             '# they make put into the port folder (outputs = {"setup/files/x" = "build/x"}), and their licences\n'
             '# (licenses = {"LICENSE.x.txt" = "path"})',
    'files': '# the porter\'s own files (paths relative to this file), copied into setup/: converter plugins\n'
             '# (--plugin), managed = assemblies shipped into the game (--managed-file), assembly_patches =\n'
             '# [{name, command, files}] ({setup} = the setup folder), hooks = {game|pre_convert|post_convert|\n'
             '# post_pack|post_install = "x.sh"} sourced by the setup, extra = files for the port folder',
}
QUESTIONS_DOC = """# Questions PortMaster's patcher asks before the setup (each answer is kept for later runs), e.g.:
# [[questions]]
# id = "TEXTURE_QUALITY"
# question = "Texture quality? Medium is what this port is tested with."
# options = [["medium", "Medium (recommended)"], ["fast", "Fast (shorter setup)"]]
# default = "medium"
# convert_set = "textures.quality"        # the answer -> pmunity convert --set textures.quality=<answer>
# # env = "UA_POINTER"                    # or: the answer -> the game's environment
"""


def dump_toml(cfg):
    """port.toml text for a resolved config, with the keys explained."""
    out = ['# port.toml: a PortMaster port made with `python -m pmunity port new` (docs/porting.md).',
           '# Edit, then run `port new <name> --data <game>` again to regenerate the port folder.', '']
    for sec in ('port', 'game', 'player', 'convert', 'android_data', 'runtime', 'files', 'build'):
        val = cfg.get(sec, {})
        out.append(CONFIG_DOC[sec])
        out.append(f'[{sec}]')
        subs = []
        for k, v in val.items():
            if isinstance(v, dict) and sec in ('convert',) and v:
                subs.append((k, v))
                continue
            out.append(f'{_toml_key(k)} = {_toml_value(v)}')
        for k, v in subs:
            out.append(f'[{sec}.{_toml_key(k)}]')
            for kk, vv in v.items():
                out.append(f'{_toml_key(kk)} = {_toml_value(vv)}')
        out.append('')
    if cfg.get('questions'):
        for q in cfg['questions']:
            out.append('[[questions]]')
            for k, v in q.items():
                out.append(f'{_toml_key(k)} = {_toml_value(v)}')
            out.append('')
    else:
        out.append(QUESTIONS_DOC)
    return '\n'.join(out)


# =========================================================================== routes and templates

def routes():
    """{route: route.json} of the templates present (port_template/<route>/route.json)."""
    out = {}
    if os.path.isdir(TEMPLATES):
        for n in sorted(os.listdir(TEMPLATES)):
            p = os.path.join(TEMPLATES, n, 'route.json')
            if os.path.isfile(p):
                with open(p) as f:
                    r = json.load(f)
                r['dir'] = os.path.join(TEMPLATES, n)
                out[n] = r
    return out


TOKEN = re.compile(r'@@([A-Z0-9_]+)@@')


def _read(path, mode='r'):
    with open(path, mode) as f:
        return f.read()


def render(text, values, where='template'):
    def sub(m):
        k = m.group(1)
        if k not in values:
            raise SystemExit(f'{where}: unknown template value @@{k}@@')
        return str(values[k])
    return TOKEN.sub(sub, text)


def template_path(route, name):
    """A route's template file, else the common one (port_template/common/)."""
    for d in (route['dir'], os.path.join(TEMPLATES, 'common')):
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    raise SystemExit(f"template {name} is in neither {route['dir']} nor port_template/common")


def q(v):
    return shlex.quote(str(v))


def setup_env(cfg, extra=()):
    """setup/setup.env: everything the setup reads, as shell assignments (sourced by patchscript)."""
    p, g, pl, ad, fl = cfg['port'], cfg['game'], cfg['player'], cfg['android_data'], cfg['files']
    a = ad['asmpatch']
    lines = ['# Written by `pmunity port new` from port.toml: the setup\'s settings (sourced by patchscript).',
             f'PORT_NAME={q(p["name"])}', f'PORT_TITLE={q(p["title"])}', f'PORT_ROUTE={q(p["route"])}',
             f'GAME_DATA={q(g["data"])}', f'GAME_BUILD={q(g["build"])}', f'GAME_WHAT={q(g["what"])}',
             f'GAME_SIZE_MB={int(g.get("size_mb") or 0)}', f'SETUP_SPACE_MB={int(g.get("setup_space_mb") or 0)}',
             f'READY={q(p.get("ready_marker") or ".setup_done")}',
             f'UNITY_VERSIONS={q(" ".join(g["unity_versions"]))}',
             f'PLAYER_BACKEND={q(pl["backend"])}', f'PLAYER_ABI={q(pl["abi"])}',
             f'PLAYER_URL={q(pl.get("url") or "")}',
             f'PLAYER_URL_VERSION={q(g["unity_versions"][0] if g["unity_versions"] and pl.get("url") else "")}',
             'PLAYER_SHA256=' + q(' '.join(f'{k}={v}' for k, v in sorted((pl.get('sha256') or {}).items()))),
             f'APK_NAME={q(ad["name"])}', f'PACKAGE={q(ad["package"])}',
             f'ASMPATCH={q(",".join(a) if isinstance(a, list) else a)}',
             f'VIDEO_SHADER={1 if ad.get("video_shader", True) else 0}', f'SWAPPY={q(ad.get("swappy") or "off")}',
             f'STREAMING_ASSETS={q(ad.get("streaming_assets") or "obb")}', f'DATA_PATH={1 if ad.get("data_path") else 0}',
             'ANDROID_DATA_ARGS=(' + ' '.join(q(x) for x in ad.get('args', [])) + ')',
             'CONVERT_PLUGINS=(' + ' '.join(q('files/' + os.path.basename(x)) for x in fl.get('plugins', [])) + ')',
             'MANAGED_FILES=(' + ' '.join(q('files/' + os.path.basename(x)) for x in fl.get('managed', [])) + ')',
             'ASSEMBLY_PATCHES=(' + ' '.join(q(ap['name']) + ' ' + q(ap['command']) for ap in fl.get('assembly_patches', [])) + ')']
    qs = cfg.get('questions', [])
    lines.append(f'QUESTIONS={q(" ".join(x["id"] for x in qs))}')
    for x in qs:
        i = x['id']
        lines += [f'Q_{i}_OPTIONS={q(" ".join(str(o[0]) for o in x["options"]))}',
                  f'Q_{i}_DEFAULT={q(x.get("default") or x["options"][0][0])}',
                  f'Q_{i}_SET={q(x.get("convert_set") or "")}', f'Q_{i}_ENV={q(x.get("env") or "")}']
    lines += list(extra)
    return '\n'.join(lines) + '\n'


def port_env(cfg):
    """<port>/port.env: ua_host's environment, sourced by the launcher; a value already in the
    environment wins."""
    lines = ['# The game\'s runtime options (ua_host: docs/runtime.md and docs/input.md in portmaster-unity),',
             '# sourced by the launcher. Edit to taste; a variable already set in the environment wins.']
    for k, v in cfg['runtime'].items():
        lines.append(f'[ -n "${{{k}+x}}" ] || export {k}={q(v)}')
    return '\n'.join(lines) + '\n'


def lua_str(s):
    return '"' + str(s).replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


def questions_lua(qs):
    out = ["-- Asked by PortMaster's patcher before the setup starts (PATCHER_QUESTIONS in the launcher);",
           '-- each answer reaches setup/patchscript as <id>=<value> in its environment.', 'return {']
    for x in qs:
        out += ['  {', f'    id = {lua_str(x["id"])},', f'    question = {lua_str(x["question"])},', '    options = {']
        out += [f'      {{ {lua_str(o[0])}, {lua_str(o[1] if len(o) > 1 else o[0])} }},' for o in x['options']]
        out += ['    },', '  },']
    out.append('}')
    return '\n'.join(out) + '\n'


def setup_txt(cfg):
    out = [f"# {cfg['port']['title']} setup options, read once by the first-launch setup. PortMaster's setup",
           '# screen asks these itself; this file is used when it does not (older PortMaster versions).']
    for x in cfg['questions']:
        out.append('#')
        out.append('# ' + x['question'])
        for o in x['options']:
            out.append(f'#   {o[0]}  {o[1] if len(o) > 1 else ""}')
        out.append(f"{x['id'].lower()}={x.get('default') or x['options'][0][0]}")
    return '\n'.join(out) + '\n'


def port_json(cfg):
    p, g = cfg['port'], cfg['game']
    return {
        'version': 4,
        'name': f"{p['name']}.zip",
        'items': [f"{p['title']}.sh", p['name']],
        'items_opt': [],
        'attr': {
            'title': p['title'], 'porter': list(p['porter']), 'desc': p['desc'] or f"{p['title']} on Unity's Android player.",
            'desc_md': None,
            'inst': p.get('inst') or f"Requires {g['what']}: put it in ports/{p['name']}/gamedata. The first launch builds "
                    f"the game on the handheld ({p['setup_time']}; downloads Unity's Android player from Unity, about 10 MB, Wi-Fi).",
            'inst_md': None, 'genres': list(p['genres']), 'image': None, 'rtr': False, 'exp': bool(p.get('exp', True)),
            'runtime': [], 'store': list(p['store']), 'availability': p['availability'],
            'reqs': ['armhf'], 'arch': ['aarch64'], 'min_glibc': '',
        },
    }


class NewCtx:
    """What a port_new hook gets: the config, the port's source folder, write()."""

    def __init__(self, cfg, out, route):
        self.cfg, self.out, self.route = cfg, out, route
        self.port_dir = os.path.join(out, cfg['port']['name'])
        self.written = []

    def write(self, rel, data, mode=0o644):
        p = os.path.join(self.out, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb' if isinstance(data, bytes) else 'w') as f:
            f.write(data)
        os.chmod(p, mode)
        self.written.append(rel)


def new(name, data=None, config=None, out=None, route=None, title=None, player=None, force=False,
        asmpatch=None, log=print):
    """Writes a port's source folder; returns its path."""
    user = load_config(config) if config else {}
    out = os.path.abspath(out or name)
    own_toml = os.path.join(out, 'port.toml')
    if not config and os.path.isfile(own_toml):
        user = load_config(own_toml)
        config = own_toml
        log(f'using {own_toml}')
    if os.path.exists(out) and os.listdir(out) and not os.path.isfile(os.path.join(out, 'port.build.json')) and not force:
        raise SystemExit(f'{out} exists and is not a port made by `port new` (--force to write into it)')
    rep = None
    if data:
        log(f'checking {data}')
        rep = check(data, asmpatch=asmpatch)
        if not rep.get('route') and not (route or (user.get('port') or {}).get('route')):
            raise SystemExit(f"no route fits this game: {rep.get('unsupported')}")
    elif not (user.get('game') or {}).get('unity_versions'):
        raise SystemExit('--data <game> (or a config with game.unity_versions) is needed')
    over = {}
    if route:
        over.setdefault('port', {})['route'] = route
    if title:
        over.setdefault('port', {})['title'] = title
    cfg = build_config(name, rep, merge(user, over))
    if player:
        pj = os.path.join(player, 'player.json')
        if os.path.isfile(pj):
            info = json.loads(_read(pj))
            if info.get('installer', '').startswith('http') and not cfg['player']['url']:
                cfg['player']['url'] = info['installer']
        if not cfg['player']['sha256']:
            libs = os.path.join(player, 'Variations', cfg['player']['backend'], 'Release', 'Libs', cfg['player']['abi'])
            for n in ('libmain.so', 'libunity.so'):
                if os.path.isfile(os.path.join(libs, n)):
                    cfg['player']['sha256'][n] = hashlib.sha256(_read(os.path.join(libs, n), 'rb')).hexdigest()
    validate(cfg)
    rts = routes()
    r = rts.get(cfg['port']['route'])
    if r is None:
        have = ', '.join(rts) or 'none'
        raise SystemExit(f"route {cfg['port']['route']!r} has no templates (port_template/{cfg['port']['route']}/route.json; "
                         f"routes with templates: {have})")
    os.makedirs(out, exist_ok=True)
    ctx = NewCtx(cfg, out, r)
    pn, pt = cfg['port']['name'], cfg['port']['title']
    values = {'PORTDIR': pn, 'TITLE': pt, 'ZIP': f'{pn}.zip', 'LAUNCHER': f'{pt}.sh',
              'SETUP_TIME': cfg['port']['setup_time'], 'GAME_WHAT': cfg['game']['what'],
              'ROUTE': cfg['port']['route'], 'ROUTE_TEXT': ROUTE_TEXT.get(cfg['port']['route'], cfg['port']['route']),
              'UNITY_VERSIONS': ', '.join(cfg['game']['unity_versions']) or 'any the converter supports',
              'SIZE_GB': max(1, ((int(cfg['game'].get('setup_space_mb') or 0) or 3 * int(cfg['game'].get('size_mb') or 0)) + 1023) // 1024),
              'PORTER': ', '.join(cfg['port']['porter']) or 'the porter',
              'READY': cfg['port'].get('ready_marker') or '.setup_done'}
    # the files a route renders: route.json "files": {output path: template name}; output paths
    # may use @@TOKENS@@; mode 755 for scripts
    for dst, spec in r['files'].items():
        tname, mode = (spec, None) if isinstance(spec, str) else (spec['template'], spec.get('mode'))
        text = _read(template_path(r, tname))
        dst_r = render(dst, values, 'route.json')
        ctx.write(dst_r, render(text, values, tname), int(mode, 8) if mode else
                  (0o755 if dst_r.endswith(('.sh', 'patchscript')) else 0o644))
    pd = pn                                     # the folder installed as ports/<name>
    ctx.write(f'{pd}/port.json', json.dumps(port_json(cfg), indent=4) + '\n')
    ctx.write(f'{pd}/port.env', port_env(cfg))
    ctx.write(f'{pd}/setup/setup.env', setup_env(cfg))
    conv = {k: v for k, v in cfg['convert'].items()}
    ctx.write(f'{pd}/setup/convert.json', json.dumps(conv, indent=1) + '\n')
    if cfg['questions']:
        ctx.write(f'{pd}/setup/questions.lua', questions_lua(cfg['questions']))
        ctx.write(f'{pd}/setup.txt', setup_txt(cfg))
    # the porter's files
    base = os.path.dirname(os.path.abspath(config)) if config else os.getcwd()
    fl = cfg['files']
    for rel in fl.get('plugins', []) + fl.get('managed', []) + [f for ap in fl.get('assembly_patches', []) for f in ap.get('files', [])]:
        src = os.path.join(base, rel)
        if not os.path.isfile(src):
            raise SystemExit(f'files: {src} not found')
        ctx.write(f'{pd}/setup/files/{os.path.basename(rel)}', _read(src, 'rb'),
                  0o755 if os.access(src, os.X_OK) else 0o644)
    for point, rel in (fl.get('hooks') or {}).items():
        src = os.path.join(base, rel)
        if not os.path.isfile(src):
            raise SystemExit(f'files.hooks.{point}: {src} not found')
        ctx.write(f'{pd}/setup/hooks/{point}.sh', _read(src, 'rb'))
    for rel in fl.get('extra', []):
        src = os.path.join(base, rel)
        if not os.path.isfile(src):
            raise SystemExit(f'files.extra: {src} not found')
        ctx.write(f'{pd}/{os.path.basename(rel)}', _read(src, 'rb'))
    _call_hooks('port_new', ctx)
    # what was made, and from what
    if not config:
        ctx.write('port.toml', dump_toml(cfg))
    build = {'made_by': 'pmunity port new', 'config': cfg, 'route': {k: v for k, v in r.items() if k != 'dir'},
             'files': sorted(set(ctx.written))}
    if rep:
        build['check'] = {k: rep.get(k) for k in ('build', 'arch', 'backend', 'unity_version', 'scripting_runtime',
                                                  'compressed', 'input_verdict', 'route', 'notes', 'warnings')}
    ctx.write('port.build.json', json.dumps(build, indent=1, default=str) + '\n')
    # remove files an earlier run wrote that this one did not
    old = os.path.join(out, '.port_files')
    if os.path.isfile(old):
        for rel in _read(old).split('\n'):
            if rel and rel not in ctx.written and rel != 'port.toml' and os.path.isfile(os.path.join(out, rel)):
                os.remove(os.path.join(out, rel))
    with open(old, 'w') as f:
        f.write('\n'.join(sorted(set(ctx.written))) + '\n')
    log(f"port {pn} ({cfg['port']['route']} route) -> {out}")
    if rep:
        for n in rep.get('notes', []):
            log(f'- {n}')
    return out


# =========================================================================== the release zip

class PackCtx:
    """What a port_package hook gets: add(), license(), the config and the folders."""

    def __init__(self, cfg, port_src, zipdir, runtime, log):
        self.cfg, self.port_src, self.zipdir, self.runtime, self.log = cfg, port_src, zipdir, runtime, log
        self.name = cfg['port']['name']
        self.port_dir = os.path.join(zipdir, self.name)
        self.licenses = os.path.join(self.port_dir, 'licenses')

    def add(self, src, rel, mode=None):
        """Copy src (a file or folder) to <port folder>/rel."""
        dst = os.path.join(self.port_dir, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.isdir(src):
            shutil.copytree(src, dst, dirs_exist_ok=True, ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '.DS_Store'))
        else:
            shutil.copyfile(src, dst)
            if mode is not None:
                os.chmod(dst, mode)
        return dst

    def license(self, name, path=None, text=None):
        os.makedirs(self.licenses, exist_ok=True)
        dst = os.path.join(self.licenses, name)
        if path:
            shutil.copyfile(path, dst)
        else:
            with open(dst, 'w') as f:
                f.write(text)


CECIL_LICENSE = """Mono.Cecil 0.11.6 (in asmpatch)

Copyright (c) 2008 - 2015 Jb Evain
Copyright (c) 2008 - 2011 Novell, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
"""


def pybundle_licenses(tar_path):
    """One text with the licences inside the setup's Python bundle (CPython's, each package's)."""
    parts = ["The setup's Python (removed from the handheld when the setup is done): python-build-standalone's",
             'CPython and these packages. Their licences follow.']
    with tarfile.open(tar_path) as t:
        for m in t.getmembers():
            n = m.name
            if not m.isfile():
                continue
            if re.search(r'^python/lib/python3\.\d+/LICENSE\.txt$', n) or \
                    re.search(r'/site-packages/[^/]+\.dist-info/(LICEN[CS]E[^/]*|licenses/.+|COPYING[^/]*)$', n):
                rel = re.sub(r'^python/lib/python3\.\d+/', '', n)
                parts.append(f'\n\n==== {rel} ====\n')
                parts.append(t.extractfile(m).read().decode('utf-8', 'replace'))
    return '\n'.join(parts)


def needs_asmpatch(cfg):
    ad = cfg['android_data']
    return ad.get('asmpatch', 'off') != 'off' or ad.get('streaming_assets', 'obb') != 'obb' or bool(ad.get('data_path'))


def _component_ua_host(ctx, opts):
    p = opts.get('ua_host') or os.path.join(ctx.runtime, 'runtime', 'host', 'out', 'ua_host')
    if not os.path.isfile(p):
        if opts.get('no_build'):
            raise SystemExit(f'{p}: build the runtime first (runtime/build.sh)')
        ctx.log('building the runtime (runtime/build.sh, Docker)')
        subprocess.run([os.path.join(ctx.runtime, 'runtime', 'build.sh')], check=True)
    ctx.add(p, 'ua_host', 0o755)
    hyb = os.path.join(ctx.runtime, 'runtime', 'thirdparty', 'libhybris', 'LICENSE.Apache2')
    if not os.path.isfile(hyb):
        hyb = os.path.join(REPO, 'runtime', 'thirdparty', 'libhybris', 'LICENSE.Apache2')
    ctx.license('LICENSE.libhybris-Apache-2.0.txt', hyb)
    ml = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(p))), 'media', 'out', 'licenses') \
        if opts.get('ua_host') else os.path.join(ctx.runtime, 'runtime', 'media', 'out', 'licenses')
    ml = opts.get('media_licenses') or ml
    lic = sorted(n for n in os.listdir(ml) if n.endswith('.copyright')) if os.path.isdir(ml) else []
    if not lic:
        raise SystemExit(f'no decoder licences in {ml} (ua_host links the video decoders: build the runtime, '
                         f'or --media-licenses DIR)')
    for n in lic:
        ctx.license(f'LICENSE.{n[:-len(".copyright")]}.txt', os.path.join(ml, n))


def _component_pmunity(ctx, opts):
    src = HERE
    for r, ds, fs in os.walk(src):
        ds[:] = sorted(d for d in ds if d not in ('__pycache__', 'port_template'))
        for n in fs:
            if n.endswith(('.py', '.shader')):
                rel = os.path.relpath(os.path.join(r, n), os.path.dirname(src))
                ctx.add(os.path.join(r, n), os.path.join('setup', rel))


def _component_pybundle(ctx, opts):
    p = opts.get('pybundle') or os.environ.get('PYBUNDLE')
    if not p:
        p = os.path.join(REPO, 'tools', 'port', 'out', 'python.tar.gz')
        if not os.path.isfile(p):
            if opts.get('no_build'):
                raise SystemExit("the setup's Python: --pybundle FILE (tools/port/build_pybundle.sh builds it)")
            ctx.log("building the setup's Python (tools/port/build_pybundle.sh)")
            subprocess.run([os.path.join(REPO, 'tools', 'port', 'build_pybundle.sh'), p], check=True)
    ctx.add(p, os.path.join('setup', 'python.tar.gz'))
    ctx.license('LICENSE.setup-python.txt', text=pybundle_licenses(p))


def _component_asmpatch(ctx, opts):
    if not needs_asmpatch(ctx.cfg):
        return
    p = opts.get('asmpatch_bin') or os.path.join(ctx.runtime, 'tools', 'asmpatch', 'out', 'linux-arm64', 'asmpatch')
    if not os.path.isfile(p):
        if opts.get('no_build'):
            raise SystemExit(f'{p}: build asmpatch first (tools/asmpatch/build.sh)')
        ctx.log('building asmpatch (tools/asmpatch/build.sh, .NET 8 SDK)')
        subprocess.run([os.path.join(ctx.runtime, 'tools', 'asmpatch', 'build.sh')], check=True)
    ctx.add(p, os.path.join('setup', 'asmpatch'), 0o755)
    nuget = os.environ.get('NUGET_PACKAGES') or os.path.expanduser('~/.nuget/packages')
    rt = os.path.join(nuget, 'microsoft.netcore.app.runtime.linux-arm64')
    vers = sorted((v for v in os.listdir(rt) if v.startswith('8.')), key=lambda v: [int(x) if x.isdigit() else 0 for x in re.split(r'[.-]', v)]) \
        if os.path.isdir(rt) else []
    if not vers:
        raise SystemExit(f'{rt}: the .NET runtime pack (its LICENSE.TXT and THIRD-PARTY-NOTICES.TXT go with asmpatch)')
    ctx.license('LICENSE.dotnet-runtime.txt', os.path.join(rt, vers[-1], 'LICENSE.TXT'))
    ctx.license('THIRD-PARTY-NOTICES.dotnet-runtime.txt', os.path.join(rt, vers[-1], 'THIRD-PARTY-NOTICES.TXT'))
    ctx.license('LICENSE.mono.cecil.txt', text=CECIL_LICENSE)


COMPONENTS = {'ua_host': _component_ua_host, 'pmunity': _component_pmunity, 'pybundle': _component_pybundle,
              'asmpatch': _component_asmpatch}


def _write_zip(zpath, root, items):
    """A zip of root/<items> with Unix modes and fixed timestamps (the same inputs, the same zip)."""
    tmp = zpath + '.tmp'
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for item in items:
            top = os.path.join(root, item)
            paths = [top] if os.path.isfile(top) else []
            for r, ds, fs in os.walk(top):
                ds.sort()
                paths += [os.path.join(r, n) for n in sorted(fs)]
            for p in paths:
                rel = os.path.relpath(p, root).replace(os.sep, '/')
                zi = zipfile.ZipInfo(rel, date_time=(2020, 1, 1, 0, 0, 0))
                mode = 0o755 if os.access(p, os.X_OK) else 0o644
                zi.external_attr = (0o100000 | mode) << 16
                zi.create_system = 3
                zi.compress_type = zipfile.ZIP_DEFLATED
                with open(p, 'rb') as f:
                    z.writestr(zi, f.read())
    os.replace(tmp, zpath)


def package(port_src, out=None, runtime=None, log=print, **opts):
    """The release zip of a port folder made by `port new`; returns its path."""
    port_src = os.path.abspath(port_src)
    bj = os.path.join(port_src, 'port.build.json')
    if not os.path.isfile(bj):
        raise SystemExit(f'{port_src}: no port.build.json (make the port with `pmunity port new` first)')
    build = json.loads(_read(bj))
    cfg, route = build['config'], build['route']
    name, title = cfg['port']['name'], cfg['port']['title']
    out = os.path.abspath(out or os.path.join(port_src, 'out'))
    runtime = os.path.abspath(runtime or REPO)
    zipdir = os.path.join(out, 'zip')
    shutil.rmtree(zipdir, ignore_errors=True)
    os.makedirs(zipdir)
    ctx = PackCtx(cfg, port_src, zipdir, runtime, log)
    bld = cfg.get('build') or {}
    if bld.get('commands'):
        env = dict(os.environ, PMUNITY=REPO, PORT_SRC=port_src, BUILD=os.path.join(out, 'build'))
        os.makedirs(env['BUILD'], exist_ok=True)
        for cmd in bld['commands']:
            log(f'== build: {cmd}')
            subprocess.run(cmd, shell=True, cwd=port_src, env=env, check=True)
    launcher = f'{title}.sh'
    shutil.copyfile(os.path.join(port_src, launcher), os.path.join(zipdir, launcher))
    os.chmod(os.path.join(zipdir, launcher), 0o755)
    ctx.add(os.path.join(port_src, name), '.')
    for rel in ('setup/patchscript',):
        p = os.path.join(ctx.port_dir, rel)
        if os.path.isfile(p):
            os.chmod(p, 0o755)
    for c in route.get('package', []):
        c = c.rstrip('?')                       # ('?': only when the port uses it; the part decides)
        if c not in COMPONENTS:
            continue                            # (a hook module provides it)
        if c == 'asmpatch' and not needs_asmpatch(cfg):
            continue
        log(f'== {c}')
        COMPONENTS[c](ctx, opts)
    for dst, src in (bld.get('outputs') or {}).items():
        sp = os.path.join(port_src, src.replace('{build}', os.path.join(out, 'build')))
        if not os.path.exists(sp):
            raise SystemExit(f'build.outputs: {sp} was not made')
        ctx.add(sp, dst, 0o755 if os.access(sp, os.X_OK) else None)
    for n, src in (bld.get('licenses') or {}).items():
        ctx.license(n, os.path.join(port_src, src))
    _call_hooks('port_package', ctx)
    ctx.license('LICENSE.portmaster-unity.txt', os.path.join(REPO, 'LICENSE'))
    ctx.license('THIRD_PARTY.portmaster-unity.md', os.path.join(REPO, 'THIRD_PARTY.md'))
    for n in ('LICENSE', 'LICENSE.md', 'LICENSE.txt'):
        if os.path.isfile(os.path.join(port_src, n)):
            ctx.license('LICENSE.port.txt', os.path.join(port_src, n))
            break
    zpath = os.path.join(out, f'{name}.zip')
    _write_zip(zpath, zipdir, [launcher, name])
    log(f'built {zpath} ({os.path.getsize(zpath) / 2**20:.1f} MB): unzip it into the handheld\'s ports folder')
    return zpath


# =========================================================================== command line

def main(argv):
    ap = argparse.ArgumentParser(prog='pmunity port', description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('check', help='what the game is, what it needs, which route fits')
    c.add_argument('game')
    c.add_argument('--json', action='store_true', help='the report as JSON')
    c.add_argument('--asmpatch', help="asmpatch's command for its --survey (default: $PMUNITY_ASMPATCH or tools/asmpatch/out)")
    c.add_argument('--no-scan', action='store_true', help='skip scenes, textures and video (only settings and code)')
    n = sub.add_parser('new', help='write a port folder')
    n.add_argument('name', help="the port's folder in ports/ (lowercase)")
    n.add_argument('--data', help='the game (folder, .app or data folder) to check and make the port for')
    n.add_argument('--config', help='port.toml or .json (default: <out>/port.toml when it exists)')
    n.add_argument('--out', help='where the port folder goes (default ./<name>)')
    n.add_argument('--route', help='a route other than the recommended one')
    n.add_argument('--title', help='the menu name (default: the game\'s product name)')
    n.add_argument('--player', help="a player package fetched with `pmunity player fetch` (its URL and checksums go into the setup)")
    n.add_argument('--asmpatch', help="asmpatch's command for the check's survey")
    n.add_argument('--force', action='store_true', help='write into a folder that is not a port made by port new')
    p = sub.add_parser('package', help="build a port folder's release zip")
    p.add_argument('port', help='the folder `port new` wrote')
    p.add_argument('--out', help='output folder (default <port>/out)')
    p.add_argument('--runtime', help='a portmaster-unity checkout whose runtime and asmpatch are built (default: this one)')
    p.add_argument('--ua-host', help='the ua_host binary (default <runtime>/runtime/host/out/ua_host)')
    p.add_argument('--media-licenses', help="the video decoders' licences (default <runtime>/runtime/media/out/licenses)")
    p.add_argument('--pybundle', help="the setup's Python (default $PYBUNDLE, else tools/port/build_pybundle.sh builds it)")
    p.add_argument('--asmpatch-bin', help='asmpatch for linux-arm64 (default <runtime>/tools/asmpatch/out/linux-arm64/asmpatch)')
    p.add_argument('--no-build', action='store_true', help='stop instead of building what is missing')
    sub.add_parser('routes', help='the routes and whether their templates are here')
    a = ap.parse_args(argv)
    if a.cmd == 'check':
        rep = check(a.game, asmpatch=a.asmpatch, deep=not a.no_scan, log=lambda m: print(f'.. {m}', file=sys.stderr))
        if a.json:
            json.dump(rep, sys.stdout, indent=1, default=str)
            print()
        else:
            print_report(rep)
        return 0 if rep.get('route') else 1
    if a.cmd == 'new':
        new(a.name, a.data, a.config, a.out, a.route, a.title, a.player, a.force, a.asmpatch)
        return 0
    if a.cmd == 'package':
        package(a.port, a.out, a.runtime, ua_host=a.ua_host, media_licenses=a.media_licenses, pybundle=a.pybundle,
                asmpatch_bin=a.asmpatch_bin, no_build=a.no_build)
        return 0
    if a.cmd == 'routes':
        have = routes()
        for r, text in ROUTE_TEXT.items():
            print(f"{r:13} {'templates' if r in have else 'no templates':13} {text}")
        for r in have:
            if r not in ROUTE_TEXT:
                print(f"{r:13} {'templates':13} {have[r].get('title', '')}")
        return 0
