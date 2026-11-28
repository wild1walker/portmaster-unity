#!/bin/bash
# Builds the host programs: ua_host (loader + android shims + fake JNI + media core), and the device
# checks egltest / passbench / drawbench / glreplay (UA_GL_REC_FILE traces against any GLES driver) / sdltest / audiocheck / shadercheck / computecheck, gpucnt (GPU counters; static), hwcount (CPU counters; static) and
# wwrender (the Wwise player offline: docs/plugins.md, "The player"). Build ../loader, ../android, ../jni and ../media
# first (../build.sh does). Native build in the ua-build-arm64 image (Ubuntu 20.04, glibc 2.31; its
# SDL2, EGL and GLES) -> out/arm64/. ua_host is linked once ../loader/out/arm64/libsoloader.a exists;
# until then its objects are still compiled, so the sources are checked. (32-bit armhf was removed;
# the last tree with it is the tag last-armhf.)
#   STEAMSTUB=0: ua_host without the Steamworks stand-ins (steamstub.c, plugstubs.c's CSteamworks
#     table), for ports of games that do not use Steamworks
# ua_host is stripped (a port ships it: its debug info was two thirds of the file); the file as linked
# stays beside it as ua_host.debug, which tools/analysis reads for the names of its functions
# (profsum.py --lib). What runs on the handheld names functions by the exported symbols only
# (so_dladdr), which stripping keeps.
set -e
cd "$(dirname "$0")"
ARCH=${ARCH:-arm64}
[ "$ARCH" = arm64 ] || { echo "ARCH must be arm64 (armhf was removed: tag last-armhf)"; exit 1; }
STEAM=steamstub; NOSTEAM=
[ "${STEAMSTUB:-1}" = 0 ] && { STEAM=; NOSTEAM=-DUA_NO_STEAMSTUB; }
export STEAM NOSTEAM
mkdir -p out/arm64
docker image inspect ua-build-arm64 >/dev/null 2>&1 || docker build --platform linux/arm64 -t ua-build-arm64 ../docker/arm64
docker run --rm --pull=never --platform linux/arm64 -e STEAM -e NOSTEAM -v "$PWD/..":/w -w /w/host ua-build-arm64 bash -c '
  set -e
  O=out/arm64
  F="-O2 -g -Wall -I../jni/include -I../jni $NOSTEAM"
  OBJS=
  # hot-arm64.txt: the functions of ours the game threads run most (from UA_PROF recordings), each in its own
  # section (-ffunction-sections) renamed .text.hot.<name>: the linker puts them together before the rest of .text
  HOT=; [ -f hot-arm64.txt ] && HOT=$(grep -v "^#" hot-arm64.txt | while read n; do [ -n "$n" ] && printf -- "--rename-section .text.%s=.text.hot.%s " "$n" "$n"; done)
  for f in ua_host input pointer osk frames framerate gcbudget uinav monoinvoke jitlog jitwarm guiwarm lockstep monoaot quality glhooks hitches prof threads futex gltrace glfill gltexdump monodump glopt glpersist glstate glarena glpresent glmemo gpuidle glpass glspec glrename glrec glrec_fg glmerge msaafix display display_sdl fbdev gldriver $STEAM plugins plugstubs fmodstub wwise desktopenv mono64 unityhooks fmodasync unitypool unityverts unityjobs unityprefetch nativebeh enginerw unityphys2d icallcount memledger monoimage glprobe; do gcc $F -ffunction-sections -c $f.c -o $O/$f.o; [ -z "$HOT" ] || objcopy $HOT $O/$f.o; OBJS="$OBJS $O/$f.o"; done
  gcc $F -c wwise_tramp.S -o $O/wwise_tramp.o; OBJS="$OBJS $O/wwise_tramp.o"
  gcc $F -c wwplay.c -o $O/wwplay.o; gcc -O3 -ffast-math -g -w -c wwplay_vorbis.c -o $O/wwplay_vorbis.o; OBJS="$OBJS $O/wwplay.o $O/wwplay_vorbis.o"
  gcc $F -O3 -c fmplay.c -o $O/fmplay.o; OBJS="$OBJS $O/fmplay.o"
  # AArch64 unwinds with .eh_frame: the unwinders linked into the Android libraries find our modules through the loader'"'"'s
  # dl_iterate_phdr (docs/arm64.md): no flags for that. ioctl is exported (fbdev.c interposes the
  # GLES driver'"'"'s calls; this ld predates --export-dynamic-symbol). -lrt: libhybris'"'"'s timer_* and shm_open
  # are there before glibc 2.34. The loader must be in the executable itself (its thread-local
  # block holds bionic'"'"'s stack guard slot, so_loader.h).
  LOADER=../loader/out/arm64/libsoloader.a
  if [ -f $LOADER ]; then
    SHIMS=../android/out/arm64/libandroidshims.a; FJ=../jni/out/arm64/libfakejni.a
    if [ -n "$HOT" ]; then
      objcopy $HOT $LOADER $O/hot-libsoloader.a; objcopy $HOT $SHIMS $O/hot-libandroidshims.a; objcopy $HOT $FJ $O/hot-libfakejni.a
      LOADER=$O/hot-libsoloader.a; SHIMS=$O/hot-libandroidshims.a; FJ=$O/hot-libfakejni.a
    fi
    g++ -o $O/ua_host $OBJS $LOADER $SHIMS $FJ \
      ../media/out/arm64/libuamedia.a -Wl,--dynamic-list=exports.list -lSDL2 -lz -ldl -lpthread -lm -lrt
    cp $O/ua_host $O/ua_host.debug; strip $O/ua_host
    file $O/ua_host | sed "s|, BuildID[^,]*||"
  else
    rm -f $O/ua_host
    echo "ua_host not linked: no arm64 loader yet ($LOADER); objects compiled in $O"
  fi
  gcc -O2 -Wall -rdynamic -o $O/egltest egltest.c -lEGL -lGLESv2
  gcc -O2 -Wall -o $O/passbench passbench.c -lEGL -lGLESv2
  gcc -O2 -Wall -o $O/drawbench drawbench.c -lEGL -lGLESv2
  gcc -O2 -g -Wall -o $O/glreplay glreplay.c glreplay_hash.c glrename.c glpass.c glarena.c -ldl -lz -lpthread
  gcc -O2 -g -Wall -static -o $O/glpixdiff glpixdiff.c
  gcc -O2 -Wall -o $O/sdltest sdltest.c -lSDL2
  gcc -O2 -Wall -o $O/audiocheck audiocheck.c -lSDL2
  gcc -O2 -Wall -o $O/shadercheck shadercheck.c -lEGL -lGLESv2
  gcc -O2 -Wall -o $O/computecheck computecheck.c -lEGL -lGLESv2
  gcc -O2 -Wall -static -o $O/gpucnt gpucnt.c
  gcc -O2 -Wall -static -o $O/hwcount hwcount.c
  gcc -O2 -Wall -static -o $O/relayprof relayprof.c ../loader/relayout.c
  rm -rf $O/relayout; cp -r ../relayout $O/relayout; rm -f $O/relayout/README.md
  gcc -O2 -g -Wall -o $O/wwrender wwrender.c $O/wwplay.o $O/wwplay_vorbis.o -lpthread -lm
  gcc -O2 -g -Wall -o $O/fmrender fmrender.c $O/fmplay.o $O/wwplay_vorbis.o -lpthread -lm'
