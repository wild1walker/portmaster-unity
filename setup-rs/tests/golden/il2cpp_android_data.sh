#!/bin/bash
# il2cpp_android_data.sh <data> <il2cpp out> <player> <scratch dir> [android-data options...]:
# android-data --il2cpp (the il2cpp-arm64 route's step 4) by the Python and by pmu (PMU=<pmu>), on
# fresh copies of the data at the same path, with the setup's arguments (--consume-src
# --verify-bundles 2 --pack all), and what each leaves compared: exit status, every file of the
# output (the APK, android_package.json, the report; the state file without its sources' times and
# the log are left out) and of the data folder --consume-src leaves. <il2cpp out>: an `il2cpp
# --on-device` output (il2cpp_device.sh with KEEP=1); <player>: the IL2CPP player package it used.
#
#   tools/rs.sh <worktree> bash tests/golden/il2cpp_android_data.sh <data> <out> <player> <scratch>
set -u
DATA=$1
IL=$2
PKG=$3
S=$(mkdir -p "$4" && cd "$4" && pwd)
shift 4
HERE=$(cd "$(dirname "$0")" && pwd)
WT=${WT:-$(cd "$HERE/../../.." && pwd)}   # (the worktree whose convert/ is the reference Python)
PMU=${PMU:-$CARGO_TARGET_DIR/aarch64-unknown-linux-musl/release/pmu}
export PMU_TPK=${PMU_TPK:-/opt/pylibs/UnityPy/resources/lzma.tpk}
ok=0
for impl in py rs; do
  rm -rf "$S/work" "$S/ad.$impl"
  mkdir -p "$S/work" "$S/ad.$impl"
  cp -a "$DATA" "$S/work/Game_Data"
  case $impl in
    py) cmd=(env PYTHONHOME=/opt/py LD_LIBRARY_PATH="/opt/py/libs${PY_SSL_LIB:+:$PY_SSL_LIB}" PYTHONPATH="$WT/convert:/opt/pylibs"
             PYTHONDONTWRITEBYTECODE=1 /opt/py/bin/python3 -B -m pmunity) ;;
    rs) cmd=("$PMU") ;;
  esac
  "${cmd[@]}" android-data --src "$S/work/Game_Data" --pkg "$PKG" --il2cpp "$IL" --out "$S/work/android" --name Game \
    --package com.pmunity.game --swappy off --consume-src --verify-bundles 2 --pack all "$@" \
    > "$S/ad.$impl/stdout" 2> "$S/ad.$impl/stderr"
  echo $? > "$S/ad.$impl/status"
  ( cd "$S/work" && find . -type f ! -name make_android_data.log ! -name .android_data_state.json | LC_ALL=C sort |
      while IFS= read -r f; do echo "$(sha256sum < "$f" | cut -c1-64) $f"; done ) > "$S/ad.$impl/manifest"
  # the state without the sources' times (they are the copies' own)
  sed -E 's/\[[0-9]+, [0-9]+, /[<size>, <mtime>, /g' "$S/work/android/.android_data_state.json" > "$S/ad.$impl/state" 2>/dev/null
  cp "$S/work/android/zip/android_package.json" "$S/ad.$impl/" 2>/dev/null
done
rm -rf "$S/work"
for f in status manifest state android_package.json; do
  if cmp -s "$S/ad.py/$f" "$S/ad.rs/$f"; then echo "$f: the same"; else echo "$f: DIFFERS"; diff "$S/ad.py/$f" "$S/ad.rs/$f" | head -20; ok=1; fi
done
diff <(grep -v '^[0-9:]* ' "$S/ad.py/stdout" | sed -E 's/[0-9.]+ s\b/<t> s/g') <(grep -v '^[0-9:]* ' "$S/ad.rs/stdout" | sed -E 's/[0-9.]+ s\b/<t> s/g') > /dev/null || echo "(stdout differs: see $S/ad.*/stdout)"
echo "android-data --il2cpp: status $(cat "$S/ad.py/status") / $(cat "$S/ad.rs/status"), $(wc -l < "$S/ad.rs/manifest") files"
exit $ok
