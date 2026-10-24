/* Minimal Android host gate for the synthetic CPU fixture.
 *
 * The rewrite block is:
 *   stp x29,x30,[sp,#-16]!
 *   adrp x30, keypage ; ldr x30,[x30,#KGATE*8] ; blr x30
 *   ldp x29,x30,[sp],#16 ; b original+4
 *
 * On entry x30 is the gate return address and x0..x28 are guest values.
 * fixture_dispatch(ret, regs) may change regs[0..], exactly like the real
 * host gate; every guest register is restored afterwards.
 */
#ifdef __APPLE__
#define G(x) _##x
#else
#define G(x) x
#endif
.text
.globl G(fixture_gate)
.p2align 2
G(fixture_gate):
    bti c                       /* landing pad if the host enables BTI */
    stp x29, x30, [sp, #-16]!
    sub sp, sp, #768
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    str x28, [sp, #224]
    mov x0, x30
    mov x1, sp
    bl G(fixture_dispatch)
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldr x28, [sp, #224]
    add sp, sp, #768
    ldp x29, x30, [sp], #16
    ret

/* uint64_t fixture_call(uint64_t tcb, uint64_t fn, uint64_t arg)
 *
 * Swap TPIDR_EL0 to the fixture TCB, call fn(arg), restore the real TPIDR.
 * The fixture functions and the gate do not call libc, so the C caller is
 * safe as long as it only calls this helper around them.
 */
.globl G(fixture_call)
.p2align 2
G(fixture_call):
    stp x29, x30, [sp, #-16]!
    mrs x9, tpidr_el0
    stp x9, xzr, [sp, #-16]!
    msr tpidr_el0, x0
    mov x0, x2
    blr x1
    ldp x9, xzr, [sp], #16
    msr tpidr_el0, x9
    ldp x29, x30, [sp], #16
    ret
