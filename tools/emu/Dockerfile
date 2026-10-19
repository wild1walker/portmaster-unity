# Emulated test rig for the native route on a Mac (QEMU armv7): the device's 32-bit userland with
# Mesa's software GL (llvmpipe) instead of the Mali. Frame times mean nothing here; frame contents,
# GL work counts and game logic match the device.
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libegl1 libgles2 libegl-mesa0 libgl1-mesa-dri \
      libsdl2-2.0-0 python3 && rm -rf /var/lib/apt/lists/*
