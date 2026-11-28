// The Wwise stub's entry points (wwise.c): UA_AK_TRAMPS small functions, 8 bytes apart, each loading
// its number and jumping to a common part that hands the call's arguments, as the caller left them,
// to the C dispatcher, which reads them by the entry's signature (plugins.map's fn line).
//   x0-x7 and d0-d7 saved in a frame, and a pointer to the caller's stack arguments; the
//   result goes to both x0 and d0 (an integer or a float/double result, whichever the caller reads).
#define UA_AK_TRAMPS 4096

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

        .section .note.GNU-stack, "", %progbits
