// syn_rl_asm.S: test_relayout.c's AArch64 code, one function per pc-relative instruction class
// (B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, ADR, ADRP + ADD/LDR, LDR/LDRSW/PRFM literal, a jump table, a
// branch to the function's own entry), two the relayout must leave (a literal pool inside the function,
// a SIMD literal load), and two outline-atomic helpers as compiler-rt's lse.S emits them. The rl_far_*
// functions are never in the list and lie behind 1.1 MB of padding: the copies' conditional branches and
// ADRs to them and to rl_near_data do not reach and go through islands.

.macro FN name
    .globl \name
    .type \name, %function
    .p2align 2
\name:
    .cfi_startproc
.endm
.macro END name
    .cfi_endproc
    .size \name, . - \name
.endm

    .text
FN rl_leaf
    add x0, x0, #1
    ret
END rl_leaf

// rl_leaf(x) + rl_far_leaf(x): BL to a copied and to an unmoved function
FN rl_t_bl
    stp x29, x30, [sp, #-32]!
    .cfi_def_cfa_offset 32
    .cfi_offset 29, -32
    .cfi_offset 30, -24
    mov x29, sp
    str x19, [sp, #16]
    .cfi_offset 19, -16
    mov x19, x0
    bl rl_leaf
    mov x1, x0
    mov x0, x19
    mov x19, x1
    bl rl_far_leaf
    add x0, x0, x19
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    .cfi_def_cfa_offset 0
    ret
END rl_t_bl

// a tail call (B) to an unmoved function
FN rl_t_tail
    add x0, x0, #3
    b rl_far_leaf
END rl_t_tail

// B.cond out of reach (island) and CBZ to a copied function
FN rl_t_bcond
    cmp x0, #5
    b.gt rl_far_ret1
    cbz x0, rl_leaf
    mov x0, #2
    ret
END rl_t_bcond

FN rl_t_cbz
    cbz x0, rl_far_ret7
    cmp x0, #1
    b.eq 1f
    mov x0, #3
    ret
1:  mov x0, #33
    ret
END rl_t_cbz

FN rl_t_cbnz
    cbnz w0, rl_far_ret7
    mov x0, #3
    ret
END rl_t_cbnz

FN rl_t_tbz
    tbz x0, #3, rl_far_ret9
    tbnz x0, #4, rl_leaf
    mov x0, #4
    ret
END rl_t_tbz

// ADR to data outside the function (an island: adrp; add; b back)
FN rl_t_adr
    adr x0, rl_near_data
    ldr x0, [x0]
    adr x1, rl_lit_w
    ldr w1, [x1]
    add x0, x0, x1
    ret
END rl_t_adr

// literal loads from outside the function: W, X, SW, an X at an address not 8-aligned, PRFM
FN rl_t_lit
    ldr x0, rl_near_data
    ldr w1, rl_lit_w
    ldrsw x2, rl_lit_w
    ldr x3, rl_lit_unal
    prfm pldl1keep, rl_near_data
    add x0, x0, x1
    add x0, x0, x2
    eor x0, x0, x3
    ret
END rl_t_lit

// ADRP + ADD, ADRP + LDR
FN rl_t_adrp
    adrp x1, rl_data
    add x1, x1, :lo12:rl_data
    ldr x0, [x1]
    adrp x2, rl_data2
    ldr x2, [x2, :lo12:rl_data2]
    add x0, x0, x2
    ret
END rl_t_adrp

// a jump table: byte offsets in .rodata from an ADR inside the function
FN rl_t_jt
    and x0, x0, #3
    adrp x1, rl_jt
    add x1, x1, :lo12:rl_jt
    ldrb w2, [x1, x0]
    adr x3, 1f
    add x3, x3, w2, uxtb #2
    br x3
1:  mov x0, #10
    ret
    mov x0, #11
    ret
    mov x0, #12
    ret
    mov x0, #13
    b rl_leaf
END rl_t_jt

// sum of x0 .. 1 plus x1, looping through a branch to its own entry
FN rl_t_loop
    cbz x0, 1f
    add x1, x1, x0
    sub x0, x0, #1
    b rl_t_loop
1:  mov x0, x1
    ret
END rl_t_loop

// the test changes its second instruction in memory before the relayout: a changed function stays
FN rl_t_hooked
    nop
    mov x0, #5
    ret
END rl_t_hooked

// its own address through ADRP + ADD (the original's, copied or not)
FN rl_t_self
    adrp x0, rl_t_self
    add x0, x0, :lo12:rl_t_self
    ret
END rl_t_self

// stays: a literal pool inside the function
FN rl_t_incode
    ldr x0, 1f
    ret
    .p2align 3
1:  .quad 77
END rl_t_incode

// stays: a SIMD literal from outside
FN rl_t_simdlit
    ldr d0, rl_near_data
    fmov x0, d0
    ret
END rl_t_simdlit

// the outline-atomic helpers, as compiler-rt's lse.S (bti c; the LSE test; the LSE instruction; the loop)
FN rl_ldadd4_relax
    hint #34
    adrp x16, rl_have_lse
    ldrb w16, [x16, :lo12:rl_have_lse]
    cbz w16, 8f
    .inst 0xb8200020            // ldadd w0, w0, [x1]
    ret
8:  mov w16, w0
0:  ldxr w0, [x1]
    add w17, w0, w16
    stxr w15, w17, [x1]
    cbnz w15, 0b
    ret
END rl_ldadd4_relax

FN rl_cas1_acq_rel
    hint #34
    adrp x16, rl_have_lse
    ldrb w16, [x16, :lo12:rl_have_lse]
    cbz w16, 8f
    .inst 0x08e0fc41            // casalb w0, w1, [x2]
    ret
8:  uxtb w16, w0
0:  ldaxrb w0, [x2]
    cmp w0, w16
    b.ne 1f
    stlxrb w17, w1, [x2]
    cbnz w17, 0b
1:  ret
END rl_cas1_acq_rel

// v = *p; *p += x through the helper; and a byte compare-and-swap: returns old + 1000 * cas result
FN rl_t_atomic
    stp x29, x30, [sp, #-32]!
    .cfi_def_cfa_offset 32
    .cfi_offset 29, -32
    .cfi_offset 30, -24
    mov x29, sp
    stp x19, x20, [sp, #16]
    .cfi_offset 19, -16
    .cfi_offset 20, -8
    mov x19, x1
    bl rl_ldadd4_relax          // w0 = old *(int *)x1; *x1 += w0
    mov w20, w0
    mov w0, #0
    mov w1, #9
    add x2, x19, #4
    bl rl_cas1_acq_rel          // byte at x1 + 4: 0 -> 9
    mov w1, #1000
    madd w0, w0, w1, w20
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    .cfi_def_cfa_offset 0
    ret
END rl_t_atomic

// SO_RELAYOUT_DATA: two local functions reached through a table in .data.rel.ro (R_AARCH64_RELATIVE);
// rl_vt_b's address is also formed in code (rl_t_formb), so its table word keeps the original
    .type rl_vt_a, %function
    .p2align 2
rl_vt_a:
    .cfi_startproc
    add x0, x0, #7
    ret
    .cfi_endproc
    .size rl_vt_a, . - rl_vt_a
    .type rl_vt_b, %function
    .p2align 2
rl_vt_b:
    .cfi_startproc
    add x0, x0, #8
    ret
    .cfi_endproc
    .size rl_vt_b, . - rl_vt_b

FN rl_t_formb
    adrp x0, rl_vt_b
    add x0, x0, :lo12:rl_vt_b
    ret
END rl_t_formb

// rl_vtab[x0](x1)
FN rl_t_vcall
    adrp x2, rl_vtab
    add x2, x2, :lo12:rl_vtab
    ldr x3, [x2, x0, lsl #3]
    mov x0, x1
    br x3
END rl_t_vcall

// the targets the list never names, then data among the code (outside every FDE)
FN rl_far_leaf
    add x0, x0, #100
    ret
END rl_far_leaf
FN rl_far_ret1
    mov x0, #1
    ret
END rl_far_ret1
FN rl_far_ret7
    mov x0, #7
    ret
END rl_far_ret7
FN rl_far_ret9
    mov x0, #9
    ret
END rl_far_ret9

    .p2align 3
    .globl rl_near_data
rl_near_data:
    .quad 0x1122334455667788
rl_lit_w:
    .word 0x80000001
rl_lit_unal:
    .quad 0x0102030405060708
    .word 0
    // the copies lie beyond this: out of the reach of B.cond, CBZ, TBZ, ADR and literal loads
    .space 0x110000

    .section .rodata
rl_jt:
    .byte 0, 2, 4, 6

    .section .data.rel.ro, "aw"
    .p2align 3
    .globl rl_vtab
rl_vtab:
    .quad rl_vt_a, rl_vt_b, rl_far_leaf

    .data
    .p2align 3
rl_data:
    .quad 1000
rl_data2:
    .quad 234

    .bss
    .globl rl_have_lse
rl_have_lse:
    .byte 0
