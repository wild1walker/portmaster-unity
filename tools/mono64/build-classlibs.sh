#!/bin/bash
# Unity's .NET 4.x class libraries (the unityjit profile) built from Unity's own Mono sources, for a
# .NET 3.5 game on the Mono route of the 64-bit IL2CPP player (docs/mono-arm64.md, "A .NET 3.5 game
# on the 4.x class libraries"): its desktop build ships the 2.0 profile's libraries, which a 4.x
# runtime cannot run, so these replace them (mscorlib, System, System.Core, System.Xml, ...).
#   tools/mono64/build-classlibs.sh <unity version or line> <work dir>
#     e.g. tools/mono64/build-classlibs.sh 2017.4.40f1 ~/mono64   -> <work>/out/unity-2017.4-mbe/classlibs/
# Output (classlibs/): the profile's assemblies (*.dll, Facades/), etc/mono/config (Mono's Linux
# dllmaps) and etc/mono/4.5/machine.config, LICENSE.txt (their licences), SOURCE.txt. The game's
# setup picks the ones its assemblies reference (with their own references).
# As Unity's build.pl builds them (configure --with-unityjit=yes, then make PROFILE=unityjit in mcs):
# the whole tree of the branch's commit (the one build-mono.sh built the runtime from, if it ran
# first; else MONO_REF or the branch head) without the tests, the submodules the class libraries
# compile from (bdwgc, cecil, ikvm, corert, corefx; tests left out) and Mono's bootstrap compiler
# (monolite of the tree's corlib version, from download.mono-project.com, as Mono's own Makefile
# fetches it: `make get-monolite-latest`). Everything is streamed into a docker volume
# (ua-mono64-cl-<line>, about 1.6 GB), nothing large on the host's disk. The build runs in the
# ua-mono64-build image (arm64: native on an Apple-silicon Mac or an arm64 PC), 15 - 25 minutes.
# Some assemblies of the profile need submodules left out (System.Web, WCF, ...) and are not built:
# make -k. Unity's Mono is MIT-licensed (THIRD_PARTY.md); the class libraries are Mono's
# (MIT, mcs/class/LICENSE) and Microsoft's reference source (MIT).
set -e
V=${1:?usage: build-classlibs.sh <unity version or line> <work dir>}
W=${2:?usage: build-classlibs.sh <unity version or line> <work dir>}
PROFILE=${PROFILE:-unityjit}
HERE=$(cd "$(dirname "$0")" && pwd); PU=$(cd "$HERE/../.." && pwd)
LINE=$(echo "$V" | sed -E 's/^([0-9]+\.[0-9]+).*/\1/')
B=unity-$LINE-mbe
mkdir -p "$W/out/$B/classlibs"
W=$(cd "$W" && pwd); O=$W/out/$B/classlibs
PY=${PYTHON:-python3}
VOL=${MONO64_CL_VOLUME:-ua-mono64-cl-$LINE}    # (the docker volume with the tree and the build)
api() { curl -sfL "https://api.github.com/repos/$1/contents/$2?ref=$3" | "$PY" -I -c 'import json, sys; print(json.load(sys.stdin)["sha"])'; }
tarball() { curl -sfL "https://codeload.github.com/$1/tar.gz/$2"; }
commit() { curl -sfL "https://api.github.com/repos/$1/commits/$2" | "$PY" -I -c 'import json, sys; print(json.load(sys.stdin)["sha"])'; }
docker image inspect ua-build-arm64 >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-build-arm64 "$PU/runtime/docker/arm64"
docker image inspect ua-mono64-build >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-mono64-build "$HERE"
dr() { docker run --rm -i --platform linux/arm64 -v "$VOL":/b ua-mono64-build bash -c "$1"; }
docker volume create "$VOL" >/dev/null
# the runtime's commit (build-mono.sh's SOURCE.txt), so runtime and class libraries are one tree
REF=${MONO_REF:-$(sed -n 's/^[^:]*: mono \([0-9a-f]*\),.*/\1/p' "$W/src/$B/.fetched" 2>/dev/null || true)}
if ! dr "test -f /b/src/.fetched"; then
  REF=${REF:-$(commit Unity-Technologies/mono "$B")} || { echo "cannot resolve $B on GitHub"; exit 1; }
  echo "fetching Unity-Technologies/mono $B at $REF (the whole tree without tests)"
  X='--exclude=*/mcs/tests --exclude=*/mcs/errors --exclude=*/mcs/docs --exclude=*/acceptance-tests --exclude=*/mono/tests --exclude=*/Test/* --exclude=*/netcore'
  tarball Unity-Technologies/mono "$REF" | dr "rm -rf /b/src; mkdir -p /b/src; tar xz -C /b/src --strip-components=1 $X"
  for sm in external/bdwgc:Unity-Technologies/bdwgc external/cecil:mono/cecil external/ikvm:mono/ikvm-fork \
            external/corert:mono/corert external/corefx:mono/corefx; do
    p=${sm%%:*}; repo=${sm#*:}
    SHA=$(api Unity-Technologies/mono "$p" "$REF") || { echo "$p: no submodule at $REF"; continue; }
    echo "  $p: $repo $SHA"
    tarball "$repo" "$SHA" | dr "rm -rf /b/src/$p; mkdir -p /b/src/$p; tar xz -C /b/src/$p --strip-components=1 --exclude='*/tests/*' --exclude='*/Tests/*'"
    if [ "$p" = external/bdwgc ]; then
      AO=$(api Unity-Technologies/bdwgc libatomic_ops "$SHA" || true)
      [ -n "$AO" ] && tarball Unity-Technologies/libatomic_ops "$AO" | dr "mkdir -p /b/src/$p/libatomic_ops; tar xz -C /b/src/$p/libatomic_ops --strip-components=1"
    fi
  done
  # the left-out test folders configure.ac names: empty stand-ins (as build-mono.sh)
  awk '/<<.EOF.$/{f=1;next} /^EOF$/{f=0} f' "$HERE/build-mono.sh" | dr "python3 -I - /b/src"
  echo "$B: mono $REF" | dr "cat > /b/src/.fetched"
fi
CV=$(dr 'cd /b/src && V=$(sed -n "s/^AC_INIT(mono, \[\([0-9.]*\)\].*/\1/p" configure.ac) && C=$(sed -n "s/^MONO_CORLIB_COUNTER=//p" configure.ac) && IFS=. read a b c <<< "$V" && printf "1%02d%02d%02d%03d" $a $b ${c:-0} ${C:-0}')
if ! dr "test -f /b/src/mcs/class/lib/monolite/$CV/mscorlib.dll"; then
  echo "Mono's bootstrap compiler: monolite $CV"
  curl -sfL "https://download.mono-project.com/monolite/monolite-$CV-latest.tar.gz" |
    dr "mkdir -p /b/src/mcs/class/lib/monolite && cd /b/src/mcs/class/lib && tar xz && rm -rf monolite/$CV && mv -f monolite-* monolite/$CV" ||
    { echo "no monolite $CV on download.mono-project.com"; exit 1; }
fi
docker run --rm --platform linux/arm64 -e PROFILE="$PROFILE" -v "$VOL":/b -v "$O":/out ua-mono64-build bash -c '
set -e
cd /b/src; mkdir -p /b/log
export CFLAGS="-fPIC -O2 -g"
GC=--enable-boehm; grep -q "AC_ARG_ENABLE(bdwgc" configure.ac && GC=--disable-boehm
if [ ! -f config.status ]; then
  NOCONFIGURE=1 ./autogen.sh > /b/log/autogen.log 2>&1 || { tail -30 /b/log/autogen.log; exit 1; }
  ./configure --prefix=/b/inst --host=aarch64-pc-linux-gnu --with-mcs-docs=no --with-glib=embedded --disable-nls \
    --disable-btls --with-monotouch=no --disable-parallel-mark --with-sgen=no $GC --with-unityjit=yes --with-csc=mcs \
    > /b/log/configure.log 2>&1 || { tail -40 /b/log/configure.log; exit 1; }
fi
# the runtime and its mono executable, which runs the compiler (Mono 5 has eglib at the top)
if [ ! -x mono/mini/mono ]; then
  for d in external/bdwgc eglib mono/eglib mono/arch mono/utils mono/cil mono/zlib mono/metadata mono/mini; do
    [ -f $d/Makefile ] || continue
    make -j$(nproc) -C $d >> /b/log/runtime.log 2>&1 || { grep -n -B3 -A12 -m2 " error\|No rule" /b/log/runtime.log | head -60; exit 1; }
  done
fi
# basic and build profiles (the compiler, with monolite), then the profile, as Unity'"'"'s build.pl
make -k -C mcs NO_DIR_CHECK=1 PROFILES=$PROFILE all-profiles > /b/log/mcs.log 2>&1 || echo "(some assemblies of the profile were not built: /b/log/mcs.log)"
L=mcs/class/lib/$PROFILE
[ -f $L/mscorlib.dll ] || { grep -n -B3 -A8 -m3 "error" /b/log/mcs.log | head -60; exit 1; }
rm -rf /out/*; mkdir -p /out/Facades /out/etc/mono/4.5
cp $L/*.dll /out/; cp $L/Facades/*.dll /out/Facades/ 2>/dev/null || true
cp data/config /out/etc/mono/config; cp data/net_4_5/machine.config /out/etc/mono/4.5/machine.config
# their licences: Mono (MIT; PATENTS.TXT), the class libraries (mcs/class: MIT X11), the parts
# compiled from Microsoft'"'"'s reference source, corefx and CoreRT (MIT)
{ cat LICENSE; printf "\n\n"; cat PATENTS.TXT; printf "\n\n== mcs/class\n\n"; cat mcs/class/LICENSE
  for f in mcs/class/referencesource/LICENSE.txt external/corefx/LICENSE* external/corert/LICENSE*; do
    [ -f "$f" ] && { printf "\n\n== %s\n\n" "${f%/*}"; cat "$f"; }
  done; } > /out/LICENSE.txt
echo "$(cat .fetched), profile $PROFILE" > /out/SOURCE.txt
echo "$(ls /out/*.dll | wc -l) assemblies, $(ls /out/Facades | wc -l) facades: $(du -sh /out | cut -f1)"'
echo "-> $O"
