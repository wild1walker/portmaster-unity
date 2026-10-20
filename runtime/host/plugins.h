// The port's native plugins (plugins.c, docs/plugins.md): what each library a game P/Invokes
// becomes on the handheld. ua_host calls plugins_init() before it loads libmain.
#pragma once
#include "../loader/so_loader.h"

// Reads <root>/plugins.map (or $UA_PLUGIN_MAP; written by pmunity android-data), maps glibc builds of
// plugins in <libdir> to host libraries (with softfp thunks for their float-argument calls on 32-bit
// ARM), registers the built-in stubs and refusals the map asks for. Without a map: only the scan of
// <libdir>. Logs one "[plugins]" line per decision.
void plugins_init(const char *root, const char *libdir);

// For the host's dlsym hook (so_set_dlsym_hook): wraps the FMOD calls that take file paths so that
// Unity's StreamingAssets paths ("jar:file://<obb>!/assets/x.bank", "file:///android_asset/x.bank")
// reach files on disk. Returns `found` for every other symbol.
void *plugins_dlsym_hook(const char *symbol, void *found);

// A softfp-callable thunk for a hard-float host function with the given argument classes (the
// `sig` lines of plugins.map: result one of v i l f d; arguments comma-separated i l f d F<n> D<n>
// s<n> S<n>, or "-"). On AArch64 (one calling convention) it returns fn. NULL if the signature is
// not supported or no thunk is left.
void *plugins_thunk(void *fn, const char *result, const char *args);

// Built-in stub tables by id (steam, csteamworks, discord_game_sdk, discord-rpc, eos), or NULL.
const so_shim *plugins_stub_table(const char *id);

// "fmodstudio" -> "libfmodstudio.so", "libfmodstudio", "fmodstudio.so", "fmodstudio": the names Mono
// and libunity try for a DllImport name (n <= 6). Returns the count.
int plugins_name_variants(const char *name, char out[][256], int n);

// For tests: rewrites a StreamingAssets path as the FMOD wrappers do (NULL if nothing exists).
const char *plugins_asset_path(const char *in, char *buf, unsigned size);
