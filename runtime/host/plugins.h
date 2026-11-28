// The port's native plugins (plugins.c, docs/plugins.md): what each library a game P/Invokes
// becomes on the handheld. ua_host calls plugins_init() before it loads libmain.
#pragma once
#include "../loader/so_loader.h"

// Reads <root>/plugins.map (or $UA_PLUGIN_MAP; written by pmu android-data), maps glibc builds of
// plugins in <libdir> to host libraries, registers the built-in stubs and refusals the map asks for. Without a map: only the scan of
// <libdir>. Logs one "[plugins]" line per decision.
void plugins_init(const char *root, const char *libdir);

// For the host's dlsym hook (so_set_dlsym_hook): wraps the FMOD calls that take file paths so that
// Unity's StreamingAssets paths ("jar:file://<obb>!/assets/x.bank", "file:///android_asset/x.bank")
// reach files on disk. Returns `found` for every other symbol.
void *plugins_dlsym_hook(const char *symbol, void *found);

// Built-in stub tables by id (steam, csteamworks, discord_game_sdk, discord-rpc, eos), or NULL.
const so_shim *plugins_stub_table(const char *id);

// "fmodstudio" -> "libfmodstudio.so", "libfmodstudio", "fmodstudio.so", "fmodstudio": the names Mono
// and libunity try for a DllImport name (n <= 6). Returns the count.
int plugins_name_variants(const char *name, char out[][256], int n);

// For tests: rewrites a StreamingAssets path as the FMOD wrappers do (NULL if nothing exists).
const char *plugins_asset_path(const char *in, char *buf, unsigned size);
