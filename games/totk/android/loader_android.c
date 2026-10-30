/* Minimal Android/arm64 loader for the rewritten TOTK modules.
 *
 * This is the first Android host milestone: map the verified images, fill the
 * Android key page, relocate imports, install the gate table, and run the
 * cross-module nn::os::GetSystemTick probe (main PLT -> sdk -> host timer gate).
 * The full HLE/service layer is intentionally not here yet.
 */
#include "runtime.h"
#include "ipc.h"
#include "sha256.h"
#include "gfx_backend_android.h"
#include "shader_cache_android.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define RUNTIME_TABLES_DEFINE 1
#include "runtime_tables.h"

pthread_key_t runtime_keys[8];
static uint64_t timer_calls, svc_calls;

typedef struct {
    uintptr_t host_stack_top;   /* offset 0: gate_android.S reads this */
    uintptr_t host_stack_base;  /* offset 8 */
    uint8_t *guest_tls;         /* guest TPIDR_EL0 value (zeroed 8 KiB) */
    uint8_t *guest_tlr;         /* guest TPIDRRO_EL0 value (zeroed 0x200) */
    uint8_t *dummy;
    uintptr_t guest_stack_top;  /* guest code entry stack (not the HLE stack) */
} HostContext;

typedef struct {
    uint64_t k18, ktls, ktlr, reserved[5];
} GuestSlots;

#define TLS_CANARY 0x544c53434f4eULL   /* "TLSOK" */

static int tls_probe_mode;
static __thread HostContext *g_host __attribute__((aligned(64)));
static __thread GuestSlots *g_slots __attribute__((aligned(64)));
uint64_t runtime_host_ctx_off;
uint64_t runtime_guest_slots_off;
static pthread_once_t ipc_key_once = PTHREAD_ONCE_INIT;
static void ipc_key_init(void) { pthread_key_create(&runtime_keys[2], NULL); }
void runtime_trace_guest(uint64_t fp, uint64_t lr) { (void)fp; (void)lr; }

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "[totk-android] FAIL: %s\n", message);
    exit(1);
}
static void need(bool c, const char *m) { if (!c) runtime_fail(m); }

static size_t page_size(void) {
    static size_t p;
    if (!p) p = (size_t)sysconf(_SC_PAGESIZE);
    return p;
}
static size_t page_up(size_t x) { size_t p = page_size(); return (x + p - 1) & ~(p - 1); }

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t runtime_now_ns(void) { return now_ns(); }

static uintptr_t read_tpidr_el0(void) {
    uintptr_t p;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(p));
    return p;
}

void runtime_guest_stack_info(uint64_t *base, uint64_t *size);
void runtime_guest_commit(uintptr_t base, uintptr_t size);
uint32_t runtime_thread_current_handle(void);
void runtime_thread_set_guest_handle(uint32_t handle) {
    if (g_host && g_host->dummy && handle) {
        memcpy(g_host->dummy + 0x1b0, &handle, sizeof handle);
        if (getenv("TOTK_ANDROID_TLS_TRACE"))
            fprintf(stderr, "[totk-android] ThreadType %p thread_id=%#x\n", g_host->dummy, handle);
    }
}

void runtime_thread_init(void) {
    if (g_host) return;
    HostContext *hc = calloc(1, sizeof *hc);
    GuestSlots *gs = calloc(1, sizeof *gs);
    need(hc && gs, "thread context allocation");

    /* The SDK's GetCurrentStackInfo (rewritten for Android) reads the thread
     * object from TLR+0x1f8 and requires the TLR itself to be inside the
     * reported [base, base+size).  On Horizon the TLS lives in the thread's
     * stack, so carve a per-thread TLS/ThreadType block out of the guest stack
     * instead of using unrelated heap allocations. */
    uint64_t stack_base = 0, stack_size = 0;
    runtime_guest_stack_info(&stack_base, &stack_size);
    static uint64_t stack_index;
    uint64_t idx = __atomic_fetch_add(&stack_index, 1, __ATOMIC_RELAXED);
    const uint64_t area = 1u << 20;
    need(stack_size >= (idx + 1) * area, "guest stack area");
    uintptr_t area_base = (uintptr_t)(stack_base + stack_size - (idx + 1) * area);
    need(mprotect((void *)area_base, area, PROT_READ | PROT_WRITE) == 0, "guest stack mapping");
    runtime_guest_commit(area_base, area);

    /* Put the TLS/TLR/ThreadType at the bottom of this thread's stack area.
     * nn::os discovers the committed area as the thread stack, so the TLR is
     * inside the range GetCurrentStackInfo later returns. */
    hc->guest_tls = (uint8_t *)area_base;       /* 8 KiB TLS, TPIDR_EL0 guest value */
    hc->guest_tlr = hc->guest_tls + 0x2000;     /* 0x200 ThreadLocalRegion */
    hc->dummy = hc->guest_tls + 0x3000;         /* ThreadType read from TLR+0x1f8 */
    uint64_t tb = area_base, ts = area;
    memcpy(hc->dummy + 0x50, &tb, 8);           /* ThreadType::stack_base */
    memcpy(hc->dummy + 0x58, &ts, 8);           /* ThreadType::stack_size */
    /* Horizon's ThreadLocalRegion keeps the guest TPIDR_EL0 value at +0x1f0
     * and the ThreadType pointer at +0x1f8.  The SDK's thread setup/teardown
     * does `msr tpidr_el0, [tpidrro_el0+0x1f0]`; leaving +0x1f0 zero clears
     * the guest TLS pointer and the next nn::gfx TLS store faults. */
    uint64_t guest_tls_value = (uintptr_t)hc->guest_tls;
    memcpy(hc->guest_tlr + 0x1f0, &guest_tls_value, 8);
    memcpy(hc->guest_tlr + 0x1f8, &hc->dummy, 8);
    uint32_t guest_handle = runtime_thread_current_handle();
    if (guest_handle) memcpy(hc->dummy + 0x1b0, &guest_handle, 4);   /* ThreadType::thread_id */
    if (getenv("TOTK_ANDROID_TLS_TRACE"))
        fprintf(stderr, "[totk-android] thread init handle=%#x dummy=%p\n", guest_handle, hc->dummy);
    hc->guest_stack_top = area_base + area - 16;
    if (getenv("TOTK_ANDROID_TLS_TRACE") || getenv("TOTK_ANDROID_CTOR_PROBE") || tls_probe_mode)
        fprintf(stderr, "[totk-android] thread tls tlr=%p dummy=%p stack=%#llx+%#llx sp=%#llx\n",
                hc->guest_tlr, hc->dummy, (unsigned long long)area_base,
                (unsigned long long)area, (unsigned long long)hc->guest_stack_top);
    gs->ktls = (uintptr_t)hc->guest_tls;
    gs->ktlr = (uintptr_t)hc->guest_tlr;
    gs->reserved[0] = TLS_CANARY;   /* crash-dump canary: "TLSOK" */
    pthread_once(&ipc_key_once, ipc_key_init);
    pthread_setspecific(runtime_keys[2], hc->guest_tlr);
    const size_t hs = 1u << 20;
    uint8_t *stack = mmap(NULL, hs, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    need(stack != MAP_FAILED, "host stack mmap");
    need(mprotect(stack, page_size(), PROT_NONE) == 0, "host stack guard");
    hc->host_stack_base = (uintptr_t)stack;
    hc->host_stack_top = (uintptr_t)stack + hs - 64;
    g_host = hc;
    g_slots = gs;
    uintptr_t tp = read_tpidr_el0();
    fprintf(stderr, "[totk-android] tls tp=%#llx g_host=%#llx g_slots=%#llx\n",
            (unsigned long long)tp, (unsigned long long)(uintptr_t)&g_host,
            (unsigned long long)(uintptr_t)&g_slots);
    uint64_t ho = (uintptr_t)&g_host - tp;
    uint64_t so = (uintptr_t)&g_slots - tp;
    if (!runtime_host_ctx_off) runtime_host_ctx_off = ho;
    else need(ho == runtime_host_ctx_off, "per-thread TLS offset changed (host)");
    if (!runtime_guest_slots_off) runtime_guest_slots_off = so;
    else need(so == runtime_guest_slots_off, "per-thread TLS offset changed (guest slots)");
}

uintptr_t runtime_guest_stack_top(void) { return g_host ? g_host->guest_stack_top : 0; }

uint64_t runtime_guest_call(void *fn, uintptr_t stack_top,
                            uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4);

void runtime_hle_stack_set(uintptr_t top) { (void)top; }
void runtime_hle_stack_init(uint64_t off) { (void)off; }

static unsigned hash_name(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
static void make_hash(Module *m) {
    unsigned cap = 16;
    while (cap < m->nsym * 2) cap <<= 1;
    m->hash = calloc(cap, sizeof(unsigned));
    need(m->hash != NULL, "symbol hash allocation");
    m->hash_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) if (m->symbols[i].defined) {
        const char *s = m->symbols[i].name;
        unsigned p = hash_name(s) & m->hash_mask;
        while (m->hash[p] && strcmp(s, m->symbols[m->hash[p]].name)) p = (p + 1) & m->hash_mask;
        if (!m->hash[p]) m->hash[p] = i;
    }
}
static void *module_symbol(Module *m, const char *name) {
    if (!m->hash) return NULL;
    unsigned p = hash_name(name) & m->hash_mask;
    while (m->hash[p]) {
        const Symbol *s = &m->symbols[m->hash[p]];
        if (!strcmp(name, s->name)) return s->value < m->key_off ? m->base + s->value : NULL;
        p = (p + 1) & m->hash_mask;
    }
    return NULL;
}
void *runtime_symbol(const char *name) {
    for (unsigned i = 0; i < NMODULES; i++) {
        void *p = module_symbol(&modules[i], name);
        if (p) return p;
    }
    return NULL;
}
uint8_t *runtime_module_base(const char *name) {
    for (unsigned i = 0; i < NMODULES; i++) if (!strcmp(modules[i].name, name)) return modules[i].base;
    return NULL;
}
uint64_t runtime_image_bytes(void) {
    uint64_t total = 0;
    for (unsigned i = 0; i < NMODULES; i++) total += modules[i].key_off;
    return page_up(total);
}

static void map_module(Module *m, const char *dir) {
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m->name);
    need(n > 0 && (size_t)n < sizeof path, "module path too long");
    fprintf(stderr, "[totk-android] map %s from %s\n", m->name, path);
    int fd = open(path, O_RDONLY);
    need(fd >= 0, "open patched module");
    struct stat st;
    need(fstat(fd, &st) == 0 && (size_t)st.st_size == m->file_size, "patched module size");
    /* Map the whole image executable first; data/key page are made writable
     * below.  Mapping it writable first then adding PROT_EXEC is denied by
     * Android's execmod policy (verified on an arm64 device). */
    void *base = mmap(NULL, m->file_size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    close(fd);
    need(base != MAP_FAILED, "map patched module");
    fprintf(stderr, "[totk-android] mmap %s -> %p\n", m->name, base);
    m->base = base;
    m->size = page_up(m->file_size);
    char hex[65];
    sha256_hex(base, m->file_size, hex);
    fprintf(stderr, "[totk-android] sha %s -> %s\n", m->name, hex);
    need(!strcmp(hex, m->digest), "image SHA256 differs from verified rewrite");
    need(m->key_off % page_size() == 0 && m->key_off + 0x4000 <= m->patch_off &&
         m->patch_off <= m->file_size, "key/patch layout");
    /* The image was mapped executable first.  Make only the data and key
     * page writable; text and the patch area stay R-X. */
    if (mprotect(m->base + m->data_off, m->key_off - m->data_off, PROT_READ | PROT_WRITE)) {
        fprintf(stderr, "[totk-android] data RW %s: %s\n", m->name, strerror(errno));
        runtime_fail("data RW");
    }
    if (mprotect(m->base + m->key_off, 0x4000, PROT_READ | PROT_WRITE)) {
        fprintf(stderr, "[totk-android] key RW %s: %s\n", m->name, strerror(errno));
        runtime_fail("key page RW");
    }
    uint64_t *key = (void *)(m->base + m->key_off);
    for (int i = 0; i < 8; i++) key[i] = 0;
    key[0] = runtime_guest_slots_off;       /* KCTX */
    key[3] = (uintptr_t)&runtime_gate;      /* KGATE */
    make_hash(m);
    fprintf(stderr, "[totk-android] mapped %s at %p size=%zu symbols=%u gates=%u\n",
            m->name, m->base, m->file_size, m->nsym, m->ngate);
}

void *runtime_override(const char *name);
void *runtime_observer(const char *name);
void *runtime_hle_stub(const char *name);

static void relocate(Module *m) {
    for (unsigned i = 0; i < m->nreloc; i++) {
        const Reloc *r = &m->relocs[i];
        need(r->where >= m->data_off && r->where + 8 <= m->key_off && r->symbol < m->nsym,
             "relocation bounds");
        uintptr_t v;
        if (r->kind == 1027) {
            need(r->addend >= 0 && (uint64_t)r->addend < m->key_off, "relative addend");
            v = (uintptr_t)m->base + r->addend;
            m->relative++;
        } else {
            need(r->kind == 257 || r->kind == 1025 || r->kind == 1026, "relocation kind");
            const Symbol *s = &m->symbols[r->symbol];
            void *p = (r->kind == 1026 || !strcmp(s->name, "nnMain")) ? runtime_observer(s->name) : NULL;
            if (!p) p = runtime_override(s->name);
            if (p) p = runtime_hle_wrap_named(p, s->name);   /* host replacements run on the host stack */
            if (!p && s->defined) p = s->value < m->key_off ? m->base + s->value : NULL;
            if (!p) p = runtime_symbol(s->name);
            if (!p && s->binding != 2) {
                p = runtime_hle_stub(s->name);
                if (p) p = runtime_hle_wrap_named(p, s->name);
            }
            if (!p && s->binding != 2) {
                fprintf(stderr, "[totk-android] unresolved %s in %s\n", s->name, m->name);
                runtime_fail("strong import unresolved");
            }
            if (!p) m->weak++;
            if (p && !s->defined) m->cross++;
            v = (uintptr_t)p + (r->kind == 1026 ? 0 : r->addend);
            m->symbolic++;
        }
        memcpy(m->base + r->where, &v, 8);
    }
}

static void seal(Module *m) {
    size_t p = page_size();
    size_t text = page_up(m->text_end);
    size_t ro_end = m->data_off & ~(p - 1);
    need(text <= ro_end, "text/data overlap");
    /* text and the patch area were mapped R-X; do not add PROT_EXEC here. */
    (void)text;
    if (ro_end > page_up(m->text_end) && mprotect(m->base + page_up(m->text_end), ro_end - page_up(m->text_end), PROT_READ)) {
        fprintf(stderr, "[totk-android] rodata R %s: %s\n", m->name, strerror(errno));
        runtime_fail("rodata R");
    }
    if (mprotect(m->base + m->data_off, m->key_off - m->data_off, PROT_READ | PROT_WRITE)) {
        fprintf(stderr, "[totk-android] data RW %s: %s\n", m->name, strerror(errno));
        runtime_fail("data RW");
    }
    if (mprotect(m->base + m->key_off, 0x4000, PROT_READ)) {
        fprintf(stderr, "[totk-android] key R %s: %s\n", m->name, strerror(errno));
        runtime_fail("key page R");
    }
    if (m->file_size > m->patch_off)
        __builtin___clear_cache((char *)m->base + m->patch_off, (char *)m->base + m->file_size);
    __builtin___clear_cache((char *)m->base, (char *)m->base + page_up(m->text_end));
}

uint64_t runtime_svc(unsigned num, uint64_t *r, const char *module, uint64_t offset);

void runtime_dispatch(uint64_t ret, uint64_t *regs) {
    for (unsigned i = 0; i < NMODULES; i++) {
        const Module *m = &modules[i];
        for (unsigned j = 0; j < m->ngate; j++) {
            const Gate *g = &m->gates[j];
            if ((uintptr_t)m->base + g->ret != ret) continue;
            if (g->timer) {
                need(g->arg < 31 && g->arg != 18, "timer target register");
                regs[g->arg] = (uint64_t)((__uint128_t)now_ns() * 12 / 625);
                __atomic_fetch_add(&timer_calls, 1, __ATOMIC_RELAXED);
                return;
            }
            __atomic_fetch_add(&svc_calls, 1, __ATOMIC_RELAXED);
            runtime_svc(g->arg, regs, m->name, g->original);
            return;
        }
    }
    fprintf(stderr, "[totk-android] unknown gate return %#llx\n", (unsigned long long)ret);
    runtime_fail("unknown gate return address");
}

static void *probe_thread(void *unused) {
    (void)unused;
    runtime_thread_init();
    typedef uint64_t (*Fn)(void);
    Fn fn = (Fn)(void *)(modules[0].base + PROBE_PLT);
    uint64_t previous = 0;
    for (unsigned i = 0; i < 1000; i++) {
        uint64_t current = fn();
        need(current >= previous && current > 0, "cross-module clock result");
        previous = current;
    }
    return NULL;
}

/* Guest frame names for the thread dump (threads.c calls this weak hook).
 * Prefer the nearest defined symbol in the mapped module, so a dump names
 * the load/job function instead of only main+0x.... */
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
static void describe_address(uint64_t addr, char *out, size_t cap) {
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
static int crash_read(uint64_t addr, void *out, size_t n) {
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

const char *runtime_nvn_last_api(void);
const char *runtime_nvn_last_proc(void);

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *u = ctx;
    uint64_t pc = u->uc_mcontext.pc;
    uint64_t lr = u->uc_mcontext.regs[30];
    uint64_t sp = u->uc_mcontext.sp;
    uint64_t fp = u->uc_mcontext.regs[29];
    char pcd[128], lrd[128];
    describe_address(pc, pcd, sizeof pcd);
    describe_address(lr, lrd, sizeof lrd);
    fprintf(stderr, "[crash] sig=%d pc=%s lr=%s fault=%p sp=%#llx x0=%#llx x1=%#llx x8=%#llx\n",
            sig, pcd, lrd, si ? si->si_addr : NULL, (unsigned long long)sp,
            (unsigned long long)u->uc_mcontext.regs[0], (unsigned long long)u->uc_mcontext.regs[1],
            (unsigned long long)u->uc_mcontext.regs[8]);

    if (getenv("TOTK_ANDROID_CRASH_DUMP")) {
        const unsigned long long *rr = (const unsigned long long *)u->uc_mcontext.regs;
        /* The relocated `mrs tpidr_el0` reads GuestSlots->ktls.  Keep the
         * host TP and slot values in the dump so a lost guest TP is visible. */
        uint64_t host_tp = read_tpidr_el0();
        uint64_t slots_at = 0, ktls = 0, ktlr = 0, canary = 0;
        int slots_ok = 0, slots_read = 0;
        if (runtime_guest_slots_off) {
            slots_ok = crash_read((uintptr_t)(host_tp + runtime_guest_slots_off), &slots_at, 8);
            if (slots_at) {
                slots_read = crash_read((uintptr_t)(slots_at + 8), &ktls, 8);
                crash_read((uintptr_t)(slots_at + 16), &ktlr, 8);
                crash_read((uintptr_t)(slots_at + 0x18), &canary, 8);
            }
        }
        fprintf(stderr, "[crash] tls tp=%#llx slots=%#llx slots_ok=%d read=%d ktls=%#llx"
                " ktlr=%#llx canary=%#llx\n", (unsigned long long)host_tp,
                (unsigned long long)slots_at, slots_ok, slots_read,
                (unsigned long long)ktls, (unsigned long long)ktlr,
                (unsigned long long)canary);
        if (rr[1] && !(rr[1] & 7)) {
            uint64_t s[2] = {0};
            if (crash_read(rr[1], s, sizeof s)) {
                char text[129] = {0};
                uint64_t len = s[1] & 0xffffffffu;
                if (len >= sizeof text) len = sizeof text - 1;
                if (s[0] && len && crash_read(s[0], text, (size_t)len))
                    fprintf(stderr, "[crash] x1 string ptr=%#llx len=%llu '%.*s'\n",
                            (unsigned long long)s[0],
                            (unsigned long long)(s[1] & 0xffffffffu),
                            (int)len, text);
            }
        }
        for (unsigned i = 0; i < NMODULES; i++) {
            uint64_t key0 = 0;
            if (crash_read((uintptr_t)(modules[i].base + modules[i].key_off), &key0, 8))
                fprintf(stderr, "[crash] key[0] %s=%#llx\n", modules[i].name,
                        (unsigned long long)key0);
        }
        for (unsigned i = 0; i <= 28; i++) {
            char d[128];
            if (!rr[i]) continue;
            describe_address(rr[i], d, sizeof d);
            if (d[0] == 'm' || d[0] == 's')   /* module-relative address */
                fprintf(stderr, "[crash] x%u=%s\n", i, d);
        }
        /* With a zero frame pointer (tail call / indirect branch) walk the
         * raw stack instead: any word that points into a module is a
         * likely return address. */
        if (!fp) {
            uint64_t sv[96] = {0};
            if (crash_read(sp, sv, sizeof sv)) {
                for (unsigned i = 0; i < 96; i += 4)
                    fprintf(stderr, "[crash] rawstack+%#x: %#llx %#llx %#llx %#llx\n",
                            i * 8, (unsigned long long)sv[i], (unsigned long long)sv[i+1],
                            (unsigned long long)sv[i+2], (unsigned long long)sv[i+3]);
                for (unsigned i = 0; i < 96; i++) {
                    char d[128];
                    if (!sv[i]) continue;
                    describe_address(sv[i], d, sizeof d);
                    if (d[0] == 'm' || d[0] == 's')
                        fprintf(stderr, "[crash] stack+%#x: %s\n", i * 8, d);
                }
                /* Recover a frame chain when the faulting x29 is zero: scan
                 * for {candidate_fp, return_address} records. */
                for (uintptr_t a = sp; a < sp + 0x400; a += 8) {
                    uint64_t pair[2] = {0};
                    if (!crash_read(a, pair, sizeof pair)) break;
                    char d[128]; describe_address(pair[1], d, sizeof d);
                    if ((d[0] == 'm' || d[0] == 's') &&
                        pair[0] >= sp && pair[0] < sp + 0x100000)
                        fprintf(stderr, "[crash] frame-scan %#llx: fp=%#llx lr=%s\n",
                                (unsigned long long)a,
                                (unsigned long long)pair[0], d);
                }
            }
        }
        {
            uint64_t q[8] = {0};
            if (crash_read(sp, q, sizeof q))
                fprintf(stderr, "[crash] sp object: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
                        (unsigned long long)q[0], (unsigned long long)q[1],
                        (unsigned long long)q[2], (unsigned long long)q[3],
                        (unsigned long long)q[4], (unsigned long long)q[5],
                        (unsigned long long)q[6], (unsigned long long)q[7]);
        }
        fprintf(stderr, "[crash] regs x0=%#llx x1=%#llx x2=%#llx x3=%#llx x4=%#llx x5=%#llx x6=%#llx x7=%#llx\n",
                (unsigned long long)rr[0], (unsigned long long)rr[1], (unsigned long long)rr[2],
                (unsigned long long)rr[3], (unsigned long long)rr[4], (unsigned long long)rr[5],
                (unsigned long long)rr[6], (unsigned long long)rr[7]);
        fprintf(stderr, "[crash] regs x19=%#llx x20=%#llx x21=%#llx x22=%#llx x23=%#llx x24=%#llx x25=%#llx x26=%#llx x27=%#llx x28=%#llx\n",
                (unsigned long long)rr[19], (unsigned long long)rr[20], (unsigned long long)rr[21],
                (unsigned long long)rr[22], (unsigned long long)rr[23], (unsigned long long)rr[24],
                (unsigned long long)rr[25], (unsigned long long)rr[26], (unsigned long long)rr[27],
                (unsigned long long)rr[28]);
        for (unsigned i = 0; i < 4; i++) {
            uint64_t obj = rr[i];
            if (i == 1 && obj && !(obj & 7)) {
                uint64_t q[16] = {0};
                if (crash_read(obj, q, sizeof q)) {
                    fprintf(stderr, "[crash] x1 object %#llx: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
                            (unsigned long long)obj,
                            (unsigned long long)q[0], (unsigned long long)q[1],
                            (unsigned long long)q[2], (unsigned long long)q[3],
                            (unsigned long long)q[4], (unsigned long long)q[5],
                            (unsigned long long)q[6], (unsigned long long)q[7]);
                    uint64_t fallback = q[6];
                    if (fallback && !(fallback & 7)) {
                        uint64_t fq[16] = {0};
                        if (crash_read(fallback, fq, sizeof fq))
                            fprintf(stderr, "[crash] x1 fallback %#llx: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
                                    (unsigned long long)fallback,
                                    (unsigned long long)fq[0], (unsigned long long)fq[1],
                                    (unsigned long long)fq[2], (unsigned long long)fq[3],
                                    (unsigned long long)fq[4], (unsigned long long)fq[5],
                                    (unsigned long long)fq[6], (unsigned long long)fq[7]);
                    }
                    uint64_t vtable = q[0], fn = 0;
                    if (vtable && !(vtable & 7) && crash_read(vtable + 0x10, &fn, 8)) {
                        char d[128]; describe_address(fn, d, sizeof d);
                        fprintf(stderr, "[crash] x1 vtable=%#llx +0x10 -> %s\n",
                                (unsigned long long)vtable, d);
                    }
                    if (fallback && !(fallback & 7)) {
                        uint64_t sub = 0;
                        if (crash_read(fallback + 0x48, &sub, 8) && sub && !(sub & 7)) {
                            uint64_t sq[16] = {0};
                            if (crash_read(sub, sq, sizeof sq))
                                fprintf(stderr, "[crash] x1 fallback+0x48 %#llx: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
                                        (unsigned long long)sub,
                                        (unsigned long long)sq[0], (unsigned long long)sq[1],
                                        (unsigned long long)sq[2], (unsigned long long)sq[3],
                                        (unsigned long long)sq[4], (unsigned long long)sq[5],
                                        (unsigned long long)sq[6], (unsigned long long)sq[7]);
                        }
                        uint64_t subv = 0, subfn = 0;
                        if (crash_read(sub, &subv, 8) && subv && !(subv & 7) &&
                            crash_read(subv + 0x38, &subfn, 8)) {
                            char d[128]; describe_address(subfn, d, sizeof d);
                            fprintf(stderr, "[crash] sub vtable=%#llx +0x38 -> %s\n",
                                    (unsigned long long)subv, d);
                        }
                        uint64_t fv = 0, ffn = 0;
                        if (crash_read(fallback, &fv, 8) && fv && !(fv & 7) &&
                            crash_read(fv + 0x10, &ffn, 8)) {
                            char d[128]; describe_address(ffn, d, sizeof d);
                            fprintf(stderr, "[crash] x1 fallback vtable=%#llx +0x10 -> %s\n",
                                    (unsigned long long)fv, d);
                        }
                    }
                }
            }
            uint64_t q[9] = {0};
            if (!obj || (obj & 7) || !crash_read(obj, q, sizeof q)) continue;
            fprintf(stderr, "[crash] [x%u]: +0=%#llx +8=%#llx +0x20=%#llx +0x38=%#llx +0x40=%#llx\n",
                    i, (unsigned long long)q[0], (unsigned long long)q[1],
                    (unsigned long long)q[4], (unsigned long long)q[7], (unsigned long long)q[8]);
        }
        if (rr[22]) {
            uint64_t v[12] = {0};
            if (crash_read(rr[22], v, sizeof v))
                fprintf(stderr, "[crash] [x22] +0=%#llx +8=%#llx +0x10=%#llx +0x18=%#llx +0x20=%#llx\n",
                        (unsigned long long)v[0], (unsigned long long)v[1],
                        (unsigned long long)v[2], (unsigned long long)v[3],
                        (unsigned long long)v[4]);
        }
        /* The two callee-saved pointers around the nn::gfx allocation path:
         * x20 is the argument struct, x21 the object that owns the allocation.
         * Follow one level so a null heap/allocator is visible without a
         * debugger. */
        for (unsigned i = 19; i <= 22; i++) {
            uint64_t obj = rr[i], q[32] = {0};
            if (!obj || (obj & 7) || !crash_read(obj, q, sizeof q)) continue;
            fprintf(stderr, "[crash] [x%u]=%#llx: +0=%#llx +8=%#llx +0x10=%#llx +0x18=%#llx"
                    " +0x20=%#llx +0x28=%#llx +0x30=%#llx +0x38=%#llx\n",
                    i, (unsigned long long)obj,
                    (unsigned long long)q[0], (unsigned long long)q[1],
                    (unsigned long long)q[2], (unsigned long long)q[3],
                    (unsigned long long)q[4], (unsigned long long)q[5],
                    (unsigned long long)q[6], (unsigned long long)q[7]);
            for (unsigned j = 0; j < 8; j++) {
                uint64_t p = q[j], v = 0;
                if (!p || (p & 7) || !crash_read(p, &v, 8)) continue;
                fprintf(stderr, "[crash]   [x%u+%#x] -> %#llx: [0]=%#llx\n", i, j * 8,
                        (unsigned long long)p, (unsigned long long)v);
            }
        }
        uint8_t *mb = runtime_module_base("main");
        if (mb) {
            static const struct { uint32_t off; const char *name; } globals[] = {
                {0x4557768, "alloc-global"}, {0x4557798, "current-thread"},
                {0x4557728, "resource-table"}, {0x4557730, "resource-2"},
                {0x4563010, "gfx-mgr"}, {0x4563018, "gfx-mgr-2"},
                {0x45586f0, "gfx-device-slot"},
                {0x4540aa8, "nvn-proc-aa8"}, {0x4540ab0, "nvn-proc-ab0"},
                {0x453efa0, "got-unlock-mutex"}, {0x453ef98, "got-lock-mutex"},
                {0x453ef88, "got-memset"}, {0x453efb8, "got-sqrtf"},
                {0x45409a0, "nvn-proc-9a0"}, {0x45409a8, "nvn-proc-9a8"},
                {0x45407a0, "nvn-proc-7a0"}, {0x45409f8, "nvn-proc-9f8"},
            };
            for (unsigned i = 0; i < sizeof globals / sizeof *globals; i++) {
                uint64_t p = 0, v = 0;
                if (!crash_read((uintptr_t)(mb + globals[i].off), &p, 8)) continue;
                if (p && !(p & 7) && crash_read(p, &v, 8)) {
                    fprintf(stderr, "[crash] %s @%#x: %#llx -> [0]=%#llx\n",
                            globals[i].name, globals[i].off,
                            (unsigned long long)p, (unsigned long long)v);
                } else {
                    fprintf(stderr, "[crash] %s @%#x: %#llx (null)\n",
                            globals[i].name, globals[i].off, (unsigned long long)p);
                }
                if (!strcmp(globals[i].name, "resource-table") && p && !(p & 7)) {
                    uint64_t t[16] = {0};
                    if (crash_read(p, t, sizeof t))
                        for (unsigned j = 0; j < 16; j += 2)
                            fprintf(stderr, "[crash]   resource-table+%#x: %#llx %#llx\n",
                                    j * 8, (unsigned long long)t[j],
                                    (unsigned long long)t[j + 1]);
                }
            }
            /* nn::gfx device/context registry: [main+0x4542f08] -> manager,
             * manager+0x18 -> list head; nodes link at +0x598 and carry the
             * thread id at +0x5e8.  This is the lookup that fails at
             * main+0x794d30. */
            {
                uint64_t mgr = 0, head = 0;
                if (crash_read((uintptr_t)(mb + 0x4542f08), &mgr, 8) && mgr && !(mgr & 7)) {
                    fprintf(stderr, "[crash] gfx-registry mgr=%#llx\n", (unsigned long long)mgr);
                    if (crash_read(mgr, &head, 8))
                        fprintf(stderr, "[crash] gfx-registry mgr[0]=%#llx\n", (unsigned long long)head);
                    {
                        uint64_t q[8] = {0};
                        if (crash_read(mgr + 0x18, q, sizeof q))
                            fprintf(stderr, "[crash] gfx-registry mgr+0x18..0x50: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
                                    (unsigned long long)q[0], (unsigned long long)q[1],
                                    (unsigned long long)q[2], (unsigned long long)q[3],
                                    (unsigned long long)q[4], (unsigned long long)q[5],
                                    (unsigned long long)q[6], (unsigned long long)q[7]);
                    }
                    if (crash_read(mgr + 0x18, &head, 8) && head && !(head & 7)) {
                        uint64_t hv[4] = {0};
                        if (crash_read(head, hv, sizeof hv))
                            fprintf(stderr, "[crash] gfx-registry head=%#llx: %#llx %#llx %#llx %#llx\n",
                                    (unsigned long long)head, (unsigned long long)hv[0],
                                    (unsigned long long)hv[1], (unsigned long long)hv[2],
                                    (unsigned long long)hv[3]);
                        uint64_t node = hv[0];
                        if (crash_read(head, &node, 8)) {
                            for (unsigned n = 0; n < 8 && node && !(node & 7); n++) {
                                uint64_t key = 0, next = 0;
                                crash_read(node + 0x5e8, &key, 8);
                                crash_read(node + 0x598, &next, 8);
                                fprintf(stderr, "[crash] gfx-registry node[%u]=%#llx key+0x5e8=%#llx next=%#llx\n",
                                        n, (unsigned long long)node, (unsigned long long)key,
                                        (unsigned long long)next);
                                node = next;
                            }
                        }
                    }
                }
            }
            uint8_t *sb = runtime_module_base("sdk");
            if (sb) {
                static const uint32_t sdk_slots[] = {0xd0308, 0xd0310, 0xd0318, 0xd0320, 0xd0328, 0xd0330,
                                                     0xddc000, 0xddc008, 0xddc010, 0xddc018, 0xddc020, 0xddc028};
                for (unsigned i = 0; i < sizeof sdk_slots / sizeof *sdk_slots; i++) {
                    uint64_t v = 0;
                    if (crash_read((uintptr_t)(sb + sdk_slots[i]), &v, 8))
                        fprintf(stderr, "[crash] sdk-got+%#x: %#llx\n",
                                sdk_slots[i], (unsigned long long)v);
                }
            }
            uint64_t g[16] = {0};
            if (crash_read((uintptr_t)(mb + 0x463c000), g, sizeof g))
                for (unsigned i = 0; i < 16; i += 2)
                    fprintf(stderr, "[crash] sead+%#x: %#llx %#llx\n", i * 8,
                            (unsigned long long)g[i], (unsigned long long)g[i + 1]);
        }
    }

    /* Guest frame-pointer chain: at x29 the pair is {caller fp, return address}. */
    uint64_t chain_fp = fp;
    uint64_t caller_heap = 0;
    if (chain_fp && !(chain_fp & 7)) {
        for (unsigned n = 0; n < 16 && chain_fp; n++) {
            uint64_t pair[2];
            if (!crash_read(chain_fp, pair, sizeof pair)) break;
            char fd[128];
            describe_address(pair[1], fd, sizeof fd);
            fprintf(stderr, "[crash] frame %u: fp=%#llx lr=%s\n",
                    n, (unsigned long long)chain_fp, fd);
            if (getenv("TOTK_ANDROID_CRASH_DUMP") && n == 2) {
                /* 0x811784: x29 = sp+0x10, saves x23 (the sead heap the
                 * allocation uses) at fp+0x38; the size requested from that
                 * heap is kept at fp-8. */
                uint64_t v[8] = {0};
                if (crash_read(chain_fp - 8, v, 8))
                    fprintf(stderr, "[crash] 0x811784 requested heap size=%#llx\n",
                            (unsigned long long)v[0]);
                if (crash_read(chain_fp + 0x38, v, 8)) {
                    fprintf(stderr, "[crash] 0x811784 x23=%#llx\n", (unsigned long long)v[0]);
                    caller_heap = v[0];
                }
            }
            if (getenv("TOTK_ANDROID_CRASH_DUMP") && n == 3) {
                /* 0x810c68: x29 = sp+0x10.  sp+0x108/0x110 is the per-resource
                 * required-size vector; sp+0x118/0x120 is the object vector. */
                uint64_t base = chain_fp - 0xa9930, v[4] = {0};
                uint32_t count = 0, ocount = 0;
                uint64_t data = 0, odata = 0;
                if (crash_read(base + 0x108, &count, 4)) { }
                if (crash_read(base + 0x110, &data, 8)) { }
                if (crash_read(base + 0x118, &ocount, 4)) { }
                if (crash_read(base + 0x120, &odata, 8)) { }
                fprintf(stderr, "[crash] 0x810c68 size-vector count=%u data=%#llx"
                        " object-vector count=%u data=%#llx\n",
                        count, (unsigned long long)data, ocount, (unsigned long long)odata);
                for (uint32_t i = 0; i < count && i < 8 && data; i++) {
                    uint64_t s = 0, o = 0;
                    if (!crash_read(data + i * 8, &s, 8)) break;
                    if (odata) crash_read(odata + i * 8, &o, 8);
                    fprintf(stderr, "[crash] 0x810c68 resource[%u]: size=%#llx object=%#llx\n",
                            i, (unsigned long long)s, (unsigned long long)o);
                }
                if (crash_read(base + 0x98, v, 8))
                    fprintf(stderr, "[crash] 0x810c68 x21/resource=%#llx\n",
                            (unsigned long long)v[0]);
            }
            uint64_t next = pair[0];
            if (!next || (next & 7) || next <= chain_fp || next - chain_fp > (64u << 20)) break;
            chain_fp = next;
        }
    } else {
        char fd[128];
        describe_address(lr, fd, sizeof fd);
        fprintf(stderr, "[crash] frame 0: fp=%#llx lr=%s\n", (unsigned long long)fp, fd);
    }

    if (getenv("TOTK_ANDROID_CRASH_DUMP") && caller_heap && !(caller_heap & 7)) {
        uint64_t v[32] = {0};
        if (crash_read(caller_heap, v, sizeof v))
            for (unsigned i = 0; i < 32; i += 2)
                fprintf(stderr, "[crash] heap %#llx+%#x: %#llx %#llx\n",
                        (unsigned long long)caller_heap, i * 8,
                        (unsigned long long)v[i], (unsigned long long)v[i + 1]);
    }

    char hle[256];
    if (runtime_hle_last_call(hle, sizeof hle)) fprintf(stderr, "[crash] last HLE: %s\n", hle);
    else fprintf(stderr, "[crash] last HLE: none\n");
    fprintf(stderr, "[crash] last NVN api: %s (last proc lookup: %s)\n",
            runtime_nvn_last_api(), runtime_nvn_last_proc());

    RuntimeIpcRecord ipc;
    if (runtime_ipc_last(&ipc) && ipc.valid)
        fprintf(stderr, "[crash] last IPC: service=%s command=%u object=%u type=%u data=%u result=%u\n",
                ipc.service, ipc.command, ipc.object, ipc.type, ipc.data_size, ipc.result);
    else fprintf(stderr, "[crash] last IPC: none\n");

    fprintf(stderr, "[crash] guest thread handle=%#x\n", runtime_thread_current_handle());
    runtime_hle_trace_report();   /* no allocation/stdio locking beyond this handler */
    _exit(128 + sig);
}

static uint64_t host_add(uint64_t a, uint64_t b) { return a + b; }

static uint64_t guest_call1(void *fn, uint64_t a0) {
    return runtime_guest_call(fn, runtime_guest_stack_top(), a0, 0, 0, 0, 0);
}
static uint64_t guest_call2(void *fn, uint64_t a0, uint64_t a1) {
    return runtime_guest_call(fn, runtime_guest_stack_top(), a0, a1, 0, 0, 0);
}

typedef struct { uint32_t slot; uint64_t value; int ok; } TlsThreadProbe;
static void *tls_probe_thread(void *p) {
    TlsThreadProbe *t = p;
    runtime_thread_init();
    void *set = runtime_symbol("_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm");
    void *get = runtime_symbol("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE");
    if (!set || !get) { t->ok = 0; return NULL; }
    guest_call2(set, t->slot, t->value);
    uint64_t v = guest_call1(get, t->slot);
    uint64_t tlr_tls = 0;
    if (g_slots) memcpy(&tlr_tls, (void *)(uintptr_t)(g_slots->ktlr + 0x1f0), 8);
    t->ok = v == t->value && g_slots && tlr_tls == g_slots->ktls;
    fprintf(stderr, "[totk-tls] thread slot=%u get=%#llx tlr_tls=%#llx ok=%d\n",
            t->slot, (unsigned long long)v, (unsigned long long)tlr_tls, t->ok);
    return NULL;
}

static void tls_probe(void) {
    void *alloc = runtime_symbol("_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE");
    void *set = runtime_symbol("_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm");
    void *get = runtime_symbol("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE");
    void *free_slot = runtime_symbol("_ZN2nn2os11FreeTlsSlotENS0_7TlsSlotE");
    void *cur = runtime_symbol("_ZN2nn2os16GetCurrentThreadEv");
    void *stack = runtime_symbol("_ZN2nn2os19GetCurrentStackInfoEPmS1_");
    fprintf(stderr, "[totk-tls] symbols alloc=%p set=%p get=%p free=%p cur=%p stack=%p\n",
            alloc, set, get, free_slot, cur, stack);
    need(alloc && set && get && free_slot && cur && stack, "nn::os TLS symbols");

    uint32_t slot = 0xffffffffu;
    uint32_t ar = (uint32_t)guest_call2(alloc, (uintptr_t)&slot, 0);
    fprintf(stderr, "[totk-tls] AllocateTlsSlot -> result=%#x slot=%u\n", ar, slot);
    need(ar == 0 && slot != 0xffffffffu, "AllocateTlsSlot");
    guest_call2(set, slot, 0x1122334455667788ull);
    uint64_t gv = guest_call1(get, slot);
    fprintf(stderr, "[totk-tls] GetTlsValue(main) -> %#llx\n", (unsigned long long)gv);
    need(gv == 0x1122334455667788ull, "GetTlsValue(main)");

    void *thread = (void *)(uintptr_t)guest_call1(cur, 0);
    uint64_t base = 0, size = 0;
    uint64_t sb = 0, ss = 0;
    runtime_guest_call(stack, runtime_guest_stack_top(), (uintptr_t)&sb, (uintptr_t)&ss, 0, 0, 0);
    base = sb; size = ss;
    fprintf(stderr, "[totk-tls] GetCurrentThread=%p stack=%#llx+%#llx\n",
            thread, (unsigned long long)base, (unsigned long long)size);
    need(thread != NULL, "GetCurrentThread");
    uint64_t tlr = 0;
    if (g_slots) tlr = g_slots->ktlr;
    need(base && size && tlr >= base && tlr < base + size, "GetCurrentStackInfo covers TLR");
    uint64_t tlr_tls = 0;
    if (tlr) memcpy(&tlr_tls, (void *)(uintptr_t)(tlr + 0x1f0), 8);
    need(g_slots && tlr_tls == g_slots->ktls,
         "ThreadLocalRegion +0x1f0 holds the guest TLS pointer");

    TlsThreadProbe tp = {.slot = slot, .value = 0xaabbccddeeff0011ull, .ok = 0};
    pthread_t th;
    need(pthread_create(&th, NULL, tls_probe_thread, &tp) == 0, "TLS probe thread");
    need(pthread_join(th, NULL) == 0, "TLS probe join");
    need(tp.ok, "per-thread TLS value");
    need(guest_call1(get, slot) == 0x1122334455667788ull, "TLS value leaked across threads");

    guest_call1(free_slot, slot);
    printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"tls\","
           "\"slot\":%u,\"thread_independent\":true,\"stack\":\"%#llx+%#llx\"}\n",
           slot, (unsigned long long)base, (unsigned long long)size);
    fflush(stdout);
    _exit(0);
}

/* STOP_AFTER_INITS is a diagnostic for the constructor pass, so it implies INITS. */
static int inits_enabled(void) {
    return tls_probe_mode || getenv("TOTK_ANDROID_INITS") || getenv("TOTK_ANDROID_STOP_AFTER_INITS");
}

static void loader_ready(void) {
    printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"sdk-start\",\"stage\":\"loader-ready\"}\n");
    fflush(stdout);
    if (!inits_enabled()) {
        runtime_hle_trace_report();
        _exit(0);
    }
}

/* Use the host override when present; runtime_symbol() returns the guest SDK
 * function, which would go through fsp-srv and defeat this probe. */
static void *fs_entry(const char *name) {
    if (getenv("TOTK_ANDROID_FS_BY_SERVICE")) return runtime_symbol(name);
    void *p = runtime_override(name);
    return p ? p : runtime_symbol(name);
}

static void fs_probe(void) {
    const char *profile = getenv("TOTK_ANDROID_PROFILE");
    if (!profile) profile = getenv("TOTK_ANDROID_FSROOT");
    if (!profile) profile = "/data/local/tmp/totk-android/user-data";
    mkdir(profile, 0777);

    uint8_t *sdk = runtime_module_base("sdk");
    if (sdk) fprintf(stderr, "[totk-android] fs state=%u flag=%u\n", sdk[0xc688c0], sdk[0xc688d0]);
    void (*fs_init)(void) = (void (*)(void))runtime_symbol("_ZN2nn2fs24InitializeForApplicationEv");
    if (fs_init && sdk && sdk[0xc688c0] != 1 && !sdk[0xc688d0]) {
        fprintf(stderr, "[totk-android] InitializeForApplication -> %d\n", ((int (*)(void))fs_init)());
    }
    int (*mount_temp)(const char *) = (int (*)(const char *))fs_entry("_ZN2nn2fs21MountTemporaryStorageEPKc");
    int (*mount_save)(const char *, const uint64_t *) = (int (*)(const char *, const uint64_t *))fs_entry("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE");
    int (*mount_rom)(const char *, void *, uint64_t) = (int (*)(const char *, void *, uint64_t))fs_entry("_ZN2nn2fs8MountRomEPKcPvm");
    int (*create_file)(const char *, int64_t) = (int (*)(const char *, int64_t))fs_entry("_ZN2nn2fs10CreateFileEPKcl");
    int (*delete_file)(const char *) = (int (*)(const char *))fs_entry("_ZN2nn2fs10DeleteFileEPKc");
    int (*write_file)(uint64_t, int64_t, const void *, uint64_t, const int32_t *) = (int (*)(uint64_t, int64_t, const void *, uint64_t, const int32_t *))fs_entry("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE");
    int (*commit)(const char *) = (int (*)(const char *))fs_entry("_ZN2nn2fs6CommitEPKc");
    int (*open_file)(uint64_t *, const char *, int) = (int (*)(uint64_t *, const char *, int))fs_entry("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
    int (*get_size)(int64_t *, uint64_t) = (int (*)(int64_t *, uint64_t))fs_entry("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE");
    int (*read_file)(uint64_t *, uint64_t, int64_t, void *, uint64_t) = (int (*)(uint64_t *, uint64_t, int64_t, void *, uint64_t))fs_entry("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm");
    int (*open_dir)(uint64_t *, const char *, int) = (int (*)(uint64_t *, const char *, int))fs_entry("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci");
    int (*read_dir)(int64_t *, void *, uint64_t, int64_t) = (int (*)(int64_t *, void *, uint64_t, int64_t))fs_entry("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl");
    void (*close_file)(uint64_t) = (void (*)(uint64_t))fs_entry("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
    void (*close_dir)(uint64_t) = (void (*)(uint64_t))fs_entry("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE");
    need(mount_temp && open_file && get_size && read_file && open_dir && read_dir, "nn::fs probe symbols");

    int mr = mount_temp("temp");
    fprintf(stderr, "[totk-android] MountTemporaryStorage -> %d\n", mr);
    need(mr == 0, "MountTemporaryStorage");

    /* mount_temp() creates/cleans <profile>/temp, so write the file afterwards. */
    char temp_dir[PATH_MAX], test_path[PATH_MAX];
    snprintf(temp_dir, sizeof temp_dir, "%s/temp", profile);
    snprintf(test_path, sizeof test_path, "%s/test.txt", temp_dir);
    int fd = open(test_path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    need(fd >= 0, "create fs probe file");
    need(write(fd, "hello", 5) == 5, "write fs probe file");
    close(fd);

    uint64_t file = 0;
    int fr = open_file(&file, "temp:/test.txt", 1);
    fprintf(stderr, "[totk-android] OpenFile -> %d handle=%#llx\n", fr, (unsigned long long)file);
    need(fr == 0, "OpenFile temp:/test.txt");
    int64_t size = -1;
    int sr = get_size(&size, file);
    fprintf(stderr, "[totk-android] GetFileSize -> %d size=%lld\n", sr, (long long)size);
    need(sr == 0 && size == 5, "GetFileSize temp:/test.txt");
    char data[32] = {0};
    uint64_t read = 0;
    int rr = read_file(&read, file, 0, data, sizeof data);
    fprintf(stderr, "[totk-android] ReadFile -> %d read=%llu data=%.*s\n",
            rr, (unsigned long long)read, (int)(read < sizeof data ? read : sizeof data), data);
    need(rr == 0 && read == 5 && !memcmp(data, "hello", 5), "ReadFile temp:/test.txt");
    if (close_file) close_file(file);
    uint64_t dir = 0;
    int dr = open_dir(&dir, "temp:/", 3);
    fprintf(stderr, "[totk-android] OpenDirectory -> %d handle=%#llx\n", dr, (unsigned long long)dir);
    need(dr == 0, "OpenDirectory temp:/");
    uint8_t entries[0x310 * 8];
    int64_t count = 0;
    int er = read_dir(&count, entries, dir, 8);
    fprintf(stderr, "[totk-android] ReadDirectory -> %d count=%lld\n", er, (long long)count);
    need(er == 0 && count >= 1, "ReadDirectory temp:/");
    if (close_dir) close_dir(dir);
    if (mount_save && create_file && write_file && commit) {
        const uint64_t uid[2] = {0, 0};
        int mr = mount_save("save", uid);
        fprintf(stderr, "[totk-android] MountSaveData -> %d\n", mr);
        need(mr == 0, "MountSaveData save");
        if (delete_file) delete_file("save:/probe.sav");   /* keep the probe repeatable */
        int cr = create_file("save:/probe.sav", 4);
        fprintf(stderr, "[totk-android] CreateFile save:/probe.sav -> %d\n", cr);
        need(cr == 0, "CreateFile save:/probe.sav");
        uint64_t save_file = 0;
        int ofr = open_file(&save_file, "save:/probe.sav", 2);
        fprintf(stderr, "[totk-android] OpenFile save:/probe.sav -> %d handle=%#llx\n",
                ofr, (unsigned long long)save_file);
        need(ofr == 0, "OpenFile save:/probe.sav");
        int wr = write_file(save_file, 0, "save", 4, NULL);
        fprintf(stderr, "[totk-android] WriteFile save:/probe.sav -> %d\n", wr);
        need(wr == 0, "WriteFile save:/probe.sav");
        if (close_file) close_file(save_file);
        int comr = commit("save");
        fprintf(stderr, "[totk-android] Commit save -> %d\n", comr);
        need(comr == 0, "Commit save");
    }
    if (getenv("TOTK_ANDROID_ROMFS") && mount_rom) {
        int mrr = mount_rom("rom", NULL, 0);
        fprintf(stderr, "[totk-android] MountRom -> %d\n", mrr);
        need(mrr == 0, "MountRom rom");
        uint64_t rom_file = 0;
        int ror = open_file(&rom_file, "rom:/hello.txt", 1);
        fprintf(stderr, "[totk-android] OpenFile rom:/hello.txt -> %d handle=%#llx\n",
                ror, (unsigned long long)rom_file);
        need(ror == 0, "OpenFile rom:/hello.txt");
        char rom_data[32] = {0};
        uint64_t rom_read = 0;
        int rrr = read_file(&rom_read, rom_file, 0, rom_data, sizeof rom_data);
        fprintf(stderr, "[totk-android] ReadFile rom:/hello.txt -> %d read=%llu data=%.*s\n",
                rrr, (unsigned long long)rom_read,
                (int)(rom_read < sizeof rom_data ? rom_read : sizeof rom_data), rom_data);
        need(rrr == 0 && rom_read == 10 && !memcmp(rom_data, "hello-rom\n", 10), "ReadFile rom:/hello.txt");
        if (close_file) close_file(rom_file);
    }
    printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"fs\",\"read\":%llu,\"entries\":%lld}\n",
           (unsigned long long)read, (long long)count);
    fflush(stdout);
}

/* Run a module's .init_array on the host.  The guest DT_INIT wrapper only
 * runs this loop when __nnmusl_init_dso reports a fini callback (state 2); when
 * the module has none it returns 1 and the wrapper skips every constructor.
 * On this 1.0.0 Android rewrite that leaves SEAD's globals uninitialised, so
 * the loader performs the constructor pass itself after the guest's musl
 * bookkeeping has run.  This is an init hook: the guest image is untouched. */
static uint64_t run_init_array(Module *m) {
    uint64_t n = m->init_size / 8, called = 0;
    uint64_t watch_before = 0;
    uint8_t *main_base = runtime_module_base("main");
    int watch = getenv("TOTK_ANDROID_CTOR_PROBE") && main_base;
    if (watch) memcpy(&watch_before, main_base + 0x463c048, 8);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t fn = 0;
        memcpy(&fn, m->base + m->init_array + i * 8, 8);
        if (!fn) continue;
        ((void (*)(void))(uintptr_t)fn)();
        called++;
        if (watch) {
            uint64_t now = 0;
            memcpy(&now, main_base + 0x463c048, 8);
            if (now != watch_before) {
                fprintf(stderr, "[totk-android] constructor %llu sets main+0x463c048 = %#llx\n",
                        (unsigned long long)i, (unsigned long long)now);
                watch_before = now;
            }
        }
    }
    return called;
}

static void module_inits(void) {
    if (inits_enabled()) {
        for (unsigned i = 0; i < NMODULES; i++) {
            fprintf(stderr, "[totk-android] ENTER %s DT_INIT\n", modules[i].name);
            if (getenv("TOTK_ANDROID_CTOR_PROBE")) {
                const Module *m = &modules[i];
                uint32_t before = 0;
                memcpy(&before, m->base + m->ctor_flag, 4);
                uint64_t entries = m->init_size / 8, nonnull = 0;
                for (uint64_t k = 0; k < entries; k++) {
                    uint64_t fn = 0;
                    memcpy(&fn, m->base + m->init_array + k * 8, 8);
                    if (fn) nonnull++;
                }
                fprintf(stderr, "[totk-android] ctor probe %s state_before=%u .init_array=%llu entries (%llu non-null)\n",
                        m->name, before, (unsigned long long)entries, (unsigned long long)nonnull);
            }
            uint64_t ret = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                          uint64_t, uint64_t, uint64_t, uint64_t))
                            (modules[i].base + modules[i].init))(0, 0, 0, 0, 0, 0, 0, 0, 0);
            uint32_t state;
            memcpy(&state, modules[i].base + modules[i].ctor_flag, 4);
            fprintf(stderr, "[totk-android] RETURN %s DT_INIT state=%u ret=%#llx init_array=%llu\n",
                    modules[i].name, state, (unsigned long long)ret,
                    (unsigned long long)(modules[i].init_size / 8));
            need(state == 1 || state == 2, "DT_INIT deferred or failed");
            if (state == 1) {
                uint64_t called = run_init_array(&modules[i]);
                fprintf(stderr, "[totk-android] constructors %s (host pass): %llu of %llu\n",
                        modules[i].name, (unsigned long long)called,
                        (unsigned long long)(modules[i].init_size / 8));
            }
        }
        if (getenv("TOTK_ANDROID_CTOR_PROBE")) {
            uint64_t tlr = 0, tt = 0, tb = 0, ts = 0, t70 = 0;
            if (g_slots) { tlr = g_slots->ktlr; memcpy(&tt, (void *)(uintptr_t)(tlr + 0x1f8), 8); }
            if (tt) { memcpy(&tb, (void *)(uintptr_t)(tt + 0x50), 8); memcpy(&ts, (void *)(uintptr_t)(tt + 0x58), 8); memcpy(&t70, (void *)(uintptr_t)(tt + 0x70), 8); }
            fprintf(stderr, "[totk-android] tls state ktlr=%#llx tt=%#llx base=%#llx size=%#llx t70=%#llx\n",
                    (unsigned long long)tlr, (unsigned long long)tt,
                    (unsigned long long)tb, (unsigned long long)ts, (unsigned long long)t70);
            uint8_t *mb = runtime_module_base("main");
            if (mb) {
                uint64_t thread_mgr = 0;
                memcpy(&thread_mgr, mb + 0x463c000, 8);   /* 1.0.0-only diagnostic */
                fprintf(stderr, "[totk-android] after inits sead::ThreadMgr=%#llx\n",
                        (unsigned long long)thread_mgr);
            }
        }
        if (tls_probe_mode) tls_probe();
        if (getenv("TOTK_ANDROID_FS_PROBE")) fs_probe();
    }
    if (getenv("TOTK_ANDROID_STOP_AFTER_INITS")) {
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"sdk-start\",\"stage\":\"module-inits\"}\n");
        fflush(stdout);
        runtime_hle_trace_report();
        _exit(0);
    }
}

static void module_finis(void) { }

static char *read_text_file(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (off_t)(16 << 20)) { close(fd); return NULL; }
    char *b = malloc((size_t)st.st_size + 1);
    ssize_t n = b ? read(fd, b, (size_t)st.st_size) : -1;
    close(fd);
    if (!b || n != st.st_size) { free(b); return NULL; }
    b[n] = 0;
    if (size) *size = (size_t)n;
    return b;
}

/* The generated .res.json files are simple, trusted arrays.  Read the
 * "cbufs" binding numbers without pulling a JSON dependency into the CLI. */
static unsigned reflect_cbufs(const char *path, uint32_t *out, unsigned max) {
    if (!path || !*path || !out || !max) return 0;
    char *text = read_text_file(path, NULL);
    if (!text) { fprintf(stderr, "[totk-vk] ERROR read reflection %s\n", path); return 0; }
    const char *p = strstr(text, "\"cbufs\"");
    if (!p) { free(text); return 0; }
    p = strchr(p, '[');
    unsigned n = 0;
    while (p && n < max) {
        const char *obj = strchr(p, '{');
        const char *end = strchr(p, ']');
        if (!obj || (end && obj > end)) break;
        const char *close = strchr(obj, '}');
        if (!close) break;
        const char *b = strstr(obj, "\"binding\"");
        if (b && b < close) {
            b += strlen("\"binding\"");
            while (*b && (*b == ' ' || *b == ':' || *b == '\t')) b++;
            uint32_t v = (uint32_t)strtoul(b, NULL, 10);
            int dup = 0;
            for (unsigned i = 0; i < n; i++) if (out[i] == v) dup = 1;
            if (!dup) out[n++] = v;
        }
        p = close + 1;
    }
    free(text);
    return n;
}


/* The generated .res.json is trusted, flat JSON: arrays of small objects with
 * no nested objects.  These helpers locate one member without pulling a JSON
 * dependency into the CLI; bounds are always the current object's close brace. */
static const char *json_member(const char *obj, const char *end, const char *key) {
    size_t klen = strlen(key);
    const char *p = obj;
    while (p < end) {
        p = memchr(p, '"', (size_t)(end - p));
        if (!p) return NULL;
        const char *q = memchr(p + 1, '"', (size_t)(end - p - 1));
        if (!q) return NULL;
        if ((size_t)(q - p - 1) == klen && !memcmp(p + 1, key, klen)) {
            const char *c = q + 1;
            while (c < end && (*c == ':' || *c == ' ' || *c == '\t' || *c == '\r' || *c == '\n')) c++;
            return c < end ? c : NULL;
        }
        p = q + 1;
    }
    return NULL;
}

static long json_long(const char *obj, const char *end, const char *key, long dflt) {
    const char *p = json_member(obj, end, key);
    if (!p) return dflt;
    char *e = NULL;
    long v = strtol(p, &e, 10);
    return e == p ? dflt : v;
}

static int json_bool(const char *obj, const char *end, const char *key, int dflt) {
    const char *p = json_member(obj, end, key);
    if (!p) return dflt;
    if ((size_t)(end - p) >= 4 && !memcmp(p, "true", 4)) return 1;
    if ((size_t)(end - p) >= 5 && !memcmp(p, "false", 5)) return 0;
    return dflt;
}

static void json_string(const char *obj, const char *end, const char *key,
                        char *out, size_t out_size) {
    if (out && out_size) out[0] = 0;
    const char *p = json_member(obj, end, key);
    if (!p || p >= end || *p != '"') return;
    p++;
    const char *q = memchr(p, '"', (size_t)(end - p));
    if (!q) return;
    size_t n = (size_t)(q - p);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

/* Ryujinx.Graphics.Shader.TextureFormat, in declaration order. */
static uint32_t reflect_texture_format(const char *name) {
    static const char *const names[] = {
        "Unknown", "R8Unorm", "R8Snorm", "R8Uint", "R8Sint",
        "R16Float", "R16Unorm", "R16Snorm", "R16Uint", "R16Sint",
        "R32Float", "R32Uint", "R32Sint",
        "R8G8Unorm", "R8G8Snorm", "R8G8Uint", "R8G8Sint",
        "R16G16Float", "R16G16Unorm", "R16G16Snorm", "R16G16Uint", "R16G16Sint",
        "R32G32Float", "R32G32Uint", "R32G32Sint",
        "R8G8B8A8Unorm", "R8G8B8A8Snorm", "R8G8B8A8Uint", "R8G8B8A8Sint",
        "R16G16B16A16Float", "R16G16B16A16Unorm", "R16G16B16A16Snorm",
        "R16G16B16A16Uint", "R16G16B16A16Sint",
        "R32G32B32A32Float", "R32G32B32A32Uint", "R32G32B32A32Sint",
        "R10G10B10A2Unorm", "R10G10B10A2Uint", "R11G11B10Float",
    };
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(name, names[i])) return i;
    return 0;
}

static GfxBindingKind reflect_json_kind(const char *section, long type, int separate) {
    if (!strcmp(section, "cbufs")) return GFX_BIND_UBO;
    if (!strcmp(section, "sbufs")) return GFX_BIND_SSBO;
    if (!strcmp(section, "images"))
        return (type & 0xff) == 2 ? GFX_BIND_STORAGE_TEXEL_BUFFER : GFX_BIND_STORAGE_IMAGE;
    if (type == 0) return GFX_BIND_SAMPLER;
    if ((type & 0xff) == 2) return GFX_BIND_UNIFORM_TEXEL_BUFFER;
    return separate ? GFX_BIND_SAMPLED_IMAGE : GFX_BIND_COMBINED_IMAGE_SAMPLER;
}

static unsigned reflect_json_section(const char *text, const char *section,
                                     GfxBindingSpec *out, unsigned max) {
    char key[64]; snprintf(key, sizeof key, "\"%s\"", section);
    const char *p = strstr(text, key);
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    const char *end_arr = strchr(p, ']');
    if (!end_arr) return 0;
    unsigned n = 0;
    while (p < end_arr && n < max) {
        const char *obj = strchr(p, '{');
        if (!obj || obj > end_arr) break;
        const char *close = strchr(obj, '}');
        if (!close || close > end_arr) break;

        long lb = json_long(obj, close, "binding", -1);
        if (lb >= 0) {
            GfxBindingSpec s; memset(&s, 0, sizeof s);
            s.binding = (uint32_t)lb;
            long type = json_long(obj, close, "type", 0);
            s.separate = json_bool(obj, close, "separate", 0);
            s.kind = reflect_json_kind(section, type, s.separate);
            s.count = 1;
            long array = json_long(obj, close, "array", 1);
            if (!strcmp(section, "textures") || !strcmp(section, "images"))
                s.count = array > 0 ? (uint32_t)array : 0;
            s.nvn_slot = (int32_t)json_long(obj, close, "slot", -1);
            s.nvn_cbuf_slot = (int32_t)json_long(obj, close, "cbuf_slot",
                                                json_long(obj, close, "cb_slot", -1));
            s.nvn_cb_offset = (int32_t)json_long(obj, close, "cb_offset", 0);
            s.nvn_handle = (uint32_t)json_long(obj, close, "handle", 0);
            s.nvn_type = (uint32_t)type;
            s.nvn_flags = (uint32_t)json_long(obj, close, "flags", 0);
            char fmt[48]; json_string(obj, close, "format", fmt, sizeof fmt);
            s.nvn_format = reflect_texture_format(fmt);
            /* The translated immediate is the texture slot in the default c2
             * table (cbuf_slot < 0), or the encoded table offset when the
             * translator named c2 explicitly.  Keep both forms. */
            if ((!strcmp(section, "textures") || !strcmp(section, "images")) &&
                s.nvn_slot < 0 && s.nvn_handle && (s.nvn_cbuf_slot < 0 || s.nvn_cbuf_slot == 2)) {
                uint32_t off0 = s.nvn_handle & 0x3fffu;
                uint32_t bytes = off0 * 4u;
                if (bytes >= 0x168u && bytes < 0x568u) s.nvn_slot = (int32_t)((bytes - 0x168u) / 8u);
                else if (off0 < 128u) s.nvn_slot = (int32_t)off0;
            }

            int dup = 0;
            for (unsigned i = 0; i < n; i++)
                if (out[i].binding == s.binding) { dup = 1; break; }
            if (!dup) out[n++] = s;
        }
        p = close + 1;
    }
    return n;
}

static unsigned reflect_json_file(const char *path, GfxBindingSpec *out, unsigned max) {
    if (!path || !*path || !out || !max) return 0;
    char *text = read_text_file(path, NULL);
    if (!text) { fprintf(stderr, "[totk-vk] ERROR read reflection %s\n", path); return 0; }
    unsigned n = 0;
    n += reflect_json_section(text, "cbufs", out + n, max - n);
    n += reflect_json_section(text, "sbufs", out + n, max - n);
    n += reflect_json_section(text, "textures", out + n, max - n);
    n += reflect_json_section(text, "images", out + n, max - n);
    free(text);
    return n;
}

/* ------------------------------------------------------------------------- */
/* SPIR-V descriptor signature                                               */

typedef struct {
    uint32_t kind;        /* 0 none, 1 image, 2 sampler, 3 sampled image, 4 array,
                           * 5 runtime array, 6 struct, 7 pointer */
    uint32_t elem;
    uint32_t length;
    uint32_t dim, depth, arrayed, ms, sampled, format;
    uint32_t storage, block;
} ReflectSpvType;

typedef struct { uint32_t id, ptr, storage; } ReflectSpvVar;

#define REFLECT_SPV_MAX_BOUND (1u << 20)

static int reflect_spirv_file(const char *path, uint32_t target_set,
                              GfxBindingSpec *out, unsigned max) {
    if (!path || !*path || !out || !max) return 0;
    size_t size = 0;
    uint8_t *raw = (uint8_t *)read_text_file(path, &size);
    if (!raw) { fprintf(stderr, "[totk-vk] ERROR read SPIR-V %s\n", path); return 0; }
    int ok = 0;
    if (size < 20 || (size & 3) || memcmp(raw, "\x03\x02\x23\x07", 4)) {
        fprintf(stderr, "[totk-vk] ERROR bad SPIR-V %s\n", path);
        free(raw);
        return 0;
    }
    uint32_t *w = (uint32_t *)raw;
    uint32_t bound = w[3];
    if (!bound || bound > REFLECT_SPV_MAX_BOUND) { free(raw); return 0; }

    ReflectSpvType *types = calloc(bound, sizeof *types);
    ReflectSpvVar *vars = calloc(bound, sizeof *vars);
    uint32_t *dset = malloc(bound * sizeof *dset);
    uint32_t *dbind = malloc(bound * sizeof *dbind);
    uint32_t *dblock = calloc(bound, sizeof *dblock);
    uint32_t *consts = calloc(bound, sizeof *consts);
    uint8_t *const_ok = calloc(bound, 1);
    if (!types || !vars || !dset || !dbind || !dblock || !consts || !const_ok) goto done;
    for (uint32_t i = 0; i < bound; i++) { dset[i] = UINT32_MAX; dbind[i] = UINT32_MAX; }
    unsigned nvars = 0;

    for (size_t i = 5; i < size / 4;) {
        uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
        if (!wc || i + wc > size / 4) break;
        const uint32_t *o = w + i + 1;
        switch (op) {
        case 25: /* OpTypeImage */
            if (wc >= 9) {
                uint32_t id = o[0];
                if (id < bound) {
                    types[id].kind = 1;
                    types[id].dim = o[2]; types[id].depth = o[3];
                    types[id].arrayed = o[4]; types[id].ms = o[5];
                    types[id].sampled = o[6]; types[id].format = o[7];
                }
            }
            break;
        case 26: /* OpTypeSampler */
            if (wc >= 2 && o[0] < bound) types[o[0]].kind = 2;
            break;
        case 27: /* OpTypeSampledImage */
            if (wc >= 3 && o[0] < bound) { types[o[0]].kind = 3; types[o[0]].elem = o[1]; }
            break;
        case 28: /* OpTypeArray */
            if (wc >= 4 && o[0] < bound) {
                types[o[0]].kind = 4; types[o[0]].elem = o[1]; types[o[0]].length = o[2];
            }
            break;
        case 29: /* OpTypeRuntimeArray */
            if (wc >= 3 && o[0] < bound) { types[o[0]].kind = 5; types[o[0]].elem = o[1]; }
            break;
        case 30: /* OpTypeStruct */
            if (wc >= 2 && o[0] < bound) types[o[0]].kind = 6;
            break;
        case 32: /* OpTypePointer */
            if (wc >= 4 && o[0] < bound) {
                types[o[0]].kind = 7; types[o[0]].storage = o[1]; types[o[0]].elem = o[2];
            }
            break;
        case 43: /* OpConstant */
        case 50: /* OpSpecConstant */
            if (wc >= 4 && o[1] < bound) { consts[o[1]] = o[2]; const_ok[o[1]] = 1; }
            break;
        case 59: /* OpVariable */
            if (wc >= 4 && nvars < bound) {
                vars[nvars++] = (ReflectSpvVar){ o[1], o[0], o[2] };
            }
            break;
        case 71: /* OpDecorate */
            if (wc >= 4 && o[0] < bound) {
                if (o[1] == 33) dbind[o[0]] = o[2];       /* Binding */
                else if (o[1] == 34) dset[o[0]] = o[2];   /* DescriptorSet */
                else if (o[1] == 2) dblock[o[0]] = 2;     /* Block */
                else if (o[1] == 3) dblock[o[0]] = 3;     /* BufferBlock */
            }
            break;
        default:
            break;
        }
        i += wc;
    }

    unsigned n = 0;
    for (unsigned v = 0; v < nvars && n < max; v++) {
        uint32_t id = vars[v].id, ptr = vars[v].ptr;
        if (id >= bound || ptr >= bound || dset[id] == UINT32_MAX || dbind[id] == UINT32_MAX)
            continue;
        if (types[ptr].kind != 7) continue;
        uint32_t t = types[ptr].elem;
        uint32_t count = 1;
        for (unsigned guard = 0; guard < 8 && t < bound; guard++) {
            uint32_t k = types[t].kind;
            if (k == 4) {
                uint32_t len = types[t].length < bound && const_ok[types[t].length]
                                   ? consts[types[t].length] : 0;
                count = len;
                t = types[t].elem;
            } else if (k == 5) {
                count = 0;
                t = types[t].elem;
            } else break;
        }
        if (t >= bound) continue;
        GfxBindingSpec s; memset(&s, 0, sizeof s);
        s.set = target_set;
        s.binding = dbind[id];
        s.count = count;   /* 0 = runtime array; the backend uses the pool size */
        uint32_t k = types[t].kind;
        if (k == 3) { /* sampled image: Buffer dimension is a texel buffer */
            uint32_t img = types[t].elem;
            s.kind = (img < bound && types[img].kind == 1 && types[img].dim == 5)
                         ? GFX_BIND_UNIFORM_TEXEL_BUFFER
                         : GFX_BIND_COMBINED_IMAGE_SAMPLER;
        } else if (k == 2) {
            s.kind = GFX_BIND_SAMPLER;
        } else if (k == 1) {
            int buffer = types[t].dim == 5;
            int storage = types[t].sampled == 2 || (types[t].sampled != 1 && types[t].format != 0);
            s.kind = buffer ? (storage ? GFX_BIND_STORAGE_TEXEL_BUFFER : GFX_BIND_UNIFORM_TEXEL_BUFFER)
                            : (storage ? GFX_BIND_STORAGE_IMAGE : GFX_BIND_SAMPLED_IMAGE);
        } else if (k == 6) {
            /* SPIR-V 1.3 uses StorageBuffer (12) for SSBOs; older frontends use
             * Uniform (2) plus the legacy BufferBlock decoration. */
            s.kind = (types[ptr].storage == 12 || dblock[t] == 3) ? GFX_BIND_SSBO : GFX_BIND_UBO;
        } else {
            /* Uniform variables point at a struct; if the type was not seen,
             * storage class 2 is still a uniform block. */
            s.kind = GFX_BIND_UBO;
        }
        int dup = 0;
        for (unsigned i = 0; i < n; i++)
            if (out[i].binding == s.binding) { dup = 1; break; }
        if (!dup) out[n++] = s;
    }
    ok = (int)n;

done:
    free(types); free(vars); free(dset); free(dbind); free(dblock);
    free(consts); free(const_ok); free(raw);
    return ok;
}

/* User-defined interface locations.  A fragment input with no matching vertex
 * output is not a valid pipeline test: some Adreno driver versions fail (or
 * appear to hang) inside vkCreateGraphicsPipelines before reporting an error. */
static int spirv_interface_locations(const char *path, int fragment,
                                     uint32_t *out, unsigned max) {
    if (!path || !*path || !out || !max) return 0;
    size_t size = 0;
    uint8_t *raw = (uint8_t *)read_text_file(path, &size);
    if (!raw) return 0;
    if (size < 20 || (size & 3) || memcmp(raw, "\x03\x02\x23\x07", 4)) { free(raw); return 0; }
    uint32_t *w = (uint32_t *)raw, bound = w[3];
    if (!bound || bound > REFLECT_SPV_MAX_BOUND) { free(raw); return 0; }
    uint32_t *ptr_of = calloc(bound, sizeof *ptr_of);
    uint32_t *storage = calloc(bound, sizeof *storage);
    int32_t *location = malloc(bound * sizeof *location);
    uint8_t *builtin = calloc(bound, 1);
    uint32_t *entry_vars = calloc(bound, sizeof *entry_vars);
    if (!ptr_of || !storage || !location || !builtin || !entry_vars) {
        free(ptr_of); free(storage); free(location); free(builtin); free(entry_vars); free(raw);
        return 0;
    }
    for (uint32_t i = 0; i < bound; i++) location[i] = -1;
    unsigned nentry = 0, n = 0;

    for (size_t i = 5; i < size / 4;) {
        uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
        if (!wc || i + wc > size / 4) break;
        const uint32_t *o = w + i + 1;
        if (op == 32 && wc >= 4 && o[0] < bound) {          /* OpTypePointer */
            storage[o[0]] = o[1];
        } else if (op == 59 && wc >= 4 && o[1] < bound) {   /* OpVariable */
            ptr_of[o[1]] = o[0];
        } else if (op == 71 && wc >= 4 && o[0] < bound) {   /* OpDecorate */
            if (o[1] == 30) location[o[0]] = (int32_t)o[2]; /* Location */
            else if (o[1] == 11) builtin[o[0]] = 1;         /* BuiltIn */
        } else if (op == 15 && wc >= 4) {                   /* OpEntryPoint */
            uint32_t model = o[0];
            if ((fragment && model != 4) || (!fragment && model != 0)) { i += wc; continue; }
            unsigned k = 2;  /* entry point name, null-terminated and padded */
            while (k < wc - 1) {
                uint32_t v = o[k++];
                if ((v & 0xff) == 0 || (v & 0xff00) == 0 ||
                    (v & 0xff0000) == 0 || (v & 0xff000000) == 0) break;
            }
            for (; k < wc - 1 && nentry < bound; k++) entry_vars[nentry++] = o[k];
        }
        i += wc;
    }

    for (unsigned i = 0; i < nentry && n < max; i++) {
        uint32_t id = entry_vars[i];
        if (id >= bound || builtin[id] || location[id] < 0) continue;
        uint32_t ptr = ptr_of[id];
        uint32_t want = fragment ? 1 : 3;   /* Input / Output */
        if (ptr >= bound || storage[ptr] != want) continue;
        int dup = 0;
        for (unsigned j = 0; j < n; j++) if (out[j] == (uint32_t)location[id]) dup = 1;
        if (!dup) out[n++] = (uint32_t)location[id];
    }
    free(ptr_of); free(storage); free(location); free(builtin); free(entry_vars); free(raw);
    return (int)n;
}

static int reflect_check_interface(const char *vs_path, const char *ps_path) {
    uint32_t vs_out[64], ps_in[64];
    int nvs = spirv_interface_locations(vs_path, 0, vs_out, 64);
    int nps = spirv_interface_locations(ps_path, 1, ps_in, 64);
    int bad = 0;
    for (int i = 0; i < nps; i++) {
        int found = 0;
        for (int j = 0; j < nvs; j++) if (vs_out[j] == ps_in[i]) found = 1;
        if (!found) {
            fprintf(stderr, "[totk-vk] ERROR PS input location %u has no VS output "
                            "(pipeline interface mismatch; use a matching VS/PS pair)\n", ps_in[i]);
            bad = 1;
        }
    }
    return bad ? 0 : 1;
}

/* SPIR-V owns the descriptor type and array length; .res.json only supplies
 * the NVN source metadata (slot/handle/format/flags). */
static unsigned reflect_merge(GfxBindingSpec *out, unsigned max,
                              const GfxBindingSpec *json, unsigned njson,
                              const GfxBindingSpec *spv, unsigned nspv) {
    unsigned n = 0;
    for (unsigned i = 0; i < nspv && n < max; i++) {
        GfxBindingSpec s = spv[i];
        for (unsigned j = 0; j < njson; j++) {
            if (json[j].binding != s.binding) continue;
            s.nvn_slot = json[j].nvn_slot;
            s.nvn_cbuf_slot = json[j].nvn_cbuf_slot;
            s.nvn_cb_offset = json[j].nvn_cb_offset;
            s.nvn_handle = json[j].nvn_handle;
            s.nvn_type = json[j].nvn_type;
            s.nvn_format = json[j].nvn_format;
            s.nvn_flags = json[j].nvn_flags;
            s.separate = json[j].separate;
            break;
        }
        out[n++] = s;
    }
    return n;
}

unsigned gfx_reflect_full(const char *json_path, const char *spv_path, uint32_t target_set,
                          GfxBindingSpec *out, unsigned max) {
    GfxBindingSpec json[64], spv[64];
    unsigned njson = reflect_json_file(json_path, json, 64);
    unsigned nspv = reflect_spirv_file(spv_path, target_set, spv, 64);
    if (!nspv) {
        for (unsigned i = 0; i < njson && i < max; i++) { json[i].set = target_set; out[i] = json[i]; }
        return njson < max ? njson : max;
    }
    return reflect_merge(out, max, json, njson, spv, nspv);
}

static const char *reflect_kind_name(GfxBindingKind k) {
    switch (k) {
    case GFX_BIND_UBO: return "ubo";
    case GFX_BIND_SSBO: return "ssbo";
    case GFX_BIND_SAMPLER: return "sampler";
    case GFX_BIND_SAMPLED_IMAGE: return "sampled-image";
    case GFX_BIND_COMBINED_IMAGE_SAMPLER: return "combined-sampler";
    case GFX_BIND_STORAGE_IMAGE: return "storage-image";
    case GFX_BIND_UNIFORM_TEXEL_BUFFER: return "uniform-texel-buffer";
    case GFX_BIND_STORAGE_TEXEL_BUFFER: return "storage-texel-buffer";
    default: return "?";
    }
}

static int reflect_has_non_ubo(const char *path) {
    GfxBindingSpec s[64];
    unsigned n = reflect_json_file(path, s, 64);
    for (unsigned i = 0; i < n; i++)
        if (s[i].kind != GFX_BIND_UBO) return 1;
    return 0;
}

static void reflect_log(const char *stage, const GfxBindingSpec *s, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        fprintf(stderr, "[totk-vk] reflect %s set=%u binding=%u kind=%s count=%u"
                        " nvn(slot=%d cbuf=%d off=%d handle=%#x type=%#x fmt=%u flags=%#x separate=%d)\n",
                stage, s[i].set, s[i].binding, reflect_kind_name(s[i].kind), s[i].count,
                s[i].nvn_slot, s[i].nvn_cbuf_slot, s[i].nvn_cb_offset, s[i].nvn_handle,
                s[i].nvn_type, s[i].nvn_format, s[i].nvn_flags, s[i].separate);
}

int gfx_spirv_move_set(uint8_t *code, size_t size, uint32_t from, uint32_t to) {
    if (!code || size < 20 || (size & 3)) return 0;
    uint32_t *w = (uint32_t *)code;
    for (size_t i = 5; i < size / 4; ) {
        uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
        if (!wc || i + wc > size / 4) break;
        /* OpDecorate target DescriptorSet value */
        if (op == 71 && wc >= 4 && w[i + 2] == 34 && w[i + 3] == from) w[i + 3] = to;
        i += wc;
    }
    return 1;
}

static int probe_vulkan_triangle(const char *dir) {
    const char *fs = getenv("TOTK_ANDROID_TRIANGLE_FRAMES");
    int frames = fs && *fs ? atoi(fs) : 60;
    if (frames < 1) frames = 1;
    if (frames > 600) frames = 600;
    GfxOffscreenTarget *t = gfx_backend_offscreen_create(1280, 720, 0x25);
    if (!t) { fprintf(stderr, "[totk-vk] ERROR triangle target\n"); return 1; }
    GfxTrianglePipeline *p = gfx_backend_builtin_triangle_pipeline();
    if (!p) { gfx_backend_offscreen_destroy(t); return 1; }
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < frames; i++) {
        if (!gfx_backend_triangle_draw(p, t, 0.05f, 0.08f, 0.12f, 1.0f)) {
            fprintf(stderr, "[totk-vk] ERROR triangle draw frame %d\n", i);
            gfx_backend_offscreen_destroy(t);
            return 1;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double sec = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    uint8_t center[4] = {0}, corner[4] = {0};
    gfx_backend_offscreen_read_pixel(t, 640, 360, center);
    gfx_backend_offscreen_read_pixel(t, 0, 0, corner);
    uint64_t hash = gfx_backend_offscreen_hash(t);
    char shot[PATH_MAX];
    const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
    if (env && *env) snprintf(shot, sizeof shot, "%s", env);
    else snprintf(shot, sizeof shot, "%s/triangle.ppm", dir);
    int shot_ok = gfx_backend_offscreen_write_ppm(t, shot);
    int ok = center[0] > 200 && center[1] > 60 && center[1] < 100 &&
             center[2] < 40 && corner[0] < 40 && corner[1] < 40;
    printf("{\"status\":\"%s\",\"host\":\"android\",\"probe\":\"vulkan-triangle\","
           "\"frames\":%d,\"fps\":%.2f,\"center\":\"%02x%02x%02x%02x\","
           "\"corner\":\"%02x%02x%02x%02x\",\"hash\":\"%016llx\",\"screenshot\":\"%s\","
           "\"screenshot_ok\":%s}\n",
           ok ? "pass" : "fail", frames, sec > 0 ? frames / sec : 0.0,
           center[0], center[1], center[2], center[3],
           corner[0], corner[1], corner[2], corner[3],
           (unsigned long long)hash, shot, shot_ok ? "true" : "false");
    gfx_backend_offscreen_destroy(t);
    return ok ? 0 : 1;
}

static int boot_sdk(void) {
    uint32_t main_handle = svc_thread_main_init();
    runtime_thread_set_guest_handle(main_handle);
    void *start = runtime_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    need(start != NULL, "nn::init::Start symbol");
    fprintf(stderr, "[totk-android] ENTER nn::init::Start\n");
    runtime_guest_call(start, runtime_guest_stack_top(), main_handle, 0,
                       (uint64_t)(uintptr_t)loader_ready,
                       (uint64_t)(uintptr_t)module_inits,
                       (uint64_t)(uintptr_t)module_finis);
    runtime_fail("nn::init::Start returned");
    return 1;
}

static int probe_hle(void) {
    typedef uint64_t (*Fn)(uint64_t, uint64_t);
    Fn f = (Fn)runtime_hle_wrap_named((void *)host_add, "host_add");
    need(f != NULL, "HLE thunk");
    uint64_t r = f(3, 4);
    need(r == 7, "HLE thunk result");
    printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"hle-stack\",\"result\":7}\n");
    return 0;
}

static int probe(void) {
    uintptr_t slot;
    memcpy(&slot, modules[0].base + PROBE_SLOT, 8);
    void *required = runtime_symbol("_ZN2nn2os13GetSystemTickEv");
    need(required != NULL, "GetSystemTick symbol");
    need(slot == (uintptr_t)required, "main PLT GOT resolution");
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) need(pthread_create(&threads[i], NULL, probe_thread, NULL) == 0, "probe thread");
    for (unsigned i = 0; i < 4; i++) need(pthread_join(threads[i], NULL) == 0, "join probe");
    need(timer_calls == 4000, "timer gate count");
    printf("{\"status\":\"pass\",\"host\":\"android\",\"execution\":\"native arm64 main PLT -> sdk -> host timer -> return\","
           "\"calls\":4000,\"threads\":4,\"modules\":[");
    for (unsigned i = 0; i < NMODULES; i++) {
        Module *m = &modules[i];
        printf("%s{\"name\":\"%s\",\"relative\":%u,\"symbolic\":%u,\"cross\":%u,\"weak_zero\":%u}",
               i ? "," : "", m->name, m->relative, m->symbolic, m->cross, m->weak);
    }
    puts("],\"scope\":\"cross-module calls; constructors and game entry not run\"}");
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    fprintf(stderr, "[totk-android] main start\n");
    need(argc == 3, "usage: totk-android <rewritten-dir> probe");
    runtime_thread_init();
    fprintf(stderr, "[totk-android] thread init: host_ctx_off=%#llx guest_slots_off=%#llx\n",
            (unsigned long long)runtime_host_ctx_off, (unsigned long long)runtime_guest_slots_off);
    for (unsigned i = 0; i < NMODULES; i++) map_module(&modules[i], argv[1]);
    fprintf(stderr, "[totk-android] mapped all\n");
    for (unsigned i = 0; i < NMODULES; i++) relocate(&modules[i]);
    fprintf(stderr, "[totk-android] relocated all\n");
    for (unsigned i = 0; i < NMODULES; i++) seal(&modules[i]);
    fprintf(stderr, "[totk-android] sealed all\n");
    if (!strcmp(argv[2], "probe")) return probe();
    if (!strcmp(argv[2], "probe-hle")) return probe_hle();
    if (!strcmp(argv[2], "probe-vulkan-triangle")) return probe_vulkan_triangle(argv[1]);
    if (!strcmp(argv[2], "probe-totk-reflect")) {
        const char *vsp = getenv("TOTK_ANDROID_SPV_VS");
        const char *psp = getenv("TOTK_ANDROID_SPV_PS");
        const char *rvp = getenv("TOTK_ANDROID_REFL_VS");
        const char *rpp = getenv("TOTK_ANDROID_REFL_PS");
        if (!vsp || !psp || !rvp || !rpp) {
            fprintf(stderr, "[totk-vk] ERROR probe-totk-reflect needs SPV_VS/SPV_PS/REFL_VS/REFL_PS\n");
            return 1;
        }
        if (!reflect_check_interface(vsp, psp) && !getenv("TOTK_ANDROID_ALLOW_INTERFACE_MISMATCH"))
            return 1;
        size_t vsn = 0, psn = 0;
        uint8_t *vs = (uint8_t *)read_text_file(vsp, &vsn);
        uint8_t *ps = (uint8_t *)read_text_file(psp, &psn);
        if (!vs || !ps) { free(vs); free(ps); return 1; }
        gfx_spirv_move_set(ps, psn, 0, 1);
        GfxBindingSpec vss[64], pss[64];
        unsigned nvs = gfx_reflect_full(rvp, vsp, 0, vss, 64);
        unsigned nps = gfx_reflect_full(rpp, psp, 1, pss, 64);
        fprintf(stderr, "[totk-vk] reflected bindings vs=%u ps=%u\n", nvs, nps);
        reflect_log("vs", vss, nvs);
        reflect_log("ps", pss, nps);
        GfxOffscreenTarget *t = gfx_backend_offscreen_create(1280, 720, 0x25);
        GfxReflectedPipeline *p = t ? gfx_backend_reflected_pipeline_create(
            vs, vsn, ps, psn, gfx_backend_vk_format_for_nvn(0x25), vss, nvs, pss, nps) : NULL;
        free(vs); free(ps);
        if (!t || !p) {
            if (t) gfx_backend_offscreen_destroy(t);
            fprintf(stderr, "[totk-vk] ERROR reflected pipeline\n");
            return 1;
        }
        for (int i = 0; i < 10; i++)
            if (!gfx_backend_reflected_draw(p, t, 0.05f, 0.08f, 0.12f, 1.0f)) {
                fprintf(stderr, "[totk-vk] ERROR reflected draw %d\n", i);
                gfx_backend_reflected_pipeline_destroy(p);
                gfx_backend_offscreen_destroy(t);
                return 1;
            }
        uint8_t center[4] = {0}, corner[4] = {0};
        gfx_backend_offscreen_read_pixel(t, 640, 360, center);
        gfx_backend_offscreen_read_pixel(t, 0, 0, corner);
        int visible = center[0] != corner[0] || center[1] != corner[1] || center[2] != corner[2];
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/reflect.ppm", argv[1]);
        int shot_ok = gfx_backend_offscreen_write_ppm(t, shot);
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"totk-reflect\","
               "\"vs_bindings\":%u,\"ps_bindings\":%u,\"center\":\"%02x%02x%02x%02x\","
               "\"corner\":\"%02x%02x%02x%02x\",\"visible\":%s,\"screenshot\":\"%s\","
               "\"screenshot_ok\":%s}\n", nvs, nps, center[0], center[1], center[2], center[3],
               corner[0], corner[1], corner[2], corner[3], visible ? "true" : "false",
               shot, shot_ok ? "true" : "false");
        gfx_backend_reflected_pipeline_destroy(p);
        gfx_backend_offscreen_destroy(t);
        return 0;
    }
    if (!strcmp(argv[2], "probe-totk-vs")) {
        const char *path = getenv("TOTK_ANDROID_SPV_VS");
        if (!path || !*path) path = "/data/local/tmp/totk-android/spv/system-vs-8511c97f.spv";
        int fd = open(path, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "[totk-vk] ERROR open %s\n", path); return 1; }
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (off_t)(16 << 20)) { close(fd); return 1; }
        uint8_t *vs = malloc((size_t)st.st_size);
        ssize_t got = vs ? read(fd, vs, (size_t)st.st_size) : -1;
        close(fd);
        if (!vs || got != st.st_size) { free(vs); return 1; }
        const char *ps_path = getenv("TOTK_ANDROID_SPV_PS");
        if (ps_path && *ps_path &&
            !reflect_check_interface(path, ps_path) && !getenv("TOTK_ANDROID_ALLOW_INTERFACE_MISMATCH")) {
            free(vs);
            return 1;
        }
        uint8_t *ps = NULL;
        size_t ps_size = 0;
        if (ps_path && *ps_path) {
            int pfd = open(ps_path, O_RDONLY);
            if (pfd < 0 || fstat(pfd, &st) != 0 || st.st_size <= 0 || st.st_size > (off_t)(16 << 20)) {
                if (pfd >= 0) close(pfd);
                free(vs);
                fprintf(stderr, "[totk-vk] ERROR open %s\n", ps_path);
                return 1;
            }
            ps_size = (size_t)st.st_size;
            ps = malloc(ps_size);
            ssize_t pgot = ps ? read(pfd, ps, ps_size) : -1;
            close(pfd);
            if (!ps || pgot != st.st_size) { free(vs); free(ps); return 1; }
        }
        GfxUboBinding ubos[8];
        uint32_t nubo = 0;
        const char *rv = getenv("TOTK_ANDROID_REFL_VS");
        const char *rp = getenv("TOTK_ANDROID_REFL_PS");
        if (((rv && reflect_has_non_ubo(rv)) || (rp && reflect_has_non_ubo(rp))) &&
            !getenv("TOTK_ANDROID_ALLOW_PARTIAL_REFLECTION")) {
            fprintf(stderr, "[totk-vk] ERROR probe-totk-vs only builds cbuf UBOs; "
                            "use probe-totk-reflect for sbufs/textures/images\n");
            free(vs); free(ps);
            return 1;
        }
        if (rv || rp) {
            uint32_t b[8];
            unsigned n = reflect_cbufs(rv, b, 8);
            for (unsigned i = 0; i < n; i++) ubos[nubo++] = (GfxUboBinding){ b[i], 0x10000 };
            n = reflect_cbufs(rp, b, 8);
            for (unsigned i = 0; i < n; i++) {
                int dup = 0;
                for (uint32_t j = 0; j < nubo; j++) if (ubos[j].binding == b[i]) dup = 1;
                if (!dup && nubo < 8) ubos[nubo++] = (GfxUboBinding){ b[i], 0x10000 };
            }
            fprintf(stderr, "[totk-vk] reflection cbufs: %u binding(s)\n", nubo);
        }
        if (!nubo) { ubos[nubo++] = (GfxUboBinding){1, 0x10000}; ubos[nubo++] = (GfxUboBinding){2, 0x10000}; }
        GfxOffscreenTarget *t = gfx_backend_offscreen_create(1280, 720, 0x25);
        GfxTrianglePipeline *p = NULL;
        if (t) {
            if (ps) {
                p = gfx_backend_triangle_pipeline_create_ubo(vs, (size_t)got, ps, ps_size,
                                                             gfx_backend_vk_format_for_nvn(0x25), ubos, nubo);
            } else {
                p = gfx_backend_totk_vs_pipeline_create(vs, (size_t)got, gfx_backend_vk_format_for_nvn(0x25));
            }
        }
        free(vs); free(ps);
        if (!t || !p) { if (t) gfx_backend_offscreen_destroy(t); fprintf(stderr, "[totk-vk] ERROR TOTK VS pipeline\n"); return 1; }
        float c4[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        float c11[4] = {255.0f, 0.0f, 0.0f, 0.0f};
        if (nubo) {
            if (!gfx_backend_triangle_write_ubo(p, 1, 147 * 16, c4, sizeof c4) ||
                !gfx_backend_triangle_write_ubo(p, 2, 25 * 16, c11, sizeof c11)) {
                fprintf(stderr, "[totk-vk] ERROR TOTK VS UBO write\n");
                gfx_backend_offscreen_destroy(t);
                return 1;
            }
        }
        for (int i = 0; i < 10; i++)
            if (!gfx_backend_triangle_draw(p, t, 0.05f, 0.08f, 0.12f, 1.0f)) {
                fprintf(stderr, "[totk-vk] ERROR TOTK VS draw %d\n", i);
                gfx_backend_offscreen_destroy(t);
                return 1;
            }
        uint8_t center[4] = {0}, corner[4] = {0};
        gfx_backend_offscreen_read_pixel(t, 640, 360, center);
        gfx_backend_offscreen_read_pixel(t, 0, 0, corner);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/totk-vs.ppm", argv[1]);
        int ok = ps_path && *ps_path
                 ? (center[0] > 200 && center[1] > 200 && center[2] > 200)
                 : (center[0] > 200 && center[1] > 60 && center[1] < 100 && center[2] < 40);
        int shot_ok = gfx_backend_offscreen_write_ppm(t, shot);
        printf("{\"status\":\"%s\",\"host\":\"android\",\"probe\":\"totk-vs\","
               "\"ps\":\"%s\",\"center\":\"%02x%02x%02x%02x\",\"corner\":\"%02x%02x%02x%02x\","
               "\"screenshot\":\"%s\",\"screenshot_ok\":%s}\n",
               ok ? "pass" : "fail", (ps_path && *ps_path) ? ps_path : "builtin",
               center[0], center[1], center[2], center[3],
               corner[0], corner[1], corner[2], corner[3], shot, shot_ok ? "true" : "false");
        gfx_backend_offscreen_destroy(t);
        return ok ? 0 : 1;
    }
    if (!strcmp(argv[2], "probe-vulkan-reflect-texture")) {
        if (!gfx_backend_reflected_texture_selftest()) {
            fprintf(stderr, "[totk-vk] reflected texture selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vulkan-reflect-texture\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-vulkan-reflect-ubo")) {
        if (!gfx_backend_reflected_ubo_selftest()) {
            fprintf(stderr, "[totk-vk] reflected UBO selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vulkan-reflect-ubo\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-vulkan-texture")) {
        GfxOffscreenTarget *t = gfx_backend_offscreen_create(1280, 720, 0x25);
        GfxTexturedPipeline *p = gfx_backend_builtin_textured_pipeline();
        static const uint8_t checker[16] = {
            255, 0, 0, 255,   0, 255, 0, 255,
            0, 0, 255, 255,   255, 255, 255, 255,
        };
        GfxTexture *tex = gfx_backend_texture_create_rgba8(2, 2, checker);
        if (!t || !p || !tex) {
            fprintf(stderr, "[totk-vk] ERROR texture probe setup\n");
            if (tex) gfx_backend_texture_destroy(tex);
            if (t) gfx_backend_offscreen_destroy(t);
            return 1;
        }
        for (int i = 0; i < 10; i++)
            if (!gfx_backend_textured_draw(p, tex, t, 0.05f, 0.08f, 0.12f, 1.0f)) {
                fprintf(stderr, "[totk-vk] ERROR textured draw %d\n", i);
                return 1;
            }
        uint8_t center[4] = {0}, corner[4] = {0};
        gfx_backend_offscreen_read_pixel(t, 640, 360, center);
        gfx_backend_offscreen_read_pixel(t, 0, 0, corner);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/texture.ppm", argv[1]);
        int shot_ok = gfx_backend_offscreen_write_ppm(t, shot);
        int ok = center[0] > 80 && center[0] < 180 && center[1] > 80 && center[1] < 180 &&
                 center[2] > 80 && center[2] < 180 && corner[0] < 40;
        printf("{\"status\":\"%s\",\"host\":\"android\",\"probe\":\"vulkan-texture\","
               "\"center\":\"%02x%02x%02x%02x\",\"corner\":\"%02x%02x%02x%02x\","
               "\"screenshot\":\"%s\",\"screenshot_ok\":%s}\n",
               ok ? "pass" : "fail", center[0], center[1], center[2], center[3],
               corner[0], corner[1], corner[2], corner[3], shot, shot_ok ? "true" : "false");
        gfx_backend_texture_destroy(tex);
        gfx_backend_offscreen_destroy(t);
        return ok ? 0 : 1;
    }
    if (!strcmp(argv[2], "probe-shader-cache")) {
        const char *root = getenv("TOTK_ANDROID_SHADER_CACHE");
        if (!root || !*root) root = runtime_shader_cache_root();
        static const char *const stages[] = { "vs", "ps", "gs", "tcs", "tes", "cs" };
        unsigned total = 0, failed = 0, pairs = 0;
        for (unsigned s = 0; s < sizeof stages / sizeof stages[0]; s++) {
            char dir[PATH_MAX];
            snprintf(dir, sizeof dir, "%s/%s", root, stages[s]);
            DIR *d = opendir(dir);
            if (!d) continue;
            struct dirent *ent;
            while ((ent = readdir(d))) {
                const char *name = ent->d_name;
                size_t n = strlen(name);
                if (n != 64 + 4 || strcmp(name + 64, ".spv")) continue;
                int key_ok = 1;
                for (size_t i = 0; i < 64; i++) {
                    char c = name[i];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { key_ok = 0; break; }
                }
                if (!key_ok) { failed++; continue; }
                char path[PATH_MAX];
                snprintf(path, sizeof path, "%s/%s", dir, name);
                int fd = open(path, O_RDONLY);
                if (fd < 0) { failed++; continue; }
                struct stat st;
                if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (off_t)(16 << 20)) {
                    close(fd); failed++; continue;
                }
                uint8_t *buf = malloc((size_t)st.st_size);
                ssize_t got = buf ? read(fd, buf, (size_t)st.st_size) : -1;
                close(fd);
                if (!buf || got != st.st_size) { free(buf); failed++; continue; }
                total++;
                if (!gfx_backend_validate_spirv(buf, (size_t)got, name)) failed++;
                free(buf);
                char res[PATH_MAX];
                snprintf(res, sizeof res, "%.*s.res.json", (int)(strlen(path) - 4), path);
                if (access(res, R_OK) == 0) pairs++;
            }
            closedir(d);
        }
        if (!total) {
            fprintf(stderr, "[totk-spv] ERROR no keyed .spv modules under %s\n", root);
            return 1;
        }
        printf("{\"status\":\"%s\",\"host\":\"android\",\"probe\":\"shader-cache\","
               "\"root\":\"%s\",\"modules\":%u,\"reflections\":%u,\"failed\":%u}\n",
               failed ? "fail" : "pass", root, total, pairs, failed);
        return failed ? 1 : 0;
    }
    if (!strcmp(argv[2], "probe-spirv")) {
        const char *dir = getenv("TOTK_ANDROID_SPV_DIR");
        if (!dir) dir = "/data/local/tmp/totk-android/spv";
        DIR *d = opendir(dir);
        if (!d) { fprintf(stderr, "[totk-spv] ERROR cannot open %s\n", dir); return 1; }
        unsigned total = 0, failed = 0;
        struct dirent *ent;
        while ((ent = readdir(d))) {
            size_t n = strlen(ent->d_name);
            if (n < 5 || strcmp(ent->d_name + n - 4, ".spv")) continue;
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
            int fd = open(path, O_RDONLY);
            if (fd < 0) { fprintf(stderr, "[totk-spv] ERROR open %s\n", path); failed++; continue; }
            struct stat st;
            if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (off_t)(16 << 20)) {
                fprintf(stderr, "[totk-spv] ERROR size %s\n", path); close(fd); failed++; continue;
            }
            uint8_t *buf = malloc((size_t)st.st_size);
            ssize_t got = buf ? read(fd, buf, (size_t)st.st_size) : -1;
            close(fd);
            if (!buf || got != st.st_size) { fprintf(stderr, "[totk-spv] ERROR read %s\n", path); free(buf); failed++; continue; }
            total++;
            if (!gfx_backend_validate_spirv(buf, (size_t)got, ent->d_name)) failed++;
            free(buf);
        }
        closedir(d);
        if (!total) { fprintf(stderr, "[totk-spv] ERROR no .spv files in %s\n", dir); return 1; }
        printf("{\"status\":\"%s\",\"host\":\"android\",\"probe\":\"spirv\",\"modules\":%u,\"failed\":%u}\n",
               failed ? "fail" : "pass", total, failed);
        return failed ? 1 : 0;
    }
    if (!strcmp(argv[2], "probe-vulkan")) {
        if (!gfx_backend_selftest()) { fprintf(stderr, "[totk-vk] selftest FAIL\n"); return 1; }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vulkan-offscreen\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-vulkan-texture-formats")) {
        int gfx_backend_texture_formats_selftest(void);
        if (!gfx_backend_texture_formats_selftest()) {
            fprintf(stderr, "[totk-vk] texture formats selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vulkan-texture-formats\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-vulkan-mrt")) {
        int gfx_backend_mrt_selftest(void);
        if (!gfx_backend_mrt_selftest()) {
            fprintf(stderr, "[totk-vk] MRT selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vulkan-mrt\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-vulkan-surface-target")) {
        if (!gfx_backend_surface_target_selftest()) {
            fprintf(stderr, "[totk-vk] surface target selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vulkan-surface-target\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-texture")) {
        int runtime_nvn_textured_draw_selftest(const char *screenshot, int frames);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-texture.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 60;
        if (!runtime_nvn_textured_draw_selftest(shot, frames)) {
            fprintf(stderr, "[totk-nvn] NVN texture draw selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-texture\","
               "\"frames\":%d,\"screenshot\":\"%s\"}\n", frames, shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-draw")) {
        int runtime_nvn_draw_selftest(const char *screenshot, int frames,
                                      const char *vs_path, const char *ps_path, int expect_visible);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-draw.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 60;
        const char *vs = getenv("TOTK_ANDROID_SPV_VS");
        const char *ps = getenv("TOTK_ANDROID_SPV_PS");
        int visible = !(vs && *vs && ps && *ps);
        if (!runtime_nvn_draw_selftest(shot, frames, vs, ps, visible)) {
            fprintf(stderr, "[totk-nvn] NVN draw selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-draw\","
               "\"frames\":%d,\"visible\":%s,\"screenshot\":\"%s\"}\n",
               frames, visible ? "true" : "false", shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-reflect")) {
        int runtime_nvn_draw_reflect_selftest(const char *screenshot, int frames,
                                              const char *vs_path, const char *ps_path,
                                              const char *vs_res, const char *ps_res);
        const char *vs = getenv("TOTK_ANDROID_SPV_VS");
        const char *ps = getenv("TOTK_ANDROID_SPV_PS");
        const char *vres = getenv("TOTK_ANDROID_REFL_VS");
        const char *pres = getenv("TOTK_ANDROID_REFL_PS");
        if (!vs || !ps || !vres || !pres) {
            fprintf(stderr, "[totk-nvn] ERROR probe-nvn-reflect needs SPV_VS/SPV_PS/REFL_VS/REFL_PS\n");
            return 1;
        }
        if (!reflect_check_interface(vs, ps) && !getenv("TOTK_ANDROID_ALLOW_INTERFACE_MISMATCH"))
            return 1;
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-reflect.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 60;
        if (!runtime_nvn_draw_reflect_selftest(shot, frames, vs, ps, vres, pres)) {
            fprintf(stderr, "[totk-nvn] NVN reflected draw selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-reflect\","
               "\"frames\":%d,\"screenshot\":\"%s\"}\n", frames, shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-texture-reflect")) {
        int runtime_nvn_texture_bind_selftest(const char *screenshot, int frames);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-texture-reflect.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 2;
        if (!runtime_nvn_texture_bind_selftest(shot, frames)) {
            fprintf(stderr, "[totk-nvn] NVN textured reflected draw selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-texture-reflect\","
               "\"frames\":%d,\"screenshot\":\"%s\"}\n", frames, shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-state")) {
        int runtime_nvn_state_selftest(const char *screenshot, int frames);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-state.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 2;
        if (!getenv("TOTK_ANDROID_SHADER_CACHE")) {
            static char cache[PATH_MAX];
            snprintf(cache, sizeof cache, "%s/shader-cache", argv[1]);
            setenv("TOTK_ANDROID_SHADER_CACHE", cache, 1);
        }
        if (!runtime_nvn_state_selftest(shot, frames)) {
            fprintf(stderr, "[totk-nvn] NVN state selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-state\","
               "\"frames\":%d,\"shader_cache\":\"%s\",\"screenshot\":\"%s\"}\n",
               frames, getenv("TOTK_ANDROID_SHADER_CACHE"), shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-frame")) {
        int runtime_nvn_frame_selftest(const char *screenshot, int frames);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-frame.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 2;
        if (!getenv("TOTK_ANDROID_SHADER_CACHE")) {
            static char cache[PATH_MAX];
            snprintf(cache, sizeof cache, "%s/shader-cache", argv[1]);
            setenv("TOTK_ANDROID_SHADER_CACHE", cache, 1);
        }
        if (!runtime_nvn_frame_selftest(shot, frames)) {
            fprintf(stderr, "[totk-nvn] NVN frame (shader cache) selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-frame\","
               "\"frames\":%d,\"shader_cache\":\"%s\",\"screenshot\":\"%s\"}\n",
               frames, getenv("TOTK_ANDROID_SHADER_CACHE"), shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-bind")) {
        int runtime_nvn_bind_selftest(void);
        if (!runtime_nvn_bind_selftest()) {
            fprintf(stderr, "[totk-nvn] NVN bind selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-bind\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-submit")) {
        int runtime_nvn_submit_selftest(void);
        if (!runtime_nvn_submit_selftest()) { fprintf(stderr, "[totk-nvn] submit selftest FAIL\n"); return 1; }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-submit-present\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-cb-lifetime")) {
        int runtime_nvn_cb_lifetime_selftest(void);
        if (!runtime_nvn_cb_lifetime_selftest()) {
            fprintf(stderr, "[totk-nvn] command-buffer lifetime selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-cb-lifetime\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-mrt")) {
        int runtime_nvn_mrt_selftest(const char *screenshot, int frames);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-mrt.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 2;
        if (!runtime_nvn_mrt_selftest(shot, frames)) {
            fprintf(stderr, "[totk-nvn] NVN MRT selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-mrt\","
               "\"frames\":%d,\"color0\":\"%s.c0.ppm\",\"color1\":\"%s.c1.ppm\"}\n",
               frames, shot, shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-segment-targets")) {
        int runtime_nvn_segment_selftest(const char *screenshot, int frames);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/nvn-segment.ppm", argv[1]);
        const char *fs = getenv("TOTK_ANDROID_NVN_DRAW_FRAMES");
        int frames = fs && *fs ? atoi(fs) : 1;
        if (!runtime_nvn_segment_selftest(shot, frames)) {
            fprintf(stderr, "[totk-nvn] NVN segment targets selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-segment-targets\","
               "\"frames\":%d,\"color0\":\"%s.c0.ppm\",\"color1\":\"%s.c1.ppm\"}\n",
               frames, shot, shot);
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-fence")) {
        int runtime_nvn_fence_selftest(void);
        if (!runtime_nvn_fence_selftest()) {
            fprintf(stderr, "[totk-nvn] NVN fence/map ordering selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-fence\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-vertex-state")) {
        int runtime_nvn_vertex_state_selftest(void);
        if (!runtime_nvn_vertex_state_selftest()) {
            fprintf(stderr, "[totk-nvn] NVN vertex-state/pool-inheritance selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-vertex-state\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-cb-call")) {
        int runtime_nvn_cb_call_selftest(void);
        if (!runtime_nvn_cb_call_selftest()) {
            fprintf(stderr, "[totk-nvn] NVN finalized-handle CallCommands selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-cb-call\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvn-pool-lifetime")) {
        int runtime_nvn_pool_lifetime_selftest(void);
        unsigned runtime_nvn_tex_pool_max(void);
        if (!runtime_nvn_pool_lifetime_selftest()) {
            fprintf(stderr, "[totk-nvn] pool lifetime selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvn-pool-lifetime\","
               "\"registrations\":40000,\"max_entries\":%u}\n", runtime_nvn_tex_pool_max());
        return 0;
    }
    if (!strcmp(argv[2], "probe-vi")) {
        int runtime_vi_selftest(void);
        if (!runtime_vi_selftest()) { fprintf(stderr, "[totk-vi] selftest FAIL\n"); return 1; }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vi-native-window\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-nvdrv")) {
        int runtime_nvdrv_selftest(void);
        if (!runtime_nvdrv_selftest()) { fprintf(stderr, "[totk-nvdrv] selftest FAIL\n"); return 1; }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"nvdrv\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-tls")) { tls_probe_mode = 1; return boot_sdk(); }
    if (!strcmp(argv[2], "boot-sdk")) return boot_sdk();
    runtime_fail("unknown mode");
}
