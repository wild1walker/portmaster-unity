#!/bin/sh
# unityfs.sh <data dir> <out dir> [--only TEXT] [--no-write] [--no-patch] [--no-objects]: the golden
# test of crates/unityfs, in the build image (tools/rs.sh <worktree> sh tests/golden/unityfs.sh ...):
# the Rust side (examples/golden.rs) and the Python side (unityfs_golden.py: pmunity, UnityPy 1.25.4)
# read, rewrite, edit, inline, recompress and platform-patch every Unity file of the data folder,
# each into <out dir>/rs or <out dir>/py one file at a time, and unityfs_compare.py compares their
# lines (<out dir>/rs.jsonl, py.jsonl). The class database is UnityPy's ($PMU_TPK).
# RUST_ONLY=1 / PY_ONLY=1 run one side (the other's lines kept from before).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
wt=$(cd "$here/../../.." && pwd)
data=$1; out=$2; shift 2
mkdir -p "$out"
export PMU_TPK="${PMU_TPK:-/opt/pylibs/UnityPy/resources/lzma.tpk}"
py() { PYTHONHOME=/opt/py LD_LIBRARY_PATH=/opt/py/libs PYTHONPATH="$wt/convert:/opt/pylibs" /opt/py/bin/python3 "$@"; }
if [ -z "${PY_ONLY:-}" ]; then
  (cd "$wt/setup-rs" && CARGO_INCREMENTAL=0 cargo build --release -q -p unityfs --example golden)
  "${CARGO_TARGET_DIR:-$wt/setup-rs/target}/release/examples/golden" "$data" "$out/rs" "$@" > "$out/rs.jsonl"
  rm -rf "$out/rs"
fi
if [ -z "${RUST_ONLY:-}" ]; then
  py "$here/unityfs_golden.py" "$data" "$out/py" "$@" > "$out/py.jsonl"
  rm -rf "$out/py"
fi
py "$here/unityfs_compare.py" "$out/py.jsonl" "$out/rs.jsonl"
