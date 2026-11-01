#!/bin/bash
# android_data.sh <scratch dir> [case...]: golden tests of pmu android-data and patch-obb pack
# (convert/pmunity/android_data.py, patch_obb.py) on the Python's converted game
# ($PMU_TEST_DATA/converted-fast, with the DLC), with the invocations of Cuphead's setups. In the
# build image, after `cargo build --release --target aarch64-unknown-linux-musl` (or PMU=<pmu>):
#
#   tools/rs.sh <worktree> bash tests/golden/android_data.sh <scratch dir> [case...]
#
#   32      setup/patchscript step 4: the 32-bit player (Unity 2017.4.9f1, from the donor), the code
#           patch run by android-data (--assembly-patch), --consume-src, --pack all
#   64      setup64/patchscript step 5: the 64-bit IL2CPP player 2017.4.40f1 (--mono64: the data is
#           retargeted to it), Mono 4.x's class libraries, the port's plugin map
#   mini32, mini64   the same on a part of the data (the settings, Managed, Resources, a few scenes
#           and textures inlined from their .resS, 40 small bundles and two of the DLC's): minutes
#   obb     setup/dlcscript step 3: a game built (by the Python, as mini32) without the DLC's
#           bundles ($PMU_TEST_DATA/cuphead/dlc_bundles.txt), then patch-obb pack of the DLC's
#           converted bundles beside it
#   space   mini32 on a card that is too full ($PMU_TEST_FREE_BYTES free: the Python's os.statvfs
#           and pmu's statvfs report it): exit 4 (NO_SPACE) and the message, before any archive
#
# Each case runs the Python (python -m pmunity) and pmu on fresh copies of the data, at the same
# paths, one after the other (the disk holds one copy and its output at a time), and compares the
# exit status, stdout's pack: lines, and every file left: the output folder (APK, OBBs,
# android_package.json, plugins.map, the report and state), each archive's entries, and what
# --consume-src leaves of the data. Logs are not compared. Files under 4 MB of both runs are kept in
# <scratch>/keep/<case>.<impl>/ to look at a difference. PY_ONLY=1 / RUST_ONLY=1 run one side (the
# other's results kept from before). The tools the setups use (codepatch, CupheadPort.dll, Mono
# 4.x's class libraries) come from $PMU_TEST_DATA/port/setup*/setup*.zip; the players from the
# donors with the Python's player donor. pmu reads UnityPy's class database ($PMU_TPK).
set -u
S=$(mkdir -p "$1" && cd "$1" && pwd)
shift
CASES=${*:-mini32 mini64 obb space 32 64}
HERE=$(cd "$(dirname "$0")" && pwd)
WT=$(cd "$HERE/../../.." && pwd)
PMU=${PMU:-$CARGO_TARGET_DIR/aarch64-unknown-linux-musl/release/pmu}
T=${PMU_TEST_DATA:?set PMU_TEST_DATA}
DATA=$T/converted-fast
DLCN=$T/cuphead/dlc_bundles.txt
export PMU_TPK=${PMU_TPK:-/opt/pylibs/UnityPy/resources/lzma.tpk}
PY() { PYTHONHOME=/opt/py LD_LIBRARY_PATH=/opt/py/libs PYTHONPATH=$WT/convert:/opt/pylibs /opt/py/bin/python3 -P "$@"; }
pass=0 fail=0

# the setups' tools and the players (made once)
prep() {
  if [ ! -f "$S/setup/codepatch" ]; then
    rm -rf "$S/setup"
    mkdir -p "$S/setup"
    PY -c "import sys, zipfile
for z, pre in ((sys.argv[1], ('codepatch', 'CupheadPort.dll')), (sys.argv[2], ('mono64/classlibs/', 'mono64/etc/', 'plugins-il2cpp.json'))):
    z = zipfile.ZipFile(z)
    z.extractall(sys.argv[3], [n for n in z.namelist() if n.startswith(pre)])" \
      "$T/port/setup/setup.zip" "$T/port/setup64/setup64.zip" "$S/setup" || exit 1
    chmod +x "$S/setup/codepatch"
  fi
  if [ ! -f "$S/p32/player.json" ]; then
    rm -rf "$S/p32"
    PY -m pmunity player donor 2017.4.9f1 "$S/p32" --apk "$T/port/donor/donor-2017.4.9f1-mono-armv7.apk" > /dev/null || exit 1
  fi
  if [ ! -f "$S/p64/player.json" ]; then
    rm -rf "$S/p64"
    PY -m pmunity player donor 2017.4.40f1 "$S/p64" --backend il2cpp --abi arm64-v8a --managed \
      --apk "$T/port/donor/donor-2017.4.40f1-il2cpp-arm64.apk" --apk "$T/port/donor/donor-2017.4.40f1-mono-armv7.apk" > /dev/null || exit 1
  fi
  # the mini data: what is copied of the data folder
  if [ ! -s "$S/mini.txt" ]; then
    (cd "$DATA" && {
      ls -d globalgamemanagers globalgamemanagers.assets boot.config animclips.txt .pmunity.json Managed Resources \
        level0 level1 level2 sharedassets0.assets sharedassets1.assets sharedassets2.assets sharedassets2.assets.resS \
        resources.assets resources.assets.resS resources.resource StreamingAssets/Logo.png
      ls -S StreamingAssets/AssetBundles | grep -vxF -f "$DLCN" | tail -40 | sed 's|^|StreamingAssets/AssetBundles/|'
      ls -S StreamingAssets/AssetBundles | grep -xF -f "$DLCN" | tail -2 | sed 's|^|StreamingAssets/AssetBundles/|'
    }) > "$S/mini.txt"
  fi
}

# manifest <dir> <out>: every path under dir (types, sizes, SHA-256), logs left out; each zip's
# entries (name, size, CRC, offset) after it
manifest() {
  if [ ! -e "$1" ]; then echo "(none)" > "$2"; return; fi
  (cd "$1" && {
    find . -mindepth 1 ! -name make_android_data.log \( -type f -printf 'f %s %p\n' \) -o \( ! -type f -printf '%y %p\n' \) | LC_ALL=C sort
    find . -type f ! -name make_android_data.log -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum
    find . -type f \( -name '*.apk' -o -name '*.obb' \) -print0 | LC_ALL=C sort -z | while IFS= read -r -d '' z; do
      echo "== $z"
      PY -c "import sys, zipfile
for i in zipfile.ZipFile(sys.argv[1]).infolist(): print(i.filename, i.file_size, '%08x' % i.CRC, i.header_offset)" "$z"
    done
  }) > "$2"
}

# keep <dir> <dest>: the files under 4 MB of a run (logs too), to look at a difference
keep() {
  rm -rf "$2"
  mkdir -p "$2"
  [ -d "$1" ] && (cd "$1" && find . -type f -size -4M -print0 | xargs -0 -r cp --parents -t "$2")
}

# run <case> <py|rs> <workdir> <outdir> <src dir> <args...>: android-data (or patch-obb) with the
# setup's working directory; manifests of the out dir and the data left, status, time, peak RSS
run() {
  local c=$1 impl=$2 wd=$3 out=$4 src=$5
  shift 5
  local r=$S/r/$c.$impl t0=$SECONDS
  mkdir -p "$S/r"
  # (the state file records the sources' modification times: the same for both runs)
  find "$S/gd" "$S/dlc" -exec touch -h -d @1767225600 {} + 2>/dev/null
  local cmd=("$PMU" "$@")
  [ "$impl" = py ] && cmd=(env PYTHONHOME=/opt/py LD_LIBRARY_PATH=/opt/py/libs PYTHONPATH="$WT/convert:/opt/pylibs" \
    /opt/py/bin/python3 -P "$HERE/android_data_py.py" "$@")
  (cd "$wd" && PY "$HERE/peak_rss.py" "$r.rss" "${cmd[@]}") > "$r.out" 2> "$r.err"
  echo $? > "$r.rc"
  echo $((SECONDS - t0)) > "$r.time"
  grep '^pack: ' "$r.out" | grep -v ' MiB$' > "$r.pack"
  manifest "$out" "$r.tree"
  manifest "$src" "$r.src"
  keep "$out" "$S/keep/$c.$impl"
}

check() {
  local r=$S/r/$1 why=""
  cmp -s "$r.py.rc" "$r.rs.rc" || why="$why status $(cat "$r.py.rc") vs $(cat "$r.rs.rc");"
  cmp -s "$r.py.pack" "$r.rs.pack" || why="$why pack lines;"
  cmp -s "$r.py.tree" "$r.rs.tree" || why="$why output;"
  cmp -s "$r.py.src" "$r.rs.src" || why="$why data left;"
  local n z e
  n=$(grep -c '^f ' "$r.py.tree")
  z=$(grep -c '^== ' "$r.py.tree")
  e=$(grep -v '^[fdl] \|^[0-9a-f]\{64\} \|^== ' "$r.py.tree" | wc -l)
  if [ -z "$why" ]; then
    pass=$((pass + 1))
    echo "ok   $1 (status $(cat "$r.rs.rc"); $n files, $z archives, $e archive entries identical;" \
      "py $(cat "$r.py.time") s $(( $(cat "$r.py.rss") / 1024 )) MB, pmu $(cat "$r.rs.time") s $(( $(cat "$r.rs.rss") / 1024 )) MB)"
  else
    fail=$((fail + 1))
    echo "FAIL $1:$why"
    diff "$r.py.tree" "$r.rs.tree" | head -20
    diff "$r.py.src" "$r.rs.src" | head -10
    diff "$r.py.pack" "$r.rs.pack" | head -6
    tail -3 "$r.rs.err"
  fi
}

# fresh <dir> [mini] [nodlc]: the converted data copied to <dir> (all of it, or the mini part),
# without the DLC's bundles with nodlc. A whole copy made beforehand ($S/data_ready, with the file
# $S/data.ready) is used instead, once: the disk is slow.
fresh() {
  local d=$1 mini=${2:-} nodlc=${3:-}
  rm -rf "$d"
  mkdir -p "$(dirname "$d")"
  if [ -z "$mini" ] && [ -f "$S/data.ready" ] && [ -d "$S/data_ready" ]; then
    mv "$S/data_ready" "$d" && rm -f "$S/data.ready"
  elif [ -n "$mini" ]; then
    mkdir -p "$d"
    (cd "$DATA" && while read -r p; do cp -a --parents "$p" "$d/" || exit 1; done < "$S/mini.txt") || exit 1
  else
    mkdir -p "$d"
    cp -a "$DATA/." "$d/" || exit 1
  fi
  if [ -n "$nodlc" ]; then
    while read -r n; do rm -f "$d/StreamingAssets/AssetBundles/$n"; done < "$DLCN"
  fi
}

# the 32-bit setup's android-data (step 4), in $S/gd as the setup has it: the data in
# gamedata/Cuphead_Data, Mono's config from the app in gamedata/mono-etc, GOG's .info files
# beside the data. extra: arguments added (none in the setup)
setup32() {
  local c=$1 impl=$2 mini=$3 GD=$S/gd
  local D=$GD/Cuphead_Data W=$GD/.setup
  rm -rf "$GD"
  fresh "$D" "$mini"
  cp -r "$T/Cuphead.app/Contents/Mono/etc" "$GD/mono-etc"
  cp "$T/Cuphead.app/Contents/Resources/"goggame-*.info "$GD/"
  mkdir -p "$W"
  local DLC=; [ -f "$D/StreamingAssets/AssetBundles/atlas_map_common_dlc" ] && DLC=--steam-dlc
  run "$c" "$impl" "$S/setup" "$W/android" "$D" android-data --src "$D" --pkg "$S/p32" --out "$W/android" \
    --name Cuphead --package com.cupheadport.game \
    --root-file animclips.txt --drop-missing-assembly GalaxyCSharp.dll --managed-file "$S/setup/CupheadPort.dll" \
    --game-managed "$D/Managed" \
    --assembly-patch Assembly-CSharp.dll "env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 '$S/setup/codepatch' --target=android $DLC {managed} {out}" \
    --mono-etc "$GD/mono-etc" --dllmap SDL2=libSDL2-2.0.so.0 --consume-src --verify-bundles 2 --pack all --no-video-shader
  rm -rf "$GD"
}

# the 64-bit setup's android-data (step 5), in $S/gd/.setup64 as the setup has it
setup64() {
  local c=$1 impl=$2 mini=$3 GD=$S/gd
  local W=$GD/.setup64
  local D=$W/Cuphead_Data MP=$W/managed M64=$S/setup/mono64
  rm -rf "$GD"
  fresh "$D" "$mini"
  cp "$T/Cuphead.app/Contents/Resources/"goggame-*.info "$W/"
  local DLC=; [ -f "$DATA/StreamingAssets/AssetBundles/atlas_map_common_dlc" ] && DLC=--steam-dlc
  # (setup64 step 4: the code patch, run before android-data; once, the same for both runs)
  if [ ! -f "$S/managed64/CupheadPort.dll" ]; then
    rm -rf "$S/managed64"
    DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 "$S/setup/codepatch" --target=android $DLC "$D/Managed" "$S/managed64" > /dev/null \
      && cp "$S/setup/CupheadPort.dll" "$S/managed64/" || exit 1
  fi
  cp -a "$S/managed64" "$MP"
  local CL=() f
  for f in "$M64"/classlibs/*.dll; do CL+=(--managed-file "$f"); done
  run "$c" "$impl" "$S/setup" "$W/android" "$D" android-data --src "$D" --pkg "$S/p64" --out "$W/android" \
    --name Cuphead --package com.cupheadport.game \
    --root-file animclips.txt --drop-missing-assembly GalaxyCSharp.dll --mono64 --managed-file "$MP/Assembly-CSharp.dll" \
    --managed-file "$MP/CupheadPort.dll" "${CL[@]}" --mono-etc "$M64/etc" --dllmap SDL2=libSDL2-2.0.so.0 \
    --plugin-map "$S/setup/plugins-il2cpp.json" --consume-src --verify-bundles 2 --pack all --no-video-shader
  rm -rf "$GD"
}

case32() { setup32 32 "$1" ""; }
case64() { setup64 64 "$1" ""; }
casemini32() { setup32 mini32 "$1" mini; }
casemini64() { setup64 mini64 "$1" mini; }
casespace() { PMU_TEST_FREE_BYTES=$((600 << 20)) setup32 space "$1" mini; }

# the game built without the DLC (by the Python, once: the input of both packs; patch-obb pack
# reads its android_package.json), then the DLC's bundles packed beside it (the converted
# bundles, linked: pack only reads them)
caseobb() {
  local impl=$1 G=$S/obbgame B=$S/dlc/AssetBundles
  if [ ! -f "$G/android_package.json" ]; then
    local GD=$S/gd
    local D=$GD/Cuphead_Data W=$GD/.setup
    rm -rf "$GD" "$G"
    fresh "$D" mini nodlc
    cp -r "$T/Cuphead.app/Contents/Mono/etc" "$GD/mono-etc"
    (cd "$S/setup" && PY -m pmunity android-data --src "$D" --pkg "$S/p32" --out "$W/android" --name Cuphead \
      --package com.cupheadport.game --root-file animclips.txt --drop-missing-assembly GalaxyCSharp.dll \
      --managed-file "$S/setup/CupheadPort.dll" --mono-etc "$GD/mono-etc" --dllmap SDL2=libSDL2-2.0.so.0 \
      --consume-src --verify-bundles 0 --pack all --no-video-shader > "$S/obbgame.log" 2>&1) || { echo "FAIL obb: the game"; exit 1; }
    mv "$W/android/zip" "$G"
    rm -rf "$GD"
  fi
  rm -rf "$S/dlc"
  mkdir -p "$B"
  while read -r n; do ln -s "$DATA/StreamingAssets/AssetBundles/$n" "$B/$n"; done < "$DLCN"
  local g=$S/obbrun
  rm -rf "$g"
  cp -al "$G" "$g"
  run obb "$impl" "$S/setup" "$g" "$B" patch-obb pack "$B" "$g"
  rm -rf "$g" "$S/dlc"
}

prep
for c in $CASES; do
  [ -z "${RUST_ONLY:-}" ] && "case$c" py
  [ -z "${PY_ONLY:-}" ] && "case$c" rs
  [ -z "${PY_ONLY:-}" ] && check "$c"
done
echo "android-data golden: $pass identical, $fail different"
[ "$fail" = 0 ]
