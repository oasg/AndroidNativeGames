/* NVN module: queue_0. */
#include "nvn_internal.h"

/* ------------------------------------------------------------------ */
/* Queue                                                               */

void nvnQueueBuilderSetDefaults(queue_builder_t *b) {
    if (!b) return;
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->cmd_sz = 65536; b->comp_sz = 262144; b->ctl_sz = 16384; b->flush = 4096;
}

void nvnQueueBuilderSetDevice(queue_builder_t *b, void *d) { if (b) b->dev = d; }

void nvnQueueBuilderSetFlags(queue_builder_t *b, int f) { if (b) b->flags = (uint32_t)f; }

void nvnQueueBuilderSetCommandMemorySize(queue_builder_t *b, size_t n) { if (b) b->cmd_sz = n; }

void nvnQueueBuilderSetComputeMemorySize(queue_builder_t *b, size_t n) { if (b) b->comp_sz = n; }

void nvnQueueBuilderSetControlMemorySize(queue_builder_t *b, size_t n) { if (b) b->ctl_sz = n; }

void nvnQueueBuilderSetCommandFlushThreshold(queue_builder_t *b, size_t n) { if (b) b->flush = n; }

void nvnQueueBuilderSetQueuePriority(queue_builder_t *b, int p) { if (b) b->priority = (uint32_t)p; }

int nvnQueueBuilderGetQueuePriority(const queue_builder_t *b) { return b ? (int)b->priority : 0; }

size_t nvnQueueBuilderGetQueueMemorySize(const queue_builder_t *b) {
    if (!b) return 0;
    return (size_t)((b->cmd_sz + b->comp_sz + b->ctl_sz + 4095) & ~(uint64_t)4095);
}

void nvnQueueBuilderSetQueueMemory(queue_builder_t *b, void *m, size_t n) {
    if (b) { b->mem = m; b->mem_sz = n; }
}

void *nvnQueueBuilderGetMemory(const queue_builder_t *b) { return b ? b->mem : NULL; }

size_t nvnQueueBuilderGetMemorySize(const queue_builder_t *b) { return b ? (size_t)b->mem_sz : 0; }

void *nvnQueueBuilderGetDevice(const queue_builder_t *b) { return b ? b->dev : NULL; }

int nvnQueueBuilderGetFlags(const queue_builder_t *b) { return b ? (int)b->flags : 0; }

size_t nvnQueueBuilderGetCommandMemorySize(const queue_builder_t *b) { return b ? (size_t)b->cmd_sz : 0; }

size_t nvnQueueBuilderGetComputeMemorySize(const queue_builder_t *b) { return b ? (size_t)b->comp_sz : 0; }

size_t nvnQueueBuilderGetControlMemorySize(const queue_builder_t *b) { return b ? (size_t)b->ctl_sz : 0; }

size_t nvnQueueBuilderGetCommandFlushThreshold(const queue_builder_t *b) { return b ? (size_t)b->flush : 0; }

uint32_t nvnQueueInitialize(queue_t *q, const queue_builder_t *b) { nvn_api("nvnQueueInitialize");
    if (!q || !b) { nvn_fail("nvnQueueInitialize", "null queue/builder"); return 0; }
    memset(q, 0, sizeof *q);
    q->magic = NVN_MAGIC('Q', 'U', 'E', 'U');
    q->dev = b->dev;
    if (getenv("TOTK_ANDROID_TRACE_NVN"))
        fprintf(stderr, "[totk-nvn] QueueInitialize %p dev=%p memory=%p+%#llx\n",
                (void *)q, b->dev, b->mem, (unsigned long long)b->mem_sz);
    return 1;
}

void nvnQueueFinalize(queue_t *q) { if (q) q->magic = 0; }

void nvnQueueSetDebugLabel(queue_t *q, const char *l) { (void)q; (void)l; }

int nvn_gpu_drain(const char *what) {
    if (!nvn_gpu_pending_work) return 1;
    if (!gfx_backend_vulkan_available()) { nvn_gpu_pending_work = 0; return 1; }
    if (!getenv("TOTK_ANDROID_NO_QUEUE_IDLE") && !gfx_backend_queue_idle()) {
        char d[96];
        snprintf(d, sizeof d, "%s: GPU work did not reach idle", what);
        nvn_fail(what, d);
        return 0;
    }
    nvn_gpu_pending_work = 0;
    return 1;
}

void nvnQueueSubmitCommands(queue_t *q, int n, const uint64_t *cmds) { nvn_api("nvnQueueSubmitCommands");
    if (nvn_trace_frame())
        fprintf(stderr, "[totk-frame] submit-call thread=%#x queue=%p n=%d h0=%#llx\n",
                runtime_thread_current_handle(), (void *)q, n,
                n > 0 && cmds ? (unsigned long long)cmds[0] : 0);
    if (!nvn_obj(q, NVN_MAGIC('Q', 'U', 'E', 'U'), "nvnQueueSubmitCommands")) return;
    if (!cmds && n) { nvn_fail("nvnQueueSubmitCommands", "null command handle array"); return; }
    q->submits += (uint64_t)n;
    {
        static int on = -1;
        if (on < 0) on = getenv("TOTK_ANDROID_TRACE_CB_FLOW") != NULL;
        if (on) {
            static unsigned shown;
            if (shown++ < 240) {
                fprintf(stderr, "[totk-cb] submit thread=%#x n=%d", runtime_thread_current_handle(), n);
                for (int i = 0; i < n; i++) fprintf(stderr, " %#llx", (unsigned long long)cmds[i]);
                fputc('\n', stderr);
            }
        }
    }
    nvn_frame_submit_trace(n, cmds);
    nvn_frame_ubo_trace(n, cmds);
    unsigned total = __atomic_add_fetch(&nvn_gpu_submits, (unsigned)n, __ATOMIC_RELAXED);
    if (total <= 8 || getenv("TOTK_ANDROID_TRACE_NVN"))
        fprintf(stderr, "[totk-nvn] GPU submit #%u queue=%p commands=%d (CPU-only host)\n",
                total, (void *)q, n);
    nvn_scene_submit_pending();
    nvn_gpu_pending_work = 1;
    nvn_process_submit_commands(cmds, n);
    if (!nvn_gpu_drain("nvnQueueSubmitCommands"))
        fprintf(stderr, "[totk-nvn] ERROR submit returned before GPU completion\n");
}

void nvnQueueFlush(queue_t *q) { (void)q; }

void nvnQueueFinish(queue_t *q) { (void)q; (void)nvn_gpu_drain("nvnQueueFinish"); }

void nvnQueuePresentTexture(queue_t *q, win_t *w, int index) { nvn_api("nvnQueuePresentTexture");
    if (!nvn_obj(q, NVN_MAGIC('Q', 'U', 'E', 'U'), "nvnQueuePresentTexture")) return;
    if (!nvn_obj(w, NVN_MAGIC('W', 'I', 'N', 'D'), "nvnQueuePresentTexture(window)")) return;
    (void)index;
    w->presents++;
    unsigned total = __atomic_add_fetch(&nvn_gpu_presents, 1, __ATOMIC_RELAXED);
    if (total <= 4)
        fprintf(stderr, "[totk-nvn] present #%u window=%p index=%d (host backend)\n",
                total, (void *)w, index);
    if ((total % 60) == 0) nvn_frame_report_orphans();
    if ((total % 600) == 0 && (nvn_trace_composite() ||
                               getenv("TOTK_ANDROID_TRACE_CBACCT")))
        nvn_cbacct_log("present");
    nvn_gpu_pending_work = 1;
    if (w->native && !runtime_native_window_present(w->native))
        nvn_fail("nvnQueuePresentTexture", "native window present failed");
}

int nvnQueueGetError(queue_t *q, void *info) {
    (void)q; if (info) memset(info, 0, 0x40); return 0;
}

size_t nvnQueueGetTotalCommandMemoryUsed(queue_t *q) { return q ? (size_t)q->cmd_used : 0; }

size_t nvnQueueGetTotalControlMemoryUsed(queue_t *q) { return q ? (size_t)q->ctl_used : 0; }

size_t nvnQueueGetTotalComputeMemoryUsed(queue_t *q) { return q ? (size_t)q->comp_used : 0; }

void nvnQueueResetMemoryUsageCounts(queue_t *q) {
    if (q) q->cmd_used = q->comp_used = q->ctl_used = 0;
}

int nvnQueueWaitSync(queue_t *q, const sync_t *s) { nvn_api("nvnQueueWaitSync");
    if (!nvn_obj(q, NVN_MAGIC('Q', 'U', 'E', 'U'), "nvnQueueWaitSync")) return 0;
    if (!s || s->magic != NVN_MAGIC('S', 'Y', 'N', 'C')) {
        nvn_fail("nvnQueueWaitSync", "invalid sync");
        return 0;
    }
    int r = nvnSyncWait(s, ~0ull);
    return r == NVN_SYNC_WAIT_CONDITION_SATISFIED || r == NVN_SYNC_WAIT_ALREADY_SIGNALED;
}

int nvnQueueAcquireTexture(queue_t *q, win_t *w, int *index) {
    if (!nvn_obj(q, NVN_MAGIC('Q', 'U', 'E', 'U'), "nvnQueueAcquireTexture")) return 1;
    if (!nvn_obj(w, NVN_MAGIC('W', 'I', 'N', 'D'), "nvnQueueAcquireTexture(window)")) return 1;
    if (!index) { nvn_fail("nvnQueueAcquireTexture", "null index"); return 1; }
    /* The texture is renderable only after the queued GPU work is done. */
    if (!nvn_gpu_drain("nvnQueueAcquireTexture")) return 1;
    *index = w->ntex ? (w->next++ % w->ntex) : 0;
    return 0;
}

/* Per-target draw histogram for a retired recording.  TOTK's large UI/atlas
 * command buffers set a 1920x1080 0x3f colour target but draw only to small
 * atlas textures; knowing which target actually received the draws avoids
 * treating the cleared 0x3f target as a rendered scene pass. */
void nvn_frame_target_hist(const cb_t *c, const void *chosen) {
    nvn_cb_target_t *t = nvn_cb_target(c, 0);
    if (!t || !t->draws) return;
    struct { const void *tex; uint32_t fmt, w, h, n; } g[8];
    unsigned ng = 0;
    for (uint32_t i = 0; i < t->ndraws; i++) {
        const nvn_draw_snap_t *d = &t->draws[i];
        if (!d->have_layout || !d->color0_tex) continue;
        unsigned k;
        for (k = 0; k < ng; k++)
            if (g[k].tex == d->color0_tex && g[k].fmt == (uint32_t)d->layout.color_formats[0] &&
                g[k].w == d->width && g[k].h == d->height) break;
        if (k == ng && ng < sizeof g / sizeof g[0]) {
            g[ng].tex = d->color0_tex;
            g[ng].fmt = (uint32_t)d->layout.color_formats[0];
            g[ng].w = d->width;
            g[ng].h = d->height;
            g[ng].n = 0;
            ng++;
        }
        if (k < ng) g[k].n++;
    }
    fprintf(stderr, "[totk-frame] draw targets:");
    for (unsigned k = 0; k < ng; k++)
        fprintf(stderr, " %p vkfmt=%#x %ux%u draws=%u%s", g[k].tex, g[k].fmt, g[k].w,
                g[k].h, g[k].n, g[k].tex == chosen ? "*" : "");
    fputc('\n', stderr);
}

void nvn_frame_draw_trace(cb_t *c, const char *what, int count) {
    if (!nvn_trace_frame() || !c) return;
    cb_meta_t *m = nvn_cb_meta(c, 1);
    if (m) m->rt_draws++;
    static unsigned logged;
    if (logged++ >= 64 && (!m || m->rt_nvn_format != 0x3f)) return;
    fprintf(stderr, "[totk-frame] draw cb=%p thread=%#x what=%s count=%d rt0=%p fmt=%#x"
                    " %ux%u draws=%u\n",
            (void *)c, runtime_thread_current_handle(), what, count,
            m ? m->rt_color0 : NULL, m ? m->rt_nvn_format : 0,
            m ? m->rt_width : 0, m ? m->rt_height : 0, m ? m->rt_draws : 0);
}

void nvn_frame_ubo_trace(int n, const uint64_t *cmds) {
    static unsigned reports;
    if (!nvn_trace_frame() || !(reports < 8 || (reports % 300) == 0)) return;
    reports++;
    for (int i = 0; i < n; i++) {
        cb_t *c = nvn_cb_by_handle(cmds[i]);
        if (!c || !c->draw_count || !c->bind_state) continue;
        const nvn_bind_state_t *b = c->bind_state;
        for (int st = 0; st < 6; st++) for (int s = 0; s < 16; s++) {
            if (!(b->ubo_mask[st] & (1u << s))) continue;
            uint64_t addr = b->ubo[st][s].address;
            size_t size = b->ubo[st][s].size;
            uint32_t first[4] = {0};
            if (addr && size >= sizeof first) memcpy(first, (const void *)(uintptr_t)addr, sizeof first);
            int nz = 0; size_t nz_off = 0;
            const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
            for (size_t k = 0; addr && k < size; k++) if (p[k]) { nz = 1; nz_off = k; break; }
            fprintf(stderr, "[totk-frame] ubo cb=%p stage=%d slot=%d addr=%#llx size=%zu nonzero=%d off=%zu"
                            " first=%08x %08x %08x %08x\n",
                    (void *)c, st, s, (unsigned long long)addr, size, nz, nz_off,
                    first[0], first[1], first[2], first[3]);
        }
    }
}

void nvn_frame_submit_trace(int n, const uint64_t *cmds) {
    if (!nvn_trace_frame()) return;
    fprintf(stderr, "[totk-frame] submit thread=%#x n=%d handles=",
            runtime_thread_current_handle(), n);
    for (int i = 0; i < n; i++) {
        cb_t *c = nvn_cb_by_handle(cmds[i]);
        fprintf(stderr, " %#llx%s%s", (unsigned long long)cmds[i],
                c ? "" : "(dead)", nvn_orphan_seen(cmds[i]) ? "(orphan)" : "");
        if (c) { cb_meta_t *m = nvn_cb_meta(c, 1); if (m) m->submit_count++; }
    }
    fputc('\n', stderr);
}

/* Called from Present once in a while: if a recorded handle was dropped
 * unsubmitted, show where its recording thread is now. */
void nvn_frame_report_orphans(void) {
    if (!nvn_trace_frame()) return;
    for (unsigned i = 0; i < g_orphan_count; i++) {
        if (g_orphans[i].reported) continue;
        g_orphans[i].reported = 1;
        int wkind = -1, nwh = 0, last_svc = 0, exited = 0;
        uint32_t wh[4] = {0};
        uint64_t waddr = 0, lr = 0, fp = 0, nsvc = 0;
        int ok = runtime_thread_probe(g_orphans[i].thread, &wkind, &waddr, wh, &nwh,
                                      &last_svc, &lr, &fp, &nsvc, &exited);
        fprintf(stderr, "[totk-frame] unsubmitted handle=%#llx cb=%p thread=%#x cmds=%u draws=%d lr=%#llx"
                        " | owner %s wkind=%d waddr=%#llx wh=%#x,%#x last_svc=%#x lr=%#llx nsvc=%llu\n",
                (unsigned long long)g_orphans[i].handle, g_orphans[i].cb, g_orphans[i].thread,
                g_orphans[i].commands, g_orphans[i].draws, (unsigned long long)g_orphans[i].lr,
                ok ? "live" : "gone", wkind, (unsigned long long)waddr,
                wh[0], wh[1], last_svc, (unsigned long long)lr, (unsigned long long)nsvc);
    }
}

/* Recording-thread pipeline warm-up.  Mirrors the layout/vertex/variant key
 * computed by nvn_process_submit_commands so the submit path finds a cached
 * pipeline instead of creating one on the render thread. */
void nvn_warm_recording(void *vc) {
    cb_t *cb = vc;
    if (!cb || cb->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !cb->prog)
        return;
    win_t *w = cb->win;
    int have_window = w && w->magic == NVN_MAGIC('W', 'I', 'N', 'D');
    prog_t *p = cb->prog;
    if (p->magic != NVN_MAGIC('P', 'R', 'O', 'G')) return;
    GfxRenderTargetLayout layout;
    uint32_t rw = 0, rh = 0;
    int have_layout = 0;
    int window_texture = 1;
    const nvn_draw_snap_t *snap = nvn_cb_last_draw(cb);
    nvn_cb_target_t *cbt = nvn_cb_target(cb, 0);
    if (snap && snap->have_layout) {
        layout = snap->layout;
        rw = snap->width;
        rh = snap->height;
        window_texture = snap->window_texture;
        have_layout = 1;
    } else if (cbt && cbt->valid) {
        layout = cbt->layout;
        rw = cbt->width;
        rh = cbt->height;
        window_texture = cbt->window_texture;
        have_layout = 1;
    } else if (have_window) {
        nvn_window_layout(w, &layout, &rw, &rh);
        have_layout = layout.color_count > 0 && rw && rh;
    }
    if (!have_layout) return;
    int window_pass = have_window && window_texture &&
                      nvn_layout_matches_window(w, &layout, rw, rh);
    uint32_t fmt = window_pass ? nvn_window_format(w) : layout.color_formats[0];
    /* Same depth-free window layout the submit path uses. */
    GfxRenderTargetLayout wlayout;
    const GfxRenderTargetLayout *pl = &layout;
    if (window_pass) {
        uint32_t ww = 0, wh = 0;
        nvn_window_layout(w, &wlayout, &ww, &wh);
        wlayout.depth_format = 0;
        pl = &wlayout;
        if (!fmt) fmt = wlayout.color_formats[0];
    }
    GfxVertexLayout input;
    uint64_t vertex_sig = 0;
    const nvn_bind_state_t *wbs = nvn_cb_draw_state(cb);
    nvn_build_vertex_layout(wbs, &input, &vertex_sig);
    char variant[192];
    nvn_variant_suffix(p, cb, variant, sizeof variant);
    (void)nvn_program_cached_pipeline(p, fmt, pl, window_pass,
                                      input.nattributes ? &input : NULL,
                                      vertex_sig, variant, 1);
}

/* Create each offscreen segment's pipeline on the recording thread.  TOTK's
 * scene command buffer has no window, so the regular EndRecording warm-up
 * (which needs cb->win) skips it; creating the pipeline on the submit thread
 * aborts the Adreno driver for some shader pairs. */
void nvn_warm_scene(cb_t *c) {
    if (!c || !c->prog || !c->bind_state) return;
    prog_t *p = c->prog;
    if (p->magic != NVN_MAGIC('P', 'R', 'O', 'G')) return;
    nvn_seg_t *segs = NULL;
    uint32_t nseg = nvn_cb_segments(c, &segs);
    for (uint32_t si = 0; si < nseg; si++) {
        nvn_seg_t *sg = &segs[si];
        if (!sg->draw_count || !sg->have_layout || sg->window_texture || !sg->color0_tex)
            continue;
        const nvn_bind_state_t *b = (sg->have_last && sg->last.have_state)
                                  ? &sg->last.state : c->bind_state;
        if (!b) continue;
        /* Same read-only view the submit path builds from the retired
         * snapshot, so the pipeline cache key and variant match. */
        cb_t view;
        memset(&view, 0, sizeof view);
        view.magic = NVN_MAGIC('C', 'M', 'D', 'B');
        view.dev = c->dev;
        view.cmds = c->cmds + sg->first_cmd;
        view.ncmds = sg->ncmds;
        view.bind_state = (void *)b;
        view.prog = c->prog;
        view.win = c->win;
        view.tex_pool = c->tex_pool;
        view.samp_pool = c->samp_pool;
        view.draw_count = (int)sg->draw_count;
        GfxVertexLayout input;
        uint64_t sig = 0;
        nvn_build_vertex_layout(b, &input, &sig);
        char variant[192];
        nvn_variant_suffix(p, &view, variant, sizeof variant);
        (void)nvn_program_cached_pipeline(p, (uint32_t)sg->layout.color_formats[0],
                                          &sg->layout, 0,
                                          input.nattributes ? &input : NULL, sig,
                                          variant, 1);
    }
    free(segs);
}

int nvn_gpu_trace(void) {
    static int on = -1;
    if (on < 0) on = getenv("TOTK_ANDROID_TRACE_GPU") != NULL;
    return on;
}

void nvn_gpu_report(const char *what, const nvn_cmd_t *cmd, const char *why) {
    g_gpu_stats.unsupported++;
    static unsigned logged;
    if (logged++ < 32 || nvn_gpu_trace())
        fprintf(stderr, "[totk-nvn] %s cmd=%u: %s (not faked)\n", what,
                cmd ? cmd->kind : 0u, why ? why : "unsupported");
}

/* Replay every recorded transfer/compute command of a command list in the
 * order the guest recorded them.  Draws are translated separately, so this
 * runs before the draw list is executed. */
void nvn_gpu_stats_log(const char *where) {
    static unsigned calls;
    if (!nvn_gpu_trace() && !nvn_trace_composite()) return;
    if (calls++ % 256) return;
    fprintf(stderr, "[totk-nvn] gpu stats %s: recorded clear=%u t2t=%u b2t=%u t2b=%u"
                    " dispatch=%u blit=%u | replayed clear=%u/%u copy=%u/%u upload=%u/%u"
                    " download=%u/%u dispatch=%u/%u rt-unsupported=%u unsupported=%u\n",
            where ? where : "?", nvn_cb_clear_texture_calls, nvn_cb_copy_t2t_calls,
            nvn_cb_copy_b2t, nvn_cb_copy_t2b_calls, nvn_cb_dispatch_calls, nvn_cb_blit_calls,
            g_gpu_stats.clear_ok, g_gpu_stats.clear_fail,
            g_gpu_stats.copy_ok, g_gpu_stats.copy_fail,
            g_gpu_stats.upload_ok, g_gpu_stats.upload_fail,
            g_gpu_stats.download_ok, g_gpu_stats.download_fail,
            g_gpu_stats.dispatch_ok, g_gpu_stats.dispatch_fail,
            g_gpu_stats.rt_unsupported, g_gpu_stats.unsupported);
}
