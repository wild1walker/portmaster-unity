// The Mach-O route (docs/macho-route.md): a macOS IL2CPP game's own arm64 code, its
// GameAssembly.dylib, on Unity's arm64-v8a IL2CPP player of the same version. libunity.so loads
// the bridge (../macho/bridge: the Mach-O loader, the Darwin shims and a forwarder per il2cpp_*
// function) as its libil2cpp.so, as it loads the Mono route's (mono64.c): a host (glibc) library
// in the player's lib folder under IL2CPP's name, beside GameAssembly.dylib and the libc++ built
// as Mach-O. The game's plugins are Android libraries: the bridge opens them with the loader.
//   UA_SCRIPTING=macho  the bridge whatever android_package.json says ("scripting": "macho" there)
// arm64 only: a macOS game has no 32-bit ARM code.
#include "macho64.h"
#include "../loader/so_loader.h"
#include "../macho/bridge/ua_macho.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int macho64_init(const char *libdir, const char *scripting)
{
    const char *e = getenv("UA_SCRIPTING");
    const char *want = e && *e ? e : scripting;
    if (!want || strcmp(want, "macho") != 0) return 0;
#if !defined(__aarch64__)
    fprintf(stderr, "[host] scripting macho: a macOS game's code is arm64, this host is not\n");
    return -1;
#else
    static char bridge[600];
    snprintf(bridge, sizeof bridge, "%s/libil2cpp.so", libdir);
    if (access(bridge, R_OK) != 0) {
        fprintf(stderr, "[host] scripting macho: no %s (the Mach-O route's bridge goes there)\n", bridge);
        return -1;
    }
    void *h = dlopen(bridge, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "[host] %s: %s\n", bridge, dlerror()); return -1; }
    ua_macho_setup_fn setup = (ua_macho_setup_fn)dlsym(h, "ua_macho_setup");
    if (!setup) {
        fprintf(stderr, "[host] %s: not the Mach-O bridge (no ua_macho_setup: an IL2CPP build's?)\n", bridge);
        return -1;
    }
    static struct ua_macho_host host;
    host.size = sizeof host;
    host.dlopen = so_dlopen;
    host.dlsym = so_dlsym;
    host.dlclose = so_dlclose;
    host.iterate_phdr = so_dl_iterate_phdr;
    host.keep_unblocked = so_signal_keep_unblocked;
    host.thread_start_hook = so_set_thread_start_hook;
    host.libdir = libdir;
    if (setup(&host) != 0) { fprintf(stderr, "[host] %s: the game's code did not load\n", bridge); return -1; }
    // macOS code keeps data below the stack pointer: no handler may run on its stack
    so_signal_handlers_onstack();
    so_map_library("libil2cpp.so", bridge);
    fprintf(stderr, "[host] scripting: a macOS game's GameAssembly.dylib on the IL2CPP player (%s)\n", bridge);
    return 1;
#endif
}
