# ua-rust: the image setup-rs builds and tests in (setup-rs/tools/rs.sh, build_pmu.sh): arm64 Debian
# bookworm (tools/port/rehearse's handheld userland, ua-port-rehearse), Rust 1.88 with the static
# aarch64 musl target, and the reference Python the golden tests compare pmu with: PortMaster's
# python_3.11 at /opt/py with the setup's packages at /opt/pylibs (run it with PYTHONHOME=/opt/py
# LD_LIBRARY_PATH=/opt/py/libs PYTHONPATH=<portmaster-unity>/convert:/opt/pylibs /opt/py/bin/python3).
# setup/pmu as a release ships it also embeds a file of /opt/pylibs: UnityPy's class database
# (UnityPy/resources/lzma.tpk, build_pmu.sh's PMU_TPK; UnityPy's licence goes with it).
#
# Built once, then Dockerfile.gcc10 on top of it (the same tag), in a folder with this file and
# three files that are not in the repository:
#   python_3.11.squashfs  PortMaster's python_3.11 runtime (what PortMaster puts into its libs/)
#   7zzs.aarch64          PortMaster's 7-Zip (PortMaster/7zzs.aarch64 in PortMaster-GUI's release zip)
#   pylibs.tar            pylibs/: the setup's Python packages for CPython 3.11 aarch64, unpacked
#                         wheels (no symbolic links): UnityPy 1.25.4, astc_encoder_py 0.1.12 and
#                         tpk_ar 0.2.4 built from source in quay.io/pypa/manylinux_2_28_aarch64
#                         (cp311); lz4 4.4.5, pillow 12.3.0, texture2ddecoder 1.0.6, brotli 1.2.0,
#                         archspec 0.2.6, attrs 26.1.0, fsspec 2026.9.0, pyfmodex 0.7.2 from PyPI
#                         (manylinux aarch64, cp311); convert/native/guided_astc's wheel (1.0); a
#                         stand-in fmod_toolkit (UnityPy imports it; FMOD is not redistributable)
#   docker build --platform linux/arm64 -t ua-port-rehearse tools/port/rehearse
#   docker build --platform linux/arm64 -t ua-rust -f setup-rs/docker/Dockerfile <that folder>
#   docker build --platform linux/arm64 -t ua-rust -f setup-rs/docker/Dockerfile.gcc10 setup-rs/docker
FROM ua-port-rehearse
RUN apt-get update && apt-get install -y --no-install-recommends curl ca-certificates build-essential clang musl-tools pkg-config git \
    && rm -rf /var/lib/apt/lists/*
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:$PATH
RUN curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain 1.88.0 -t aarch64-unknown-linux-musl \
    && rustup component add clippy rustfmt
COPY python_3.11.squashfs 7zzs.aarch64 pylibs.tar /tmp/
RUN chmod +x /tmp/7zzs.aarch64 && /tmp/7zzs.aarch64 x -snl -y -o/opt/py /tmp/python_3.11.squashfs >/dev/null \
    && tar xf /tmp/pylibs.tar -C /opt 2>/dev/null; rm -f /tmp/python_3.11.squashfs /tmp/pylibs.tar && mv /tmp/7zzs.aarch64 /usr/local/bin/7zzs \
    && ls /opt/py/bin/python3 /opt/pylibs/UnityPy >/dev/null
