#!/bin/sh
# wwplay.c's unit test (test_wwplay.c: a made-up bank, mixed and measured) and a render with
# wwrender of the same bank to a WAV file. Built and run in ua-build-arm64.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-arm64}
[ "$ARCH" = arm64 ] || { echo "ARCH must be arm64 (armhf was removed: tag last-armhf)"; exit 1; }
SCRIPT='0 bank Test
0 post #130
0.2 post #100
0.5 post #103
1.0 post #131
2.0 post #132
2.5 post #133
3.0 end'
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
