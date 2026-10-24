/* Android/arm64 host-stack switch for HLE calls.
 *
 * A guest call through the PLT/GOT enters a thunk with x16 = host function and
 * x17 = wrapper index.  Switch to the calling thread's HostContext stack, call
 * the host function, then restore the guest stack.  Nested calls stay on the
 * host stack because HostContext->host_stack_top is cleared while it is used.
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
    stp x29, x30, [sp]
    stp x13, x12, [sp, #16]
    mov x29, sp
    blr x16
    ldp x13, x12, [sp, #16]
    ldp x29, x30, [sp]
    mov sp, x13
    mrs x9, tpidr_el0
    adrp x10, runtime_host_ctx_off
    ldr x10, [x10, #:lo12:runtime_host_ctx_off]
    ldr x11, [x9, x10]
    str x12, [x11]
    ret

1:  br x16

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
