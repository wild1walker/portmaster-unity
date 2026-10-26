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
docker image inspect ua-build-arm64 >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-build-arm64 "$PU/runtime/docker/arm64"
docker image inspect ua-mono64-build >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-mono64-build "$HERE"
docker run --rm --platform linux/arm64 -v ua-mono64:/b -v "$D":/src:ro -v "$W/out/$B":/out ua-mono64-build bash -c '
set -e
B='"$B"'
[ -f /b/$B/.copied ] || { rm -rf /b/$B; mkdir -p /b/$B; cp -a /src/. /b/$B/; touch /b/$B/.copied; }
cd /b/$B
# as Unity'"'"'s build.pl builds the Linux players'"'"' runtime (no class libraries, no btls, Boehm,
# no parallel marking), optimized for speed instead of size
export CFLAGS="-fPIC -O2 -g"
# the Boehm collector is external/bdwgc: Mono 5 builds it as the "bdwgc" runtime next to an older
# included Boehm ("boehm", libgc/, which does not know aarch64): that one is left out; Mono 6
# calls external/bdwgc "boehm"
GC=--enable-boehm; grep -q "AC_ARG_ENABLE(bdwgc" configure.ac && GC=--disable-boehm
if [ ! -f config.status ]; then
  NOCONFIGURE=1 ./autogen.sh > /out/autogen.log 2>&1 || { tail -30 /out/autogen.log; exit 1; }
  ./configure --prefix=/b/$B-inst --host=aarch64-pc-linux-gnu --disable-mcs-build --with-mcs-docs=no \
    --with-glib=embedded --disable-nls --disable-btls --with-monotouch=no --disable-parallel-mark \
    --with-sgen=no $GC > /out/configure.log 2>&1 || { tail -40 /out/configure.log; exit 1; }
fi
: > /out/make.log
# (eglib: at the top of the tree in Mono 5.0 - 2017.4 - and in mono/ later)
for d in external/bdwgc eglib mono/culture mono/eglib mono/arch mono/utils mono/cil mono/zlib mono/sgen \
         mono/metadata mono/mini mono/profiler mono/native support; do
  [ -d $d ] && [ -f $d/Makefile ] || continue
  # mono/mini: the runtime library only (its mono executable wants files of the full build)
  if [ $d = mono/mini ]; then make -k -j$(nproc) -C $d >> /out/make.log 2>&1 || true
  elif [ $d = mono/native ]; then make -j$(nproc) -C $d >> /out/make.log 2>&1 || echo "mono/native: not built (make.log)"
  # (the profilers are not shipped; Mono 5.0'"'"'s want the old Boehm runtime this build leaves out)
  elif [ $d = mono/profiler ]; then make -j$(nproc) -C $d >> /out/make.log 2>&1 || echo "mono/profiler: not built (make.log)"
  else make -j$(nproc) -C $d >> /out/make.log 2>&1 || { grep -n -B3 -A12 -m2 " error\|No rule" /out/make.log | head -60; exit 1; }
  fi
done
L=mono/mini/.libs/libmonobdwgc-2.0.so; [ -f $L ] || L=mono/mini/.libs/libmonoboehm-2.0.so   # (Mono 6 names it so)
[ -f $L ] || { grep -n -B3 -A12 -m2 " error" /out/make.log | head -60; exit 1; }
cp -L $L /out/libmonobdwgc-2.0.debug.so
strip -o /out/libmonobdwgc-2.0.so /out/libmonobdwgc-2.0.debug.so
for f in support/.libs/libMonoPosixHelper.so mono/native/.libs/libmono-native.so; do
  [ -f $f ] && strip -o /out/$(basename $f) $f
done
ls -la /out/*.so'
echo "-> $W/out/$B"
