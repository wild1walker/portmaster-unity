# Media core, armhf (32-bit ARM players): Debian bookworm's cross toolchain plus the static
# decoder libraries of its armhf -dev packages (libvorbis + libogg, libopus, OpenH264), merged into
# libuamedia.a by ../build.sh. EGL/GLES headers for the surface code (it dlopens them).
FROM debian:bookworm
RUN dpkg --add-architecture armhf && apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf libc6-dev-armhf-cross binutils-arm-linux-gnueabihf \
      libvpx-dev:armhf libvorbis-dev:armhf libogg-dev:armhf libopus-dev:armhf libopenh264-dev:armhf \
      libegl-dev:armhf libgles-dev:armhf zlib1g-dev:armhf \
      make file && \
    rm -rf /var/lib/apt/lists/*
# libvpx from Debian's own source package (the same release and patches as libvpx-dev above),
# built for ARMv7 with NEON into /opt/vpx, which ../build.sh links instead of Debian's: Debian
# builds armhf libvpx as plain C (--target=generic-gnu --enable-small, NEON being optional on
# armhf), which decodes Hollow Knight's 1920x1080 VP8 cinematics at 7 fps on a Cortex-A53.
# NEON is always on (no run-time check): Unity's armeabi-v7a players require it.
RUN sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources && apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends dpkg-dev perl xz-utils && \
    mkdir /tmp/vpx && cd /tmp/vpx && apt-get source libvpx && cd libvpx-*/ && \
    CROSS=arm-linux-gnueabihf- ./configure --target=armv7-linux-gcc --prefix=/opt/vpx \
      --enable-pic --enable-static --disable-shared --disable-runtime-cpu-detect --enable-neon \
      --size-limit=16384x16384 --enable-postproc --enable-vp9-postproc --enable-vp9-highbitdepth \
      --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-install-bins \
      --extra-cflags="-mfloat-abi=hard -mfpu=neon" && \
    make -j"$(nproc)" && make install && \
    cp /usr/share/doc/libvpx-dev/copyright /opt/vpx/copyright && \
    dpkg-parsechangelog -l debian/changelog -S Version > /opt/vpx/version && \
    cd / && rm -rf /tmp/vpx /var/lib/apt/lists/*
