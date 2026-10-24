/* Minimal Android/arm64 loader for the rewritten TOTK modules.
 *
 * This is the first Android host milestone: map the verified images, fill the
 * Android key page, relocate imports, install the gate table, and run the
 * cross-module nn::os::GetSystemTick probe (main PLT -> sdk -> host timer gate).
 * The full HLE/service layer is intentionally not here yet.
 */
#include "runtime.h"
#include "sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
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
    uintptr_t host_stack_base;
    uint8_t *guest_tls;         /* guest TPIDR_EL0 value (zeroed 8 KiB) */
    uint8_t *guest_tlr;         /* guest TPIDRRO_EL0 value (zeroed 0x200) */
    uint8_t *dummy;
} HostContext;

typedef struct {
    uint64_t k18, ktls, ktlr, reserved[5];
} GuestSlots;

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

void runtime_thread_init(void) {
    if (g_host) return;
    HostContext *hc = calloc(1, sizeof *hc);
    GuestSlots *gs = calloc(1, sizeof *gs);
    need(hc && gs, "thread context allocation");
    hc->guest_tls = calloc(1, 8192);
    hc->guest_tlr = calloc(1, 0x200);
    hc->dummy = calloc(1, 0x400);
    need(hc->guest_tls && hc->guest_tlr && hc->dummy, "guest TLS/TLR allocation");
    memcpy(hc->guest_tlr + 0x1f8, &hc->dummy, 8);
    gs->ktls = (uintptr_t)hc->guest_tls;
    gs->ktlr = (uintptr_t)hc->guest_tlr;
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
            void *p = r->kind == 1026 ? runtime_observer(s->name) : NULL;
            if (!p && s->defined) p = s->value < m->key_off ? m->base + s->value : NULL;
            if (!p) p = runtime_symbol(s->name);
            if (!p) {
                p = runtime_override(s->name);
                if (!p) p = runtime_hle_stub(s->name);
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

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    (void)si;
    ucontext_t *u = ctx;
    uintptr_t pc = u->uc_mcontext.pc, lr = u->uc_mcontext.regs[30], sp = u->uc_mcontext.sp;
    const char *mn = "?";
    uint64_t off = 0;
    for (unsigned i = 0; i < NMODULES; i++) {
        uintptr_t b = (uintptr_t)modules[i].base;
        if (pc >= b && pc < b + modules[i].size) { mn = modules[i].name; off = pc - b; break; }
    }
    fprintf(stderr, "[totk-android] CRASH sig=%d pc=%s+%#llx lr=%#llx sp=%#llx x0=%#llx x1=%#llx x8=%#llx\n",
            sig, mn, (unsigned long long)off, (unsigned long long)lr, (unsigned long long)sp,
            (unsigned long long)u->uc_mcontext.regs[0], (unsigned long long)u->uc_mcontext.regs[1],
            (unsigned long long)u->uc_mcontext.regs[8]);
    _exit(128 + sig);
}

static uint64_t host_add(uint64_t a, uint64_t b) { return a + b; }

static void loader_ready(void) {
    printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"sdk-start\",\"stage\":\"loader-ready\"}\n");
    fflush(stdout);
    if (!getenv("TOTK_ANDROID_INITS")) _exit(0);
}

static void module_inits(void) {
    if (getenv("TOTK_ANDROID_INITS")) {
        for (unsigned i = 0; i < NMODULES; i++) {
            fprintf(stderr, "[totk-android] ENTER %s DT_INIT\n", modules[i].name);
            ((void (*)(void))(modules[i].base + modules[i].init))();
            fprintf(stderr, "[totk-android] RETURN %s DT_INIT\n", modules[i].name);
        }
    }
    printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"sdk-start\",\"stage\":\"module-inits\"}\n");
    fflush(stdout);
    _exit(0);
}

static void module_finis(void) { }

static int boot_sdk(void) {
    uint32_t main_handle = svc_thread_main_init();
    void *start = runtime_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    need(start != NULL, "nn::init::Start symbol");
    fprintf(stderr, "[totk-android] ENTER nn::init::Start\n");
    ((void (*)(uint64_t, uint64_t, void (*)(void), void (*)(void), void (*)(void)))start)
        (main_handle, 0, loader_ready, module_inits, module_finis);
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
    if (!strcmp(argv[2], "boot-sdk")) return boot_sdk();
    runtime_fail("unknown mode");
}
