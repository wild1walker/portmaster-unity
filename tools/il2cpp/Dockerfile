# pmunity-il2cpp: runs Unity's il2cpp (a .NET program: net471 builds of Unity 2018.4 - 2019.4,
# .NET Core 3.1 builds of 2020.x, .NET 5/6 of 2021.x+; all run on .NET 8 with roll-forward) and
# the C++ build il2cpp drives, for Android targets. The Android NDK's own compilers are x86-64
# programs; here the system's clang and lld stand in for them (tools/il2cpp/ndk-clang.sh, placed
# in the NDK tree by `pmunity il2cpp`), with the NDK's sysroot, libc++ and libgcc. So the image
# runs natively on arm64 (Apple Silicon, ARM Linux) and x86-64 hosts alike.
#
#   docker build -t pmunity-il2cpp tools/il2cpp
#
# Ubuntu 24.04: its archive has .NET 8 (dotnet-runtime-8.0) for amd64 and arm64, and Mono 6.8
# for the .NET Framework builds of UnityLinker (2018.4 - 2019.4), which use .NET Framework-only
# types at run time (as Unity's own Linux and macOS editors, they run on Mono; mono-devel for the
# netstandard facades they reference). clang 14: the
# newest clang that still compiles the C of the older libil2cpp / Boehm GC trees without
# errors for implicit declarations (clang 16 made those errors).
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        dotnet-runtime-8.0 clang-14 lld-14 llvm-14 python3 ca-certificates \
        mono-devel \
    && rm -rf /var/lib/apt/lists/*
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_ROLL_FORWARD=Major \
    PMUNITY_LLVM=/usr/lib/llvm-14/bin
