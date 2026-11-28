# A handheld's userland for rehearsing a port's first launch (tools/port/rehearse.sh): aarch64 with
# busybox's tools first on PATH (as on Knulli), bash, curl and CA certificates (the game itself runs
# in tools/emu's arm64 rig instead).
FROM debian:bookworm-slim
RUN apt-get update && \
    apt-get install -y --no-install-recommends busybox-static curl ca-certificates && \
    rm -rf /var/lib/apt/lists/* && mkdir -p /bb && busybox --install -s /bb
