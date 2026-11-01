#!/bin/bash
# pkgzip.sh <scratch dir> [part...]: golden tests of pmu's macpkg, zip-extract, player donor,
# patch-obb new/ggm/check and the aligned zip writer (crates/pkgzip) against the Python
# (convert/pmunity, `python -m zipfile -e`) on the same inputs: real ones from $PMU_TEST_DATA and
# made-up ones (pkgzip_make.py). Runs in the build image, after
# `cargo build --release --target aarch64-unknown-linux-musl` (or PMU=<a pmu binary>):
#
#   tools/rs.sh <worktree> bash tests/golden/pkgzip.sh <scratch dir> [pkgs zip donor obb writezip dlc both game cut]
#
# Each case runs the Python and pmu with the same arguments and compares the exit status, stdout,
# the output tree (paths, types, modes, sizes, SHA-256 of every file) and stderr (where the Python
# ends with a message rather than a traceback). Big outputs are hashed and removed before the next
# run: the game's installer unpacks to ~4 GB. A real setup.zip at <scratch dir>/setup.zip is tested
# too. KEEP_PY=1 keeps the Python's results of an earlier run (its runs of the big installers take
# the longest). Prints one line a case and the totals; exits 1 when a case differs.
set -u
S=$(cd "$1" && pwd)
shift
PARTS=${*:-pkgs zip donor obb writezip dlc both game cut}
HERE=$(cd "$(dirname "$0")" && pwd)
WT=$(cd "$HERE/../../.." && pwd)
PMU=${PMU:-$CARGO_TARGET_DIR/aarch64-unknown-linux-musl/release/pmu}
T=${PMU_TEST_DATA:?set PMU_TEST_DATA}
PY() { PYTHONHOME=/opt/py LD_LIBRARY_PATH=/opt/py/libs PYTHONPATH=$WT/convert:/opt/pylibs /opt/py/bin/python3 -P "$@"; }
pass=0 fail=0 files=0

# tree <dir> <manifest>: every path under dir with its type and mode (files: size and SHA-256)
tree() {
  if [ ! -e "$1" ]; then echo "(none)" > "$2"; return; fi
  (cd "$1" && {
    find . -mindepth 1 \( -type f -printf 'f %#m %s %p\n' \) -o \( -type d -printf 'd %#m %p\n' \) -o -printf '%y %p\n' | LC_ALL=C sort
    find . -type f -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum
  }) > "$2"
}

# run <name> <py|rs> <out dir> <args...>: the command (pmunity's or pmu's) with its out, err, status
# and the tree of <out dir> under $S/r/<name>.<impl>.*; then the out dir is removed
run() {
  local name=$1 impl=$2 out=$3
  shift 3
  local r=$S/r/$name.$impl t0=$SECONDS
  mkdir -p "$S/r"
  rm -rf "$out"
  if [ "$impl" = py ] && [ -n "${KEEP_PY:-}" ] && [ -f "$r.tree" ]; then return; fi   # (a Python run kept)
  if [ "$impl" = py ]; then
    if [ "$1" = zip-extract ]; then shift; PY -m zipfile -e "$@"; else PY -m pmunity "$@"; fi
  else
    "$PMU" "$@"
  fi > "$r.out" 2> "$r.err"
  echo $? > "$r.rc"
  echo $((SECONDS - t0)) > "$r.time"
  tree "$out" "$r.tree.part" && mv "$r.tree.part" "$r.tree"     # (whole, or not there)
  rm -rf "$out"
}

# check <name>: compares the two runs of a case
check() {
  local r=$S/r/$1 why=""
  cmp -s "$r.py.rc" "$r.rs.rc" || why="$why status $(cat "$r.py.rc") vs $(cat "$r.rs.rc");"
  cmp -s "$r.py.out" "$r.rs.out" || why="$why stdout;"
  cmp -s "$r.py.tree" "$r.rs.tree" || why="$why tree;"
  if [ "$(cat "$r.py.rc")" = 2 ]; then          # (argparse: its usage, then the error line)
    [ "$(tail -1 "$r.py.err")" = "$(tail -1 "$r.rs.err")" ] || why="$why stderr;"
  elif ! grep -q Traceback "$r.py.err"; then
    cmp -s "$r.py.err" "$r.rs.err" || why="$why stderr;"
  fi
  local n
  n=$(grep -c '^f ' "$r.py.tree")
  files=$((files + n))
  if [ -z "$why" ]; then
    pass=$((pass + 1))
    echo "ok   $1 (status $(cat "$r.rs.rc"), $n files; py $(cat "$r.py.time") s, pmu $(cat "$r.rs.time") s)"
  else
    fail=$((fail + 1))
    echo "FAIL $1:$why"
    diff "$r.py.tree" "$r.rs.tree" | head -5
    diff "$r.py.out" "$r.rs.out" | head -5
  fi
  if grep -q Traceback "$r.py.err"; then
    echo "     python: $(tail -1 "$r.py.err" | cut -c1-150)"
    echo "     pmu:    $(tail -1 "$r.rs.err" | cut -c1-150)"
  fi
}

# both <name> <out dir> <args...>: runs and compares a case
both() {
  local name=$1 out=$2
  shift 2
  run "$name" py "$out" "$@"
  run "$name" rs "$out" "$@"
  check "$name"
}

DLC=$T/pkg/dlc_cuphead___the_delicious_last_course_enUS_1_3_9_.pkg
GAME=$T/pkg/cuphead_enUS_1_3_9_.pkg
O=$S/o

for part in $PARTS; do
  case $part in
  pkgs)       # made-up installers: every payload form, encodings, member kinds, errors
    PY "$HERE/pkgzip_make.py" pkgs "$S/pkgs"
    for p in pbzx bz2 gz2 gog nofiles notxar cut cuttoc; do
      both "macpkg-$p" "$O" macpkg "$O/root" "$S/pkgs/$p.pkg"
    done
    both macpkg-several "$O" macpkg "$O/root" "$S/pkgs/gog.pkg" "$S/pkgs/pbzx.pkg" "$S/pkgs/cut.pkg" "$S/pkgs/bz2.pkg"
    both macpkg-usage "$O" macpkg "$O/root"
    ;;
  zip)        # made-up zips and the setup's own (python -m zipfile -e)
    PY "$HERE/pkgzip_make.py" zips "$S/zips"
    [ -f "$S/setup.zip" ] && cp "$S/setup.zip" "$S/zips/"
    for z in "$S"/zips/*.zip; do
      both "zip-$(basename "$z" .zip)" "$O" zip-extract "$z" "$O/x"
    done
    ;;
  donor)      # the player out of the donor APKs, as both setups run it
    D=$T/donor
    both donor-9f1-mono "$O" player donor 2017.4.9f1 "$O/p" --apk "$D/donor-2017.4.9f1-mono-armv7.apk"
    both donor-40f1-il2cpp "$O" player donor 2017.4.40f1 "$O/p" --backend il2cpp --abi arm64-v8a --managed \
      --apk "$D/donor-2017.4.40f1-il2cpp-arm64.apk" --apk "$D/donor-2017.4.40f1-mono-armv7.apk"
    both donor-abbrev "$O" player donor --back=il2cpp 2017.4.40f1 --abi arm64-v8a --man "$O/p" \
      --apk "$D/donor-2017.4.40f1-il2cpp-arm64.apk" --apk="$D/donor-2017.4.40f1-mono-armv7.apk"
    both donor-wrong-version "$O" player donor 2017.4.40f1 "$O/p" --apk "$D/donor-2017.4.9f1-mono-armv7.apk"
    both donor-missing "$O" player donor 2017.4.9f1 "$O/p" --backend il2cpp --abi arm64-v8a --apk "$D/donor-2017.4.9f1-mono-armv7.apk"
    both donor-no-apk "$O" player donor 2017.4.9f1 "$O/p"
    both donor-bad-choice "$O" player donor 2017.4.9f1 "$O/p" --abi x86 --apk "$D/donor-2017.4.9f1-mono-armv7.apk"
    ;;
  obb)        # patch-obb new, ggm, check on the DLC's real bundles beside a made-up built game
    B=$S/obb
    rm -rf "$B"
    mkdir -p "$B"
    NAMES=$T/cuphead/dlc_bundles.txt
    DLCB=$T/dlc-unpacked.app/Contents/Resources/Data/StreamingAssets/AssetBundles
    GAMEB=$T/Cuphead.app/Contents/Resources/Data/StreamingAssets/AssetBundles
    GGM=$T/Cuphead.app/Contents/Resources/Data/globalgamemanagers
    mapfile -t N < "$NAMES"
    PY "$HERE/pkgzip_make.py" game "$B/game" Cuphead.apk "$GGM" main.3.com.example.game.obb "${N[@]:0:3}" atlas_x
    PY "$HERE/pkgzip_make.py" game "$B/game2" Cuphead.apk "$GGM" ""
    PY -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1],'w'); [z.writestr('assets/AssetBundles/'+n, b'') for n in sys.argv[2:]]; z.close()" \
      "$B/game2/patch.3.com.example.game.obb" "${N[@]:3:2}"
    link() { mkdir -p "$1"; for n in "${@:2}"; do ln -s "$DLCB/$n" "$1/$n"; done; }
    link "$B/some" "${N[@]:0:50}"                      # a copy that stopped part way
    link "$B/cut" "${N[@]:1}"                          # one bundle cut short
    head -c 100000 "$DLCB/${N[0]}" > "$B/cut/${N[0]}"
    link "$B/nogame3" "${N[@]:3}"                      # all but what the built game holds
    link "$B/notes" "${N[@]:0:4}"
    : > "$B/notes/${N[4]}"                             # an empty file, a header alone, a dot file
    head -c 40 "$DLCB/${N[5]}" > "$B/notes/${N[5]}"
    echo x > "$B/notes/.DS_Store"
    mkdir "$B/notes/${N[6]}"                           # (a folder of a bundle's name: not a bundle)
    mkdir -p "$B/gameonly"
    for f in "$GAMEB"/*; do grep -qx "$(basename "$f")" "$NAMES" || ln -s "$f" "$B/gameonly/"; done
    printf '%s\r\n' "${N[@]:0:10}" > "$B/names-crlf.txt"
    both obb-new "$O" patch-obb new "$DLCB" "$B/game"
    both obb-new-patch "$O" patch-obb new "$GAMEB" "$B/game2"
    PY -m pmunity patch-obb ggm "$B/game" "$B/ggm.py"; "$PMU" patch-obb ggm "$B/game" "$B/ggm.rs"
    if cmp -s "$B/ggm.py" "$B/ggm.rs" && cmp -s "$B/ggm.py" "$GGM"; then pass=$((pass + 1)); echo "ok   obb-ggm (bytes)"
    else fail=$((fail + 1)); echo "FAIL obb-ggm"; fi
    both obb-ggm-noapk "$O" patch-obb ggm "$B/some" "$O/ggm"
    both obb-check-dlc-alone "$O" patch-obb check "$NAMES" "$DLCB"
    both obb-check-game-and-dlc "$O" patch-obb check "$NAMES" "$GAMEB" "$DLCB"
    both obb-check-game-alone "$O" patch-obb check "$NAMES" "$B/gameonly"
    both obb-check-some "$O" patch-obb check "$NAMES" "$B/some"
    both obb-check-cut "$O" patch-obb check "$NAMES" "$B/cut"
    both obb-check-built-game "$O" patch-obb check "$NAMES" "$B/nogame3" --game "$B/game"
    both obb-check-without-game "$O" patch-obb check "$NAMES" "$B/nogame3"
    both obb-check-patch-obb "$O" patch-obb check "$NAMES" --game "$B/game2" "$B/nogame3"
    both obb-check-notes "$O" patch-obb check "$NAMES" "$B/notes"
    both obb-check-crlf "$O" patch-obb check "$B/names-crlf.txt" "$B/notes"
    both obb-check-game-last "$O" patch-obb check "$NAMES" "$DLCB" --game
    both obb-check-nodir "$O" patch-obb check "$NAMES" "$B/none"
    both obb-usage "$O" patch-obb new "$DLCB"
    both obb-usage2 "$O" patch-obb check "$NAMES"
    rm -rf "$B"
    ;;
  writezip)   # android_data._write_zip and verify_zip vs pkgzip::zipwrite, same entries
    Z=$S/wz
    rm -rf "$Z"
    mkdir -p "$Z"
    DATA=$T/Cuphead.app/Contents/Resources/Data
    {
      printf 'assets/0-1-2-3-4\thex:\n'
      printf 'assets/bin/Data/globalgamemanagers\tfile:%s\n' "$DATA/globalgamemanagers"
      printf 'assets/bin/Data/Managed/Assembly-CSharp.dll\tfile:%s\n' "$DATA/Managed/Assembly-CSharp.dll"
      for f in $(ls "$DATA/StreamingAssets/AssetBundles" | head -45); do
        printf 'assets/AssetBundles/%s\tfile:%s\n' "$f" "$DATA/StreamingAssets/AssetBundles/$f"
      done
      printf 'assets/é ü.bin\thex:00010203\n'
      printf 'unity_obb_guid\thex:%s\n' "$(printf 0-1-2-3-4 | od -An -tx1 | tr -d ' \n')"
    } > "$Z/spec"
    PY "$HERE/pkgzip_writezip.py" "$Z/spec" "$Z/py.obb" > "$Z/py.log"
    PMU_ZIP_SPEC=$Z/spec PMU_ZIP_OUT=$Z/rs.obb cargo test -q -p pkgzip --test golden_writezip -- --ignored > "$Z/rs.log" 2>&1
    n=$(wc -l < "$Z/spec")
    if cmp -s "$Z/py.obb" "$Z/rs.obb" && PY "$HERE/pkgzip_writezip.py" --verify "$Z/spec" "$Z/rs.obb" > /dev/null; then
      pass=$((pass + 1)); echo "ok   writezip ($n entries, $(stat -c %s "$Z/rs.obb") bytes, identical)"
    else
      fail=$((fail + 1)); echo "FAIL writezip"; cmp "$Z/py.obb" "$Z/rs.obb"; tail -3 "$Z/rs.log"
    fi
    rm -rf "$Z"
    ;;
  dlc)        # the DLC's real installer (1.6 GB unpacked)
    both macpkg-dlc "$O" macpkg "$O/root" "$DLC"
    ;;
  both)       # a GOG-style game installer, then the real DLC merged into its .app (as the setup runs
              # both installers; the real game's would need 5.5 GB of disk)
    dest=$(sed -n 's/.*Scripts\/payload -> \/\(.*\): [0-9]* files$/\1/p' "$S/r/macpkg-dlc.py.out")
    [ -n "$dest" ] || { echo "FAIL both: run the dlc part first"; fail=$((fail + 1)); continue; }
    mapfile -t P < <(sed -n "s|^f [^ ]* [^ ]* ./$dest/||p" "$S/r/macpkg-dlc.py.tree" | head -5)
    PY "$HERE/pkgzip_make.py" gamepkg "$S/fake" "/$(dirname "$dest")" "$(basename "$dest")" "${P[@]}"
    both macpkg-game-then-dlc "$O" macpkg "$O/root" "$S/fake/fakegame.pkg" "$DLC"
    rm -rf "$S/fake"
    ;;
  game)       # the game's real installer (~4 GB unpacked)
    both macpkg-game "$O" macpkg "$O/root" "$GAME"
    ;;
  cut)        # copies of the real installers that stopped early (exit 3)
    C=$S/cut
    rm -rf "$C"
    mkdir -p "$C/a" "$C/b" "$C/c"
    head -c 100000000 "$GAME" > "$C/a/$(basename "$GAME")"
    head -c 30000 "$GAME" > "$C/b/$(basename "$GAME")"
    truncate -s $(($(stat -L -c %s "$GAME") - 1)) "$C/b/$(basename "$GAME")"   # (sparse: one byte short)
    head -c 9000 "$DLC" > "$C/c/$(basename "$DLC")"                           # (the TOC cut)
    both macpkg-cut-100mb "$O" macpkg "$O/root" "$C/a/$(basename "$GAME")"
    both macpkg-cut-1byte "$O" macpkg "$O/root" "$C/b/$(basename "$GAME")"
    both macpkg-cut-toc "$O" macpkg "$O/root" "$C/c/$(basename "$DLC")"
    both macpkg-ok-then-cut "$O" macpkg "$O/root" "$S/pkgs/gog.pkg" "$C/b/$(basename "$GAME")"
    rm -rf "$C"
    ;;
  *) echo "unknown part $part" ;;
  esac
done
echo "pkgzip golden: $pass identical, $fail different ($files files compared)"
[ "$fail" = 0 ]
