#!/usr/bin/env python3
"""nsisrange.py list|extract ...: the NSIS reader of convert/pmunity/nsis.py (see its usage), which
the converter's `player` command uses to fetch Unity's Android player."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'convert'))
from pmunity.nsis import main  # noqa: E402

if __name__ == '__main__':
    main()
