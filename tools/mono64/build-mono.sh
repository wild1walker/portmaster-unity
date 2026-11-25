#!/bin/bash
# Unity's Mono runtime for aarch64 Linux (glibc), for the Mono route on the 64-bit IL2CPP player
# (docs/mono-arm64.md): the runtime of a Unity line's MonoBleedingEdge, built from Unity's own
# sources (github.com/Unity-Technologies/mono, branch unity-<line>-mbe) as Unity's Linux players
# build it, without the class libraries (the game ships its own, in its Managed folder).
#   tools/mono64/build-mono.sh <unity version or line> <work dir>
#     e.g. tools/mono64/build-mono.sh 2019.4.8f1 ~/mono64     -> <work>/out/unity-2019.4-mbe/
# Output: libmonobdwgc-2.0.so (stripped; .debug.so with symbols), libMonoPosixHelper.so and, for
# Mono 6 (2021.2+), libmono-native.so: they go into the game's lib/ next to the bridge
# (runtime/mono64, installed as lib/libil2cpp.so).
# What it fetches (once, into <work>/src/<branch>): the source tarball of the branch's head commit
# (resolved once and printed; MONO_REF=<commit> builds that commit instead) from GitHub without
# the class libraries, tests and external/ (about 45 MB on disk), then the pinned bdwgc and
# libatomic_ops submodules (and for Mono 6 corefx's src/Native and the branch's corefx-bugfix,
# which libmono-native is built from). The build runs in an arm64 container (ua-mono64-build: the
# ua-build-arm64 image, Ubuntu 20.04 / glibc 2.31, plus autotools), natively on an Apple-silicon
# Mac or an arm64 Linux PC, in a docker volume (ua-mono64); about 5 minutes on 8 cores.
# MONO64_LLVM=1: instead, the AOT compiler with Mono's LLVM back end and LLVM's opt and llc (a measurement:
# no faster than the JIT in Hollow Knight; the block marked MONO64_LLVM below, docs/mono-arm64.md).
# MONO64_MANAGED_ALLOC=1|0: with or without a managed allocator for Boehm (tools/mono64/patches/
# managed-alloc-<line>.patch: 6000.0, on by default, and 2017.4; the source patches below, docs/mono-arm64.md).
# Unity's Mono is MIT-licensed (THIRD_PARTY.md); what this builds stays on the porter's machine and
# goes to the player's device with the port's setup, as Unity's player does.
set -e
V=${1:?usage: build-mono.sh <unity version or line> <work dir>}
W=${2:?usage: build-mono.sh <unity version or line> <work dir>}
HERE=$(cd "$(dirname "$0")" && pwd); PU=$(cd "$HERE/../.." && pwd)
LINE=$(echo "$V" | sed -E 's/^([0-9]+\.[0-9]+).*/\1/')
B=unity-$LINE-mbe
mkdir -p "$W/src" "$W/out/$B"
W=$(cd "$W" && pwd)
D=$W/src/$B
PY=${PYTHON:-python3}
api() { curl -sfL "https://api.github.com/repos/$1/contents/$2?ref=$3" | "$PY" -I -c 'import json, sys; print(json.load(sys.stdin)["sha"])'; }
tarball() { curl -sfL "https://codeload.github.com/$1/tar.gz/$2"; }
commit() { curl -sfL "https://api.github.com/repos/$1/commits/$2" | "$PY" -I -c 'import json, sys; print(json.load(sys.stdin)["sha"])'; }
if [ ! -f "$D/.fetched" ]; then
  rm -rf "$D"; mkdir -p "$D"
  # one commit for every file fetched (the branch may move between the downloads)
  REF=${MONO_REF:-$(commit Unity-Technologies/mono "$B")} || { echo "cannot resolve $B on GitHub"; exit 1; }
  echo "fetching Unity-Technologies/mono $B at $REF"
  tarball Unity-Technologies/mono "$REF" | tar xz -C "$D" --strip-components=1 \
    --exclude='*/mcs/class' --exclude='*/mcs/tests' --exclude='*/mcs/errors' --exclude='*/mcs/docs' \
    --exclude='*/mcs/tools' --exclude='*/mcs/packages' --exclude='*/acceptance-tests' --exclude='*/mono/tests' \
    --exclude='*/netcore' --exclude='*/external/*'
  GC=$(api Unity-Technologies/mono external/bdwgc "$REF")
  mkdir -p "$D/external/bdwgc"; tarball Unity-Technologies/bdwgc "$GC" | tar xz -C "$D/external/bdwgc" --strip-components=1
  AO=$(api Unity-Technologies/bdwgc libatomic_ops "$GC" || true)
  if [ -n "$AO" ]; then
    mkdir -p "$D/external/bdwgc/libatomic_ops"
    tarball Unity-Technologies/libatomic_ops "$AO" | tar xz -C "$D/external/bdwgc/libatomic_ops" --strip-components=1
  fi
  if [ -d "$D/mono/native" ]; then              # Mono 6: libmono-native from corefx's native code
    CF=$(api Unity-Technologies/mono external/corefx "$REF")
    mkdir -p "$D/external/corefx"
    W1=; tar --version 2>/dev/null | grep -q 'GNU tar' && W1=--wildcards      # (bsdtar: patterns as is)
    tarball mono/corefx "$CF" | tar xz $W1 -C "$D/external/corefx" --strip-components=1 '*/src/Native/*' '*/LICENSE.TXT'
    tarball Unity-Technologies/mono "$REF" | tar xz $W1 -C "$D" --strip-components=1 '*/external/corefx-bugfix/*'
  fi
  # the left-out test folders configure.ac and the m4 files name: empty stand-ins
  "$PY" -I - "$D" <<'EOF'
import glob, os, re, sys
s = sys.argv[1]
text = ''.join(open(f, errors='replace').read() for f in [os.path.join(s, 'configure.ac'), os.path.join(s, 'acinclude.m4')]
               + glob.glob(os.path.join(s, 'm4', '*.m4')) if os.path.exists(f))
names = set(re.findall(r'\b((?:mono/tests|acceptance-tests)[\w/.+-]*)', text))
names |= {d + '/Makefile' for d in ('acceptance-tests', 'mono/tests', 'mono/tests/gc-descriptors', 'mono/tests/testing_gac')}
for n in sorted(names):
    n = n[:-3] if n.endswith('.in') else n
    p = os.path.join(s, n)
    base = os.path.basename(n)
    if base == 'Makefile':
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if not os.path.exists(p + '.am'):
            open(p + '.am', 'w').write('# left out of the runtime-only source download\n')
    elif not any(m.startswith(n + '/') for m in names):     # a file configure writes from <name>.in
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if not os.path.exists(p + '.in'):
            open(p + '.in', 'w').write('\n')
EOF
  echo "$B: mono $REF, bdwgc $GC, libatomic_ops $AO${CF:+, corefx $CF}" > "$D/.fetched"
fi
cp "$D/.fetched" "$W/out/$B/SOURCE.txt"         # which commits the libraries were built from
# --- MONO64_LLVM=1: the ahead-of-time compiler with Mono's LLVM back end (docs/mono-arm64.md, "Mono's
# LLVM back end, measured in the game"), built instead of the runtime, into <work>/out/<branch>-llvm/:
# the same runtime configured --enable-llvm (LLVM linked in, for mono_main --aot=llvm) plus LLVM's opt and llc,
# from the LLVM fork the branch names (external/llvm-project: dotnet/llvm-project, LLVM 6.0 with Mono's
# changes), aarch64 back end only, release build, libstdc++ linked in. Its own docker volume
# (ua-mono64-llvm, about 3 GB: LLVM's build tree) and its own source tree, so a plain build is not
# touched; the volume can go once out/ has the files (docker volume rm ua-mono64-llvm). About 70
# minutes on 8 cores. The images it compiles load in the plain runtime (same sources: same version).
# (MONO64_RUNTIME_VOLUME: another docker volume for the build tree, or an absolute path: a folder of the host)
VOL=${MONO64_RUNTIME_VOLUME:-ua-mono64}; OUT=$W/out/$B; LLVMCONF=; LLVMDIR=
case "$VOL" in /*) mkdir -p "$VOL" ;; esac
if [ "${MONO64_LLVM:-0}" = 1 ]; then
  VOL=ua-mono64-llvm; OUT=$W/out/$B-llvm; mkdir -p "$OUT"; cp "$D/.fetched" "$OUT/SOURCE.txt"
  REF=$(sed -nE 's/.*: mono ([0-9a-f]+).*/\1/p' "$D/.fetched")
  LL=$(api Unity-Technologies/mono external/llvm-project "$REF") || { echo "no external/llvm-project at $REF"; exit 1; }
  echo "llvm-project (dotnet/llvm-project) $LL" >> "$OUT/SOURCE.txt"
  docker image inspect ua-build-arm64 >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-build-arm64 "$PU/runtime/docker/arm64"
  docker image inspect ua-mono64-build >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-mono64-build "$HERE"
  if ! docker run --rm -v "$VOL":/b ua-mono64-build test -f /b/llvm-$LL/.fetched; then
    echo "fetching dotnet/llvm-project $LL (llvm/ only, without its tests)"
    tarball dotnet/llvm-project "$LL" | docker run --rm -i --platform linux/arm64 -v "$VOL":/b ua-mono64-build bash -c "
      set -e; rm -rf /b/llvm-$LL; mkdir -p /b/llvm-$LL/src
      tar xz -C /b/llvm-$LL/src --strip-components=2 --wildcards '*/llvm/*' --exclude='*/llvm/test/*' --exclude='*/llvm/unittests/*'
      touch /b/llvm-$LL/.fetched"
  fi
  docker run --rm --platform linux/arm64 -v "$VOL":/b -v "$OUT":/out ua-mono64-build bash -c '
set -e
L=/b/llvm-'"$LL"'
# Unity'"'"'s branch passes llc -disable-fault-maps, an option of a later fork commit than the one it
# names (fault maps are never emitted for aarch64 by LLVM 6): llc accepts it here and ignores it
if ! grep -q disable-fault-maps $L/src/tools/llc/llc.cpp; then
  sed -i "/^static cl::opt<bool> NoVerify/i static cl::opt<bool> DisableFaultMaps(\"disable-fault-maps\", cl::Hidden, cl::desc(\"Accepted for Mono; no effect\"));" $L/src/tools/llc/llc.cpp
  rm -f $L/usr/bin/llc
fi
if [ ! -x $L/usr/bin/llc ]; then
  mkdir -p $L/build; cd $L/build
  # mono/llvm/build.mk'"'"'s options, the aarch64 back end only; no assertions; libstdc++ and libgcc
  # linked in so the tools run on any glibc handheld
  [ -f Makefile ] || cmake -DCMAKE_INSTALL_PREFIX=$L/usr -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_BUILD_TESTS=Off -DLLVM_INCLUDE_TESTS=Off -DLLVM_BUILD_EXAMPLES=Off -DLLVM_INCLUDE_EXAMPLES=Off \
    -DLLVM_INCLUDE_DOCS=Off -DLLVM_TOOLS_TO_BUILD="opt;llc;llvm-config;llvm-dis" -DLLVM_TARGETS_TO_BUILD=AArch64 \
    -DLLVM_ENABLE_ASSERTIONS=Off -DLLVM_ENABLE_LIBXML2=Off -DLLVM_ENABLE_TERMINFO=Off -DLLVM_ENABLE_ZLIB=Off \
    -DHAVE_FUTIMENS=0 -DLLVM_PARALLEL_LINK_JOBS=2 \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" $L/src > /out/llvm-cmake.log 2>&1 || { tail -30 /out/llvm-cmake.log; exit 1; }
  make -j$(nproc) > /out/llvm-make.log 2>&1 || { grep -n -B3 -A10 -m3 "error" /out/llvm-make.log | head -50; exit 1; }
  make install > /out/llvm-install.log 2>&1
fi
strip -o /out/opt $L/usr/bin/opt; strip -o /out/llc $L/usr/bin/llc
cp $L/src/LICENSE.TXT /out/LLVM-LICENSE.TXT
ls -la /out/opt /out/llc'
  # (LLVM is C++: libmono names libstdc++, which libtool's C link leaves out)
  LLVMCONF="--enable-llvm --with-llvm=/b/llvm-$LL/usr LIBS=-lstdc++"; LLVMDIR=llvm
fi
# --- (end of MONO64_LLVM)
# --- Source patches (tools/mono64/patches/<name>-<line>.patch), copied next to the sources; the build
# applies the set to its tree:
#   thread-exit: a thread Mono detached is not unregistered from the collector a second time when it
#     exits (with thread-local free lists, the second time faults); the lines whose collector has them:
#     6000.0 always, 2017.4 with the managed allocator
#   managed-alloc (MONO64_MANAGED_ALLOC=1|0; default 1 for 6000.0, 0 for the others): the JIT's
#     allocations of small objects, arrays and strings off the thread's Boehm free list
#     (docs/mono-arm64.md, "A managed allocator for Boehm")
#   invoke-info (the lines that have one: 2017.4): mono_unity_runtime_invoke_info exported, the runtime-invoke
#     wrapper and compiled code mono_jit_runtime_invoke keeps for a method, for the bridge's cached invoke
#     (UA_MONO64_FAST_INVOKE, docs/mono-arm64.md)
rm -rf "$D/.ua-patches"; mkdir -p "$D/.ua-patches"
MA=${MONO64_MANAGED_ALLOC:-auto}
if [ "$MA" = auto ]; then [ "$LINE" = 6000.0 ] && MA=1 || MA=0; fi
if [ "$MA" = 1 ]; then
  [ -f "$HERE/patches/managed-alloc-$LINE.patch" ] || { echo "MONO64_MANAGED_ALLOC: no patch for the $LINE line"; exit 1; }
  cp "$HERE/patches/managed-alloc-$LINE.patch" "$D/.ua-patches/"
fi
if [ -f "$HERE/patches/thread-exit-$LINE.patch" ] && { [ "$LINE" = 6000.0 ] || [ "$MA" = 1 ]; }; then
  cp "$HERE/patches/thread-exit-$LINE.patch" "$D/.ua-patches/"
fi
[ -f "$HERE/patches/invoke-info-$LINE.patch" ] && cp "$HERE/patches/invoke-info-$LINE.patch" "$D/.ua-patches/"
# --- (end of the source patches)
docker image inspect ua-build-arm64 >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-build-arm64 "$PU/runtime/docker/arm64"
docker image inspect ua-mono64-build >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-mono64-build "$HERE"
docker run --rm --platform linux/arm64 -v "$VOL":/b -v "$D":/src:ro -v "$OUT":/out ua-mono64-build bash -c '
set -e
B='"$B"'
[ -f /b/$B/.copied ] || { rm -rf /b/$B; mkdir -p /b/$B; cp -a /src/. /b/$B/; touch /b/$B/.copied; }
cd /b/$B
# as Unity'"'"'s build.pl builds the Linux players'"'"' runtime (no class libraries, no btls, Boehm,
# no parallel marking), optimized for speed instead of size
export CFLAGS="-fPIC -O2 -g"
# Boehm without its byte at the end of every object (DONT_ADD_BYTE_AT_END; interior pointers are
# still recognised): with 16-byte granules that byte costs a whole granule for every object whose
# size is a multiple of 16 (docs/mono-arm64.md, "the collector'"'"'s layout"). It only keeps an
# object alive through a pointer just past its end, which Mono'"'"'s JIT never needs (SGen does not
# honour one either).
# 8-byte granules (GC_GRANULE_BYTES=8, one word: the 32-bit collector'"'"'s granule and SGen'"'"'s
# alignment on aarch64) for the lines a game has run with them: Mono'"'"'s objects are multiples of 8
# bytes, and bdwgc'"'"'s 64-bit default of 16 rounds every other one up by 8 (Hollow Knight, 6000.0:
# 98.7 -> 91.8 MB live; Enter the Gungeon, 2017.4: the Keep's heap 280 -> 196 - 262 MB, 162 -> 212 MB available
# on muOS, docs/mono-arm64.md). MONO64_GRANULE=8|16 chooses for any line.
# A tree built with other flags has its collector built again.
GCFLAGS="-DDONT_ADD_BYTE_AT_END"
GRANULE='"${MONO64_GRANULE:-auto}"'
if [ "$GRANULE" = auto ]; then case $B in unity-6000.0-mbe|unity-2017.4-mbe) GRANULE=8 ;; *) GRANULE=16 ;; esac; fi
case $GRANULE in
  8) GCFLAGS="$GCFLAGS -DGC_GRANULE_BYTES=8 -DGC_GRANULE_WORDS=1" ;;
  16) ;;
  *) echo "MONO64_GRANULE must be 8 or 16"; exit 1 ;;
esac
# --- The source patches (/src/.ua-patches) applied to the build tree; another set takes the applied one
# out first, and the runtime library is linked again. The collector of the 2017.4 line (bdwgc 7.6) has
# no thread-local free lists on aarch64 Linux unless asked (configure defines THREAD_LOCAL_ALLOC there
# for x86 only): with the managed allocator it is built with them
WANT=$(cd /src/.ua-patches 2>/dev/null && ls *.patch 2>/dev/null | tr "\n" " ")
WANTSUM=$(cd /src/.ua-patches 2>/dev/null && cat $WANT 2>/dev/null | md5sum | cut -c1-12)
if [ "$(cat .ua-patchset 2>/dev/null)" != "$WANT$WANTSUM" ]; then
  for p in $(ls -r .ua-applied 2>/dev/null); do patch -R -p1 -s < .ua-applied/$p || { echo "cannot take out $p"; exit 1; }; done
  rm -rf .ua-applied; mkdir -p .ua-applied
  for p in $WANT; do patch -p1 -s --forward < /src/.ua-patches/$p || { echo "$p does not apply"; exit 1; }; cp /src/.ua-patches/$p .ua-applied/; done
  echo "$WANT$WANTSUM" > .ua-patchset
  rm -f mono/mini/.libs/libmonobdwgc-2.0.so* mono/mini/.libs/libmonoboehm-2.0.so* mono/mini/libmono*-2.0.la
fi
case "$WANT" in *managed-alloc*) [ $B = unity-2017.4-mbe ] && GCFLAGS="$GCFLAGS -DTHREAD_LOCAL_ALLOC" ;; esac
# --- (end of the source patches)
if [ "$(cat .ua-gcflags 2>/dev/null)" != "$GCFLAGS" ]; then
  [ -f external/bdwgc/Makefile ] && make -C external/bdwgc clean > /dev/null 2>&1
  rm -f mono/mini/.libs/libmonobdwgc-2.0.so* mono/mini/.libs/libmonoboehm-2.0.so* mono/mini/libmono*-2.0.la
fi
# the Boehm collector is external/bdwgc: Mono 5 builds it as the "bdwgc" runtime next to an older
# included Boehm ("boehm", libgc/, which does not know aarch64): that one is left out; Mono 6
# calls external/bdwgc "boehm"
GC=--enable-boehm; grep -q "AC_ARG_ENABLE(bdwgc" configure.ac && GC=--disable-boehm
if [ ! -f config.status ]; then
  NOCONFIGURE=1 ./autogen.sh > /out/autogen.log 2>&1 || { tail -30 /out/autogen.log; exit 1; }
  ./configure --prefix=/b/$B-inst --host=aarch64-pc-linux-gnu --disable-mcs-build --with-mcs-docs=no \
    --with-glib=embedded --disable-nls --disable-btls --with-monotouch=no --disable-parallel-mark \
    --with-sgen=no $GC '"$LLVMCONF"' > /out/configure.log 2>&1 || { tail -40 /out/configure.log; exit 1; }
fi
: > /out/make.log
# (eglib: at the top of the tree in Mono 5.0 - 2017.4 - and in mono/ later)
for d in external/bdwgc eglib mono/culture mono/eglib mono/arch mono/utils mono/cil mono/zlib mono/sgen \
         mono/metadata '$LLVMDIR' mono/mini mono/profiler mono/native support; do
  [ -d $d ] && [ -f $d/Makefile ] || continue
  # mono/mini: the runtime library only (its mono executable wants files of the full build)
  if [ $d = mono/mini ]; then make -k -j$(nproc) -C $d >> /out/make.log 2>&1 || true
  elif [ $d = external/bdwgc ]; then
    make -j$(nproc) -C $d CFLAGS="$CFLAGS $GCFLAGS" >> /out/make.log 2>&1 || { grep -n -B3 -A12 -m2 " error" /out/make.log | head -60; exit 1; }
    echo "$GCFLAGS" > .ua-gcflags
  elif [ $d = mono/native ]; then make -j$(nproc) -C $d >> /out/make.log 2>&1 || echo "mono/native: not built (make.log)"
  # (the profilers are not shipped; Mono 5.0'"'"'s want the old Boehm runtime this build leaves out)
  elif [ $d = mono/profiler ]; then make -j$(nproc) -C $d >> /out/make.log 2>&1 || echo "mono/profiler: not built (make.log)"
  else make -j$(nproc) -C $d >> /out/make.log 2>&1 || { grep -n -B3 -A12 -m2 " error\|No rule" /out/make.log | head -60; exit 1; }
  fi
done
L=mono/mini/.libs/libmonobdwgc-2.0.so; [ -f $L ] || L=mono/mini/.libs/libmonoboehm-2.0.so   # (Mono 6 names it so)
[ -f $L ] || { grep -n -B3 -A12 -m2 " error" /out/make.log | head -60; exit 1; }
# what the bridge binds without asking must be there: a runtime linked without Mono'"'"'s collector
# glue starts and then stops at its first collection
for f in mono_jit_init_version mono_runtime_invoke mono_add_internal_call mono_gc_collect mono_gc_get_used_size mono_gc_get_heap_size GC_gcollect; do
  nm -D --defined-only $L | grep -q " T $f\$" || { echo "$L does not export $f: not a complete runtime (make.log)"; exit 1; }
done
cp -L $L /out/libmonobdwgc-2.0.debug.so
strip -o /out/libmonobdwgc-2.0.so /out/libmonobdwgc-2.0.debug.so
for f in support/.libs/libMonoPosixHelper.so mono/native/.libs/libmono-native.so; do
  [ -f $f ] && strip -o /out/$(basename $f) $f
done
ls -la /out/*.so'
echo "-> $OUT"
