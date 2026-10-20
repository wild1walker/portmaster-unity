# Native plugins, platform SDKs and HTTPS

A desktop build ships its native plugins as x86/x86-64 (or macOS) libraries in `<Game>_Data/Plugins`;
none of them loads on ARM. What a port can give each library the game's managed code calls:

| use | what the runtime does | for |
|---|---|---|
| `lib <file>` | an ARM build in the port's `lib/` (beside `libunity.so`): an Android (bionic) library is loaded by the loader like Unity's own; a Linux (glibc) one becomes a host library (`so_map_library`) | FMOD, Wwise, open-source plugins built for ARM |
| `host <lib>` | a library of the handheld's system, e.g. `libsqlite3.so.0` | SQLite (Mono.Data.Sqlite), others on request |
| `stub <id>` | a built-in library that answers as a PC on which that service's client is not running or not installed | Steamworks, CSteamworks, Discord Game SDK, Discord RPC, Epic Online Services |
| `none` | `dlopen` fails: `DllNotFoundException` at the first call, as on a PC without that library | everything else |

Nothing here grants entitlements, fakes a sign-in or bypasses DRM: a stub's only answers are the
failures a PC without the service gives.

## The survey and the plugin map

`python -m pmunity plugins <Game>_Data [--map plugins.map] [--json report.json] [--plugin-map port.json]`
(convert/pmunity/plugins.py) reads the game's assemblies (convert/pmunity/clrmeta.py: the ImplMap,
ModuleRef, method signature and constant tables of every assembly that is not Unity's or the .NET
runtime's) and the files in its Plugins folders, and reports for every library the managed code
P/Invokes: the entry points, which assemblies and types call it, the desktop files that match it,
the SDK it is (a table of known names), the use the port gets by default and why. `pmunity
android-data` runs the same survey: its warnings say what each library becomes, and
`android_data_report.json` has the whole report under `plugins` (`modules`, `files`, `fmod`, `map`).

android-data writes `plugins.map` into its output folder and beside the APK (`zip/`), where ua_host
reads it (`<root>/plugins.map`, or `UA_PLUGIN_MAP=<file>`):

```
map fmodstudio lib libfmodstudio.so
map steam_api stub steam
map GalaxyCSharpGlue none
sig fmodstudio FMOD_Studio_VCA_SetVolume i i,f
```

A port changes any entry with `--plugin-map <json>` (port configs carry it as `"plugins"`):

```json
{"plugins": {"sqlite3": {"use": "host", "lib": "libsqlite3.so.0"},
             "discord_game_sdk": {"use": "stub"},
             "fmodstudio": {"use": "lib", "file": "libfmodstudio.so"},
             "StandaloneFileBrowser": "none"}}
```

Rules ua_host follows (runtime/host/plugins.c, logged as `[plugins]` lines):
- The names a DllImport name is registered under are the ones Mono and libunity try: `fmodstudio`,
  `libfmodstudio.so`, `libfmodstudio`, `fmodstudio.so`.
- An ARM library in `lib/` always wins over a `stub`, `none` or `host` entry. An Android library must
  carry Android's name for it (`lib<name>.so`): libunity loads plugins through
  `UnityPlayer.loadLibrary(name)` (which also runs their `JNI_OnLoad`), then Mono opens them.
- Every glibc library of the player's architecture in `lib/` is mapped to its names even without a
  map line (a porter can drop in a Linux ARM build); its DT_NEEDED entries that are in `lib/` are
  loaded first (`libfmodstudio.so.13` needs `libfmod.so.13`, also found as `libfmod.so`).
- Defaults that change nothing for a game that works today: `none` lines only refuse what was not
  there; SDL2, OpenAL and libudev (Rewired's Linux path) are left to Mono's dllmap and the loader
  (`keep` in the report); steam_api's stub was already registered for `libsteam_api.so`.

### Float arguments on 32-bit ARM (`sig` lines)

The armeabi-v7a player's Mono passes `float`/`double` arguments and results in core registers
(base AAPCS, "softfp"); a glibc armhf library expects them in VFP registers. An Android library is
softfp itself and needs nothing. For a glibc library ua_host builds a thunk per entry point that has
such arguments, from the `sig` lines android-data writes (the argument classes come from the
DllImport declarations: `i` word, `l` 64-bit integer, `f` float, `d` double, `F<n>`/`D<n>` a struct of
n floats/doubles (a VFP candidate), `s<n>`/`S<n>` another struct of n words, word/doubleword aligned;
structs are resolved through the game's and Unity's assemblies, enums to their underlying type). The
thunk reads the caller's argument words and calls the library with the VFP variant's layout:
singles back-filled into free `s` registers, doubles in even-aligned pairs, VFP arguments past `s15`
on the stack (and no more VFP registers after the first one that does not fit), integers and other
structs in `r0`-`r3` and the stack, structs split between `r3` and the stack only while nothing is
on the stack. Results: `v i l f d` (a float or double result comes back in `r0`/`r0:r1`). 256 thunks
are available. AArch64 players share AAPCS64 with the host: no thunks.

## FMOD

Unity's FMOD for Unity integration P/Invokes FMOD Engine (`fmod` and `fmodstudio`; since FMOD 2.02
the core API is imported from `fmodstudio` too). FMOD's licence makes it free for small developers,
but every download needs an FMOD account: the port tools never ship FMOD, and the porter (or the
player, in a port's setup) supplies FMOD's ARM libraries of the game's FMOD version.

**Route: FMOD's own Android libraries**, of the same version as the game's integration, in the
port's `lib/`:

| | |
|---|---|
| files | `libfmod.so` and `libfmodstudio.so` (FMOD 2.02+: both; the core is reached through `libfmodstudio.so`'s dependency), and the DSP plugins the game's FMOD settings load (e.g. `libresonanceaudio.so`) |
| ABI | `armeabi-v7a` for the armv7 Mono player (`arm64-v8a` for an arm64 player) |
| from | FMOD Engine `<version>` for Android (`fmodstudioapi<XXYYZZ>android.tar.gz`: `api/core/lib/<abi>/`, `api/studio/lib/<abi>/`), or FMOD for Unity `<version>` (the Android libraries in the `.unitypackage`) |
| version | exactly the game's (`pmunity plugins` prints it); FMOD keeps the API within a major line (2.02.x), and the integration passes its header version to `System_Create` |

Why this route: these libraries are armeabi-v7a code like Unity's player, so Mono's calls need no
ABI conversion; they need only what the runtime already gives Unity's own built-in FMOD (bionic
through the loader: their imports are libc, libm, libdl and liblog; OpenSL ES through the android
shims, which play through SDL2 or, when a sound server holds the card, `pw-cat`/`pacat`); and they
read their banks as plain files. FMOD's Linux libraries would output through ALSA or PulseAudio
themselves, which on the handhelds' 32-bit userlands is exactly what fails when PipeWire holds the
card (the reason the OpenSL shim has its pipe output), and on 32-bit ARM every call with a float
argument would need a thunk. They still work as `lib` entries (glibc libraries in `lib/` are mapped,
the `sig` lines give the thunks), e.g. on an arm64 host with a sound server.

What the runtime adds for FMOD:
- `org.fmod.FMOD` (runtime/jni/up_fmod.c), the Java class FMOD's Android library calls
  (`checkInit`, `getAssetManager`, `getOutputSampleRate`, `getOutputBlockSize`, `supportsLowLatency`,
  `supportsAAudio`, `getAudioDevices`, ...). A desktop build of the integration never calls
  `FMOD.init(activity)` (that call is compiled for Android only), so the class answers as initialized
  with the activity. Low latency and no AAudio make FMOD's automatic output OpenSL ES.
  `org.fmod.AudioDevice` (the AudioTrack output) refuses to start, logged: FMOD then fails its
  output and the integration falls back to no sound.
- Bank paths. The integration loads banks from `Application.streamingAssetsPath`
  (`jar:file://<obb>!/assets/...` on Android) or, with the Android platform in its settings, from
  `file:///android_asset/...`; FMOD opens neither inside an OBB. android-data keeps `*.bank` files
  out of the OBB, beside the APK in `assets/` (`--loose-assets <glob>` for others, on by default
  when the game uses FMOD), and ua_host's dlsym hook wraps `FMOD_Studio_System_LoadBankFile`,
  `FMOD5_System_CreateSound`/`CreateStream` (file names only) and `FMOD5_System_LoadPlugin`
  (the desktop `<dataPath>/Plugins/lib<x>.so` layout -> `lib/lib<x>.so`) to give FMOD those files.
- The version, from the game: the integration's `FMOD.VERSION.number` (the header version its
  `System_Create` calls pass), its DllImport names and `dllSuffix`, and the desktop libraries' Linux
  SONAME line (`libfmodstudio.so.13` = 2.02), Windows version resource or macOS bundle version;
  `pmunity plugins` says which files of which release to supply (the `fmod.supply` part of the report).

**Test double.** runtime/host/test/fmod_double.c implements the ~200 FMOD Studio and core calls
the integration makes, from FMOD's public API names and documented signatures (no FMOD code or
header), logging every call; it is built both as an NDK-style Android library and as a glibc
library (`runtime/host/test/plugins_test.sh`). In the unit test (`test_plugins lib`) a softfp caller
standing in for Mono goes through the loader, `plugins.map` and (glibc, 32-bit ARM) the thunks; in
the emulated rig it replaces `libfmodstudio.so` for a corpus game (results below).

## Other SDKs

| SDK | DllImport | default | what happens |
|---|---|---|---|
| Steamworks (Steamworks.NET 9+, Facepunch) | `steam_api`, `steam_api64` | stub `steam` | `SteamAPI_Init*` fail as without a running Steam client (runtime/host/steamstub.c) |
| Steamworks.NET before 9.0 | `CSteamworks` | stub `csteamworks` | `Init`/`InitSafe` false, `RestartAppIfNecessary` false |
| Steam encrypted app tickets | `sdkencryptedappticket` | none | only reached after a successful Steam init |
| Discord Game SDK | `discord_game_sdk` | stub `discord_game_sdk` | `DiscordCreate` -> `NotRunning`; the C# wrapper throws `ResultException`, as without a Discord client |
| Discord RPC | `discord-rpc` | stub `discord-rpc` | every call does nothing (never connects) |
| Epic Online Services | `EOSSDK-*` | stub `eos` | `EOS_Initialize` succeeds, `EOS_Platform_Create` gives no platform (NULL) |
| GOG Galaxy | `GalaxyCSharpGlue`, `Galaxy64` | none | the SWIG glue's first call throws `DllNotFoundException` (GOG builds catch Galaxy failures; Cuphead's does) |
| Discord Social SDK | `discord_partner_sdk` | none | no stub yet |
| Wwise | `AkSoundEngine` | lib | supply Wwise's Android `libAkSoundEngine.so` of the same version; without it the integration logs an error and the game is silent |
| SQLite | `sqlite3` | host `libsqlite3.so.0` | the system's library (32-bit for the armv7 player); missing: `DllNotFoundException` as before |
| StandaloneFileBrowser | `StandaloneFileBrowser` | none | no file dialogs on a handheld |
| Burst | (no DllImport: `lib_burst_generated`) | - | absent on ARM: Burst runs the managed versions of its jobs |
| Unity's Linux launcher | (`ScreenSelector.so`) | - | the desktop player's own dialog; never used |
| Windows / macOS system libraries | `kernel32`, `user32`, `/System/Library/...` | - | fail as on a Linux PC |

Survey of the corpus (the six corpus games and four `svc-*` ones, free itch.io Linux builds) and of
the Plugins folders of 68 more free Linux Mono builds (read from their zip directories): Burst
(17 of 68), Steamworks (11), Discord Game SDK (3), FMOD Studio with Resonance Audio (3), SQLite (1),
StandaloneFileBrowser (1), CSteamworks (1); Unity's ScreenSelector in 26. None ships GOG Galaxy,
EOS or Wwise for Linux; Cuphead's macOS build has GOG Galaxy and CSteamworks P/Invokes.

## HTTPS

**Unity 2018.4 and later** verify certificates in libunity (unitytls on mbedTLS: UnityWebRequest's
native transport from 2019.4, and Mono's TLS for `HttpWebRequest`/`SslStream` of .NET 4.x games).
On Android unitytls takes its trust anchors from Java:

```
TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm()).init((KeyStore) null)
  .getTrustManagers()[0]  (an X509TrustManager)  .getAcceptedIssuers()  ->  X509Certificate[]
  each .getEncoded()  ->  DER, into mbedTLS's CA chain
```

and checks the server's chain, validity and host name itself. runtime/jni/up_tls.c gives these
classes over the handheld's CA bundle: `UA_CA_BUNDLE`, `SSL_CERT_FILE`, then the bundle files of
Linux distributions (`/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`,
`/etc/ssl/ca-bundle.pem`, `/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem`, `/etc/ssl/cert.pem`,
`/etc/ssl/cacert.pem`, ...), then the certificates in `SSL_CERT_DIR`, `/etc/ssl/certs`,
`/system/etc/security/cacerts`. PEM blocks (`CERTIFICATE`, and OpenSSL's `TRUSTED CERTIFICATE` with
its trust settings cut off) become DER; duplicates are dropped. The trust decisions are Android's:
only these system anchors (Android apps since API 24 ignore user-added CAs; there are none here),
and the verification is mbedTLS's inside libunity. With no bundle the anchor list is empty, logged
once, and every HTTPS request fails its certificate check: nothing is accepted unverified.
`X509TrustManager.checkServerTrusted` (which libunity does not call) refuses rather than accepts.
`KeyStore.getInstance("AndroidCAStore")` lists the same certificates.

Test (`runtime/jni/test/tls_test.sh`, a Docker image with Debian's mbedTLS, openssl and Python): a
self-made CA, two local HTTPS servers; a client on mbedTLS (as unitytls) gets its anchors through
JNI exactly as libunity does. It must verify the server of the test CA and read the page, and refuse
the same server under another host name (CN mismatch), a server whose certificate comes from an
unknown CA (not trusted), and every server when there is no bundle; a Debian ca-certificates bundle
(150 certificates) round-trips too, DER equal to openssl's.

**Unity 2017.4 - 2018.4 UnityWebRequest/WWW** go through Unity's Java class
`com.unity3d.player.UnityWebRequest` (HttpURLConnection on a phone). runtime/jni answers every request
as a phone without a network does (UnknownHostException: the request completes with an error at
once). Decision: real HTTP(S) for these versions is worth having only as an option (a port opts in):
the transport is small (the Java class's natives are `uploadCallback`, `downloadCallback`,
`headerCallback`, `contentLengthCallback`, `responseCodeCallback`, `errorCallback`,
`validateCertificateCallback`), the system's libcurl through dlopen would do the HTTP and its own
certificate checks against the same bundle, but the handhelds' 32-bit userlands may have no libcurl,
and the games of those versions that need the network are rare (analytics and news feeds in the
corpus, whose failure changes nothing). It is not implemented yet.

## Corpus results (emulated rig)

See the compat table (docs/compat/README.md, "Plugins and services").
