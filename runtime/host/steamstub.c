// libsteam_api.so for ports: the Steamworks flat API as a PC without a running Steam client answers
// it. Desktop games ship Valve's x86/x86_64 library in <game>_Data/Plugins, which no ARM port can
// load; Steamworks.NET's usual SteamManager then catches the DllNotFoundException and quits the
// game (Application.Quit) before its first scene. With this library: RestartAppIfNecessary is
// false, every Init is false (or k_ESteamAPIInitResult_NoSteamClient with its message), and the
// game goes on as it does on a PC where Steam is not running. The per-interface functions are
// never reached after a failed Init; a game that calls one anyway gets Mono's
// EntryPointNotFoundException. A real ARM build of the library in the library folder wins: the
// stub is only registered when there is none.
#include "steamstub.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int  s_false(void) { return 0; }
static void s_void(void) { }
static void *s_null(void) { return NULL; }
static uint64_t s_u64(void) { return 0; }     // 64-bit results: both registers on 32-bit ARM
static const char *s_empty(void) { return ""; }

// SteamAPI_InitFlat(SteamErrMsg *) / SteamInternal_SteamAPI_Init(const char *versions, SteamErrMsg *)
// / SteamInternal_GameServer_Init_V2(..., SteamErrMsg *): ESteamAPIInitResult, message in a char[1024]
#define NO_STEAM_CLIENT 2
static void no_client_msg(char *msg) { if (msg) snprintf(msg, 1024, "Steam is not running"); }
static int s_init_flat(char *msg) { no_client_msg(msg); return NO_STEAM_CLIENT; }
static int s_internal_init(const char *versions, char *msg) { (void)versions; no_client_msg(msg); return NO_STEAM_CLIENT; }
static int s_gs_init_v2(uint32_t ip, uint16_t gport, uint16_t qport, int mode, const char *ver, const char *versions, char *msg)
{
    (void)ip; (void)gport; (void)qport; (void)mode; (void)ver; (void)versions;
    no_client_msg(msg);
    return NO_STEAM_CLIENT;
}

#define F(name, fn) { name, (void *)fn }
static const so_shim steam_api_shims[] = {
    // start-up, as without a Steam client
    F("SteamAPI_RestartAppIfNecessary", s_false),
    F("SteamAPI_Init", s_false),
    F("SteamAPI_InitSafe", s_false),
    F("SteamAPI_InitFlat", s_init_flat),
    F("SteamInternal_SteamAPI_Init", s_internal_init),
    F("SteamAPI_IsSteamRunning", s_false),
    F("SteamAPI_GetSteamInstallPath", s_empty),
    F("SteamAPI_GetHSteamPipe", s_false),
    F("SteamAPI_GetHSteamUser", s_false),
    F("SteamGameServer_Init", s_false),
    F("SteamGameServer_InitSafe", s_false),
    F("SteamInternal_GameServer_Init", s_false),
    F("SteamInternal_GameServer_Init_V2", s_gs_init_v2),
    F("SteamGameServer_GetHSteamPipe", s_false),
    F("SteamGameServer_GetHSteamUser", s_false),
    F("SteamGameServer_BSecure", s_false),
    F("SteamGameServer_GetSteamID", s_u64),
    // what a game may still call once Init failed: nothing to do
    F("SteamAPI_Shutdown", s_void),
    F("SteamAPI_RunCallbacks", s_void),
    F("SteamAPI_ReleaseCurrentThreadMemory", s_void),
    F("SteamAPI_RegisterCallback", s_void),
    F("SteamAPI_UnregisterCallback", s_void),
    F("SteamAPI_RegisterCallResult", s_void),
    F("SteamAPI_UnregisterCallResult", s_void),
    F("SteamAPI_SetMiniDumpComment", s_void),
    F("SteamAPI_WriteMiniDump", s_void),
    F("SteamAPI_UseBreakpadCrashHandler", s_void),
    F("SteamAPI_SetBreakpadAppID", s_void),
    F("SteamAPI_SetTryCatchCallbacks", s_void),
    F("SteamAPI_ManualDispatch_Init", s_void),
    F("SteamAPI_ManualDispatch_RunFrame", s_void),
    F("SteamAPI_ManualDispatch_GetNextCallback", s_false),
    F("SteamAPI_ManualDispatch_FreeLastCallback", s_void),
    F("SteamAPI_ManualDispatch_GetAPICallResult", s_false),
    F("SteamGameServer_Shutdown", s_void),
    F("SteamGameServer_RunCallbacks", s_void),
    F("SteamGameServer_ReleaseCurrentThreadMemory", s_void),
    // interface lookups: no interfaces
    F("SteamInternal_ContextInit", s_null),
    F("SteamInternal_CreateInterface", s_null),
    F("SteamInternal_FindOrCreateUserInterface", s_null),
    F("SteamInternal_FindOrCreateGameServerInterface", s_null),
    F("SteamClient", s_null),
    F("SteamGameServerClient", s_null),
    { NULL, NULL },
};

const so_shim *steamstub_table(void) { return steam_api_shims; }

void steamstub_register(const char *libdir)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/libsteam_api.so", libdir);
    if (access(path, R_OK) == 0) return;
    so_register_fake_lib("libsteam_api.so", steam_api_shims);
}
