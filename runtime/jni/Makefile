# fakejni + C UnityPlayer, armhf (Debian bookworm cross gcc, glibc 2.36).
# Usage (from the Mac):   ./build.sh            (builds in docker, runs the QEMU unit test)
# Inside the container:   make all test-qemu

CROSS   ?= arm-linux-gnueabihf-
CC      := $(CROSS)gcc
AR      := $(CROSS)ar
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -fPIC -Wall -Wextra -Wno-unused-parameter -march=armv7-a -mfpu=neon -mfloat-abi=hard
OUT     := out

LIB_OBJS := $(OUT)/fakejni.o $(OUT)/unityplayer.o $(OUT)/up_android.o $(OUT)/up_input.o

all: $(OUT)/libfakejni.a $(OUT)/test_fakejni $(OUT)/test_unityplayer

$(OUT):
	mkdir -p $(OUT)

$(OUT)/fakejni.o: fakejni.c fakejni.h include/jni.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/%.o: %.c unityplayer.h up_internal.h fakejni.h include/jni.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/libfakejni.a: $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# "Android side" of the tests: built like NDK armeabi-v7a code (softfp calling convention).
$(OUT)/android_side.o: test/android_side.c include/jni.h | $(OUT)
	$(CC) -std=gnu11 -O2 -g -Wall -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp \
	      -ffreestanding -fno-builtin -fno-stack-protector -c $< -o $@

$(OUT)/fake_unity.o: test/fake_unity.c include/jni.h | $(OUT)
	$(CC) -std=gnu11 -O2 -g -Wall -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp \
	      -ffreestanding -fno-builtin -fno-stack-protector -c $< -o $@

$(OUT)/test_fakejni: test/test_fakejni.c $(OUT)/android_side.o $(OUT)/libfakejni.a
	$(CC) $(CFLAGS) $< $(OUT)/android_side.o -L$(OUT) -lfakejni -lz -lpthread -lm \
	      -Wl,--no-warn-mismatch -o $@

$(OUT)/test_unityplayer: test/test_unityplayer.c $(OUT)/fake_unity.o $(OUT)/libfakejni.a
	$(CC) $(CFLAGS) $< $(OUT)/fake_unity.o -L$(OUT) -lfakejni -lz -lpthread -lm \
	      -Wl,--no-warn-mismatch -o $@

# run with the qemu-user binary of the build container
test-qemu: $(OUT)/test_fakejni $(OUT)/test_unityplayer
	qemu-arm -L /usr/arm-linux-gnueabihf $(OUT)/test_fakejni
	qemu-arm -L /usr/arm-linux-gnueabihf $(OUT)/test_unityplayer

clean:
	rm -rf $(OUT)

.PHONY: all clean test-qemu
