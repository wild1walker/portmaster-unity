# The 64-bit runtime's build environment with a current clang (19, apt.llvm.org): Ubuntu 20.04 (glibc
# 2.31) as runtime/docker/arm64, for what needs a newer compiler than its gcc 9: the Mach-O route's
# C++ runtime (LLVM's libc++, libc++abi and libunwind built from source, runtime/macho/cxx) and the
# bridge linked with it. Runs natively on Apple Silicon and on arm64 Linux.
# BASE: arm64v8/ubuntu:20.04, or runtime/docker/arm64's image when Docker Hub cannot be reached
ARG BASE=arm64v8/ubuntu:20.04
FROM ${BASE}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates curl gnupg file python3 python3-pip ninja-build xz-utils zlib1g-dev \
 && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
 && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/focal/ llvm-toolchain-focal-19 main" > /etc/apt/sources.list.d/llvm.list \
 && apt-get update && apt-get install -y --no-install-recommends clang-19 lld-19 llvm-19 \
 && pip3 install --no-cache-dir 'cmake>=3.20,<3.30' \
 && rm -rf /var/lib/apt/lists/*
ENV CC=clang-19 CXX=clang++-19
WORKDIR /src
