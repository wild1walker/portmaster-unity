#!/bin/sh
# convert_madeup.sh <scratch dir>: `pmu convert` against `python -m pmunity convert` on made-up
# data (crates/convert/tests/madeup: no game files), in the build image (tools/rs.sh <worktree>
# sh tests/golden/convert_madeup.sh <dir>). Each case runs both on copies of the same data folder
# with the same arguments (a case's prep runs first on both copies, by the implementation it
# names) and convert_compare.py compares their exit statuses, output and trees, byte for byte.
# Cuphead's plugin: $ANIMCLIPS_PY, else $PMU_TEST_DATA/cuphead/animclips.py (portmaster-cuphead's
# convert/animclips.py). CASES="a b" runs only those; PMU=<binary> tests that pmu (default: the
# native release build).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
wt=$(cd "$here/../../.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
anim=${ANIMCLIPS_PY:-${PMU_TEST_DATA:-}/cuphead/animclips.py}
[ -f "$anim" ] || { echo "no animclips.py at $anim (set ANIMCLIPS_PY)" >&2; exit 2; }
export PMU_TPK="${PMU_TPK:-/opt/pylibs/UnityPy/resources/lzma.tpk}"
(cd "$wt/setup-rs" && CARGO_INCREMENTAL=0 cargo build --release -q -p pmu && CARGO_INCREMENTAL=0 cargo build --release -q -p convert --example madeup)
tgt=${CARGO_TARGET_DIR:-$wt/setup-rs/target}/release
PMU=${PMU:-$tgt/pmu}      # (PMU=<the musl build> tests the binary a setup ships)
src=$out/src
rm -rf "$src"
"$tgt/examples/madeup" "$src"
cp "$anim" "$src/animclips.py"
echo '{"steps": ["shaders", "settings", "audio"]}' > "$src/defaults.json"
# an ASTC guide of the atlas bundle's textures, made by the Python (astc_guide.py, which needs numpy:
# fetched with pip into the scratch folder when the image has none) for Cuphead's settings
pyx() { PYTHONHOME=/opt/py LD_LIBRARY_PATH=/opt/py/libs PYTHONPATH="$wt/convert:/opt/pylibs:$out/numpy" /opt/py/bin/python3 "$@"; }
if ! pyx -c 'import numpy' 2>/dev/null; then
  # (PortMaster's Python has no ssl module for pip: the wheel comes with curl)
  whl=$(curl -fsSL https://pypi.org/pypi/numpy/2.2.6/json | pyx -c 'import json, sys
print(next(u["filename"] + " " + u["url"] for u in json.load(sys.stdin)["urls"]
           if "cp311-cp311-manylinux" in u["filename"] and "aarch64" in u["filename"]))' 2>/dev/null) &&
    curl -fsSL -o "$out/${whl%% *}" "${whl#* }" &&
    pyx -m pip install -q --no-index --target "$out/numpy" "$out/${whl%% *}" > /dev/null 2>&1 || true
fi
if pyx -m pmunity astc-guide "$src/Data" "$src/guide_fast.bin" --config "$src/cuphead.json" --set textures.quality=fast \
     --target fast --only atlas_main > "$src/guide.txt" 2>&1; then
  GUIDE=$src/guide_fast.bin
else
  echo "(no ASTC guide: textures_guide skipped)"; tail -n 3 "$src/guide.txt"; GUIDE=
fi

impl() {  # impl py|rs <args...>: one convert run (from the run folder)
  side=$1; shift
  if [ "$side" = py ]; then
    PYTHONHOME=/opt/py LD_LIBRARY_PATH=/opt/py/libs PYTHONPATH="$wt/convert:/opt/pylibs" /opt/py/bin/python3 -m pmunity convert "$@"
  else
    "$PMU" convert "$@"
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

case_ cuphead - - Data $C
case_ again both "$C" Data $C
case_ resumed both "$C --steps shaders" Data $C
case_ from_python py "$C --steps shaders,settings,textures" Data $C
case_ from_pmu rs "$C --steps shaders,settings,textures" Data $C
case_ textures_unscaled - - Data $C --steps textures --set textures.scale=1.0 --set textures.scale_sharp=1.0 --set textures.inline=true --set textures.inline_streams=false
case_ textures_search - - Data $C --steps textures --set textures.mode=search --set textures.quality=fastest --set textures.mips=keep
[ -n "$GUIDE" ] && case_ textures_guide - - Data $C --steps textures --set "textures.guide=$GUIDE"
case_ textures_only - - Data $C --steps textures --only atlas_
case_ budget_none - - Data $C $STEPS --set audio.pcm_budget_mb=null --set audio.compressed_min_s=10
case_ budget_float - - Data $C $STEPS --set audio.pcm_budget_mb=4.5 --set audio.resident_scenes=1
case_ audio_cfg both "$C --steps audio" Data $C --steps audio --set audio.compressed_min_s=3
case_ grab_fetch - - Data $C --steps shaders --set shaders.grab_fetch=always
case_ grab_fetch_true - - Data $C --steps shaders --set shaders.grab_fetch=true
case_ grab_fetch_names - - Data $C --steps shaders --set shaders.grab_fetch=always --set 'shaders.grab_fetch_names=Offset|Screen'
case_ options - - Data $C $STEPS --set shaders.precision=highp --set shaders.glcore4x=true --set settings.audio_sample_rate=24000 --set settings.real_voices=16 --set settings.graphics_apis=null
case_ defaults - - Data --config "$src/defaults.json"
case_ overlay - - Data Out $C
case_ unknown_step - - Data $C --steps settings,nope
case_ no_data - - Missing $C
echo "$fails failed"
[ "$fails" = 0 ]
