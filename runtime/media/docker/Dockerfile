# Media core, armhf (32-bit ARM players): Debian bookworm's cross toolchain plus the static
# decoder libraries of its armhf -dev packages (libvpx, libvorbis + libogg, libopus, OpenH264),
# merged into libuamedia.a by ../build.sh. EGL/GLES headers for the surface code (it dlopens them).
FROM debian:bookworm
RUN dpkg --add-architecture armhf && apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf libc6-dev-armhf-cross binutils-arm-linux-gnueabihf \
      libvpx-dev:armhf libvorbis-dev:armhf libogg-dev:armhf libopus-dev:armhf libopenh264-dev:armhf \
      libegl-dev:armhf libgles-dev:armhf zlib1g-dev:armhf \
      make file && \
    rm -rf /var/lib/apt/lists/*
