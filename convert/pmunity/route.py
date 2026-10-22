"""pmunity route: which way a Unity game fits a PortMaster handheld, and why.

    python -m pmunity route <game folder | <Game>_Data | <Game>.app | executable> [--json | --env]
                            [--launcher OUT.sh [--port NAME] [--title TEXT] [--template FILE]]

Two routes (docs/box64-route.md):
  android  Unity's own ARM Android player of the game's version runs natively on the handheld;
           the desktop data is converted (`pmunity convert`, `pmunity android-data`). Needs a Mono
           build of a Unity version the converter and runtime cover, with OpenGL shaders (macOS or
           Linux build; Windows builds: Direct3D 11 through `--from d3d11`), and no native plugin
           the game cannot do without that exists only for x86. Roughly twice the box64 route's
           frame rate.
  box64    the game's own Linux x86_64 build runs unmodified under box64 (x86_64 -> ARM64
           translation), with the device's GL (or gl4es for GL 2 players). Takes any Unity
           version and IL2CPP builds (GameAssembly.so: the game's code is x86 machine code) and
           x86-only plugins, at the cost of emulation.

Everything here only reads the game's files (ELF/PE/Mach-O headers, the data's serialized-file
headers, strings in the player binary); nothing of the game is executed. With --launcher, the
box64 route's PortMaster launcher (port_template/box64/launcher.sh, the template `pmunity port new
--route box64` uses) is written for this game; --env prints the game.env the box64 port's setup
writes. port_check / port_new / port_package are `pmunity port`'s extension points (port.py).
"""
import argparse
import json
import mmap
import os
import re
import struct
import sys

from . import unity_version as UV

# BuildTarget numbers in a serialized file's header (Unity's enum)
PLATFORMS = {2: 'macos', 4: 'macos', 27: 'macos', 5: 'windows', 19: 'windows', 17: 'linux', 24: 'linux',
             25: 'linux', 13: 'android', 9: 'ios', 20: 'webgl'}
ELF_MACHINES = {3: 'x86', 62: 'x86_64', 40: 'armv7', 183: 'arm64'}
PE_MACHINES = {0x14c: 'x86', 0x8664: 'x86_64', 0xaa64: 'arm64', 0x1c4: 'armv7'}
MACHO_CPUS = {7: 'x86', 0x01000007: 'x86_64', 0x0100000c: 'arm64', 12: 'armv7'}
DATA_MARKERS = ('globalgamemanagers', 'mainData', 'data.unity3d')

# Native plugins we know: (regex on the file name, what it is, effect on the Android route,
# severity there: 'ok' = handled, 'minor' = the game runs with less, 'major' = a feature (sound,
# video, the game itself) is lost; effect on the box64 route)
KNOWN_PLUGINS = [
    (r'^(lib)?_?burst_generated', 'Burst-compiled jobs',
     'not loaded: Burst jobs fall back to their managed code (slower; fine unless the game insists on Burst)', 'minor',
     'emulated'),
    (r'steam_api|CSteamworks|steamworks', 'Steamworks',
     'replaced by the runtime\'s libsteam_api stub (SteamAPI_Init fails, as without a Steam client)', 'ok',
     'emulated; without a Steam client SteamAPI_Init fails (games usually go on; a few quit)'),
    (r'fmod', 'FMOD Studio integration',
     'needs FMOD\'s ARM Android libraries, which a desktop build does not carry: the game is silent without them', 'major',
     'emulated (audio through the device\'s ALSA / PulseAudio)'),
    (r'AkSoundEngine|wwise', 'Wwise sound engine',
     'needs Wwise\'s ARM Android libraries: no sound (or no start) without them', 'major', 'emulated'),
    (r'resonanceaudio', 'Resonance Audio spatializer', 'missing: spatialized sources lose their effect', 'minor',
     'emulated'),
    (r'ScreenSelector', 'Unity\'s start-up resolution dialog (GTK)', 'not used', 'ok',
     'shown only when the build enables the dialog (needs GTK 2 on the device; pass -screen-* arguments)'),
    (r'discord', 'Discord SDK', 'missing: Discord features off', 'minor', 'emulated; fails without a Discord client'),
    (r'galaxy', 'GOG Galaxy SDK', 'missing: Galaxy features off', 'minor', 'emulated; fails without the Galaxy client'),
    (r'EOSSDK', 'Epic Online Services', 'missing: online features off', 'minor', 'emulated (no network on most handhelds)'),
    (r'AVPro', 'AVPro Video', 'missing: the game\'s videos do not play', 'major', 'emulated (software decoding)'),
    (r'^lib(SDL2|sdl2)', 'SDL2 (the game\'s own copy)', 'not used', 'ok', 'box64 uses the device\'s SDL2 instead'),
]

# Libraries box64 0.4.4 runs natively (its src/library_list.h, "wrapped"): the device's aarch64
# copy is used. Anything else a player needs is loaded as x86_64 code (box64's x64lib or the game's).
_WRAPPED = re.compile(r'^lib(c|m|dl|pthread|rt|util|bsd|uuid|resolv|crypt)\.so|^ld-linux|'
                      r'^lib(GL|EGL|GLESv2|GLX|OpenGL|GLU)[._]|^lib(X[A-Za-z0-9-]*|xcb[-a-z0-9]*|xkb[a-z0-9-]*|xshmfence)\.so|'
                      r'^lib(SDL2|sdl2)[-_.]|^lib(asound|pulse|pulse-simple|pulse-mainloop-glib|openal)\.so|'
                      r'^lib(gtk-x11-2\.0|gdk-x11-2\.0|gtk-3|gdk-3|gdk_pixbuf-2\.0|gio-2\.0|gobject-2\.0|glib-2\.0|dbus-1|dbus-glib-1)\.so|'
                      r'^lib(z|udev|vulkan|gbm|drm|freetype|fontconfig|png12|png16|bz2|lzma|expat|curl|curl-gnutls|ssl|crypto|xml2|xslt)\.so|'
                      r'^libwayland-(client|cursor|egl)\.so|^libicu(uc|i18n)\.so')
# x86_64 libraries box64 brings itself (its x64lib folder): ship them with the port
_X64LIB = re.compile(r'^lib(stdc\+\+\.so\.[56]|gcc_s\.so\.1|unwind\.so\.8)$')


# ---------------------------------------------------------------------------------------------
# Binaries (headers only)

def _read(path, n=4096):
    try:
        with open(path, 'rb') as f:
            return f.read(n)
    except OSError:
        return b''


def binary_kind(path):
    """('elf'|'pe'|'macho', arch) of an executable or library, or None."""
    h = _read(path, 4096)
    if h[:4] == b'\x7fELF' and len(h) >= 20:
        end = '<' if h[5] == 1 else '>'
        return 'elf', ELF_MACHINES.get(struct.unpack_from(end + 'H', h, 18)[0], 'other')
    if h[:2] == b'MZ' and len(h) >= 0x40:
        pe, = struct.unpack_from('<I', h, 0x3c)
        h = _read(path, pe + 6)
        if h[pe:pe + 4] == b'PE\0\0':
            return 'pe', PE_MACHINES.get(struct.unpack_from('<H', h, pe + 4)[0], 'other')
        return None
    magic = h[:4]
    if magic in (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe'):
        return 'macho', MACHO_CPUS.get(struct.unpack_from('<i', h, 4)[0], 'other')
    if magic == b'\xca\xfe\xba\xbe' and len(h) >= 8:            # fat (universal) binary
        n, = struct.unpack_from('>I', h, 4)
        if n < 16:
            cpus = [MACHO_CPUS.get(struct.unpack_from('>i', h, 8 + 20 * i)[0], 'other') for i in range(n)]
            return 'macho', '+'.join(cpus)
    return None


def elf_info(path):
    """ELF facts: machine, type, interpreter, DT_NEEDED, SONAME and the highest GLIBC_ / GLIBCXX_
    symbol versions asked for (section headers; {} for a non-ELF file)."""
    try:
        f = open(path, 'rb')
    except OSError:
        return {}
    with f:
        try:
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        except ValueError:
            return {}
        try:
            return _elf_info(m)
        except (struct.error, IndexError, ValueError):
            return {}
        finally:
            m.close()


def _elf_info(m):
    if m[:4] != b'\x7fELF':
        return {}
    is64, end = m[4] == 2, '<' if m[5] == 1 else '>'
    if is64:
        e_type, e_machine = struct.unpack_from(end + 'HH', m, 16)
        e_phoff, e_shoff = struct.unpack_from(end + 'QQ', m, 32)
        e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from(end + 'HHHH', m, 54)
    else:
        e_type, e_machine = struct.unpack_from(end + 'HH', m, 16)
        e_phoff, e_shoff = struct.unpack_from(end + 'II', m, 28)
        e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from(end + 'HHHH', m, 42)
    info = {'machine': ELF_MACHINES.get(e_machine, str(e_machine)), 'bits': 64 if is64 else 32,
            'type': {2: 'exec', 3: 'dyn'}.get(e_type, str(e_type)), 'interp': None, 'needed': [], 'soname': None,
            'glibc': None, 'glibcxx': None}
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type, = struct.unpack_from(end + 'I', m, o)
        if p_type == 3:                                         # PT_INTERP
            off, size = (struct.unpack_from(end + 'QxxxxxxxxxxxxxxxxQ', m, o + 8) if is64 else
                         struct.unpack_from(end + 'IxxxxxxxxI', m, o + 4))
            info['interp'] = bytes(m[off:off + size]).split(b'\0')[0].decode('latin1')
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        if is64:
            _, sh_type, _, _, sh_offset, sh_size, sh_link = struct.unpack_from(end + 'IIQQQQI', m, o)
        else:
            _, sh_type, _, _, sh_offset, sh_size, sh_link = struct.unpack_from(end + 'IIIIIII', m, o)
        secs.append((sh_type, sh_offset, sh_size, sh_link))

    def cstr(sec, off):
        base = secs[sec][1] + off
        return bytes(m[base:m.find(b'\0', base)]).decode('latin1')

    versions = set()
    for sh_type, off, size, link in secs:
        if sh_type == 6:                                        # SHT_DYNAMIC
            ent = 16 if is64 else 8
            for j in range(size // ent):
                tag, val = struct.unpack_from(end + ('qQ' if is64 else 'iI'), m, off + j * ent)
                if tag == 0:
                    break
                if tag == 1:
                    info['needed'].append(cstr(link, val))
                elif tag == 14:
                    info['soname'] = cstr(link, val)
        elif sh_type == 0x6ffffffe:                             # SHT_GNU_verneed
            p = off
            while True:
                vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from(end + 'HHIII', m, p)
                a = p + vn_aux
                for _ in range(vn_cnt):
                    _, _, _, vna_name, vna_next = struct.unpack_from(end + 'IHHII', m, a)
                    versions.add(cstr(link, vna_name))
                    if not vna_next:
                        break
                    a += vna_next
                if not vn_next:
                    break
                p += vn_next
    for key, prefix in (('glibc', 'GLIBC_'), ('glibcxx', 'GLIBCXX_')):
        vs = [v for v in versions if v.startswith(prefix) and re.match(r'^[\d.]+$', v[len(prefix):])]
        if vs:
            info[key] = max(vs, key=lambda v: tuple(int(x) for x in v[len(prefix):].split('.')))
    return info


_GFX_FLAGS = re.compile(rb'force-(glcore[0-9]*|vulkan|opengl|gles[0-9a-z]*|wayland|gfx-direct)\0')
_DLOPEN = re.compile(rb'(lib[A-Za-z0-9_+-]+\.so(?:\.[0-9]+)*)\0')


def player_strings(path):
    """What the player binary (UnityPlayer.so or the executable) can do, from its strings: the
    -force-* renderer switches and the shared libraries it names (dlopen) - read, never run."""
    out = {'flags': [], 'libs': [], 'egl': False, 'glx_es': False}
    try:
        with open(path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
            out['flags'] = sorted({x.decode() for x in _GFX_FLAGS.findall(m)})
            out['libs'] = sorted({x.decode() for x in _DLOPEN.findall(m)})
            out['egl'] = m.find(b'eglCreateWindowSurface\0') >= 0
            # the player's built-in SDL2 asks GLX for OpenGL ES contexts with this extension: the
            # -force-gles mode (its switch is built in code, not a string)
            out['glx_es'] = m.find(b'GLX_EXT_create_context_es2_profile\0') >= 0
    except (OSError, ValueError):
        pass
    return out


# ---------------------------------------------------------------------------------------------
# The build's layout

def _is_data(d):
    return any(os.path.isfile(os.path.join(d, n)) for n in DATA_MARKERS)


def find_data_dirs(path, depth=4):
    """Data folders (<Game>_Data, <Game>.app/Contents/Resources/Data) at or below path."""
    path = os.path.abspath(path)
    if os.path.isfile(path):
        path = os.path.dirname(path)
    if _is_data(path):
        return [path]
    app = os.path.join(path, 'Contents', 'Resources', 'Data')
    if _is_data(app):
        return [app]
    found = []
    for root, dirs, _ in os.walk(path):
        if root[len(path):].count(os.sep) >= depth:
            dirs[:] = []
            continue
        for d in sorted(dirs):
            p = os.path.join(root, d)
            if d.endswith('_Data') and _is_data(p):
                found.append(p)
            elif d.endswith('.app') and _is_data(os.path.join(p, 'Contents', 'Resources', 'Data')):
                found.append(os.path.join(p, 'Contents', 'Resources', 'Data'))
        dirs[:] = [d for d in dirs if not d.endswith(('_Data', '.app'))]
    return found


def _sf_head(data):
    for n in ('globalgamemanagers', 'mainData'):
        p = os.path.join(data, n)
        if os.path.isfile(p):
            return _read(p, 512)
    return b''


def _lenient_version(data):
    """The Unity version of data in a SerializedFile format newer than unity_version knows (format
    23+, Unity 6.x): the version string still follows the 48-byte header of format 22."""
    head = _sf_head(data)
    if len(head) < 64:
        return None
    fmt, = struct.unpack_from('>I', head, 8)
    if fmt < 22:
        return None
    end = head.find(b'\0', 48)
    try:
        return UV.Version(head[48:end].decode('ascii')) if end > 48 else None
    except (ValueError, UnicodeDecodeError):
        return None


def data_platform(data):
    """The build target the data was made for (from its first serialized file's header)."""
    head = _sf_head(data)
    if not head:
        return None
    try:
        fmt, _, off, big = UV.sf_header(head)
    except ValueError:
        try:                                    # format 23+: laid out as 22
            fmt, = struct.unpack_from('>I', head, 8)
            end = head.index(b'\0', 48)
            off, big = end + 1, head[16] != 0
        except (ValueError, struct.error):
            return None
        if fmt < 22:
            return None
    try:
        t, = struct.unpack_from('>i' if big else '<i', head, off)
    except struct.error:
        return None
    return PLATFORMS.get(t, f'target {t}')


def _first(paths):
    return next((p for p in paths if os.path.exists(p)), None)


def layout(data):
    """Where a build's pieces are, from its data folder: the executable(s), UnityPlayer, the IL2CPP
    code, the Mono runtime, the plugins."""
    data = os.path.abspath(data)
    L = {'data': data, 'kind': None, 'game_dir': None, 'name': None, 'exes': [], 'player_lib': None,
         'game_assembly': None, 'mono': None, 'plugins_dir': None}
    if data.endswith(os.path.join('Contents', 'Resources', 'Data')):
        contents = os.path.dirname(os.path.dirname(data))
        L.update(kind='macos-app', game_dir=os.path.dirname(contents),
                 name=os.path.basename(os.path.dirname(contents))[:-len('.app')])
        macos = os.path.join(contents, 'MacOS')
        if os.path.isdir(macos):
            L['exes'] = [os.path.join(macos, n) for n in sorted(os.listdir(macos))
                         if binary_kind(os.path.join(macos, n))]
        fw = os.path.join(contents, 'Frameworks')
        L['player_lib'] = _first([os.path.join(fw, 'UnityPlayer.dylib')])
        L['game_assembly'] = _first([os.path.join(fw, 'GameAssembly.dylib')])
        L['mono'] = _first([os.path.join(contents, 'MonoBleedingEdge'), os.path.join(contents, 'Mono'),
                            os.path.join(fw, 'MonoEmbedRuntime'), os.path.join(fw, 'MonoBleedingEdge')])
        L['plugins_dir'] = _first([os.path.join(contents, 'PlugIns'), os.path.join(contents, 'Plugins')])
        return L
    gd = os.path.dirname(data)
    name = os.path.basename(data)
    name = name[:-len('_Data')] if name.endswith('_Data') else name
    L.update(kind='folder', game_dir=gd, name=name)
    exes = []
    for n in sorted(os.listdir(gd)):
        p = os.path.join(gd, n)
        if not os.path.isfile(p) or not (n == name or n.startswith(name + '.')) or n.endswith('.debug'):
            continue
        k = binary_kind(p)
        if k and k[0] == 'elf' and elf_info(p).get('interp') or k and k[0] == 'pe' and n.lower().endswith('.exe'):
            exes.append(p)
    L['exes'] = exes
    L['player_lib'] = _first([os.path.join(gd, n) for n in ('UnityPlayer.so', 'UnityPlayer.dll')])
    L['game_assembly'] = _first([os.path.join(gd, n) for n in ('GameAssembly.so', 'GameAssembly.dll')])
    L['mono'] = _first([os.path.join(data, 'MonoBleedingEdge'), os.path.join(data, 'Mono'),
                        os.path.join(gd, 'MonoBleedingEdge'), os.path.join(gd, 'Mono')])
    L['plugins_dir'] = _first([os.path.join(data, 'Plugins')])
    return L


def plugins(plugins_dir):
    """Native plugins under the build's plugin folder: [{file, format, arch, known, ...}]."""
    out = []
    if not plugins_dir:
        return out
    for root, dirs, files in os.walk(plugins_dir):
        for n in sorted(files + [d for d in dirs if d.endswith('.bundle')]):
            p = os.path.join(root, n)
            if n.endswith('.bundle'):
                macos = os.path.join(p, 'Contents', 'MacOS')
                bins = [os.path.join(macos, x) for x in sorted(os.listdir(macos))] if os.path.isdir(macos) else []
                k = next((binary_kind(b) for b in bins if binary_kind(b)), None)
            elif n.endswith(('.so', '.dll', '.dylib')) or '.so.' in n:
                k = binary_kind(p)
            else:
                continue
            if not k:
                continue
            e = {'file': os.path.relpath(p, plugins_dir), 'format': k[0], 'arch': k[1]}
            for rx, what, android, sev, box64 in KNOWN_PLUGINS:
                if re.search(rx, n, re.I):
                    e.update(what=what, android=android, severity=sev, box64=box64)
                    break
            else:
                e.update(what=None, android='an x86-only native plugin: the Android player needs an ARM build of it '
                                            '(or a game that runs without it)', severity='major', box64='emulated')
            out.append(e)
        dirs[:] = [d for d in dirs if not d.endswith('.bundle')]
    return out


# ---------------------------------------------------------------------------------------------
# The verdict

def _android_range():
    return '%d.%d - %d.%d' % (UV.FIRST + UV.LAST)


def examine(data):
    """All facts about one build (data folder) and the verdict for both routes."""
    L = layout(data)
    facts = {'data': data, 'name': L['name'], 'layout_kind': L['kind']}
    try:
        v = UV.of_data(data)
    except (UV.Unsupported, ValueError, OSError) as ex:
        v = _lenient_version(data)
        if v is None:
            facts['unity_error'] = str(ex)
    facts['unity'] = v.text if v else None
    facts['compressed'] = os.path.isfile(os.path.join(data, 'data.unity3d')) and not os.path.isfile(
        os.path.join(data, 'globalgamemanagers'))
    facts['platform'] = data_platform(data) or ('macos' if L['kind'] == 'macos-app' else None)
    il2cpp = bool(L['game_assembly']) or os.path.isdir(os.path.join(data, 'il2cpp_data'))
    mono = os.path.isdir(os.path.join(data, 'Managed')) and not il2cpp
    facts['backend'] = 'il2cpp' if il2cpp else 'mono' if mono else None
    facts['mono_runtime'] = (None if not mono or not L['mono'] else
                             'bdwgc (MonoBleedingEdge, .NET 4.x)' if 'BleedingEdge' in L['mono'] or
                             'EmbedRuntime' in L['mono'] else 'legacy Mono (.NET 2.0/3.5)')
    exes = []
    for p in L['exes']:
        k = binary_kind(p) or ('?', '?')
        e = {'file': os.path.relpath(p, L['game_dir']), 'format': k[0], 'arch': k[1]}
        if k[0] == 'elf':
            ei = elf_info(p)
            e.update(needed=ei.get('needed', []), glibc=ei.get('glibc'), glibcxx=ei.get('glibcxx'))
        exes.append(e)
    facts['executables'] = exes
    if facts['platform'] is None:               # a compressed build: its serialized files are in data.unity3d
        kinds = {e['format'] for e in exes}
        facts['platform'] = {frozenset({'pe'}): 'windows', frozenset({'elf'}): 'linux'}.get(frozenset(kinds))
    elf = sorted((e for e in exes if e['format'] == 'elf'), key=lambda e: e['arch'] != 'x86_64')
    player = L['player_lib'] if L['player_lib'] and L['player_lib'].endswith('.so') else next(
        (os.path.join(L['game_dir'], e['file']) for e in elf), None)
    facts['player_binary'] = os.path.relpath(player, L['game_dir']) if player else None
    if player:
        ps = player_strings(player)
        pi = elf_info(player)
        facts['player'] = {'renderers': _renderers(ps, v), 'flags': ps['flags'], 'egl': ps['egl'],
                           'needed': pi.get('needed', []), 'dlopen': ps['libs'], 'glibc': pi.get('glibc'),
                           'glibcxx': pi.get('glibcxx')}
    if L['game_assembly']:
        gi = elf_info(L['game_assembly'])
        facts['game_assembly'] = {'file': os.path.relpath(L['game_assembly'], L['game_dir']),
                                  'arch': (binary_kind(L['game_assembly']) or ('', '?'))[1],
                                  'needed': gi.get('needed', []), 'glibc': gi.get('glibc')}
    facts['plugins'] = plugins(L['plugins_dir'])
    facts['libraries'] = _libraries(facts)
    facts['android'] = _android_verdict(facts, v)
    facts['box64'] = _box64_verdict(facts, v)
    a, b = facts['android']['fits'], facts['box64']['fits']
    facts['route'] = 'android' if a else 'box64' if b else None
    return facts


def _renderers(ps, v):
    flags = ps['flags']
    r = []
    if any(f.startswith('glcore') for f in flags):
        r.append('glcore')
    if 'vulkan' in flags:
        r.append('vulkan')
    # Unity 5.x before 5.5 kept the legacy OpenGL 2 renderer on Linux (-force-opengl); later players
    # still accept the switch, as a name for OpenGL Core
    if 'opengl' in flags and (v is None or v.line < (5, 5)):
        r.append('opengl2')
    # OpenGL ES through -force-gles (seen working on 2017.4 - 2022.3 Linux players)
    if 'glcore' in r and ps.get('glx_es') and (v is None or v.line >= (2017, 1)):
        r.append('gles')
    return r


_GLIBC = re.compile(r'^(ld-linux.*|lib(c|m|dl|pthread|rt|util|resolv|crypt|anl|nsl)\.so\.\d+)$')
# names a player opens at run time (dlopen) that matter for a port; SDL's GLES 1 renderer and the
# like are left out
_RUNTIME_LIBS = re.compile(r'^lib(GL|EGL|GLESv2|X11|Xext|Xcursor|Xrandr|Xi|Xinerama|Xss|Xxf86vm|xkbcommon|'
                           r'wayland-(client|cursor|egl)|asound|pulse-simple|udev|vulkan|dbus-1|gtk-x11-2\.0)\.so\.\d+$')


def _libraries(facts):
    """Shared libraries the Linux build needs, split as box64 loads them: native = the device's
    aarch64 copy (box64 wraps it), x64lib = box64's own x86_64 copy, game = shipped with the game,
    emulated = an x86_64 copy the port has to bring. 'runtime' ones are opened with dlopen when
    present (GL, X11, audio, input); the others are DT_NEEDED. glibc's own are left out."""
    needed, runtime = set(), set()
    for e in facts['executables']:
        needed.update(e.get('needed', []))
    p = facts.get('player') or {}
    needed.update(p.get('needed', []))
    runtime.update(n for n in p.get('dlopen', []) if _RUNTIME_LIBS.match(n))
    needed.update((facts.get('game_assembly') or {}).get('needed', []))
    own = {os.path.basename(x['file']) for x in facts['plugins']} | {'UnityPlayer.so', 'GameAssembly.so'}
    out = {'native': [], 'x64lib': [], 'game': [], 'emulated': [], 'runtime': sorted(runtime - needed)}
    for n in sorted(needed | runtime):
        if _GLIBC.match(n):
            continue
        if n in own:
            out['game'].append(n)
        elif _X64LIB.match(n):
            out['x64lib'].append(n)
        elif _WRAPPED.match(n):
            out['native'].append(n)
        else:
            out['emulated'].append(n)
    return out


def _android_verdict(f, v):
    why, block, caveats = [], [], []
    if v is None:
        block.append('the Unity version cannot be read from the data')
    elif not UV.FIRST <= v.line <= UV.LAST:
        block.append(f'Unity {v}: the converter and runtime cover {_android_range()}')
    else:
        why.append(f'Unity {v} is in the covered range {_android_range()}')
    if f['backend'] == 'il2cpp':
        block.append('IL2CPP build: the game\'s code is compiled x86 machine code, there is no IL to run on '
                     'the Android player\'s Mono (unless the game\'s own Android build is owned)')
    elif f['backend'] == 'mono':
        why.append('Mono build: its assemblies run on the Android player\'s Mono')
    else:
        block.append('scripting backend not found (no Managed/ or il2cpp_data/)')
    plat = f['platform']
    if plat in ('linux', 'macos'):
        why.append(f'{plat} build: OpenGL Core shaders, converted to GLES 3')
    elif plat == 'windows':
        caveats.append('Windows build: Direct3D 11 shaders only, translated with `shaders.from = ["d3d11"]` '
                       '(tools/hlslcc; pmhlslcc for the handheld)')
        if v is not None and v.line >= (2021, 2):
            caveats.append('Windows build of 2021.2+: its class libraries are the Windows-only unityjit-win32 ones; '
                           '`python -m pmunity.winbcl` fetches the Unix ones for `android-data --managed-file`')
    elif plat:
        block.append(f'{plat} build: not a desktop build')
    if f.get('compressed'):
        caveats.append('compressed build (data.unity3d): `pmunity unpack` first')
    for p in f['plugins']:
        if p['arch'] in ('arm64', 'armv7'):
            continue
        msg = f"plugin {p['file']}" + (f" ({p['what']})" if p['what'] else '') + f": {p['android']}"
        if p['severity'] == 'major':
            block.append(msg) if not p['what'] else caveats.append(msg + ' [major]')
        elif p['severity'] == 'minor':
            caveats.append(msg)
    return {'fits': not block, 'why': why, 'blockers': block, 'caveats': caveats}


def _box64_verdict(f, v):
    why, block, caveats, needs = [], [], [], []
    linux = [e for e in f['executables'] if e['format'] == 'elf']
    x64 = [e for e in linux if e['arch'] == 'x86_64']
    x86 = [e for e in linux if e['arch'] == 'x86']
    if x64:
        why.append(f"Linux x86_64 build ({x64[0]['file']}): runs unmodified under box64")
    elif x86:
        why.append(f"Linux x86 (32-bit) build ({x86[0]['file']})")
        caveats.append('32-bit x86: needs box86 on a 32-bit ARM userland (armhf libraries, GL and SDL2), '
                       'or box64 built with BOX32 (experimental); see docs/box64-route.md')
    elif f['backend'] == 'mono' and f['platform'] == 'macos' and v is not None:
        why.append(f'macOS Mono build: its data runs on Unity\'s own Linux player of {v} (Unity\'s Linux '
                   'Mono support installer), under box64')
        caveats.append('the Linux player comes from Unity\'s installer (download on the device, not shipped)')
    elif f['backend'] == 'il2cpp':
        block.append(f"IL2CPP {f['platform'] or ''} build without a Linux one: its code is compiled for "
                     f"{f['platform'] or 'another system'}; get the game's Linux build")
    elif f['backend'] == 'mono' and f['platform'] == 'windows':
        block.append('Windows-only Mono build: Unity\'s Linux player needs OpenGL Core shaders and this data '
                     'has Direct3D ones; get the macOS or Linux build')
    else:
        block.append('no Linux build found')
    p = f.get('player') or {}
    r = p.get('renderers', [])
    if 'opengl2' in r:
        needs.append('OpenGL 2.1 (legacy renderer): gl4es on GLES 2 works (-force-opengl)')
    if 'glcore' in r:
        needs.append('desktop OpenGL 3.2+ core for the OpenGL Core renderer: Mesa (Panfrost / Turnip / llvmpipe); '
                     'gl4es (GL 2.1) is not enough')
    if 'gles' in r:
        needs.append('or, on GLES-only drivers, the player\'s OpenGL ES mode: -force-gles with GLES 3 shaders added '
                     '(pmunity convert --config tools/box64/gles.json) and glxes (GLX for ES contexts on EGL)')
    needs.append('an X11 display: Xwayland or PortMaster\'s Westonpack on firmwares without X')
    libs = f['libraries']
    if libs['native']:
        needs.append('native (aarch64) libraries box64 maps the x86_64 ones to: ' + ', '.join(libs['native']))
    if libs['x64lib']:
        needs.append('x86_64 libraries from box64\'s x64lib, shipped with the port: ' + ', '.join(libs['x64lib']))
    if libs['emulated']:
        caveats.append('x86_64 libraries box64 does not wrap; the port must ship x86_64 copies: '
                       + ', '.join(libs['emulated']))
    if f['backend'] == 'mono':
        caveats.append('Mono JIT under emulation: expect about half the Android route\'s frame rate '
                       '(Mono AOT of the assemblies helps first-use hitches)')
    for pl in f['plugins']:
        if pl['arch'] in ('x86_64', 'x86') and pl['what'] and 'fails without' in pl['box64']:
            caveats.append(f"plugin {pl['file']} ({pl['what']}): {pl['box64']}")
    return {'fits': not block, 'why': why, 'blockers': block, 'caveats': caveats, 'needs': needs}


# ---------------------------------------------------------------------------------------------

def report(facts):
    """Human-readable lines for one build."""
    f = facts
    out = [f"{f['name'] or '?'}: {f['data']}"]
    out.append(f"  Unity {f['unity'] or '? (' + f.get('unity_error', '') + ')'}, {f['platform'] or '?'} build, "
               f"{f['backend'] or '? backend'}" + (f", {f['mono_runtime']}" if f.get('mono_runtime') else ''))
    for e in f['executables']:
        out.append(f"  executable {e['file']}: {e['format']} {e['arch']}" +
                   (f", needs {e['glibc']}" if e.get('glibc') else ''))
    if f.get('player'):
        p = f['player']
        out.append(f"  player {f['player_binary']}: renderers {', '.join(p['renderers']) or '?'}"
                   + (f"; x86_64 side asks for {p['glibc']}" if p.get('glibc') else ''))
    if f.get('game_assembly'):
        out.append(f"  IL2CPP code {f['game_assembly']['file']} ({f['game_assembly']['arch']})")
    for pl in f['plugins']:
        out.append(f"  plugin {pl['file']}: {pl['format']} {pl['arch']}" + (f" ({pl['what']})" if pl['what'] else ''))
    route = f['route']
    out.append(f"route: {route or 'none'}" + {'android': ' (Unity\'s Android player, data converted)',
                                               'box64': ' (the game\'s Linux build under box64)'}.get(route, ''))
    for name in ('android', 'box64'):
        r = f[name]
        out.append(f"  {name}: {'fits' if r['fits'] else 'does not fit'}")
        for k in ('why', 'blockers', 'caveats', 'needs'):
            for line in r.get(k, []):
                out.append(f"    {k[:-1] if k.endswith('s') else k}: {line}")
    return out


HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE_DIR = os.path.join(HERE, 'port_template', 'box64')       # the box64 route's port templates
TOOLS = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'tools', 'box64')


def gl_mode(facts):
    """How the game draws on a handheld: gles (the player's OpenGL ES mode; GLES-only drivers are
    the common case) when the player has it and the converter can add GLES 3 shaders to the data
    (its Unity versions), else gl4es for the legacy OpenGL 2 renderer (5.4 and older), else mesa
    (OpenGL Core: a desktop-GL driver such as Panfrost)."""
    r = (facts.get('player') or {}).get('renderers', [])
    try:
        v = UV.Version(facts['unity']) if facts.get('unity') else None
    except ValueError:
        v = None
    if 'gles' in r and v is not None and UV.FIRST <= v.line <= UV.LAST:
        return 'gles'
    if 'opengl2' in r:
        return 'gl4es'
    return 'mesa'


def game_env(facts, base):
    """game.env for the box64 launcher (shell assignments): the game's folder relative to base
    (the port's gamedata/), its executable and data folder, the GL mode."""
    import shlex
    x64 = next(e for e in facts['executables'] if e['format'] == 'elf' and e['arch'] == 'x86_64')
    game_dir = os.path.relpath(os.path.dirname(facts['data']), os.path.abspath(base))
    vals = {'GAME_DIR': game_dir, 'EXE': os.path.basename(x64['file']), 'DATA': os.path.basename(facts['data']),
            'UNITY': facts['unity'] or '', 'BACKEND': facts['backend'] or '', 'GL_MODE': gl_mode(facts)}
    return ''.join(f'{k}={shlex.quote(v)}\n' for k, v in vals.items())


TOKENS = {'READY': '.setup_done', 'SETUP_TIME': 'a few minutes', 'ROUTE': 'box64'}


def launcher(facts, template=None, port=None, title=None, **extra):
    """The box64 route's launcher (port_template/box64/launcher.sh) with its @@NAME@@ values filled,
    as `pmunity port new` fills them; extra = more values (upper-cased keys)."""
    with open(template or os.path.join(TEMPLATE_DIR, 'launcher.sh')) as f:
        text = f.read()
    port = port or re.sub(r'[^a-z0-9]+', '', (facts['name'] or 'game').lower()) or 'game'
    title = title or facts['name'] or port
    vals = dict(TOKENS, TITLE=title, PORTDIR=port, ZIP=f'{port}.zip', LAUNCHER=f'{title}.sh',
                GAME_WHAT=f"the Linux build of {title}")
    vals.update({k.upper(): str(v) for k, v in extra.items()})
    missing = sorted(set(re.findall(r'@@([A-Z0-9_]+)@@', text)) - set(vals))
    if missing:
        raise ValueError('template values missing: ' + ', '.join(missing))
    return re.sub(r'@@([A-Z0-9_]+)@@', lambda m: vals[m.group(1)], text)


# ---------------------------------------------------------------------------------------------
# `pmunity port` extension points (port.py calls them in this module: HOOK_MODULES)

def port_check(rep):
    """Adds what this module knows to the box64 route candidate of a port check."""
    if not rep.get('data') or not os.path.isdir(rep['data']):
        return
    f = examine(rep['data'])
    r = next((x for x in rep.get('routes', []) if x['route'] == 'box64'), None)
    if r is None or not r['ok']:
        return
    apis = rep.get('graphics_apis')
    if apis is not None and not {'OpenGLCore', 'OpenGL2'} & set(apis):
        # (Vulkan only: HDRP's Linux builds, or OpenGL Core taken out of the build's graphics APIs)
        r['ok'] = False
        r['why'].append(f"the data has {', '.join(apis) or 'no'} shaders, no OpenGL ones: the box64 port draws with "
                        f"OpenGL (gles, gl4es or Mesa); this player would need Vulkan on the handheld")
        return
    r['needs'] += [f"GL mode {gl_mode(f)} ({'GLES 3 shaders added by the setup' if gl_mode(f) == 'gles' else 'data as it is'})"]
    r['needs'] += [c for c in f['box64']['caveats'] if 'Mono JIT' in c or 'plugin' in c]


def port_new(ctx):
    """What a box64 port needs besides its templates: the setup's shader config, and port.json
    as PortMaster should list it (Westonpack, no 32-bit userland, box64's glibc)."""
    p = ctx.cfg['port']
    if p['route'] != 'box64':
        return
    with open(os.path.join(TOOLS, 'gles.json')) as f:
        ctx.write(f"{p['name']}/setup/gles.json", f.read())
    pj_path = os.path.join(ctx.out, p['name'], 'port.json')
    if not os.path.isfile(pj_path):
        return
    with open(pj_path) as f:
        pj = json.load(f)
    a = pj['attr']
    if not p.get('desc'):
        a['desc'] = f"{p['title']}: the game's own Linux build under box64."
    if not p.get('inst'):
        a['inst'] = (f"Requires {ctx.cfg['game']['what']}: put it in ports/{p['name']}/gamedata. The first launch "
                     f"prepares it ({p['setup_time']}).")
    a['runtime'] = sorted(set(a.get('runtime') or []) | {'weston_pkg_0.2.squashfs'})
    a['reqs'] = [r for r in a.get('reqs') or [] if r != 'armhf']
    a['min_glibc'] = '2.29'                     # box64 built on Debian 11 (tools/box64/build.sh)
    ctx.write(f"{p['name']}/port.json", json.dumps(pj, indent=4) + '\n')
    # port.env: the launcher's options (port.toml's [runtime] values, then the others as comments)
    env = os.path.join(ctx.out, p['name'], 'port.env')
    kept = []
    if os.path.isfile(env):
        with open(env) as f:
            kept = [l for l in f.read().splitlines() if l and not l.startswith('#')]
    ctx.write(f"{p['name']}/port.env", '\n'.join(
        ["# The game's options for the box64 launcher (docs/box64-route.md in portmaster-unity), sourced by it;",
         '# a variable already set in the environment wins. Uncomment to change:'] + kept +
        ['#PMU_GL=gles          # gles | mesa | gl4es (default: what the setup found, in game.env)',
         '#PMU_BOX64=default    # safe | default | fast (box64.env)',
         '#PMU_MAXCPU=2         # CPUs box64 reports to the game (Unity sizes its job threads by it)',
         '#PMU_ARGS="-force-gfx-direct"   # more arguments for the game',
         '#PMU_DYNACACHE=0      # box64 translates the game again at every start']) + '\n')


def port_package(ctx):
    """The release zip's box64 part: box64 and its x86-64 libraries, glxes, gl4es (tools/box64/build.sh
    output: $BOX64_OUT or <runtime>/tools/box64/out), box64.env, unityprefs.sh, their licences."""
    if ctx.cfg['port']['route'] != 'box64':
        return
    out = os.environ.get('BOX64_OUT') or os.path.join(ctx.runtime, 'tools', 'box64', 'out')
    need = ['box64/box64', 'box64/x64lib', 'glxes/libGL.so.1', 'gl4es/libGL.so.1', 'licenses']
    missing = [n for n in need if not os.path.exists(os.path.join(out, n))]
    if missing:
        raise SystemExit(f"box64 route: {out} lacks {', '.join(missing)} (run tools/box64/build.sh, or set BOX64_OUT)")
    ctx.add(os.path.join(out, 'box64', 'box64'), 'box64/box64', 0o755)
    ctx.add(os.path.join(out, 'box64', 'x64lib'), 'box64/x64lib')
    ctx.add(os.path.join(out, 'glxes', 'libGL.so.1'), 'glxes/libGL.so.1')
    ctx.add(os.path.join(out, 'gl4es', 'libGL.so.1'), 'gl4es/libGL.so.1')
    tools = os.path.join(ctx.runtime, 'tools', 'box64')
    for n in ('box64.env', 'unityprefs.sh'):
        ctx.add(os.path.join(tools, n), f'box64/{n}')
    for n in sorted(os.listdir(os.path.join(out, 'licenses'))):
        ctx.license(n, os.path.join(out, 'licenses', n))


def main(argv=None):
    ap = argparse.ArgumentParser(prog='pmunity route', description='which route fits a Unity game, and why')
    ap.add_argument('path', help='game folder, <Game>_Data, <Game>.app or the executable')
    ap.add_argument('--json', action='store_true', help='all facts as JSON')
    ap.add_argument('--env', action='store_true',
                    help="game.env of the first build that fits the box64 route (shell assignments, paths relative "
                         "to <path>): what the box64 port's setup writes for its launcher")
    ap.add_argument('--launcher', metavar='OUT', help="write the box64 route's launcher for the game (a port made "
                                                     "with `pmunity port new --route box64` has it already)")
    ap.add_argument('--port', help="the port's folder name under ports/ (default: from the game's name)")
    ap.add_argument('--title', help="the game's title in the launcher")
    ap.add_argument('--template', default=None, help='launcher template (default: port_template/box64/launcher.sh)')
    a = ap.parse_args(argv)
    dirs = find_data_dirs(a.path)
    if not dirs:
        sys.exit(f'{a.path}: no Unity data folder (<Game>_Data with globalgamemanagers, mainData or data.unity3d)')
    all_facts = [examine(d) for d in dirs]
    if a.env:
        f = next((x for x in all_facts if x['box64']['fits'] and
                  any(e['format'] == 'elf' and e['arch'] == 'x86_64' for e in x['executables'])), None)
        if not f:
            sys.exit(f'{a.path}: no Linux x86-64 build here')
        sys.stdout.write(game_env(f, a.path if os.path.isdir(a.path) else os.path.dirname(a.path)))
        return
    if a.json:
        print(json.dumps(all_facts if len(all_facts) > 1 else all_facts[0], indent=1))
    else:
        for i, f in enumerate(all_facts):
            if i:
                print()
            print('\n'.join(report(f)))
    if a.launcher:
        f = next((x for x in all_facts if x['box64']['fits']), None)
        if not f:
            sys.exit('no build here fits the box64 route: no launcher written')
        text = launcher(f, a.template, port=a.port, title=a.title)
        with open(a.launcher, 'w') as out:
            out.write(text)
        print(f'launcher: {a.launcher}', file=sys.stderr)


if __name__ == '__main__':
    main()
