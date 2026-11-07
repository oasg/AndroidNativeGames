/* Module: crash_0. */
#include "loader_internal.h"

void crash_altstack_install(void) {
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = g_altstack;
    ss.ss_size = sizeof g_altstack;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
}

/* Guest frame names for the thread dump (threads.c calls this weak hook).
 * Prefer the nearest defined symbol in the mapped module, so a dump names
 * the load/job function instead of only main+0x.... */
/* Used by the Android virtual-core async-preemption handler: only preempt a
 * thread interrupted in original guest code at the top level, never inside a
 * host HLE call or a rewritten gate. */
int runtime_guest_code_pc(uint64_t pc) {
    for (unsigned i = 0; i < NMODULES; i++) {
        uintptr_t b = (uintptr_t)modules[i].base;
        if (!b) continue;
        if (pc >= b && pc < b + modules[i].text_end) return 1;
    }
    return 0;
}

int runtime_guest_top_level(uint64_t pc) {
    if (!runtime_guest_code_pc(pc)) return 0;
    uintptr_t tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    uint64_t off = __atomic_load_n(&runtime_host_ctx_off, __ATOMIC_RELAXED);
    /* Not initialised (or a host thread): the host-stack slot cannot prove
     * this is top-level guest code, so never preempt.  The old code returned 1
     * here, which would let SIGUSR2 take the global lock from a host frame. */
    if (!off || !tp) return 0;
    HostContext *hc = *(HostContext **)(tp + off);
    return hc && hc->host_stack_top != 0;
}

void runtime_describe_address(uint64_t addr, char *out, size_t cap) {
    for (unsigned i = 0; i < NMODULES; i++) {
        uintptr_t b = (uintptr_t)modules[i].base;
        if (addr < b || addr >= b + modules[i].size) continue;
        uint64_t off = addr - b;
        const Symbol *best = NULL;
        for (unsigned k = 1; k < modules[i].nsym; k++) {
            const Symbol *sy = &modules[i].symbols[k];
            if (!sy->defined || !sy->name || sy->value > off) continue;
            if (sy->size && off < sy->value + sy->size &&
                (!best || sy->value > best->value))
                best = sy;
        }
        if (best)
            snprintf(out, cap, "%s+0x%llx", best->name,
                     (unsigned long long)(off - best->value));
        else
            snprintf(out, cap, "%s+0x%llx", modules[i].name, (unsigned long long)off);
        return;
    }
    Dl_info di;
    if (dladdr((void *)(uintptr_t)addr, &di) && di.dli_fname) {
        const char *base = strrchr(di.dli_fname, '/');
        snprintf(out, cap, "%s+0x%llx", base ? base + 1 : di.dli_fname,
                 (unsigned long long)(addr - (uintptr_t)di.dli_fbase));
        return;
    }
    snprintf(out, cap, "0x%llx", (unsigned long long)addr);
}

/* Module-relative name for a crash address, with a host symbol fallback. */
void describe_address(uint64_t addr, char *out, size_t cap) {
    for (unsigned i = 0; i < NMODULES; i++) {
        uintptr_t b = (uintptr_t)modules[i].base;
        if (addr >= b && addr < b + modules[i].size) {
            snprintf(out, cap, "%s+%#llx", modules[i].name, (unsigned long long)(addr - b));
            return;
        }
    }
    Dl_info di;
    if (dladdr((void *)(uintptr_t)addr, &di) && di.dli_fname) {
        const char *base = strrchr(di.dli_fname, '/');
        snprintf(out, cap, "%s+%#llx", base ? base + 1 : di.dli_fname,
                 (unsigned long long)(addr - (uintptr_t)di.dli_fbase));
        return;
    }
    snprintf(out, cap, "0x%llx", (unsigned long long)addr);
}

/* Read guest memory from the signal handler without faulting again.  The guest
 * stacks are ordinary mappings of this process; /proc/self/mem turns a stale
 * frame pointer into a short read instead of a nested SIGSEGV. */
int crash_read(uint64_t addr, void *out, size_t n) {
    static int mem = -2;
    /* Bionic returns heap pointers with a top-byte tag.  The CPU ignores it,
     * but /proc/self/mem's file offset is not a user pointer and is not
     * untagged by the kernel, so mask it here. */
    addr &= 0x00ffffffffffffffULL;
    if (mem == -2) mem = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (mem < 0) return 0;
    return pread(mem, out, n, (off_t)addr) == (ssize_t)n;
}

/* Used by the SVC layer for non-fatal guest Break/asserts. */
void runtime_guest_frame_report(uint64_t fp, uint64_t lr) {
    char d[128];
    describe_address(lr, d, sizeof d);
    fprintf(stderr, "[guest] lr=%s fp=%#llx\n", d, (unsigned long long)fp);
    uint64_t chain_fp = fp;
    for (unsigned n = 0; n < 12 && chain_fp && !(chain_fp & 7); n++) {
        uint64_t pair[2];
        if (!crash_read(chain_fp, pair, sizeof pair)) break;
        describe_address(pair[1], d, sizeof d);
        fprintf(stderr, "[guest] frame %u: fp=%#llx lr=%s\n",
                n, (unsigned long long)chain_fp, d);
        if (!pair[0] || (pair[0] & 7) || pair[0] <= chain_fp) break;
        chain_fp = pair[0];
    }
}

/* One-line guest frame chain for the core-stall watchdog (threads.c).  Runs on
 * an ordinary thread, not in a signal handler; /proc/self/mem turns a stale
 * frame pointer into a short read instead of a nested SIGSEGV. */
void chain_append(char *out, size_t cap, size_t *n, const char *s) {
    if (*n >= cap) return;
    int k = snprintf(out + *n, cap - *n, "%s", s);
    if (k > 0) *n += (size_t)k;
    if (*n > cap) *n = cap;
}

int runtime_guest_chain(uint64_t lr, uint64_t fp, char *out, size_t cap) {
    if (!out || !cap) return 0;
    size_t n = 0;
    char d[128];
    describe_address(lr, d, sizeof d);
    chain_append(out, cap, &n, d);
    uint64_t chain_fp = fp;
    for (unsigned i = 0; i < 12 && chain_fp && !(chain_fp & 7); i++) {
        uint64_t pair[2];
        if (!crash_read(chain_fp, pair, sizeof pair)) break;
        describe_address(pair[1], d, sizeof d);
        chain_append(out, cap, &n, " <- ");
        chain_append(out, cap, &n, d);
        if (!pair[0] || (pair[0] & 7) || pair[0] <= chain_fp) break;
        chain_fp = pair[0];
    }
    return 1;
}
