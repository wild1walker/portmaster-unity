#!/bin/bash
# convert.sh <scratch dir> [mode...]: the golden test of `pmu convert` on Cuphead's data, with the
# setup's own arguments (portmaster-cuphead's patchscript, fast quality):
#
#   convert <data> --config cuphead.json --plugin animclips.py
#           --set textures.quality=fast --set textures.guide=astc_guide_fast.bin
#
# Runs in the build image, after `cargo build --release --target aarch64-unknown-linux-musl` (or
# PMU=<a pmu binary>):  tools/rs.sh <worktree> bash tests/golden/convert.sh <scratch dir> [modes]
#
# Reads $PMU_TEST_DATA (read-only): Cuphead.app (its Contents/Resources/Data is copied to the
# scratch folder for each mode), cuphead/{cuphead.json,animclips.py}, guides/astc_guide_fast.bin,
# and converted-fast (the Python's output of that command: the reference). Modes:
#
#   rs     pmu alone; its tree must be converted-fast's
#   rs-py  pmu, then the Python runs the same command on the same folder (a setup that falls back
#          to the Python where pmu stops, status 2: the steps pmu has not ported yet); the tree
#          must be converted-fast's
#   py-rs  the Python with --steps shaders,settings,textures, then pmu runs the whole command
#          (it continues the Python's journal: audio, animclips); the tree must be converted-fast's
#   both   the Python and pmu, each on its own copy (twice the data's size free): logs (timings
#          and the copies' paths apart), statuses and trees compared with each other
#
# Default: rs-py py-rs. Trees are compared file by file (convert_compare.py: bytes, empty folders,
# the journal with audio.switch in any order); logs and trees are kept under <scratch>/<mode> until
# the next run of that mode, the data copies are removed. Exits 1 when a mode differs.
set -u
S=$(mkdir -p "$1" && cd "$1" && pwd)
shift
MODES=${*:-rs-py py-rs}
HERE=$(cd "$(dirname "$0")" && pwd)
WT=$(cd "$HERE/../../.." && pwd)
PMU=${PMU:-${CARGO_TARGET_DIR:-$WT/setup-rs/target}/aarch64-unknown-linux-musl/release/pmu}
T=${PMU_TEST_DATA:?set PMU_TEST_DATA}
DATA=$T/Cuphead.app/Contents/Resources/Data
REF=$T/converted-fast
export PMU_TPK=${PMU_TPK:-/opt/pylibs/UnityPy/resources/lzma.tpk}
ARGS=(--config "$T/cuphead/cuphead.json" --plugin "$T/cuphead/animclips.py"
      --set textures.quality=fast --set "textures.guide=$T/guides/astc_guide_fast.bin")
PY() { PYTHONHOME=/opt/py LD_LIBRARY_PATH=/opt/py/libs PYTHONPATH=$WT/convert:/opt/pylibs /opt/py/bin/python3 -P "$@"; }
CMP() { PY "$HERE/convert_compare.py" "$@"; }
[ -x "$PMU" ] || { echo "no pmu at $PMU (build it, or set PMU)" >&2; exit 2; }
[ -d "$DATA" ] && [ -d "$REF" ] || { echo "no Cuphead data or converted-fast under \$PMU_TEST_DATA" >&2; exit 2; }
fail=0

# run <dir> <py|rs> <log name> <args...>: one convert run on <dir>/Data (from <dir>), its output in
# <dir>/<log name>.{out,err,status}
run() {
  local d=$1 impl=$2 log=$3 t0=$SECONDS
  shift 3
  if [ "$impl" = py ]; then
    (cd "$d" && PY -m pmunity convert Data "$@" > "$log.out" 2> "$log.err"; echo $? > "$log.status")
  else
    (cd "$d" && "$PMU" convert Data "$@" > "$log.out" 2> "$log.err"; echo $? > "$log.status")
  fi
  echo "  $log: $impl exit $(cat "$d/$log.status") in $((SECONDS - t0)) s"
}

fresh() {  # fresh <dir>: <dir>/Data a new copy of the game's data
  rm -rf "$1"
  mkdir -p "$1"
  cp -R "$DATA" "$1/Data"
}

result() {  # result <mode> <status>
  if [ "$2" = 0 ]; then echo "ok   $1"; else echo "FAIL $1"; fail=$((fail + 1)); fi
}

for mode in $MODES; do
  d=$S/$mode
  echo "== $mode"
  case $mode in
    rs)
      fresh "$d"
      run "$d" rs rs "${ARGS[@]}"
      CMP --tree "$REF" "$d/Data" > "$d/compare.txt"; r=$?
      [ "$(cat "$d/rs.status")" = 0 ] || r=1
      ;;
    rs-py)
      fresh "$d"
      run "$d" rs rs "${ARGS[@]}"
      run "$d" py py "${ARGS[@]}"
      CMP --tree "$REF" "$d/Data" > "$d/compare.txt"; r=$?
      [ "$(cat "$d/py.status")" = 0 ] || r=1
      ;;
    py-rs)
      fresh "$d"
      run "$d" py py "${ARGS[@]}" --steps shaders,settings,textures
      run "$d" rs rs "${ARGS[@]}"
      CMP --tree "$REF" "$d/Data" > "$d/compare.txt"; r=$?
      [ "$(cat "$d/rs.status")" = 0 ] || r=1
      ;;
    both)
      fresh "$d/py"
      fresh "$d/rs"
      for impl in py rs; do
        run "$d/$impl" "$impl" run "${ARGS[@]}"
        mv "$d/$impl/run.out" "$d/$impl/out.txt"; mv "$d/$impl/run.err" "$d/$impl/err.txt"; mv "$d/$impl/run.status" "$d/$impl/status"
      done
      CMP "$d/py" "$d/rs" > "$d/compare.txt"; r=$?
      ;;
    *) echo "unknown mode $mode (rs, rs-py, py-rs, both)"; r=1 ;;
  esac
  head -n 40 "$d/compare.txt" 2>/dev/null
  find "$d" -name Data -prune -exec rm -rf {} +
  result "$mode" "$r"
done
echo "$fail failed"
[ "$fail" = 0 ]
