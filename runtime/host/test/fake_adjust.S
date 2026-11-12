// test_unityhooks.c's stand-in for AdjustThreadingModeForAPI(mode, renderer, flags): mode 3..6 on
// renderer 11 -> 2, anything else -> mode. Written here (no Unity code): a prologue of the shape the
// players' functions have (ARM state on 32-bit ARM, A64 on AArch64), then a loop whose branch stays
// past the instructions the hook moves. Position-independent: the test copies it into a page. On
// AArch64 more stand-ins follow it, for the trampoline's rewriting of pc-relative instructions.
#if defined(__arm__)
	.text
	.arm
	.balign 4
	.globl fake_adjust
	.type fake_adjust, %function
fake_adjust:
	push	{r4, r5, r6, lr}
	mov	r4, r1
	mov	r5, #2
1:	subs	r5, r5, #1
	bne	1b
	mov	r5, r0
	cmp	r4, #11
	bne	2f
	sub	r6, r0, #3
	cmp	r6, #3
	movls	r5, #2
2:	mov	r0, r5
	pop	{r4, r5, r6, pc}
	.globl fake_adjust_end
fake_adjust_end:
#elif defined(__aarch64__)
	.text
	.balign 4
	.globl fake_adjust
	.type fake_adjust, %function
fake_adjust:
	stp	x30, x21, [sp, #-0x20]!
	stp	x20, x19, [sp, #0x10]
	cmp	w0, #0x3
	mov	w19, w1
	mov	w20, #2
1:	subs	w20, w20, #1
	b.ne	1b
	mov	w20, w0
	cmp	w19, #11
	b.ne	2f
	sub	w21, w0, #3
	cmp	w21, #3
	mov	w21, #2
	csel	w20, w21, w20, ls
2:	mov	w0, w20
	ldp	x20, x19, [sp, #0x10]
	ldp	x30, x21, [sp], #0x20
	ret
	.globl fake_adjust_end
fake_adjust_end:

// Stand-ins whose first four instructions (the ones the hook moves) have pc-relative operands, which the
// trampoline rewrites: the shapes of BufferManagerGLES::UpdateLiveSetFromPending (an early b.lt out) and
// of its destructor (a bl) in the arm64 players, then adrp, adr, cbz, tbnz, cbnz and b.
	.balign 4
	.globl fake_cond
fake_cond:				// (x0 -> u64[4]) 7 when the int at +0x18 is < 1, else twice it plus 1
	ldr	x8, [x0, #0x18]
	cmp	w8, #0x1
	b.lt	1f
	str	x30, [sp, #-0x10]!
	lsl	x0, x8, #1
	add	x0, x0, #1
	ldr	x30, [sp], #0x10
	ret
1:	mov	x0, #7
	ret
	.globl fake_cond_end
fake_cond_end:

	.balign 4
	.globl fake_call
fake_call:				// (x0) 4 * x0: the call returns among the moved instructions
	stp	x30, x21, [sp, #-0x20]!
	stp	x20, x19, [sp, #0x10]
	mov	x19, x0
	bl	1f
	add	x0, x0, x19
	ldp	x20, x19, [sp, #0x10]
	ldp	x30, x21, [sp], #0x20
	ret
1:	add	x0, x0, x0, lsl #1
	ret
	.globl fake_call_end
fake_call_end:

	.balign 8
	.globl fake_addr
fake_addr:				// (x0) 0: the adrp's page; 2, 3: the word the adr points at; else their sum
	.inst	0xb0000009		// adrp x9, <the next page>
	adr	x10, 3f
	cbz	x0, 1f
	tbnz	w0, #1, 2f
	ldr	x10, [x10]
	add	x0, x9, x10
	ret
1:	mov	x0, x9
	ret
2:	ldr	x0, [x10]
	ret
3:	.quad	0x1122334455667788
	.globl fake_addr_end
fake_addr_end:

	.balign 4
	.globl fake_jump
fake_jump:				// (w0) 1 for -5, else 2 * (w0 + 5)
	mov	w9, #5
	adds	w0, w0, w9
	cbnz	w0, 1f
	b	2f
1:	lsl	w0, w0, #1
	ret
2:	mov	w0, #1
	ret
	.globl fake_jump_end
fake_jump_end:

// Entries that cannot be moved (the tool gives them no row; the hook refuses them): a literal load, a
// branch to another of the moved instructions
	.balign 8
	.globl fake_literal
fake_literal:				// (x0) x0 + 3
	ldr	x9, 1f
	add	x0, x0, x9
	nop
	ret
1:	.quad	3
	.globl fake_literal_end
fake_literal_end:

	.balign 4
	.globl fake_inner
fake_inner:				// (x0 > 0) 0, after x0 turns of a loop inside the entry
	mov	x9, x0
1:	subs	x9, x9, #1
	b.ne	1b
	mov	x0, x9
	ret
	.globl fake_inner_end
fake_inner_end:
#endif
	.section .note.GNU-stack, "", %progbits
