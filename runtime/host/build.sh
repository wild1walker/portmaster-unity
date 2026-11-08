#!/bin/bash
# Builds the host programs: ua_host (loader + android shims + fake JNI + media core), and the device
# checks egltest / passbench / drawbench / sdltest / audiocheck / shadercheck, gpucnt (GPU counters; static) and
# wwrender (the Wwise player offline: docs/plugins.md, "The player"). Build ../loader, ../android, ../jni and ../media
# first for the same ARCH (../build.sh does).
#   ARCH=armhf (default): cross-compiled for the device's 32-bit userland in the ua-host-armhf image
#     -> out/
#   ARCH=arm64: native build in the ua-build-arm64 image (Ubuntu 20.04, glibc 2.31; its SDL2, EGL
#     and GLES) -> out/arm64/. ua_host is linked once ../loader/out/arm64/libsoloader.a exists;
#     until then its objects are still compiled, so the sources are checked.
#   STEAMSTUB=0: ua_host without the Steamworks stand-ins (steamstub.c, plugstubs.c's CSteamworks
#     table), for ports of games that do not use Steamworks
set -e
cd "$(dirname "$0")"
ARCH=${ARCH:-armhf}
STEAM=steamstub; NOSTEAM=
[ "${STEAMSTUB:-1}" = 0 ] && { STEAM=; NOSTEAM=-DUA_NO_STEAMSTUB; }
export STEAM NOSTEAM
if [ "$ARCH" = arm64 ]; then
  mkdir -p out/arm64
  docker image inspect ua-build-arm64 >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-build-arm64 ../docker/arm64
  docker run --rm --pull=never --platform linux/arm64 -e STEAM -e NOSTEAM -v "$PWD/..":/w -w /w/host ua-build-arm64 bash -c '
    set -e
    O=out/arm64
    F="-O2 -g -Wall -I../jni/include -I../jni $NOSTEAM"
    OBJS=
    for f in ua_host input pointer osk frames framerate gcbudget uinav jitlog jitwarm monoaot quality glhooks hitches prof threads futex gltrace glfill gltexdump monodump glopt glpersist glstate glarena glpresent glmemo gpuidle glpass glspec glrename glrec glrec_fg glmerge msaafix display display_sdl fbdev $STEAM plugins plugstubs wwise desktopenv mono64 unityhooks unitypool unityverts; do gcc $F -c $f.c -o $O/$f.o; OBJS="$OBJS $O/$f.o"; done
    gcc $F -c wwise_tramp.S -o $O/wwise_tramp.o; OBJS="$OBJS $O/wwise_tramp.o"
    gcc $F -c wwplay.c -o $O/wwplay.o; gcc -O3 -ffast-math -g -w -c wwplay_vorbis.c -o $O/wwplay_vorbis.o; OBJS="$OBJS $O/wwplay.o $O/wwplay_vorbis.o"
    # No __gnu_Unwind_Find_exidx export: that is ARM EHABI. AArch64 unwinds with .eh_frame, and the
    # unwinders linked into the Android libraries find our modules through the loader'"'"'s
    # dl_iterate_phdr (docs/arm64.md): no flags for that. ioctl is exported (fbdev.c interposes the
    # GLES driver'"'"'s calls; this ld predates --export-dynamic-symbol). -lrt: libhybris'"'"'s timer_* and shm_open
    # are there before glibc 2.34. The loader must be in the executable itself (its thread-local
    # block holds bionic'"'"'s stack guard slot, so_loader.h).
    LOADER=../loader/out/arm64/libsoloader.a
    if [ -f $LOADER ]; then
      g++ -o $O/ua_host $OBJS $LOADER ../android/out/arm64/libandroidshims.a ../jni/out/arm64/libfakejni.a \
        ../media/out/arm64/libuamedia.a -Wl,--dynamic-list=exports.list -lSDL2 -lz -ldl -lpthread -lm -lrt
      file $O/ua_host | sed "s|, BuildID[^,]*||"
    else
      rm -f $O/ua_host
      echo "ua_host not linked: no arm64 loader yet ($LOADER); objects compiled in $O"
    fi
    gcc -O2 -Wall -rdynamic -o $O/egltest egltest.c -lEGL -lGLESv2
    gcc -O2 -Wall -o $O/passbench passbench.c -lEGL -lGLESv2
    gcc -O2 -Wall -o $O/drawbench drawbench.c -lEGL -lGLESv2
    gcc -O2 -Wall -o $O/sdltest sdltest.c -lSDL2
    gcc -O2 -Wall -o $O/audiocheck audiocheck.c -lSDL2
    gcc -O2 -Wall -o $O/shadercheck shadercheck.c -lEGL -lGLESv2
    gcc -O2 -Wall -static -o $O/gpucnt gpucnt.c
    gcc -O2 -g -Wall -o $O/wwrender wwrender.c $O/wwplay.o $O/wwplay_vorbis.o -lpthread -lm'
  exit
fi
[ "$ARCH" = armhf ] || { echo "ARCH must be armhf or arm64"; exit 1; }
mkdir -p out
docker image inspect ua-host-armhf >/dev/null 2>&1 || docker build -q -t ua-host-armhf . >/dev/null
docker run --rm --pull=never -e STEAM -e NOSTEAM -v "$PWD/..":/w -w /w/host ua-host-armhf bash -c '
  set -e
  F="-O2 -g -Wall -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -funwind-tables -I../jni/include -I../jni $NOSTEAM"
  arm-linux-gnueabihf-gcc $F -c ua_host.c -o out/ua_host.o
  arm-linux-gnueabihf-gcc $F -c input.c -o out/input.o
  arm-linux-gnueabihf-gcc $F -c pointer.c -o out/pointer.o
  arm-linux-gnueabihf-gcc $F -c osk.c -o out/osk.o
  arm-linux-gnueabihf-gcc $F -c frames.c -o out/frames.o
  arm-linux-gnueabihf-gcc $F -c framerate.c -o out/framerate.o
  arm-linux-gnueabihf-gcc $F -c gcbudget.c -o out/gcbudget.o
  arm-linux-gnueabihf-gcc $F -c uinav.c -o out/uinav.o
  arm-linux-gnueabihf-gcc $F -c jitlog.c -o out/jitlog.o
  arm-linux-gnueabihf-gcc $F -c monoaot.c -o out/monoaot.o
  arm-linux-gnueabihf-gcc $F -c jitwarm.c -o out/jitwarm.o
  arm-linux-gnueabihf-gcc $F -c quality.c -o out/quality.o
  arm-linux-gnueabihf-gcc $F -c glhooks.c -o out/glhooks.o
  arm-linux-gnueabihf-gcc $F -c hitches.c -o out/hitches.o
  arm-linux-gnueabihf-gcc $F -c prof.c -o out/prof.o
  arm-linux-gnueabihf-gcc $F -c threads.c -o out/threads.o
  arm-linux-gnueabihf-gcc $F -c futex.c -o out/futex.o
  arm-linux-gnueabihf-gcc $F -c gltrace.c -o out/gltrace.o
  arm-linux-gnueabihf-gcc $F -c glfill.c -o out/glfill.o
  arm-linux-gnueabihf-gcc $F -c gltexdump.c -o out/gltexdump.o
  arm-linux-gnueabihf-gcc $F -c monodump.c -o out/monodump.o
  arm-linux-gnueabihf-gcc $F -c glopt.c -o out/glopt.o
  arm-linux-gnueabihf-gcc $F -c glpersist.c -o out/glpersist.o
  arm-linux-gnueabihf-gcc $F -c glstate.c -o out/glstate.o
  arm-linux-gnueabihf-gcc $F -c glarena.c -o out/glarena.o
  arm-linux-gnueabihf-gcc $F -c glpresent.c -o out/glpresent.o
  arm-linux-gnueabihf-gcc $F -c glmemo.c -o out/glmemo.o
  arm-linux-gnueabihf-gcc $F -c gpuidle.c -o out/gpuidle.o
  arm-linux-gnueabihf-gcc $F -c glpass.c -o out/glpass.o
  arm-linux-gnueabihf-gcc $F -c glspec.c -o out/glspec.o
  arm-linux-gnueabihf-gcc $F -c glrename.c -o out/glrename.o
  arm-linux-gnueabihf-gcc $F -c glrec.c -o out/glrec.o
  arm-linux-gnueabihf-gcc $F -c glrec_fg.c -o out/glrec_fg.o
  arm-linux-gnueabihf-gcc $F -c glmerge.c -o out/glmerge.o
  arm-linux-gnueabihf-gcc $F -c msaafix.c -o out/msaafix.o
  arm-linux-gnueabihf-gcc $F -c display.c -o out/display.o
  arm-linux-gnueabihf-gcc $F -c display_sdl.c -o out/display_sdl.o
  arm-linux-gnueabihf-gcc $F -c fbdev.c -o out/fbdev.o
  rm -f out/steamstub.o; [ -z "$STEAM" ] || arm-linux-gnueabihf-gcc $F -c steamstub.c -o out/steamstub.o
  arm-linux-gnueabihf-gcc $F -c plugins.c -o out/plugins.o
  arm-linux-gnueabihf-gcc $F -c plugstubs.c -o out/plugstubs.o
  arm-linux-gnueabihf-gcc $F -c wwise.c -o out/wwise.o
  arm-linux-gnueabihf-gcc $F -c desktopenv.c -o out/desktopenv.o
  arm-linux-gnueabihf-gcc $F -c wwise_tramp.S -o out/wwise_tramp.o
  # the Wwise player: NEON (every armeabi-v7a Unity player needs it); stb_vorbis at -O3 -ffast-math
  # (3 % faster on the RG34XX-SP)
  arm-linux-gnueabihf-gcc $F -mfpu=neon -c wwplay.c -o out/wwplay.o
  arm-linux-gnueabihf-gcc -O3 -ffast-math -g -w -march=armv7-a -mfpu=neon -mfloat-abi=hard -funwind-tables -c wwplay_vorbis.c -o out/wwplay_vorbis.o
  arm-linux-gnueabihf-gcc $F -c mono64.c -o out/mono64.o
  arm-linux-gnueabihf-gcc $F -c unityhooks.c -o out/unityhooks.o
  arm-linux-gnueabihf-gcc $F -c unitypool.c -o out/unitypool.o
  arm-linux-gnueabihf-gcc $F -ffp-contract=off -c unityverts.c -o out/unityverts.o
  arm-linux-gnueabihf-g++ -o out/ua_host out/ua_host.o out/input.o out/pointer.o out/osk.o out/frames.o out/framerate.o out/gcbudget.o out/uinav.o out/jitlog.o out/monoaot.o out/jitwarm.o out/quality.o out/glhooks.o out/hitches.o out/prof.o out/threads.o out/futex.o out/gltrace.o out/glfill.o out/gltexdump.o out/monodump.o out/glopt.o out/glpersist.o out/glstate.o out/glarena.o out/glpresent.o out/glmemo.o out/gpuidle.o out/glpass.o out/glspec.o out/glrename.o out/glrec.o out/glrec_fg.o out/glmerge.o out/msaafix.o \
    out/display.o out/display_sdl.o out/fbdev.o ${STEAM:+out/steamstub.o} out/plugins.o out/plugstubs.o out/wwise.o out/wwise_tramp.o out/wwplay.o out/wwplay_vorbis.o out/desktopenv.o out/mono64.o out/unityhooks.o out/unitypool.o out/unityverts.o ../loader/out/libsoloader.a ../android/out/libandroidshims.a \
    ../jni/out/libfakejni.a ../media/out/libuamedia.a -Wl,--export-dynamic-symbol=__gnu_Unwind_Find_exidx -Wl,--dynamic-list=exports.list \
    -lSDL2 -lz -ldl -lpthread -lm
  arm-linux-gnueabihf-gcc -O2 -Wall -rdynamic -o out/egltest egltest.c -lEGL -lGLESv2
  arm-linux-gnueabihf-gcc -O2 -Wall -o out/passbench passbench.c -lEGL -lGLESv2
  arm-linux-gnueabihf-gcc -O2 -Wall -o out/drawbench drawbench.c -lEGL -lGLESv2
  arm-linux-gnueabihf-gcc -O2 -Wall -o out/sdltest sdltest.c -lSDL2
  arm-linux-gnueabihf-gcc -O2 -Wall -o out/audiocheck audiocheck.c -lSDL2
  arm-linux-gnueabihf-gcc -O2 -Wall -o out/shadercheck shadercheck.c -lEGL -lGLESv2
  arm-linux-gnueabihf-gcc -O2 -Wall -static -o out/gpucnt gpucnt.c
  arm-linux-gnueabihf-gcc -O2 -g -Wall -o out/wwrender wwrender.c out/wwplay.o out/wwplay_vorbis.o -lpthread -lm
  file out/ua_host'
