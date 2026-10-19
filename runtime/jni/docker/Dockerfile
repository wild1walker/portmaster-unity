# Cross toolchain for the fake JNI layer (armhf, glibc 2.36) and qemu-user for its tests
FROM debian:bookworm
ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture armhf && apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl make file gcc-arm-linux-gnueabihf libc6-dev-armhf-cross zlib1g-dev:armhf \
      binutils-arm-linux-gnueabihf qemu-user \
  && rm -rf /var/lib/apt/lists/*
WORKDIR /src
