#!/bin/sh
# wwplay.c's unit test (test_wwplay.c: a made-up bank, mixed and measured) and a render with
# wwrender of the same bank to a WAV file. ARCH=armhf (default): built in ua-host-armhf, run in an
# arm/v7 container (Docker's emulation); ARCH=arm64: built and run in ua-build-arm64.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-armhf}
SCRIPT='0 bank Test
0 post #130
0.2 post #100
0.5 post #103
1.0 post #131
2.0 post #132
2.5 post #133
3.0 end'
if [ "$ARCH" = arm64 ]; then
  docker run --rm --platform linux/arm64 -e SCRIPT="$SCRIPT" -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c '
    set -e
    O=out/arm64/test/wwplay; mkdir -p $O
    gcc -O2 -g -Wall -c wwplay.c -o $O/wwplay.o
    gcc -O2 -g -w -c wwplay_vorbis.c -o $O/wwplay_vorbis.o
    gcc -O2 -g -Wall -o $O/test_wwplay test/test_wwplay.c $O/wwplay.o $O/wwplay_vorbis.o -lpthread -lm
    gcc -O2 -g -Wall -o $O/wwrender wwrender.c $O/wwplay.o $O/wwplay_vorbis.o -lpthread -lm
    T=$(mktemp -d)
    $O/test_wwplay $T test/data | grep -v "^  \[wwise\]"
    echo "$SCRIPT" | $O/wwrender -o $O/render.wav $T - 2>/dev/null
    ls -l $O/render.wav'
  exit
fi
docker run --rm -v "$PWD/..":/w -w /w/host ua-host-armhf sh -c '
  set -e
  O=out/test/wwplay; mkdir -p $O
  CC=arm-linux-gnueabihf-gcc
  F="-O2 -g -Wall -march=armv7-a -mfpu=neon -mfloat-abi=hard"
  $CC $F -c wwplay.c -o $O/wwplay.o
  $CC $F -w -c wwplay_vorbis.c -o $O/wwplay_vorbis.o
  $CC $F -o $O/test_wwplay test/test_wwplay.c $O/wwplay.o $O/wwplay_vorbis.o -lpthread -lm
  $CC $F -o $O/wwrender wwrender.c $O/wwplay.o $O/wwplay_vorbis.o -lpthread -lm'
docker run --rm --platform linux/arm/v7 -e SCRIPT="$SCRIPT" -v "$PWD/..":/w -w /w/host debian:bookworm-slim sh -c '
  set -e
  O=out/test/wwplay
  T=$(mktemp -d)
  $O/test_wwplay $T test/data | grep -v "^  \[wwise\]"
  echo "$SCRIPT" | $O/wwrender -o $O/render.wav $T - 2>/dev/null
  ls -l $O/render.wav'
