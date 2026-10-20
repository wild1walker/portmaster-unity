#!/usr/bin/env python3
"""Members of the player's own Java classes (com.unity3d.*, org.fmod.*, bitter.*, com.google.*)
that libunity can reach by name: non-native methods and fields whose name occurs in libunity as
a NUL-terminated string (JNI looks members up by name). Natives are left out (libunity registers
them, see jnitables.py).

usage: python3 -I javacalls.py javap_s.txt libunity.so > javacalls.tsv
Output rows as jnibindings.py writes them (class, member, descriptor, kind), for jnicover.
"""
import sys, re

PKG = ('com.unity3d.', 'org.fmod.', 'bitter.', 'com.google.')
# Java-internal: anonymous classes and the usual interface methods they implement
COMMON = {'run', 'equals', 'hashCode', 'toString', 'handleMessage', 'onClick', 'invoke', 'compare',
          'surfaceChanged', 'surfaceCreated', 'surfaceDestroyed', 'onCancel', 'onDismiss', 'onKey',
          'onTouch', 'onFrameAvailable', 'getInstance', 'values', 'valueOf', 'close', 'clone'}


def main():
    jp, lib = sys.argv[1], sys.argv[2]
    data = open(lib, 'rb').read()
    names = set(m.group(1).decode() for m in re.finditer(rb'\0([A-Za-z_$][\w$]{2,80})(?=\0)', data))
    cls = None
    pending = None
    rows = set()
    for line in open(jp):
        line = line.rstrip('\n')
        m = re.match(r'^(?:[\w ]*?)(?:class|interface|enum) ([\w.$]+)', line)
        if m and not line.startswith(' '):
            cls = m.group(1) if m.group(1).startswith(PKG) and not re.search(r'\$\d+', m.group(1)) else None
            pending = None
            continue
        if cls is None:
            continue
        s = line.strip()
        if s.startswith('descriptor:') and pending:
            name, kind = pending
            desc = s.split(':', 1)[1].strip()
            if name in names and name not in COMMON:
                rows.add((cls.replace('.', '/'), name, desc, kind))
            pending = None
            continue
        if not s or s == '}' or ' native ' in ' ' + s + ' ':
            pending = None
            continue
        m = re.search(r'([\w$<>]+)\((.*)\)(?: throws [\w.$, ]+)?;$', s)
        if m:
            name = m.group(1)
            if name == cls.split('.')[-1]:
                name = '<init>'
            pending = (name if name != '<init>' else None, 'method')
            if pending[0] is None:
                pending = None
            continue
        m = re.search(r'([\w$]+);$', s)
        if m:
            pending = (m.group(1), 'static-field' if ' static ' in ' ' + s else 'field')
    for r in sorted(rows):
        print('\t'.join(r))


main()
