// ua_host's built-in Wwise: the libAkSoundEngine a game's Wwise integration P/Invokes, built from
// the game's own entry points (plugins.map's fn lines), and the interface of the sound backend that
// plays its banks (docs/plugins.md, "Wwise"). Without a backend the stub is silent: every call
// succeeds, banks "load", events end at once, and the callbacks the integration waits for (bank
// loaded, end of event) reach the game.
//
// A backend is a glibc shared library for the handheld (UA_WWISE_BACKEND=<path>) that exports
//     const ua_wwise_backend *ua_wwise_backend_get(const ua_wwise_host *host);
// It is asked once, at the first call into the stub; NULL (or a version it does not know) keeps the
// silent stub. Every function pointer may be NULL (the stub then answers as without a backend).
// Strings are UTF-8, valid only during the call. IDs are Wwise's (ua_wwise_host.id of a name).
#pragma once
#include <stdint.h>

#define UA_WWISE_BACKEND_VERSION 1

// AKRESULT values the stub and backends answer with (the game's own enum is read from its
// assemblies where it has one; these have not changed since Wwise 2013)
#define UA_AK_SUCCESS 1
#define UA_AK_FAIL 2

// one argument or result, by its class in the fn line: b i (int32), u (uint32), l (64-bit), f, d,
// p (a pointer: IntPtr, a SWIG object), r (a ref/out pointer), z (a string), s/S/F/D (a struct by
// value: p points at its bytes)
typedef union { int64_t i; uint64_t u; double d; float f; void *p; const char *z; } ua_ak_value;

typedef struct ua_ak_call {
    const char *entry;              // the P/Invoke's entry point, e.g. "CSharp_PostEvent__SWIG_6"
    const char *api;                // the Wwise call: "PostEvent" (CSharp_ and __SWIG_<n> cut off)
    char ret;                       // the result's class (v b i u l f d p z)
    int nargs;
    const char *const *args;        // each argument's class ("u", "z", "F3", ...)
    ua_ak_value *a;                 // the arguments
} ua_ak_call;

typedef struct ua_wwise_host {
    int version;                                            // UA_WWISE_BACKEND_VERSION
    // the event ended (a backend that returned 1 from post_event calls this): its end-of-event
    // callback, if the game asked for one, is delivered at the game's next callback poll
    void (*event_done)(uint32_t playing_id);
    // a callback of another type for an event (AK_Marker, AK_Duration, music sync ...), by the
    // game's AkCallbackType value: fields are the callback info's accessor names without the class,
    // e.g. {"uIdentifier", "uPosition"} for a marker; values as their classes
    void (*event_callback)(uint32_t playing_id, uint32_t type, int n, const char *const *field, const ua_ak_value *value);
    // a file the game names (a bank, a .wem stream) on disk: the base paths it set, StreamingAssets
    // beside the APK, the APK's assets folder; NULL if none
    const char *(*file)(const char *name, char *buf, unsigned size);
    uint32_t (*id)(const char *name);                       // Wwise's ID of a name (FNV-1, lower case)
    uint32_t (*constant)(const char *name, uint32_t dflt);  // the game's enum value ("AkCallbackType.AK_Marker")
    void (*log)(const char *fmt, ...);                      // a "[wwise]" line in the log
} ua_wwise_host;

typedef struct ua_wwise_backend {
    int version;                    // UA_WWISE_BACKEND_VERSION
    const char *name;
    // every call is offered here first: return 1 and set *ret to answer it (any entry point, by
    // c->api); 0 leaves it to the stub (which then calls the functions below)
    int  (*call)(const ua_ak_call *c, ua_ak_value *ret);
    // the sound engine: results are AKRESULT
    int  (*init)(const char *base_path, const char *language);
    void (*term)(void);
    void (*render)(void);                                   // RenderAudio: once a frame
    void (*suspend)(int suspended);
    // banks: by file name (ua_wwise_host.file finds it; id is then the ID of the name without its
    // extension, as in the bank's header), by ID (name NULL), or from the game's memory (data, size;
    // id 0); *bank_id holds id on the way in and is the ID the game gets back
    int  (*load_bank)(const char *name, uint32_t id, const void *data, uint32_t size, uint32_t *bank_id);
    int  (*unload_bank)(uint32_t bank_id);
    // an event: return 1 while it plays (call event_done when it ends), 0 if it ended already
    int  (*post_event)(uint32_t event_id, uint64_t game_object, uint32_t playing_id, uint32_t flags);
    void (*stop)(uint32_t playing_id);                      // StopPlayingID
    void (*stop_all)(uint64_t game_object, int every_object);
    // game syncs: playing_id 0 = not by playing ID; every_object = the global value
    void (*set_rtpc)(uint32_t rtpc, float value, uint64_t game_object, uint32_t playing_id, int every_object);
    void (*set_state)(uint32_t group, uint32_t state);
    void (*set_switch)(uint32_t group, uint32_t value, uint64_t game_object);
    void (*post_trigger)(uint32_t trigger, uint64_t game_object);
    // game objects and listeners
    void (*game_object)(uint64_t game_object, const char *name, int registered);
    void (*set_position)(uint64_t game_object, const float position[3], const float front[3], const float top[3]);
} ua_wwise_backend;

typedef const ua_wwise_backend *(*ua_wwise_backend_fn)(const ua_wwise_host *host);

// ---- for ua_host (plugins.c) --------------------------------------------------------------------
#include "../loader/so_loader.h"

// The stub's entry points for one DllImport name: one shim per fn line (entry, result class,
// argument classes "u,z,p" or "-"), the game's enum constants (name, value). NULL if none.
typedef struct { const char *entry, *ret, *args; } ua_ak_fn;
typedef struct { const char *name; long long value; } ua_ak_const;
const so_shim *wwise_stub_table(const char *module, const ua_ak_fn *fns, int nfns, const ua_ak_const *consts, int nconsts);
