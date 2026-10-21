# A handheld's userland for rehearsing a port's first launch (tools/port/rehearse.sh): aarch64 with
# busybox's tools first on PATH (as on Knulli), bash, curl, CA certificates, and the 32-bit glibc
# loader the launcher checks for (the game itself runs in tools/emu's armv7 rig instead).
FROM debian:bookworm-slim
RUN dpkg --add-architecture armhf && apt-get update && \
    apt-get install -y --no-install-recommends busybox-static curl ca-certificates libc6:armhf && \
    rm -rf /var/lib/apt/lists/* && mkdir -p /bb && busybox --install -s /bb
