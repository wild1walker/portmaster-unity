#!/bin/bash
# build_pmu.sh <out dir>: the setup's tools as a port's release ships them: <out>/pmu (static
# aarch64 musl, Unity's class database from UnityPy embedded) and <out>/licenses/LICENSE.*.txt, the
# licence texts of what is compiled into it (the crates from crates.io and Rust's standard library,
# musl, lz4, UnityPy's class database, the vendored C/C++ of setup-rs/vendor). Built in the Rust
# image (tools/rs.sh: ua-rust with GCC 10, setup-rs/docker/Dockerfile.gcc10, so the ASTC blocks are
# the Python's) from a copy of setup-rs at a neutral path (the binary names its source files).
# `pmunity port package` runs it (tools/port/out/pmu) for ports whose setup runs pmu.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PM=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: build_pmu.sh <out dir>}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
B=$(mktemp -d "${TMPDIR:-/tmp}/build-pmu.XXXXXX")
trap 'rm -rf "$B"' EXIT
RS_ROOT=${RS_ROOT:-$B} RS_MOUNTS= RS_MEM=${RS_MEM:-3g} "$HERE/rs.sh" "$PM" bash -c '
  set -euo pipefail
  export CARGO_TARGET_DIR=/target/pmu-release PMU_TPK=/opt/pylibs/UnityPy/resources/lzma.tpk
  T=aarch64-unknown-linux-musl; O=/tmp/out; mkdir -p $O /build; cp -a . /build/setup-rs; cd /build/setup-rs
  command -v "${TC_CXX:-none}" >/dev/null || { echo "ua-rust has no GCC 10 (TC_CXX; setup-rs/docker/Dockerfile.gcc10): the ASTC blocks would not all match" >&2; exit 1; }
  cargo build -q --release --target $T -p pmu >&2
  cp $CARGO_TARGET_DIR/$T/release/pmu $O/
  cat /opt/pylibs/unitypy-*.dist-info/licenses/LICENSE > $O/unitypy.txt
  cat /usr/share/doc/musl/copyright > $O/musl.txt
  cat $(ls -d /opt/cargo/registry/src/*/lz4-sys-1.9.4 | head -1)/liblz4/lib/LICENSE > $O/lz4.txt
  MIT=$(sed /\<year\>/d $(rustc --print sysroot)/share/doc/rust/licenses/MIT.txt)
  { echo "Rust standard library ($(rustc --version | cut -d" " -f2)): MIT or Apache-2.0, copyright the Rust project contributors"
    echo; echo "$MIT"
    cargo tree -q -p pmu --target $T -e normal,no-proc-macro --prefix none --no-dedupe --format "{p}|{l}" | grep -v "(/" | sort -u |
    while IFS="|" read -r p l; do
      n=${p% v*}; v=${p##* v}; d=$(ls -d /opt/cargo/registry/src/*/$n-$v | head -1)
      printf "\n\n%s %s (%s)\n\n" "$n" "$v" "$l"
      [ $n = lz4-sys ] && { printf "the lz4 library: LICENSE.lz4.txt. The crate (lz4-rs): Copyright (c) 2015 Artem V. Navrotskiy\n%s\n" "$MIT"; continue; }
      f=; case "$l" in *MIT*) f=$(ls "$d"/LICEN[CS]E-MIT* "$d"/LICENSES/MIT* 2>/dev/null || true) ;; esac   # (MIT where offered)
      [ -n "$f" ] || f=$(find "$d" -maxdepth 1 -type f \( -name "LICEN[CS]E*" -o -name "COPYING*" -o -name "UNLICENSE*" \) | sort)
      [ -n "$f" ] || { echo "$n $v: no licence file" >&2; exit 1; }
      cat $f
    done; } > $O/rust-crates.txt
  tar -c -C $O .' | tar -x -C "$B"
[ -s "$B/pmu" ] || { echo "pmu was not built" >&2; exit 1; }
L="$OUT/licenses"
rm -rf "$L"
mkdir -p "$L"
V="$PM/setup-rs/vendor"
{ echo "astc-encoder (Arm), compiled into setup/pmu"; echo; cat "$V/astc-encoder/LICENSE.txt"; } > "$L/LICENSE.astc-encoder.txt"
{ echo "Pillow's libImaging/Resample.c, compiled into setup/pmu (its BCn decoder, ported: public domain)"; echo
  cat "$V/pillow/LICENSE"; } > "$L/LICENSE.pillow.txt"
{ echo "texture2ddecoder's crunch unpackers, compiled into setup/pmu"; echo; cat "$V/texture2ddecoder/LICENSE"
  for f in "$V"/texture2ddecoder/src/Texture2DDecoder/{crunch,unitycrunch}/crn_decomp.h; do
    printf '\n\n%s\n\n' "${f#$V/texture2ddecoder/src/Texture2DDecoder/}"
    sed -n '/crn.* uses .*ZLIB license/,/This notice may not be removed/p' "$f" | sed 's#^ *// \{0,1\}##'
  done; } > "$L/LICENSE.texture2ddecoder.txt"
{ echo "optimized-routines (Arm): sinf, cosf, sincosf, compiled into setup/pmu"; echo; cat "$V/optimized-routines/LICENSE"; } > "$L/LICENSE.optimized-routines.txt"
{ echo "lz4 1.9.4 (the crate lz4-sys), compiled into setup/pmu"; echo; cat "$B/lz4.txt"; } > "$L/LICENSE.lz4.txt"
{ echo "UnityPy: its class id names and class database (resources/lzma.tpk), in setup/pmu"; echo; cat "$B/unitypy.txt"; } > "$L/LICENSE.unitypy.txt"
{ echo "musl libc, linked into setup/pmu"; echo; cat "$B/musl.txt"; } > "$L/LICENSE.musl.txt"
{ echo "The Rust crates (crates.io) and Rust's standard library compiled into setup/pmu"; echo; cat "$B/rust-crates.txt"; } > "$L/LICENSE.rust-crates.txt"
cp "$B/pmu" "$OUT/pmu"
chmod 755 "$OUT/pmu"
echo "$OUT/pmu ($(wc -c < "$OUT/pmu" | tr -d " ") bytes), licences in $L"
