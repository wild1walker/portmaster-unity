#!/usr/bin/env python3
"""Compare the notes survey_player.sh wrote for two Unity Android players, and check the newer
player's imports against what the runtime provides (shimnames.py) and glibc.

usage: python3 -I playerdiff.py BASE_NOTES NEW_NOTES RUNTIME_DIR [--all-missing]
Prints a plain-text report: library facts, imports added/removed (with who would provide each:
a runtime shim, the host glibc, or nobody = the loader's abort stub), RegisterNatives tables,
the Java members libunity binds, the player's Java classes and natives, interesting strings.
"""
import sys, os, json, re, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shimnames  # noqa: E402


def load_facts(d):
    try:
        txt = open(os.path.join(d, 'facts.json')).read()
    except OSError:
        return {}
    out, dec, p = {}, json.JSONDecoder(), 0
    while p < len(txt):
        while p < len(txt) and txt[p].isspace():
            p += 1
        if p >= len(txt):
            break
        obj, p = dec.raw_decode(txt, p)
        for k, v in obj.items():
            out[k] = v
    return out


def load_imports(d):
    """{tag: {lib: {name: (prov, type, bind, flags)}}}"""
    out = {}
    base = os.path.join(d, 'imports')
    if not os.path.isdir(base):
        return out
    for tag in os.listdir(base):
        for fn in os.listdir(os.path.join(base, tag)):
            if not fn.startswith('imports_'):
                continue
            lib = fn[len('imports_'):-4]
            m = {}
            for line in open(os.path.join(base, tag, fn)):
                if line.startswith('#'):
                    continue
                f = line.rstrip('\n').split('\t')
                if len(f) >= 7:
                    m[f[1]] = (f[0], f[2], f[3], f[6])
            out.setdefault(tag, {})[lib] = m
    return out


def load_needed(d, tag):
    out = {}
    try:
        s = json.load(open(os.path.join(d, 'imports', tag, 'summary.json')))
    except (OSError, ValueError):
        # summary.json is several JSON objects in a row
        txt = open(os.path.join(d, 'imports', tag, 'summary.json')).read() if os.path.exists(
            os.path.join(d, 'imports', tag, 'summary.json')) else ''
        dec, p, s = json.JSONDecoder(), 0, {}
        while p < len(txt):
            while p < len(txt) and txt[p].isspace():
                p += 1
            if p >= len(txt):
                break
            o, p = dec.raw_decode(txt, p)
            s.update(o)
    return s


def natives(d):
    out = set()
    try:
        for line in open(os.path.join(d, 'jnitables.txt')):
            if line.startswith('  '):
                f = line.split()
                out.add((f[0], f[1]))
    except OSError:
        pass
    return out


def bindings(d):
    out = set()
    try:
        for line in open(os.path.join(d, 'bindings.tsv')):
            if line.startswith('#'):
                continue
            f = line.rstrip('\n').split('\t')
            if len(f) >= 4:
                out.add((f[0], f[1], f[2], f[3]))
    except OSError:
        pass
    return out


def javap(d):
    """{class: set(member lines)}"""
    out = {}
    cur = None
    try:
        for line in open(os.path.join(d, 'javap.txt')):
            line = line.rstrip()
            m = re.match(r'^(?:public |protected |private |final |abstract |static |synchronized |strictfp )*'
                         r'(?:class|interface|enum) ([\w.$]+)', line)
            if m:
                cur = m.group(1)
                out[cur] = set()
                continue
            if cur and line.startswith('  ') and line.strip() not in ('}',):
                out[cur].add(line.strip())
    except OSError:
        pass
    return out


def strings(d):
    try:
        return set(l.rstrip('\n') for l in open(os.path.join(d, 'strings.txt')))
    except OSError:
        return set()


def main():
    a, b, rt = sys.argv[1:4]
    all_missing = '--all-missing' in sys.argv
    prov = shimnames.provided(rt)
    mapped, shimlibs, denied = shimnames.libraries(rt)
    P = print

    fa, fb = load_facts(a), load_facts(b)
    P('== libraries (facts)')
    for path, f in sorted(fb.items()):
        P('%s: %s' % (path, ', '.join('%s=%s' % (k, f.get(k)) for k in
                                         ('needed', 'reloc_formats', 'load_align', 'android_ident', 'pt_tls',
                                          'ifunc_exports', 'gnu_property', 'emutls_symbols') if f.get(k))))
        bad_needed = [n for n in f.get('needed', []) if n not in mapped and n not in shimlibs
                      and not n.startswith(('libmain', 'libunity', 'libmono', 'libil2cpp'))]
        if bad_needed:
            P('   NEEDED without a mapping or shim: %s' % bad_needed)

    ia, ib = load_imports(a), load_imports(b)
    P('\n== imports')
    for tag in sorted(ib):
        # compare with the same variation of the base, else with its Mono ARMv7 one
        btag = tag if tag in ia else 'mono-armeabi-v7a'
        for lib in sorted(ib[tag]):
            new = ib[tag][lib]
            old = ia.get(btag, {}).get(lib, {})
            added = sorted(set(new) - set(old))
            removed = sorted(set(old) - set(new))
            cnt = collections.Counter()
            missing = []
            for n in new:
                p, t, bnd, fl = new[n]
                if n in prov:
                    cnt['shim'] += 1
                elif n.startswith(('mono_', 'il2cpp_')):
                    cnt['dep'] += 1           # from a sibling Android library (libmonoboehm = libmonobdwgc)
                elif 'NOT_IN_GLIBC' not in fl:
                    cnt['host'] += 1
                elif bnd == 'WEAK':
                    cnt['weak'] += 1          # unresolved weak: 0, as the libraries expect
                else:
                    cnt['MISSING'] += 1
                    missing.append(n)
            P('-- %s/%s (vs %s/%s): %d imports (%s); +%d -%d' % (
                tag, lib, btag, lib if old else '-', len(new), dict(cnt), len(added), len(removed)))
            for n in added:
                p, t, bnd, fl = new[n]
                where = 'shim:' + prov[n] if n in prov else ('dep' if n.startswith(('mono_', 'il2cpp_')) else
                                                             'host' if 'NOT_IN_GLIBC' not in fl else
                                                             'weak' if bnd == 'WEAK' else 'MISSING')
                P('   + %-40s %-12s %-6s %-5s %s %s' % (n, p, t, bnd, where,
                                                       '' if fl == '-' else fl[:90]))
            if removed:
                P('   - ' + ' '.join(removed))
            if all_missing and missing:
                P('   all MISSING: ' + ' '.join(sorted(missing)))

    na, nb = natives(a), natives(b)
    P('\n== RegisterNatives tables: +%d -%d (of %d)' % (len(nb - na), len(na - nb), len(nb)))
    for n in sorted(nb - na):
        P('   + %s %s' % n)
    for n in sorted(na - nb):
        P('   - %s %s' % n)

    # natives runtime/jni calls by name and signature
    used = set()
    jdir = os.path.join(rt, 'jni')
    for fn in sorted(os.listdir(jdir)):
        if fn.endswith('.c'):
            src = open(os.path.join(jdir, fn)).read()
            for m in re.finditer(r'(?:up_native|have_native|fj_call_native(?:_a)?\([^,]*,)\s*\(?\s*"(\w+)",\s*"([^"]+)"', src):
                used.add((m.group(1), m.group(2)))
    names_b = {}
    for n, g in nb:
        names_b.setdefault(n, set()).add(g)
    P('   natives runtime/jni calls: %d' % len(used))
    for n, g in sorted(used):
        if (n, g) not in nb:
            P('   ! %s %s: %s' % (n, g, ('signature now ' + ' / '.join(sorted(names_b[n]))) if n in names_b
                                 else 'not registered by this libunity/libmain'))

    ba, bb = bindings(a), bindings(b)
    P('\n== Java members bound by libunity: %d (+%d -%d); classes %d' % (
        len(bb), len(bb - ba), len(ba - bb), len(set(x[0] for x in bb))))
    ca = set(x[0] for x in ba)
    for x in sorted(bb - ba):
        P('   + %s%s %s %s %s' % ('[new class] ' if x[0] not in ca else '', x[0], x[1], x[2], x[3]))
    for x in sorted(ba - bb):
        P('   - %s %s %s %s' % x)

    ja, jb = javap(a), javap(b)
    P('\n== Java side (classes.jar): %d classes (+%d -%d)' % (len(jb), len(set(jb) - set(ja)), len(set(ja) - set(jb))))
    for c in sorted(set(jb) - set(ja)):
        nat = [m for m in jb[c] if ' native ' in ' ' + m]
        if re.search(r'\$\d+$', c) and not nat:
            continue
        P('   + class %s%s' % (c, ('  natives: ' + ' | '.join(nat)) if nat else ''))
    for c in sorted(set(ja) - set(jb)):
        if not re.search(r'\$\d+$', c):
            P('   - class %s' % c)
    for c in sorted(set(ja) & set(jb)):
        add, rem = jb[c] - ja[c], ja[c] - jb[c]
        anon = re.search(r'\$\d+$', c)            # renumbered anonymous classes: natives only
        if (not anon and c.startswith('com.unity3d.player.')) or any(' native ' in ' ' + m for m in add | rem):
            if add or rem:
                P('   ~ %s' % c)
                for m in sorted(add):
                    P('       + ' + m)
                for m in sorted(rem):
                    P('       - ' + m)
    allnat = sorted('%s: %s' % (c, m) for c in jb for m in jb[c] if ' native ' in ' ' + m)
    P('   natives declared in Java: %d' % len(allnat))

    sa, sb = strings(a), strings(b)
    P('\n== strings: +%d -%d' % (len(sb - sa), len(sa - sb)))
    for s in sorted(sb - sa):
        P('   + ' + s[:160])
    for s in sorted(sa - sb):
        P('   - ' + s[:160])


main()
