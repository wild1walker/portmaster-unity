#!/bin/bash
# test/run.sh <mesa tree> <out dir> [glreplay dir]: the build checked off the device, in ua-mesa-build24.
# fake_kbase.c stands in for /dev/mali0 (LD_PRELOAD: kbase's ioctls, memory, atoms completed at once, no
# sync files, any DRM ioctl on the node counted as "foreign"); mesa_smoke draws, reads back and swaps 30
# frames on UK 11.17 (H700) and 11.7 (RK3326), and with PAN_KBASE_CACHED_WINDOW=0; with a glreplay
# (runtime/host/out/arm64), glreplay --info prints what Mesa tells Unity. No GPU runs anything: this checks
# that the paths run (no crash, no hang, no GL error, no DRM ioctl on the kbase node), not the picture.
set -e
SRC=$(cd "${1:?mesa tree}" && pwd); OUT=$(cd "${2:?out dir}" && pwd); HERE=$(cd "$(dirname "$0")" && pwd)
H=${3:+$(cd "$3" && pwd)}
docker run --rm --pull=never --platform linux/arm64 -v "$SRC":/src:ro -v "$OUT":/o:ro -v "$HERE":/t:ro ${H:+-v "$H":/h:ro} \
  ua-mesa-build24 bash -c '
  set -e
  gcc -O1 -g -Wall -shared -fPIC -I/src/src/panfrost/lib/kmod/kbase -o /tmp/fake_kbase.so /t/fake_kbase.c -ldl -lpthread
  gcc -O1 -g -Wall -I/src/include -o /tmp/mesa_smoke /t/mesa_smoke.c -L/o -Wl,-rpath-link,/o -lEGL -lGLESv2
  export LD_LIBRARY_PATH=/o EGL_PLATFORM=surfaceless MESA_KBASE_NODE=/dev/mali0
  for uk in 17 7; do
    echo "== UK 11.$uk"
    FAKE_KBASE_UK_MINOR=$uk LD_PRELOAD=/tmp/fake_kbase.so timeout 120 /tmp/mesa_smoke 30 2>&1 | tee /tmp/r.txt
    grep -q "ok mesa_smoke" /tmp/r.txt && grep -q " 0 foreign ioctls" /tmp/r.txt
  done
  echo "== PAN_KBASE_CACHED_WINDOW=0"
  PAN_KBASE_CACHED_WINDOW=0 LD_PRELOAD=/tmp/fake_kbase.so timeout 120 /tmp/mesa_smoke 5
  if [ -x /h/glreplay ]; then
    echo "== glreplay --info"
    GLREP_EGL=/o/libEGL.so.1 GLREP_GLES=/o/libGLESv2.so.2 GLREP_PLATFORM=surfaceless LD_PRELOAD=/tmp/fake_kbase.so \
      timeout 60 /h/glreplay --info
  fi'
