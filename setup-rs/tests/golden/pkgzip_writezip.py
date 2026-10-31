"""android_data._write_zip (then verify_zip) of the entries a spec lists, for pkgzip.sh's writezip
part: one entry a line, `<name>\tfile:<path>` or `<name>\thex:<bytes in hex>`.

    python pkgzip_writezip.py [--verify] <spec> <out.zip>
"""
import sys

from pmunity import android_data as AD


def entries(spec):
    out = []
    for line in open(spec, encoding='utf-8').read().split('\n'):
        if not line:
            continue
        name, src = line.split('\t', 1)
        kind, _, v = src.partition(':')
        out.append((name, bytes.fromhex(v) if kind == 'hex' else v))
    return out


if __name__ == '__main__':
    a = sys.argv[1:]
    verify_only = a[0] == '--verify'
    spec, out = a[-2:]
    e = entries(spec)
    if not verify_only:
        AD._write_zip(out, e, print)
    AD.verify_zip(out, e, print)
