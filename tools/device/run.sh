#!/bin/bash
# Device test run of ua_host (copy next to ua_host): run.sh <log> [seconds] [release|dev]
# Silent by default; UA_AUDIO=1 plays sound.
# The Development player (devlib/) logs much more; release is lib/. Extra env passes through
# (e.g. UA_MONO_TRACE=1 FJ_TRACE=1 UA_UNITY_ARGS=...).
cd "$(dirname "$0")" || exit 1
LOG=${1:-run.log}; SECS=${2:-40}; FLAVOR=${3:-release}
ROOT=$PWD
[ "$FLAVOR" = dev ] && export UA_LIBDIR=$ROOT/devlib
# silent by default (SDL's dummy driver still consumes audio in real time); UA_AUDIO=1 for sound
if [ "$UA_AUDIO" = 1 ]; then export SDL_AUDIODRIVER=alsa AUDIODEV=plughw:0,0; else export SDL_AUDIODRIVER=dummy; fi
# the pad's SDL GameController mapping: PortMaster's database for this firmware (a PortMaster
# launcher started from EmulationStation gets it through get_controls instead)
PM=${XDG_DATA_HOME:-$HOME/.local/share}/PortMaster
[ -z "$SDL_GAMECONTROLLERCONFIG" ] && [ -f "$PM/knulli/gamecontrollerdb.txt" ] &&
  export SDL_GAMECONTROLLERCONFIG_FILE="$PM/knulli/gamecontrollerdb.txt"
# package name, build id and APK: from android_package.json beside the APK (or UA_PACKAGE, UA_META, UA_APK_NAME)
LD_LIBRARY_PATH=/lib32:/usr/lib32 ./ua_host "$ROOT" > "$LOG" 2>&1 &
pid=$!
for _ in $(seq "$SECS"); do kill -0 $pid 2>/dev/null || break; sleep 1; done
if kill -0 $pid 2>/dev/null; then kill -INT $pid; sleep 3; kill -0 $pid 2>/dev/null && kill -KILL $pid; fi
wait $pid 2>/dev/null; echo "exit $?" >> "$LOG"
