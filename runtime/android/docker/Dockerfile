# Cross toolchain for the Android shims (armhf, glibc 2.36).
FROM debian:bookworm
RUN dpkg --add-architecture armhf && apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      gcc-arm-linux-gnueabihf binutils-arm-linux-gnueabihf libc6-dev-armhf-cross \
      libsdl2-dev:armhf libegl-dev:armhf make file ca-certificates && \
    rm -rf /var/lib/apt/lists/*
