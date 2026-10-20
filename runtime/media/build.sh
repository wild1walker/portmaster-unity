#!/bin/bash
# Builds the media core: out/libuamedia.a (armhf) or out/arm64/libuamedia.a, our objects with the
# build image's static decoder libraries merged in (libvpx, libvorbis, libogg, libopus, OpenH264),
# out/[arm64/]licenses/ (their licence texts, to ship with a binary that links them), and the device
# check out/[arm64/]umbench (what decoding a clip costs: `umbench <file> [offset size]`).
#   ARCH=armhf (default): Debian bookworm cross toolchain, image ua-media-armhf (docker/Dockerfile)
#   ARCH=arm64: native in ua-media-arm64 (docker/Dockerfile.arm64, on top of ua-build-arm64)
# ./build.sh test-ffmpeg  the decoders of the system's FFmpeg (um_ffmpeg.c) on clips made by FFmpeg's
#   tools, in an image with Debian's FFmpeg (docker/Dockerfile.ffmpeg-test, built on first use)
# ./build.sh test  also builds and runs the unit tests (test/): armhf in an arm/v7 container
#   (Docker's emulation), arm64 natively; the GL test needs the emulated rig's image ua-emu-armhf
#   (tools/emu) for armhf, Mesa from ua-build-arm64 for arm64.
# OPENH264=0 ./build.sh  leaves OpenH264 out (H.264 then only through the system's libavcodec, when
#   the handheld has one): for a release that must not carry an H.264 decoder. The default is in.
# Link with: libuamedia.a -lstdc++ -ldl -lpthread -lm
set -e
cd "$(dirname "$0")"
ARCH=${ARCH:-armhf}
case "$ARCH" in
  armhf) IMG=ua-media-armhf; DF=docker/Dockerfile; PLAT=; O=out; CROSS=arm-linux-gnueabihf-
         LIBDIR=/usr/lib/arm-linux-gnueabihf; ARCHF="-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard" ;;
  arm64) IMG=ua-media-arm64; DF=docker/Dockerfile.arm64; PLAT="--platform linux/arm64"; O=out/arm64; CROSS=
         LIBDIR=/usr/lib/aarch64-linux-gnu; ARCHF=
         docker image inspect ua-build-arm64 >/dev/null 2>&1 || docker build -t ua-build-arm64 ../docker/arm64 ;;
  *) echo "ARCH must be armhf or arm64"; exit 1 ;;
esac
docker image inspect $IMG >/dev/null 2>&1 || docker build $PLAT -t $IMG -f $DF docker
docker run --rm $PLAT -v "$PWD/..":/w -w /w/media -e O=$O -e CROSS=$CROSS -e LIBDIR=$LIBDIR -e ARCHF="$ARCHF" \
  -e TEST="$1" -e OPENH264="${OPENH264:-1}" $IMG bash -c '
  set -e
  mkdir -p $O/obj $O/licenses $O/test
  CC=${CROSS}gcc; CXX=${CROSS}g++; AR=${CROSS}ar
  H264=; [ "$OPENH264" = 0 ] || H264=$(ls $LIBDIR/libopenh264.a /usr/local/lib/libopenh264.a 2>/dev/null | head -1)
  DEFS="-DUM_HAVE_VPX -DUM_HAVE_VORBIS -DUM_HAVE_OPUS -DUM_HAVE_MP4 -DUM_HAVE_FFMPEG"
  [ -n "$H264" ] && DEFS="$DEFS -DUM_HAVE_H264"
  CFLAGS="-O2 -g -fPIC -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_FILE_OFFSET_BITS=64 $ARCHF $DEFS -I/usr/local/include"
  OBJS=
  for f in um_format um_extractor um_webm um_mp4 um_codec um_surface um_vpx um_audio um_h264 um_ffmpeg um_stubs um_compat; do
    $CC $CFLAGS -c $f.c -o $O/obj/$f.o; OBJS="$OBJS $O/obj/$f.o"
  done
  rm -f $O/libuamedia.a $O/core.a
  $AR rcs $O/core.a $OBJS
  { echo "create $O/libuamedia.a"; echo "addlib $O/core.a"
    for l in $LIBDIR/libvpx.a $LIBDIR/libvorbis.a $LIBDIR/libogg.a $LIBDIR/libopus.a $H264; do echo "addlib $l"; done
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
      $LIBDIR/libvorbisenc.a $LIBDIR/libvpx.a $LIBDIR/libvorbis.a $LIBDIR/libogg.a $LIBDIR/libopus.a $H264 -lstdc++ -ldl -lpthread -lm
    $CC $CFLAGS -I. -o $O/test/test_surface_gl test/test_surface_gl.c $O/libuamedia.a -lEGL -lGLESv2 -lstdc++ -ldl -lpthread -lm
    JNI=../jni/out; [ "$O" = out/arm64 ] && JNI=../jni/out/arm64
    if [ -f $JNI/libfakejni.a ]; then
      $CC $CFLAGS -I. -o $O/test/test_media_jni test/test_media_jni.c test/clips.c test/webm_writer.c $JNI/libfakejni.a \
        $O/libuamedia.a $LIBDIR/libvorbisenc.a $LIBDIR/libvpx.a $LIBDIR/libvorbis.a $LIBDIR/libogg.a $LIBDIR/libopus.a \
        -lz -lstdc++ -ldl -lpthread -lm
      $CC $CFLAGS -I. -I../android -o $O/test/test_media_ndk test/test_media_ndk.c test/clips.c test/webm_writer.c \
        ../android/media_ndk.c ../android/native_window.c ../android/log_shim.c $JNI/libfakejni.a \
        $O/libuamedia.a $LIBDIR/libvorbisenc.a $LIBDIR/libvpx.a $LIBDIR/libvorbis.a $LIBDIR/libogg.a $LIBDIR/libopus.a \
        -lz -lstdc++ -ldl -lpthread -lm
    else echo "no $JNI/libfakejni.a: build ../jni first for the Java-side and NDK tests"; fi
  fi
'
if [ "$1" = test-ffmpeg ]; then
  # the FFmpeg decoders against clips FFmpeg's tools make (Debian's FFmpeg in a test image)
  P=linux/arm/v7; [ "$ARCH" = arm64 ] && P=linux/arm64
  FI=ua-media-ffmpeg-$ARCH
  docker image inspect $FI >/dev/null 2>&1 || docker build --platform $P -t $FI -f docker/Dockerfile.ffmpeg-test docker
  docker run --rm --platform $P -v "$PWD":/w -w /w $FI timeout 900 sh test/ffmpeg_clips.sh /tmp/clips ./$O/test/test_media_ffmpeg
fi
if [ "$1" = test ]; then
  if [ "$ARCH" = armhf ]; then
    docker run --rm --platform linux/arm/v7 -v "$PWD":/w -w /w debian:bookworm-slim sh -c \
      'timeout 900 ./out/test/test_media; [ ! -x ./out/test/test_media_jni ] || timeout 900 ./out/test/test_media_jni; [ ! -x ./out/test/test_media_ndk ] || timeout 900 ./out/test/test_media_ndk'
    docker image inspect ua-emu-armhf >/dev/null 2>&1 && \
      docker run --rm --platform linux/arm/v7 -v "$PWD/../..":/r -w /r/runtime/media -e EGL_PLATFORM=surfaceless \
        -e LIBGL_ALWAYS_SOFTWARE=1 ua-emu-armhf timeout 600 ./out/test/test_surface_gl ../../convert/pmunity/video_decode_android.shader
  else
    docker run --rm $PLAT -v "$PWD":/w -w /w $IMG sh -c \
      'timeout 900 ./out/arm64/test/test_media; [ ! -x ./out/arm64/test/test_media_jni ] || timeout 900 ./out/arm64/test/test_media_jni; [ ! -x ./out/arm64/test/test_media_ndk ] || timeout 900 ./out/arm64/test/test_media_ndk'
    docker run --rm $PLAT -v "$PWD/../..":/r -w /r/runtime/media -e EGL_PLATFORM=surfaceless -e LIBGL_ALWAYS_SOFTWARE=1 \
      $IMG timeout 600 ./out/arm64/test/test_surface_gl ../../convert/pmunity/video_decode_android.shader
  fi
fi
