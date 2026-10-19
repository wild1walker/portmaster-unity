#!/usr/bin/env python3
"""Names for libunity.so offsets: sym.py <hex offset>...  (symbols: see unitysyms.py)"""
import sys

from unitysyms import namer

name = namer(short=False)
for x in sys.argv[1:]:
    print(f'{x}: {name("libunity.so+0x" + x.lower().replace("0x", ""))}')
