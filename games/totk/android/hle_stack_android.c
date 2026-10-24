/* Android HLE thunk table plus the desktop host-call tracer (runtime/hle_stack.c).
 *
 * Every host replacement is wrapped in a fixed 16-byte thunk; the thunk enters
 * runtime_hle_switch (hle_stack_android.S), which switches to the guest thread's
 * host stack before calling the real host function.  TOTK_ANDROID_TRACE_HLE:
 *
 *   0 / 1  count every wrapped call; the exit summary is sorted by count
 *   2      also log the first call of each function (time, guest thread, name)
 *
 * Tracing only adds work when the environment variable is set.  While tracing
 * is on the last call is remembered so a crash report can name it.
 */
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HLE_THUNKS 4096
#define HLE_THUNK_BYTES 16

extern char *__cxa_demangle(const char *, char *, size_t *, int *);

void *runtime_hle_targets[HLE_THUNKS];
const char *runtime_hle_names[HLE_THUNKS];
extern char runtime_hle_thunks[];
int runtime_hle_trace;                       /* read by hle_stack_android.S on every wrapped call */

static _Atomic uint64_t runtime_hle_counts[HLE_THUNKS];
static _Atomic unsigned char runtime_hle_seen[HLE_THUNKS];
static _Atomic int runtime_hle_last = -1;
static uint64_t runtime_hle_t0;
static unsigned runtime_hle_used;
static pthread_mutex_t runtime_hle_lock = PTHREAD_MUTEX_INITIALIZER;

/* Same host function -> same wrapper thunk, so pointers the game compares stay
 * consistent.  The first name given is kept: it names the import the wrapper
 * was made for and is only used by diagnostics. */
void *runtime_hle_wrap_named(void *fn, const char *name) {
    if (!fn) return NULL;
    pthread_mutex_lock(&runtime_hle_lock);
    unsigned i = 0;
    while (i < runtime_hle_used && runtime_hle_targets[i] != fn) i++;
    if (i == runtime_hle_used) {
        if (runtime_hle_used == HLE_THUNKS) {
            pthread_mutex_unlock(&runtime_hle_lock);
            fprintf(stderr, "[totk-android] HLE thunk table full\n");
            return fn;
        }
        runtime_hle_targets[runtime_hle_used++] = fn;
    }
    if (!runtime_hle_names[i] && name) {
        /* Demangle once, during relocation, so logging and the crash summary
           never allocate. */
        int status = -1;
        char *d = name[0] == '_' && name[1] == 'Z' ? __cxa_demangle(name, NULL, NULL, &status) : NULL;
        runtime_hle_names[i] = d && !status ? d : strdup(name);
    }
    pthread_mutex_unlock(&runtime_hle_lock);
    return runtime_hle_thunks + (size_t)i * HLE_THUNK_BYTES;
}

void *runtime_hle_wrap(void *fn) { return runtime_hle_wrap_named(fn, NULL); }

/* A readable name: the demangled import stored at wrap time, else the host
 * function address. */
static void hle_name_of(unsigned i, char *out, size_t cap) {
    const char *n = i < HLE_THUNKS ? runtime_hle_names[i] : NULL;
    if (n) { snprintf(out, cap, "%s", n); return; }
    void *fn = i < HLE_THUNKS ? runtime_hle_targets[i] : NULL;
    snprintf(out, cap, "wrapper %u -> %p", i, fn);
}

void runtime_hle_describe(unsigned i, char *out, size_t cap) {
    if (!out || !cap) return;
    if (i >= HLE_THUNKS) { snprintf(out, cap, "wrapper %u", i); return; }
    hle_name_of(i, out, cap);
}

/* hle_stack_android.S calls this before each wrapped call while tracing (on the
 * host stack, with x0..x8, x16 and q0..q7 still live). */
void runtime_hle_note(uint64_t i) {
    if (i >= HLE_THUNKS) return;
    atomic_fetch_add_explicit(&runtime_hle_counts[i], 1, memory_order_relaxed);
    atomic_store_explicit(&runtime_hle_last, (int)i, memory_order_relaxed);
    runtime_thread_note_hle((unsigned)i);
    if (__atomic_load_n(&runtime_hle_trace, __ATOMIC_RELAXED) != 2) return;
    if (atomic_exchange_explicit(&runtime_hle_seen[i], 1, memory_order_relaxed)) return;
    char text[256];
    hle_name_of((unsigned)i, text, sizeof text);
    fprintf(stderr, "[hle] %8.3f h=%#x %s\n",
            (runtime_now_ns() - runtime_hle_t0) / 1e9, runtime_thread_current_handle(), text);
}

typedef struct { uint64_t n; unsigned i; } hle_hit;

/* Sorted summary since tracing began.  Called from atexit and from the crash
 * handler: the hit table is static and the insertion sort only touches it, so
 * this neither overflows a small guest thread stack nor allocates. */
void runtime_hle_trace_report(void) {
    if (!runtime_hle_t0) return;
    static hle_hit hits[HLE_THUNKS];
    unsigned k = 0;
    uint64_t total = 0;
    for (unsigned i = 0; i < HLE_THUNKS; i++) {
        uint64_t d = atomic_load_explicit(&runtime_hle_counts[i], memory_order_relaxed);
        if (d) { hits[k].n = d; hits[k].i = i; k++; total += d; }
    }
    for (unsigned a = 1; a < k; a++) {
        hle_hit h = hits[a];
        unsigned b = a;
        while (b && (hits[b - 1].n < h.n || (hits[b - 1].n == h.n && hits[b - 1].i > h.i))) {
            hits[b] = hits[b - 1];
            b--;
        }
        hits[b] = h;
    }
    fprintf(stderr, "[hle] tracing off after %.1f s: %llu calls to %u functions\n",
            (runtime_now_ns() - runtime_hle_t0) / 1e9, (unsigned long long)total, k);
    for (unsigned j = 0; j < k; j++) {
        char text[256];
        hle_name_of(hits[j].i, text, sizeof text);
        fprintf(stderr, "[hle] %10llu  %s\n", (unsigned long long)hits[j].n, text);
    }
    runtime_hle_t0 = 0;   /* report once */
}

void runtime_hle_trace_set(int mode) {
    if (mode < 0 || mode > 2) {
        fprintf(stderr, "[hle] trace mode must be 0, 1 or 2\n");
        return;
    }
    if (mode) {
        for (unsigned i = 0; i < HLE_THUNKS; i++) {
            atomic_store_explicit(&runtime_hle_counts[i], 0, memory_order_relaxed);
            atomic_store_explicit(&runtime_hle_seen[i], 0, memory_order_relaxed);
        }
        runtime_hle_t0 = runtime_now_ns();
        __atomic_store_n(&runtime_hle_trace, mode, __ATOMIC_RELEASE);
        fprintf(stderr, "[hle] tracing on: counting every host call%s\n",
                mode == 2 ? ", logging the first call of each function" : "");
        return;
    }
    __atomic_store_n(&runtime_hle_trace, 0, __ATOMIC_RELEASE);
    runtime_hle_trace_report();
}

/* "last HLE: ..." for the crash report; returns 0 when no wrapped call has run. */
int runtime_hle_last_call(char *out, size_t cap) {
    int i = atomic_load_explicit(&runtime_hle_last, memory_order_relaxed);
    if (i < 0 || i >= HLE_THUNKS) return 0;
    runtime_hle_describe((unsigned)i, out, cap);
    return 1;
}

__attribute__((constructor)) static void runtime_hle_trace_from_env(void) {
    const char *v = getenv("TOTK_ANDROID_TRACE_HLE");
    if (!v || !*v) return;
    int mode = atoi(v);
    if (mode < 0 || mode > 2) mode = 1;
    /* Mode 0 is the desktop "stop and print the summary" command; at process
     * start it means "count quietly and print the summary on exit". */
    runtime_hle_trace_set(mode ? mode : 1);
    atexit(runtime_hle_trace_report);
}
