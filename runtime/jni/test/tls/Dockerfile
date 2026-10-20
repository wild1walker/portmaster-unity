# up_tls.c's test (tls_test.sh): mbedTLS (the TLS library unitytls is built on) as the client,
# openssl for a self-made CA, Python's ssl module for the local HTTPS servers. Test-only image.
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev libmbedtls-dev openssl python3 ca-certificates libcurl4 zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
