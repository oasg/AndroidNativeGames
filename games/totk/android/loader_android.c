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
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
            void *p = (r->kind == 1026 || !strcmp(s->name, "nnMain")) ? runtime_observer(s->name) : NULL;
            if (!p) p = runtime_override(s->name);
            if (p) p = runtime_hle_wrap_named(p, s->name);   /* host replacements run on the host stack */
            if (!p && s->defined) p = s->value < m->key_off ? m->base + s->value : NULL;
            if (!p) p = runtime_symbol(s->name);
            if (!p) {
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

/* Module-relative name for a crash address, with a host symbol fallback. */
static void describe_address(uint64_t addr, char *out, size_t cap) {
    for (unsigned i = 0; i < NMODULES; i++) {
        uintptr_t b = (uintptr_t)modules[i].base;
        if (addr >= b && addr < b + modules[i].size) {
            snprintf(out, cap, "%s+%#llx", modules[i].name, (unsigned long long)(addr - b));
            return;
        }
    }
    snprintf(out, cap, "0x%llx", (unsigned long long)addr);
}

/* Read guest memory from the signal handler without faulting again.  The guest
 * stacks are ordinary mappings of this process; /proc/self/mem turns a stale
 * frame pointer into a short read instead of a nested SIGSEGV. */
static int crash_read(uint64_t addr, void *out, size_t n) {
    static int mem = -2;
    if (mem == -2) mem = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (mem < 0) return 0;
    return pread(mem, out, n, (off_t)addr) == (ssize_t)n;
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *u = ctx;
    uint64_t pc = u->uc_mcontext.pc;
    uint64_t lr = u->uc_mcontext.regs[30];
    uint64_t sp = u->uc_mcontext.sp;
    uint64_t fp = u->uc_mcontext.regs[29];
    char pcd[128], lrd[128];
    describe_address(pc, pcd, sizeof pcd);
    describe_address(lr, lrd, sizeof lrd);
    fprintf(stderr, "[crash] sig=%d pc=%s lr=%s fault=%p sp=%#llx x8=%#llx\n",
            sig, pcd, lrd, si ? si->si_addr : NULL, (unsigned long long)sp,
            (unsigned long long)u->uc_mcontext.regs[8]);

    /* Guest frame-pointer chain: at x29 the pair is {caller fp, return address}. */
    uint64_t chain_fp = fp;
    if (chain_fp && !(chain_fp & 7)) {
        for (unsigned n = 0; n < 16 && chain_fp; n++) {
            uint64_t pair[2];
            if (!crash_read(chain_fp, pair, sizeof pair)) break;
            char fd[128];
            describe_address(pair[1], fd, sizeof fd);
            fprintf(stderr, "[crash] frame %u: fp=%#llx lr=%s\n",
                    n, (unsigned long long)chain_fp, fd);
            uint64_t next = pair[0];
            if (!next || (next & 7) || next <= chain_fp || next - chain_fp > (64u << 20)) break;
            chain_fp = next;
        }
    } else {
        char fd[128];
        describe_address(lr, fd, sizeof fd);
        fprintf(stderr, "[crash] frame 0: fp=%#llx lr=%s\n", (unsigned long long)fp, fd);
    }

    char hle[256];
    if (runtime_hle_last_call(hle, sizeof hle)) fprintf(stderr, "[crash] last HLE: %s\n", hle);
    else fprintf(stderr, "[crash] last HLE: none\n");

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

/* STOP_AFTER_INITS is a diagnostic for the constructor pass, so it implies INITS. */
static int inits_enabled(void) {
    return getenv("TOTK_ANDROID_INITS") || getenv("TOTK_ANDROID_STOP_AFTER_INITS");
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

static void module_inits(void) {
    if (inits_enabled()) {
        for (unsigned i = 0; i < NMODULES; i++) {
            fprintf(stderr, "[totk-android] ENTER %s DT_INIT\n", modules[i].name);
            ((void (*)(void))(modules[i].base + modules[i].init))();
            uint32_t state;
            memcpy(&state, modules[i].base + modules[i].ctor_flag, 4);
            fprintf(stderr, "[totk-android] RETURN %s DT_INIT state=%u init_array=%llu\n",
                    modules[i].name, state, (unsigned long long)(modules[i].init_size / 8));
            /* musl's __nnmusl_init_dso returns 2 after running .init_array and 1 when the
               module has no fini callback; both mean the constructor pass completed. */
            need(state == 1 || state == 2, "DT_INIT deferred or skipped constructors");
        }
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
