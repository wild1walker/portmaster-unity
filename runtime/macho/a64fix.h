// a64fix.h - A64 code built for Apple silicon (ARMv8.4 and later) on the handhelds' ARMv8.0
// cores: the instructions such a core lacks are found, and the ones a macOS IL2CPP build
// carries are rewritten to ARMv8.0 code with the same result. docs/macho-route.md.
//   ldapr (RCPC)                    -> ldar in place (a stronger ordering, the same value)
//   LSE ld<op>, st<op>, swp, cas    -> a branch to the site's own routine in the island: an
//                                      ldaxr/stlxr loop that keeps every other register and NZCV
//   eor3 (SHA3)                     -> two eor in the island
//   sdot, udot (dot product)        -> widening multiplies and pairwise adds in the island
// Anything else this CPU lacks (casp, FP16, bcax, ...) is counted and the image refused:
// it never runs with an instruction that would trap.
#ifndef UA_A64FIX_H
#define UA_A64FIX_H

#include <stddef.h>
#include <stdint.h>

enum {
    A64_LSE, A64_CAS, A64_CASP, A64_LDAPR, A64_RCPC2, A64_DOT, A64_EOR3, A64_SHA3, A64_SHA512,
    A64_FP16, A64_FRINT, A64_JSCVT, A64_FLAGM, A64_RDM, A64_FHM, A64_CRC, A64_AES, A64_SHA12,
    A64_FCMA, A64_PAUTH, A64_FLAGM2, A64_SB, A64_PSTATE,
    A64_NKIND
};

struct a64fix {
    uint8_t *island;             // writable, executable, within a branch's reach of the code
    size_t   island_bytes, island_used;
    unsigned lacks;              // 1 << kind: what this CPU has no instructions for
    long     found[A64_NKIND];   // by kind, in the code given so far
    long     rewritten, refused;
    char     refused_what[160];
};

// lacks from the CPU (AT_HWCAP). UA_A64FIX=all: as an ARMv8.0 core without the optional
// extensions' newer halves (every kind this file rewrites is rewritten: the Mac-side tests);
// UA_A64FIX=off: nothing is rewritten.
void a64fix_init(struct a64fix *f, uint8_t *island, size_t island_bytes);
// Scans and rewrites [code, code+size) (writable now); skip: n (start, end) pairs of data in the
// code, as LC_DATA_IN_CODE lists them. -1: the island is full or out of reach.
int  a64fix_code(struct a64fix *f, uint8_t *code, size_t size, const uint8_t *const *skip, int nskip);
const char *a64fix_kind_name(int kind);
// The kind of one instruction word (-1: ARMv8.0's own).
int  a64fix_kind(uint32_t insn);

#endif
