#!/bin/sh
# prof.c's unit test (test_prof.c: a fake UnityMain, render thread and Mono; scenarios on, req, off),
# the sampling cost ("spin": work done with no sampling, at 1000 and 500 Hz; only meaningful natively,
# not under QEMU), then tools/analysis/profsum.py on the profile it wrote (when python3 is here).
# ARCH=armhf (default): built in ua-host-armhf, run in an arm/v7 container (QEMU on most PCs);
# ARCH=arm64: built and run in ua-build-arm64. The binary also runs on a handheld:
# out/test/test_prof on <dir> (LD_LIBRARY_PATH=/lib32:/usr/lib32 for the 32-bit one).
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-armhf}
build='set -e
  mkdir -p $O/prof
  $CC -O2 -g -funwind-tables -Wall -Wextra -rdynamic -I../jni/include -I../jni -o $O/test_prof test/test_prof.c prof.c -lpthread -ldl'
run='for s in on req off; do rm -rf $O/prof/$s && mkdir -p $O/prof/$s && $O/test_prof $s $O/prof/$s; done
  mkdir -p $O/prof/spin; for hz in 0 1000 0 1000 500; do $O/test_prof spin $O/prof/spin $hz; done'
if [ "$ARCH" = arm64 ]; then
  O=out/arm64/test
  docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=$O CC=gcc; $build; $run"
else
  O=out/test
  docker run --rm -v "$PWD/..":/w -w /w/host ua-host-armhf sh -c "O=$O CC=arm-linux-gnueabihf-gcc; $build"
  docker run --rm --platform linux/arm/v7 -v "$PWD/..":/w -w /w/host debian:bookworm-slim sh -c "set -e; O=$O; $run"
fi
if command -v python3 >/dev/null 2>&1; then
  out=$(python3 ../../tools/analysis/profsum.py "$O/prof/on/prof-1.txt" --top 8)
  printf '%s\n' "$out" | head -40
  printf '%s\n' "$out" | grep -q 'M:Fake.Jit:Loop' && printf '%s\n' "$out" | grep -q 'spin_inner' ||
    { echo "profsum: FAILED (no spin_inner / M:Fake.Jit:Loop in the summary)"; exit 1; }
  echo "profsum: ok"
fi
