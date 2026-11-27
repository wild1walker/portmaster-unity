#!/bin/bash
# Reference for the page's packing step: `pmu android-data` as the Cuphead
# patchscript runs it, on the game data of a page-built zip after pmu's shaders step, without
# --consume-src (the input is kept: $REF/data, the converted data; output $REF/android)
#   tests/refcup_android.sh <page-built zip> <player dir (pm player donor output)>
set -e
ZIP=${1:?zip}; PLAYER=${2:?player dir}
R=${REF:-$PWD/ref-cupad}; PMU=${PMU:?a pmu for this machine: setup-rs/tools/build_pmu.sh <dir> builds the aarch64 Linux one}
rm -rf "$R"; mkdir -p "$R"
# (the game's data and the setup, as the handheld has them: unzip keeps the modes)
unzip -qo "$ZIP" 'cuphead/gamedata/*' 'cuphead/setup/*' -x 'cuphead/setup/pylibs/*' -d "$R"
G="$R/cuphead"; S="$G/setup"; GD="$G/gamedata"; D="$GD/Cuphead_Data"
chmod +x "$S/codepatch"
cd "$S"
export QEMU_LD_PREFIX=${QEMU_LD_PREFIX:?an arm64 root file system for qemu}
# the shaders step in place (as the handheld: the page has done the other steps; the journal says so)
"$PMU" convert "$D" --config "$S/cuphead.json" --plugin animclips.py > "$R/convert.log" 2>&1 \
  || { echo "convert failed"; tail "$R/convert.log"; exit 1; }
grep -E "^\[.*(shaders|textures|settings|audio|animclips):" "$R/convert.log" | cut -c1-200
rm -rf "$R/data"; cp -a "$D" "$R/data"         # (the packing input, kept for the page's run)
DLC=; [ -f "$GD/.setup/.dlc_files" ] && DLC=--steam-dlc
/usr/bin/time -v "$PMU" android-data --src "$D" --pkg "$PLAYER" --out "$R/android" --name Cuphead --package com.cupheadport.game \
  --root-file animclips.txt --drop-missing-assembly GalaxyCSharp.dll --managed-file "$S/CupheadPort.dll" --game-managed "$D/Managed" \
  --assembly-patch Assembly-CSharp.dll "env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 '$S/codepatch' --target=android $DLC {managed} {out}" \
  --mono-etc "$GD/mono-etc" --dllmap SDL2=libSDL2-2.0.so.0 --verify-bundles 2 --pack all --no-video-shader > "$R/android.log" 2>&1 \
  || { echo "android-data failed"; tail -20 "$R/android.log"; exit 1; }
grep -E "placed:|pack:|plugins.map|done in|Elapsed|Maximum resident" "$R/android.log"
