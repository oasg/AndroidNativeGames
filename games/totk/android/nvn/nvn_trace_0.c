/* NVN module: trace_0. */
#include "nvn_internal.h"

int nvn_crash_read_mem(uintptr_t addr, void *out, size_t n) {
    int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t got = pread(fd, out, n, (off_t)addr);
    close(fd);
    return got == (ssize_t)n;
}

uint64_t nvn_crash_u64(uintptr_t addr) {
    uint64_t v = 0;
    if (!nvn_crash_read_mem(addr, &v, sizeof v)) return 0;
    return v;
}

void nvn_crash_x8_dump(uint64_t x8) {
    if (!x8) return;
    uint64_t w[8] = {0};
    if (!nvn_crash_read_mem((uintptr_t)x8, w, sizeof w)) return;
    dprintf(2, "[guest-x8] %#llx: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
            (unsigned long long)x8, (unsigned long long)w[0], (unsigned long long)w[1],
            (unsigned long long)w[2], (unsigned long long)w[3], (unsigned long long)w[4],
            (unsigned long long)w[5], (unsigned long long)w[6], (unsigned long long)w[7]);
    uint64_t p = nvn_crash_u64((uintptr_t)x8 + 0x30);
    uint64_t c = nvn_crash_u64((uintptr_t)x8 + 0x28);
    dprintf(2, "[guest-x8] +0x28=%#llx +0x30=%#llx\n",
            (unsigned long long)c, (unsigned long long)p);
    if (p) {
        uint64_t q[6] = {0};
        if (nvn_crash_read_mem((uintptr_t)p, q, sizeof q))
            dprintf(2, "[guest-x8] data %#llx: %#llx %#llx %#llx %#llx %#llx %#llx\n",
                    (unsigned long long)p, (unsigned long long)q[0], (unsigned long long)q[1],
                    (unsigned long long)q[2], (unsigned long long)q[3],
                    (unsigned long long)q[4], (unsigned long long)q[5]);
    }
}

void nvn_host_crash_raw(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *u = ctx;
    uintptr_t pc = 0, lr = 0, sp = 0;
    uint64_t gfp = 0, glr = 0;
#if defined(__aarch64__)
    pc = (uintptr_t)u->uc_mcontext.pc;
    lr = (uintptr_t)u->uc_mcontext.regs[30];
    sp = (uintptr_t)u->uc_mcontext.sp;
    gfp = u->uc_mcontext.regs[29];
    glr = u->uc_mcontext.regs[30];
#endif
    dprintf(2, "[host-crash] raw sig=%d pc=%p lr=%p sp=%p fault=%p\n",
            sig, (void *)pc, (void *)lr, (void *)sp,
            si ? si->si_addr : NULL);
    if (getenv("TOTK_ANDROID_CRASH_X8")) {
        uint64_t x8 = 0;
#if defined(__aarch64__)
        x8 = u->uc_mcontext.regs[8];
#endif
        nvn_crash_x8_dump(x8);
    }
    Dl_info di;
    uintptr_t base = 0;
    const char *name = "?";
    if (dladdr((void *)pc, &di) && di.dli_fbase) {
        base = (uintptr_t)di.dli_fbase;
        name = di.dli_fname ? di.dli_fname : "?";
    }
    dprintf(2, "[host-crash] sig=%d pc=%p lr=%p sp=%p fault=%p base=%p off=%#lx name=%s\n",
            sig, (void *)pc, (void *)lr, (void *)sp,
            si ? si->si_addr : NULL, (void *)base,
            (unsigned long)(pc - base), name);
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        static char maps[16384];
        ssize_t n = read(fd, maps, sizeof maps - 1);
        close(fd);
        if (n > 0) {
            maps[n] = 0;
            char *line = maps;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                unsigned long long a = 0, b = 0, off = 0;
                char perms[8] = {0}, path[256] = {0};
                if (sscanf(line, "%llx-%llx %7s %llx %*s %*s %255s",
                           &a, &b, perms, &off, path) >= 4 &&
                    pc >= a && pc < b)
                    dprintf(2, "[host-crash] map=%s pc_off=%#llx\n", line,
                            (unsigned long long)(pc - a));
                line = nl ? nl + 1 : NULL;
            }
        }
    }
    runtime_guest_frame_report(gfp, glr);
    _exit(139);
}

void nvn_install_host_crash_late(void) {
    const char *e = getenv("TOTK_ANDROID_HOST_CRASH");
    if (!e || !*e) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = nvn_host_crash_raw;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

void nvn_api(const char *what) {
    g_last_api = what;
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] api %s thread=%#x\n", what, runtime_thread_current_handle());
}

const char *runtime_nvn_last_api(void) { return g_last_api; }

const char *runtime_nvn_last_proc(void) { return g_last_proc; }

void nvn_fail(const char *what, const char *detail) {
    if (nvn_fail_count++ < 48)
        fprintf(stderr, "[totk-nvn] ERROR %s%s%s\n", what,
                detail && *detail ? ": " : "", detail ? detail : "");
}

int nvn_trace(void) {
    static int on = -1;
    if (on < 0) on = getenv("TOTK_ANDROID_TRACE_NVN") != NULL;
    return on;
}

/* Command-buffer lifetime trace for the first real frame: who recorded a
 * handle, which handles a submit names, and which recorded buffers were
 * dropped without ever being submitted.  Deliberately separate from the
 * per-call TRACE_NVN firehose. */
int nvn_trace_frame(void) {
    static int on = -1;
    if (on < 0) on = getenv("TOTK_ANDROID_TRACE_FRAME") != NULL;
    return on;
}

/* Focused first-frame state trace: records the command-buffer sequence that
 * registers build a first draw from, without the full per-call NVN trace. */
int nvn_trace_first(void) {
    static int on = -1;
    if (on < 0) on = getenv("TOTK_ANDROID_TRACE_FIRST") != NULL;
    return on;
}

int nvn_obj(const void *p, uint32_t magic, const char *what) {
    if (!p) { nvn_fail(what, "null object"); return 0; }
    if (*(const uint32_t *)p != magic) {
        char d[80];
        snprintf(d, sizeof d, "bad magic %#x (expected %#x)",
                 *(const uint32_t *)p, magic);
        nvn_fail(what, d);
        return 0;
    }
    return 1;
}

void nvn_dump_vertex_stream(const char *tag, const GfxVertexLayout *l,
                                   const nvn_bind_state_t *bv, int maxv) {
    if (!tag || !l || !bv) return;
    fprintf(stderr, "[totk-ui-seg] %s layout bindings=%u attrs=%u", tag,
            l->nbindings, l->nattributes);
    for (uint32_t i = 0; i < l->nbindings; i++)
        fprintf(stderr, " b%u(stride=%u,rate=%u)", l->bindings[i].binding,
                l->bindings[i].stride, l->bindings[i].input_rate);
    for (uint32_t i = 0; i < l->nattributes; i++)
        fprintf(stderr, " a%u(loc=%u,bind=%u,fmt=%#x,off=%u)",
                i, l->attributes[i].location, l->attributes[i].binding,
                l->attributes[i].format, l->attributes[i].offset);
    fputc('\n', stderr);
    uint32_t stride = l->nbindings ? l->bindings[0].stride : 0;
    if (!stride || !(bv->vb_mask & 1u) || !bv->vb[0].address) return;
    uint32_t nv = (uint32_t)(bv->vb[0].size / stride);
    if (nv > (uint32_t)maxv) nv = (uint32_t)maxv;
    const uint8_t *p = (const uint8_t *)(uintptr_t)bv->vb[0].address;
    for (uint32_t v = 0; v < nv; v++) {
        const uint16_t *h = (const uint16_t *)(p + (size_t)v * stride);
        fprintf(stderr, "[totk-ui-seg] %s v%u: x=%.4f y=%.4f z=%.4f w=%.4f"
                        " | u=%.4f v=%.4f\n", tag, v,
                nvn_half_to_float(h[0]), nvn_half_to_float(h[1]),
                nvn_half_to_float(h[2]), nvn_half_to_float(h[3]),
                nvn_half_to_float(h[4]), nvn_half_to_float(h[5]));
    }
}

void nvn_orphan_add(const cb_t *c, const cb_meta_t *m) {
    unsigned slot = g_orphan_next++ % NVN_ORPHAN_MAX;
    g_orphans[slot].handle = c->handle;
    g_orphans[slot].cb = c;
    g_orphans[slot].thread = m ? m->record_thread : 0;
    g_orphans[slot].commands = m ? m->record_commands : c->ncmds;
    g_orphans[slot].draws = (uint32_t)c->draw_count;
    g_orphans[slot].lr = m ? m->record_lr : 0;
    g_orphans[slot].reported = 0;
    if (g_orphan_count < NVN_ORPHAN_MAX) g_orphan_count++;
    if (nvn_trace_frame())
        fprintf(stderr, "[totk-frame] ORPHAN cb=%p handle=%#llx thread=%#x cmds=%u draws=%d lr=%#llx (reinitialised)\n",
                (void *)c, (unsigned long long)c->handle, m ? m->record_thread : 0,
                m ? m->record_commands : c->ncmds, c->draw_count, m ? (unsigned long long)m->record_lr : 0);
}

void nvn_cbacct_log(const char *why) {
    nvn_cbacct_t a;
    pthread_mutex_lock(&g_cbacct_lock);
    a = g_cbacct;
    pthread_mutex_unlock(&g_cbacct_lock);
    fprintf(stderr, "[totk-cbacct] %s recorded=%u finalized=%u submitted=%u retired=%u"
                    " call_cmds=%u unsubmitted=%u peak_inflight=%u | draws_buf=%u gpu_buf=%u"
                    " mask_buf=%u\n", why ? why : "?", a.recorded, a.finalized, a.submitted,
            a.retired, a.call_cmds, a.unsubmitted, a.peak_inflight, a.with_draws,
            a.with_gpu, a.with_mask_target);
}

void nvn_cbacct_bump(int which) {
    pthread_mutex_lock(&g_cbacct_lock);
    switch (which) {
    case 0: g_cbacct.recorded++; if (g_cbacct.recorded > g_cbacct.peak_inflight)
                g_cbacct.peak_inflight = g_cbacct.recorded; break;
    case 1: g_cbacct.finalized++; break;
    case 2: g_cbacct.submitted++; break;
    case 3: g_cbacct.retired++; break;
    case 4: g_cbacct.call_cmds++; break;
    case 5: g_cbacct.with_draws++; break;
    case 6: g_cbacct.unsubmitted++; break;
    case 7: g_cbacct.with_mask_target++; break;
    default: break;
    }
    pthread_mutex_unlock(&g_cbacct_lock);
}

int nvn_orphan_seen(uint64_t handle) {
    for (unsigned i = 0; i < g_orphan_count; i++)
        if (g_orphans[i].handle == handle) return 1;
    return 0;
}

void nvn_trace_bad_target(const char *what, uint32_t index, const void *p) {
    if (!getenv("TOTK_ANDROID_TRACE_TARGET")) return;
    static unsigned logged;
    if (logged++ >= 64) return;
    tex_t raw;
    memset(&raw, 0, sizeof raw);
    int read = p && nvn_crash_read_mem((uintptr_t)p, &raw, sizeof raw);
    fprintf(stderr, "[totk-target] bad %s[%u] p=%p read=%d magic=%#x serial=%#llx"
                    " fmt=%#x %ux%u tgt=%d levels=%u mem=%p pool=%p off=%#llx\n",
            what, index, p, read, read ? raw.magic : 0,
            read ? (unsigned long long)raw.serial : 0ull,
            read ? (unsigned)raw.d.format : 0, read ? raw.d.width : 0,
            read ? raw.d.height : 0, read ? raw.d.target : 0,
            read ? raw.d.levels : 0, read ? (void *)raw.mem : NULL,
            read ? (void *)raw.pool : NULL, read ? (long long)raw.off : 0ll);
    if (!read) return;
    pthread_mutex_lock(&g_tex_obj_lock);
    unsigned shown = 0;
    for (unsigned i = 0; i < g_tex_obj_count && shown < 8; i++) {
        const tex_t *t = g_tex_objs[i];
        int serial_match = raw.serial && t->serial == raw.serial;
        int field_match = t->d.width == raw.d.width && t->d.height == raw.d.height &&
                          t->d.format == raw.d.format && t->mem == raw.mem;
        if (!serial_match && !field_match) continue;
        fprintf(stderr, "[totk-target] candidate reg=%p serial=%#llx match=%s%s"
                        " fmt=%#x %ux%u mem=%p\n", (void *)t,
                (unsigned long long)t->serial, serial_match ? "serial" : "",
                field_match ? (serial_match ? "+fields" : "fields") : "",
                (unsigned)t->d.format, t->d.width, t->d.height, (void *)t->mem);
        shown++;
    }
    fprintf(stderr, "[totk-target] registry=%u\n", g_tex_obj_count);
    pthread_mutex_unlock(&g_tex_obj_lock);
}

/* --- Lane 2 scene-segment diagnostics ---------------------------------
 * A UI recording is split into segments; every segment that draws has to run,
 * in recording order, or the later segments sample a target nothing wrote.
 * Report, per segment, exactly why it is or is not executed. */
int nvn_trace_ui_segs(void) {
    static int on = -1;
    if (on < 0) on = getenv("TOTK_ANDROID_TRACE_UI_SEGS") != NULL;
    return on;
}

/* First-frame diagnostics: one line per bound target/binding with the NVN
 * descriptor fields needed to describe the frame (format, kind, size, layers,
 * mips, samples).  Only runs with TOTK_ANDROID_TRACE_NVN. */
__attribute__((unused)) static void nvn_trace_tex(const char *what, const tex_t *t) {
    if (!t || t->magic != NVN_MAGIC('T', 'E', 'X', 0)) {
        fprintf(stderr, "[totk-nvn] %s texture=NULL\n", what);
        return;
    }
    fprintf(stderr, "[totk-nvn] %s texture=%p target=%d format=%#x %ux%ux%u layers=%u mips=%u samples=%d\n",
            what, (const void *)t, t->d.target, t->d.format, t->d.width, t->d.height,
            t->d.depth ? t->d.depth : 1u, t->has_layout ? t->L.layers : 1u,
            t->d.levels, t->d.samples);
}

/* Optional offline capture: write the exact control/code pair the game binds
 * so a host-side totk_xlate run can produce its SPIR-V.  Game-derived bytes
 * stay in TOTK_ANDROID_SHADER_DUMP, normally under /data/local/tmp. */
void nvn_dump_shader(const RuntimeShaderIdentity *id, const void *control,
                            const void *code) {
    const char *dir = getenv("TOTK_ANDROID_SHADER_DUMP");
    if (!dir || !*dir || !id || !control || !code) return;
    if (!id->control_key_bytes || !id->code_bytes) return;
    char path[RUNTIME_SHADER_PATH_MAX];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s.ctl", dir, id->key);
    if (stat(path, &st) == 0) return;
    mkdir(dir, 0755);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    if (fwrite(control, 1, id->control_key_bytes, f) != id->control_key_bytes) {
        fclose(f); unlink(path); return;
    }
    fclose(f);
    snprintf(path, sizeof path, "%s/%s.code", dir, id->key);
    f = fopen(path, "wb");
    if (!f) return;
    if (fwrite(code, 1, id->code_bytes, f) != id->code_bytes) {
        fclose(f); unlink(path); return;
    }
    fclose(f);
    if (getenv("TOTK_ANDROID_TRACE_NVN"))
        fprintf(stderr, "[totk-nvn] dumped shader %s %s ctl=%u code=%u\n",
                runtime_shader_stage_name(id->stage), id->key,
                id->control_key_bytes, id->code_bytes);
}

/* Is [addr, addr+size) inside a known guest memory pool?  Used before reading
 * past a bound vertex buffer: the ring can be larger than the bound range, but
 * reading beyond the mapping must never happen. */
int nvn_guest_range_ok(uint64_t addr, size_t size) {
    int ok = 0;
    if (!addr || !size) return 0;
    pthread_mutex_lock(&g_pool_epoch_lock);
    for (unsigned i = 0; i < g_pool_epoch_count && !ok; i++) {
        const pool_t *pp = g_pool_epochs[i].p;
        if (!pp || pp->magic != NVN_MAGIC('P', 'O', 'O', 'L') || !pp->mem) continue;
        uint64_t base = (uint64_t)(uintptr_t)pp->mem;
        if (addr >= base && size <= pp->size && addr - base <= pp->size - size) ok = 1;
    }
    pthread_mutex_unlock(&g_pool_epoch_lock);
    return ok;
}

int nvn_trace_inputs(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("TOTK_ANDROID_TRACE_INPUTS");
        enabled = e && *e;
    }
    return enabled;
}

int nvn_trace_save_ui(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("TOTK_ANDROID_TRACE_SAVE_UI");
        enabled = e && *e;
    }
    return enabled;
}

int nvn_trace_composite(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("TOTK_ANDROID_TRACE_COMPOSITE");
        enabled = e && *e;
    }
    return enabled;
}

uint32_t nvn_first_nonzero(const uint8_t *p, size_t n, size_t cap) {
    if (!p) return UINT32_MAX;
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) if (p[i]) return (uint32_t)i;
    return UINT32_MAX;
}
