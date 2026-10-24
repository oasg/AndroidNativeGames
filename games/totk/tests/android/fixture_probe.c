/* Android/arm64 probe for the CPU rewrite fixture.
 *
 * Usage: fixture_probe <main.patched.bin>
 *
 * Maps the rewritten image, fills the Android key page (KCTX/KGATE) and a
 * synthetic TCB/GuestSlots, then calls one function per rewrite kind.  The
 * fixture functions and the host gate are pure assembly/C without libc/TLS;
 * TPIDR_EL0 is swapped only around fixture_call().
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fixture_config.h"

extern uint64_t fixture_call(uint64_t tcb, uint64_t fn, uint64_t arg);
extern void fixture_gate(void);

static volatile int g_mode;
static volatile uint64_t g_calls;

/* Called by fixture_gate with the gate return address and a saved register
 * frame.  It must not use libc or TLS. */
void fixture_dispatch(uint64_t ret, uint64_t *regs) {
    (void)ret;
    g_calls++;
    if (g_mode == 1) regs[0] = 0xCAFE;
    else if (g_mode == 2) regs[0] = 0x123456789ULL;
}

static void die(const char *what) {
    fprintf(stderr, "FAIL: %s (%s)\n", what, strerror(errno));
    exit(1);
}

static void check(const char *name, uint64_t got, uint64_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL %-12s got %#llx want %#llx\n", name,
                (unsigned long long)got, (unsigned long long)want);
        exit(1);
    }
    printf("PASS %s\n", name);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <main.patched.bin>\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) die("open fixture");
    struct stat st;
    if (fstat(fd, &st)) die("fstat fixture");
    size_t n = (size_t)st.st_size;
    uint8_t *base = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) die("mmap fixture");

    /* Key page: KCTX = offset of the thread's GuestSlots pointer from
     * TPIDR_EL0; KGATE = fixture gate address.  The other slots are zero. */
    uint64_t *key = (uint64_t *)(base + FIX_KEY_PAGE_OFF);
    for (int i = 0; i < 8; i++) key[i] = 0;
    key[FIX_KCTX_IDX] = FIX_KCTX_OFF;
    key[FIX_KGATE_IDX] = (uint64_t)(uintptr_t)&fixture_gate;
    key[FIX_KRES0_IDX] = 0xABCD00001234ULL;

    uint8_t *tcb = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint64_t *slots = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tcb == MAP_FAILED || slots == MAP_FAILED) die("mmap tcb/slots");
    memset(tcb, 0, 0x1000);
    memset(slots, 0, 0x1000);
    *(uint64_t *)(tcb + FIX_KCTX_OFF) = (uint64_t)(uintptr_t)slots;
    slots[FIX_KTLS_IDX] = 0x1111222233334444ULL;
    slots[FIX_KTLR_IDX] = 0x5555666677778888ULL;

    /* Code and patch area are executable; the key page stays readable. */
    if (mprotect(base, FIX_TEXT_END, PROT_READ | PROT_EXEC)) die("mprotect text");
    if (mprotect(base + FIX_PATCH_OFF, n - FIX_PATCH_OFF,
                 PROT_READ | PROT_EXEC)) die("mprotect patch");

    uint64_t tcb_addr = (uint64_t)(uintptr_t)tcb;

    g_mode = 1;
    check("svc", fixture_call(tcb_addr, (uint64_t)(uintptr_t)(base + FIX_FUNC_SVC), 0), 0xCAFE);

    check("tls_read", fixture_call(tcb_addr, (uint64_t)(uintptr_t)(base + FIX_FUNC_TLS_READ), 0),
          slots[FIX_KTLS_IDX]);

    g_mode = 0;
    check("tls_write", fixture_call(tcb_addr, (uint64_t)(uintptr_t)(base + FIX_FUNC_TLS_WRITE), 0),
          0x1234);
    check("tls_written", slots[FIX_KTLS_IDX], 0x1234);

    check("tlr_read", fixture_call(tcb_addr, (uint64_t)(uintptr_t)(base + FIX_FUNC_TLR_READ), 0),
          0x5555666677778888ULL);

    g_mode = 2;
    check("cnt", fixture_call(tcb_addr, (uint64_t)(uintptr_t)(base + FIX_FUNC_CNT), 0),
          0x123456789ULL);

    uint32_t *cell = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cell == MAP_FAILED) die("mmap exclusive cell");
    *cell = 0;
    uint64_t r = fixture_call(tcb_addr, (uint64_t)(uintptr_t)(base + FIX_FUNC_EXCL),
                              (uint64_t)(uintptr_t)cell);
    check("excl_return", r, 1);
    check("excl_cell", *cell, 1);

    check("param", fixture_call(tcb_addr, (uint64_t)(uintptr_t)(base + FIX_FUNC_PARAM), 0),
          0xABCD00001234ULL);

    uint64_t before = g_calls;
    g_mode = 3;
    check("obs", fixture_call(tcb_addr, (uint64_t)(uintptr_t)(base + FIX_FUNC_OBS), 0x42), 0x42);
    check("obs_gate", g_calls > before, 1);

    printf("PASS all (gate calls %llu)\n", (unsigned long long)g_calls);
    return 0;
}
