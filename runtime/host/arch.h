// What the host's crash report (ua_host.c) and stack sampler (hitches.c) need to know about the
// CPU: where a signal context keeps pc, lr and sp, which words can be code addresses, and how wide
// an address prints. 32-bit ARM (armeabi-v7a players) and AArch64 (arm64-v8a players).
#ifndef UA_ARCH_H
#define UA_ARCH_H
#include <stdint.h>
#include <ucontext.h>

#if defined(__aarch64__)
#define UC_PC(uc) ((uintptr_t)(uc)->uc_mcontext.pc)
#define UC_LR(uc) ((uintptr_t)(uc)->uc_mcontext.regs[30])   // x30 is the link register
#define UC_SP(uc) ((uintptr_t)(uc)->uc_mcontext.sp)
// A64 instructions are 4 bytes and 4-byte aligned
static inline int code_aligned(uintptr_t v) { return (v & 3) == 0; }
#elif defined(__arm__)
#define UC_PC(uc) ((uintptr_t)(uc)->uc_mcontext.arm_pc)
#define UC_LR(uc) ((uintptr_t)(uc)->uc_mcontext.arm_lr)
#define UC_SP(uc) ((uintptr_t)(uc)->uc_mcontext.arm_sp)
// a Thumb address has bit 0 set, an ARM one is 4-byte aligned
static inline int code_aligned(uintptr_t v) { return (v & 1) || (v & 3) == 0; }
#else
#error "the host runs Android ARM libraries: build it for 32-bit ARM or AArch64"
#endif

// v, a word on a stack that points into executable memory starting at lo, is a return address:
// the instruction before it is a call (ARM/Thumb BL/BLX, A64 BL/BLR and the pointer-authenticating
// BLRAA/BLRAB forms). The stack samplers (hitches.c, prof.c) keep only such words.
static inline int is_return_address(uintptr_t v, uintptr_t lo)
{
#if defined(__aarch64__)
    if (!code_aligned(v) || v < lo + 4) return 0;
    uint32_t i = *(const uint32_t *)(v - 4);
    return (i & 0xFC000000) == 0x94000000          // BL imm26
        || (i & 0xFFFFFC1F) == 0xD63F0000          // BLR Xn
        || (i & 0xFEFFF800) == 0xD63F0800;         // BLRAA/BLRAAZ/BLRAB/BLRABZ
#else
    if (v & 1) {                                   // Thumb
        uintptr_t a = v - 1;
        if (a < lo + 4) return 0;
        uint16_t h1 = *(const uint16_t *)(a - 4), h2 = *(const uint16_t *)(a - 2);
        if ((h1 & 0xF800) == 0xF000 && (h2 & 0xC000) == 0xC000) return 1;      // BL / BLX imm
        return (h2 & 0xFF87) == 0x4780;                                        // BLX reg
    }
    if ((v & 3) || v < lo + 4) return 0;
    uint32_t i = *(const uint32_t *)(v - 4);
    return (i & 0x0F000000) == 0x0B000000 || (i & 0xFE000000) == 0xFA000000 || (i & 0x0FFFFFF0) == 0x012FFF30;
#endif
}

// hex digits of an address (8 or 16), for "%0*lx"
#define ADDR_HEX ((int)(2 * sizeof(void *)))

#endif
