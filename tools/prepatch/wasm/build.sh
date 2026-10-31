#!/bin/bash
# build.sh [out.wasm]: pmucore.wasm, the pixel work of prepatch.html's texture conversion, from the
# same sources pmunity's textures step runs on the handheld:
#   Pillow 12.3.0       libImaging/Resample.c, BcnDecode.c (unchanged; shim/Imaging.h stands in for Pillow's)
#   astc_encoder_py 0.1.12   its astc-encoder sources (ARM, Apache-2.0), no SIMD ISA (portable)
#   texture2ddecoder 1.0.6   the crunch unpackers (MIT; crunch: zlib)
#   portmaster-unity    convert/native/guided_astc/guided_astc.c (MIT), its core cut from the Python wrappe
#   lz4 4.4.5 (python-lz4)   its bundled lz4 library: LZ4HC level 9 for bundles as UnityPy writes them (BSD-2)
# Needs wasi-sdk (WASI_SDK, default ~/wasi-sdk) and python3; PMUNITY = a portmaster-unity checkout.
# The sources go to SRC (default ~/.cache/pmucore-src), fetched from PyPI once.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/pmucore.wasm}
SDK=${WASI_SDK:-$HOME/wasi-sdk}
SRC=${SRC:-$HOME/.cache/pmucore-src}
PMU=${PMUNITY:?set PMUNITY to a portmaster-unity checkout}
mkdir -p "$SRC"
fetch() {   # fetch <pypi name> <version> <unpacked folder>
  [ -d "$SRC/$3" ] && return
  python3 -I - "$1" "$2" "$SRC" <<'PY'
import json, sys, tarfile, urllib.request, io
name, ver, dst = sys.argv[1:]
d = json.load(urllib.request.urlopen(f'https://pypi.org/pypi/{name}/{ver}/json'))
u = next(x for x in d['urls'] if x['packagetype'] == 'sdist')
data = urllib.request.urlopen(u['url']).read()
import hashlib
assert hashlib.sha256(data).hexdigest() == u['digests']['sha256'], 'checksum'
tarfile.open(fileobj=io.BytesIO(data)).extractall(dst, filter='data')
PY
}
fetch pillow 12.3.0 pillow-12.3.0
fetch astc-encoder-py 0.1.12 astc_encoder_py-0.1.12
fetch texture2ddecoder 1.0.6 texture2ddecoder-1.0.6
fetch lz4 4.4.5 lz4-4.4.5

B=$(mktemp -d); trap 'rm -rf "$B"' EXIT
# guided_astc's core: everything before the Python wrapper, without Python's and pthreads' headers
G="$PMU/convert/native/guided_astc/guided_astc.c"
awk '/^static int get_buf/{exit} {print}' "$G" | grep -vE '^#define (Py_LIMITED_API|PY_SSIZE_T_CLEAN)|^#include <(Python|pthread)\.h>' > "$B/guided_core.inc"

CC="$SDK/bin/clang --target=wasm32-wasip1 --sysroot=$SDK/share/wasi-sysroot"
CXX="$SDK/bin/clang++ --target=wasm32-wasip1 --sysroot=$SDK/share/wasi-sysroot"
OPT="-O3 -msimd128 -mbulk-memory -mnontrapping-fptoint -DNDEBUG"
PIL="$SRC/pillow-12.3.0/src/libImaging"
A="$SRC/astc_encoder_py-0.1.12/src/astc-encoder/Source"
T="$SRC/texture2ddecoder-1.0.6/src/Texture2DDecoder"
objs=()
# Pillow's files beside the stand-in headers (they include "Imaging.h" from their own folder)
cp "$PIL/Resample.c" "$PIL/BcnDecode.c" "$HERE"/shim/*.h "$B/"
cc1() { $CC $OPT -I"$B" -c "$1" -o "$2"; objs+=("$2"); }
cc1 "$HERE/src/resize.c" "$B/resize.o"
cc1 "$HERE/src/decode.c" "$B/decode.o"
cc1 "$HERE/src/guided.c" "$B/guided.o"
L4="$SRC/lz4-4.4.5/lz4libs"
for f in lz4 lz4hc; do $CC $OPT -I"$L4" -c "$L4/$f.c" -o "$B/$f.o"; objs+=("$B/$f.o"); done
$CC $OPT -I"$L4" -c "$HERE/src/lz4glue.c" -o "$B/lz4glue.o"; objs+=("$B/lz4glue.o")
# SIMD=1 (default): astc-encoder's own SSE 4.1 vector layer on WebAssembly SIMD (shim/sse/immintrin.h: SSE's
# semantics); its invariance mode is on, so every vector layer gives the same blocks. SIMD=0: the portable one.
if [ "${SIMD:-1}" = 1 ]; then
  ASTC_DEFS="-DASTCENC_SSE=41 -DASTCENC_AVX=0 -DASTCENC_POPCNT=0 -DASTCENC_F16C=0 -DASTCENC_NEON=0 -DASTCENC_SVE=0 -I$HERE/shim/sse"
else
  ASTC_DEFS="-DASTCENC_SSE=0 -DASTCENC_AVX=0 -DASTCENC_POPCNT=0 -DASTCENC_F16C=0 -DASTCENC_NEON=0 -DASTCENC_SVE=0"
fi
for f in averages_and_directions block_sizes color_quantize color_unquantize compress_symbolic compute_variance \
         decompress_symbolic diagnostic_trace entry find_best_partitioning ideal_endpoints_and_weights image \
         integer_sequence mathlib mathlib_softfloat partition_tables percentile_tables pick_best_endpoint_format \
         quantization symbolic_physical weight_align weight_quant_xfer_tables; do
  $CXX $OPT -std=c++14 -fno-exceptions -fno-rtti $ASTC_DEFS -include "$HERE/shim/nothreads.h" -I"$A" \
    -c "$A/astcenc_$f.cpp" -o "$B/astc_$f.o"
  objs+=("$B/astc_$f.o")
done
$CXX $OPT -std=c++14 -fno-exceptions -fno-rtti $ASTC_DEFS -I"$A" -c "$HERE/src/astc.cpp" -o "$B/astc_glue.o"; objs+=("$B/astc_glue.o")
for f in crunch unitycrunch; do
  $CXX $OPT -std=c++14 -fno-exceptions -fno-rtti -w -I"$T" -c "$T/$f.cpp" -o "$B/t2d_$f.o"; objs+=("$B/t2d_$f.o")
done
$CXX $OPT -std=c++14 -fno-exceptions -fno-rtti -I"$T" -c "$HERE/src/crunch.cpp" -o "$B/crunch_glue.o"; objs+=("$B/crunch_glue.o")

$CXX $OPT -mexec-model=reactor -fno-exceptions "${objs[@]}" -o "$OUT" \
  -Wl,--export=malloc,--export=free -Wl,--max-memory=4294967296 -Wl,--strip-all
echo "built $OUT ($(stat -c %s "$OUT") bytes)"
