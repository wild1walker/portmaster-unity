#define _GNU_SOURCE
// plugins.c: the native plugins of a port (docs/plugins.md). A desktop build's plugins are x86
// libraries; the port gives each library its managed code P/Invokes one of:
//   lib  <file>  an ARM build in lib/: an Android (bionic) library is loaded by the loader as
//                usual; a Linux (glibc) one is mapped to the host library (so_map_library)
//   host <lib>   a library of the handheld's system (e.g. libsqlite3.so.0)
//   stub <id>    a built-in library answering as a PC without that service (plugstubs.c, steamstub.c)
//   none         dlopen fails: DllNotFoundException, as on a PC without the library
// from plugins.map (pmunity android-data writes it beside the APK). Glibc libraries found in lib/
// are mapped even without a map line.
//
// 32-bit ARM: Mono (armeabi-v7a) passes float and double arguments in core registers (softfp), a
// glibc armhf library expects them in VFP registers. For every entry point with such arguments the
// map has a `sig` line (from the game's DllImport declarations); this file builds a thunk for it
// that moves the arguments as the VFP variant of the AAPCS wants them, and calls the library. On
// AArch64 both sides use AAPCS64 and nothing is converted.
//
// FMOD: Unity's StreamingAssets path is "jar:file://<apk or obb>!/assets" on Android, which FMOD's
// file system cannot open; the wrappers below give FMOD the file on disk (android-data puts the
// banks beside the APK, in assets/).
#include "plugins.h"
#ifndef UA_NO_STEAMSTUB
#include "steamstub.h"
#endif

#include <dlfcn.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG(...) fprintf(stderr, "[plugins] " __VA_ARGS__)

static char g_root[PATH_MAX], g_libdir[PATH_MAX];

// ------------------------------------------------------------------------------------- names

static void norm_name(const char *in, char *out, size_t n)
{
    const char *b = strrchr(in, '/');
    b = b ? b + 1 : in;
    snprintf(out, n, "%s", b);
    char *p = strstr(out, ".so");
    if (p && (p[3] == 0 || p[3] == '.')) *p = 0;
    size_t l = strlen(out);
    if (l > 4 && (!strcasecmp(out + l - 4, ".dll"))) out[l - 4] = 0;
    if (!strncmp(out, "lib", 3) && out[3]) memmove(out, out + 3, strlen(out + 3) + 1);
}

int plugins_name_variants(const char *name, char out[][256], int n)
{
    char base[256];
    norm_name(name, base, sizeof base);
    const char *forms[6] = { "%s", "lib%s.so", "lib%s", "%s.so", NULL, NULL };
    int k = 0;
    if (n > 0) snprintf(out[k++], 256, "%s", strrchr(name, '/') ? strrchr(name, '/') + 1 : name);
    for (int i = 0; forms[i] && k < n; i++) {
        char v[256];
        snprintf(v, sizeof v, forms[i], base);
        int dup = 0;
        for (int j = 0; j < k; j++) dup |= !strcmp(out[j], v);
        if (!dup) snprintf(out[k++], 256, "%s", v);
    }
    return k;
}

// ------------------------------------------------------------------------------------- ELF files

typedef struct { int ok, android, machine; char soname[128]; char needed[16][128]; int nneeded; } elf_facts;

// what a library file is: its machine, whether it is an Android library (an "Android" ELF note, or
// DT_NEEDED libc.so / liblog.so: bionic names), SONAME and DT_NEEDED entries
static int elf_read(const char *path, elf_facts *f)
{
    memset(f, 0, sizeof *f);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 64 || st.st_size > (512 << 20)) { close(fd); return 0; }
    unsigned char *d = malloc(st.st_size);
    if (!d || read(fd, d, st.st_size) != st.st_size) { free(d); close(fd); return 0; }
    close(fd);
    size_t size = st.st_size;
    if (memcmp(d, ELFMAG, 4) != 0) { free(d); return 0; }
    int is64 = d[EI_CLASS] == ELFCLASS64;
#define RD(T, off) (*(T *)(d + (off)))
    uint64_t phoff; unsigned phentsize, phnum;
    if (is64) { f->machine = RD(uint16_t, 18); phoff = RD(uint64_t, 32); phentsize = RD(uint16_t, 54); phnum = RD(uint16_t, 56); }
    else { f->machine = RD(uint16_t, 18); phoff = RD(uint32_t, 28); phentsize = RD(uint16_t, 42); phnum = RD(uint16_t, 44); }
    uint64_t dynoff = 0, dynsz = 0;
    struct { uint64_t va, off, sz; } load[16];
    int nload = 0;
    for (unsigned i = 0; i < phnum && phoff + (uint64_t)(i + 1) * phentsize <= size; i++) {
        uint64_t o = phoff + (uint64_t)i * phentsize, type, off, va, fsz;
        if (is64) { type = RD(uint32_t, o); off = RD(uint64_t, o + 8); va = RD(uint64_t, o + 16); fsz = RD(uint64_t, o + 32); }
        else { type = RD(uint32_t, o); off = RD(uint32_t, o + 4); va = RD(uint32_t, o + 8); fsz = RD(uint32_t, o + 16); }
        if (off + fsz > size) continue;
        if (type == PT_LOAD && nload < 16) { load[nload].va = va; load[nload].off = off; load[nload].sz = fsz; nload++; }
        if (type == PT_DYNAMIC) { dynoff = off; dynsz = fsz; }
        if (type == PT_NOTE && memmem(d + off, fsz, "Android", 7)) f->android = 1;
    }
    uint64_t strtab = 0, sonameoff = (uint64_t)-1, need[16];
    int nn = 0;
    size_t w = is64 ? 16 : 8;
    for (uint64_t o = dynoff; dynsz && o + w <= dynoff + dynsz; o += w) {
        int64_t tag = is64 ? RD(int64_t, o) : RD(int32_t, o);
        uint64_t val = is64 ? RD(uint64_t, o + 8) : RD(uint32_t, o + 4);
        if (tag == DT_NULL) break;
        if (tag == DT_STRTAB) strtab = val;
        if (tag == DT_SONAME) sonameoff = val;
        if (tag == DT_NEEDED && nn < 16) need[nn++] = val;
    }
    uint64_t so = 0;
    int have = 0;
    for (int i = 0; i < nload; i++)
        if (strtab >= load[i].va && strtab < load[i].va + load[i].sz) { so = strtab - load[i].va + load[i].off; have = 1; }
#define STR(v) (so + (v) < size ? (const char *)d + so + (v) : "")
    if (have) {
        if (sonameoff != (uint64_t)-1) snprintf(f->soname, sizeof f->soname, "%s", STR(sonameoff));
        for (int i = 0; i < nn; i++) {
            snprintf(f->needed[f->nneeded++], 128, "%s", STR(need[i]));
            if (!strcmp(STR(need[i]), "libc.so") || !strcmp(STR(need[i]), "liblog.so")) f->android = 1;
        }
    }
#undef STR
#undef RD
    free(d);
    f->ok = 1;
    return 1;
}

#if defined(__aarch64__)
#define HOST_MACHINE EM_AARCH64
#else
#define HOST_MACHINE EM_ARM
#endif

// ------------------------------------------------------------------------------------- the map

typedef struct { char name[128], use[8], arg[256]; } map_ent;
typedef struct { char module[128], entry[128], ret[8], args[160]; } sig_ent;
static map_ent *g_map; static int g_nmap;
static sig_ent *g_sig; static int g_nsig;

// host-mapped libraries: DllImport base name (normalized) -> what dlopen gets
typedef struct { char base[128], target[PATH_MAX]; } host_ent;
static host_ent g_host[64]; static int g_nhost;

static void read_map(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char a[16], b[128], c[128], d[256], e[160];
        line[strcspn(line, "\r\n#")] = 0;
        int n = sscanf(line, "%15s %127s %127s %255s %159s", a, b, c, d, e);
        if (n >= 3 && !strcmp(a, "map")) {
            g_map = realloc(g_map, (g_nmap + 1) * sizeof *g_map);
            map_ent *m = &g_map[g_nmap++];
            snprintf(m->name, sizeof m->name, "%s", b);
            snprintf(m->use, sizeof m->use, "%s", c);
            snprintf(m->arg, sizeof m->arg, "%s", n >= 4 ? d : "");
        } else if (n >= 5 && !strcmp(a, "sig")) {
            g_sig = realloc(g_sig, (g_nsig + 1) * sizeof *g_sig);
            sig_ent *s = &g_sig[g_nsig++];
            snprintf(s->module, sizeof s->module, "%s", b);
            snprintf(s->entry, sizeof s->entry, "%s", c);
            snprintf(s->ret, sizeof s->ret, "%s", d);
            snprintf(s->args, sizeof s->args, "%s", e);
        }
    }
    fclose(f);
    LOG("%s: %d libraries, %d float signatures\n", path, g_nmap, g_nsig);
}

static void map_variants(const char *name, const char *target)
{
    char v[6][256];
    int n = plugins_name_variants(name, v, 6);
    for (int i = 0; i < n; i++) so_map_library(v[i], target);
}

static void add_host(const char *name, const char *target)
{
    char base[128];
    norm_name(name, base, sizeof base);
    for (int i = 0; i < g_nhost; i++)
        if (!strcmp(g_host[i].base, base)) { snprintf(g_host[i].target, sizeof g_host[i].target, "%s", target); goto map; }
    if (g_nhost < 64) {
        snprintf(g_host[g_nhost].base, sizeof g_host[0].base, "%s", base);
        snprintf(g_host[g_nhost].target, sizeof g_host[0].target, "%s", target);
        g_nhost++;
    }
map:
    map_variants(name, target);
}

// a glibc library's DT_NEEDED entries that are in lib/ too (e.g. libfmodstudio.so.13 needs
// libfmod.so.13): loaded first, so that the dynamic linker finds them by SONAME
static void preload_siblings(const char *path, const elf_facts *f, int depth)
{
    if (depth > 4) return;
    for (int i = 0; i < f->nneeded; i++) {
        char p[PATH_MAX + 130];
        snprintf(p, sizeof p, "%s/%s", g_libdir, f->needed[i]);
        elf_facts g;
        int found = access(p, R_OK) == 0;
        if (!found) {                       // libfmod.so.13 shipped as libfmod.so
            char base[128];
            snprintf(base, sizeof base, "%s", f->needed[i]);
            char *s = strstr(base, ".so.");
            if (s) { s[3] = 0; snprintf(p, sizeof p, "%s/%s", g_libdir, base); found = access(p, R_OK) == 0; }
        }
        if (!found || !elf_read(p, &g) || g.android || g.machine != HOST_MACHINE) continue;
        preload_siblings(p, &g, depth + 1);
        void *h = dlopen(p, RTLD_NOW | RTLD_GLOBAL);
        LOG("%s needs %s: %s\n", path, f->needed[i], h ? p : dlerror());
    }
}

// lib/: every glibc library of this architecture becomes the host library for its names
static void scan_libdir(void)
{
    DIR *dir = opendir(g_libdir);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (!strstr(e->d_name, ".so")) continue;
        char p[PATH_MAX + 260];
        snprintf(p, sizeof p, "%s/%s", g_libdir, e->d_name);
        elf_facts f;
        if (!elf_read(p, &f) || f.android) continue;
        if (f.machine != HOST_MACHINE) {
            LOG("%s: not an ARM library of this build (ELF machine %d): ignored\n", e->d_name, f.machine);
            continue;
        }
        preload_siblings(p, &f, 0);
        add_host(e->d_name, p);
        LOG("%s: a glibc library: the host library for its DllImport names\n", e->d_name);
    }
    closedir(dir);
}

static int in_libdir(const char *name)
{
    char v[6][256];
    int n = plugins_name_variants(name, v, 6);
    for (int i = 0; i < n; i++) {
        char p[PATH_MAX + 260];
        snprintf(p, sizeof p, "%s/%s", g_libdir, v[i]);
        elf_facts f;          // an ARM library of this build (a desktop x86 one copied there does not count)
        if (strstr(v[i], ".so") && elf_read(p, &f) && f.machine == HOST_MACHINE) return 1;
    }
    return 0;
}

static void apply_map(void)
{
    for (int i = 0; i < g_nmap; i++) {
        map_ent *m = &g_map[i];
        char base[128];
        norm_name(m->name, base, sizeof base);
        if (!strcmp(m->use, "stub")) {
            const so_shim *t = plugins_stub_table(m->arg);
            if (!t) { LOG("%s: no built-in stub \"%s\"\n", m->name, m->arg); continue; }
            if (in_libdir(m->name)) { LOG("%s: an ARM build is in lib/: used instead of the %s stub\n", m->name, m->arg); continue; }
            char v[6][256];
            int n = plugins_name_variants(m->name, v, 6);
            for (int k = 0; k < n; k++) so_register_fake_lib(v[k], t);
            LOG("%s: built-in stub %s\n", m->name, m->arg);
        } else if (!strcmp(m->use, "none")) {
            if (in_libdir(m->name)) { LOG("%s: an ARM build is in lib/: used\n", m->name); continue; }
            map_variants(m->name, NULL);
            LOG("%s: not available (DllNotFoundException at the first call)\n", m->name);
        } else if (!strcmp(m->use, "host")) {
            if (in_libdir(m->name)) { LOG("%s: an ARM build is in lib/: used instead of %s\n", m->name, m->arg); continue; }
            void *h = dlopen(m->arg, RTLD_NOW | RTLD_GLOBAL);
            if (!h) { LOG("%s: system library %s missing (%s): DllNotFoundException at the first call\n", m->name, m->arg, dlerror()); continue; }
            add_host(m->name, m->arg);
            LOG("%s: system library %s\n", m->name, m->arg);
        } else if (!strcmp(m->use, "lib")) {
            char p[PATH_MAX + 260];
            snprintf(p, sizeof p, "%s/%s", g_libdir, m->arg);
            elf_facts f;
            if (access(p, R_OK) != 0) { LOG("%s: lib/%s missing: supply an ARM build (docs/plugins.md); until then DllNotFoundException\n", m->name, m->arg); continue; }
            if (!elf_read(p, &f)) { LOG("%s: lib/%s is not an ELF library\n", m->name, m->arg); continue; }
            if (f.android) {
                char want[300];
                snprintf(want, sizeof want, "lib%s.so", base);
                if (strcmp(m->arg, want)) LOG("%s: lib/%s is an Android library: name it %s (Android's rule)\n", m->name, m->arg, want);
                else LOG("%s: lib/%s (Android library)\n", m->name, m->arg);
            } else if (f.machine == HOST_MACHINE) {
                add_host(m->name, p);       // (also found by the scan when it carries the usual name)
                LOG("%s: lib/%s (glibc library)\n", m->name, m->arg);
            } else LOG("%s: lib/%s is not an ARM library of this build (ELF machine %d)\n", m->name, m->arg, f.machine);
        } else LOG("%s: unknown use \"%s\"\n", m->name, m->use);
    }
}

// the sig lines of host-mapped libraries become thunks, found before the library's own symbols
static void apply_sigs(void)
{
    so_shim *tab = calloc(g_nsig + 1, sizeof *tab);
    int n = 0, missing = 0, unsupported = 0;
    for (int i = 0; i < g_nsig; i++) {
        sig_ent *s = &g_sig[i];
        char base[128];
        norm_name(s->module, base, sizeof base);
        const char *target = NULL;
        for (int k = 0; k < g_nhost; k++) if (!strcmp(g_host[k].base, base)) target = g_host[k].target;
        if (!target) continue;
        void *h = dlopen(target, RTLD_NOW | RTLD_GLOBAL);
        void *fn = h ? dlsym(h, s->entry) : NULL;
        if (!fn) { missing++; continue; }
        void *t = plugins_thunk(fn, s->ret, s->args);
        if (!t) { unsupported++; LOG("%s %s(%s): no thunk (unsupported or none left)\n", s->module, s->entry, s->args); continue; }
        if (t == fn) continue;
        tab[n].name = strdup(s->entry);
        tab[n].fn = t;
        n++;
    }
    if (n) so_add_shims(tab);
    else free(tab);
    if (n || missing || unsupported)
        LOG("%d softfp thunks for float arguments%s (%d entry points not in the libraries, %d unsupported)\n", n,
#if defined(__arm__) && defined(__ARM_PCS_VFP)
            "",
#else
            " (none needed on this architecture)",
#endif
            missing, unsupported);
}

void plugins_init(const char *root, const char *libdir)
{
    if (!realpath(root, g_root)) snprintf(g_root, sizeof g_root, "%s", root);
    if (!realpath(libdir, g_libdir)) snprintf(g_libdir, sizeof g_libdir, "%s", libdir);
    char p[PATH_MAX + 20];
    const char *mp = getenv("UA_PLUGIN_MAP");
    if (!mp) { snprintf(p, sizeof p, "%s/plugins.map", root); mp = p; }
    read_map(mp);
    scan_libdir();
    apply_map();
    apply_sigs();
}

// ------------------------------------------------------------------------------------- FMOD paths

static int is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

const char *plugins_asset_path(const char *in, char *buf, unsigned size)
{
    if (!in) return NULL;
    char arch[PATH_MAX], rel[PATH_MAX];
    arch[0] = 0;
    if (!strncmp(in, "jar:file://", 11)) {
        const char *bang = strstr(in, "!/");
        if (!bang) return NULL;
        snprintf(arch, sizeof arch, "%.*s", (int)(bang - (in + 11)), in + 11);
        snprintf(rel, sizeof rel, "%s", bang + 2);                  // assets/...
    } else if (!strncmp(in, "file:///android_asset/", 22)) {
        snprintf(rel, sizeof rel, "assets/%s", in + 22);
    } else if (!strncmp(in, "file://", 7) && in[7] == '/') {
        if (!is_file(in + 7)) return NULL;
        snprintf(buf, size, "%s", in + 7);
        return buf;
    } else return NULL;
    if (strstr(rel, "..")) return NULL;
    if (arch[0] && is_dir(arch)) {                                 // a directory "APK" (unpacked data)
        snprintf(buf, size, "%s/%s", arch, rel);
        if (is_file(buf)) return buf;
    }
    if (arch[0]) {                                                 // beside the APK / OBB
        char d[PATH_MAX];
        snprintf(d, sizeof d, "%s", arch);
        char *s = strrchr(d, '/');
        if (s) { *s = 0; snprintf(buf, size, "%s/%s", d, rel); if (is_file(buf)) return buf; }
    }
    snprintf(buf, size, "%s/%s", g_root, rel);
    if (is_file(buf)) return buf;
    const char *ad = getenv("ANDROID_ASSET_DIR");
    if (ad && !strncmp(rel, "assets/", 7)) {
        snprintf(buf, size, "%s/%s", ad, rel + 7);
        if (is_file(buf)) return buf;
    }
    return NULL;
}

static const char *fmod_path(const char *what, const char *in, char *buf, unsigned size)
{
    const char *p = plugins_asset_path(in, buf, size);
    if (p) LOG("FMOD %s: %s -> %s\n", what, in, p);
    else if (in && (!strncmp(in, "jar:", 4) || !strncmp(in, "file:", 5))) LOG("FMOD %s: %s: no such file beside the APK\n", what, in);
    return p ? p : in;
}

// FMOD_Studio_System_LoadBankFile(system, filename, flags, bank*); FMOD_System_CreateSound /
// CreateStream(system, name_or_data, mode, exinfo*, sound*); FMOD_System_LoadPlugin(system, filename,
// handle*, priority). No float arguments: the same in both calling conventions.
typedef int (*loadbank_fn)(void *, const char *, unsigned, void **);
typedef int (*createsound_fn)(void *, const char *, unsigned, void *, void **);
typedef int (*loadplugin_fn)(void *, const char *, unsigned *, unsigned);
static loadbank_fn r_loadbank;
static createsound_fn r_createsound, r_createstream, r_createsound4, r_createstream4;
static loadplugin_fn r_loadplugin, r_loadplugin4;

static SO_ABI int w_loadbank(void *sys, const char *name, unsigned flags, void **bank)
{
    char b[PATH_MAX];
    return r_loadbank(sys, fmod_path("bank", name, b, sizeof b), flags, bank);
}
#define FMOD_OPENMEMORY 0x00000800u
#define FMOD_OPENMEMORY_POINT 0x10000000u
#define FMOD_OPENUSER 0x00000008u
#define SOUND_W(w, r, what)                                                                         \
    static SO_ABI int w(void *sys, const char *name, unsigned mode, void *ex, void **snd)           \
    {                                                                                               \
        char b[PATH_MAX];                                                                           \
        if (!(mode & (FMOD_OPENMEMORY | FMOD_OPENMEMORY_POINT | FMOD_OPENUSER)))                    \
            name = fmod_path(what, name, b, sizeof b);                                              \
        return r(sys, name, mode, ex, snd);                                                         \
    }
SOUND_W(w_createsound, r_createsound, "sound")
SOUND_W(w_createstream, r_createstream, "stream")
SOUND_W(w_createsound4, r_createsound4, "sound")
SOUND_W(w_createstream4, r_createstream4, "stream")
// a plugin path of the desktop layout (<dataPath>/Plugins/libx.so): the library in lib/
#define PLUGIN_W(w, r)                                                                              \
    static SO_ABI int w(void *sys, const char *name, unsigned *h, unsigned prio)                    \
    {                                                                                               \
        char b[PATH_MAX + 260];                                                                     \
        if (name && !is_file(name)) {                                                               \
            const char *s = strrchr(name, '/');                                                     \
            snprintf(b, sizeof b, "%s/%s", g_libdir, s ? s + 1 : name);                             \
            if (is_file(b)) { LOG("FMOD plugin: %s -> %s\n", name, b); name = b; }                  \
        }                                                                                           \
        return r(sys, name, h, prio);                                                               \
    }
PLUGIN_W(w_loadplugin, r_loadplugin)
PLUGIN_W(w_loadplugin4, r_loadplugin4)

void *plugins_dlsym_hook(const char *sym, void *p)
{
    if (!p || strncmp(sym, "FMOD", 4) != 0) return p;
#define WRAP(name, r, w) if (!strcmp(sym, name)) { r = (__typeof__(r))p; return (void *)w; }
    WRAP("FMOD_Studio_System_LoadBankFile", r_loadbank, w_loadbank)
    WRAP("FMOD5_System_CreateSound", r_createsound, w_createsound)
    WRAP("FMOD5_System_CreateStream", r_createstream, w_createstream)
    WRAP("FMOD_System_CreateSound", r_createsound4, w_createsound4)
    WRAP("FMOD_System_CreateStream", r_createstream4, w_createstream4)
    WRAP("FMOD5_System_LoadPlugin", r_loadplugin, w_loadplugin)
    WRAP("FMOD_System_LoadPlugin", r_loadplugin4, w_loadplugin4)
#undef WRAP
    return p;
}

// ------------------------------------------------------------------------------------- softfp thunks

#if defined(__arm__) && defined(__ARM_PCS_VFP)
#define TH_N 256            // thunks available
#define TH_WORDS 32         // softfp argument words read: r0-r3 and 28 stack words
#define TH_STACK 32         // hard-float stack words passed
#define TH_MAXARGS 24             /* = SIG_MAXARGS */

typedef struct { void *fn; char ret; int n; char k[TH_MAXARGS]; unsigned char c[TH_MAXARGS]; } th_sig;
static th_sig th_sigs[TH_N];
static int th_used;
static pthread_mutex_t th_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    uint32_t core[4], stk[TH_STACK];
    union { uint32_t s[16]; double d[8]; } v;
    int ncrn, nsaa, vfp_off, err;
    unsigned used;          // single-precision VFP registers taken (bit per s register)
} th_frame;

static void put_core(th_frame *f, const uint32_t *w, int nw, int align8, int composite)
{
    if (align8 && (f->ncrn & 1)) f->ncrn++;
    if (f->ncrn + nw <= 4) { memcpy(&f->core[f->ncrn], w, 4 * nw); f->ncrn += nw; return; }
    if (composite && f->ncrn < 4 && f->nsaa == 0) {        // C.5: split between r3.. and the stack
        int r = 4 - f->ncrn;
        memcpy(&f->core[f->ncrn], w, 4 * r);
        memcpy(&f->stk[0], w + r, 4 * (nw - r));
        f->ncrn = 4; f->nsaa = nw - r;
        return;
    }
    f->ncrn = 4;
    if (align8) f->nsaa = (f->nsaa + 1) & ~1;
    if (f->nsaa + nw > TH_STACK) { f->err = 1; return; }
    memcpy(&f->stk[f->nsaa], w, 4 * nw);
    f->nsaa += nw;
}

// a VFP candidate (float, double, or a homogeneous aggregate of n of them): the lowest free run of
// s registers (d registers: even-aligned pairs), else the stack and no more VFP registers (C.2.cp)
static void put_vfp(th_frame *f, const uint32_t *w, int ne, int dbl)
{
    int need = ne * (dbl ? 2 : 1);
    if (!f->vfp_off)
        for (int s = 0; s + need <= 16; s += dbl ? 2 : 1) {
            unsigned mask = ((1u << need) - 1) << s;
            if (f->used & mask) continue;
            memcpy(&f->v.s[s], w, 4 * need);
            f->used |= mask;
            return;
        }
    f->vfp_off = 1;
    if (dbl) f->nsaa = (f->nsaa + 1) & ~1;
    if (f->nsaa + need > TH_STACK) { f->err = 1; return; }
    memcpy(&f->stk[f->nsaa], w, 4 * need);
    f->nsaa += need;
}

#define S8(x) x, x, x, x, x, x, x, x
typedef uint64_t (*hf_u)(uint32_t, uint32_t, uint32_t, uint32_t, S8(double), S8(uint32_t), S8(uint32_t), S8(uint32_t), S8(uint32_t));
typedef float (*hf_f)(uint32_t, uint32_t, uint32_t, uint32_t, S8(double), S8(uint32_t), S8(uint32_t), S8(uint32_t), S8(uint32_t));
typedef double (*hf_d)(uint32_t, uint32_t, uint32_t, uint32_t, S8(double), S8(uint32_t), S8(uint32_t), S8(uint32_t), S8(uint32_t));
#define CALLARGS(f) f.core[0], f.core[1], f.core[2], f.core[3], \
    f.v.d[0], f.v.d[1], f.v.d[2], f.v.d[3], f.v.d[4], f.v.d[5], f.v.d[6], f.v.d[7], \
    f.stk[0], f.stk[1], f.stk[2], f.stk[3], f.stk[4], f.stk[5], f.stk[6], f.stk[7], \
    f.stk[8], f.stk[9], f.stk[10], f.stk[11], f.stk[12], f.stk[13], f.stk[14], f.stk[15], \
    f.stk[16], f.stk[17], f.stk[18], f.stk[19], f.stk[20], f.stk[21], f.stk[22], f.stk[23], \
    f.stk[24], f.stk[25], f.stk[26], f.stk[27], f.stk[28], f.stk[29], f.stk[30], f.stk[31]

// lays out the base-AAPCS words of a softfp call (w: r0-r3, then the stack) for the VFP variant
static int th_layout(const th_sig *s, const uint32_t *w, th_frame *f)
{
    memset(f, 0, sizeof *f);
    int wi = 0;
    for (int i = 0; i < s->n; i++) {
        int c = s->c[i];
        switch (s->k[i]) {
        case 'i': put_core(f, w + wi, 1, 0, 0); wi += 1; break;
        case 'l': wi = (wi + 1) & ~1; put_core(f, w + wi, 2, 1, 0); wi += 2; break;
        case 's': put_core(f, w + wi, c, 0, 1); wi += c; break;
        case 'S': wi = (wi + 1) & ~1; put_core(f, w + wi, c, 1, 1); wi += c; break;
        case 'f': put_vfp(f, w + wi, 1, 0); wi += 1; break;
        case 'd': wi = (wi + 1) & ~1; put_vfp(f, w + wi, 1, 1); wi += 2; break;
        case 'F': put_vfp(f, w + wi, c, 0); wi += c; break;
        case 'D': wi = (wi + 1) & ~1; put_vfp(f, w + wi, c, 1); wi += 2 * c; break;
        default: f->err = 1;
        }
        if (wi > TH_WORDS) f->err = 1;
    }
    return !f->err;
}

static uint64_t th_call(const th_sig *s, const uint32_t *w)
{
    th_frame f;
    th_layout(s, w, &f);                // (checked when the thunk was made)
    switch (s->ret) {
    case 'f': { float r = ((hf_f)s->fn)(CALLARGS(f)); uint32_t b; memcpy(&b, &r, 4); return b; }
    case 'd': { double r = ((hf_d)s->fn)(CALLARGS(f)); uint64_t b; memcpy(&b, &r, 8); return b; }
    default: return ((hf_u)s->fn)(CALLARGS(f));
    }
}

#define TA uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, \
    uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11, uint32_t a12, uint32_t a13, uint32_t a14, uint32_t a15, \
    uint32_t a16, uint32_t a17, uint32_t a18, uint32_t a19, uint32_t a20, uint32_t a21, uint32_t a22, uint32_t a23, \
    uint32_t a24, uint32_t a25, uint32_t a26, uint32_t a27, uint32_t a28, uint32_t a29, uint32_t a30, uint32_t a31
#define TW a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, \
    a23, a24, a25, a26, a27, a28, a29, a30, a31
// each entry reads its caller's 32 argument words (the words past the real arguments are the
// caller's frame: read, never used) and runs its signature
#define B4(s) ((s[0] - '0') * 64 + (s[1] - '0') * 16 + (s[2] - '0') * 4 + (s[3] - '0'))   /* "0123" -> 27 */
#define TH(k) static SO_ABI uint64_t th_e##k(TA) { uint32_t w[TH_WORDS] = { TW }; return th_call(&th_sigs[B4(#k)], w); }
#define TH4(k) TH(k##0) TH(k##1) TH(k##2) TH(k##3)
#define TH16(k) TH4(k##0) TH4(k##1) TH4(k##2) TH4(k##3)
#define TH64(k) TH16(k##0) TH16(k##1) TH16(k##2) TH16(k##3)
TH64(0) TH64(1) TH64(2) TH64(3)
#define E(k) (void *)th_e##k,
#define E4(k) E(k##0) E(k##1) E(k##2) E(k##3)
#define E16(k) E4(k##0) E4(k##1) E4(k##2) E4(k##3)
#define E64(k) E16(k##0) E16(k##1) E16(k##2) E16(k##3)
static void *const th_entry[TH_N] = { E64(0) E64(1) E64(2) E64(3) };
#endif

#define SIG_MAXARGS 24
typedef struct { char ret; int n; char k[SIG_MAXARGS]; unsigned char c[SIG_MAXARGS]; int floats; } sig_parsed;

// "i,f,F3,s2" -> classes; 0 if malformed (the same check on every architecture)
static int sig_parse(const char *ret, const char *args, sig_parsed *s)
{
    memset(s, 0, sizeof *s);
    if (!ret || !args || !strchr("vilfd", ret[0]) || !ret[0] || ret[1]) return 0;
    s->ret = ret[0];
    s->floats = s->ret == 'f' || s->ret == 'd';
    if (!strcmp(args, "-")) return 1;
    const char *p = args;
    while (*p) {
        if (s->n >= SIG_MAXARGS) return 0;
        char k = *p++;
        int c = 1;
        if (!strchr("ilfdFDsS", k)) return 0;
        if (strchr("FDsS", k)) { c = (int)strtol(p, (char **)&p, 10); if (c < 1 || c > 16) return 0; }
        if (strchr("FD", k) && c > 4) return 0;                // not a homogeneous aggregate
        s->floats |= strchr("fdFD", k) != NULL;
        s->k[s->n] = k; s->c[s->n] = (unsigned char)c; s->n++;
        if (*p == ',') p++;
        else if (*p) return 0;
    }
    return 1;
}

void *plugins_thunk(void *fn, const char *ret, const char *args)
{
    sig_parsed sp;
    if (!fn || !sig_parse(ret, args, &sp)) return NULL;
    if (!sp.floats) return fn;                      // nothing to convert
#if defined(__arm__) && defined(__ARM_PCS_VFP)
    th_sig s;
    memset(&s, 0, sizeof s);
    s.fn = fn;
    s.ret = sp.ret;
    s.n = sp.n;
    memcpy(s.k, sp.k, sizeof s.k);
    memcpy(s.c, sp.c, sizeof s.c);
    uint32_t w[TH_WORDS] = { 0 };
    th_frame f;
    if (!th_layout(&s, w, &f)) return NULL;         // more argument words than a thunk carries
    pthread_mutex_lock(&th_mu);
    int k = th_used < TH_N ? th_used++ : -1;
    if (k >= 0) th_sigs[k] = s;
    pthread_mutex_unlock(&th_mu);
    return k >= 0 ? th_entry[k] : NULL;
#else
    return fn;                                      // AAPCS64 on both sides
#endif
}

// ------------------------------------------------------------------------------------- stubs

extern const so_shim plugstub_discord_game_sdk[], plugstub_discord_rpc[], plugstub_eos[];
#ifndef UA_NO_STEAMSTUB
extern const so_shim plugstub_csteamworks[];
#endif

const so_shim *plugins_stub_table(const char *id)
{
    if (!id) return NULL;
#ifndef UA_NO_STEAMSTUB                         // (build.sh STEAMSTUB=0: no Steamworks stand-ins)
    if (!strcmp(id, "steam")) return steamstub_table();
    if (!strcmp(id, "csteamworks")) return plugstub_csteamworks;
#endif
    if (!strcmp(id, "discord_game_sdk")) return plugstub_discord_game_sdk;
    if (!strcmp(id, "discord-rpc")) return plugstub_discord_rpc;
    if (!strcmp(id, "eos")) return plugstub_eos;
    return NULL;
}
