#!/bin/bash
# PC side: one measured run of a game already on a handheld (as tools/device/test.sh left it),
# with an optional profile, and the results brought back and summed up.
#   DEVICE=root@<handheld> tools/device/profbench.sh <game folder on the handheld> <name> [VAR=value ...]
#   DEVICE=root@<handheld> tools/device/profbench.sh <game folder on the handheld> --list <file>
# <name> names the run (its results go to $OUT/<name>/); VAR=value settings are passed to that run
# (ua_host's UA_* switches, PLAN, MEASURE, ...). A --list file holds one run per line, "<name>
# [VAR=value ...]" in shell quoting ('#' starts a comment): the runs go one after another, then a
# table compares them (tools/analysis/benchsum.py).
# Each run (tools/device/profrun.sh on the handheld, detached: a dropped connection does not end it):
#   starts the game silent, follows PLAN to the point to measure (e.g. a save continued from the
#   title screen: profrun.sh lists the steps), measures MEASURE seconds (frame rate every second,
#   CPU and GPU clocks, per-thread and per-core CPU time, memory and swap traffic, temperatures;
#   with UA_PROF=<hz> a profile: runtime/host/prof.c), stops the game and starts EmulationStation
#   again. Nothing on the handheld changes but <game folder>/bench/: clocks and governors are read,
#   never set.
# Settings (environment):
#   DEVICE, SSH_OPTS     as for test.sh (user@host over SSH, or adb / adb:<serial>: transport.sh)
#   OUT=<dir>            results (default ./profbench)
#   PLAN, MEASURE        defaults for every run (a run's own VAR=value wins)
#   STOP_ES=0            leave EmulationStation running (default: stopped during the runs: it
#                        holds GPU memory and CPU time)
#   ARCH, HOST_BIN       the ua_host to send (default this checkout's runtime/host/out/ua_host);
#                        SEND_HOST=0 runs the game folder's own ua_host
#   SYMS=<dir>           Unity's symbol files for the summaries (benchsum.py --syms; naming them needs
#                        pyelftools in PYTHON, default python3)
#   UA_*, SDL_*, GC_*, MONO_* in the environment pass through to every run, as with test.sh
set -u
DIR=${1:?usage: DEVICE=user@host profbench.sh <game folder on the handheld> <name> [VAR=value ...] | --list <file>}; shift
: "${DEVICE:?set DEVICE=user@host (the handheld)}"
HERE=$(cd "$(dirname "$0")" && pwd); PU=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-$PWD/profbench}
case "${ARCH:-armhf}" in
  armhf) BIN=${HOST_BIN:-$PU/runtime/host/out/ua_host} ;;
  arm64) BIN=${HOST_BIN:-$PU/runtime/host/out/arm64/ua_host} ;;
  *) echo "ARCH=${ARCH}: armhf or arm64"; exit 1 ;;
esac
. "$HERE/transport.sh"                                        # dev_sh, dev_tar_in, dev_tar_out
rshn() { dev_sh "$@"; }
q() { [ $# = 0 ] || printf '%q ' "$@"; }
TARC=(); tar --version 2>/dev/null | grep -q bsdtar && TARC=(--no-xattrs --no-mac-metadata)
B=$DIR/bench

# the runs: one from the command line, or a list
runs=()
if [ "${1:-}" = --list ]; then
  [ -f "${2:-}" ] || { echo "--list: no file ${2:-}"; exit 1; }
  while IFS= read -r line; do runs+=("$line"); done < <(python3 -I -c '
import shlex, sys
for line in open(sys.argv[1]):
    w = shlex.split(line, comments=True)
    if w: print(shlex.join(w))' "$2")
else
  [ -n "${1:-}" ] || { echo "a run name is needed"; exit 1; }
  runs+=("$(printf '%q ' "$@")")
fi

echo "== $(dev_name):$DIR (${#runs[@]} run(s))"
rshn "cd $(q "$DIR") && [ -d lib ] && ls *.apk >/dev/null 2>&1 && mkdir -p bench" ||
  { echo "$DIR on $(dev_name) has no game (lib/ and an APK: send it with test.sh first), or the handheld is not answering"; exit 1; }
if [ "${SEND_HOST:-1}" = 1 ]; then
  [ -f "$BIN" ] || { echo "no ua_host at $BIN: build it first (docs/building.md)"; exit 1; }
  COPYFILE_DISABLE=1 tar ${TARC[@]+"${TARC[@]}"} -cf - -C "$(dirname "$BIN")" "$(basename "$BIN")" -C "$HERE" profrun.sh | dev_tar_in "$B" &&
    rshn "cd $(q "$B") && { [ $(q "$(basename "$BIN")") = ua_host ] || mv -f $(q "$(basename "$BIN")") ua_host; } && chmod +x ua_host profrun.sh" ||
    { echo "copying ua_host failed"; exit 1; }
else
  COPYFILE_DISABLE=1 tar ${TARC[@]+"${TARC[@]}"} -cf - -C "$HERE" profrun.sh | dev_tar_in "$B" &&
    rshn "cd $(q "$B") && cp -f ../ua_host ua_host && chmod +x ua_host profrun.sh" || { echo "copying failed"; exit 1; }
fi

base=()
for v in $(compgen -e); do
  for p in UA_ SDL_ GC_ MONO_; do [[ $v == "$p"* ]] && base+=("$v=${!v}") && break; done
done
[ -n "${PLAN:-}" ] && base+=("PLAN=$PLAN")
[ -n "${MEASURE:-}" ] && base+=("MEASURE=$MEASURE")
[ "${STOP_ES:-1}" = 1 ] && base+=("UATEST_STOP_ES=1")
names=()
for r in "${runs[@]}"; do
  eval "set -- $r"       # (words quoted by printf %q / shlex.join above)
  name=$1; shift
  case $name in *[!A-Za-z0-9._-]*|'') echo "run name '$name': letters, digits, . _ - only"; exit 1 ;; esac
  envs=("${base[@]+"${base[@]}"}")
  for kv in "$@"; do
    [[ $kv == *=* ]] || { echo "$name: '$kv' is not VAR=value"; exit 1; }
    [[ $kv == UA_AUDIO=* || $kv == SDL_AUDIODRIVER=* ]] && { echo "$name: runs are silent ($kv ignored)"; continue; }
    envs+=("$kv")
  done
  echo "== $name: ${*:-defaults}"
  # (detached as test.sh does: setsid -f where there is one, which an adb shell's end does not reach)
  run="nohup env $(q "${envs[@]+"${envs[@]}"}") ./profrun.sh $(q "$DIR" "$name") > $(q "$name.out") 2>&1 < /dev/null"
  rshn "cd $(q "$B") && rm -f bench.done && if setsid -f true 2>/dev/null; then setsid -f $run; else { setsid $run & }; fi" ||
    { echo "starting the run failed"; exit 1; }
  deadline=$((SECONDS + 3600)); lost=0; started=$((SECONDS + 30))
  until rshn "[ -f $(q "$B/bench.done") ]" 2>/dev/null; do
    if [ $started -gt 0 ] && [ $SECONDS -gt $started ]; then
      rshn "[ -f $(q "$B/$name/run.log") ]" 2>/dev/null ||
        { echo "the run did not start:"; rshn "cat $(q "$B/$name.out")" 2>/dev/null; exit 1; }
      started=0
    fi
    if ! rshn true 2>/dev/null; then
      [ $lost = 0 ] && echo "   (the handheld is not answering; the run goes on there, waiting for it)"; lost=1
    fi
    [ $SECONDS -gt $deadline ] && { echo "no end of the run after an hour (see $B/$name.out on the handheld)"; exit 1; }
    sleep 10
  done
  mkdir -p "$OUT" && rm -rf "${OUT:?}/$name"
  dev_tar_out "$B" "$(q "$name" "$name.out")" | tar -xf - -C "$OUT" || { echo "fetching the results failed"; exit 1; }
  mv -f "$OUT/$name.out" "$OUT/$name/" 2>/dev/null
  echo "   $(cat "$OUT/$name/result" 2>/dev/null || echo 'no result')"
  names+=("$OUT/$name")
done
echo "== results in $OUT"
"${PYTHON:-python3}" -I "$PU/tools/analysis/benchsum.py" ${SYMS:+--syms "$SYMS"} "${names[@]}"
