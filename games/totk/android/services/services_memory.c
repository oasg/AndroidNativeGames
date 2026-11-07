/* Module: memory. */
#include "services_internal.h"

/* Committed pages inside the low guest address space.  The low region itself
 * is only a reservation; QueryMemory must not claim all of it, otherwise the
 * SDK's AddressSpaceAllocator cannot find a thread-stack guard page.  One entry
 * per guest thread stack plus every MapMemory/MapPhysicalMemory range, so keep
 * room for more than the runtime's 256-thread stack limit. */
static struct { uintptr_t base, end; } commits[MAX_COMMITS];

static unsigned ncommits;

/* Host-backed guest ranges outside the low reservation (for example the nvdrv
 * nvmap window).  QueryMemory must report these as mapped, otherwise the
 * SDK's address allocator could hand the same address to the game. */
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
        int warned;
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
        int warned;
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

int commit_range(uintptr_t addr, uintptr_t *base, uintptr_t *end) {
    int found = 0;
    pthread_mutex_lock(&commit_lock);
    for (unsigned i = 0; i < ncommits; i++)
        if (addr >= commits[i].base && addr < commits[i].end) {
            *base = commits[i].base; *end = commits[i].end; found = 1; break;
        }
    pthread_mutex_unlock(&commit_lock);
    return found;
}

uintptr_t next_commit(uintptr_t addr) {
    uintptr_t next = 0;
    pthread_mutex_lock(&commit_lock);
    for (unsigned i = 0; i < ncommits; i++)
        if (commits[i].base > addr && (!next || commits[i].base < next)) next = commits[i].base;
    pthread_mutex_unlock(&commit_lock);
    return next;
}

uint8_t *reserve_fixed(uint64_t at, uint64_t size) {
    void *p = mmap((void *)(uintptr_t)at, (size_t)size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void memory_init(void) {
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

void query_memory(uint64_t out, uint64_t addr) {
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
