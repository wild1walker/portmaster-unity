#!/bin/bash
# PC side: test a packed game on a handheld over SSH, and bring back what happened.
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
#                     a ControlMaster connection opened beforehand (see SSH_OPTS).
#   SSH_OPTS="..."    extra ssh/scp options, e.g. "-o ControlPath=~/.ssh/cm-%r@%h" to reuse an open
#                     ControlMaster connection
#   REMOTE=<dir>      the folder on the handheld (default: uatest/<game folder's name> in the login's
#                     home directory)
#   ARCH=armhf|arm64  which ua_host to send: runtime/host/out/ua_host (armhf, the default) or
#                     runtime/host/out/arm64/ua_host
#   HOST_BIN=<file>   another ua_host build to send instead
#   OUT=<dir>         where results go (default: devtest-<game folder's name>/ in the current folder)
#   STOP_ES=1         stop EmulationStation during the run and start it again afterwards (Knulli and
#                     Batocera: /etc/init.d/S31emulationstation); it holds GPU memory and CPU time
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
# shellcheck disable=SC2206
OPTS=(${SSH_OPTS:-})
rsh() { ssh ${OPTS[@]+"${OPTS[@]}"} "$DEVICE" "$@"; }         # with stdin (tar streams)
rshn() { ssh -n ${OPTS[@]+"${OPTS[@]}"} "$DEVICE" "$@"; }     # without
q() { [ $# = 0 ] || printf '%q ' "$@"; }
# macOS tar (bsdtar) would add its metadata (._ files, xattr headers) to the archive
TARC=(); tar --version 2>/dev/null | grep -q bsdtar && TARC=(--no-xattrs --no-mac-metadata)

echo "== copying to $DEVICE:$REMOTE"
remote_sizes=$(rshn "mkdir -p $(q "$REMOTE") && cd $(q "$REMOTE") && find . -type f ! -name 'shot-*' ! -name '*.log' -exec stat -c '%s %Y %n' {} ';'") || {
  echo "cannot reach $DEVICE (ssh must work without a password prompt)"; exit 1; }
changed=$(cd "$GAME" && REMOTE_SIZES="$remote_sizes" python3 -I -c '
import os
have = {}
for line in os.environ["REMOTE_SIZES"].splitlines():
    size, mtime, name = line.split(" ", 2)
    have[os.path.normpath(name)] = (int(size), int(mtime))
for top, dirs, files in os.walk("."):
    dirs[:] = [d for d in dirs if not d.startswith(".")]
    for f in files:
        p = os.path.normpath(os.path.join(top, f))
        if f.startswith(".") or f.startswith("shot-") or f.endswith(".log"):
            continue
        st = os.stat(p)
        if have.get(p) != (st.st_size, int(st.st_mtime)):
            print(p)')
if [ -n "$changed" ]; then
  n=$(printf '%s\n' "$changed" | wc -l | tr -d ' ')
  mb=$(cd "$GAME" && printf '%s\n' "$changed" | python3 -I -c 'import os,sys; print(sum(os.path.getsize(l.strip()) for l in sys.stdin) >> 20)')
  echo "   $n changed file(s), $mb MB"
  (cd "$GAME" && printf '%s\n' "$changed" | COPYFILE_DISABLE=1 tar ${TARC[@]+"${TARC[@]}"} -cf - -T -) | rsh "cd $(q "$REMOTE") && tar -xof -" ||
    { echo "copy failed"; exit 1; }
else
  echo "   up to date"
fi
COPYFILE_DISABLE=1 tar ${TARC[@]+"${TARC[@]}"} -cf - -C "$(dirname "$BIN")" "$(basename "$BIN")" -C "$HERE" uatest.sh |
  rsh "cd $(q "$REMOTE") && tar -xof - && { [ $(q "$(basename "$BIN")") = ua_host ] || mv -f $(q "$(basename "$BIN")") ua_host; } && chmod +x ua_host uatest.sh" ||
  { echo "copying ua_host failed"; exit 1; }

# the game's settings: UA_*, SDL_*, GC_*, MONO_* and DEV_ENV's prefixes
envs=()
for v in $(compgen -e); do
  for p in UA_ SDL_ GC_ MONO_ ${DEV_ENV:-}; do [[ $v == "$p"* ]] && envs+=("$v=${!v}") && break; done
done

ES=/etc/init.d/S31emulationstation
if [ "${STOP_ES:-0}" = 1 ]; then
  echo "== stopping EmulationStation"
  rshn "[ -x $ES ] && $ES stop >/dev/null 2>&1; sleep 2" || true
  trap 'rshn "[ -x $ES ] && $ES start >/dev/null 2>&1"; echo "== EmulationStation started again"' EXIT
fi

echo "== running for ${SECS} s${*:+, screenshots at $* s}"
rshn "cd $(q "$REMOTE") && env $(q "${envs[@]+"${envs[@]}"}")./uatest.sh . $(q "$SECS" "$@")"

echo "== results in $OUT"
mkdir -p "$OUT" && rm -f "$OUT"/shot-*.ppm "$OUT"/shot-*.png
rshn "cd $(q "$REMOTE") && tar -cf - \$(ls run.log mem.log shot-*.ppm 2>/dev/null)" | tar -xf - -C "$OUT" ||
  { echo "fetching the results failed"; exit 1; }
python3 -I - "$OUT" <<'EOF'
import glob, os, re, sys
out = sys.argv[1]
log = open(os.path.join(out, 'run.log'), errors='replace').read()
fps = [m.groups() for m in re.finditer(r'\[fps\] ([\d.]+) fps, frame [\d.]+ avg / ([\d.]+) p99 .* \((\d+) frames, \d+ total, (\d+) s\)(?: rss (\d+) MB)?', log)]
if fps:
    # the first window holds the load: leave it out of the average when there are others
    w = fps[1:] if len(fps) > 1 else fps
    frames = sum(int(f[2]) for f in w); secs = sum(int(f[2]) / float(f[0]) for f in w if float(f[0]) > 0)
    print(f'frames:  {frames / secs:.1f} fps average after the first window, lowest window {min(float(f[0]) for f in w):.1f} fps, '
          f'worst p99 {max(float(f[1]) for f in w):.1f} ms ({len(fps)} windows)')
else:
    print('frames:  no [fps] lines: the game did not reach its first frames (see run.log)')
mem = open(os.path.join(out, 'mem.log')).read() if os.path.exists(os.path.join(out, 'mem.log')) else ''
avail = [int(x) for x in re.findall(r'avail=(\d+)M', mem)]; rss = [int(x) for x in re.findall(r'rss=(\d+)M', mem)]
if avail:
    print(f'memory:  game up to {max(rss or [0])} MB resident; the system had {min(avail)} MB available at the lowest')
end = re.findall(r'^exit (-?\d+)$', log, re.M); code = end[-1] if end else '?'
crash = re.findall(r'^.*(?:\[crash\]|FORTIFY|abort message|Fatal signal|SIGSEGV).*$', log, re.M)
if 'LOW MEMORY' in log:
    ended = 'stopped by the memory watchdog: the game needs less memory (e.g. smaller textures, textures.scale)'
elif crash:
    ended = f'crashed (exit {code}): ' + crash[0][:200]
elif '[uatest] ran the full' in log:
    ended = 'ran the whole time (stopped at the end)'
else:
    m = re.search(r'exited on its own after (\d+) s', log)
    ended = f'the game exited on its own{" after " + m.group(1) + " s" if m else ""}, exit {code} (see run.log)'
print('ended:   ' + ended)
shots = sorted(glob.glob(os.path.join(out, 'shot-*.ppm')), key=lambda f: int(re.findall(r'\d+', os.path.basename(f))[0]))
try:
    from PIL import Image
    for f in shots:
        Image.open(f).save(f[:-4] + '.png'); os.remove(f)
    shots = [f[:-4] + '.png' for f in shots]
except ImportError:
    pass
if shots:
    print('shots:   ' + ' '.join(os.path.basename(f) for f in shots))
EOF
