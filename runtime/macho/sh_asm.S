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

// The contexts (sh_context.c). x9: an mcontext. What a call keeps goes into it, with the x0 a
// resumed getcontext returns; x10 is used.
    .macro CTX_SAVE
    stp x19, x20, [x9, #168]
    stp x21, x22, [x9, #184]
    stp x23, x24, [x9, #200]
    stp x25, x26, [x9, #216]
    stp x27, x28, [x9, #232]
    stp x29, x30, [x9, #248]
    mov x10, sp
    str x10, [x9, #264]
    str xzr, [x9, #16]
    str d8, [x9, #424]
    str d9, [x9, #440]
    str d10, [x9, #456]
    str d11, [x9, #472]
    str d12, [x9, #488]
    str d13, [x9, #504]
    str d14, [x9, #520]
    str d15, [x9, #536]
    .endm

// int getcontext(ucontext_t *)
    .globl ua_getcontext
    .hidden ua_getcontext
    .type ua_getcontext, %function
    .balign 16
ua_getcontext:
    .cfi_startproc
    add x9, x0, #64
    CTX_SAVE
    stp x29, x30, [sp, #-16]!
    .cfi_def_cfa_offset 16
    .cfi_offset x29, -16
    .cfi_offset x30, -8
    mov x29, sp
    mov x1, sp
    bl ua_ctx_fill
    ldp x29, x30, [sp], #16
    .cfi_def_cfa_offset 0
    .cfi_restore x29
    .cfi_restore x30
    mov x0, #0
    ret
    .cfi_endproc
    .size ua_getcontext, .-ua_getcontext

// int swapcontext(ucontext_t *, const ucontext_t *): the rest is C, which returns to the
// caller itself when it has nothing to switch to
    .globl ua_swapcontext
    .hidden ua_swapcontext
    .type ua_swapcontext, %function
    .balign 16
ua_swapcontext:
    .cfi_startproc
    cbz x0, 1f
    cbz x1, 1f
    add x9, x0, #64
    CTX_SAVE
1:  mov x2, sp
    b ua_ctx_swap
    .cfi_endproc
    .size ua_swapcontext, .-ua_swapcontext

// void ua_ctx_jump(const void *mcontext): on with what it holds, as a return of 0
    .globl ua_ctx_jump
    .hidden ua_ctx_jump
    .type ua_ctx_jump, %function
    .balign 16
ua_ctx_jump:
    .cfi_startproc
    ldp x19, x20, [x0, #168]
    ldp x21, x22, [x0, #184]
    ldp x23, x24, [x0, #200]
    ldp x25, x26, [x0, #216]
    ldp x27, x28, [x0, #232]
    ldr d8, [x0, #424]
    ldr d9, [x0, #440]
    ldr d10, [x0, #456]
    ldr d11, [x0, #472]
    ldr d12, [x0, #488]
    ldr d13, [x0, #504]
    ldr d14, [x0, #520]
    ldr d15, [x0, #536]
    ldp x29, x30, [x0, #248]
    ldr x9, [x0, #264]
    mov sp, x9
    mov x0, #0
    ret
    .cfi_endproc
    .size ua_ctx_jump, .-ua_ctx_jump

// Where a context made by makecontext starts: sp at its arguments (4 bytes each, eight at
// least), x19 the ucontext, x20 the function, x29 the top of its stack. No caller.
    .globl ua_ctx_start
    .hidden ua_ctx_start
    .type ua_ctx_start, %function
    .balign 16
ua_ctx_start:
    .cfi_startproc
    .cfi_undefined x30
    ldp w0, w1, [sp]
    ldp w2, w3, [sp, #8]
    ldp w4, w5, [sp, #16]
    ldp w6, w7, [sp, #24]
    add sp, sp, #32
    blr x20
    mov sp, x29
    mov x0, x19
    bl ua_ctx_done
    brk #1
    .cfi_endproc
    .size ua_ctx_start, .-ua_ctx_start
    .section .note.GNU-stack,"",%progbits
