#!/bin/sh
# prof.c's unit test (test_prof.c: a fake UnityMain, render thread and Mono; scenarios on, req, off),
# the sampling cost ("spin": work done with no sampling, at 1000 and 500 Hz; only meaningful natively,
# not under QEMU), then `pmu-dev profsum` (setup-rs/tools/pmu-dev.sh, Docker) on the profile it wrote.
# Built and run in ua-build-arm64. The binary also runs on a handheld: out/arm64/test/test_prof on <dir>.
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-arm64}
[ "$ARCH" = arm64 ] || { echo "ARCH must be arm64 (armhf was removed: tag last-armhf)"; exit 1; }
build='set -e
  mkdir -p $O/prof
  $CC -O2 -g -funwind-tables -Wall -Wextra -rdynamic -I../jni/include -I../jni -o $O/test_prof test/test_prof.c prof.c -lpthread -ldl'
run='for s in on req off; do rm -rf $O/prof/$s && mkdir -p $O/prof/$s && $O/test_prof $s $O/prof/$s; done
  mkdir -p $O/prof/spin; for hz in 0 1000 0 1000 500; do $O/test_prof spin $O/prof/spin $hz; done'
O=out/arm64/test
docker run --rm --platform linux/arm64 -v "$PWD/..":/w -w /w/host ua-build-arm64 sh -c "O=$O CC=gcc; $build; $run"
out=$(../../setup-rs/tools/pmu-dev.sh profsum "$PWD/$O/prof/on/prof-1.txt" --top 8)
printf '%s\n' "$out" | head -40
printf '%s\n' "$out" | grep -q 'M:Fake.Jit:Loop' && printf '%s\n' "$out" | grep -q 'spin_inner' ||
  { echo "profsum: FAILED (no spin_inner / M:Fake.Jit:Loop in the summary)"; exit 1; }
echo "profsum: ok"
