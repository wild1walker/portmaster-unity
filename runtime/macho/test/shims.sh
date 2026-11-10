#!/bin/sh
# shims.sh - builds the loader with the Darwin shims and the harness (machorun), on the arm64
# Linux image run.sh names.
#   shims.sh                       build, into $OUT
#   shims.sh cover NAMES           every import name of the file has an answer
#   shims.sh run IMAGE.dylib...    load the images and call the last one's test_main
#   shims.sh il2cpp DATA_DIR IMAGE.dylib...   start a game's IL2CPP runtime (il2cpprun.c)
set -e
cd "$(dirname "$0")/.."
CC=${CC:-clang-19}
W="-O2 -g -Wall -Wextra -Wno-unused-parameter -ftls-model=initial-exec"
OUT=${OUT:-/tmp/ua-macho-test}
mkdir -p "$OUT"
SRC="macho.c a64fix.c dyld.c tlv.c tlv_thunk.S sh_asm.S sh_table.c sh_errno.c sh_mem.c sh_misc.c sh_signal.c sh_context.c
     sh_stdio.c sh_file.c sh_time.c sh_thread.c sh_locale.c sh_net.c sh_cf.c"
# -rdynamic: libunwind finds this program's own frames; the images' frames come through dyld.c
for prog in machorun il2cpprun; do
    $CC $W -pthread -rdynamic -o "$OUT/$prog" test/$prog.c $SRC cxx/out/arm64/libunwind.a -ldl -lm
done
case "$1" in
  cover) "$OUT/machorun" --cover "$2" ;;
  run)   shift; "$OUT/machorun" "$@" ;;
  il2cpp) shift; "$OUT/il2cpprun" "$@" ;;
esac
