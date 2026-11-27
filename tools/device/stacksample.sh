#!/bin/bash
# stacksample.sh <pid> <thread-name> <samples> [interval s]: repeated `pmu-dev stacks` snapshots of one thread,
# on the handheld (pmu-dev beside this script: setup-rs/tools/pmu-dev.sh --build <dir>, the static aarch64 build)
for i in $(seq "$3"); do "$(dirname "$0")/pmu-dev" stacks "$1" "$2" 64 | grep -o 'libunity.so+0x[0-9a-f]*\|libGLES[^ ]*\|libmali.so+0x[0-9a-f]*\|libc.so.6+0x[0-9a-f]*\|syscall [0-9]*' | head -6 | tr '\n' ' '; echo; sleep "${4:-0.4}"; done
