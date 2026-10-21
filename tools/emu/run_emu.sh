#!/bin/sh
# Inside the ua-emu-armhf container (see tools/emu/emu.sh): the device layout under /emu, with the
# APK/OBB from the data tool's zip dir (/zip), host binaries from /hostbin. Extra env passes through.
# run_emu.sh <log> <seconds> [release|dev]
cd /emu || exit 1
for p in /zip/*; do
  f=$(basename "$p")
  [ -e "$f" ] || [ -L "$f" ] || ln -s "$p" "$f"
done
# an APK variant (A/B): the .apk in /zip2 replaces the default one
for p in /zip/*.apk; do
  f=$(basename "$p")
  if [ -e "/zip2/$f" ]; then ln -sfn "/zip2/$f" "$f"; else ln -sfn "$p" "$f"; fi
done
cp /hostbin/ua_host ./ua_host
LOG=${1:-emu.log}; SECS=${2:-600}; FLAVOR=${3:-release}
[ "$FLAVOR" = dev ] && export UA_LIBDIR=/emu/devlib
export UA_EGL_PBUFFER=1 ANDROID_WINDOW_SIZE=${ANDROID_WINDOW_SIZE:-720x480} EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1
export SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy}
timeout -k 20 -s INT "$SECS" ./ua_host /emu > "$LOG" 2>&1   # (-k: a hung shutdown)
echo "exit $?" >> "$LOG"
