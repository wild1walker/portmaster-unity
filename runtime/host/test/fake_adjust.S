// test_unityhooks.c's stand-in for AdjustThreadingModeForAPI(mode, renderer, flags): mode 3..6 on
// renderer 11 -> 2, anything else -> mode. Written here (no Unity code): a prologue of the shape the
// players' functions have (ARM state on 32-bit ARM, A64 on AArch64), then a loop whose branch stays
// past the instructions the hook moves. Position-independent: the test copies it into a page.
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
#endif
	.section .note.GNU-stack, "", %progbits
