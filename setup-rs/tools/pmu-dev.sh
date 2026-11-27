#!/bin/bash
# pmu-dev.sh <tool> [args...]: the developer's tools of crates/devtools (`pmu-dev`: profiling summaries,
# generators, the regression run, the player survey's ELF and JNI tools, prefs, ...; `pmu-dev.sh` alone
# lists them) on this computer, through pmu.sh: a static
# aarch64 build (tools/port/out/pmu-dev/pmu-dev, built here in the Rust image when missing or older
# than its sources) in an arm64 container that sees the paths in the arguments.
#   PMU_DEV_BIN   another linux-arm64 pmu-dev to run
#   UA_SYMS, UA_LIBUNITY_SYM, UA_STATE_RE, UA_LOADING_RE, PMU_DEV_CACHE (default tools/analysis: the
#                 symbol tables' cache) are passed to it
# pmu-dev.sh --build <out dir>: only build it (static aarch64 musl), e.g. for a handheld (stacks).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PM=$(cd "$HERE/../.." && pwd)
build() {   # build <out dir>
  local out; mkdir -p "$1"; out=$(cd "$1" && pwd)
  RS_ROOT=${RS_ROOT:-$out} RS_MOUNTS= RS_MEM=${RS_MEM:-3g} "$HERE/rs.sh" "$PM" bash -c '
    set -euo pipefail
    export CARGO_TARGET_DIR=$CARGO_TARGET_DIR-release   # (this checkout's own: build_pmu.sh)
    T=aarch64-unknown-linux-musl; mkdir -p /build; cp -a . /build/setup-rs; cd /build/setup-rs
    cargo build -q --locked --release --target $T -p devtools >&2
    cat $CARGO_TARGET_DIR/$T/release/pmu-dev' > "$out/pmu-dev.tmp"
  [ -s "$out/pmu-dev.tmp" ] || { rm -f "$out/pmu-dev.tmp"; echo "pmu-dev was not built" >&2; exit 1; }
  chmod 755 "$out/pmu-dev.tmp"; mv "$out/pmu-dev.tmp" "$out/pmu-dev"
}
if [ "${1:-}" = --build ]; then build "${2:?usage: pmu-dev.sh --build <out dir>}"; echo "$(cd "$2" && pwd)/pmu-dev"; exit 0; fi
if [ -n "${PMU_DEV_BIN:-}" ]; then
  BIN=$PMU_DEV_BIN
else
  BIN=$PM/tools/port/out/pmu-dev/pmu-dev
  if [ ! -x "$BIN" ] || [ -n "$(cd "$PM/setup-rs" && find Cargo.toml Cargo.lock crates vendor -newer "$BIN" -type f \
       -print -quit 2>/dev/null)" ]; then
    echo "== building pmu-dev (Docker)" >&2
    build "$(dirname "$BIN")" >&2
  fi
fi
export PMU_DEV_CACHE=${PMU_DEV_CACHE:-$PM/tools/analysis}
PMU_ENV="${PMU_ENV:-} UA_SYMS UA_LIBUNITY_SYM UA_STATE_RE UA_LOADING_RE PMU_DEV_CACHE" \
PMU_MOUNTS="${PMU_MOUNTS:-} ${UA_SYMS:-} ${UA_LIBUNITY_SYM:-} $PMU_DEV_CACHE" PMU_BIN=$BIN exec "$HERE/pmu.sh" "$@"
