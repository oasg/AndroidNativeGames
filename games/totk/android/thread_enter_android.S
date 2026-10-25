/* Android/arm64 guest-thread entry and exit, same ABI as runtime/thread_enter.S
 * but without Darwin's leading underscores. */
/* Run a guest entry point on its guest stack.
 *   x0 = function, x1 = guest stack top, x2..x6 = a0..a4
 * Guest code sees sp in the guest stack region (and the TLR is inside the same
 * range), which is what nn::os records during initialization. */
.text
.globl runtime_guest_call
.p2align 2
runtime_guest_call:
    stp x19, x30, [sp, #-16]!
    mov x19, sp
    mov x8, x0
    mov sp, x1
    mov x0, x2
    mov x1, x3
    mov x2, x4
    mov x3, x5
    mov x4, x6
    blr x8
    mov sp, x19
    ldp x19, x30, [sp], #16
    ret

.text
.globl dm_thread_enter
.p2align 2
dm_thread_enter:
    mov x4, sp
    stp x4, x29, [x3]
    mov sp, x2
    mov x29, #0
    mov x16, x0
    mov x0, x1
    adr x30, 1f
    br x16
1:  bl dm_thread_returned
    brk #0

.globl runtime_thread_leave
.p2align 2
runtime_thread_leave:
    ldp x4, x29, [x0]
    mov sp, x4
    mov x0, x1
    br x2

.section .note.GNU-stack,"",@progbits
