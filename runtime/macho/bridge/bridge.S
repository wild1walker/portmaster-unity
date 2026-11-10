// bridge.S - the il2cpp_* functions Unity's Android player calls in its libil2cpp.so, each
// passed on to the macOS image's function of that name, and what stands between the image's
// calls and the player's functions (the engine's internal calls). docs/macho-route.md.
//
// Both sides are AArch64 and agree on nearly all of a call. What differs:
//   - an argument narrower than 32 bits: a macOS callee counts on its caller having extended
//     it, an AAPCS64 caller need not. The forwarders extend the ones il2cpp_api.inc lists;
//   - a result narrower than 32 bits: a macOS caller counts on the callee having extended it,
//     an AAPCS64 callee need not. ua_icall_thunk extends an internal call's;
//   - arguments on the stack: macOS packs them at their own size and alignment, AAPCS64 gives
//     each 8 bytes. ua_icall_thunk lays an internal call's out again;
//   - macOS code keeps data in the 128 bytes below sp: every thread that runs it needs its
//     signal handlers on another stack (darwin_thread_enter), which ua_macho_call sees to on a
//     thread's first call.
// x9-x15 are temporaries on both sides (neither passes anything in them): free to use here.

#define U8(i)  and w##i, w##i, 0xff ;
#define S8(i)  sxtb w##i, w##i ;
#define U16(i) and w##i, w##i, 0xffff ;
#define S16(i) sxth w##i, w##i ;
// A class or a method: one of the bridge's own (bridge.c: a pointer into the page
// ua_standin_space, here in x10) never reaches the image.
#define H(i)   eor x9, x10, x##i ; lsr x9, x9, 12 ; cbz x9, 9f ;

// A forwarder, its slot (the image's function, filled by bridge.c; until then and for a name
// the image does not export: ua_miss_NAME, which says the name and stops) and its row in
// ua_api_tab { name, slot, alt }. APIH's forwarder has a second way out, for a call with a
// class or method of the bridge's own: its alt slot holds bridge.c's function for that, or
// ua_altmiss_NAME, which says the name and stops.
#define API_HEAD(name) \
    .text ; .globl name ; .type name, %function ; .balign 4 ; \
name:
#define API_PASS(name) \
    adrp x16, ua_slot_##name ; \
    ldr x16, [x16, :lo12:ua_slot_##name] ; \
    b ua_macho_call ;
#define API_MISS(name) \
ua_miss_##name: \
    adrp x0, ua_str_##name ; \
    add x0, x0, :lo12:ua_str_##name ; \
    b ua_api_missing ; \
    .pushsection .rodata.ua_api_str, "a", %progbits ; ua_str_##name: .asciz #name ; .popsection ; \
    .pushsection .data.ua_api_slots, "aw", %progbits ; .balign 8 ; ua_slot_##name: .quad ua_miss_##name ; .popsection ;
#define API(name, ...) \
    API_HEAD(name) __VA_ARGS__ API_PASS(name) \
    .size name, . - name ; \
    API_MISS(name) \
    .pushsection ua_api_tab, "aw", %progbits ; .balign 8 ; .quad ua_str_##name, ua_slot_##name, 0 ; .popsection ;
#define APIH(name, ...) \
    API_HEAD(name) adrp x10, ua_standin_space ; __VA_ARGS__ API_PASS(name) \
9:  adrp x16, ua_alt_##name ; \
    ldr x16, [x16, :lo12:ua_alt_##name] ; \
    b ua_macho_call ; \
    .size name, . - name ; \
    API_MISS(name) \
ua_altmiss_##name: \
    adrp x0, ua_str_##name ; \
    add x0, x0, :lo12:ua_str_##name ; \
    b ua_standin_missing ; \
    .pushsection .data.ua_api_slots, "aw", %progbits ; .balign 8 ; ua_alt_##name: .quad ua_altmiss_##name ; .popsection ; \
    .pushsection ua_api_tab, "aw", %progbits ; .balign 8 ; .quad ua_str_##name, ua_slot_##name, ua_alt_##name ; .popsection ;

#include "il2cpp_api.inc"

    .text
// x16: the image's function; every argument register as the caller set it.
    .hidden ua_macho_call
    .type ua_macho_call, %function
    .balign 16
ua_macho_call:
    .cfi_startproc
    adrp x17, :gottprel:ua_thread_in
    ldr x17, [x17, :gottprel_lo12:ua_thread_in]
    mrs x9, tpidr_el0
    ldrb w9, [x9, x17]
    cbz w9, 1f
    br x16
1:  sub sp, sp, 224                     // the thread's first call
    .cfi_def_cfa_offset 224
    stp x29, x30, [sp]
    .cfi_offset x29, -224
    .cfi_offset x30, -216
    mov x29, sp
    stp x0, x1, [sp, 16]
    stp x2, x3, [sp, 32]
    stp x4, x5, [sp, 48]
    stp x6, x7, [sp, 64]
    stp x8, x16, [sp, 80]
    stp q0, q1, [sp, 96]
    stp q2, q3, [sp, 128]
    stp q4, q5, [sp, 160]
    stp q6, q7, [sp, 192]
    bl ua_macho_thread_first
    ldp x0, x1, [sp, 16]
    ldp x2, x3, [sp, 32]
    ldp x4, x5, [sp, 48]
    ldp x6, x7, [sp, 64]
    ldp x8, x16, [sp, 80]
    ldp q0, q1, [sp, 96]
    ldp q2, q3, [sp, 128]
    ldp q4, q5, [sp, 160]
    ldp q6, q7, [sp, 192]
    ldp x29, x30, [sp]
    add sp, sp, 224
    .cfi_def_cfa_offset 0
    .cfi_restore x29
    .cfi_restore x30
    br x16
    .cfi_endproc
    .size ua_macho_call, . - ua_macho_call

// An internal call's stub (bridge.c writes them: adr x17, its record; ldr x16, [x17]; br x16)
// comes here with x17 = the record { handler, target, layout }.
//
// ua_icall_lazy: the call's first time. bridge.c reads the method's signature from the
// image's metadata and sets the handler: the player's function itself when the two sides
// agree on the whole call, ua_icall_thunk else.
    .globl ua_icall_lazy
    .hidden ua_icall_lazy
    .type ua_icall_lazy, %function
    .balign 16
ua_icall_lazy:
    .cfi_startproc
    sub sp, sp, 224
    .cfi_def_cfa_offset 224
    stp x29, x30, [sp]
    .cfi_offset x29, -224
    .cfi_offset x30, -216
    mov x29, sp
    stp x0, x1, [sp, 16]
    stp x2, x3, [sp, 32]
    stp x4, x5, [sp, 48]
    stp x6, x7, [sp, 64]
    stp x8, x17, [sp, 80]
    stp q0, q1, [sp, 96]
    stp q2, q3, [sp, 128]
    stp q4, q5, [sp, 160]
    stp q6, q7, [sp, 192]
    mov x0, x17
    bl ua_icall_classify
    ldp x0, x1, [sp, 16]
    ldp x2, x3, [sp, 32]
    ldp x4, x5, [sp, 48]
    ldp x6, x7, [sp, 64]
    ldp x8, x17, [sp, 80]
    ldp q0, q1, [sp, 96]
    ldp q2, q3, [sp, 128]
    ldp q4, q5, [sp, 160]
    ldp q6, q7, [sp, 192]
    ldp x29, x30, [sp]
    add sp, sp, 224
    .cfi_def_cfa_offset 0
    .cfi_restore x29
    .cfi_restore x30
    ldr x16, [x17]
    br x16
    .cfi_endproc
    .size ua_icall_lazy, . - ua_icall_lazy

// ua_icall_thunk: calls the record's target with the stack arguments laid out as AAPCS64 has
// them, and extends a narrow result. The layout (bridge.c, struct icall_layout):
//   u8 result (0 as it is, 1 u8, 2 s8, 3 u16, 4 s16), u8 0, u16 bytes of stack for the callee
//   (a multiple of 16), u16 n, u16 0, then n of { u16 offset in the caller's stack arguments,
//   u16 offset in the callee's, u16 bytes }.
// The frame is described: an exception the player's function raises unwinds through here.
    .globl ua_icall_thunk
    .hidden ua_icall_thunk
    .type ua_icall_thunk, %function
    .balign 16
ua_icall_thunk:
    .cfi_startproc
    stp x29, x30, [sp, -32]!
    .cfi_def_cfa_offset 32
    .cfi_offset x29, -32
    .cfi_offset x30, -24
    mov x29, sp
    .cfi_def_cfa_register x29
    stp x19, x20, [sp, 16]
    .cfi_offset x19, -16
    .cfi_offset x20, -8
    mov x19, x17
    ldr x20, [x19, 16]
    ldrh w9, [x20, 2]
    sub sp, sp, x9
    ldrh w10, [x20, 4]
    add x11, x20, 8
    add x12, x29, 32                    // the caller's stack arguments
1:  cbz w10, 3f
    ldrh w13, [x11]
    ldrh w14, [x11, 2]
    ldrh w15, [x11, 4]
    add x13, x12, x13
    add x14, sp, x14
2:  ldrb w9, [x13], 1
    strb w9, [x14], 1
    subs w15, w15, 1
    b.ne 2b
    add x11, x11, 6
    sub w10, w10, 1
    b 1b
3:  ldr x16, [x19, 8]
    blr x16
    ldrb w9, [x20]
    cbz w9, 9f
    cmp w9, 2
    b.eq 5f
    b.lo 4f
    cmp w9, 3
    b.eq 6f
    sxth w0, w0
    b 9f
4:  and w0, w0, 0xff
    b 9f
5:  sxtb w0, w0
    b 9f
6:  and w0, w0, 0xffff
9:  mov sp, x29
    ldp x19, x20, [sp, 16]
    ldp x29, x30, [sp], 32
    .cfi_def_cfa sp, 0
    .cfi_restore x19
    .cfi_restore x20
    .cfi_restore x29
    .cfi_restore x30
    ret
    .cfi_endproc
    .size ua_icall_thunk, . - ua_icall_thunk

    .section .note.GNU-stack, "", %progbits
