// The Mono route on the 64-bit player (docs/mono-arm64.md): a Mono desktop game's own assemblies
// on Unity's arm64-v8a IL2CPP player, whose libunity.so then loads the bridge
// (../mono64/il2cpp_mono.c: the il2cpp_* API on Unity's Mono, libmonobdwgc-2.0.so built for
// aarch64 Linux) as its libil2cpp.so. Both are host (glibc) libraries in the player's lib folder,
// the bridge under IL2CPP's name: libunity checks that the file is there, then dlopens it, and the
// Java side System.loads it; both are mapped to the host library, which gets the loader's
// dlopen/dlsym first for the game's P/Invokes. (One file: the same library under two names would
// be loaded twice where the file system has no links, as on the handhelds' exFAT cards.)
//   UA_SCRIPTING=mono  the bridge whatever android_package.json says ("scripting": "mono" there,
//                      which `pmunity android-data --mono64` writes); il2cpp: never
#include "mono64.h"
#include "../loader/so_loader.h"
#include "../mono64/il2cpp_mono.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *so_dlopen(const char *filename, int flags);
void *so_dlsym(void *handle, const char *symbol);

int mono64_init(const char *libdir, const char *scripting, const char *unity_version, int gc_time_slice_ms)
{
    const char *e = getenv("UA_SCRIPTING");
    const char *want = e && *e ? e : scripting;
    if (!want || strcmp(want, "mono") != 0) return 0;
    static char bridge[600];
    snprintf(bridge, sizeof bridge, "%s/libil2cpp.so", libdir);
    if (access(bridge, R_OK) != 0) {
        fprintf(stderr, "[host] scripting mono: no %s (the Mono route's bridge and libmonobdwgc-2.0.so go there)\n", bridge);
        return -1;
    }
    if (unity_version && *unity_version) setenv("UA_UNITY_VERSION", unity_version, 0);
    void *h = dlopen(bridge, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "[host] %s: %s\n", bridge, dlerror()); return -1; }
    ua_mono_setup_fn setup = (ua_mono_setup_fn)dlsym(h, "ua_mono_setup");
    static struct ua_mono_host host;
    host.size = sizeof host;
    host.dlopen = so_dlopen;
    host.dlsym = so_dlsym;
    host.libdir = libdir;
    host.unity_version = unity_version;
    host.managed_dir = getenv("UA_MONO64_MANAGED");
    host.gc_time_slice_ms = gc_time_slice_ms;      // (UA_MONO64_GC_SLICE wins, in the bridge)
    if (!setup || setup(&host) != 0) {
        fprintf(stderr, "[host] %s: not the Mono bridge (no ua_mono_setup: an IL2CPP build's?)\n", bridge);
        return -1;
    }
    so_map_library("libil2cpp.so", bridge);
    fprintf(stderr, "[host] scripting: Mono on the IL2CPP player (%s)\n", bridge);
    return 1;
}
