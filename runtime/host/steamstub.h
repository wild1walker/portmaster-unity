// libsteam_api.so stub: the Steamworks flat API without a Steam client (steamstub.c)
#pragma once
#include "../loader/so_loader.h"

// registers the stub unless <libdir>/libsteam_api.so exists (a real ARM build of it)
void steamstub_register(const char *libdir);
