#!/bin/sh
# build-cxx-macho.sh - on a Mac with Xcode's command line tools: LLVM 19.1.7's libc++ and
# libc++abi built as macOS arm64 dylibs -> out/macos-arm64/{libc++.1.dylib,libc++abi.1.dylib}.
# The Mach-O loader maps them beside the game's image and binds the image's libc++ imports to
# them, so the library is the one the image's inline code was compiled against, layout for
# layout (mbstate_t, ctype masks, errno numbers, pthread types, std::string): nothing of it is
# imitated on the Linux side. Their own libSystem imports go to the same Darwin shims as the
# game's. Configured as libcxx/cmake/caches/Apple.cmake does (ABI version 1, namespace __1,
# new and delete in libc++abi, the forgiving dynamic_cast, the same export lists), with two
# differences that leave the ABI alone:
#   -mcpu=generic      no instruction a Cortex-A35/A53 lacks (test/run.sh lists them: none).
#                      -march=armv8-a is not enough: the CPU stays apple-m1 and brings LSE along
#   -no_fixup_chains   rebases and binds as LC_DYLD_INFO opcode streams
# No cmake on the Mac is needed: the source lists below are libcxx/src/CMakeLists.txt's and
# libcxxabi/src/CMakeLists.txt's for Apple. The exports are checked against LLVM's own list
# (libcxx/lib/abi/arm64-apple-darwin.*.abilist). Apple's libc++ is never read or copied.
#   LLVM_SRC   an extracted llvm-project-19.1.7.src (default: extracted from $LLVM_CACHE's tarball)
#   BUILD      the build directory (default: a temporary one, removed)
set -e
cd "$(dirname "$0")"
V=19.1.7
SHA=82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501
CACHE=${LLVM_CACHE:-$PWD/../../../.cache}
OUT=$PWD/out/macos-arm64
B=${BUILD:-$(mktemp -d)}
mkdir -p "$OUT" "$B/abi" "$B/cxx"
S=$LLVM_SRC
if [ -z "$S" ]; then
  T="$CACHE/llvm-project-$V.src.tar.xz"
  if [ ! -f "$T" ]; then
    mkdir -p "$CACHE"
    curl -fL --retry 5 -o "$T.part" "https://github.com/llvm/llvm-project/releases/download/llvmorg-$V/llvm-project-$V.src.tar.xz"
    mv "$T.part" "$T"
  fi
  echo "$SHA  $T" | shasum -a 256 -c - >/dev/null || { echo "$T: sha256 differs" >&2; exit 1; }
  tar -xJf "$T" -C "$B" "llvm-project-$V.src/libcxx" "llvm-project-$V.src/libcxxabi"
  S="$B/llvm-project-$V.src"
fi
[ -f "$S/libcxx/src/string.cpp" ] || { echo "$S: no libcxx sources" >&2; exit 1; }

# The headers as a build generates them: libcxx/include with __config_site and __assertion_handler.
INC="$B/include"
rm -rf "$INC"
cp -R "$S/libcxx/include" "$INC"
cp "$S/libcxx/vendor/llvm/default_assertion_handler.in" "$INC/__assertion_handler"
cat > "$INC/__config_site" <<'EOF'
#ifndef _LIBCPP___CONFIG_SITE
#define _LIBCPP___CONFIG_SITE
#define _LIBCPP_ABI_VERSION 1
#define _LIBCPP_ABI_NAMESPACE __1
#define _LIBCPP_PSTL_BACKEND_LIBDISPATCH
#define _LIBCPP_HARDENING_MODE_DEFAULT _LIBCPP_HARDENING_MODE_NONE
#endif
EOF

TARGET="-arch arm64 -mmacosx-version-min=11.0 -mcpu=generic"
COMMON="$TARGET -std=c++23 -nostdinc++ -O2 -DNDEBUG -fstrict-aliasing -D_LIBCPP_BUILDING_LIBRARY \
  -I$INC -I$S/libcxxabi/include -I$S/libcxx/src"
ABI_FLAGS="$COMMON -D_LIBCXXABI_BUILDING_LIBRARY -D_LIBCXXABI_FORGIVING_DYNAMIC_CAST"
CXX_FLAGS="$COMMON -DLIBCXX_BUILDING_LIBCXXABI -D_LIBCPP_REMOVE_TRANSITIVE_INCLUDES \
  -fvisibility=hidden -fvisibility-inlines-hidden -faligned-allocation -fsized-deallocation"

ABI_SRC="cxa_aux_runtime cxa_default_handlers cxa_demangle cxa_exception_storage cxa_guard
  cxa_handlers cxa_vector cxa_virtual stdlib_exception stdlib_stdexcept stdlib_typeinfo
  abort_message fallback_malloc private_typeinfo stdlib_new_delete cxa_exception cxa_personality"
CXX_SRC="algorithm any bind call_once charconv chrono error_category exception expected
  filesystem/filesystem_clock filesystem/filesystem_error filesystem/path functional hash
  legacy_pointer_safety memory memory_resource new_handler new_helpers optional print
  random_shuffle ryu/d2fixed ryu/d2s ryu/f2s stdexcept string system_error typeinfo valarray
  variant vector verbose_abort
  atomic barrier condition_variable_destructor condition_variable future mutex_destructor mutex
  shared_mutex thread
  random
  fstream ios ios.instantiations iostream locale ostream regex strstream
  filesystem/directory_entry filesystem/directory_iterator filesystem/operations"

# compile DIR FLAGS SRCDIR NAMES: every source to DIR/<name with / as _>.o, in parallel.
compile() {
  d=$1; f=$2; s=$3; shift 3
  for n in $*; do echo "$n"; done | xargs -P "$(sysctl -n hw.ncpu)" -I{} sh -c '
    o="$1/$(echo "$4" | tr / _).o"
    xcrun clang++ $2 -c "$3/$4.cpp" -o "$o" || exit 255' sh "$d" "$f" "$s" {}
}
compile "$B/abi" "$ABI_FLAGS" "$S/libcxxabi/src" $ABI_SRC
compile "$B/cxx" "$CXX_FLAGS" "$S/libcxx/src" $CXX_SRC

L="$S/libcxxabi/lib"
REEXP="cxxabiv1 fundamental-types itanium-base std-misc new-delete std-exceptions itanium-exceptions personality-v0"
EXPORTS="-Wl,-exported_symbols_list,$L/symbols-not-reexported.exp"
REEXPORTS=""
for e in $REEXP; do
  EXPORTS="$EXPORTS -Wl,-exported_symbols_list,$L/$e.exp"
  REEXPORTS="$REEXPORTS -Wl,-reexported_symbols_list,$L/$e.exp"
done
LINK="$TARGET -dynamiclib -nostdlib++ -Wl,-no_fixup_chains -compatibility_version 1"
xcrun clang++ $LINK -o "$OUT/libc++abi.1.dylib" "$B"/abi/*.o \
  -install_name /usr/lib/libc++abi.dylib -current_version 1900.7 $EXPORTS
xcrun clang++ $LINK -o "$OUT/libc++.1.dylib" "$B"/cxx/*.o "$OUT/libc++abi.1.dylib" \
  -install_name /usr/lib/libc++.1.dylib -current_version 1900.7 $REEXPORTS \
  -Wl,-unexported_symbols_list,"$S/libcxx/lib/libc++unexp.exp" \
  -Wl,-force_symbols_not_weak_list,"$S/libcxx/lib/notweak.exp" \
  -Wl,-force_symbols_weak_list,"$S/libcxx/lib/weak.exp"

# The exports against LLVM's list for this target: every name there, and no other.
A="$S/libcxx/lib/abi/arm64-apple-darwin.libcxxabi.v1.stable.exceptions.nonew.abilist"
grep "'is_defined': True" "$A" | sed "s/.*'name': '\([^']*\)'.*/\1/" | sort -u > "$B/want.txt"
xcrun nm -g "$OUT/libc++.1.dylib" | awk '$1 != "U" && $2 != "U" { print ($1 == "I" ? $2 : $3) }' | sort -u > "$B/have.txt"
if ! diff "$B/want.txt" "$B/have.txt" > "$B/exports.diff"; then
  echo "libc++.1.dylib: exports differ from $(basename "$A"):" >&2
  head -40 "$B/exports.diff" >&2
  exit 1
fi
echo "exports: $(wc -l < "$B/have.txt" | tr -d ' ') names, as LLVM's list for arm64-apple-darwin"
[ -n "$BUILD" ] || rm -rf "$B"
ls -l "$OUT"
