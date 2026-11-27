# The Mono runtime build (build-mono.sh): the arm64 build image plus autotools, and Python 3 as
# "python" for Mono's own build (its genmdesc.py; nothing of portmaster-unity's runs on it).
FROM ua-build-arm64
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      autoconf automake libtool libtool-bin bison gettext autopoint cmake python3 && \
    rm -rf /var/lib/apt/lists/* && ln -sf /usr/bin/python3 /usr/local/bin/python
