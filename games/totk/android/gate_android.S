/* Android/arm64 host gate.
 *
 * The rewrite block enters here with x30 = the gate return address and x0..x28
 * live guest values.  We save the guest stack words, switch to the per-thread
 * HostContext stack (TPIDR_EL0 + runtime_host_ctx_off), save every register and
 * FP state, call runtime_dispatch(ret, regs), then restore everything.
 *
 * HostContext layout: [0] = host_stack_top, [8] = host_stack_base.
 */
.text
.globl runtime_gate
.p2align 2
runtime_gate:
    bti c
    stp x16, x17, [sp, #-16]!
    str x30, [sp, #-16]!
    mov x16, sp                    /* guest_sp: [0]=x30, [8]=x16, [16]=x17 */

    mrs x17, tpidr_el0
    adrp x30, runtime_host_ctx_off
    ldr x30, [x30, #:lo12:runtime_host_ctx_off]
    cbz x30, 1f
    ldr x17, [x17, x30]            /* HostContext* */
    cbz x17, 1f
    ldr x30, [x17]                 /* host_stack_top */
    cbz x30, 1f
    str xzr, [x17]                 /* in use; nested gates stay on this stack */
    sub sp, x30, #32
    stp x16, x30, [sp]             /* guest_sp, host_top */
    str x17, [sp, #16]             /* HostContext* */
    b 2f

1:  /* no host stack: use the current stack, 16-byte aligned.  x30 is free here
     * (the guest value was saved at [x16] and is restored below), so it does
     * not clobber a guest register like x9 would. */
    mov x30, x16
    and x30, x30, #0xfffffffffffffff0
    sub sp, x30, #32
    stp x16, xzr, [sp]
    str xzr, [sp, #16]

2:  /* restore guest x30/x16/x17 before saving the full register frame */
    ldr x30, [x16]
    ldr x17, [x16, #16]
    ldr x16, [x16, #8]

    sub sp, sp, #800
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
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    stp q0, q1, [sp, #256]
    stp q2, q3, [sp, #288]
    stp q4, q5, [sp, #320]
    stp q6, q7, [sp, #352]
    stp q8, q9, [sp, #384]
    stp q10, q11, [sp, #416]
    stp q12, q13, [sp, #448]
    stp q14, q15, [sp, #480]
    stp q16, q17, [sp, #512]
    stp q18, q19, [sp, #544]
    stp q20, q21, [sp, #576]
    stp q22, q23, [sp, #608]
    stp q24, q25, [sp, #640]
    stp q26, q27, [sp, #672]
    stp q28, q29, [sp, #704]
    stp q30, q31, [sp, #736]
    mrs x9, nzcv
    str x9, [sp, #768]
    mrs x9, fpsr
    str x9, [sp, #776]
    mrs x9, fpcr
    str x9, [sp, #784]

    mov x0, x30                    /* gate identity = return address */
    mov x1, sp                     /* register frame */
    bl runtime_dispatch

    ldr x9, [sp, #808]             /* host_top */
    cbz x9, 3f
    ldr x10, [sp, #816]            /* HostContext* */
    cbz x10, 3f
    str x9, [x10]                  /* give the host stack back */
3:
    ldr x9, [sp, #784]
    msr fpcr, x9
    ldr x9, [sp, #776]
    msr fpsr, x9
    ldr x9, [sp, #768]
    msr nzcv, x9
    ldp q0, q1, [sp, #256]
    ldp q2, q3, [sp, #288]
    ldp q4, q5, [sp, #320]
    ldp q6, q7, [sp, #352]
    ldp q8, q9, [sp, #384]
    ldp q10, q11, [sp, #416]
    ldp q12, q13, [sp, #448]
    ldp q14, q15, [sp, #480]
    ldp q16, q17, [sp, #512]
    ldp q18, q19, [sp, #544]
    ldp q20, q21, [sp, #576]
    ldp q22, q23, [sp, #608]
    ldp q24, q25, [sp, #640]
    ldp q26, q27, [sp, #672]
    ldp q28, q29, [sp, #704]
    ldp q30, q31, [sp, #736]
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
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    ldr x16, [sp, #800]            /* guest_sp */
    add sp, x16, #32               /* discard the 32-byte guest-stack save */
    ret

.section .note.GNU-stack,"",@progbits
