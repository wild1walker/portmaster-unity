#!/bin/bash
# Reference for the page's settings / audio / animclips steps: pmu's own (`pmu convert`) on
# the game data of a page-built zip (textures already converted there), in overlay mode: the files those
# steps write land in $REF/out, with their journal (.pmunity.json).
#   tests/refcup_steps.sh <page-built zip> [steps]
set -e
ZIP=${1:?zip}; STEPS=${2:-settings,audio,animclips}
R=${REF:-$PWD/ref-cupsteps}; PMU=${PMU:?a pmu for this machine: setup-rs/tools/build_pmu.sh <dir> builds the aarch64 Linux one}
rm -rf "$R"; mkdir -p "$R"
# (the game's data and the setup, as the handheld has them: unzip keeps the modes)
unzip -qo "$ZIP" 'cuphead/gamedata/*' 'cuphead/setup/*' -x 'cuphead/setup/pylibs/*' -d "$R"
G="$R/cuphead"; S="$G/setup"; D="$G/gamedata/Cuphead_Data"
cd "$S"
/usr/bin/time -v "$PMU" convert "$D" "$R/out" --config "$S/cuphead.json" \
  --plugin animclips.py --steps "$STEPS" > "$R/run.log" 2>&1 || { echo "convert failed: $R/run.log"; tail -20 "$R/run.log"; exit 1; }
grep -E "settings:|audio:|animclips:|Elapsed|Maximum resident" "$R/run.log"
ls -la "$R/out"
