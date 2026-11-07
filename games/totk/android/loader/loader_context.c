/* Module: context. */
#include "loader_internal.h"

/* Minimal Android/arm64 loader for the rewritten TOTK modules.
 *
 * This is the first Android host milestone: map the verified images, fill the
 * Android key page, relocate imports, install the gate table, and run the
 * cross-module nn::os::GetSystemTick probe (main PLT -> sdk -> host timer gate).
 * The full HLE/service layer is intentionally not here yet.
 */


pthread_key_t runtime_keys[8];

static uint64_t svc_calls;

/* The app shell loads this code as a shared library and runs the loader on a
 * worker thread.  Global-dynamic TLS gives per-thread offsets, while the guest
 * gate (tpidr_el0 + fixed offset) assumes one static offset.  A per-thread
 * host-context table or a gate-visible helper is needed before the shared
 * library can run multi-threaded guest code; initial-exec attributes make
 * Android's dlopen reject the library outright. */
static __thread HostContext *g_host __attribute__((aligned(64)));

static pthread_once_t ipc_key_once = PTHREAD_ONCE_INIT;

/* Bionic implements pthread_getspecific(key) as __get_tls()[key], i.e. the
 * per-thread value lives at TPIDR_EL0 + key * sizeof(void*).  Keeping the
 * HostContext and GuestSlots pointers in two pthread keys therefore gives the
 * gate/guest a fixed offset that is identical on every thread, even when this
 * code is a dlopened shared library with global-dynamic __thread TLS. */
static pthread_key_t host_ctx_key;

static pthread_key_t guest_slots_key;

static pthread_once_t ctx_key_once = PTHREAD_ONCE_INIT;

/* Adreno's submit path runs on the guest stack (NVN procs are called directly,
 * not through the HLE gate).  Its frame can cross the top/bottom page of a
 * 1 MiB guest stack, so commit a generous read/write window around the real
 * SVC stack top. */
void runtime_thread_prepare_stack(uintptr_t top) {
    if (!top || top < (2u << 20)) return;
    uintptr_t lo = (top - (2u << 20)) & ~(uintptr_t)0xfff;
    size_t len = (2u << 20) + (64u << 10);
    if (mprotect((void *)lo, len, PROT_READ | PROT_WRITE) == 0)
        runtime_guest_commit(lo, len);
}

void ipc_key_init(void) { pthread_key_create(&runtime_keys[2], NULL); }

void ctx_key_init(void) {
    pthread_key_create(&host_ctx_key, NULL);
    pthread_key_create(&guest_slots_key, NULL);
}

void runtime_trace_guest(uint64_t fp, uint64_t lr) { (void)fp; (void)lr; }

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "[totk-android] FAIL: %s\n", message);
    exit(1);
}

void need(bool c, const char *m) { if (!c) runtime_fail(m); }

size_t page_size(void) {
    static size_t p;
    if (!p) p = (size_t)sysconf(_SC_PAGESIZE);
    return p;
}

size_t page_up(size_t x) { size_t p = page_size(); return (x + p - 1) & ~(p - 1); }

uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t runtime_now_ns(void) { return now_ns(); }

uintptr_t read_tpidr_el0(void) {
    uintptr_t p;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(p));
    return p;
}

void runtime_thread_set_guest_handle(uint32_t handle) {
    if (g_host && g_host->dummy && handle) {
        memcpy(g_host->dummy + 0x1b0, &handle, sizeof handle);
        if (getenv("TOTK_ANDROID_TLS_TRACE"))
            fprintf(stderr, "[totk-android] ThreadType %p thread_id=%#x\n", g_host->dummy, handle);
    }
}

void runtime_thread_init(void) {
    crash_altstack_install();
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
    uint64_t stack_index;
    uint64_t idx = __atomic_fetch_add(&stack_index, 1, __ATOMIC_RELAXED);
    const uint64_t area = 1u << 20;
    need(stack_size >= (idx + 1) * area, "guest stack area");
    uintptr_t area_base = (uintptr_t)(stack_base + stack_size - (idx + 1) * area);
    need(mprotect((void *)area_base, area, PROT_READ | PROT_WRITE) == 0,
         "guest stack mapping");
    runtime_guest_commit(area_base, area);
    /* TLS/TLR lives in a dedicated host mapping, not in the guest stack: the
     * guest can unmap a thread stack while the SDk still reads the TLS slot,
     * which produced sdk+0x126efc. */
    uint8_t *tls = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    need(tls != MAP_FAILED, "thread TLS mapping");
    hc->guest_tls = tls;
    hc->guest_tlr = hc->guest_tls + 0x2000;
    hc->dummy = hc->guest_tls + 0x3000;
    uint64_t tb = (uintptr_t)tls, ts = 0x4000;
    hc->guest_stack_top = area_base + area - 16;
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
    if (getenv("TOTK_ANDROID_TLS_TRACE") || getenv("TOTK_ANDROID_CTOR_PROBE") || tls_probe_mode)
        fprintf(stderr, "[totk-android] thread tls tlr=%p dummy=%p stack=%#llx+%#llx sp=%#llx\n",
                hc->guest_tlr, hc->dummy, (unsigned long long)area_base,
                (unsigned long long)area, (unsigned long long)hc->guest_stack_top);
    gs->ktls = (uintptr_t)hc->guest_tls;
    gs->ktlr = (uintptr_t)hc->guest_tlr;
    gs->reserved[0] = TLS_CANARY;   /* crash-dump canary: "TLSOK" */
    pthread_once(&ipc_key_once, ipc_key_init);
    pthread_setspecific(runtime_keys[2], hc->guest_tlr);
    /* Vulkan/Adreno submission and shader setup run inside HLE gates on this
     * stack.  1 MiB overflowed during the first 0x3f UI submit; the trace
     * printing only masked it by changing frame layout. */
    const size_t hs = 16u << 20;
    uint8_t *stack = mmap(NULL, hs, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    need(stack != MAP_FAILED, "host stack mmap");
    need(mprotect(stack, page_size(), PROT_NONE) == 0, "host stack guard");
    hc->host_stack_base = (uintptr_t)stack;
    hc->host_stack_top = (uintptr_t)stack + hs - 64;
    g_host = hc;
    g_slots = gs;
    pthread_once(&ctx_key_once, ctx_key_init);
    pthread_setspecific(host_ctx_key, hc);
    pthread_setspecific(guest_slots_key, gs);
    uintptr_t tp = read_tpidr_el0();
    /* key * sizeof(void*) is the fixed TCB slot offset.  Verify the slot
     * really contains the pointers we just stored before the gate uses it. */
    uint64_t ho = (uint64_t)host_ctx_key * sizeof(void *);
    uint64_t so = (uint64_t)guest_slots_key * sizeof(void *);
    uintptr_t slot_hc = *(uintptr_t *)(tp + ho);
    uintptr_t slot_gs = *(uintptr_t *)(tp + so);
    fprintf(stderr, "[totk-android] tls tp=%#llx host_key=%d off=%#llx slot=%#llx"
                    " guest_key=%d off=%#llx slot=%#llx\n",
            (unsigned long long)tp, (int)host_ctx_key, (unsigned long long)ho,
            (unsigned long long)slot_hc, (int)guest_slots_key,
            (unsigned long long)so, (unsigned long long)slot_gs);
    need(slot_hc == (uintptr_t)hc && slot_gs == (uintptr_t)gs,
         "pthread key TCB slot layout differs");
    if (!runtime_host_ctx_off) runtime_host_ctx_off = ho;
    else need(ho == runtime_host_ctx_off, "per-thread host slot changed");
    if (!runtime_guest_slots_off) runtime_guest_slots_off = so;
    else need(so == runtime_guest_slots_off, "per-thread guest slot changed");
}

uintptr_t runtime_guest_stack_top(void) { return g_host ? g_host->guest_stack_top : 0; }

void runtime_hle_stack_set(uintptr_t top) { (void)top; }

void runtime_hle_stack_init(uint64_t off) { (void)off; }

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
