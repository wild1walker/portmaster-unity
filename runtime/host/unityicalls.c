// unityicalls.c: engine calls that answer the same for the same name, answered from what they answered before
// (UA_PROPERTY_ID=1; off by default).
//
// Shader.PropertyToID(string) converts the name to ASCII and looks it up in Unity's map of shader property names (a
// std::map of C strings: a dozen strcmp of cache-missing nodes), adding it the first time; the number a name gets is
// never changed or reused while the player runs. Scripts that call it every frame instead of keeping the number
// (Enter the Gungeon's do: ~0.1 ms a frame of the G350's Bullet Hell fight) pay the lookup each time. With
// UA_PROPERTY_ID=1 the engine call is registered (mono_add_internal_call) as a wrapper that keeps, on UnityMain, each
// name's number after Unity's own call returned it and gives it again for the same characters; a null string, a name
// longer than 64 characters, any other thread and the first call for each name go to Unity's own. The number is what
// Unity's call would return: the name's entry exists from that first call on and does not change.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "unityicalls.h"

typedef void (*add_icall_fn)(const char *name, const void *fn);
static add_icall_fn r_add;
static int32_t (*o_property_to_id)(const void *str);
static pid_t g_main;

// Mono 2.6's MonoString: the object header (vtable, synchronisation), the length at +8, the UTF-16 characters at +12
struct pent { uint32_t hash; int32_t id; uint16_t len; uint16_t chars[64]; };
#define NPENT 1024
static struct pent g_pent[NPENT];
static unsigned g_npent;
static unsigned long g_hits, g_calls;

static pid_t tid(void)
{
    static __thread pid_t t;
    if (!t) t = (pid_t)syscall(SYS_gettid);
    return t;
}

static int32_t property_to_id(const void *str)
{
    if (!str || !g_main || tid() != g_main) return o_property_to_id(str);
    g_calls++;
    int32_t len = *(const int32_t *)((const char *)str + 8);
    const uint16_t *c = (const uint16_t *)((const char *)str + 12);
    if (len <= 0 || len > 64) return o_property_to_id(str);
    uint32_t h = 2166136261u;
    for (int32_t i = 0; i < len; i++) { h ^= c[i]; h *= 16777619u; }
    unsigned i0 = (h ^ (h >> 16)) & (NPENT - 1);
    for (unsigned k = 0; k < NPENT; k++) {
        struct pent *e = &g_pent[(i0 + k) & (NPENT - 1)];
        if (!e->len) {
            int32_t id = o_property_to_id(str);
            if (g_npent < NPENT * 3 / 4) {
                e->hash = h; e->id = id; e->len = (uint16_t)len;
                memcpy(e->chars, c, (size_t)len * 2);
                g_npent++;
            }
            return id;
        }
        if (e->hash == h && e->len == len && !memcmp(e->chars, c, (size_t)len * 2)) { g_hits++; return e->id; }
    }
    return o_property_to_id(str);
}

static void w_add(const char *name, const void *fn)
{
    if (name && fn && !strcmp(name, "UnityEngine.Shader::PropertyToID") && !o_property_to_id) {
        o_property_to_id = (int32_t (*)(const void *))fn;
        r_add(name, (const void *)property_to_id);
        fprintf(stderr, "[icalls] Shader.PropertyToID: each name's number kept on UnityMain (UA_PROPERTY_ID)\n");
        return;
    }
    r_add(name, fn);
}

void *unityicalls_dlsym_hook(const char *symbol, void *found)
{
    if (!found || strcmp(symbol, "mono_add_internal_call") || found == (void *)w_add) return found;
    const char *v = getenv("UA_PROPERTY_ID");
    if (!v || strcmp(v, "1")) return found;
    r_add = (add_icall_fn)found;
    return (void *)w_add;
}

void unityicalls_frame(void)
{
    if (!o_property_to_id) return;
    if (!g_main) g_main = tid();
}
