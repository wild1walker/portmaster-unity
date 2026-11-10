// tlv_thunk.S - what an image's thread-local descriptor calls (macOS's tlv_get_addr): x0 the
// descriptor { thunk, key, offset }, the variable's address back in x0. The caller counts on
// every register but x0, x16, x17, x30 and the flags, the vector registers included. key is
// the image's place in ua_tlv_slots (tlv.c), which holds the calling thread's block per image;
// a thread's first use of an image's variables goes to ua_tlv_slow, which makes the block.
    .text
    .globl ua_tlv_get_addr
    .hidden ua_tlv_get_addr
    .type ua_tlv_get_addr, %function
    .balign 16
ua_tlv_get_addr:
    .cfi_startproc
    stp x29, x30, [sp, #-32]!
    .cfi_def_cfa_offset 32
    .cfi_offset x29, -32
    .cfi_offset x30, -24
    stp x0, x1, [sp, #16]
    adrp x0, :tlsdesc:ua_tlv_slots
    ldr x1, [x0, #:tlsdesc_lo12:ua_tlv_slots]
    add x0, x0, :tlsdesc_lo12:ua_tlv_slots
    .tlsdesccall ua_tlv_slots
    blr x1
    mrs x16, tpidr_el0
    add x16, x16, x0                    // the thread's ua_tlv_slots
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #32
    .cfi_def_cfa_offset 0
    .cfi_restore x29
    .cfi_restore x30
    ldr x17, [x0, #8]                   // key
    ldr x17, [x16, x17, lsl #3]
    cbz x17, 1f
    ldr x16, [x0, #16]                  // offset
    add x0, x17, x16
    ret
1:  sub sp, sp, #672
    stp x29, x30, [sp]
    .cfi_def_cfa_offset 672
    .cfi_offset x29, -672
    .cfi_offset x30, -664
    mov x29, sp
    stp x1, x2, [sp, #16]
    stp x3, x4, [sp, #32]
    stp x5, x6, [sp, #48]
    stp x7, x8, [sp, #64]
    stp x9, x10, [sp, #80]
    stp x11, x12, [sp, #96]
    stp x13, x14, [sp, #112]
    stp x15, x18, [sp, #128]
    stp q0, q1, [sp, #144]
    stp q2, q3, [sp, #176]
    stp q4, q5, [sp, #208]
    stp q6, q7, [sp, #240]
    stp q8, q9, [sp, #272]
    stp q10, q11, [sp, #304]
    stp q12, q13, [sp, #336]
    stp q14, q15, [sp, #368]
    stp q16, q17, [sp, #400]
    stp q18, q19, [sp, #432]
    stp q20, q21, [sp, #464]
    stp q22, q23, [sp, #496]
    stp q24, q25, [sp, #528]
    stp q26, q27, [sp, #560]
    stp q28, q29, [sp, #592]
    stp q30, q31, [sp, #624]
    bl ua_tlv_slow
    ldp x1, x2, [sp, #16]
    ldp x3, x4, [sp, #32]
    ldp x5, x6, [sp, #48]
    ldp x7, x8, [sp, #64]
    ldp x9, x10, [sp, #80]
    ldp x11, x12, [sp, #96]
    ldp x13, x14, [sp, #112]
    ldp x15, x18, [sp, #128]
    ldp q0, q1, [sp, #144]
    ldp q2, q3, [sp, #176]
    ldp q4, q5, [sp, #208]
    ldp q6, q7, [sp, #240]
    ldp q8, q9, [sp, #272]
    ldp q10, q11, [sp, #304]
    ldp q12, q13, [sp, #336]
    ldp q14, q15, [sp, #368]
    ldp q16, q17, [sp, #400]
    ldp q18, q19, [sp, #432]
    ldp q20, q21, [sp, #464]
    ldp q22, q23, [sp, #496]
    ldp q24, q25, [sp, #528]
    ldp q26, q27, [sp, #560]
    ldp q28, q29, [sp, #592]
    ldp q30, q31, [sp, #624]
    ldp x29, x30, [sp]
    add sp, sp, #672
    .cfi_def_cfa_offset 0
    .cfi_restore x29
    .cfi_restore x30
    ret
    .cfi_endproc
    .size ua_tlv_get_addr, .-ua_tlv_get_addr
    .section .note.GNU-stack,"",%progbits
