/*
 * bionic_setjmp.S - setjmp/longjmp for Android code on glibc.
 *
 * bionic's jmp_buf is 64 words (sigjmp_buf 65); glibc's is larger and glibc's
 * setjmp would write past the end of a bionic buffer (mask_was_saved and the
 * saved signal mask). We use our own layout inside the bionic buffer:
 *   [0]      savemask flag
 *   [1..2]   saved signal mask (kernel sigset, 64 bits)
 *   [3..10]  r4-r11
 *   [11]     sp
 *   [12]     lr
 *   [13..28] d8-d15
 *   [29]     fpscr
 */
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

    .section .note.GNU-stack,"",%progbits
