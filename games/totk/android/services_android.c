/* Minimal Android Horizon SVC layer for the first SDK-start probe.
 * Thread/synchronization SVCs come from runtime/threads.c (svc_thread);
 * memory/time/debug are the small set nn::init::Start needs before nnMain. */
#include "runtime.h"
#include "ipc.h"
#include "runtime_tables.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE MAP_FIXED
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#define GUEST_LIMIT (1ull << 39)
#define LOW_AT 0x300000000ull
#define STACK_OFF 0x10000000ull
#define STACK_SIZE 0x10000000ull
#define HEAP_OFF (STACK_OFF + STACK_SIZE + 0x10000000ull)
#define HEAP_CAP (4ull << 30)
#define LOW_SIZE (HEAP_OFF + HEAP_CAP)
#define ALIAS_AT 0x7000000000ull
#define ALIAS_MAX (64ull << 30)
#define APP_POOL 0xcd500000ull

void runtime_guest_frame_report(uint64_t fp, uint64_t lr);

static uint8_t *low_space, *alias_space, *heap;
static size_t heap_size, alias_size;
static uint8_t user_exception_context[0x1000] __attribute__((aligned(0x1000)));

/* Committed pages inside the low guest address space.  The low region itself
 * is only a reservation; QueryMemory must not claim all of it, otherwise the
 * SDK's AddressSpaceAllocator cannot find a thread-stack guard page.  One entry
 * per guest thread stack plus every MapMemory/MapPhysicalMemory range, so keep
 * room for more than the runtime's 256-thread stack limit. */
#define MAX_COMMITS 1024
static struct { uintptr_t base, end; } commits[MAX_COMMITS];
static unsigned ncommits;
/* Host-backed guest ranges outside the low reservation (for example the nvdrv
 * nvmap window).  QueryMemory must report these as mapped, otherwise the
 * SDK's address allocator could hand the same address to the game. */
#define MAX_EXTRA 8
static struct { uintptr_t base, end; } extra_ranges[MAX_EXTRA];
static unsigned nextra;
static pthread_mutex_t commit_lock = PTHREAD_MUTEX_INITIALIZER;
void runtime_guest_map_extra(uintptr_t base, uintptr_t size) {
    if (!base || !size) return;
    pthread_mutex_lock(&commit_lock);
    if (nextra < MAX_EXTRA) {
        extra_ranges[nextra].base = base;
        extra_ranges[nextra].end = base + size;
        nextra++;
    } else {
        static int warned;
        if (!warned++) fprintf(stderr, "[totk-android] extra guest range table full\n");
    }
    pthread_mutex_unlock(&commit_lock);
}

void runtime_guest_commit(uintptr_t base, uintptr_t size) {
    if (!size) return;
    size_t ps = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t lo = base & ~(uintptr_t)(ps - 1);
    uintptr_t hi = (base + size + ps - 1) & ~(uintptr_t)(ps - 1);
    pthread_mutex_lock(&commit_lock);
    for (unsigned i = 0; i < ncommits; i++) {
        if (lo <= commits[i].end && hi >= commits[i].base) {
            if (lo < commits[i].base) commits[i].base = lo;
            if (hi > commits[i].end) commits[i].end = hi;
            pthread_mutex_unlock(&commit_lock);
            return;
        }
    }
    if (ncommits < MAX_COMMITS) {
        commits[ncommits].base = lo;
        commits[ncommits].end = hi;
        ncommits++;
    } else {
        static int warned;
        if (!warned++) fprintf(stderr, "[totk-android] commit table full (%u entries)\n", MAX_COMMITS);
    }
    pthread_mutex_unlock(&commit_lock);
}

void runtime_guest_uncommit(uintptr_t base, uintptr_t size) {
    if (!size) return;
    size_t ps = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t lo = base & ~(uintptr_t)(ps - 1);
    uintptr_t hi = (base + size + ps - 1) & ~(uintptr_t)(ps - 1);
    pthread_mutex_lock(&commit_lock);
    for (unsigned i = 0; i < ncommits; i++) {
        if (commits[i].base == lo && commits[i].end == hi) {
            commits[i] = commits[--ncommits];
            break;
        }
    }
    pthread_mutex_unlock(&commit_lock);
}

static int commit_range(uintptr_t addr, uintptr_t *base, uintptr_t *end) {
    int found = 0;
    pthread_mutex_lock(&commit_lock);
    for (unsigned i = 0; i < ncommits; i++)
        if (addr >= commits[i].base && addr < commits[i].end) {
            *base = commits[i].base; *end = commits[i].end; found = 1; break;
        }
    pthread_mutex_unlock(&commit_lock);
    return found;
}

static uintptr_t next_commit(uintptr_t addr) {
    uintptr_t next = 0;
    pthread_mutex_lock(&commit_lock);
    for (unsigned i = 0; i < ncommits; i++)
        if (commits[i].base > addr && (!next || commits[i].base < next)) next = commits[i].base;
    pthread_mutex_unlock(&commit_lock);
    return next;
}

/* lm (LogManager): the SDK's ILogService command 0 is OpenLogger(pid).  The
 * host returns an ILogger whose Log/SetDestination commands go to a silent
 * sink, so nn::lm::detail::ShimLibraryGlobal initializes. */
static void logger_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)rs;
    switch (rq->cmd) {
    case 0: /* Log(in buffer) */
    case 1: /* SetDestination(destination) */
        return;
    default:
        ipc_unimplemented(s, rq);
    }
}

static uint32_t lm_open_logger(IpcSession *s, IpcResponse *rs) {
    if (s->domain) {
        uint32_t id = ipc_domain_add_object(s, "ILogger", logger_handler, NULL, NULL);
        IPC_OUT_OBJECT(rs, id);
        return id;
    }
    uint32_t h = runtime_ipc_new_session("ILogger", logger_handler, NULL, NULL);
    rs->move[rs->nmove++] = h;
    return h;
}

static void lm_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: { /* OpenLogger(pid) -> ILogger */
        uint64_t pid = 0;
        if (rq->data_size >= 8) memcpy(&pid, rq->data, 8);
        fprintf(stderr, "[totk-ipc] lm OpenLogger pid=%#llx domain=%p\n",
                (unsigned long long)pid, (void *)s->domain);
        lm_open_logger(s, rs);
        return;
    }
    case 3: { /* OpenLogger on a domain object -> ILogger */
        fprintf(stderr, "[totk-ipc] lm OpenLogger cmd3 domain=%p\n", (void *)s->domain);
        lm_open_logger(s, rs);
        return;
    }
    default:
        ipc_unimplemented(s, rq);
    }
}
__attribute__((constructor)) static void register_android_services(void) {
    runtime_ipc_register("lm", lm_handler, NULL, NULL);
}

static uint8_t *reserve_fixed(uint64_t at, uint64_t size) {
    void *p = mmap((void *)(uintptr_t)at, (size_t)size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static void memory_init(void) {
    if (low_space) return;
    low_space = reserve_fixed(LOW_AT, LOW_SIZE);
    if (!low_space) {
        fprintf(stderr, "[totk-android] cannot reserve guest low region at %#llx\n", (unsigned long long)LOW_AT);
        runtime_fail("guest low region");
    }
    heap = low_space + HEAP_OFF;
    alias_size = ALIAS_MAX;
    while (alias_size >= (4ull << 30)) {
        alias_space = reserve_fixed(ALIAS_AT, alias_size);
        if (alias_space) break;
        alias_size >>= 1;
    }
    if (!alias_space) {
        /* Android may already use the preferred fixed window (or refuse such a
           large MAP_FIXED_NOREPLACE); the SDK only needs the address GetInfo
           reports, not 0x7000000000 specifically. */
        alias_size = ALIAS_MAX;
        while (alias_size >= (4ull << 30)) {
            void *p = mmap(NULL, (size_t)alias_size, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (p != MAP_FAILED) { alias_space = p; break; }
            alias_size >>= 1;
        }
        if (!alias_space) alias_size = 0;
    }
    fprintf(stderr, "[totk-android] guest layout: low %#llx+%#llx stack %#llx+%#llx heap %#llx alias %#llx+%#llx\n",
            (unsigned long long)(uintptr_t)low_space, (unsigned long long)LOW_SIZE,
            (unsigned long long)(uintptr_t)(low_space + STACK_OFF), (unsigned long long)STACK_SIZE,
            (unsigned long long)(uintptr_t)heap,
            (unsigned long long)(uintptr_t)alias_space, (unsigned long long)alias_size);
}

/* The Android rewrite resolves GetCurrentStackInfo through the guest TLR:
 * [TPIDRRO_EL0 + 0x1f8] is the ThreadType, whose +0x50/+0x58 are the stack
 * base and size.  The TLR must lie inside that range, as on Horizon where the
 * thread-local region is carved out of the thread's stack. */
void runtime_guest_stack_info(uint64_t *base, uint64_t *size) {
    memory_init();
    *base = (uintptr_t)low_space + STACK_OFF;
    *size = STACK_SIZE;
}

static uint64_t get_info(unsigned id) {
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

static void query_memory(uint64_t out, uint64_t addr) {
    uint64_t base = addr & ~0xfffull, size = 0x1000;
    uint32_t state = 0, perm = 0, attr = 0;
    for (unsigned i = 0; i < NMODULES; i++) {
        Module *m = &modules[i];
        uint64_t b = (uintptr_t)m->base;
        if (addr >= b && addr < b + m->key_off) {
            base = b; size = m->key_off; state = 5; perm = 3;
        }
    }
    for (unsigned i = 0; i < nextra; i++) {
        if (addr >= extra_ranges[i].base && addr < extra_ranges[i].end) {
            base = extra_ranges[i].base; size = extra_ranges[i].end - base;
            state = 5; perm = 3;
            break;
        }
    }
    if (low_space && addr >= (uintptr_t)low_space && addr < (uintptr_t)low_space + LOW_SIZE) {
        /* Only the heap and the guest stack are committed.  The rest of the
         * low region is address space reserved for AllocateAddressRegion /
         * AllocateMemoryPages; reporting the whole reservation as mapped makes
         * AddressSpaceAllocator fail to find a thread-stack guard page. */
        if (addr >= (uintptr_t)heap && addr < (uintptr_t)heap + heap_size) {
            base = (uintptr_t)heap; size = heap_size; state = 5; perm = 3;
        } else if (commit_range(addr, &base, &size)) {
            size -= base; state = 5; perm = 3;
        } else {
            /* Free address-space extent, at least up to the next commit or
             * reserved region.  The SDK asks for a thread stack, not a page. */
            uintptr_t lo = addr & ~0xfffull;
            uintptr_t limit = (uintptr_t)low_space + LOW_SIZE;
            uintptr_t stack_lo = (uintptr_t)low_space + STACK_OFF;
            uintptr_t stack_hi = stack_lo + STACK_SIZE;
            if (heap && lo < (uintptr_t)heap && (uintptr_t)heap < limit) limit = (uintptr_t)heap;
            if (lo < stack_lo && stack_lo < limit) limit = stack_lo;
            if (lo >= stack_lo && lo < stack_hi) {
                uintptr_t nc = next_commit(lo);
                if (nc && nc < limit) limit = nc;
                else if (stack_hi < limit) limit = stack_hi;
            }
            base = lo;
            size = limit > lo ? limit - lo : 0x1000;
            state = 0; perm = 0;
        }
    }
    if (alias_space && addr >= (uintptr_t)alias_space && addr < (uintptr_t)alias_space + alias_size) {
        base = (uintptr_t)alias_space; size = alias_size; state = 0; perm = 0;
    }
    if (!addr) { base = 0; size = (uintptr_t)low_space ? (uintptr_t)low_space : GUEST_LIMIT; state = 0; perm = 0; }
    if (out) {
        struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, pad; } info = {
            base, size, state, attr, perm, 0, 0, 0
        };
        memcpy((void *)(uintptr_t)out, &info, sizeof info);
    }
}

uint64_t runtime_svc(unsigned num, uint64_t *r, const char *module, uint64_t offset) {
    static int trace = -1;
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
