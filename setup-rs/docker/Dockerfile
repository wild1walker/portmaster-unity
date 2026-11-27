# ua-rust: the image setup-rs builds and tests in (setup-rs/tools/rs.sh, build_pmu.sh): arm64 Debian
# bookworm (tools/port/rehearse's handheld userland, ua-port-rehearse), Rust 1.88 with the static
# aarch64 musl target, PortMaster's 7-Zip, and Unity's class database that setup/pmu embeds and the
# tests read: UnityPy's resources/lzma.tpk with UnityPy's licence (MIT), at /opt/unitypy ($PMU_TPK),
# from UnityPy 1.25.4's source release on PyPI (the archive and the file checked by their sha256: a
# download that differs stops the build). No Python: the converter it once compared pmu with
# is gone (an image built before 2026-11-27 had the class database at /opt/pylibs: build again).
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
# (UnityPy 1.25.4's source release on PyPI, checked by its sha256; only these two files are kept)
ARG UNITYPY=https://files.pythonhosted.org/packages/10/a4/5b1c786cae645d30c882eac7933ab7c8916e13f1a571bf7d0f2cdc942a52/unitypy-1.25.4.tar.gz
RUN mkdir -p /opt/unitypy && cd /tmp && curl -fsSL -o unitypy.tar.gz "$UNITYPY" \
    && echo "91d6169d73db8eb5f11fd6531b4e5e980f53dae00d5e3cbab827bbf4765de6d3  unitypy.tar.gz" | sha256sum -c - \
    && tar -xzf unitypy.tar.gz unitypy-1.25.4/UnityPy/resources/lzma.tpk unitypy-1.25.4/LICENSE \
    && mv unitypy-1.25.4/UnityPy/resources/lzma.tpk unitypy-1.25.4/LICENSE /opt/unitypy/ \
    && rm -rf unitypy.tar.gz unitypy-1.25.4 \
    && echo "16f1f8b809d1cd3bc3aeebf0cd43ecd3690b326a7a88b2273e57bcd83f5df608  /opt/unitypy/lzma.tpk" | sha256sum -c -
ENV PMU_TPK=/opt/unitypy/lzma.tpk
COPY 7zzs.aarch64 /usr/local/bin/7zzs
RUN chmod +x /usr/local/bin/7zzs
