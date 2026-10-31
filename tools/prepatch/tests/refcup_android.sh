#!/bin/bash
# Reference for the page's packing step: pmunity's android-data (the release's setup, PC venv) as the Cuphead
# patchscript runs it, on the game data of a page-built zip after pmunity's shaders step, without
# --consume-src (the input is kept: $REF/data, the converted data; output $REF/android)
#   tests/refcup_android.sh <page-built zip> <player dir (pm player donor output)>
set -e
ZIP=${1:?zip}; PLAYER=${2:?player dir}
R=${REF:-$PWD/ref-cupad}; VENV=${PMU_PY:?a Python with the setup's package versions (setup/pylibs, for the PC)}
rm -rf "$R"; mkdir -p "$R"
"$VENV" - "$ZIP" "$R" <<'EOF'
import sys, zipfile, os
z = zipfile.ZipFile(sys.argv[1])
for i in z.infolist():
    n = i.filename
    if n.startswith('cuphead/gamedata/') or (n.startswith('cuphead/setup/') and not n.startswith('cuphead/setup/pylibs/')):
        p = z.extract(i, sys.argv[2]); m = i.external_attr >> 16
        if m and not i.is_dir(): os.chmod(p, m & 0o777)
EOF
G="$R/cuphead"; S="$G/setup"; GD="$G/gamedata"; D="$GD/Cuphead_Data"
chmod +x "$S/codepatch"
cd "$S"
export PYTHONPATH="$S" QEMU_LD_PREFIX=${QEMU_LD_PREFIX:?an arm64 root file system for qemu}
# the shaders step in place (as the handheld: the page has done the other steps; the journal says so)
"$VENV" -m pmunity convert "$D" --config "$S/cuphead.json" --plugin "$S/animclips.py" > "$R/convert.log" 2>&1 \
  || { echo "convert failed"; tail "$R/convert.log"; exit 1; }
grep -E "^\[.*(shaders|textures|settings|audio|animclips):" "$R/convert.log" | cut -c1-200
rm -rf "$R/data"; cp -a "$D" "$R/data"         # (the packing input, kept for the page's run)
DLC=; [ -f "$GD/.setup/.dlc_files" ] && DLC=--steam-dlc
/usr/bin/time -v "$VENV" -m pmunity android-data --src "$D" --pkg "$PLAYER" --out "$R/android" --name Cuphead --package com.cupheadport.game \
  --root-file animclips.txt --drop-missing-assembly GalaxyCSharp.dll --managed-file "$S/CupheadPort.dll" --game-managed "$D/Managed" \
  --assembly-patch Assembly-CSharp.dll "env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 '$S/codepatch' --target=android $DLC {managed} {out}" \
  --mono-etc "$GD/mono-etc" --dllmap SDL2=libSDL2-2.0.so.0 --verify-bundles 2 --pack all --no-video-shader > "$R/android.log" 2>&1 \
  || { echo "android-data failed"; tail -20 "$R/android.log"; exit 1; }
grep -E "placed:|pack:|plugins.map|done in|Elapsed|Maximum resident" "$R/android.log"
