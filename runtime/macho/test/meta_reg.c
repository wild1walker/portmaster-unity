// meta_reg.c - a metadata registration as an IL2CPP binary holds one, for the setup's reader
// of a game binary's pointers (setup-rs: androiddata's il2cppmeta.rs, whose test
// `macho_linker_output` reads this file as build-mac.sh links it in each form of fixups, with
// its symbols and without). The registration is 16 words, counts and pointers in turn: the
// fourth pair is the type table, the sixth and the seventh have one entry per type definition.
// Type 2k names a type definition (a plain number), type 2k+1 points to type 2k.
#include <stdint.h>

#define TYPES 4096
#define DEFS 1234

struct type { const void *data; uint32_t bits; };
extern const struct type types[TYPES];

#define T2(i)    { (const void *)(uintptr_t)(0x1000 + (i)), 0x120000 + (i) }, { &types[i], 0x120001 + (i) },
#define T8(i)    T2(i) T2((i) + 2) T2((i) + 4) T2((i) + 6)
#define T32(i)   T8(i) T8((i) + 8) T8((i) + 16) T8((i) + 24)
#define T128(i)  T32(i) T32((i) + 32) T32((i) + 64) T32((i) + 96)
#define T512(i)  T128(i) T128((i) + 128) T128((i) + 256) T128((i) + 384)
#define T2048(i) T512(i) T512((i) + 512) T512((i) + 1024) T512((i) + 1536)
const struct type types[TYPES] = { T2048(0) T2048(2048) };

#define P2(i)    &types[i], &types[(i) + 1],
#define P8(i)    P2(i) P2((i) + 2) P2((i) + 4) P2((i) + 6)
#define P32(i)   P8(i) P8((i) + 8) P8((i) + 16) P8((i) + 24)
#define P128(i)  P32(i) P32((i) + 32) P32((i) + 64) P32((i) + 96)
#define P512(i)  P128(i) P128((i) + 128) P128((i) + 256) P128((i) + 384)
#define P2048(i) P512(i) P512((i) + 512) P512((i) + 1024) P512((i) + 1536)
const struct type *const table[TYPES] = { P2048(0) P2048(2048) };

const int32_t sizes[DEFS] = { 1 };

struct pair { intptr_t count; const void *list; };
__attribute__((visibility("hidden")))
const struct pair g_MetadataRegistration[8] = {
    { 0, 0 }, { 0, 0 }, { 0, 0 }, { TYPES, table }, { 0, 0 }, { DEFS, sizes }, { DEFS, sizes }, { 0, 0 },
};

const void *meta_reg(void) { return g_MetadataRegistration; }
