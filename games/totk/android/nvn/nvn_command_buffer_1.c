/* NVN module: command_buffer_1. */
#include "nvn_internal.h"

void nvn_cb_retire(cb_t *c) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) return;
    if (!c->handle || (!c->ncmds && !c->draw_count)) return;
    nvn_warm_scene(c);   /* create the scene pipeline on the recording thread */
    nvn_cbacct_bump(3);
    /* Snapshot per-segment pipelines while the guest program is live.  The
     * queue may render these after the guest finalizes/reuses the program. */
    nvn_seg_t *prepared = NULL;
    uint32_t nprepared = nvn_cb_segments(c, &prepared);
    nvn_seg_prepare(prepared, nprepared, c);
    pthread_mutex_lock(&g_retired_lock);
    nvn_retired_t *e = NULL, *oldest = NULL;
    for (unsigned i = 0; i < NVN_RETIRED_MAX; i++) {
        if (g_retired[i].used && g_retired[i].handle == c->handle) { e = &g_retired[i]; break; }
        if (!g_retired[i].used && !e) e = &g_retired[i];
        if (g_retired[i].used && (!oldest || g_retired[i].age < oldest->age)) oldest = &g_retired[i];
    }
    if (!e) e = oldest;
    if (!e) { pthread_mutex_unlock(&g_retired_lock); return; }
    nvn_retired_clear_locked(e);
    uint32_t n = c->ncmds;
    nvn_cmd_t *cmds = n ? calloc(n, sizeof *cmds) : NULL;
    uint32_t copied = 0;
    if (n && !cmds) { pthread_mutex_unlock(&g_retired_lock); return; }
    for (uint32_t i = 0; i < n; i++) {
        if (!nvn_cmd_clone(&cmds[i], &c->cmds[i])) {
            for (uint32_t k = 0; k < i; k++) nvn_cmd_payload_free(&cmds[k]);
            free(cmds);
            pthread_mutex_unlock(&g_retired_lock);
            return;
        }
        copied++;
    }
    e->used = 1;
    e->handle = c->handle;
    cb_meta_t *rm = nvn_cb_meta(c, 0);
    e->record_seq = rm ? rm->record_seq : 0;
    e->age = ++g_retired_age;
    e->source = c;
    e->cmds = cmds;
    e->ncmds = copied;
    if (c->bind_state) {
        e->bind = malloc(sizeof(nvn_bind_state_t));
        if (e->bind) memcpy(e->bind, c->bind_state, sizeof(nvn_bind_state_t));
    }
    e->prog = c->prog;
    e->win = c->win;
    e->dev = c->dev;
    e->tex_pool = c->tex_pool;
    e->samp_pool = c->samp_pool;
    e->draw_count = (uint32_t)c->draw_count;
    nvn_cb_target_t *t = nvn_cb_target(c, 0);
    if (t) {
        e->layout = t->layout;
        e->width = t->width;
        e->height = t->height;
        e->have_layout = t->valid;
        e->window_texture = t->window_texture;
        e->color0_tex = t->color0_tex;
    }
    const nvn_draw_snap_t *last = nvn_cb_last_draw(c);
    if (last) {
        e->last = *last;
        e->have_last = 1;
        if (last->color0_tex) e->color0_tex = last->color0_tex;
        if (last->have_layout) {
            e->layout = last->layout;
            e->width = last->width;
            e->height = last->height;
            e->have_layout = 1;
            e->window_texture = last->window_texture;
        }
    }
    /* Split at every SetRenderTargets.  Only segments that actually drew are
     * queued; a segment that only clears/state-binds its target is recorded as
     * an empty target so the compositor's sample can be reported honestly. */
    e->seg = prepared;
    e->nseg = nprepared;
    /* A title/UI recording can switch targets dozens of times; the 0x3f
     * composite is one of the later segments.  Do not truncate the segment
     * lists at 64 or the compositor's source target never reaches the
     * scene queue. */
    uint32_t *queue_segs = e->nseg ? calloc(e->nseg, sizeof *queue_segs) : NULL;
    uint32_t *queue_prio = e->nseg ? calloc(e->nseg, sizeof *queue_prio) : NULL;
    uint32_t *empty_segs = e->nseg ? calloc(e->nseg, sizeof *empty_segs) : NULL;
    unsigned nqueue = 0, nempty = 0;
    for (uint32_t i = 0; e->seg && i < e->nseg; i++) {
        nvn_seg_t *sg = &e->seg[i];
        if (!sg->have_layout || sg->window_texture || !sg->color0_tex) continue;
        if (sg->draw_count > 0) {
            uint32_t prio = 1u;
            if (sg->have_last) {
                const nvn_bind_state_t *sb = &sg->last.state;
                if (sb->tex_mask[0][0] || sb->tex_mask[1][0]) prio = 2u;
            }
            if (queue_segs && nqueue < e->nseg) {
                queue_segs[nqueue] = i;
                if (queue_prio) queue_prio[nqueue] = prio;
                nqueue++;
            }
        } else if (sg->clear_count == 0) {
            if (empty_segs && nempty < e->nseg) empty_segs[nempty++] = i;
        }
    }
    /* One dump per UI recording: every segment, its key state and the exact
     * reason it will or will not be replayed. */
    if (nvn_ui_seg_want()) {
        int has_ui = 0;
        for (uint32_t i = 0; e->seg && i < e->nseg; i++)
            if (nvn_seg_is_ui(&e->seg[i])) has_ui = 1;
        if (has_ui) {
            nvn_ui_seg_logs++;
            fprintf(stderr, "[totk-ui-seg] retire handle=%#llx nseg=%u nqueue=%u nempty=%u"
                            " draws=%u prog=%p bind=%d\n",
                    (unsigned long long)e->handle, e->nseg, nqueue, nempty,
                    e->draw_count, e->prog, e->bind != NULL);
            for (uint32_t si = 0; si < e->nseg; si++) {
                nvn_seg_t *sg = &e->seg[si];
                int queued = 0;
                uint32_t prio = 0;
                for (unsigned qi = 0; qi < nqueue; qi++)
                    if (queue_segs[qi] == si) { queued = 1; prio = queue_prio[qi]; break; }
                const tex_t *st = nvn_tex_registry_find(sg->color0_tex);
                fprintf(stderr, "[totk-ui-seg]  seg=%u cmd=[%u,%u) draws=%u clears=%u"
                                " layout=%d %ux%u fmt=%#x tex=%p prog=%p pstate=%p rp=%p"
                                " warm=%d last=%d queued=%d prio=%u reason=%s\n",
                        si, sg->first_cmd, sg->first_cmd + sg->ncmds, sg->draw_count,
                        sg->clear_count, sg->have_layout, sg->width, sg->height,
                        st ? (uint32_t)st->d.format : 0, sg->color0_tex, e->prog,
                        (void *)sg->pstate, (void *)sg->pipeline, sg->warm,
                        sg->have_last, queued, prio, nvn_seg_reason(sg, e));
            }
        }
    }
    {
        static int on = -1;
        if (on < 0) on = getenv("TOTK_ANDROID_TRACE_SEGS") != NULL;
        if (on) {
            static unsigned shown;
            if ((e->nseg || e->draw_count > 6 || e->color0_tex) && shown++ < 120) {
                fprintf(stderr, "[totk-segs] handle=%#llx src=%p nseg=%u nqueue=%u nempty=%u"
                                " draws=%u window=%d\n", (unsigned long long)e->handle,
                        (void *)c, e->nseg, nqueue, nempty, e->draw_count, c->win != NULL);
                for (uint32_t si = 0; si < e->nseg; si++) {
                    nvn_seg_t *sg = &e->seg[si];
                    int queued = 0;
                    for (unsigned qi = 0; qi < nqueue; qi++)
                        if (queue_segs[qi] == si) { queued = 1; break; }
                    const tex_t *st = nvn_tex_registry_find(sg->color0_tex);
                    uint32_t fmt = st ? (uint32_t)st->d.format : 0;
                    if (si != 0 && si + 1 != e->nseg && sg->width < 1024 &&
                        fmt != 0x3f && !queued)
                        continue;
                    fprintf(stderr, "[totk-segs]  %u cmd=[%u,%u) draws=%u tex=%p fmt=%#x"
                                    " %ux%u win=%d clear=%u queued=%d last=%d\n",
                            si, sg->first_cmd, sg->first_cmd + sg->ncmds, sg->draw_count,
                            sg->color0_tex, fmt, sg->width, sg->height,
                            sg->window_texture, sg->clear_count, queued, sg->have_last);
                }
            }
        }
    }
    if ((nvn_trace_frame() || nvn_trace_inputs()) && e->draw_count)
        nvn_frame_target_hist(c, e->color0_tex);
    uint64_t scene_handle = e->handle;
    pthread_mutex_unlock(&g_retired_lock);
    if ((nvn_trace_inputs() || nvn_trace_composite()) && copied >= 200) {
        static unsigned seg_dumps;
        unsigned limit = nvn_trace_composite() ? 3 : 1;
        if (seg_dumps++ < limit) {
            fprintf(stderr, "[totk-frame] segs handle=%#llx nseg=%u\n",
                    (unsigned long long)c->handle, e->nseg);
            for (uint32_t si = 0; si < e->nseg; si++) {
                nvn_seg_t *sg = &e->seg[si];
                uint32_t clears = 0, cmask = 0;
                float clr[4] = { 0, 0, 0, 0 };
                for (uint32_t ci = sg->first_cmd; ci < sg->first_cmd + sg->ncmds; ci++) {
                    const nvn_cmd_t *cc = &e->cmds[ci];
                    if (cc->kind != NVN_CMD_CLEAR_COLOR) continue;
                    clears++;
                    if (cc->u.clear.index == 0) {
                        memcpy(clr, cc->u.clear.rgba, sizeof clr);
                        cmask = cc->u.clear.mask;
                    }
                }
                int queued = 0;
                for (unsigned qi = 0; qi < nqueue; qi++) if (queue_segs[qi] == si) queued = 1;
                char uibuf[256] = "";
                if (sg->have_last) {
                    cb_t tv;
                    memset(&tv, 0, sizeof tv);
                    tv.tex_pool = e->tex_pool;
                    nvn_bs_samples_ui(&sg->last.state, &tv, uibuf, sizeof uibuf);
                }
                const tex_t *st = nvn_tex_registry_find(sg->color0_tex);
                fprintf(stderr, "[totk-frame] segs  %u cmd=[%u,%u) draws=%u tex=%p"
                                " fmt=%#x %ux%u win=%d clear=%u rgba=%.3f,%.3f,%.3f,%.3f"
                                " mask=%#x queued=%d nui=%d%s%s\n",
                        si, sg->first_cmd, sg->first_cmd + sg->ncmds, sg->draw_count,
                        sg->color0_tex, st ? st->d.format : 0, sg->width, sg->height,
                        sg->window_texture, clears, clr[0], clr[1], clr[2], clr[3], cmask,
                        queued, uibuf[0] ? 1 : 0, uibuf[0] ? " ui=" : "", uibuf);
                for (uint32_t ci = sg->first_cmd; ci < sg->first_cmd + sg->ncmds; ci++) {
                    const nvn_cmd_t *cc = &e->cmds[ci];
                    if (cc->kind != NVN_CMD_CLEAR_COLOR) continue;
                    fprintf(stderr, "[totk-frame] segs    clear idx=%d mask=%#x"
                                    " rgba=%.3f,%.3f,%.3f,%.3f\n",
                            cc->u.clear.index, cc->u.clear.mask,
                            cc->u.clear.rgba[0], cc->u.clear.rgba[1],
                            cc->u.clear.rgba[2], cc->u.clear.rgba[3]);
                }
            }
        }
    }
    if (nvn_trace_frame() || nvn_trace_inputs())
        fprintf(stderr, "[totk-frame] retire cb=%p handle=%#llx cmds=%u draws=%u bind=%d"
                        " rt0=%p segs=%u queued=%u empty=%u\n", (void *)c,
                (unsigned long long)c->handle, copied, e->draw_count, e->bind != NULL,
                e->color0_tex, e->nseg, nqueue, nempty);
    for (unsigned i = 0; i < nqueue; i++)
        nvn_scene_queue(scene_handle, queue_segs[i], rm ? rm->record_seq : 0,
                        queue_prio ? queue_prio[i] : 0,
                        e->seg && queue_segs[i] < e->nseg
                            ? e->seg[queue_segs[i]].color0_tex : NULL,
                        e->seg && queue_segs[i] < e->nseg
                            ? e->seg[queue_segs[i]].width : 0,
                        e->seg && queue_segs[i] < e->nseg
                            ? e->seg[queue_segs[i]].height : 0,
                        e->seg && queue_segs[i] < e->nseg
                            ? e->seg[queue_segs[i]].draw_count : 0);
    for (unsigned i = 0; i < nempty; i++) {
        nvn_retired_t *re = NULL;
        pthread_mutex_lock(&g_retired_lock);
        for (unsigned k = 0; k < NVN_RETIRED_MAX; k++)
            if (g_retired[k].used && g_retired[k].handle == scene_handle) {
                re = &g_retired[k];
                break;
            }
        const void *etex = (re && empty_segs[i] < re->nseg)
                         ? re->seg[empty_segs[i]].color0_tex : NULL;
        pthread_mutex_unlock(&g_retired_lock);
        nvn_note_empty_target(etex);
    }
    free(queue_segs);
    free(queue_prio);
    free(empty_segs);
}

uint32_t nvnCommandBufferInitialize(cb_t *c, void *d) { nvn_api("nvnCommandBufferInitialize");
    if (!c) { nvn_fail("nvnCommandBufferInitialize", "null command buffer"); return 0; }
    /* Re-initialising the same guest object must drop the previous recording
     * and registry entry, not leak them or leave a stale handle resolvable. */
    if (c->magic == NVN_MAGIC('C', 'M', 'D', 'B') && c->ncmds) {
        cb_meta_t *m = nvn_cb_meta(c, 0);
        if (!m || !m->submit_count) nvn_orphan_add(c, m);
    }
    { cb_meta_t *m = nvn_cb_meta(c, 0); if (m) { memset(m, 0, sizeof *m); m->cb = c; } }
    nvn_retired_drop_cb(c);
    for (unsigned i = 0; i < g_ncbs; i++) {
        if (g_cbs[i] != c) continue;
        nvn_cb_untrack(c);
        if (c->magic == NVN_MAGIC('C', 'M', 'D', 'B')) {
            nvn_cb_free_commands(c);
            free(c->bind_state);
        }
        break;
    }
    memset(c, 0, sizeof *c);
    nvn_cb_target_forget(c);
    c->magic = NVN_MAGIC('C', 'M', 'D', 'B');
    c->dev = d;
    c->serial = ++nvn_cb_serial;
    if (!nvn_cb_track(c)) {
        c->magic = 0;
        nvn_fail("nvnCommandBufferInitialize", "out of memory for command-buffer registry");
        return 0;
    }
    if (nvn_trace()) fprintf(stderr, "[totk-nvn] CommandBufferInitialize %p\n", (void *)c);
    return 1;
}

void nvnCommandBufferFinalize(cb_t *c) {
    if (!c) return;
    if (c->magic == NVN_MAGIC('C', 'M', 'D', 'B') && c->ncmds) {
        cb_meta_t *m = nvn_cb_meta(c, 0);
        if (!m || !m->submit_count) nvn_orphan_add(c, m);
        if (nvn_trace_frame())
            fprintf(stderr, "[totk-frame] finalize cb=%p handle=%#llx thread=%#x cmds=%u draws=%d\n",
                    (void *)c, (unsigned long long)c->handle, runtime_thread_current_handle(),
                    c->ncmds, c->draw_count);
    }
    {
        cb_meta_t *m = nvn_cb_meta(c, 0);
        unsigned kinds = 0;
        int cand = nvn_cb_mask_candidate(c, &kinds);
        if (!m || !m->submit_count) {
            nvn_cbacct_bump(6);
            if (cand || nvn_trace_composite() ||
                getenv("TOTK_ANDROID_TRACE_CBACCT"))
                fprintf(stderr, "[totk-cbacct] NEVER-SUBMITTED cb=%p handle=%#llx"
                                " thread=%#x cmds=%u draws=%d mask_candidate=%d kinds=%#x\n",
                        (void *)c, (unsigned long long)c->handle,
                        m ? m->record_thread : runtime_thread_current_handle(),
                        c->ncmds, c->draw_count, cand, kinds);
        }
    }
    nvn_cb_retire(c);
    nvn_cb_untrack(c);
    nvn_cb_target_forget(c);
    nvn_cb_free_commands(c);
    free(c->bind_state);
    c->bind_state = NULL;
    c->prog = NULL;
    c->win = NULL;
    c->draw_first = c->draw_count = 0;
    c->recording = 0;
    c->handle = 0;
    c->magic = 0;
}

void nvnCommandBufferSetDebugLabel(cb_t *c, const char *l) { (void)c; (void)l; }

void nvnCommandBufferSetMemoryCallback(cb_t *c, void *cb) { if (c) c->mem_cb = cb; }

void nvnCommandBufferSetMemoryCallbackData(cb_t *c, void *d) { if (c) c->mem_cb_data = d; }

void nvnCommandBufferSetControlMemoryCallbackEnabled(cb_t *c, int e) { (void)c; (void)e; }

void nvnCommandBufferSetCommandMemoryCallbackEnabled(cb_t *c, int e) { (void)c; (void)e; }

void nvnCommandBufferAddCommandMemory(cb_t *c, const pool_t *p, ptrdiff_t off, size_t n) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferAddCommandMemory", "invalid command buffer"); return; }
    if (!nvn_obj(p, NVN_MAGIC('P', 'O', 'O', 'L'), "nvnCommandBufferAddCommandMemory(pool)")) return;
    if (!p->mem) { nvn_fail("nvnCommandBufferAddCommandMemory", "pool has no memory"); return; }
    c->cmd = p->mem + off;
    c->cmd_size = n;
    c->cmd_used = 0;
}

void nvnCommandBufferAddControlMemory(cb_t *c, void *m, size_t n) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferAddControlMemory", "invalid command buffer"); return; }
    c->ctl = m;
    c->ctl_size = n;
    c->ctl_used = 0;
}

size_t nvnCommandBufferGetControlMemorySize(const cb_t *c) { return c ? c->ctl_size : 0; }

size_t nvnCommandBufferGetControlMemoryUsed(const cb_t *c) { return c ? c->ctl_used : 0; }

size_t nvnCommandBufferGetCommandMemorySize(const cb_t *c) { return c ? c->cmd_size : 0; }

size_t nvnCommandBufferGetCommandMemoryUsed(const cb_t *c) { return c ? c->cmd_used : 0; }

void nvnCommandBufferBeginRecording(cb_t *c) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBeginRecording", "invalid command buffer"); return; }
    /* The frame loop re-records a buffer that still holds an unsubmitted
     * recording.  That handle is gone the moment ncmds is reset, so report it
     * here as well as on Initialize. */
    if (c->ncmds) {
        cb_meta_t *m = nvn_cb_meta(c, 0);
        if (!m || !m->submit_count) nvn_orphan_add(c, m);
        if (nvn_trace_frame() && c->ncmds >= 32) {
            uint32_t hist[16] = {0};
            for (uint32_t i = 0; i < c->ncmds; i++) hist[c->cmds[i].kind & 15]++;
            fprintf(stderr, "[totk-frame] begin-over cb=%p handle=%#llx thread=%#x cmds=%u draws=%d kinds=",
                    (void *)c, (unsigned long long)c->handle, runtime_thread_current_handle(),
                    c->ncmds, c->draw_count);
            for (int i = 0; i < 16; i++) if (hist[i]) fprintf(stderr, " %d:%u", i, hist[i]);
            fputc('\n', stderr);
        }
    }
    {
        cb_meta_t *rm = nvn_cb_meta(c, 1);
        uint64_t seq = ++nvn_record_count;
        if (rm) rm->record_seq = seq;
    }
    if (nvn_trace_first() && nvn_record_count <= 64)
        fprintf(stderr, "[totk-nvn] BeginRecording #%u cb=%p rec=%u prog=%p win=%p"
                        " tpool=%p spool=%p\n", nvn_record_count, (void *)c, c->recording,
                c->prog, c->win, (const void *)c->tex_pool, (const void *)c->samp_pool);
    c->recording = 1;
    c->cmd_used = 0;
    c->ctl_used = 0;
    c->draw_first = 0;
    c->draw_count = 0;
    c->ncmds = 0;
    { nvn_cb_target_t *t = nvn_cb_target(c, 0); if (t) t->ndraws = 0; }
    /* NVN keeps the command buffer's bound state across Begin/EndRecording:
     * the recording only appends commands.  Do not reset the bind state or the
     * resolved render targets here, or state the guest bound before
     * BeginRecording (uniform buffers, vertex layout, pools) disappears from
     * the next submission.  nvnCommandBufferInitialize is what starts a fresh
     * command buffer with default state. */
}
