/*
 * bionic_setjmp.S - setjmp/longjmp for Android code on glibc.
 *
 * bionic's jmp_buf is _JBLEN longs (sigjmp_buf one more): 64 words on 32-bit ARM, 32 on
 * AArch64. glibc's is larger and glibc's setjmp would write past the end of a bionic buffer
 * (mask_was_saved and the saved signal mask). Android code never looks inside, so we use our
 * own layout inside the bionic buffer, in its words (32 or 64 bits):
 *
 *   32-bit ARM                               AArch64
 *   [0]      savemask flag                   [0]       savemask flag
 *   [1..2]   signal mask (kernel, 64 bits)   [1]       signal mask (kernel, 64 bits)
 *   [3..10]  r4-r11                          [2..13]   x19-x30
 *   [11]     sp                              [14]      sp
 *   [12]     lr                              [15..22]  d8-d15
 *   [13..28] d8-d15                          (24 of 32 words)
 *   [29]     fpscr
 *
 * AArch64: the callee-saved registers of AAPCS64 (x19-x29, lr, sp, the low halves of v8-v15).
 * x18 is not kept: Android code never uses it (it is reserved there), and glibc's setjmp leaves
 * it out too. FPCR is not kept, as in bionic's and glibc's.
 */
#if defined(__arm__)
    .syntax unified
    .arm
    .fpu vfpv3-d16
    .text

    .global bionic_sigsetjmp
    .type bionic_sigsetjmp, %function
    .align 2
bionic_sigsetjmp:
    str     r1, [r0, #0]
    add     r2, r0, #12
    stmia   r2!, {r4-r11}
    str     sp, [r2], #4
    str     lr, [r2], #4
    vstmia  r2!, {d8-d15}
    vmrs    r3, fpscr
    str     r3, [r2]
    cmp     r1, #0
    beq     1f
    push    {r0, lr}
    add     r0, r0, #4
    bl      bionic_jmp_savemask
    pop     {r0, lr}
1:  mov     r0, #0
    bx      lr
    .size bionic_sigsetjmp, . - bionic_sigsetjmp

    .global bionic_setjmp
    .type bionic_setjmp, %function
    .align 2
bionic_setjmp:
    mov     r1, #1
    b       bionic_sigsetjmp
    .size bionic_setjmp, . - bionic_setjmp

    .global bionic__setjmp
    .type bionic__setjmp, %function
    .align 2
bionic__setjmp:
    mov     r1, #0
    b       bionic_sigsetjmp
    .size bionic__setjmp, . - bionic__setjmp

    .global bionic_siglongjmp
    .type bionic_siglongjmp, %function
    .align 2
bionic_siglongjmp:
    mov     r4, r0
    mov     r5, r1
    ldr     r2, [r4, #0]
    cmp     r2, #0
    beq     1f
    bic     r3, sp, #7          /* keep the stack 8-byte aligned for the C call */
    mov     sp, r3
    add     r0, r4, #4
    bl      bionic_jmp_restoremask
1:  mov     r0, r5
    add     r12, r4, #12
    ldmia   r12!, {r4-r11}
    ldr     sp, [r12], #4
    ldr     lr, [r12], #4
    vldmia  r12!, {d8-d15}
    ldr     r1, [r12]
    vmsr    fpscr, r1
    cmp     r0, #0
    moveq   r0, #1
    bx      lr
    .size bionic_siglongjmp, . - bionic_siglongjmp

    .global bionic_longjmp
    .type bionic_longjmp, %function
bionic_longjmp = bionic_siglongjmp

    .global bionic__longjmp
    .type bionic__longjmp, %function
bionic__longjmp = bionic_siglongjmp

#elif defined(__aarch64__)
    .text

    .global bionic_sigsetjmp
    .type bionic_sigsetjmp, %function
    .p2align 2
bionic_sigsetjmp:
    str     w1, [x0, #0]
    stp     x19, x20, [x0, #16]
    stp     x21, x22, [x0, #32]
    stp     x23, x24, [x0, #48]
    stp     x25, x26, [x0, #64]
    stp     x27, x28, [x0, #80]
    stp     x29, x30, [x0, #96]
    mov     x2, sp
    str     x2, [x0, #112]
    stp     d8, d9, [x0, #120]
    stp     d10, d11, [x0, #136]
    stp     d12, d13, [x0, #152]
    stp     d14, d15, [x0, #168]
    cbz     w1, 1f
    stp     x0, x30, [sp, #-16]!
    add     x0, x0, #8
    bl      bionic_jmp_savemask
    ldp     x0, x30, [sp], #16
1:  mov     w0, #0
    ret
    .size bionic_sigsetjmp, . - bionic_sigsetjmp

    .global bionic_setjmp
    .type bionic_setjmp, %function
    .p2align 2
bionic_setjmp:
    mov     w1, #1
    b       bionic_sigsetjmp
    .size bionic_setjmp, . - bionic_setjmp

    .global bionic__setjmp
    .type bionic__setjmp, %function
    .p2align 2
bionic__setjmp:
    mov     w1, #0
    b       bionic_sigsetjmp
    .size bionic__setjmp, . - bionic__setjmp

    .global bionic_siglongjmp
    .type bionic_siglongjmp, %function
    .p2align 2
bionic_siglongjmp:
    /* x19/x20 are free: they are reloaded from the buffer below */
    mov     x19, x0
    mov     w20, w1
    ldr     w2, [x19, #0]
    cbz     w2, 1f
    add     x0, x19, #8
    bl      bionic_jmp_restoremask
1:  mov     x0, x19
    mov     w1, w20
    ldp     x19, x20, [x0, #16]
    ldp     x21, x22, [x0, #32]
    ldp     x23, x24, [x0, #48]
    ldp     x25, x26, [x0, #64]
    ldp     x27, x28, [x0, #80]
    ldp     x29, x30, [x0, #96]
    ldr     x2, [x0, #112]
    mov     sp, x2
    ldp     d8, d9, [x0, #120]
    ldp     d10, d11, [x0, #136]
    ldp     d12, d13, [x0, #152]
    ldp     d14, d15, [x0, #168]
    cmp     w1, #0
    csinc   w0, w1, wzr, ne         /* longjmp(buf, 0) returns 1 */
    ret
    .size bionic_siglongjmp, . - bionic_siglongjmp

    .global bionic_longjmp
    .type bionic_longjmp, %function
bionic_longjmp = bionic_siglongjmp

    .global bionic__longjmp
    .type bionic__longjmp, %function
bionic__longjmp = bionic_siglongjmp

#else
#error "bionic_setjmp.S: 32-bit ARM or AArch64 only"
#endif

    .section .note.GNU-stack,"",%progbits
