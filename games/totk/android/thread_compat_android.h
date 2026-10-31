#pragma once
/* Android/Linux shims for the Darwin-only pieces of runtime/threads.c.
 * Diagnostics that use Mach VM remain available as best-effort memory reads. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef int mach_port_t;
typedef unsigned qos_class_t;
#define QOS_CLASS_USER_INTERACTIVE 0u
#define QOS_CLASS_USER_INITIATED   0u
#define QOS_CLASS_DEFAULT          0u
#define QOS_CLASS_UTILITY          0u
#define QOS_CLASS_BACKGROUND       0u
#define QOS_CLASS_UNSPECIFIED      0u

static inline int pthread_set_qos_class_self_np(qos_class_t q, int rel) {
    (void)q; (void)rel; return 0;
}
static inline int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel) {
    (void)t; if (q) *q = QOS_CLASS_UNSPECIFIED; if (rel) *rel = 0; return 0;
}
static inline mach_port_t pthread_mach_thread_np(pthread_t t) {
    (void)t; return (mach_port_t)gettid();
}
/* Darwin's pthread_threadid_np(NULL, &id) means the calling thread, and that
 * is how the runtime uses it for its own bookkeeping.  The old shim ignored a
 * non-NULL pthread_t and always returned the caller's gettid, so every line of
 * a thread dump showed the dumper's tid.  Resolve the passed thread through
 * pthread_gettid_np (bionic API 21+); without it only the caller is known. */
static inline int pthread_threadid_np(pthread_t t, uint64_t *id) {
    if (!id) return EINVAL;
#if defined(__ANDROID_API__) && __ANDROID_API__ >= 21
    pid_t tid = t ? pthread_gettid_np(t) : gettid();
    if (tid <= 0) { *id = 0; return ESRCH; }
    *id = (uint64_t)tid;
    return 0;
#else
    if (t) { *id = 0; return ENOSYS; }   /* cannot map an arbitrary pthread_t */
    *id = (uint64_t)gettid();
    return 0;
#endif
}
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m,
                                                     const struct timespec *rel) {
    struct timespec now, abs;
    clock_gettime(CLOCK_REALTIME, &now);
    abs.tv_sec = now.tv_sec + rel->tv_sec;
    abs.tv_nsec = now.tv_nsec + rel->tv_nsec;
    if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, m, &abs);
}

typedef uint64_t mach_vm_size_t;
typedef uint64_t mach_vm_address_t;
typedef int kern_return_t;
#define KERN_SUCCESS 0
static inline mach_port_t mach_task_self(void) { return 0; }
static inline kern_return_t mach_vm_read_overwrite(mach_port_t task, mach_vm_address_t addr,
                                                    mach_vm_size_t size, mach_vm_address_t out,
                                                    mach_vm_size_t *got) {
    (void)task;
    memcpy((void *)(uintptr_t)out, (const void *)(uintptr_t)addr, (size_t)size);
    if (got) *got = size;
    return KERN_SUCCESS;
}
