#!/bin/sh
# Tests the SDL display backend (host/display*.c + android/egl_glue.c) without a device: builds
# test_display for aarch64 and runs it, natively on Apple Silicon / arm64 Linux, on Mesa llvmpipe:
#   SDL 2.0.10 (ua-display-test: ua-build-arm64 + Xvfb + weston, Ubuntu 20.04)
#     X11       Xvfb, fullscreen and windowed; the presented frame is read back from the window
#     Wayland   weston's headless backend, UA_DISPLAY=auto
#     fallback  no usable SDL video driver: UA_DISPLAY=auto takes the fbdev path
#   SDL 2.26 (ua-display-test-sway: Debian bookworm + sway + Xvfb)
#     Wayland   sway (wlroots' headless backend) at 1920x1080, as on ROCKNIX; vsync on and off;
#               a window sway tiles to another size
#     X11, offscreen
# Ubuntu's SDL 2.0.10 has neither the offscreen nor the KMSDRM driver; KMSDRM needs a DRM device.
# (SDL 2.26 crashes in SDL_Init against weston's seatless headless backend, hence sway there.)
set -e
cd "$(dirname "$0")/../.."
docker image inspect ua-build-arm64 >/dev/null 2>&1 || docker build -t ua-build-arm64 docker/arm64
docker image inspect ua-display-test >/dev/null 2>&1 || docker build -t ua-display-test - <<'EOF'
FROM ua-build-arm64
RUN apt-get update && apt-get install -y --no-install-recommends xvfb weston && rm -rf /var/lib/apt/lists/*
EOF
docker image inspect ua-display-test-sway >/dev/null 2>&1 || docker build -t ua-display-test-sway - <<'EOF'
FROM debian:bookworm
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gcc libc6-dev \
      libsdl2-dev libegl-dev libgles-dev libegl-mesa0 libgl1-mesa-dri sway xvfb && rm -rf /var/lib/apt/lists/* && \
    useradd -m u
EOF
SRC="host/test/test_display.c host/display.c host/display_sdl.c host/fbdev.c android/egl_glue.c android/native_window.c android/log_shim.c"
CFLAGS="-O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter"

echo "##### SDL 2.0.10 (Ubuntu 20.04)"
docker run --rm --platform linux/arm64 -v "$PWD":/w:ro -w /w -e SRC="$SRC" -e CFLAGS="$CFLAGS" ua-display-test sh -c '
  set -e
  gcc $CFLAGS -o /tmp/test_display $SRC -lSDL2 -ldl -lpthread
  export LIBGL_ALWAYS_SOFTWARE=1 XDG_RUNTIME_DIR=/tmp/xdg
  mkdir -p -m 700 $XDG_RUNTIME_DIR
  run() { echo "== $*"; env "$@" timeout 180 /tmp/test_display; }

  Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
  sleep 1
  run DISPLAY=:99 SDL_VIDEODRIVER=x11 UA_DISPLAY=sdl TEST_SIZE=1280x720
  run DISPLAY=:99 SDL_VIDEODRIVER=x11 UA_DISPLAY=sdl UA_DISPLAY_WINDOW=640x360 TEST_SIZE=640x360

  weston --backend=headless-backend.so --width=1280 --height=720 --socket=wayland-1 --idle-time=0 >/tmp/weston.log 2>&1 &
  sleep 2
  run WAYLAND_DISPLAY=wayland-1 SDL_VIDEODRIVER=wayland UA_DISPLAY=auto TEST_SIZE=1280x720 || { cat /tmp/weston.log; exit 1; }

  run SDL_VIDEODRIVER=none UA_DISPLAY=auto TEST_EXPECT=fbdev'

echo "##### SDL 2.26 (Debian bookworm)"
docker run --rm --platform linux/arm64 -v "$PWD":/w:ro -w /w -e SRC="$SRC" -e CFLAGS="$CFLAGS" ua-display-test-sway sh -c '
  set -e
  gcc $CFLAGS -o /tmp/test_display $SRC -lSDL2 -ldl -lpthread
  mkdir -p -m 700 /tmp/xdg && chown u /tmp/xdg
  # sway refuses to run as root
  as_u() { runuser -u u -- env XDG_RUNTIME_DIR=/tmp/xdg LIBGL_ALWAYS_SOFTWARE=1 "$@"; }
  run() { echo "== $*"; as_u "$@" timeout 180 /tmp/test_display; }
  printf "output HEADLESS-1 resolution 1920x1080\n" >/tmp/sway.cfg
  as_u WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman sway -c /tmp/sway.cfg >/tmp/sway.log 2>&1 &
  sleep 3
  W="WAYLAND_DISPLAY=wayland-1 SDL_VIDEODRIVER=wayland"
  run $W UA_DISPLAY=auto TEST_SIZE=1920x1080 || { cat /tmp/sway.log; exit 1; }
  run $W UA_DISPLAY=sdl UA_SWAP_INTERVAL=0 TEST_SIZE=1920x1080
  run $W UA_DISPLAY=sdl UA_DISPLAY_WINDOW=640x360          # tiled by sway to its own size

  Xvfb :98 -screen 0 1280x720x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
  sleep 1
  run DISPLAY=:98 SDL_VIDEODRIVER=x11 UA_DISPLAY=sdl TEST_SIZE=1280x720
  run SDL_VIDEODRIVER=offscreen UA_DISPLAY=sdl UA_DISPLAY_WINDOW=800x600 TEST_SIZE=800x600'
echo "display tests passed"
