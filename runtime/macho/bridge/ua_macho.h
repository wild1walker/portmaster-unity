// ua_macho.h - what the host (../../host/macho64.c) hands the Mach-O bridge at start.
#ifndef UA_MACHO_BRIDGE_H
#define UA_MACHO_BRIDGE_H

#include <stddef.h>

struct dl_phdr_info;

struct ua_macho_host {
    size_t size;
    // the runtime's loader: the game's plugins are Android libraries (bionic's flag values)
    void *(*dlopen)(const char *path, int bionic_flags);
    void *(*dlsym)(void *handle, const char *name);
    int   (*dlclose)(void *handle);
    // the Android libraries' program headers, then the host's: an exception an engine call
    // raises unwinds through libunity.so's frames
    int   (*iterate_phdr)(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data);
    // a signal no mask the Android libraries set may block (the bridge stops threads with it)
    void  (*keep_unblocked)(int sig);
    // hook() on every thread the process makes from then on, before its start routine: each gets
    // its alternate signal stack there (macOS code keeps data below the stack pointer), also
    // one that first runs the game's code through a callback and not through an il2cpp_* call
    void  (*thread_start_hook)(void (*hook)(void));
    const char *libdir;         // GameAssembly.dylib, libc++.1.dylib and libc++abi.1.dylib are there
};
typedef int (*ua_macho_setup_fn)(const struct ua_macho_host *host);

#endif
