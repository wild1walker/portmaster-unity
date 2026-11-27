#!/bin/bash
# PC side: test a packed game on a handheld over SSH or adb, and bring back what happened.
#   DEVICE=root@<handheld> tools/device/test.sh <game folder> <seconds> [screenshot seconds...]
# <game folder> holds what the handheld needs: lib/ with the player's libraries
# (Variations/mono/Release/Libs/<abi>/*.so plus the Mono runtime the game uses, see docs/porting.md
# step 4) and the contents of android-data's zip/ folder (APK, OBBs, android_package.json).
# Steps:
#   1. copies the folder to the handheld (only files whose size or time changed), with this checkout's
#      ua_host and uatest.sh
#   2. runs it there for <seconds> (uatest.sh: silent, a memory watchdog, the screenshots)
#   3. copies run.log, mem.log and the screenshots back into $OUT, and prints a summary: frames per
#      second, memory, how the run ended
# Settings (environment):
#   DEVICE=user@host  the handheld (required). SSH must log in without a password prompt: a key, or
#                     a ControlMaster connection opened beforehand (see SSH_OPTS). DEVICE=adb (or
#                     adb:<serial>): over adb instead, e.g. USB without a network (transport.sh)
#   SSH_OPTS="..."    extra ssh/scp options, e.g. "-o ControlPath=~/.ssh/cm-%r@%h" to reuse an open
#                     ControlMaster connection
#   REMOTE=<dir>      the folder on the handheld (default: uatest/<game folder's name> in the login's
#                     home directory)
#   ARCH=armhf|arm64  which ua_host to send: runtime/host/out/ua_host (armhf, the default) or
#                     runtime/host/out/arm64/ua_host
#   HOST_BIN=<file>   another ua_host build to send instead
#   OUT=<dir>         where results go (default: devtest-<game folder's name>/ in the current folder)
#   STOP_ES=1         stop EmulationStation during the run and start it again afterwards (Knulli and
#                     Batocera: /etc/init.d/S31emulationstation); it holds GPU memory and CPU time.
#                     The handheld does both itself: the run is detached there, and a dropped
#                     connection neither ends it early nor leaves EmulationStation stopped
#   UA_*, SDL_*, GC_*, MONO_* and the prefixes in DEV_ENV (space-separated) pass through to the game,
#   e.g. UA_PAD_NAME="Xbox Wireless Controller" UA_MEM_MIN=150 UA_AUDIO=1
# Nothing runs on the handheld except the files sent here, and the watchdog only stops that game.
set -u
GAME=${1:?usage: DEVICE=user@host test.sh <game folder> <seconds> [screenshot seconds...]}; SECS=${2:-60}; shift 2
: "${DEVICE:?set DEVICE=user@host (the handheld)}"
HERE=$(cd "$(dirname "$0")" && pwd); PU=$(cd "$HERE/../.." && pwd)
GAME=$(cd "$GAME" && pwd) || exit 1; NAME=$(basename "$GAME")
REMOTE=${REMOTE:-uatest/$NAME}; OUT=${OUT:-$PWD/devtest-$NAME}
case "${ARCH:-armhf}" in
  armhf) BIN=${HOST_BIN:-$PU/runtime/host/out/ua_host} ;;
  arm64) BIN=${HOST_BIN:-$PU/runtime/host/out/arm64/ua_host} ;;
  *) echo "ARCH=${ARCH}: armhf or arm64"; exit 1 ;;
esac
[ -f "$BIN" ] || { echo "no ua_host at $BIN: build it first (docs/building.md)"; exit 1; }
[ -d "$GAME/lib" ] || { echo "$GAME has no lib/ folder with the player's libraries"; exit 1; }
ls "$GAME"/*.apk >/dev/null 2>&1 || { echo "$GAME has no APK (copy android-data's zip/ folder into it)"; exit 1; }
. "$HERE/transport.sh"                                        # dev_sh, dev_tar_in, dev_tar_out
q() { [ $# = 0 ] || printf '%q ' "$@"; }
# macOS tar (bsdtar) would add its metadata (._ files, xattr headers) to the archive
TARC=(); tar --version 2>/dev/null | grep -q bsdtar && TARC=(--no-xattrs --no-mac-metadata)

echo "== copying to $(dev_name):$REMOTE"
remote_sizes=$(dev_sh "mkdir -p $(q "$REMOTE") && cd $(q "$REMOTE") && find . -type f ! -name 'shot-*' ! -name '*.log' -exec stat -c '%s %Y %n' {} ';'") || {
  echo "cannot reach $(dev_name) (ssh must work without a password prompt; adb: adb devices lists it)"; exit 1; }
# the files whose size or time differ from the handheld's copy (hidden files and folders, shots and logs left out)
lstat() { stat -c '%s %Y %n' "$@" 2>/dev/null || stat -f '%z %m %N' "$@"; }    # (GNU, else BSD stat)
local_sizes=$(cd "$GAME" && find . \( -name '.?*' -prune \) -o -type f ! -name 'shot-*' ! -name '*.log' -print | while IFS= read -r f; do lstat "$f"; done)
strip='{ n = $0; sub(/^[^ ]* [^ ]* /, "", n) }'
changed=$(awk "$strip NR == FNR { have[n] = \$1 \" \" \$2; next } { if (have[n] != \$1 \" \" \$2) print n }" \
  <(printf '%s\n' "$remote_sizes") <(printf '%s\n' "$local_sizes"))
mb=$(awk "NR == FNR { c[\$0] = 1; next } $strip { if (n in c) s += \$1 } END { print int(s / 1048576) }" \
  <(printf '%s\n' "$changed") <(printf '%s\n' "$local_sizes"))
if [ -n "$changed" ]; then
  n=$(printf '%s\n' "$changed" | wc -l | tr -d ' ')
  echo "   $n changed file(s), $mb MB"
  (cd "$GAME" && printf '%s\n' "$changed" | COPYFILE_DISABLE=1 tar ${TARC[@]+"${TARC[@]}"} -cf - -T -) | dev_tar_in "$REMOTE" ||
    { echo "copy failed"; exit 1; }
else
  echo "   up to date"
fi
COPYFILE_DISABLE=1 tar ${TARC[@]+"${TARC[@]}"} -cf - -C "$(dirname "$BIN")" "$(basename "$BIN")" -C "$HERE" uatest.sh | dev_tar_in "$REMOTE" &&
  dev_sh "cd $(q "$REMOTE") && { [ $(q "$(basename "$BIN")") = ua_host ] || mv -f $(q "$(basename "$BIN")") ua_host; } && chmod +x ua_host uatest.sh" ||
  { echo "copying ua_host failed"; exit 1; }

# the game's settings: UA_*, SDL_*, GC_*, MONO_* and DEV_ENV's prefixes
envs=()
for v in $(compgen -e); do
  for p in UA_ SDL_ GC_ MONO_ ${DEV_ENV:-}; do [[ $v == "$p"* ]] && envs+=("$v=${!v}") && break; done
done

# The run happens on the handheld by itself (detached, with its own EmulationStation stop and
# start: uatest.sh's UATEST_STOP_ES), so a dropped connection neither ends it early nor leaves
# EmulationStation stopped; this end waits for its uatest.done and reconnects when it can
[ "${STOP_ES:-0}" = 1 ] && { envs+=("UATEST_STOP_ES=1"); echo "== EmulationStation is stopped for the run and started again after it (on the handheld)"; }
echo "== running for ${SECS} s${*:+, screenshots at $* s}"
# (detached: util-linux's setsid -f forks first, which an adb shell's end does not reach, as it
# reaches a plain background job; BusyBox's setsid has no -f: there the background job, as over SSH)
run="nohup env $(q "${envs[@]+"${envs[@]}"}")./uatest.sh . $(q "$SECS" "$@") > uatest.out 2>&1 < /dev/null"
dev_sh "cd $(q "$REMOTE") && rm -f uatest.done && if setsid -f true 2>/dev/null; then setsid -f $run; else { setsid $run & }; fi" ||
  { echo "starting the run failed"; exit 1; }
deadline=$((SECONDS + SECS + 600)); lost=0
until dev_sh "[ -f $(q "$REMOTE/uatest.done")]" 2>/dev/null; do
  if ! dev_sh true 2>/dev/null; then
    [ $lost = 0 ] && echo "   (the handheld is not answering; the run goes on there, waiting for it)"; lost=1
  fi
  [ $SECONDS -gt $deadline ] && { echo "no end of the run after $((SECS + 600)) s (see uatest.out on the handheld)"; exit 1; }
  sleep 10
done
[ "${STOP_ES:-0}" = 1 ] && echo "== EmulationStation started again (on the handheld)"

echo "== results in $OUT"
mkdir -p "$OUT" && rm -f "$OUT"/shot-*.ppm "$OUT"/shot-*.png
dev_tar_out "$REMOTE" run.log mem.log 'shot-*.ppm' | tar -xf - -C "$OUT" ||
  { echo "fetching the results failed"; exit 1; }
"$PU/setup-rs/tools/pmu-dev.sh" uatest-sum "$OUT"
