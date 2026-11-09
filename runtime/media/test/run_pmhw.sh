#!/bin/sh
# test_pmhw in each of its modes (see test_pmhw.c); $1 = the test directory (out/test or out/arm64/test)
T=${1:?test directory}
[ -x "$T/test_pmhw" ] || { echo "no $T/test_pmhw (built only with OpenH264)"; exit 0; }
rc=0
for m in none hw nohw off refuse midway path h264env vp8 vp8nohw; do
  L=$T/pmhw; F=; P=; H=
  case $m in none) L= ;; nohw|refuse|midway) F=$m ;; vp8nohw) F=nohw ;; off) P=0 ;; path) L=; P=$T/pmhw/libpmhwdec.so.1 ;; h264env) H=ffmpeg ;; esac
  env LD_LIBRARY_PATH="$L" FAKE_PMHW="$F" UM_PMHWDEC="$P" UM_H264="$H" timeout 120 "$T/test_pmhw" $m || rc=1
done
exit $rc
