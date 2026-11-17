// ua_host's built-in FMOD (fmodstub.c): the libfmod / libfmodstudio a desktop build's FMOD for Unity
// integration P/Invokes, for ports without FMOD's ARM libraries (plugins.map: `stub fmod`)
#pragma once
#include "../loader/so_loader.h"

const so_shim *fmodstub_table(void);
