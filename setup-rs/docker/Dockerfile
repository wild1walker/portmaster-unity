# ua-rust: the image setup-rs builds and tests in (setup-rs/tools/rs.sh, build_pmu.sh): arm64 Debian
# bookworm (tools/port/rehearse's handheld userland, ua-port-rehearse), Rust 1.88 with the static
# aarch64 musl target, PortMaster's 7-Zip, and Unity's class database that setup/pmu embeds and the
# tests read: UnityPy's resources/lzma.tpk with UnityPy's licence (MIT), at /opt/unitypy ($PMU_TPK),
# from UnityPy's repository at the 1.25.4 tag, checked against the sha256 of that release's file (a
# repository whose file differs stops the build). No Python: the converter it once compared pmu with
# is gone (images built before 2026-11-27 have the class database at
# /opt/pylibs/UnityPy/resources/lzma.tpk instead: build_pmu.sh and pmu-dev.sh look there too).
#
# Built once, then Dockerfile.gcc10 on top of it (the same tag), in a folder with this file and
# a file that is not in the repository:
#   7zzs.aarch64          PortMaster's 7-Zip (PortMaster/7zzs.aarch64 in PortMaster-GUI's release zip)
#   docker build --platform linux/arm64 -t ua-port-rehearse tools/port/rehearse
#   docker build --platform linux/arm64 -t ua-rust -f setup-rs/docker/Dockerfile <that folder>
#   docker build --platform linux/arm64 -t ua-rust -f setup-rs/docker/Dockerfile.gcc10 setup-rs/docker
FROM ua-port-rehearse
# (libc6-armhf-cross: armhf glibc's exports, which `pmu-dev elf-imports` judges a player's imports by;
# tools/re/survey_player.sh)
RUN apt-get update && apt-get install -y --no-install-recommends curl ca-certificates build-essential clang musl-tools pkg-config git libc6-armhf-cross \
    && rm -rf /var/lib/apt/lists/*
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:$PATH
RUN curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain 1.88.0 -t aarch64-unknown-linux-musl \
    && rustup component add clippy rustfmt
ARG UNITYPY=https://raw.githubusercontent.com/K0lb3/UnityPy/1.25.4
RUN mkdir -p /opt/unitypy && curl -fsSL -o /opt/unitypy/lzma.tpk "$UNITYPY/UnityPy/resources/lzma.tpk" \
    && curl -fsSL -o /opt/unitypy/LICENSE "$UNITYPY/LICENSE" \
    && echo "16f1f8b809d1cd3bc3aeebf0cd43ecd3690b326a7a88b2273e57bcd83f5df608  /opt/unitypy/lzma.tpk" | sha256sum -c -
ENV PMU_TPK=/opt/unitypy/lzma.tpk
COPY 7zzs.aarch64 /usr/local/bin/7zzs
RUN chmod +x /usr/local/bin/7zzs
