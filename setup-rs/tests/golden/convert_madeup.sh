#!/bin/sh
# convert_madeup.sh <scratch dir>: `pmu convert` against `python -m pmunity convert` on made-up
# data (crates/convert/tests/madeup: no game files), in the build image (tools/rs.sh <worktree>
# sh tests/golden/convert_madeup.sh <dir>). Each case runs both on copies of the same data folder
# with the same arguments (a case's prep runs first on both copies, by the implementation it
# names) and convert_compare.py compares their exit statuses, output and trees, byte for byte.
# Cuphead's plugin: $ANIMCLIPS_PY, else $PMU_TEST_DATA/cuphead/animclips.py (portmaster-cuphead's
# convert/animclips.py). CASES="a b" runs only those.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
wt=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
anim=${ANIMCLIPS_PY:-${PMU_TEST_DATA:-}/cuphead/animclips.py}
[ -f "$anim" ] || { echo "no animclips.py at $anim (set ANIMCLIPS_PY)" >&2; exit 2; }
export PMU_TPK="${PMU_TPK:-/opt/pylibs/UnityPy/resources/lzma.tpk}"
(cd "$wt/setup-rs" && CARGO_INCREMENTAL=0 cargo build --release -q -p pmu && CARGO_INCREMENTAL=0 cargo build --release -q -p convert --example madeup)
tgt=${CARGO_TARGET_DIR:-$wt/setup-rs/target}/release
src=$out/src
rm -rf "$src"
"$tgt/examples/madeup" "$src"
cp "$anim" "$src/animclips.py"
echo '{"steps": ["shaders", "settings", "audio"]}' > "$src/defaults.json"

impl() {  # impl py|rs <args...>: one convert run (from the run folder)
  side=$1; shift
  if [ "$side" = py ]; then
    PYTHONHOME=/opt/py LD_LIBRARY_PATH=/opt/py/libs PYTHONPATH="$wt/convert:/opt/pylibs" /opt/py/bin/python3 -m pmunity convert "$@"
  else
    "$tgt/pmu" convert "$@"
  fi
}

C="--config $src/cuphead.json --plugin $src/animclips.py --set textures.quality=fast --set textures.guide=$src/astc_guide_fast.bin"
STEPS="--steps shaders,settings,audio,animclips"
fails=0
# case <name> <prep: -, py, rs or both> <prep args | -> <args>: <args> name the data folder Data
case_() {
  name=$1 prep=$2 pargs=$3; shift 3
  if [ -n "${CASES:-}" ] && ! echo " $CASES " | grep -q " $name "; then return 0; fi
  for side in py rs; do
    d=$out/$name/$side
    rm -rf "$d"; mkdir -p "$d"
    cp -R "$src/Data" "$d/Data"
    case $prep in
      -) ;;
      both) (cd "$d" && impl "$side" Data $pargs > prep.txt 2>&1 || true) ;;
      *) (cd "$d" && impl "$prep" Data $pargs > prep.txt 2>&1 || true) ;;
    esac
    (cd "$d" && { impl "$side" "$@" > out.txt 2> err.txt; echo $? > status; } || true)
  done
  folders=Data; [ -d "$out/$name/py/Out" ] && folders="Data Out"
  if PYTHONHOME=/opt/py LD_LIBRARY_PATH=/opt/py/libs /opt/py/bin/python3 "$here/convert_compare.py" "$out/$name/py" "$out/$name/rs" $folders; then
    echo "ok   $name (exit $(cat "$out/$name/py/status"))"
  else
    echo "FAIL $name"; fails=$((fails + 1))
  fi
}

case_ cuphead - - Data $C $STEPS
case_ again both "$C $STEPS" Data $C $STEPS
case_ resumed both "$C --steps shaders" Data $C $STEPS
case_ from_python py "$C --steps shaders,settings" Data $C $STEPS
case_ from_pmu rs "$C --steps shaders,settings,audio" Data $C $STEPS
case_ budget_none - - Data $C $STEPS --set audio.pcm_budget_mb=null --set audio.compressed_min_s=10
case_ budget_float - - Data $C $STEPS --set audio.pcm_budget_mb=4.5 --set audio.resident_scenes=1
case_ audio_cfg both "$C --steps audio" Data $C --steps audio --set audio.compressed_min_s=3
case_ options - - Data $C $STEPS --set shaders.precision=highp --set shaders.glcore4x=true --set settings.audio_sample_rate=24000 --set settings.real_voices=16 --set settings.graphics_apis=null
case_ defaults - - Data --config "$src/defaults.json"
case_ overlay - - Data Out $C $STEPS
case_ unknown_step - - Data $C --steps settings,nope
case_ no_data - - Missing $C
echo "$fails failed"
[ "$fails" = 0 ]
