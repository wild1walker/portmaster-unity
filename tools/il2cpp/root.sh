#!/bin/sh
# Makes the system `pmunity il2cpp` runs Unity's il2cpp and the C++ build in, as plain folders
# (<root>/usr, <root>/etc, about 0.85 GB) that are bind-mounted over a stock ubuntu:24.04
# container's own (`docker run -v <root>/usr:/usr:ro -v <root>/etc:/etc:ro ubuntu:24.04 ...`). No
# image is built: Docker's own disk only holds the stock image, and the folders can live on any
# drive. The packages are downloaded by apt and unpacked with dpkg-deb -x (no maintainer
# scripts: none of these needs one; dpkg itself cannot install onto Docker Desktop's file
# sharing, whose permissions refuse its mode-0 staging folders).
#
#   tools/il2cpp/root.sh <root>          (pmunity il2cpp runs it when <root>/.complete is missing)
#
# What is installed, from Ubuntu's archive (amd64 and arm64 alike, so it runs natively on Apple
# Silicon and ARM Linux as on x86-64):
#   dotnet-runtime-8.0  il2cpp / UnityLinker of Unity 2020.1+ (.NET Core 3.1, .NET 5/6 builds, run
#                       on .NET 8 with roll-forward)
#   mono-devel          their .NET Framework builds (2018.4 - 2019.4), which use .NET Framework-only
#                       types at run time; Unity's own Linux and macOS editors run them on Mono too
#                       (mono-devel for the netstandard facades they reference)
#   clang-14 lld-14 llvm-14   stand in for the Android NDK's x86-64 compilers and binutils
#                       (tools/il2cpp/ndk-clang.sh). 14: the newest clang that still compiles the
#                       C of the older libil2cpp / Boehm GC trees (clang 16 made implicit
#                       declarations errors)
# A case-insensitive file system (macOS's default) is fine: the only clashing names are a few
# Linux kernel headers nothing here uses.
set -e
root=${1:?usage: root.sh <root folder>}
image=${PMUNITY_IL2CPP_BASE:-ubuntu:24.04}
mkdir -p "$root"
root=$(cd "$root" && pwd)
chmod -R u+rwx "$root" 2>/dev/null || true
rm -rf "$root/usr" "$root/etc" "$root/.part" "$root/debs" "$root/cache" "$root/lists" "$root/.complete"
mkdir -p "$root/.part" "$root/debs/partial" "$root/lists/partial" "$root/cache"
docker run --rm -v "$root:/r" "$image" sh -c '
  set -e
  cp -a /usr /etc /r/.part/ && mv /r/.part/usr /r/.part/etc /r/ && rmdir /r/.part
  A="-o Dir::State::Lists=/r/lists -o Dir::Cache=/r/cache -o Dir::Cache::archives=/r/debs"
  apt-get $A update > /dev/null
  apt-get $A install -y --download-only --no-install-recommends \
      dotnet-runtime-8.0 mono-devel clang-14 lld-14 llvm-14 > /r/download.log 2>&1 || { cat /r/download.log; exit 1; }
  for d in /r/debs/*.deb; do dpkg-deb -x "$d" /r/; done'
docker run --rm -v "$root/usr:/usr" -v "$root/etc:/etc" "$image" ldconfig
rm -rf "$root/debs" "$root/cache" "$root/lists" "$root/download.log"
touch "$root/.complete"
echo "il2cpp root ready: $root ($(du -sh "$root" | cut -f1))"
