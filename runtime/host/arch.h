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

// hex digits of an address (8 or 16), for "%0*lx"
#define ADDR_HEX ((int)(2 * sizeof(void *)))

#endif
