# Emulated test rig for the native route (QEMU armv7, or AArch64 for arm64-v8a players; emu.sh
# builds it per platform): the device's userland with Mesa's software GL (llvmpipe) instead of the
# Mali. Frame times mean nothing here; frame contents, GL work counts and game logic match the device.
# BASE: the platform's image by name (emu.sh: arm32v7/ or arm64v8/debian), so a cached image of another
# architecture is never used (x86 PCs without BuildKit)
ARG BASE=debian:bookworm-slim
FROM $BASE
RUN apt-get update && apt-get install -y --no-install-recommends libegl1 libgles2 libegl-mesa0 libgl1-mesa-dri \
      libsdl2-2.0-0 python3 && rm -rf /var/lib/apt/lists/*
