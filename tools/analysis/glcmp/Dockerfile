# pmu-glcmp: Mesa (llvmpipe) GLES 3 with surfaceless EGL, for tools/analysis/glcmp (docker build -t pmu-glcmp tools/analysis/glcmp)
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev libegl-dev libgles-dev libegl-mesa0 libgl1-mesa-dri libgbm1 && rm -rf /var/lib/apt/lists/*
