// The Wwise stub's entry points (wwise.c): UA_AK_TRAMPS small functions, 8 bytes apart, each loading
// its number and jumping to a common part that hands the call's arguments, as the caller left them,
// to the C dispatcher, which reads them by the entry's signature (plugins.map's fn line).
//   32-bit ARM (the armeabi-v7a player's Mono: base AAPCS, floats in core registers): r0-r3 are
//   pushed right below the caller's stack arguments, so the dispatcher gets one run of argument
//   words; its 64-bit result is r0:r1 (a float's bits in r0).
//   AArch64: x0-x7 and d0-d7 saved in a frame, and a pointer to the caller's stack arguments; the
//   result goes to both x0 and d0 (an integer or a float/double result, whichever the caller reads).
#define UA_AK_TRAMPS 4096

#if defined(__arm__)
        .syntax unified
        .text
        .arm
        .p2align 2
        .globl  ua_ak_tramps
        .type   ua_ak_tramps, %function
ua_ak_tramps:
        .set    k, 0
        .rept   UA_AK_TRAMPS
        movw    ip, #k
        b       ua_ak_common
        .set    k, k + 1
        .endr
        .size   ua_ak_tramps, . - ua_ak_tramps
        .type   ua_ak_common, %function
ua_ak_common:
        push    {r0-r3}
        push    {r4, lr}                // 24 bytes: the stack stays 8-byte aligned
        mov     r0, ip
        add     r1, sp, #8              // r0-r3, then the caller's stack arguments
        ldr     r12, 1f
2:      add     r12, pc, r12            // ua_ak_dispatch32, ARM or Thumb (the word has its bit 0)
        blx     r12
        pop     {r4, lr}
        add     sp, sp, #16
        bx      lr
        .p2align 2
1:      .word   ua_ak_dispatch32 - (2b + 8)
        .size   ua_ak_common, . - ua_ak_common

#elif defined(__aarch64__)
        .text
        .p2align 2
        .globl  ua_ak_tramps
        .type   ua_ak_tramps, %function
ua_ak_tramps:
        .set    k, 0
        .rept   UA_AK_TRAMPS
        mov     x16, #k
        b       ua_ak_common
        .set    k, k + 1
        .endr
        .size   ua_ak_tramps, . - ua_ak_tramps
        .type   ua_ak_common, %function
ua_ak_common:
        stp     x29, x30, [sp, #-144]!
        mov     x29, sp
        stp     x0, x1, [sp, #16]
        stp     x2, x3, [sp, #32]
        stp     x4, x5, [sp, #48]
        stp     x6, x7, [sp, #64]
        stp     d0, d1, [sp, #80]
        stp     d2, d3, [sp, #96]
        stp     d4, d5, [sp, #112]
        stp     d6, d7, [sp, #128]
        mov     w0, w16
        add     x1, sp, #16             // x0-x7, then d0-d7
        add     x2, sp, #144            // the caller's stack arguments
        bl      ua_ak_dispatch64
        fmov    d0, x0
        ldp     x29, x30, [sp], #144
        ret
        .size   ua_ak_common, . - ua_ak_common
#endif

        .section .note.GNU-stack, "", %progbits
