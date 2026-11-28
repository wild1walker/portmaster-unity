# fakejni + C UnityPlayer, arm64: native gcc of the ua-build-arm64 image (Ubuntu 20.04, glibc 2.31)
# -> out/arm64/. (32-bit armhf was removed; the last tree with it is the tag last-armhf.)
# Usage (from the Mac):   ./build.sh   (docker; builds and runs the tests)
# Inside the container:   make all test

ARCH    ?= arm64
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -fPIC -ffunction-sections -Wall -Wextra -Wno-unused-parameter

ifneq ($(ARCH),arm64)
$(error ARCH must be arm64 (armhf was removed: tag last-armhf))
endif
CROSS   ?=
OUT     := out/arm64
# arm64-v8a code and the host share AAPCS64: the "Android side" of the tests is ordinary code
ANDROID_CFLAGS := -std=gnu11 -O2 -g -Wall
TEST_LDFLAGS   :=

CC      := $(CROSS)gcc
AR      := $(CROSS)ar

LIB_OBJS := $(OUT)/fakejni.o $(OUT)/unityplayer.o $(OUT)/up_android.o $(OUT)/up_input.o $(OUT)/up_softinput.o $(OUT)/up_media.o \
            $(OUT)/up_tls.o $(OUT)/up_fmod.o $(OUT)/up_http.o $(OUT)/up_unity6.o $(OUT)/up_legacy.o $(OUT)/up_services.o

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
	$(CC) $(CFLAGS) $< $(OUT)/android_side.o -L$(OUT) -lfakejni -lz -ldl -lpthread -lm \
	      $(TEST_LDFLAGS) -o $@

$(OUT)/test_unityplayer: test/test_unityplayer.c test/fake_unity.h $(OUT)/fake_unity.o $(OUT)/libfakejni.a
	$(CC) $(CFLAGS) $< $(OUT)/fake_unity.o -L$(OUT) -lfakejni -lz -ldl -lpthread -lm \
	      $(TEST_LDFLAGS) -o $@

# run natively (in ua-build-arm64, or on the device)
test: $(OUT)/test_fakejni $(OUT)/test_unityplayer
	$(OUT)/test_fakejni
	$(OUT)/test_unityplayer

clean:
	rm -f $(OUT)/*.o $(OUT)/*.a $(OUT)/test_fakejni $(OUT)/test_unityplayer

.PHONY: all clean test
