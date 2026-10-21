// libsteam_api.so stub: the Steamworks flat API without a Steam client (steamstub.c)
#pragma once
#include "../loader/so_loader.h"

// registers the stub unless <libdir>/libsteam_api.so exists (a real ARM build of it)
void steamstub_register(const char *libdir);
// the stub's table (plugins.c registers it under more names when plugins.map asks)
const so_shim *steamstub_table(void);
