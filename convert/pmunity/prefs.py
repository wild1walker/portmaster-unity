"""`python -m pmunity prefs`: a game's PlayerPrefs between a PC and the handheld.

    python -m pmunity prefs show <file>
    python -m pmunity prefs copy <from> <to> [--all]

Formats (by content; a new <to> by its name):
  - Unity's Linux player: `~/.config/unity3d/<Company>/<Product>/prefs`, XML
    (`<pref name="..." type="int|float|string">`, strings base64-encoded UTF-8);
  - ua_host (the Android player's SharedPreferences): `ports/<port>/data/shared_prefs/
    <package>.v2.playerprefs.prefs`, one `<i|f|s>\\t<key>\\t<value>` line per entry, `%XX` for
    tab, newline, carriage return and `%`.

`copy` puts <from>'s entries into <to> (made if missing) and writes <to> in its own format; <to>'s
other entries stay. The display settings a player keeps per machine (`Screenmanager *`,
`UnitySelectMonitor`) are left as <to> has them unless --all. Windows keeps PlayerPrefs in the
registry (HKCU\\Software\\<Company>\\<Product>) and macOS in ~/Library/Preferences/
unity.<Company>.<Product>.plist: neither is read here. docs/android-data.md ("Saves") has where
each platform keeps saves.
"""
import argparse
import base64
import os
import re
import sys
import xml.etree.ElementTree as ET

PER_MACHINE = re.compile(r'^(Screenmanager .*|UnitySelectMonitor)$')
TYPES = {'int': 'i', 'float': 'f', 'string': 's'}


def _esc(s):
    return ''.join(f'%{ord(c):02X}' if c in '%\t\n\r' else c for c in s)


def _unesc(s):
    return re.sub(r'%([0-9A-Fa-f]{2})', lambda m: chr(int(m.group(1), 16)), s)


def kind(path):
    """'linux' | 'handheld' for an existing file, by its content."""
    with open(path, 'rb') as f:
        head = f.read(256).lstrip()
    if head.startswith(b'<'):
        return 'linux'
    return 'handheld'


def kind_for_new(path):
    return 'handheld' if path.endswith('.prefs') else 'linux'


def read(path):
    """[(key, type 'i'|'f'|'s', value)] in the file's order."""
    out = []
    if kind(path) == 'linux':
        root = ET.parse(path).getroot()
        if root.tag != 'unity_prefs':
            raise ValueError(f'{path}: not a Unity prefs file (<{root.tag}>)')
        for p in root.iter('pref'):
            t = TYPES.get(p.get('type'))
            if t is None:
                raise ValueError(f'{path}: pref {p.get("name")!r}: type {p.get("type")!r}')
            text = (p.text or '').strip()
            v = base64.b64decode(text).decode('utf-8') if t == 's' else int(text) if t == 'i' else float(text)
            out.append((p.get('name'), t, v))
        return out
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if len(line) < 3 or line[1] != '\t' or line[0] not in 'ifs':
                continue
            key, _, val = line[2:].partition('\t')
            t = line[0]
            out.append((_unesc(key), t, _unesc(val) if t == 's' else int(val) if t == 'i' else float(val)))
    return out


def _fmt_float(v):
    s = '%.9g' % v
    return s


def write(path, entries, fmt):
    tmp = path + '.tmp'
    if fmt == 'linux':
        lines = ['<unity_prefs version_major="1" version_minor="1">']
        names = {'i': 'int', 'f': 'float', 's': 'string'}
        for k, t, v in entries:
            val = base64.b64encode(v.encode('utf-8')).decode() if t == 's' else str(v) if t == 'i' else _fmt_float(v)
            lines.append(f'\t<pref name="{_xml_attr(k)}" type="{names[t]}">{val}</pref>')
        lines.append('</unity_prefs>')
        text = '\n'.join(lines) + '\n'
    else:
        text = ''.join(f'{t}\t{_esc(k)}\t{_esc(v) if t == "s" else v if t == "i" else _fmt_float(v)}\n' for k, t, v in entries)
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    with open(tmp, 'w', encoding='utf-8') as f:
        f.write(text)
    os.replace(tmp, path)


def _xml_attr(s):
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')


def copy(src, dst, keep_all=False):
    """<src>'s entries into <dst> (its own format); returns (copied, kept per-machine)."""
    new = read(src)
    old = read(dst) if os.path.exists(dst) else []
    fmt = kind(dst) if os.path.exists(dst) else kind_for_new(dst)
    take = [(k, t, v) for k, t, v in new if keep_all or not PER_MACHINE.match(k)]
    names = {k for k, _, _ in take}
    merged = [e for e in old if e[0] not in names] + take
    write(dst, merged, fmt)
    return len(take), len(new) - len(take)


def main(argv=None):
    ap = argparse.ArgumentParser(prog='pmunity prefs', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('show')
    s.add_argument('file')
    c = sub.add_parser('copy')
    c.add_argument('src')
    c.add_argument('dst')
    c.add_argument('--all', action='store_true', help='also the per-machine display settings (Screenmanager *)')
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'show':
            for k, t, v in read(a.file):
                print(f'{ {"i": "int", "f": "float", "s": "string"}[t]:6} {k} = {v!r}')
            return 0
        n, skipped = copy(a.src, a.dst, a.all)
        print(f'{n} entries -> {a.dst}' + (f' ({skipped} display settings left as they were: --all copies them)' if skipped else ''))
        return 0
    except (OSError, ValueError, ET.ParseError) as ex:
        sys.exit(f'prefs: {ex}')


if __name__ == '__main__':
    main()
