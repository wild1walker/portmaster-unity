# Cross toolchain + local test runner for the loader.
# Debian bookworm (glibc 2.36) armhf cross gcc, running natively on the host arch
# (arm64 on Apple Silicon). Also installs the armhf runtime (multiarch) and qemu-user
# so armhf test binaries can be run with `qemu-arm` without pulling an arm/v7 image.
FROM debian:bookworm
RUN dpkg --add-architecture armhf && apt-get update && DEBIAN_FRONTEND=noninteractive \
    apt-get install -y --no-install-recommends \
      gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf binutils-arm-linux-gnueabihf libc6-dev-armhf-cross \
      make file python3 qemu-user \
      libc6:armhf libgcc-s1:armhf zlib1g:armhf libstdc++6:armhf \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
