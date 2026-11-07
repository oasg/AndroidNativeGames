/* NVN module: command_buffer_2. */
#include "nvn_internal.h"

uint64_t nvnCommandBufferEndRecording(cb_t *c) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferEndRecording", "invalid command buffer"); return 0; }
    c->recording = 0;
    uint64_t h = 0x100000000ull | (c->serial & 0xffffffffull);
    c->handle = h;
    {
        unsigned kinds = 0;
        int cand = nvn_cb_mask_candidate(c, &kinds);
        nvn_cbacct_bump(0);
        nvn_cbacct_bump(1);
        if (c->draw_count) nvn_cbacct_bump(5);
        if (cand) nvn_cbacct_bump(7);
        if (cand && (nvn_trace_composite() || getenv("TOTK_ANDROID_TRACE_CBACCT")))
            fprintf(stderr, "[totk-cbacct] mask-candidate cb=%p handle=%#llx thread=%#x"
                            " cmds=%u draws=%d kinds=%#x\n", (void *)c,
                    (unsigned long long)h, runtime_thread_current_handle(), c->ncmds,
                    c->draw_count, kinds);
    }
    {
        static int on = -1;
        if (on < 0) on = getenv("TOTK_ANDROID_TRACE_CB_FLOW") != NULL;
        if (on) {
            static unsigned shown;
            if (shown++ < 240)
                fprintf(stderr, "[totk-cb] end cb=%p h=%#llx ncmds=%u draws=%d prog=%p win=%p"
                                " thread=%#x\n", (void *)c, (unsigned long long)h, c->ncmds,
                        c->draw_count, (void *)c->prog, (void *)c->win,
                        runtime_thread_current_handle());
        }
    }
    cb_meta_t *meta = nvn_cb_meta(c, 1);
    uint32_t rec_thread = runtime_thread_current_handle();
    uint64_t rec_lr = 0;
    if (meta) {
        meta->record_thread = rec_thread;
        meta->record_commands = c->ncmds;
        meta->submit_count = 0;
    }
    if (nvn_trace_frame()) {
        uint64_t svc_lr = 0, svc_fp = 0, nsvc = 0;
        int svc = 0, wk = -1, nwh = 0, ex = 0; uint32_t whs[4] = {0}; uint64_t wa = 0;
        runtime_thread_probe(rec_thread, &wk, &wa, whs, &nwh, &svc, &svc_lr, &svc_fp, &nsvc, &ex);
        rec_lr = svc_lr;
        if (meta) meta->record_lr = svc_lr;
        fprintf(stderr, "[totk-frame] end cb=%p handle=%#llx thread=%#x cmds=%u draws=%d prog=%p win=%p"
                        " rt0=%p fmt=%#x %ux%u rt_draws=%u lr=%#llx svc_lr=%#llx last_svc=%#x\n",
                (void *)c, (unsigned long long)h, rec_thread, c->ncmds,
                c->draw_count, c->prog, c->win,
                meta ? meta->rt_color0 : NULL, meta ? meta->rt_nvn_format : 0,
                meta ? meta->rt_width : 0, meta ? meta->rt_height : 0,
                meta ? meta->rt_draws : 0, (unsigned long long)rec_lr,
                (unsigned long long)svc_lr, svc);
    }
    if (nvn_trace_frame() && c->draw_count) {
        static int window_dumps;
        if (window_dumps++ < 2) {
            fprintf(stderr, "[totk-frame] draw-cb handle=%#llx cmds=%u draws=%d:\n",
                    (unsigned long long)h, c->ncmds, c->draw_count);
            const nvn_draw_snap_t *ds = nvn_cb_last_draw(c);
            if (ds && ds->have_state) {
                const nvn_bind_state_t *db = &ds->state;
                fprintf(stderr, "[totk-frame] draw-state va=%#x va0=%#x vs=%#llx vb=%#x"
                                " vb0addr=%#llx vb0size=%zu depth_test=%d cull=%d blend=%#x"
                                " chmask=%#x layout=%dx%d\n",
                        db->va_mask, db->va[0], (unsigned long long)db->vs[0], db->vb_mask,
                        (unsigned long long)db->vb[0].address, db->vb[0].size, db->rs.ds.dt, db->rs.poly.cull,
                        db->rs.color.blend_en, db->rs.chmask.mask, ds->width, ds->height);
                if (db->vb[0].address) {
                    const float *v = (const float *)(uintptr_t)db->vb[0].address;
                    fprintf(stderr, "[totk-frame] vb0 floats:");
                    for (int k = 0; k < 16; k++) fprintf(stderr, " %.3f", v[k]);
                    fputc('\n', stderr);
                    const uint8_t *raw = (const uint8_t *)(uintptr_t)db->vb[0].address;
                    fprintf(stderr, "[totk-frame] vb0 hex:");
                    for (int k = 0; k < 64 && (size_t)k < db->vb[0].size; k++) fprintf(stderr, "%02x", raw[k]);
                    fputc('\n', stderr);
                    for (uint32_t ci = 0; ci < c->ncmds; ci++) {
                        if (c->cmds[ci].kind != NVN_CMD_DRAW_INDEXED) continue;
                        const uint8_t *ib = (const uint8_t *)(uintptr_t)c->cmds[ci].u.draw_indexed.indices;
                        fprintf(stderr, "[totk-frame] index type=%u count=%u hex:",
                                c->cmds[ci].u.draw_indexed.index_type,
                                c->cmds[ci].u.draw_indexed.count);
                        for (uint32_t k = 0; k < c->cmds[ci].u.draw_indexed.count && k < 16; k++) fprintf(stderr, " %02x", ib[k]);
                        fputc('\n', stderr);
                        break;
                    }
                }
            }
            for (uint32_t i = 0; i < c->ncmds; i++) {
                const nvn_cmd_t *cc = &c->cmds[i];
                fprintf(stderr, "  %u kind=%u", i, cc->kind);
                if (cc->kind == NVN_CMD_DRAW || cc->kind == NVN_CMD_DRAW_INDEXED)
                    fprintf(stderr, " prim=%u first=%u count=%u", cc->u.draw.primitive,
                            cc->u.draw.first, cc->u.draw.count);
                else if (cc->kind == NVN_CMD_SET_RENDER_TARGETS)
                    fprintf(stderr, " n=%u rt0=%p depth=%p", cc->u.targets.count,
                            cc->u.targets.tex[0], cc->u.targets.depth);
                else if (cc->kind == NVN_CMD_SET_VIEWPORT || cc->kind == NVN_CMD_SET_SCISSOR)
                    fprintf(stderr, " rect=%d,%d %dx%d", cc->u.rect.x, cc->u.rect.y,
                            cc->u.rect.width, cc->u.rect.height);
                else if (cc->kind == NVN_CMD_CLEAR_COLOR)
                    fprintf(stderr, " i=%d rgba=%.2f,%.2f,%.2f,%.2f", cc->u.clear.index,
                            cc->u.clear.rgba[0], cc->u.clear.rgba[1], cc->u.clear.rgba[2], cc->u.clear.rgba[3]);
                else if (cc->kind == NVN_CMD_UPDATE_UBO)
                    fprintf(stderr, " dst=%#llx n=%u", (unsigned long long)cc->u.update.dst, cc->u.update.size);
                fputc('\n', stderr);
            }
        }
    }
    if (nvn_trace_frame() && c->ncmds >= 32) {
        uint32_t hist[16] = {0};
        for (uint32_t i = 0; i < c->ncmds; i++) hist[c->cmds[i].kind & 15]++;
        fprintf(stderr, "[totk-frame] kinds handle=%#llx cmds=%u draws=%d:",
                (unsigned long long)h, c->ncmds, c->draw_count);
        for (int i = 0; i < 16; i++) if (hist[i]) fprintf(stderr, " %d:%u", i, hist[i]);
        fputc('\n', stderr);
    }
    if (nvn_trace_first() && (nvn_record_count <= 64 || (c->draw_count && nvn_draw_rec_trace++ < 8))) {
        const nvn_bind_state_t *b = c->bind_state;
        fprintf(stderr, "[totk-nvn] EndRecording #%u cb=%p bs=%p handle=%#llx cmds=%u draw=%d"
                        " va_mask=%#x va0=%#x vs_mask=%#x vs0=%#llx vb_mask=%#x ubo=%#x tex0=%#x\n",
                nvn_record_count, (void *)c, (void *)b, (unsigned long long)h, c->ncmds,
                c->draw_count, b ? b->va_mask : 0, b ? b->va[0] : 0,
                b ? b->vs_mask : 0, b ? (unsigned long long)b->vs[0] : 0,
                b ? b->vb_mask : 0, b ? b->ubo_mask[0] : 0, b ? b->tex_mask[0][0] : 0);
        if (b) for (int i = 0; i < 16; i++) {
            if (!(b->va_mask & (1u << i)) && !(b->vs_mask & (1u << i))) continue;
            fprintf(stderr, "[totk-nvn] EndRecording #%u state[%d] va=%#x vs=%#llx\n",
                    nvn_record_count, i, b->va[i], (unsigned long long)b->vs[i]);
        }
    }
    if (nvn_trace()) fprintf(stderr, "[totk-nvn] CommandBufferEndRecording %p -> handle %#llx (%u cmds)\n",
                             (void *)c, (unsigned long long)h, c->ncmds);
    /* Create the reflected pipeline on the recording thread.  The Adreno
     * driver aborts a first vkCreateGraphicsPipelines from the submit thread
     * for some real TOTK shader pairs; the same call on the game thread
     * succeeds, and the submit path then reuses the cached pipeline. */
    nvn_warm_recording(c);
    return h;
}

void nvnCommandBufferBindProgram(cb_t *c, const prog_t *p, int stage_mask) {
    nvn_api("nvnCommandBufferBindProgram");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindProgram", "invalid command buffer"); return; }
    if (!nvn_obj(p, NVN_MAGIC('P', 'R', 'O', 'G'), "nvnCommandBufferBindProgram(program)")) return;
    c->prog = (void *)p;
    if (nvn_trace() || (nvn_trace_first() && nvn_draw_trace++ < 32))
        fprintf(stderr, "[totk-nvn] BindProgram cb=%p rec=%u program=%p stageMask=%#x\n",
                (void *)c, c->recording, (const void *)p, stage_mask);
}

/* Host bridge used by the self-tests; the guest NVN API is BindProgram. */
void nvnCommandBufferSetProgram(cb_t *c, prog_t *p) {
    nvnCommandBufferBindProgram(c, p, -1);
}

void nvnCommandBufferSetWindow(cb_t *c, win_t *w) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetWindow", "invalid command buffer"); return; }
    if (!nvn_obj(w, NVN_MAGIC('W', 'I', 'N', 'D'), "nvnCommandBufferSetWindow(window)")) return;
    c->win = w;
    if (nvn_trace_first() && nvn_rt_trace++ < 32)
        fprintf(stderr, "[totk-nvn] SetWindow cb=%p rec=%u win=%p native=%p\n",
                (void *)c, c->recording, (void *)w, w->native);
}

void nvnCommandBufferDrawArrays(cb_t *c, int mode, int first, int count) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) {
        nvn_fail("nvnCommandBufferDrawArrays", "invalid command buffer");
        return;
    }
    if (count < 0 || first < 0) {
        nvn_fail("nvnCommandBufferDrawArrays", "negative first/count");
        return;
    }
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_DRAW);
    if (cmd) {
        cmd->u.draw.primitive = (uint32_t)mode;
        cmd->u.draw.first = (uint32_t)first;
        cmd->u.draw.count = (uint32_t)count;
        cmd->u.draw.instances = 1;
        cmd->u.draw.base_instance = 0;
        nvn_cb_snapshot_vertex_buffers(cmd, c->bind_state,
                                       (uint32_t)first + (uint32_t)count);
    }
    c->draw_first = first;
    c->draw_count += count;
    nvn_cb_draw_snapshot(c);
    nvn_frame_draw_trace(c, "arrays", count);
    nvn_cb_trace_record_vb(c, "arrays", (uint32_t)count);
    if (nvn_trace() || (nvn_trace_first() && nvn_draw_trace++ < 32))
        fprintf(stderr, "[totk-nvn] CommandBufferDrawArrays cb=%p rec=%u mode=%d first=%d count=%d"
                        " va_mask=%#x vs_mask=%#x\n", (void *)c, c->recording, mode, first, count,
                c->bind_state ? ((nvn_bind_state_t *)c->bind_state)->va_mask : 0,
                c->bind_state ? ((nvn_bind_state_t *)c->bind_state)->vs_mask : 0);
}

void nvnCommandBufferDrawArraysInstanced(cb_t *c, int mode, int first, int count,
                                                int base_instance, int instances) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) {
        nvn_fail("nvnCommandBufferDrawArraysInstanced", "invalid command buffer");
        return;
    }
    if (count < 0 || first < 0 || base_instance < 0 || instances < 0) {
        nvn_fail("nvnCommandBufferDrawArraysInstanced", "negative argument");
        return;
    }
    if (instances == 0) return;   /* NVN: zero instances draw nothing */
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_DRAW);
    if (cmd) {
        cmd->u.draw.primitive = (uint32_t)mode;
        cmd->u.draw.first = (uint32_t)first;
        cmd->u.draw.count = (uint32_t)count;
        cmd->u.draw.instances = (uint32_t)instances;
        cmd->u.draw.base_instance = (uint32_t)base_instance;
        nvn_cb_snapshot_vertex_buffers(cmd, c->bind_state,
                                       (uint32_t)first + (uint32_t)count);
    }
    c->draw_first = first;
    c->draw_count += count;
    nvn_cb_draw_snapshot(c);
    nvn_frame_draw_trace(c, "arrays", count);
    nvn_cb_trace_record_vb(c, "arrays-inst", (uint32_t)count);
    if (nvn_trace() || (nvn_trace_first() && nvn_draw_trace++ < 32))
        fprintf(stderr, "[totk-nvn] DrawArraysInstanced cb=%p rec=%u mode=%d first=%d count=%d base=%d instances=%d\n",
                (void *)c, c->recording, mode, first, count, base_instance, instances);
}

void nvnCommandBufferSetRenderEnable(cb_t *c, int e) {
    (void)c;
    if (nvn_trace_frame()) fprintf(stderr, "[totk-frame] render-enable cb=%p thread=%#x enable=%d\n",
                                    (void *)c, runtime_thread_current_handle(), e);
}

void nvnCommandBufferBindBlendState(cb_t *c, const void *s) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !s) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.blend = *(const nvn_blend_st *)s; nvn_cb_state_event(c); }
}

void nvnCommandBufferBindColorState(cb_t *c, const void *s) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !s) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.color = *(const nvn_color_st *)s; nvn_cb_state_event(c); }
}

void nvnCommandBufferBindChannelMaskState(cb_t *c, const void *s) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !s) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.chmask = *(const nvn_chmask_st *)s; nvn_cb_state_event(c); }
}

void nvnCommandBufferBindMultisampleState(cb_t *c, const void *s) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !s) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.ms = *(const nvn_ms_st *)s; nvn_cb_state_event(c); }
}

void nvnCommandBufferBindPolygonState(cb_t *c, const void *s) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !s) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.poly = *(const nvn_poly_st *)s; nvn_cb_state_event(c); }
}

void nvnCommandBufferBindDepthStencilState(cb_t *c, const void *s) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !s) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.ds = *(const nvn_ds_st *)s; nvn_cb_state_event(c); }
}

void nvnCommandBufferSetBlendColor(cb_t *c, const float *v) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !v) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { memcpy(b->rs.blend_color, v, sizeof b->rs.blend_color); nvn_cb_state_event(c); }
}

void nvnCommandBufferSetStencilRef(cb_t *c, int face, int v) {
    (void)face;
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.stencil_ref = v; nvn_cb_state_event(c); }
}

void nvnCommandBufferSetStencilMask(cb_t *c, int face, int v) {
    (void)face;
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.stencil_mask = v; nvn_cb_state_event(c); }
}

void nvnCommandBufferSetStencilValueMask(cb_t *c, int face, int v) {
    (void)face;
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.stencil_value_mask = v; nvn_cb_state_event(c); }
}

void nvnCommandBufferClearDepthStencil(cb_t *c, float depth, uint32_t dmask,
                                              int stencil, int smask) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) return;
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_CLEAR_DEPTH_STENCIL);
    if (cmd) {
        cmd->u.clear_ds.depth = depth;
        cmd->u.clear_ds.dmask = dmask;
        cmd->u.clear_ds.stencil = stencil;
        cmd->u.clear_ds.smask = (uint32_t)smask;
    }
}

void nvnCommandBufferSetRenderTargets(cb_t *c, int n, const void *const *t,
                                             const void *const *views,
                                             const void *depth, const void *depth_view) {
    nvn_api("nvnCommandBufferSetRenderTargets");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetRenderTargets", "invalid command buffer"); return; }
    nvn_bind_state_t *bs = nvn_cb_bind_state(c);
    if (bs) bs->depth_tex = depth;
    if (n < 0 || n > GFX_MAX_COLOR_TARGETS || (!t && n)) { nvn_fail("nvnCommandBufferSetRenderTargets", "invalid target count"); return; }
    int requested_n = n;
    while (n > 1 && t && !t[n - 1]) n--;
    if (getenv("TOTK_ANDROID_TRACE_TARGET")) {
        fprintf(stderr, "[totk-target] SetRenderTargets cb=%p requested=%d active=%d depth=%p colors=",
                (void *)c, requested_n, n, depth);
        for (int i = 0; i < requested_n && i < GFX_MAX_COLOR_TARGETS; i++) {
            uintptr_t v = 0;
            if (!t || !nvn_crash_read_mem((uintptr_t)(t + i), &v, sizeof v)) v = 0;
            fprintf(stderr, " %p", (void *)v);
        }
        fprintf(stderr, "\n");
    }
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_SET_RENDER_TARGETS);
    if (cmd) {
        cmd->u.targets.count = (uint32_t)n;
        for (int i = 0; i < n; i++) cmd->u.targets.tex[i] = (void *)t[i];
        cmd->u.targets.depth = (void *)depth;
    }
    /* Resolve the complete set now: every color target and the depth target
     * keep their own guest format/dimensions for the layout and pipeline key. */
    nvn_cb_target_t *cbt = nvn_cb_target(c, 1);
    if (cbt) {
        cbt->valid = 0;
        cbt->width = cbt->height = 0;
        memset(&cbt->layout, 0, sizeof cbt->layout);
        if (n > 0) {
            const char *why = NULL;
            if (nvn_rt_layout_from_targets((uint32_t)n, (void *const *)t, views,
                                           depth, depth_view,
                                           &cbt->layout, &cbt->width, &cbt->height, &why)) {
                cbt->valid = 1;
                /* Keep the resolved layout with the recorded command so the
                 * segment split (which runs at retire time) uses the same
                 * view formats as the recording did. */
                if (cmd) {
                    cmd->u.targets.layout = cbt->layout;
                    cmd->u.targets.width = cbt->width;
                    cmd->u.targets.height = cbt->height;
                    cmd->u.targets.have_layout = 1;
                }
            } else {
                char d[128];
                snprintf(d, sizeof d, "cannot resolve render targets: %s", why ? why : "unknown");
                nvn_fail("nvnCommandBufferSetRenderTargets", d);
            }
        }
        cbt->color0_tex = (n > 0 && t) ? t[0] : NULL;
    }
    /* NVN binds textures, not windows.  Resolve the window whose swapchain
     * texture is color target 0 so present/draw share one host target. */
    if (n > 0 && t && t[0]) {
        win_t *w = nvn_window_for_texture(t[0]);
        if (w) { c->win = w; if (cbt) cbt->window_texture = 1; }
    }
    cb_meta_t *m = nvn_cb_meta(c, 1);
    const tex_t *t0m = (n > 0 && t && t[0]) ? nvn_tex_registry_find(t[0]) : NULL;
    if (m && n > 0) {
        m->rt_color0 = t[0];
        m->rt_nvn_format = t0m ? (uint32_t)t0m->d.format : 0;
        m->rt_width = t0m ? t0m->d.width : (cbt ? cbt->width : 0);
        m->rt_height = t0m ? t0m->d.height : (cbt ? cbt->height : 0);
        m->rt_draws = 0;
    }
    if (nvn_trace_frame() && n) {
        const tex_t *dt = nvn_tex_registry_find(depth);
        fprintf(stderr, "[totk-frame] rt cb=%p thread=%#x n=%d color0=%p fmt=%#x %ux%u"
                        " depth=%p dfmt=%#x %ux%u window=%p\n",
                (void *)c, runtime_thread_current_handle(), n, t[0],
                t0m ? t0m->d.format : 0, t0m ? t0m->d.width : 0, t0m ? t0m->d.height : 0,
                depth, dt ? dt->d.format : 0, dt ? dt->d.width : 0, dt ? dt->d.height : 0,
                (void *)c->win);
    }
    if (nvn_trace_first() && nvn_rt_trace++ < 32 && n) {
        const tex_t *ti = nvn_tex_registry_find(t[0]);
        fprintf(stderr, "[totk-nvn] SetRenderTargets cb=%p n=%d color0=%p tex=%p fmt=%#x"
                        " %ux%u depth=%p window=%p\n", (void *)c, n, t[0], (const void *)ti,
                ti ? ti->d.format : 0, ti ? ti->d.width : 0, ti ? ti->d.height : 0,
                depth, (void *)c->win);
    }
    if (nvn_trace() && n) {
        const tex_t *t0 = nvn_tex_registry_find(t[0]);
        const tex_t *dt = nvn_tex_registry_find(depth);
        fprintf(stderr, "[totk-nvn] SetRenderTargets count=%d window=%p", n, (void *)c->win);
        for (int i = 0; i < n; i++) {
            const tex_t *ti = nvn_tex_registry_find(t[i]);
            fprintf(stderr, " color%d=%p(%s fmt=%#x %ux%u)", i, t[i], ti ? "tex" : "?",
                    ti ? ti->d.format : 0, ti ? ti->d.width : 0, ti ? ti->d.height : 0);
        }
        fprintf(stderr, " depth=%p(tex=%d fmt=%#x %ux%u)\n", depth, dt != NULL,
                dt ? dt->d.format : 0, dt ? dt->d.width : 0, dt ? dt->d.height : 0);
        (void)t0;
    }

}

void nvnCommandBufferClearColor(cb_t *c, int i, const float *v, int mask) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferClearColor", "invalid command buffer"); return; }
    if (i < 0 || i > 7 || !v) { nvn_fail("nvnCommandBufferClearColor", "invalid target/color"); return; }
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_CLEAR_COLOR);
    if (cmd) {
        memcpy(cmd->u.clear.rgba, v, sizeof cmd->u.clear.rgba);
        cmd->u.clear.index = i;
        cmd->u.clear.mask = (uint32_t)mask;
    }
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] ClearColor target=%d rgba=%.3f,%.3f,%.3f,%.3f mask=%#x\n",
                i, v[0], v[1], v[2], v[3], mask);
}

void nvnCommandBufferClearColori(cb_t *c, int i, const int *v, int mask) {
    if (!v) { nvn_fail("nvnCommandBufferClearColori", "null color"); return; }
    const float f[4] = { (float)v[0], (float)v[1], (float)v[2], (float)v[3] };
    nvnCommandBufferClearColor(c, i, f, mask);
}

void nvnCommandBufferClearColorui(cb_t *c, int i, const uint32_t *v, int mask) {
    if (!v) { nvn_fail("nvnCommandBufferClearColorui", "null color"); return; }
    const float f[4] = { (float)v[0], (float)v[1], (float)v[2], (float)v[3] };
    nvnCommandBufferClearColor(c, i, f, mask);
}

void nvnCommandBufferSetViewport(cb_t *c, int x, int y, int w, int h) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetViewport", "invalid command buffer"); return; }
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_SET_VIEWPORT);
    if (cmd) { cmd->u.rect.x = x; cmd->u.rect.y = y; cmd->u.rect.width = w; cmd->u.rect.height = h; }
}

void nvnCommandBufferSetScissor(cb_t *c, int x, int y, int w, int h) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetScissor", "invalid command buffer"); return; }
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_SET_SCISSOR);
    if (cmd) { cmd->u.rect.x = x; cmd->u.rect.y = y; cmd->u.rect.width = w; cmd->u.rect.height = h; }
}

void nvnCommandBufferSetViewports(cb_t *c, int first, int n, const float *v) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetViewports", "invalid command buffer"); return; }
    if (first < 0 || n < 0 || (!v && n) || !n) return;
    /* NVNviewport array: x, y, width, height, near, far (six floats each). */
    nvnCommandBufferSetViewport(c, (int)v[0], (int)v[1], (int)v[2], (int)v[3]);
}

void nvnCommandBufferSetScissors(cb_t *c, int first, int n, const int *v) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetScissors", "invalid command buffer"); return; }
    if (first < 0 || n < 0 || (!v && n) || !n) return;
    nvnCommandBufferSetScissor(c, v[0], v[1], v[2], v[3]);
}
