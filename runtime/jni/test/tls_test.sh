#!/bin/sh
# up_tls.c (the trust store libunity's unitytls reads through TrustManagerFactory) against local HTTPS
# servers with a self-made CA, in a Docker image of the machine's own architecture (test/tls/Dockerfile):
# a trusted server must verify, a wrong host name, an unknown CA and an empty store must not.
set -e
cd "$(dirname "$0")/.."
docker build -q -t ua-tls-test test/tls >/dev/null
docker run --rm -v "$PWD":/src:ro ua-tls-test sh -c '
  set -e
  W=$(mktemp -d); cd $W; mkdir der
  q() { "$@" >/dev/null 2>&1; }
  q openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 30 -subj "/CN=pmunity test CA"
  q openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout ca3.key -out ca3.pem -days 30 -subj "/CN=pmunity test CA 3"
  q openssl req -x509 -newkey rsa:2048 -nodes -keyout ca2.key -out ca2.pem -days 30 -subj "/CN=some other CA"
  printf "subjectAltName=DNS:tls.test\n" > ext
  q openssl req -newkey rsa:2048 -nodes -keyout srv.key -out srv.csr -subj "/CN=tls.test"
  q openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out srv.pem -days 30 -extfile ext
  q openssl req -newkey rsa:2048 -nodes -keyout bad.key -out bad.csr -subj "/CN=tls.test"
  q openssl x509 -req -in bad.csr -CA ca2.pem -CAkey ca2.key -CAcreateserial -out bad.pem -days 30 -extfile ext
  # the bundle: plain PEM, OpenSSL "TRUSTED CERTIFICATE" form, text around the blocks, a duplicate
  { echo "# test bundle"; cat ca.pem; openssl x509 -in ca3.pem -trustout -addtrust serverAuth; openssl x509 -in ca.pem -text; } > bundle.pem
  openssl x509 -in ca.pem -outform der -out der/0.der
  openssl x509 -in ca3.pem -outform der -out der/1.der
  python3 /src/test/tls/server.py 8443 srv.pem srv.key 2>server.log &
  python3 /src/test/tls/server.py 8444 bad.pem bad.key 2>>server.log &
  gcc -O2 -g -Wall -std=gnu11 -I/src -o test_tls /src/test/test_tls.c /src/fakejni.c /src/up_tls.c -lmbedtls -lmbedx509 -lmbedcrypto -lpthread -lm
  sleep 1
  UA_CA_BUNDLE=$W/bundle.pem TLS_WANT=2 ./test_tls der 8443 8444
  # a distribution bundle (Debian ca-certificates) with the test CA added
  S=/etc/ssl/certs/ca-certificates.crt
  cat $S ca.pem > sys.pem
  UA_CA_BUNDLE=$W/sys.pem TLS_WANT=$(($(grep -c "BEGIN CERTIFICATE" $S) + 1)) ./test_tls none 8443 8444 2>&1 | grep -E "trust store|tls:|FAIL"
  # no bundle anywhere: an empty store (every check fails)
  mv /etc/ssl/certs /etc/ssl/certs.off
  mkdir empty; UA_CA_BUNDLE=$W/empty/none SSL_CERT_DIR=$W/empty TLS_WANT=0 ./test_tls none 8443 8444 2>&1 | grep -E "trust store|tls:|FAIL"
  sed -n 1,3p server.log'
