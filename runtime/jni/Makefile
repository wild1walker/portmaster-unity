# fakejni + C UnityPlayer.
#   ARCH=armhf (default): Debian bookworm cross gcc (glibc 2.36) -> out/
#   ARCH=arm64: native gcc of the ua-build-arm64 image (Ubuntu 20.04, glibc 2.31) -> out/arm64/
# Usage (from the Mac):   ./build.sh  /  ARCH=arm64 ./build.sh   (docker; builds and runs the tests)
# Inside the container:   make all test-qemu  /  make ARCH=arm64 all test

ARCH    ?= armhf
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -fPIC -Wall -Wextra -Wno-unused-parameter

ifeq ($(ARCH),arm64)
CROSS   ?=
OUT     := out/arm64
# arm64-v8a code and the host share AAPCS64: the "Android side" of the tests is ordinary code
ANDROID_CFLAGS := -std=gnu11 -O2 -g -Wall
TEST_LDFLAGS   :=
else ifeq ($(ARCH),armhf)
CROSS   ?= arm-linux-gnueabihf-
OUT     := out
CFLAGS  += -march=armv7-a -mfpu=neon -mfloat-abi=hard
# "Android side" of the tests: built like NDK armeabi-v7a code (softfp calling convention)
ANDROID_CFLAGS := -std=gnu11 -O2 -g -Wall -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp \
                  -ffreestanding -fno-builtin -fno-stack-protector
TEST_LDFLAGS   := -Wl,--no-warn-mismatch
else
$(error ARCH must be armhf or arm64)
endif

CC      := $(CROSS)gcc
AR      := $(CROSS)ar

LIB_OBJS := $(OUT)/fakejni.o $(OUT)/unityplayer.o $(OUT)/up_android.o $(OUT)/up_input.o $(OUT)/up_media.o

all: $(OUT)/libfakejni.a $(OUT)/test_fakejni $(OUT)/test_unityplayer

$(OUT):
	mkdir -p $(OUT)

$(OUT)/fakejni.o: fakejni.c fakejni.h include/jni.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/%.o: %.c unityplayer.h up_internal.h fakejni.h include/jni.h ../media/um.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/libfakejni.a: $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(OUT)/android_side.o: test/android_side.c include/jni.h | $(OUT)
	$(CC) $(ANDROID_CFLAGS) -c $< -o $@

$(OUT)/fake_unity.o: test/fake_unity.c test/fake_unity.h include/jni.h | $(OUT)
	$(CC) $(ANDROID_CFLAGS) -c $< -o $@

$(OUT)/test_fakejni: test/test_fakejni.c $(OUT)/android_side.o $(OUT)/libfakejni.a
	$(CC) $(CFLAGS) $< $(OUT)/android_side.o -L$(OUT) -lfakejni -lz -lpthread -lm \
	      $(TEST_LDFLAGS) -o $@

$(OUT)/test_unityplayer: test/test_unityplayer.c test/fake_unity.h $(OUT)/fake_unity.o $(OUT)/libfakejni.a
	$(CC) $(CFLAGS) $< $(OUT)/fake_unity.o -L$(OUT) -lfakejni -lz -lpthread -lm \
	      $(TEST_LDFLAGS) -o $@

# run natively (arm64 in ua-build-arm64, or armhf on the device / an arm/v7 container)
test: $(OUT)/test_fakejni $(OUT)/test_unityplayer
	$(OUT)/test_fakejni
	$(OUT)/test_unityplayer

# run with the qemu-user binary of the armhf build container
test-qemu: $(OUT)/test_fakejni $(OUT)/test_unityplayer
	qemu-arm -L /usr/arm-linux-gnueabihf $(OUT)/test_fakejni
	qemu-arm -L /usr/arm-linux-gnueabihf $(OUT)/test_unityplayer

clean:
	rm -f $(OUT)/*.o $(OUT)/*.a $(OUT)/test_fakejni $(OUT)/test_unityplayer

.PHONY: all clean test test-qemu
