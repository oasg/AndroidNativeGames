/* Module: svc. */
#include "services_internal.h"

static uint8_t user_exception_context[0x1000] __attribute__((aligned(0x1000)));

/* The Android rewrite resolves GetCurrentStackInfo through the guest TLR:
 * [TPIDRRO_EL0 + 0x1f8] is the ThreadType, whose +0x50/+0x58 are the stack
 * base and size.  The TLR must lie inside that range, as on Horizon where the
 * thread-local region is carved out of the thread's stack. */
void runtime_guest_stack_info(uint64_t *base, uint64_t *size) {
    memory_init();
    *base = (uintptr_t)low_space + STACK_OFF;
    *size = STACK_SIZE;
}

uint64_t get_info(unsigned id) {
    switch (id) {
        case 0: return 7;
        case 1: return 0x0ffffffff0000000ull;
        case 2: return (uintptr_t)alias_space;
        case 3: return alias_size;
        case 4: return (uintptr_t)heap;
        case 5: return HEAP_CAP;
        case 6: case 21: return APP_POOL;
        case 7: case 22: return runtime_image_bytes() + heap_size;
        case 8: case 10: return 0;
        case 11: { uint64_t v = 0; FILE *f = fopen("/dev/urandom", "rb"); if (f) { if (fread(&v, 1, 8, f) != 8) v = 0; fclose(f); } return v; }
        case 12: return (uintptr_t)low_space;
        case 13: return GUEST_LIMIT - (uintptr_t)low_space;
        case 14: return (uintptr_t)low_space + STACK_OFF;
        case 15: return STACK_SIZE;
        case 16: return 32ull << 20;
        case 17: return 0x1000;
        case 18: return 0x0100F2C0115B6000ull;
        case 20: return (uintptr_t)user_exception_context;
        case 23: return 1;
        case 28: return 0;
        default: return 0;
    }
}

uint64_t runtime_svc(unsigned num, uint64_t *r, const char *module, uint64_t offset) {
    runtime_core_checkpoint();
    int trace = -1;
    if (trace < 0) trace = getenv("TOTK_ANDROID_TRACE_SVC") != NULL;
    /* Low-noise SVC histogram for boot stalls: tells whether a hot
     * WaitSynchronization (0x18) is paired with ResetSignal/ClearEvent. */
    static int stats = -1;
    if (stats < 0) stats = getenv("TOTK_ANDROID_SVC_STATS") != NULL;
    if (stats) {
        static uint64_t total, c11, c12, c17, c18, c34, c35;
        uint64_t n = __atomic_add_fetch(&total, 1, __ATOMIC_RELAXED);
        if (num == 0x11) __atomic_add_fetch(&c11, 1, __ATOMIC_RELAXED);
        else if (num == 0x12) __atomic_add_fetch(&c12, 1, __ATOMIC_RELAXED);
        else if (num == 0x17) __atomic_add_fetch(&c17, 1, __ATOMIC_RELAXED);
        else if (num == 0x18) __atomic_add_fetch(&c18, 1, __ATOMIC_RELAXED);
        else if (num == 0x34) __atomic_add_fetch(&c34, 1, __ATOMIC_RELAXED);
        else if (num == 0x35) __atomic_add_fetch(&c35, 1, __ATOMIC_RELAXED);
        if (n % 1000000 == 0) {
            uint32_t h0 = 0;
            if (num == 0x18 && r[1] && r[2] >= 1) memcpy(&h0, (const void *)(uintptr_t)r[1], 4);
            fprintf(stderr, "[totk-svc] total=%llu 0x11=%llu 0x12=%llu 0x17=%llu 0x18=%llu 0x34=%llu 0x35=%llu h=%#x n=%llu to=%lld\n",
                    (unsigned long long)n, (unsigned long long)__atomic_load_n(&c11, __ATOMIC_RELAXED),
                    (unsigned long long)__atomic_load_n(&c12, __ATOMIC_RELAXED),
                    (unsigned long long)__atomic_load_n(&c17, __ATOMIC_RELAXED),
                    (unsigned long long)__atomic_load_n(&c18, __ATOMIC_RELAXED),
                    (unsigned long long)__atomic_load_n(&c34, __ATOMIC_RELAXED),
                    (unsigned long long)__atomic_load_n(&c35, __ATOMIC_RELAXED),
                    h0, num == 0x18 ? (unsigned long long)r[2] : 0,
                    num == 0x18 ? (long long)r[3] : 0);
        }
    }
    if (trace)
        fprintf(stderr, "[totk-android] SVC %#x at %s+%#llx x0=%#llx x1=%#llx x2=%#llx\n",
                num, module, (unsigned long long)offset,
                (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2]);
    static int trace_sync = -1;
    if (trace_sync < 0) trace_sync = getenv("TOTK_ANDROID_TRACE_SYNC") != NULL;
    if (num == 0x18 && trace_sync &&
        r[1] && *(const uint32_t *)(uintptr_t)r[1] == 0) {
        static int shown;
        if (shown++ < 3) {
            fprintf(stderr, "[svc-sync] handle0 at %s+%#llx lr=%#llx fp=%#llx\n",
                    module, (unsigned long long)offset,
                    (unsigned long long)r[30], (unsigned long long)r[29]);
            fprintf(stderr, "[svc-sync]   harr=%#llx lr0=%#llx fp0=%#llx\n",
                    (unsigned long long)r[1], (unsigned long long)r[30],
                    (unsigned long long)r[29]);
            if (r[1]) {
                uint64_t w[8] = { 0 };
                memcpy(w, (const void *)(uintptr_t)(r[1] - 16), sizeof w);
                fprintf(stderr, "[svc-sync]   harr-16: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
                        (unsigned long long)w[0], (unsigned long long)w[1],
                        (unsigned long long)w[2], (unsigned long long)w[3],
                        (unsigned long long)w[4], (unsigned long long)w[5],
                        (unsigned long long)w[6], (unsigned long long)w[7]);
            }
            uint64_t fp = r[29];
            for (int i = 0; i < 8 && fp; i++) {
                uint64_t pair[2] = { 0, 0 };
                memcpy(pair, (const void *)(uintptr_t)fp, sizeof pair);
                if (!pair[0] && !pair[1]) break;
                fprintf(stderr, "[svc-sync]   frame %d fp=%#llx lr=%#llx\n", i,
                        (unsigned long long)pair[0], (unsigned long long)pair[1]);
                if (pair[0] <= fp || (pair[0] & 7)) break;
                fp = pair[0];
            }
        }
    }
    if (svc_thread((int)num, r)) return r[0];
    if (runtime_ipc_svc(num, r)) return r[0];
    memory_init();
    switch (num) {
        case 0x01: { /* SetHeapSize */
            size_t size = (size_t)r[1];
            if (size > HEAP_CAP || (size & 0x1fffff) || size + runtime_image_bytes() > APP_POOL) { r[0] = 0xd401; return r[0]; }
            if (size > heap_size && mprotect(heap + heap_size, size - heap_size, PROT_READ | PROT_WRITE)) runtime_fail("commit heap");
            if (size < heap_size && mprotect(heap + size, heap_size - size, PROT_NONE)) runtime_fail("decommit heap");
            heap_size = size; r[0] = 0; r[1] = (uintptr_t)heap; return 0;
        }
        case 0x02: case 0x03: r[0] = 0; return 0;
        case 0x04: { /* MapMemory(dst, src, size): alias/copy the backing pages */
            uint64_t dst = r[0], src = r[1], size = r[2];
            int dst_low = low_space && dst >= (uintptr_t)low_space &&
                          dst + size <= (uintptr_t)low_space + LOW_SIZE;
            int dst_alias = alias_space && dst >= (uintptr_t)alias_space &&
                            dst + size <= (uintptr_t)alias_space + alias_size;
            if (!size || (dst & 0xfff) || (size & 0xfff) || (!dst_low && !dst_alias)) {
                r[0] = 0xd401; return r[0];
            }
            if (mprotect((void *)(uintptr_t)dst, (size_t)size, PROT_READ | PROT_WRITE)) {
                r[0] = 0xd401; return r[0];
            }
            /* Phase 1 aliases are copies: the host has no userfaultfd-backed
             * shared mapping for the guest alias window yet. */
            if (src && src != dst) {
                mprotect((void *)(uintptr_t)src, (size_t)size, PROT_READ | PROT_WRITE);
                memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)size);
            }
            if (dst_low) runtime_guest_commit(dst, size);
            r[0] = 0; return 0;
        }
        case 0x05: { /* UnmapMemory(dst, src, size): undo case 0x04 */
            uint64_t dst = r[0], size = r[2];
            int dst_low = low_space && dst >= (uintptr_t)low_space &&
                          dst + size <= (uintptr_t)low_space + LOW_SIZE;
            int dst_alias = alias_space && dst >= (uintptr_t)alias_space &&
                            dst + size <= (uintptr_t)alias_space + alias_size;
            if (!size || (dst & 0xfff) || (size & 0xfff) || (!dst_low && !dst_alias)) {
                r[0] = 0xd401; return r[0];
            }
            if (mprotect((void *)(uintptr_t)dst, (size_t)size, PROT_NONE)) {
                r[0] = 0xd401; return r[0];
            }
            if (dst_low) runtime_guest_uncommit(dst, size);
            r[0] = 0; return 0;
        }
        case 0x06: query_memory(r[0], r[2]); r[0] = 0; r[1] = 0; return 0;
        case 0x13: case 0x14: { /* MapSharedMemory / UnmapSharedMemory */
            uint32_t h = (uint32_t)r[0];
            uint64_t addr = r[1], size = r[2];
            IpcTransferMemory *tm = runtime_handle_get(h, 0);
            fprintf(stderr, "[totk-android] SVC %#x shared h=%#x addr=%#llx size=%#llx tm=%p\n",
                    num, h, (unsigned long long)addr, (unsigned long long)size, (void *)tm);
            if (num == 0x13) {
                if (!tm || !addr || !size || addr + size < addr || size > tm->size) {
                    r[0] = 0xd401; return 0;
                }
                if (mprotect((void *)(uintptr_t)addr, (size_t)size, PROT_READ | PROT_WRITE)) {
                    r[0] = 0xd401; return 0;
                }
                /* Phase 1: copy the transfer-memory content; the host has no
                 * userfaultfd-backed shared alias yet. */
                if (addr != tm->addr) memcpy((void *)(uintptr_t)addr,
                                             (const void *)(uintptr_t)tm->addr, (size_t)size);
            } else {
                if (!addr || !size || (addr & 0xfff) || (size & 0xfff)) { r[0] = 0xd401; return 0; }
                if (mprotect((void *)(uintptr_t)addr, (size_t)size, PROT_NONE)) { r[0] = 0xd401; return 0; }
            }
            r[0] = 0; return 0;
        }
        case 0x0b: { int64_t ns = (int64_t)r[0]; if (ns > 0) { struct timespec t = { ns / 1000000000, ns % 1000000000 }; nanosleep(&t, NULL); } r[0] = 0; return 0; }
        case 0x0c: case 0x0d: case 0x0e: case 0x0f: r[0] = 0; return 0;
        case 0x10: r[0] = 0; return 0;
        case 0x24: r[0] = 0; r[1] = 1; return 0;   /* GetProcessId: one application process */
        case 0x25: r[0] = 0; r[1] = (uint64_t)gettid(); return 0;
        case 0x26: {
            uint32_t result = 0;
            if (r[1] && r[2] >= 4) memcpy(&result, (void *)(uintptr_t)r[1], 4);
            fprintf(stderr, "[totk-android] guest Break reason=%#llx result=%#x at %s+%#llx\n",
                    (unsigned long long)r[0], result, module, (unsigned long long)offset);
            runtime_guest_frame_report(r[29], r[30]);
            r[0] = 0; return 0;
        }
        case 0x27: { size_t n = (size_t)r[1]; if (n > 65536) n = 65536; fwrite((void *)(uintptr_t)r[0], 1, n, stderr); r[0] = 0; return 0; }
        /* SynchronizePreemptionState (Horizon 8.0.0+).  The SDK's user-space
         * locks issue it while they spin on a contended lock.  It has no work
         * for this host; the SVC-entry checkpoint above is the useful part. */
        case 0x36: r[0] = 0; return 0;
        case 0x29: r[0] = 0; r[1] = get_info((unsigned)r[1]); return 0;
        case 0x2c: { /* MapPhysicalMemory(addr, size): commit guest pages */
            uint64_t addr = r[0], size = r[1];
            int in_alias = alias_space && addr >= (uintptr_t)alias_space &&
                           addr + size <= (uintptr_t)alias_space + alias_size;
            int in_low = low_space && addr >= (uintptr_t)low_space &&
                         addr + size <= (uintptr_t)low_space + LOW_SIZE;
            if (!size || (addr & 0xfff) || (size & 0xfff) || (!in_alias && !in_low)) {
                fprintf(stderr, "[totk-android] MapPhysicalMemory %#llx+%#llx outside guest space\n",
                        (unsigned long long)addr, (unsigned long long)size);
                r[0] = 0xd401; return r[0];
            }
            if (mprotect((void *)(uintptr_t)addr, (size_t)size, PROT_READ | PROT_WRITE)) {
                fprintf(stderr, "[totk-android] MapPhysicalMemory mprotect: %s\n", strerror(errno));
                r[0] = 0xd401; return r[0];
            }
            if (in_low) runtime_guest_commit(addr, size);
            r[0] = 0; return 0;
        }
        case 0x2d: { /* UnmapPhysicalMemory(addr, size) */
            uint64_t addr = r[0], size = r[1];
            int in_alias = alias_space && addr >= (uintptr_t)alias_space &&
                           addr + size <= (uintptr_t)alias_space + alias_size;
            int in_low = low_space && addr >= (uintptr_t)low_space &&
                         addr + size <= (uintptr_t)low_space + LOW_SIZE;
            if (!size || (addr & 0xfff) || (size & 0xfff) || (!in_alias && !in_low)) {
                r[0] = 0xd401; return r[0];
            }
            if (mprotect((void *)(uintptr_t)addr, (size_t)size, PROT_NONE)) {
                r[0] = 0xd401; return r[0];
            }
            if (in_low) runtime_guest_uncommit(addr, size);
            r[0] = 0; return 0;
        }
        default:
            fprintf(stderr, "[totk-android] unimplemented SVC %#x at %s+%#llx x0=%#llx x1=%#llx\n",
                    num, module, (unsigned long long)offset,
                    (unsigned long long)r[0], (unsigned long long)r[1]);
            r[0] = 0;
            return 0;
    }
}
