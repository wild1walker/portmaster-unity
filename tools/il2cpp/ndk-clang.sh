#!/bin/sh
# Stands in for the Android NDK's compiler and binutils inside the pmunity-il2cpp image: the NDK
# ships them as x86-64 programs only, so `pmunity il2cpp` keeps just the NDK's target files
# (sysroot, libc++, libgcc) and puts this script where il2cpp's Android build looks for its tools:
#   <ndk>/toolchains/llvm/prebuilt/linux-x86_64/bin/clang, clang++
#   <ndk>/toolchains/<triple>-4.9/prebuilt/linux-x86_64/bin/<triple>-objcopy, -strip, -ar
# It runs the system's LLVM ($PMUNITY_LLVM, default /usr/lib/llvm-14/bin) with what the NDK's
# clang has built in or finds next to itself:
#   --sysroot=<ndk>/toolchains/llvm/prebuilt/linux-x86_64/sysroot (r19+: the unified sysroot with
#     per-API libraries; r16b builds pass --sysroot themselves),
#   --gcc-toolchain=<the NDK's GCC folder> (libgcc, crtbegin.o),
#   -rtlib=libgcc -unwindlib=none: the NDK's libgcc carries the unwinder (arm: libunwind, which
#     il2cpp links itself); upstream clang would look for compiler-rt's Android builtins,
#   -fuse-ld=lld for the NDK's GNU linkers (bfd, gold).
# -D__ANDROID_API__=N is dropped: clang defines it from the target (-target <triple><API>); a
# target without the API level (r16b builds) gets N appended.
# -g is dropped unless PMUNITY_IL2CPP_DEBUG_INFO=1: debug info triples the objects' size and the
# compiler's memory, and the shipped libil2cpp.so is stripped anyway (il2cpp keeps the debug info
# in libil2cpp.sym.so). Code generation is the same either way.
# PMUNITY_IL2CPP_CCLOG=<file>: every rewritten command line is appended there.
name=$(basename "$0")
bin=$(cd "$(dirname "$0")" && pwd)
llvm=${PMUNITY_LLVM:-/usr/lib/llvm-14/bin}
case $name in
  *objcopy) exec "$llvm/llvm-objcopy" "$@" ;;
  *strip) exec "$llvm/llvm-strip" "$@" ;;
  *-ar | ar) exec "$llvm/llvm-ar" "$@" ;;
  *-nm) exec "$llvm/llvm-nm" "$@" ;;
  clang++) cc=$llvm/clang++ ;;
  *) cc=$llvm/clang ;;
esac
tc=$(dirname "$bin")                         # toolchains/llvm/prebuilt/linux-x86_64
api=
for a do case $a in -D__ANDROID_API__=*) api=${a#-D__ANDROID_API__=} ;; esac; done
link=1; sysroot=; gcc=; prev=
for a do
  shift
  if [ "$prev" = -gcc-toolchain ]; then
    set -- "$@" "--gcc-toolchain=$a"; gcc=1; prev=; continue
  fi
  if [ "$prev" = -target ]; then              # r16b builds name no API level in the triple
    case $a in *[0-9]) ;; *) a=$a$api ;; esac
  fi
  case $a in
    -gcc-toolchain) prev=$a; continue ;;
    --gcc-toolchain=*) gcc=1 ;;
    --sysroot | --sysroot=* | -isysroot) sysroot=1 ;;
    -c | -S | -E | -M | -MM) link= ;;
    -fuse-ld=*) a=-fuse-ld=lld ;;
    -D__ANDROID_API__=*) prev=; continue ;;   # (clang defines it from the target's API level)
    -g) [ -n "$PMUNITY_IL2CPP_DEBUG_INFO" ] || continue ;;
  esac
  prev=$a
  set -- "$@" "$a"
done
[ -z "$sysroot" ] && [ -d "$tc/sysroot" ] && set -- --sysroot="$tc/sysroot" "$@"
[ -z "$gcc" ] && [ -d "$tc/lib/gcc" ] && set -- --gcc-toolchain="$tc" "$@"
[ -n "$link" ] && set -- "$@" -rtlib=libgcc -unwindlib=none -fuse-ld=lld
[ -n "$PMUNITY_IL2CPP_CCLOG" ] && echo "$cc $*" >> "$PMUNITY_IL2CPP_CCLOG"
exec "$cc" "$@"
