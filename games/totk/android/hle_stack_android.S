/* Android/arm64 host-stack switch for HLE calls.
 *
 * A guest call through the PLT/GOT enters a thunk with x16 = host function and
 * x17 = wrapper index.  Switch to the calling thread's HostContext stack, call
 * the host function, then restore the guest stack.  Nested calls stay on the
 * host stack because HostContext->host_stack_top is cleared while it is used.
 *
 * While _runtime_hle_trace is set (TOTK_ANDROID_TRACE_HLE), every call is first
 * counted by _runtime_hle_note on the host stack, with the argument registers
 * kept (hle_note_kept, ported from runtime/hle_stack.S).
 */
.text
.globl runtime_hle_thunks
.p2align 2

.globl runtime_hle_dispatch
.p2align 2
runtime_hle_dispatch:
    adrp x16, runtime_hle_targets
    add x16, x16, #:lo12:runtime_hle_targets
    ldr x16, [x16, x17, lsl #3]
    b runtime_hle_switch

.p2align 2
runtime_hle_switch:
    mrs x9, tpidr_el0
    adrp x10, runtime_host_ctx_off
    ldr x10, [x10, #:lo12:runtime_host_ctx_off]
    cbz x10, 1f
    ldr x11, [x9, x10]            /* HostContext* */
    cbz x11, 1f
    ldr x12, [x11]                /* host_stack_top */
    cbz x12, 1f
    str xzr, [x11]
    mov x13, sp
    sub sp, x12, #32
    stp x29, x30, [sp]            /* frame record links the host frames back to the guest caller */
    stp x13, x12, [sp, #16]
    mov x29, sp
    adrp x14, runtime_hle_trace
    ldr w14, [x14, #:lo12:runtime_hle_trace]
    cbnz w14, 2f
3:  blr x16
    ldp x13, x12, [sp, #16]
    ldp x29, x30, [sp]
    mov sp, x13
    mrs x9, tpidr_el0
    adrp x10, runtime_host_ctx_off
    ldr x10, [x10, #:lo12:runtime_host_ctx_off]
    ldr x11, [x9, x10]
    str x12, [x11]
    ret

1:  adrp x14, runtime_hle_trace       /* already on a host stack (nested call, or a host thread) */
    ldr w14, [x14, #:lo12:runtime_hle_trace]
    cbnz w14, 4f
    br x16
2:  bl hle_note_kept                  /* x30 is in the frame record and is reloaded from it after the call */
    b 3b
4:  stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl hle_note_kept
    ldp x29, x30, [sp], #16
    br x16

/* _runtime_hle_note(x17) with x0..x8, x16 and q0..q7 kept. */
    .p2align 2
hle_note_kept:
    stp x29, x30, [sp, #-224]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp x8, x16, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    mov x0, x17
    bl runtime_hle_note
    ldp q6, q7, [sp, #192]
    ldp q4, q5, [sp, #160]
    ldp q2, q3, [sp, #128]
    ldp q0, q1, [sp, #96]
    ldp x8, x16, [sp, #80]
    ldp x6, x7, [sp, #64]
    ldp x4, x5, [sp, #48]
    ldp x2, x3, [sp, #32]
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #224
    ret

/* 16-byte thunks: thunk i enters the dispatch with x17 = i. */
.p2align 4
runtime_hle_thunks:
.set hle_i, 0
.rept 4096
    mov x17, #hle_i
    b runtime_hle_dispatch
    brk #0xb5
    brk #0xb5
.set hle_i, hle_i+1
.endr

.section .note.GNU-stack,"",@progbits
