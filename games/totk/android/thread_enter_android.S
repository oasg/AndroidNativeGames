/* Android/arm64 guest-thread entry and exit, same ABI as runtime/thread_enter.S
 * but without Darwin's leading underscores. */
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
