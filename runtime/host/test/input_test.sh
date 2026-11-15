#!/bin/sh
# input.c's d-pad hold (test_input.c: an SDL virtual gamepad, stand-ins for Unity and the frame count),
# aarch64, SDL 2.26 (ua-display-test-sway, made by display_test.sh).
set -e
cd "$(dirname "$0")/../.."
docker run --rm --platform linux/arm64 -v "$PWD":/w:ro -w /w ua-display-test-sway sh -c '
  set -e
  gcc -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Ijni -o /tmp/test_input host/test/test_input.c host/input.c -lSDL2
  SDL_VIDEODRIVER=dummy /tmp/test_input
  SDL_VIDEODRIVER=dummy /tmp/test_input nohold'
