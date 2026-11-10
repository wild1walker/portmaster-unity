// sh_asm.S - the shims that are not C calls.
    .text

// ___chkstk_darwin: x9 the bytes the caller is about to take below its sp. Every page of them
// is touched, the highest first, so a stack that ends there meets its guard page instead of
// stepping over it. Every register is kept (the flags are not), as macOS's own keeps them.
    .globl ua_chkstk_darwin
    .hidden ua_chkstk_darwin
    .type ua_chkstk_darwin, %function
    .balign 16
ua_chkstk_darwin:
    .cfi_startproc
    stp x10, x11, [sp, #-16]!
    .cfi_def_cfa_offset 16
    add x10, sp, #16                    // the caller's sp
    mov x11, x9
1:  cmp x11, #0x1000
    b.lo 2f
    sub x10, x10, #0x1000
    ldr xzr, [x10]
    sub x11, x11, #0x1000
    b 1b
2:  sub x10, x10, x11
    ldr xzr, [x10]
    ldp x10, x11, [sp], #16
    .cfi_def_cfa_offset 0
    ret
    .cfi_endproc
    .size ua_chkstk_darwin, .-ua_chkstk_darwin
    .section .note.GNU-stack,"",%progbits
