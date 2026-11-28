#define _GNU_SOURCE
// plugins.c: the native plugins of a port (docs/plugins.md). A desktop build's plugins are x86
// libraries; the port gives each library its managed code P/Invokes one of:
//   lib  <file>  an ARM build in lib/: an Android (bionic) library is loaded by the loader as
//                usual; a Linux (glibc) one is mapped to the host library (so_map_library)
//   host <lib>   a library of the handheld's system (e.g. libsqlite3.so.0)
//   stub <id>    a built-in library answering as a PC without that service (plugstubs.c, steamstub.c),
//                or Wwise's sound engine built from the game's own entry points (wwise.c: the map's
//                fn and const lines)
//   none         dlopen fails: DllNotFoundException, as on a PC without the library
// from plugins.map (pmu android-data writes it beside the APK). Glibc libraries found in lib/
// are mapped even without a map line.
//
// Mono (arm64-v8a) and a glibc aarch64 library share AAPCS64: calls go to the library unconverted
// (`sig` lines of older maps, argument classes for the 32-bit player's softfp thunks, are ignored).
//
// FMOD: Unity's StreamingAssets path is "jar:file://<apk or obb>!/assets" on Android, which FMOD's
// file system cannot open; the wrappers below give FMOD the file on disk (android-data puts the
// banks beside the APK, in assets/).
#include "plugins.h"
#include "wwise.h"
#ifndef UA_NO_STEAMSTUB
#include "steamstub.h"
#include "fmodstub.h"
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

#define HOST_MACHINE EM_AARCH64

// ------------------------------------------------------------------------------------- the map

typedef struct { char name[128], use[8], arg[256]; } map_ent;
typedef struct { char module[128], entry[128], ret[8], args[160]; } sig_ent;
static map_ent *g_map; static int g_nmap;
// fn <module> <entry> <result> <args> / const <module> <name> <value>: a stub built from the game's
// own entry points (wwise)
static sig_ent *g_fn; static int g_nfn;
typedef struct { char module[128], name[128]; long long value; } const_ent;
static const_ent *g_const; static int g_nconst;

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
        } else if (n >= 5 && !strcmp(a, "fn")) {
            g_fn = realloc(g_fn, (g_nfn + 1) * sizeof *g_fn);
            sig_ent *s = &g_fn[g_nfn++];
            snprintf(s->module, sizeof s->module, "%s", b);
            snprintf(s->entry, sizeof s->entry, "%s", c);
            snprintf(s->ret, sizeof s->ret, "%s", d);
            snprintf(s->args, sizeof s->args, "%s", e);
        } else if (n >= 4 && !strcmp(a, "const")) {
            g_const = realloc(g_const, (g_nconst + 1) * sizeof *g_const);
            const_ent *k = &g_const[g_nconst++];
            snprintf(k->module, sizeof k->module, "%s", b);
            snprintf(k->name, sizeof k->name, "%s", c);
            k->value = strtoll(d, NULL, 0);
        }
    }
    fclose(f);
    LOG("%s: %d libraries%s\n", path, g_nmap, g_nfn ? ", a stub's entry points" : "");
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
    struct dirent64 *e;
    while ((e = readdir64(dir))) {
        // <name>.so or <name>.so.<version digits>: not a copy kept beside one (libmonobdwgc-2.0.so.rc, .bak), which
        // would be a second, uninitialised copy of a library loaded already (its SONAME is not what dlopen matches)
        const char *so = strstr(e->d_name, ".so");
        while (so && strspn(so + 3, ".0123456789") != strlen(so + 3)) so = strstr(so + 3, ".so");
        if (!so) continue;
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

// Wwise (wwise.c): the stub's entry points are the game's, from the fn and const lines of that name
static const so_shim *wwise_table(const char *module)
{
    ua_ak_fn *f = calloc(g_nfn + 1, sizeof *f);
    ua_ak_const *k = calloc(g_nconst + 1, sizeof *k);
    int nf = 0, nk = 0;
    for (int i = 0; i < g_nfn; i++)
        if (!strcmp(g_fn[i].module, module)) f[nf++] = (ua_ak_fn){ g_fn[i].entry, g_fn[i].ret, g_fn[i].args };
    for (int i = 0; i < g_nconst; i++)
        if (!strcmp(g_const[i].module, module)) k[nk++] = (ua_ak_const){ g_const[i].name, g_const[i].value };
    const so_shim *t = nf ? wwise_stub_table(module, f, nf, k, nk) : NULL;
    if (!nf) LOG("%s: no entry points for the wwise stub in the map (fn lines: android-data of this version writes them)\n", module);
    free(f);
    free(k);
    return t;
}

static void apply_map(void)
{
    for (int i = 0; i < g_nmap; i++) {
        map_ent *m = &g_map[i];
        char base[128];
        norm_name(m->name, base, sizeof base);
        if (!strcmp(m->use, "stub")) {
            if (in_libdir(m->name)) { LOG("%s: an ARM build is in lib/: used instead of the %s stub\n", m->name, m->arg); continue; }
            const so_shim *t = !strcmp(m->arg, "wwise") ? wwise_table(m->name) : plugins_stub_table(m->arg);
            if (!t) { LOG("%s: no built-in stub \"%s\"\n", m->name, m->arg); continue; }
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

static int w_loadbank(void *sys, const char *name, unsigned flags, void **bank)
{
    char b[PATH_MAX];
    return r_loadbank(sys, fmod_path("bank", name, b, sizeof b), flags, bank);
}
#define FMOD_OPENMEMORY 0x00000800u
#define FMOD_OPENMEMORY_POINT 0x10000000u
#define FMOD_OPENUSER 0x00000008u
#define SOUND_W(w, r, what)                                                                         \
    static int w(void *sys, const char *name, unsigned mode, void *ex, void **snd)           \
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
    static int w(void *sys, const char *name, unsigned *h, unsigned prio)                    \
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
    if (!strcmp(id, "fmod")) return fmodstub_table();
    return NULL;
}
