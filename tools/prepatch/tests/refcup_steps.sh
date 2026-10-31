#!/bin/bash
# Reference for the page's settings / audio / animclips steps: pmunity's own (the release's setup, PC venv) on
# the game data of a page-built zip (textures already converted there), in overlay mode: the files those
# steps write land in $REF/out, with their journal (.pmunity.json).
#   tests/refcup_steps.sh <page-built zip> [steps]
set -e
ZIP=${1:?zip}; STEPS=${2:-settings,audio,animclips}
R=${REF:-$PWD/ref-cupsteps}; VENV=${PMU_PY:?a Python with the setup's package versions (setup/pylibs, for the PC)}
rm -rf "$R"; mkdir -p "$R"
"$VENV" - "$ZIP" "$R" <<'EOF'
import sys, zipfile, os
z = zipfile.ZipFile(sys.argv[1])
for i in z.infolist():
    n = i.filename
    if n.startswith('cuphead/gamedata/Cuphead_Data/') or (n.startswith('cuphead/setup/') and not n.startswith('cuphead/setup/pylibs/')):
        z.extract(i, sys.argv[2])
EOF
G="$R/cuphead"; S="$G/setup"; D="$G/gamedata/Cuphead_Data"
cd "$S"
/usr/bin/time -v env PYTHONPATH="$S" "$VENV" -m pmunity convert "$D" "$R/out" --config "$S/cuphead.json" \
  --plugin "$S/animclips.py" --steps "$STEPS" > "$R/run.log" 2>&1 || { echo "convert failed: $R/run.log"; tail -20 "$R/run.log"; exit 1; }
grep -E "settings:|audio:|animclips:|Elapsed|Maximum resident" "$R/run.log"
ls -la "$R/out"
