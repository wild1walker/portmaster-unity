/* libhybris build configuration for the Unity Android port: only hooks.c (libhybris's bionic ->
 * glibc compatibility functions) is used, with our own ELF loader; libhybris's linker plugins are not. */
#define LINKER_VERSION_DEFAULT 19          /* Android API level the hooks emulate (Unity 2017.4: 16+) */
#define LINKER_NAME_DEFAULT "jb"
#define LINKER_PLUGIN_DIR "/nonexistent"   /* no libhybris linker plugins */
