#!/bin/sh
# run.sh - the Mach-O route's tests, on an arm64 Linux with clang (the image cxx/build-cxx.sh
# describes; the instruction rewrite compares with the CPU, which must have LSE, RCPC, SHA3 and
# the dot product: Apple silicon under Docker does).
#   run.sh                 the rewrite's differential test
#   run.sh IMAGE.dylib     also: load a Mach-O image, list and rewrite what an ARMv8.0 core lacks
set -e
cd "$(dirname "$0")/.."
CC=${CC:-clang-19}
W="-O2 -g -Wall -Wextra -Wno-unused-parameter"
OUT=${OUT:-/tmp/ua-macho-test}
mkdir -p "$OUT"
$CC $W -pthread -o "$OUT/a64fix_test" test/a64fix_test.c test/run_state.S a64fix.c
"$OUT/a64fix_test"
[ -n "$1" ] || exit 0
$CC $W -o "$OUT/load_probe" test/load_probe.c macho.c
$CC $W -o "$OUT/a64fix_image" test/a64fix_image.c a64fix.c macho.c
"$OUT/load_probe" "$1"
"$OUT/a64fix_image" fix "$1"
# The libc++ built as Mach-O (cxx/build-cxx-macho.sh, on a Mac) holds nothing an ARMv8.0 core lacks.
for d in cxx/out/macos-arm64/libc++abi.1.dylib cxx/out/macos-arm64/libc++.1.dylib; do
  [ -f "$d" ] || continue
  n=$("$OUT/a64fix_image" list "$d" 2>/dev/null | wc -l)
  [ "$n" -eq 0 ] || { echo "$d: $n instructions beyond ARMv8.0" >&2; exit 1; }
  echo "$d: ARMv8.0 code only"
done
