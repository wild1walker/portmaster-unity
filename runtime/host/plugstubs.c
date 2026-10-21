// Built-in stubs for platform SDKs a desktop build P/Invokes (plugins.c, docs/plugins.md). Each
// answers as on a PC where the service's client is not running or not installed, so that a game
// takes the path it takes there: no Discord presence, no Steam overlay, no online platform. Nothing
// grants entitlements or pretends a sign-in. Written from the SDKs' public C API names and their
// documented results; no SDK code or header is used.
#include "../loader/so_loader.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int z_false(void) { return 0; }
static void z_void(void) { }
static void *z_null(void) { return NULL; }
static uint64_t z_u64(void) { return 0; }     // 64-bit results: both registers on 32-bit ARM

#define F(name, fn) { name, (void *)(fn) }

// Discord Game SDK (discord_game_sdk): DiscordCreate(version, DiscordCreateParams *, IDiscordCore **)
// returns an EDiscordResult. Without a running Discord client it fails, here with NotRunning (27);
// the C# wrapper then throws ResultException from `new Discord.Discord(...)`, which games catch
// (or log) as on a PC without Discord. The core pointer stays NULL, so nothing else is reachable.
#define DISCORD_RESULT_NOT_RUNNING 27
static int discord_create(int version, void *params, void **core)
{
    (void)version; (void)params;
    if (core) *core = NULL;
    static int once;
    if (!once++) fprintf(stderr, "[plugins] Discord Game SDK: DiscordCreate -> NotRunning (no Discord client)\n");
    return DISCORD_RESULT_NOT_RUNNING;
}
const so_shim plugstub_discord_game_sdk[] = {
    F("DiscordCreate", discord_create),
    { NULL, NULL },
};

// discord-rpc (the older library): every call returns nothing; without a Discord client the real
// library never connects, so no handler is ever called
const so_shim plugstub_discord_rpc[] = {
    F("Discord_Initialize", z_void),
    F("Discord_Shutdown", z_void),
    F("Discord_RunCallbacks", z_void),
    F("Discord_UpdateConnection", z_void),
    F("Discord_UpdatePresence", z_void),
    F("Discord_ClearPresence", z_void),
    F("Discord_Respond", z_void),
    F("Discord_UpdateHandlers", z_void),
    F("Discord_Register", z_void),
    F("Discord_RegisterSteamGame", z_void),
    { NULL, NULL },
};

// CSteamworks (Steamworks.NET before 9.0 put a C glue library between C# and libsteam_api): Init
// and InitSafe fail as without a running Steam client, RestartAppIfNecessary is false. The
// interface accessors (SteamClient_, SteamGameServerClient_) give NULL, as libsteam_api does before
// a successful init: Steamworks.NET's InteropHelp.TestIfAvailableClient then throws its own
// "Steamworks is not initialized." before any interface function (ISteam*_), as on a PC without
// Steam; the pipe and user handles are 0, the crash and minidump helpers do nothing
const so_shim plugstub_csteamworks[] = {
    F("SteamClient_", z_null),
    F("SteamGameServerClient_", z_null),
    F("GetHSteamPipe_", z_false),
    F("GetHSteamUser_", z_false),
    F("Steam_GetHSteamUserCurrent_", z_false),
    F("Steam_RunCallbacks_", z_void),
    F("Steam_RegisterInterfaceFuncs_", z_void),
    F("SteamInternal_CreateInterface_", z_null),
    F("SteamInternal_GameServer_Init_", z_false),
    F("GetSteamInstallPath", z_null),
    F("ReleaseCurrentThreadMemory", z_void),
    F("SetTryCatchCallbacks", z_void),
    F("WriteMiniDump", z_void),
    F("SetMiniDumpComment", z_void),
    F("SetBreakpadAppID", z_void),
    F("UseBreakpadCrashHandler", z_void),
    F("GameServer_BSecure", z_false),
    F("GameServer_GetHSteamPipe", z_false),
    F("GameServer_GetHSteamUser", z_false),
    F("GameServer_GetSteamID", z_u64),
    F("GameServer_ReleaseCurrentThreadMemory", z_void),
    F("Init", z_false),
    F("InitSafe", z_false),
    F("SteamAPI_Init", z_false),
    F("SteamAPI_InitSafe", z_false),
    F("RestartAppIfNecessary", z_false),
    F("SteamAPI_RestartAppIfNecessary", z_false),
    F("IsSteamRunning", z_false),
    F("SteamAPI_IsSteamRunning", z_false),
    F("Shutdown", z_void),
    F("SteamAPI_Shutdown", z_void),
    F("RunCallbacks", z_void),
    F("SteamAPI_RunCallbacks", z_void),
    F("RegisterCallback", z_void),
    F("UnregisterCallback", z_void),
    F("RegisterCallResult", z_void),
    F("UnregisterCallResult", z_void),
    F("GameServer_Init", z_false),
    F("GameServer_InitSafe", z_false),
    F("GameServer_Shutdown", z_void),
    F("GameServer_RunCallbacks", z_void),
    { NULL, NULL },
};

// Epic Online Services: EOS_Initialize succeeds (it only sets up the SDK), EOS_Platform_Create
// gives no platform (NULL, the SDK's documented failure), so a game takes its "EOS unavailable"
// path; the calls a game may still make with a NULL handle do nothing. EOS_EResult EOS_Success = 0.
static const char *eos_result_str(int r) { return r == 0 ? "EOS_Success" : "EOS_UnexpectedError"; }
static int eos_init(const void *options)
{
    (void)options;
    fprintf(stderr, "[plugins] Epic Online Services: EOS_Initialize -> EOS_Success; EOS_Platform_Create will give no platform\n");
    return 0;
}
static int eos_ok(void) { return 0; }
const so_shim plugstub_eos[] = {
    F("EOS_Initialize", eos_init),
    F("EOS_Shutdown", eos_ok),
    F("EOS_Platform_Create", z_null),
    F("EOS_Platform_Release", z_void),
    F("EOS_Platform_Tick", z_void),
    F("EOS_Logging_SetCallback", eos_ok),
    F("EOS_Logging_SetLogLevel", eos_ok),
    F("EOS_EResult_ToString", eos_result_str),
    F("EOS_EResult_IsOperationComplete", z_false),
    { NULL, NULL },
};
