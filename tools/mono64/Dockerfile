# The Mono runtime build (build-mono.sh): the arm64 build image plus autotools; python is Python 3
# (Mono's genmdesc.py runs as "python").
FROM ua-build-arm64
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      autoconf automake libtool libtool-bin bison gettext autopoint cmake && \
    rm -rf /var/lib/apt/lists/* && ln -sf /usr/bin/python3 /usr/local/bin/python
