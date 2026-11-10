#ifndef UA_MACHO64_H
#define UA_MACHO64_H
// the Mach-O route (macho64.c). scripting: android_package.json's "scripting" ("macho" or
// another). Returns 1 when the bridge is in place, 0 when not asked for, -1 on an error.
int macho64_init(const char *libdir, const char *scripting);
#endif
