#!/bin/sh
# Stands in for the Android NDK's compiler and binutils inside the pmunity-il2cpp image: the NDK
# ships them as x86-64 programs only, so `pmunity il2cpp` keeps just the NDK's target files
# (sysroot, libc++, libgcc) and puts this script where il2cpp's Android build looks for its tools:
#   <ndk>/toolchains/llvm/prebuilt/linux-x86_64/bin/clang, clang++
#   <ndk>/toolchains/<triple>-4.9/prebuilt/linux-x86_64/bin/<triple>-objcopy, -strip, -ar, -nm
#   <ndk>/toolchains/llvm/prebuilt/linux-x86_64/<triple>/bin/objcopy, ... and bin/llvm-objcopy, ...
#     (what Unity 2021.x / 2022.x's Bee build runs from NDK r21d / r23b)
# It runs the system's LLVM ($PMUNITY_LLVM, default /usr/lib/llvm-14/bin) with what the NDK's
# clang has built in or finds next to itself:
#   --sysroot=<ndk>/toolchains/llvm/prebuilt/linux-x86_64/sysroot (r19+: the unified sysroot with
#     per-API libraries; r16b builds pass --sysroot themselves),
#   --gcc-toolchain=<the NDK's GCC folder> (libgcc, crtbegin.o),
#   -rtlib=libgcc -unwindlib=none: the NDK's libgcc carries the unwinder (arm: libunwind, which
#     il2cpp links itself); upstream clang would look for compiler-rt's Android builtins
#     (r23b, which has no libgcc: those of its own clang, below),
#   -fuse-ld=lld for the NDK's GNU linkers (bfd, gold).
# -D__ANDROID_API__=N is dropped: clang defines it from the target (-target <triple><API>); a
# target without the API level (r16b builds) gets N appended.
# -fPIE next to -fPIC is dropped: 2017.4's il2cpp passes both (-fPIE last, so it wins) for the
# objects of a shared library; their direct references to default-visibility symbols (GNU
# libstdc++'s template statics) were accepted by the NDK's GNU ld, lld refuses them in a .so.
# -g is dropped unless PMUNITY_IL2CPP_DEBUG_INFO=1: debug info triples the objects' size and the
# compiler's memory, and the shipped libil2cpp.so is stripped anyway (il2cpp keeps the debug info
# in libil2cpp.sym.so). Code generation is the same either way.
# PMUNITY_ANDROID_API=N: the lowest API level to build for: a lower one in the target (-target /
# --target=<triple><API>) is raised to N (Unity 2023.1+ built with 2022.3's il2cpp, whose build
# program targets API 22, for a player built for 23: baselib.a needs API 23's libc).
# PMUNITY_IL2CPP_CCLOG=<file>: every rewritten command line is appended there.
name=$(basename "$0")
bin=$(cd "$(dirname "$0")" && pwd)
llvm=${PMUNITY_LLVM:-/usr/lib/llvm-14/bin}
case $name in
  *objcopy) exec "$llvm/llvm-objcopy" "$@" ;;
  *strip) exec "$llvm/llvm-strip" "$@" ;;
  *-ar | ar) exec "$llvm/llvm-ar" "$@" ;;
  *-nm | nm) exec "$llvm/llvm-nm" "$@" ;;
  clang++) cc=$llvm/clang++ ;;
  *) cc=$llvm/clang ;;
esac
tc=$(dirname "$bin")                         # toolchains/llvm/prebuilt/linux-x86_64
raise_api() {                                # <triple><API> -> <triple><max(API, PMUNITY_ANDROID_API)>
  n=${1##*[!0-9]}
  if [ -n "$PMUNITY_ANDROID_API" ] && [ -n "$n" ] && [ "$n" -lt "$PMUNITY_ANDROID_API" ]; then
    printf '%s' "${1%"$n"}$PMUNITY_ANDROID_API"
  else
    printf '%s' "$1"
  fi
}
api=; pic=; stdlib=; px=
for a do
  case $a in -D__ANDROID_API__=*) api=${a#-D__ANDROID_API__=} ;; -fPIC | -fpic) pic=1 ;; -stdlib=* | -nostdinc++ | -nostdlib++ | -xc) stdlib=1 ;; esac
  [ "$px" = -x ] && [ "$a" = c ] && stdlib=1          # (C: no C++ library to choose)
  px=$a
done
link=1; sysroot=; gcc=; prev=
for a do
  shift
  if [ "$prev" = -gcc-toolchain ]; then
    set -- "$@" "--gcc-toolchain=$a"; gcc=1; prev=; continue
  fi
  if [ "$prev" = -target ]; then              # r16b builds name no API level in the triple
    case $a in *[0-9]) ;; *) a=$a$api ;; esac
    [ -n "$PMUNITY_ANDROID_API" ] && a=$(raise_api "$a")
  fi
  case $a in
    --target=*) [ -n "$PMUNITY_ANDROID_API" ] && a=--target=$(raise_api "${a#--target=}") ;;
  esac
  case $a in
    -gcc-toolchain) prev=$a; continue ;;
    --gcc-toolchain=*) gcc=1 ;;
    --sysroot | --sysroot=* | -isysroot) sysroot=1 ;;
    -c | -S | -E | -M | -MM) link= ;;
    -fuse-ld=*) a=-fuse-ld=lld ;;
    -D__ANDROID_API__=*) prev=; continue ;;   # (clang defines it from the target's API level)
    -fPIE | -fpie) [ -n "$pic" ] && continue ;;
    -g) [ -n "$PMUNITY_IL2CPP_DEBUG_INFO" ] || continue ;;
  esac
  prev=$a
  set -- "$@" "$a"
done
[ -z "$sysroot" ] && [ -d "$tc/sysroot" ] && set -- --sysroot="$tc/sysroot" "$@"
# C++ with the unified sysroot's libc++ (r19+), which the NDK's clang++ uses by default: Unity
# 2021.x / 2022.x's Bee build passes no -stdlib (2017.4 - 2020.3's builder passes -stdlib=libc++)
[ "$name" = clang++ ] && [ -z "$stdlib" ] && [ -d "$tc/sysroot/usr/include/c++/v1" ] && set -- "$@" -stdlib=libc++
[ -z "$gcc" ] && [ -d "$tc/lib/gcc" ] && set -- --gcc-toolchain="$tc" "$@"
# the runtime library: the NDK's libgcc (with its unwinder) up to r21d; r23b has none, but
# compiler-rt's builtins and a static libunwind in its clang's resource folder, which the link
# then takes (-resource-dir: only libraries are looked up there at link time)
rt="-rtlib=libgcc -unwindlib=none"; libgcc=
for g in "$tc"/lib/gcc/*/*/libgcc.a; do [ -f "$g" ] && libgcc=1; done   # (r23b: lib/gcc has only crtbegin.o)
if [ -n "$link" ] && [ -z "$libgcc" ]; then
  for r in "$tc"/lib64/clang/*/lib/linux; do
    [ -d "$r" ] && rt="-rtlib=compiler-rt -unwindlib=libunwind -resource-dir=${r%/lib/linux}"
  done
fi
# shellcheck disable=SC2086
[ -n "$link" ] && set -- "$@" $rt -fuse-ld=lld
[ -n "$PMUNITY_IL2CPP_CCLOG" ] && echo "$cc $*" >> "$PMUNITY_IL2CPP_CCLOG"
exec "$cc" "$@"
