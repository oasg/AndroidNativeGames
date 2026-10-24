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

static uint8_t *low_space, *alias_space, *heap;
static size_t heap_size, alias_size;
static uint8_t user_exception_context[0x1000] __attribute__((aligned(0x1000)));

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
    if (!alias_space) alias_size = 0;
    fprintf(stderr, "[totk-android] guest layout: low %#llx+%#llx stack %#llx+%#llx heap %#llx alias %#llx+%#llx\n",
            (unsigned long long)(uintptr_t)low_space, (unsigned long long)LOW_SIZE,
            (unsigned long long)(uintptr_t)(low_space + STACK_OFF), (unsigned long long)STACK_SIZE,
            (unsigned long long)(uintptr_t)heap,
            (unsigned long long)(uintptr_t)alias_space, (unsigned long long)alias_size);
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
    uint32_t state = 3, perm = 5, attr = 0;
    for (unsigned i = 0; i < NMODULES; i++) {
        Module *m = &modules[i];
        uint64_t b = (uintptr_t)m->base;
        if (addr >= b && addr < b + m->key_off) {
            base = b; size = m->key_off; state = 5; perm = 3;
        }
    }
    if (low_space && addr >= (uintptr_t)low_space && addr < (uintptr_t)low_space + LOW_SIZE) {
        base = (uintptr_t)low_space; size = LOW_SIZE; state = 5; perm = 3;
        if (addr >= (uintptr_t)heap && addr < (uintptr_t)heap + heap_size) { base = (uintptr_t)heap; size = heap_size; }
        else if (addr >= (uintptr_t)low_space + STACK_OFF && addr < (uintptr_t)low_space + STACK_OFF + STACK_SIZE) {
            base = (uintptr_t)low_space + STACK_OFF; size = STACK_SIZE;
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
        case 0x04: case 0x05: r[0] = 0; return 0;   /* stack moves are not needed before nnMain */
        case 0x06: query_memory(r[0], r[2]); r[0] = 0; r[1] = 0; return 0;
        case 0x0b: { int64_t ns = (int64_t)r[0]; if (ns > 0) { struct timespec t = { ns / 1000000000, ns % 1000000000 }; nanosleep(&t, NULL); } r[0] = 0; return 0; }
        case 0x0c: case 0x0d: case 0x0e: case 0x0f: r[0] = 0; return 0;
        case 0x10: r[0] = 0; return 0;
        case 0x25: r[0] = 0; r[1] = (uint64_t)gettid(); return 0;
        case 0x26: fprintf(stderr, "[totk-android] guest Break reason=%#llx\n", (unsigned long long)r[0]); r[0] = 0; return 0;
        case 0x27: { size_t n = (size_t)r[1]; if (n > 65536) n = 65536; fwrite((void *)(uintptr_t)r[0], 1, n, stderr); r[0] = 0; return 0; }
        case 0x29: r[0] = 0; r[1] = get_info((unsigned)r[1]); return 0;
        case 0x2c: case 0x2d: r[0] = 0; return 0;
        default:
            fprintf(stderr, "[totk-android] unimplemented SVC %#x at %s+%#llx x0=%#llx x1=%#llx\n",
                    num, module, (unsigned long long)offset,
                    (unsigned long long)r[0], (unsigned long long)r[1]);
            r[0] = 0;
            return 0;
    }
}
