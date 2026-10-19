// hitches.c check: a thread named UnityMain does 60 fps "frames", one of them 300 ms long
// (busy in spin()); the sampler must print samples and a "frame of" line. Run under
// docker --platform linux/arm/v7 (QEMU) or on the device.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../hitches.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
__attribute__((noinline)) static volatile double spin(double secs) { double t = now(), x = 0; while (now() - t < secs) x += 1; return x; }
__attribute__((noinline)) static void frame(int i) { spin(i == 30 ? 0.3 : 0.016); }

static void *game(void *a)
{
    (void)a;
    pthread_setname_np(pthread_self(), "UnityMain");
    for (int i = 0; i < 60; i++) { frame(i); hitches_frame(); }
    return NULL;
}

int main(void)
{
    setenv("UA_HITCH_TRACE", "100", 1);
    hitches_init();
    pthread_t t;
    pthread_create(&t, NULL, game, NULL);
    pthread_join(t, NULL);
    struct timespec w = { 0, 50000000 }; nanosleep(&w, NULL);
    return 0;
}
