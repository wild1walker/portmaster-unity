// il2cpp_mono.h - what ua_host gives the Mono bridge (il2cpp_mono.c) before libunity loads it.
#ifndef IL2CPP_MONO_H
#define IL2CPP_MONO_H

struct ua_mono_host {
    int size;                                         /* sizeof(struct ua_mono_host) */
    /* the loader's dlopen/dlsym: the game's P/Invokes into Android libraries and libunity */
    void *(*dlopen)(const char *name, int flags);
    void *(*dlsym)(void *handle, const char *name);
    const char *libdir;                               /* the player's lib folder */
    const char *unity_version;                        /* the player's, e.g. "2019.4.8f1" */
    const char *managed_dir;                          /* NULL: found from il2cpp_set_data_dir */
    int gc_time_slice_ms;                             /* boot.config's gc-max-time-slice: the incremental
                                                         collector, as the desktop player runs it (0: off) */
};

/* exported by libil2cpp_mono.so; returns 0 */
typedef int (*ua_mono_setup_fn)(const struct ua_mono_host *host);

#endif
