# armhf cross toolchain for the host program (Debian bookworm: glibc 2.36, runs on the device's 2.40)
FROM debian:bookworm
RUN dpkg --add-architecture armhf && apt-get update -qq && \
    apt-get install -y -qq --no-install-recommends gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf libc6-dev-armhf-cross \
      binutils-arm-linux-gnueabihf libegl-dev:armhf libgles-dev:armhf libsdl2-dev:armhf make file >/dev/null && \
    rm -rf /var/lib/apt/lists/*
