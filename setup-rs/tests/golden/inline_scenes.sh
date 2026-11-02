#!/bin/sh
# inline_scenes.sh <scratch dir>: android-data's inlining of a bundle with several serialized
# files (a scene bundle: the scene and its .sharedAssets, textures streamed from .resS nodes)
# by pmu (crates/androiddata inline::inline_bundle_with, its example inline_bundle) against the
# Python's (android_data.py inline_bundle: UnityPy in memory), on made-up bundles
# (crates/convert/tests/madeup scene_bundles), in the build image:
#
#   tools/rs.sh <worktree> sh tests/golden/inline_scenes.sh <dir>
#
# Compares what each returns (textures inlined, the nodes dropped) and the bundle written, byte
# for byte.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
wt=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
export PMU_TPK="${PMU_TPK:-/opt/pylibs/UnityPy/resources/lzma.tpk}"
(cd "$wt/setup-rs" && CARGO_INCREMENTAL=0 cargo build --release -q -p convert --example madeup &&
  CARGO_INCREMENTAL=0 cargo build --release -q -p androiddata --example inline_bundle)
tgt=${CARGO_TARGET_DIR:-$wt/setup-rs/target}/release/examples
rm -rf "$out/scenes" "$out/py" "$out/rs"
"$tgt/madeup" "$out" --scenes
mkdir -p "$out/py" "$out/rs"
fails=0
for src in "$out"/scenes/*; do
  n=$(basename "$src")
  PYTHONHOME=/opt/py LD_LIBRARY_PATH=/opt/py/libs PYTHONPATH="$wt/convert:/opt/pylibs" /opt/py/bin/python3 -c '
import sys
from pmunity import android_data
print(android_data.inline_bundle(sys.argv[1], sys.argv[2]))' "$src" "$out/py/$n" > "$out/py/$n.txt"
  "$tgt/inline_bundle" "$src" "$out/rs/$n" > "$out/rs/$n.txt"
  if cmp -s "$out/py/$n" "$out/rs/$n" && cmp -s "$out/py/$n.txt" "$out/rs/$n.txt"; then
    echo "ok   $n: $(cat "$out/rs/$n.txt"), $(wc -c < "$out/rs/$n") bytes"
  else
    echo "FAIL $n: python $(cat "$out/py/$n.txt") $(wc -c < "$out/py/$n"), pmu $(cat "$out/rs/$n.txt") $(wc -c < "$out/rs/$n")"
    fails=$((fails + 1))
  fi
done
echo "$fails failed"
[ "$fails" = 0 ]
