// Mono's ahead-of-time compiler and images with the player's own libmono (docs/runtime.md):
//   UA_MONO_MAIN="<args>"  instead of the game: lib/libmono.so (a Mono player's) run as the mono program
//                          through its exported mono_main, e.g. its AOT compiler:
//                          "--runtime=v2.0.50727 --aot=nodebug,outfile=<out>,tool-prefix=<as/ld prefix> <assembly>"
//                          (MONO_PATH: the assemblies it references; UA_MONO_DIRS="<root>:<config dir>" for
//                          mono_set_dirs, where Mono looks for <root>/mscorlib.dll). Mono's default file maps
//                          read bionic FILE* fields inline (libmono is compiled against bionic's stdio), which a
//                          host FILE* does not have: Unity installs its own maps, and so does this mode.
//   UA_MONO_AOT=<dir>      the images (<assembly>.dll.so) served from <dir>: Mono dlopens each beside its
//                          assembly, a path inside the OBB on the Android player; Mono checks an image
//                          against its assembly itself and keeps the JIT for whatever an image lacks.
#define _GNU_SOURCE
#include "monoaot.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

const char *monoaot_rewrite(const char *path, char *buf, size_t size)
{
    static int told;
    const char *dir = getenv("UA_MONO_AOT");
    size_t l = path ? strlen(path) : 0;
    if (!dir || !*dir || l < 7 || strcmp(path + l - 7, ".dll.so")) return NULL;
    const char *b = strrchr(path, '/');
    if ((size_t)snprintf(buf, size, "%s/%s", dir, b ? b + 1 : path) >= size || access(buf, R_OK) != 0) return NULL;
    if (told++ < 100) fprintf(stderr, "[host] Mono AOT image %s\n", buf);
    return buf;
}

// the file maps: a map is the descriptor + 1 (NULL: none)
static void *fm_open(const char *name) { int fd = open(name, O_RDONLY | O_CLOEXEC); return fd < 0 ? NULL : (void *)(intptr_t)(fd + 1); }
static uint64_t fm_size(void *f) { struct stat st; return fstat((int)(intptr_t)f - 1, &st) == 0 ? (uint64_t)st.st_size : 0; }
static int fm_fd(void *f) { return (int)(intptr_t)f - 1; }
static int fm_close(void *f) { return close((int)(intptr_t)f - 1); }
static void *fm_map(size_t length, int flags, int fd, uint64_t offset, void **handle)
{
    // Mono's flags: READ 1, WRITE 2, EXEC 4, DISCARD 8, PRIVATE 16, SHARED 32
    int prot = (flags & 1 ? PROT_READ : 0) | (flags & 2 ? PROT_WRITE : 0) | (flags & 4 ? PROT_EXEC : 0);
    void *p = mmap(NULL, length, prot, flags & 32 ? MAP_SHARED : MAP_PRIVATE, fd, (off_t)offset);
    if (p == MAP_FAILED) return NULL;
    *handle = (void *)length;
    return p;
}
static int fm_unmap(void *addr, void *handle) { return munmap(addr, (size_t)handle); }

int monoaot_main(void *mono, void *(*sym)(void *, const char *), const char *args)
{
    int (*m_main)(int, char **) = (int (*)(int, char **))sym(mono, "mono_main");
    if (!m_main) { fprintf(stderr, "[host] UA_MONO_MAIN: no mono_main in the Mono library\n"); return 1; }
    void (*m_override)(void *, void *, void *, void *, void *, void *) =
        (void (*)(void *, void *, void *, void *, void *, void *))sym(mono, "mono_file_map_override");
    if (m_override) m_override((void *)fm_open, (void *)fm_size, (void *)fm_fd, (void *)fm_close, (void *)fm_map, (void *)fm_unmap);
    const char *dirs = getenv("UA_MONO_DIRS");
    void (*m_set_dirs)(const char *, const char *) = (void (*)(const char *, const char *))sym(mono, "mono_set_dirs");
    if (dirs && *dirs && m_set_dirs) {
        static char d[1024];
        snprintf(d, sizeof d, "%s", dirs);
        char *c = strchr(d, ':');
        if (c) *c++ = 0;
        m_set_dirs(d, c && *c ? c : NULL);
    }
    static char buf[8192];
    static char *argv[256];
    int argc = 0;
    argv[argc++] = "mono";
    snprintf(buf, sizeof buf, "%s", args ? args : "");
    for (char *t = strtok(buf, " "); t && argc < 255; t = strtok(NULL, " ")) argv[argc++] = t;
    argv[argc] = NULL;
    fprintf(stderr, "[host] UA_MONO_MAIN: mono_main with %d argument(s): %s\n", argc - 1, args ? args : "");
    int r = m_main(argc, argv);
    fprintf(stderr, "[host] UA_MONO_MAIN: mono_main returned %d\n", r);
    return r;
}
