#!/usr/bin/env python3
"""Turn strrefs.py output (per-function string refs of libunity) into the JNI member lists.

usage: python3 -I jnibindings.py unity_strrefs_all.txt OUT_bindings.tsv OUT_direct.txt

OUT_bindings.tsv: generated C++ bindings (namespaces android:: java:: com:: org:: bitter::
  dalvik::) -> Java class, member name, JNI signature, kind (method/static-field/ctor...)
OUT_direct.txt: other functions that reference JNI class names or signatures directly.
"""
import sys, re

src, out_b, out_d = sys.argv[1:4]
blocks = []
cur = None
for line in open(src):
    if line.startswith('FUNC '):
        parts = line.rstrip('\n').split(None, 3)
        cur = (parts[1], parts[3], [])
        blocks.append(cur)
    elif cur is not None and line.startswith('    '):
        s = line.strip()
        try:
            cur[2].append(eval(s, {'__builtins__': {}}))
        except Exception:
            pass

SIG = re.compile(r'^(\((\[*([ZBCSIJFD]|L[\w/$]+;))*\)(V|\[*([ZBCSIJFD]|L[\w/$]+;))|\[*([ZBCSIJFD]|L[\w/$]+;))$')
CLS = re.compile(r'^(android|java|javax|dalvik|com|org|bitter)/[\w/$]+$')
NS = re.compile(r'^(?:\w+ )*((android|java|javax|dalvik|com|org|bitter)::[\w:]+)::(\w+)\(')

rows = []
direct = []
for addr, name, strs in blocks:
    m = NS.match(name)
    if m:
        cpp_cls, member = m.group(1), m.group(3)
        jcls = cpp_cls.replace('::', '/')
        # nested classes: generated as Outer_Inner
        parts = jcls.split('/')
        last = parts[-1]
        if '_' in last and last[0].isupper():
            last = last.replace('_', '$')
        jcls = '/'.join(parts[:-1] + [last])
        sigs = [s for s in strs if SIG.match(s)]
        names = [s for s in strs if not SIG.match(s) and not CLS.match(s)]
        if not sigs:
            continue
        kind = 'method'
        if member.startswith('f') and len(member) > 1 and (member[1].isupper() or member[1] == '_'):
            if not sigs[0].startswith('('):
                kind = 'static-field' if re.match(r'^[A-Z0-9_]+$', member[1:]) else 'field'
        if member == '__Constructor':
            kind = 'ctor'
        for i, sg in enumerate(sigs):
            nm = names[i] if i < len(names) else ('<init>' if kind == 'ctor' else '?')
            if kind == 'ctor':
                nm = '<init>'
            if not sg.startswith('(') and kind == 'method':
                kind = 'field'
            rows.append((jcls, nm, sg, kind, '%s::%s' % (cpp_cls, member), addr))
        continue
    if any(SIG.match(s) and s.startswith('(') for s in strs) or any(CLS.match(s) for s in strs):
        jni_strs = [s for s in strs if SIG.match(s) or CLS.match(s) or re.match(r'^[a-z<][\w<>]*$', s)]
        direct.append((addr, name, jni_strs))

with open(out_b, 'w') as o:
    o.write('# java_class\tmember\tsignature\tkind\tcpp_binding\taddr\n')
    for r in sorted(set(rows)):
        o.write('\t'.join(r) + '\n')
with open(out_d, 'w') as o:
    for addr, name, strs in direct:
        o.write('%s %s\n' % (addr, name))
        o.write('    ' + ' | '.join(strs) + '\n')
print(len(set(rows)), 'binding members;', len(set(r[0] for r in rows)), 'classes;', len(direct), 'direct users')
