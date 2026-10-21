#!/usr/bin/env python3
"""Names the runtime itself provides to Android libraries (before the host's glibc fallback):
the loader's bionic table (hybris_compat.c), the libhybris hooks it compiles in, the Android
shims (liblog, libandroid, OpenSL ES, libmediandk, EGL glue), the loader's own tables, and the
libraries the loader maps, serves with shims or refuses (so_loader.c).

usage: python3 -I shimnames.py RUNTIME_DIR          -> one "kind<TAB>name" per line
       import shimnames; shimnames.provided(RUNTIME_DIR) -> {name: kind}
"""
import os, re, sys


def _read(path):
    try:
        return open(path, encoding='utf-8', errors='replace').read()
    except OSError:
        return ''


def provided(rt):
    out = {}
    q = re.compile(r'\{\s*"([A-Za-z_][\w]*)"\s*,')
    for rel, kind in (('loader/hybris_compat.c', 'bionic'), ('loader/so_loader.c', 'loader'),
                      ('loader/softfp.c', 'loader'), ('android/egl_glue.c', 'egl')):
        for m in q.finditer(_read(os.path.join(rt, rel))):
            out.setdefault(m.group(1), kind)
    hooks = _read(os.path.join(rt, 'thirdparty/libhybris/hybris/common/hooks.c'))
    for m in re.finditer(r'HOOK_(?:DIRECT|DIRECT_NO_DEBUG|INDIRECT|TO)\(\s*(\w+)', hooks):
        out.setdefault(m.group(1), 'libhybris')
    tab = _read(os.path.join(rt, 'android/shim_tables.c'))
    for macro, kind in (('LOG_FUNCS', 'liblog'), ('NDK_FUNCS', 'libandroid'), ('NDK_NEWER_FUNCS', 'libandroid'),
                        ('SLES_FUNCS', 'opensles'),
                        ('SLES_IIDS', 'opensles'), ('MEDIANDK_FUNCS', 'libmediandk'),
                        ('MEDIANDK_KEYS', 'libmediandk')):
        m = re.search(r'#define %s\(X\)((?:.*\\\n)*.*)' % macro, tab)
        if m:
            for n in re.findall(r'X\((\w+)\)', m.group(1)):
                out.setdefault(n, kind)
    return out


def libraries(rt):
    """(mapped to host libs, served by shims, refused on dlopen) from so_loader.c."""
    src = _read(os.path.join(rt, 'loader/so_loader.c'))
    mapped = re.findall(r'\{\s*"(lib[\w.+-]+\.so[\w.]*)"\s*,\s*"', src)
    m = re.search(r'g_android_only\[\]\s*=\s*\{(.*?)\};', src, re.S)
    shims = re.findall(r'"([^"]+)"', m.group(1)) if m else []
    m = re.search(r'g_denied_prefix\[\]\s*=\s*\{(.*?)\};', src, re.S)
    denied = re.findall(r'"([^"]+)"', m.group(1)) if m else []
    return mapped, shims, denied


if __name__ == '__main__':
    for n, k in sorted(provided(sys.argv[1]).items()):
        print('%s\t%s' % (k, n))
    mp, sh, de = libraries(sys.argv[1])
    print('# libraries mapped to host: %s' % ' '.join(mp))
    print('# libraries served by shims: %s' % ' '.join(sh))
    print('# dlopen refused (prefix): %s' % ' '.join(de))
