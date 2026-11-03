// Desktop answers for desktop code on the Android player: the command line, Screen.resolutions (desktopenv.c)
#pragma once
// before Unity starts: Unity's own switches (UA_UNITY_ARGS and what the host adds), the window's
// size and the display's refresh rate
void desktopenv_init(const char *unity_args, int width, int height, int refresh_hz);
// for the host's dlsym hook (so_set_dlsym_hook): wraps Mono's main-arguments setter and the
// Screen.resolutions icall as libunity registers it
void *desktopenv_dlsym_hook(const char *symbol, void *found);
// tests: where Mono's functions come from (default: the loader's handle of the Mono library)
void desktopenv_set_resolver(void *(*sym)(const char *));
