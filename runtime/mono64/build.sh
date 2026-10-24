#!/bin/sh
# Builds libil2cpp_mono.so (il2cpp_mono.c: the il2cpp_* API on Unity's Mono) for aarch64 glibc in
# the ua-build-arm64 image -> out/arm64/libil2cpp_mono.so. It binds Mono's functions at run time
# (dlsym), so it needs no Mono headers and serves every Unity line's Mono; the runtime itself,
# libmonobdwgc-2.0.so, is built from Unity's Mono sources (tools/mono64/build-mono.sh).
#   test: also builds and runs test/test_il2cpp_mono (the bridge against a stand-in Mono)
set -e
cd "$(dirname "$0")"
mkdir -p out/arm64
docker image inspect ua-build-arm64 >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-build-arm64 ../docker/arm64
docker run --rm --platform linux/arm64 -v "$PWD":/w -w /w ua-build-arm64 sh -c '
  set -e
  gcc -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -fPIC -fvisibility=hidden -shared -Wl,-soname,libil2cpp.so \
    -o out/arm64/libil2cpp_mono.so il2cpp_mono.c -ldl -lpthread
  n=$(nm -D --defined-only out/arm64/libil2cpp_mono.so | grep -c " il2cpp_")
  echo "out/arm64/libil2cpp_mono.so: $n il2cpp_* functions"
  if [ "'"$1"'" = test ]; then
    gcc -O1 -g -Wall -fPIC -shared -o out/arm64/libmonobdwgc-2.0.so test/fake_mono.c
    gcc -O1 -g -Wall -o out/arm64/test_il2cpp_mono test/test_il2cpp_mono.c -ldl -lpthread
    UA_MONO64_RUNTIME=$PWD/out/arm64/libmonobdwgc-2.0.so out/arm64/test_il2cpp_mono out/arm64/libil2cpp_mono.so
    rm -f out/arm64/libmonobdwgc-2.0.so
  fi'
