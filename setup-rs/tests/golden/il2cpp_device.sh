#!/bin/bash
# il2cpp_device.sh <data> <scratch dir> <run> [il2cpp options...]: one run of the il2cpp-arm64
# route's build, `il2cpp <data> <scratch>/out --on-device --abi arm64-v8a --home <scratch>/home
# --keep-cpp [options]`, by the Python (<run> py1, py2, ...) or by pmu (rs1, rs2, ...; PMU=<pmu>),
# in the build image (arm64, natively, as on the handheld), then what it left recorded in
# <scratch>/runs/<run>/ before the folders are removed (the disk holds one build at a time):
#
#   manifest    every file and link under home/ and out/: sha256, mode, path (links: "L path ->
#               target")
#   records/    the text the build writes besides the C++: il2cpp.json, .work's logs and step
#               records, cc.log (every compiler command), Bee's build.log and pmunity-done.jsonl,
#               the tool folder's state and wrappers, the fetched parts' .complete files
#   stdout, stderr, status, seconds
#
# Every run is at the same paths, so the two implementations' files can be compared byte for
# byte: tests/golden/il2cpp_compare.py <scratch>/runs/<a> <scratch>/runs/<b>. Two runs of the
# Python show what differs from build to build anyway (the objects of libil2cpp's Runtime.cpp,
# which embeds __DATE__ and __TIME__, and what is linked from them).
#
#   tools/rs.sh <worktree> bash tests/golden/il2cpp_device.sh <data> <scratch> py1
#   tools/rs.sh <worktree> bash tests/golden/il2cpp_device.sh <data> <scratch> rs1
#
# <data> apk:<file> runs `apk <file> <scratch>/out --target arm64 --il2cpp --on-device --home
# <scratch>/home --keep-cpp --json [options]` instead (a Mono APK compiled for the arm64 IL2CPP
# player: the apk route's player.il2cpp; the build's files are in out/.il2cpp).
# <data> fetch:<version> runs `il2cpp fetch <version> --home <scratch>/home --on-device [options]`
# instead (the toolchain only: Unity's parts, the NDK, the player, a mixed build's converter for
# 2023.1+, the handheld's tools). KEEP=1 leaves home/ and out/ in place after the run (the next run removes them first). The
# reference Python (PortMaster's python_3.11) loads OpenSSL 1.1, which handhelds' firmwares have
# and the image has not: PY_SSL_LIB=<folder with libssl.so.1.1 and libcrypto.so.1.1> (Debian 11's
# libssl1.1 package unpacked).
set -u
DATA=$1
S=$(mkdir -p "$2" && cd "$2" && pwd)
RUN=$3
shift 3
HERE=$(cd "$(dirname "$0")" && pwd)
WT=${WT:-$(cd "$HERE/../../.." && pwd)}   # (the worktree whose convert/ is the reference Python)
PMU=${PMU:-$CARGO_TARGET_DIR/aarch64-unknown-linux-musl/release/pmu}
export PMU_TPK=${PMU_TPK:-/opt/pylibs/UnityPy/resources/lzma.tpk}
R=$S/runs/$RUN
[ "${RESUME:-}" = 1 ] || rm -rf "$S/home" "$S/out"
rm -rf "$R"
mkdir -p "$R/records"
case $RUN in
  py*) cmd=(env PYTHONHOME=/opt/py LD_LIBRARY_PATH="/opt/py/libs${PY_SSL_LIB:+:$PY_SSL_LIB}" PYTHONPATH="$WT/convert:/opt/pylibs"
            PYTHONDONTWRITEBYTECODE=1 /opt/py/bin/python3 -B -m pmunity) ;;
  rs*) cmd=("$PMU") ;;
  *) echo "run: py<n> or rs<n>" >&2; exit 2 ;;
esac
t0=$(date +%s)
W=out/.work
case $DATA in
  fetch:*) "${cmd[@]}" il2cpp fetch "${DATA#fetch:}" --home "$S/home" --on-device "$@" > "$R/stdout" 2> "$R/stderr" ;;
  *)
     if [ "${DATA#apk:}" != "$DATA" ]; then
       W=out/.il2cpp/.work
       "${cmd[@]}" apk "${DATA#apk:}" "$S/out" --target arm64 --il2cpp --on-device --home "$S/home" --keep-cpp --json "$@" \
         > "$R/stdout" 2> "$R/stderr" &
     else
       "${cmd[@]}" il2cpp "$DATA" "$S/out" --on-device --abi arm64-v8a --home "$S/home" --keep-cpp "$@" \
         > "$R/stdout" 2> "$R/stderr" &
     fi
     pid=$!
     if [ -n "${KILL_AT:-}" ]; then
       # a power cut: every process of the build killed once cc.log has KILL_AT commands (all but
       # this script and its parents)
       until [ "$(cat "$S/$W/cc.log" 2>/dev/null | wc -l)" -ge "$KILL_AT" ] || ! kill -0 $pid 2>/dev/null; do sleep 0.5; done
       keep=" $$ "; p=$$
       while [ "$p" -gt 1 ]; do p=$(awk '{print $4}' /proc/$p/stat); keep="$keep$p "; done
       for d in /proc/[0-9]*; do n=${d#/proc/}; case "$keep" in *" $n "*) ;; *) kill -9 "$n" 2>/dev/null ;; esac; done
     fi
     wait $pid ;;
esac
echo $? > "$R/status"
echo $(( $(date +%s) - t0 )) > "$R/seconds"
# the manifest
( cd "$S" && find home out \( -type f -o -type l \) 2>/dev/null | LC_ALL=C sort | while IFS= read -r f; do
    if [ -L "$f" ]; then echo "L $f -> $(readlink "$f")"
    else echo "$(sha256sum < "$f" | cut -c1-64) $(stat -c %a "$f") $f"; fi
  done ) > "$R/manifest"
# the records
( cd "$S" && find out -maxdepth 1 -type f; find out/.il2cpp -maxdepth 1 -type f 2>/dev/null; find $W -maxdepth 1 -type f
  find $W/cache -path '*/buildstate/*' \( -name '*.jsonl' -o -name '*.args' \) 2>/dev/null
  find home -name '.complete*' -o -name '.state.json' -o -path 'home/device-arm64/bin/*' \
    -o -name 'player.json' -o -name 'bee_backend' -o -path '*/deploy/il2cpp' -o -name 'ndk-clang.sh' 2>/dev/null
  find home -path '*/toolchains/llvm/prebuilt/linux-x86_64/bin/clang' 2>/dev/null ) | LC_ALL=C sort -u |
  while IFS= read -r f; do
    [ -f "$S/$f" ] || continue
    [ "$(stat -c %s "$S/$f")" -lt 8000000 ] || continue
    mkdir -p "$R/records/$(dirname "$f")" && cp -p "$S/$f" "$R/records/$f"
  done
[ -n "${KEEP:-}" ] || rm -rf "$S/home" "$S/out"
echo "$RUN: status $(cat "$R/status"), $(cat "$R/seconds") s, $(wc -l < "$R/manifest") files"
