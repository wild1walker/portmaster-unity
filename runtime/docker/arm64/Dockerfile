# Native build environment for the 64-bit (arm64 / AArch64) runtime: Ubuntu 20.04 (glibc 2.31), so
# the binaries run on nearly every PortMaster firmware. Runs natively on Apple Silicon and on arm64
# Linux; tests run in it directly (no QEMU).
# arm64v8/: the arm64 image by name, so a cached amd64 ubuntu:20.04 is never used (x86 PCs without BuildKit)
FROM arm64v8/ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential file python3 zlib1g-dev \
      libsdl2-dev libegl1-mesa-dev libgles2-mesa-dev libegl-mesa0 libgl1-mesa-dri \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
