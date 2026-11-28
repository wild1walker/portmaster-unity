#!/bin/bash
# Builds the media core: out/arm64/libuamedia.a, our objects with the build image's static decoder
# libraries merged in (libvpx, libvorbis, libogg, libopus, OpenH264), out/arm64/licenses/ (their licence
# texts, to ship with a binary that links them), and the device check out/arm64/umbench (what decoding a
# clip costs: `umbench <file> [offset size]`). Native in ua-media-arm64 (docker/Dockerfile.arm64, on top
# of ua-build-arm64). (32-bit armhf was removed; the last tree with it is the tag last-armhf.)
# ./build.sh test-ffmpeg  the decoders of the system's FFmpeg (um_ffmpeg.c) on clips made by FFmpeg's
#   tools, in an image with Debian's FFmpeg (docker/Dockerfile.ffmpeg-test, built on first use)
# ./build.sh test  also builds and runs the unit tests (test/, test_pmhwdec with a stand-in libpmhwdec),
#   natively; the GL test with Mesa from ua-build-arm64.
# OPENH264=0 ./build.sh  leaves OpenH264 out (H.264 then only through the system's libavcodec, when
#   the handheld has one): for a release that must not carry an H.264 decoder. The default is in.
# Link with: libuamedia.a -lstdc++ -ldl -lpthread -lm
set -e
cd "$(dirname "$0")"
ARCH=${ARCH:-arm64}
[ "$ARCH" = arm64 ] || { echo "ARCH must be arm64 (armhf was removed: tag last-armhf)"; exit 1; }
IMG=ua-media-arm64; DF=docker/Dockerfile.arm64; PLAT="--platform linux/arm64"; O=out/arm64
LIBDIR=/usr/lib/aarch64-linux-gnu
docker image inspect ua-build-arm64 >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-build-arm64 ../docker/arm64
docker image inspect $IMG >/dev/null 2>&1 || docker build $PLAT -t $IMG -f $DF docker
docker run --rm $PLAT -v "$PWD/..":/w -w /w/media -e O=$O -e LIBDIR=$LIBDIR \
  -e TEST="$1" -e OPENH264="${OPENH264:-1}" $IMG bash -c '
  set -e
  mkdir -p $O/obj $O/licenses $O/test
  CC=gcc; CXX=g++; AR=ar
  H264=; [ "$OPENH264" = 0 ] || H264=$(ls $LIBDIR/libopenh264.a /usr/local/lib/libopenh264.a 2>/dev/null | head -1)
  VPX=$LIBDIR/libvpx.a; VPXINC=
  [ -f /opt/vpx/lib/libvpx.a ] && { VPX=/opt/vpx/lib/libvpx.a; VPXINC=-I/opt/vpx/include; }
  DEFS="-DUM_HAVE_VPX -DUM_HAVE_VORBIS -DUM_HAVE_OPUS -DUM_HAVE_MP4 -DUM_HAVE_FFMPEG"
  [ -n "$H264" ] && DEFS="$DEFS -DUM_HAVE_H264"
  CFLAGS="-O2 -g -fPIC -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_FILE_OFFSET_BITS=64 $DEFS $VPXINC -I/usr/local/include"
  OBJS=
  for f in um_format um_extractor um_webm um_mp4 um_codec um_surface um_vpx um_audio um_h264 um_ffmpeg um_pmhwdec um_stubs um_compat; do
    $CC $CFLAGS -c $f.c -o $O/obj/$f.o; OBJS="$OBJS $O/obj/$f.o"
  done
  rm -f $O/libuamedia.a $O/core.a
  $AR rcs $O/core.a $OBJS
  { echo "create $O/libuamedia.a"; echo "addlib $O/core.a"
    for l in $VPX $LIBDIR/libvorbis.a $LIBDIR/libogg.a $LIBDIR/libopus.a $H264; do echo "addlib $l"; done
    echo save; echo end; } | $AR -M
  rm -f $O/core.a
  rm -f $O/licenses/*                                   # exactly the libraries this build linked
  for p in libvpx-dev libvorbis-dev libogg-dev libopus-dev ${H264:+libopenh264-dev}; do
    [ -f /usr/share/doc/$p/copyright ] && cp /usr/share/doc/$p/copyright $O/licenses/${p%-dev}.copyright
  done
  [ -n "$H264" ] && [ -f /usr/local/lib/openh264.LICENSE ] && cp /usr/local/lib/openh264.LICENSE $O/licenses/libopenh264.LICENSE
  ls -la $O/libuamedia.a | awk "{print \$5, \$9}"; echo "decoders built in: $DEFS"
  $CC $CFLAGS -I. -o $O/umbench umbench.c $O/libuamedia.a -lstdc++ -ldl -lpthread -lm   # device check
  if [ "$TEST" = test ] || [ "$TEST" = test-ffmpeg ]; then
    $CC $CFLAGS -I. -o $O/test/test_media_ffmpeg test/test_media_ffmpeg.c $O/libuamedia.a -lstdc++ -ldl -lpthread -lm
    T264=; [ -n "$H264" ] && T264="test/test_h264.c test/mp4_writer.c"
    $CC $CFLAGS -I. -o $O/test/test_media test/test_media.c test/clips.c test/webm_writer.c $T264 $O/libuamedia.a \
      $LIBDIR/libvorbisenc.a $VPX $LIBDIR/libvorbis.a $LIBDIR/libogg.a $LIBDIR/libopus.a $H264 -lstdc++ -ldl -lpthread -lm
    $CC $CFLAGS -I. -o $O/test/test_surface_gl test/test_surface_gl.c $O/libuamedia.a -lEGL -lGLESv2 -lstdc++ -ldl -lpthread -lm
    if [ -n "$H264" ]; then             # the hardware decoder path against a stand-in libpmhwdec.so.1
      mkdir -p $O/test/pmhwdec
      $CC $CFLAGS -shared -o $O/test/pmhwdec/libpmhwdec.so.1 test/fake_pmhwdec.c
      $CC $CFLAGS -I. -o $O/test/test_pmhwdec test/test_pmhwdec.c test/clips.c test/webm_writer.c $O/libuamedia.a \
        $LIBDIR/libvorbisenc.a $VPX $LIBDIR/libvorbis.a $LIBDIR/libogg.a $LIBDIR/libopus.a $H264 -lstdc++ -ldl -lpthread -lm
    fi
    JNI=../jni/out/arm64
    if [ -f $JNI/libfakejni.a ]; then
      $CC $CFLAGS -I. -o $O/test/test_media_jni test/test_media_jni.c test/clips.c test/webm_writer.c $JNI/libfakejni.a \
        $O/libuamedia.a $LIBDIR/libvorbisenc.a $VPX $LIBDIR/libvorbis.a $LIBDIR/libogg.a $LIBDIR/libopus.a \
        -lz -lstdc++ -ldl -lpthread -lm
      $CC $CFLAGS -I. -I../android -o $O/test/test_media_ndk test/test_media_ndk.c test/clips.c test/webm_writer.c \
        ../android/media_ndk.c ../android/native_window.c ../android/log_shim.c $JNI/libfakejni.a \
        $O/libuamedia.a $LIBDIR/libvorbisenc.a $VPX $LIBDIR/libvorbis.a $LIBDIR/libogg.a $LIBDIR/libopus.a \
        -lz -lstdc++ -ldl -lpthread -lm
    else echo "no $JNI/libfakejni.a: build ../jni first for the Java-side and NDK tests"; fi
  fi
'
if [ "$1" = test-ffmpeg ]; then
  # the FFmpeg decoders against clips FFmpeg's tools make (Debian's FFmpeg in a test image)
  P=linux/arm64
  FI=ua-media-ffmpeg-$ARCH
  docker image inspect $FI >/dev/null 2>&1 || docker build --platform $P -t $FI -f docker/Dockerfile.ffmpeg-test docker
  docker run --rm --platform $P -v "$PWD":/w -w /w $FI timeout 900 sh test/ffmpeg_clips.sh /tmp/clips ./$O/test/test_media_ffmpeg
fi
if [ "$1" = test ]; then
  docker run --rm $PLAT -v "$PWD":/w -w /w $IMG sh -c \
    'timeout 900 ./out/arm64/test/test_media; [ ! -x ./out/arm64/test/test_media_jni ] || timeout 900 ./out/arm64/test/test_media_jni; [ ! -x ./out/arm64/test/test_media_ndk ] || timeout 900 ./out/arm64/test/test_media_ndk; sh test/run_pmhwdec.sh ./out/arm64/test'
  docker run --rm $PLAT -v "$PWD/../..":/r -w /r/runtime/media -e EGL_PLATFORM=surfaceless -e LIBGL_ALWAYS_SOFTWARE=1 \
    $IMG timeout 600 ./out/arm64/test/test_surface_gl ../../setup-rs/crates/androiddata/src/video_decode_android.shader
fi
