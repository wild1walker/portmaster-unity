#!/bin/bash
# stacksample.sh <pid> <thread-name> <samples> [interval s]: repeated stacks.py snapshots of one thread
for i in $(seq "$3"); do python3 "$(dirname "$0")/stacks.py" "$1" "$2" 64 | grep -o 'libunity.so+0x[0-9a-f]*\|libGLES[^ ]*\|libmali.so+0x[0-9a-f]*\|libc.so.6+0x[0-9a-f]*\|syscall [0-9]*' | head -6 | tr '\n' ' '; echo; sleep "${4:-0.4}"; done
