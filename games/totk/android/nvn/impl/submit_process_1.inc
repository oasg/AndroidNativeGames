/* nvn_process_submit_commands: implementation phase 1. */
    nvn_pool_epoch_tick();
    for (int i = 0; i < n; i++) {
        cb_t *cb = NULL;
        for (unsigned j = 0; j < g_ncbs; j++) {
            cb_t *cand = g_cbs[j];
            if (!cand || cand->magic != NVN_MAGIC('C', 'M', 'D', 'B')) continue;
            if (cand->handle == cmds[i]) { cb = cand; break; }
        }
        if (!cb || (!cb->draw_count && !cb->ncmds)) continue;
        nvn_cbacct_bump(2);
        { cb_meta_t *mm = nvn_cb_meta(cb, 1); if (mm) mm->submit_count++; }
        {
            static int on = -1;
            if (on < 0) on = getenv("TOTK_ANDROID_TRACE_SUBMIT_CB") != NULL;
            if (on) {
                static unsigned shown;
                if (cb->draw_count > 6 && shown++ < 240)
                    fprintf(stderr, "[totk-submit] handle=%#llx cb=%p ncmds=%u draws=%d"
                                    " prog=%p win=%p target=%p\n",
                            (unsigned long long)cmds[i], (void *)cb, cb->ncmds,
                            cb->draw_count, (void *)cb->prog, (void *)cb->win,
                            (void *)(nvn_cb_target(cb, 0) ? nvn_cb_target(cb, 0)->color0_tex : NULL));
            }
        }
        nvn_cb_apply_uniform_updates(cb);
        /* Replay the recorded texture transfers and compute dispatches before
         * the draw list runs, in the order the guest recorded them.  This also
         * covers command buffers with no render target (pure compute/upload
         * work), which the target check below skips. */
        nvn_exec_gpu_commands(cb->cmds, cb->ncmds, "submit");
        if (nvn_trace_first() && nvn_submit_trace++ < 4096) {
            const nvn_bind_state_t *sb = cb->bind_state;
            fprintf(stderr, "[totk-nvn] Submit#%u handle=%#llx cb=%p prog=%p draw=%d"
                            " va=%#x ubo=%#x/%#x tex=%#x tpool=%p spool=%p\n",
                    nvn_submit_trace, (unsigned long long)cmds[i], (void *)cb, (void *)cb->prog,
                    cb->draw_count, sb ? sb->va_mask : 0, sb ? sb->ubo_mask[0] : 0,
                    sb ? sb->ubo_mask[1] : 0, sb ? sb->tex_mask[0][0] : 0,
                    (const void *)cb->tex_pool, (const void *)cb->samp_pool);
            if (nvn_submit_trace == 1) for (int gs = 0; gs < 2; gs++) for (int gi = 0; gi < 16; gi++) {
                uint64_t ga = 0; size_t gn = 0;
                if (!nvn_global_ubo_get(gs, gi, &ga, &gn)) continue;
                const uint32_t *gu = (const uint32_t *)(uintptr_t)ga;
                size_t gnw = gn / 4; if (gnw > 4096) gnw = 4096;
                size_t gnz = gnw;
                for (size_t q = 0; q < gnw; q++) if (gu[q]) { gnz = q; break; }
                fprintf(stderr, "[totk-nvn] GlobalUbo[%d][%d] addr=%#llx size=%zu first_nz=%s%#zx",
                        gs, gi, (unsigned long long)ga, gn, gnz == gnw ? "none " : "",
                        gnz == gnw ? 0 : gnz * 4);
                for (size_t q = (gnz == gnw ? 0 : gnz); q < (gnz == gnw ? 0 : gnz + 8) && q < gnw; q++)
                    fprintf(stderr, " %08x", gu[q]);
                fprintf(stderr, "\n");
            }
            if (sb && nvn_submit_trace <= 8) for (int st2 = 0; st2 < 2; st2++) for (int ui = 0; ui < 16; ui++) {
                if (!(sb->ubo_mask[st2] & (1u << ui)) || !sb->ubo[st2][ui].address) continue;
                const uint32_t *u = (const uint32_t *)(uintptr_t)sb->ubo[st2][ui].address;
                size_t nw = sb->ubo[st2][ui].size / 4;
                if (nw > (1u << 16)) nw = 1u << 16;
                size_t nz = nw;
                for (size_t q = 0; q < nw; q++) if (u[q]) { nz = q; break; }
                fprintf(stderr, "[totk-nvn] Submit#%u ubo[%d][%d] addr=%#llx size=%zu first_nz=%s%#zx\n",
                        nvn_submit_trace - 1, st2, ui,
                        (unsigned long long)sb->ubo[st2][ui].address, sb->ubo[st2][ui].size,
                        nz == nw ? "none " : "", nz == nw ? 0 : nz * 4);
            }
        }
        prog_t *p = cb->prog;
        win_t *w = cb->win;
        int has_program = p && p->magic == NVN_MAGIC('P', 'R', 'O', 'G');
        int has_window = w && w->magic == NVN_MAGIC('W', 'I', 'N', 'D') &&
                         runtime_native_window_valid(w->native);
        /* A UI/scene command buffer may render only to offscreen guest
         * textures and never call SetWindow.  Do not discard it: the final
         * window pass samples its 0x3f target. */

        /* Resolve the pass target layout.  nvnCommandBufferSetRenderTargets
         * records it; a command buffer that only set a window uses that
         * window's single-target layout. */
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
        } else if (has_window) {
            nvn_window_layout(w, &layout, &rw, &rh);
            have_layout = layout.color_count > 0 && rw && rh;
        }
        if (!has_window && !have_layout) {
            /* A command buffer that only uploads, clears or dispatches compute
             * work is valid NVN: its GPU commands were replayed above. */
            if (!cb->draw_count) continue;
            nvn_fail("nvnQueueSubmitCommands", "command buffer has no target");
            continue;
        }
        int window_target = window_texture;
        int window_pass = has_window && (!have_layout ||
                          (window_target && nvn_layout_matches_window(w, &layout, rw, rh)));
        /* Why is a window pass an offscreen pass?  Report the window's guest
         * textures and the recorded target/size/format once per window cb. */
        if (has_window && nvn_trace_composite() && !window_pass) {
            static unsigned wl;
            if (wl++ < 16) {
                uint32_t ww = 0, wh = 0, wf = 0;
                runtime_native_window_info(w->native, &ww, &wh, &wf, NULL, NULL);
                GfxRenderTargetLayout wl2;
                uint32_t lw2 = 0, lh2 = 0;
                nvn_window_layout(w, &wl2, &lw2, &lh2);
                fprintf(stderr, "[totk-frame] window-miss cb=%p win=%p native=%p"
                                " win_texes=%d native=%ux%u fmt=%#x wlayout=%#x %ux%u"
                                " | target=0x%x tex0=%p %ux%u fmt=%#x matches=%d"
                                " mrt=%u depth=%#x\n",
                        (void *)cb, (void *)w, (void *)w->native, w->ntex, ww, wh, wf,
                        wl2.color_formats[0], lw2, lh2, window_target,
                        (const void *)(window_target ? cbt->color0_tex : NULL), rw, rh,
                        layout.color_count ? layout.color_formats[0] : 0,
                        nvn_layout_matches_window(w, &layout, rw, rh),
                        layout.color_count, layout.depth_format);
                for (int ti = 0; ti < w->ntex && ti < 4; ti++)
                    fprintf(stderr, "[totk-frame] window-miss   win.tex[%d]=%p\n", ti,
                            (const void *)w->tex[ti]);
            }
        }
        const nvn_bind_state_t *bs = nvn_cb_draw_state(cb);
        /* The window composite samples an alias object of the offscreen target's
         * texture.  If that alias never got the RT association, inherit it from
         * the last large scene pass before the sampler is translated. */
        if (window_pass && bs && g_last_scene_rt) {
            const tex_t *ws = nvn_window_sampled_tex(bs, cb);
            tex_bridge_t *wb = ws ? nvn_tex_bridge(ws, 0) : NULL;
            if (ws && (!wb || !wb->rt_valid)) {
                int same = ws == g_last_scene_color0 ||
                           (g_last_scene_tex && nvn_tex_same_resource(ws, g_last_scene_tex));
                if (same) {
                    nvn_tex_set_rt(ws, g_last_scene_rt, &g_last_scene_layout,
                                   g_last_scene_w, g_last_scene_h);
                    static unsigned alias_logged;
                    if (nvn_trace_composite() && alias_logged++ < 8)
                        fprintf(stderr, "[totk-frame] window sample alias tex=%p ->"
                                        " scene rt=%p %ux%u\n", (const void *)ws,
                                (void *)g_last_scene_rt, g_last_scene_w, g_last_scene_h);
                }
            }
        }
        /* Refresh the 0x3f source this window pass samples.  Cheap (small UI
         * targets only) and required for a correct composite; disable with
         * TOTK_ANDROID_NO_SAMPLED_DRAIN for A/B evidence. */
        if (window_pass) nvn_scene_submit_sampled_target(bs, cb);
        {
            static int uiflow = -1;
            if (uiflow < 0) uiflow = getenv("TOTK_ANDROID_TRACE_UI_FLOW") != NULL;
            if (uiflow && nvn_nvn_draws < 24) {
                fprintf(stderr, "[totk-ui] window cb=%p thread=%#x cmds=%u draws=%d pass=%d"
                                " target=%dx%d prog=%p samples:",
                        (void *)cb, runtime_thread_current_handle(), cb->ncmds,
                        cb->draw_count, window_pass, rw, rh, (void *)p);
                if (bs) for (int st = 0; st < 2; st++) for (int slot = 0; slot < 128; slot++) {
                    if (!(bs->tex_mask[st][slot / 32] & (1u << (slot % 32)))) continue;
                    const tex_pool_t *tp = nvn_tex_pool_for(cb);
                    const tex_t *tt = tp ? nvn_tex_pool_lookup(tp, (int)(bs->tex[st][slot] & 0xfffff)) : NULL;
                    fprintf(stderr, " %d:%d=%p", st, slot, (const void *)tt);
                    if (tt) fprintf(stderr, "(%#x %ux%u)", tt->d.format, tt->d.width, tt->d.height);
                }
                fputc('\n', stderr);
            }
        }
        if (nvn_trace_first() && nvn_nvn_draws < 4) {
            fprintf(stderr, "[totk-nvn] Submit cb=%p bs=%p va0=%#x vs0=%#llx legacy=%p window=%p win_target=%d"
                            " layout=%dx%d colors=%u depth=%#x have_layout=%d window_pass=%d"
                            " va_mask=%#x vs_mask=%#x vb_mask=%#x ubo0=%#x ssbo0=%#x tex0=%#x"
                            " tpool=%p spool=%p\n",
                    (void *)cb, (void *)bs, bs ? bs->va[0] : 0,
                    bs ? (unsigned long long)bs->vs[0] : 0, (void *)p, (void *)w, window_target, rw, rh,
                    layout.color_count, layout.depth_format, have_layout, window_pass,
                    bs ? bs->va_mask : 0, bs ? bs->vs_mask : 0, bs ? bs->vb_mask : 0,
                    bs ? bs->ubo_mask[0] : 0, bs ? bs->ssbo_mask[0] : 0,
                    bs ? bs->tex_mask[0][0] : 0,
                    (const void *)cb->tex_pool, (const void *)cb->samp_pool);
            if (bs) {
                for (int i = 0; i < 16; i++)
                    if (bs->ubo_mask[0] & (1u << i))
                        fprintf(stderr, "[totk-nvn] Submit cb=%p ubo[0][%d] addr=%#llx size=%zu\n",
                                (void *)cb, i, (unsigned long long)bs->ubo[0][i].address,
                                bs->ubo[0][i].size);
                for (int i = 0; i < 16; i++)
                    if (bs->ubo_mask[1] & (1u << i))
                        fprintf(stderr, "[totk-nvn] Submit cb=%p ubo[1][%d] addr=%#llx size=%zu\n",
                                (void *)cb, i, (unsigned long long)bs->ubo[1][i].address,
                                bs->ubo[1][i].size);
                for (int i = 0; i < 16; i++)
                    if ((bs->vb_mask & (1u << i)) && bs->vb[i].address)
                        fprintf(stderr, "[totk-nvn] Submit cb=%p vb[%d] addr=%#llx size=%zu\n",
                                (void *)cb, i, (unsigned long long)bs->vb[i].address,
                                bs->vb[i].size);
                if (nvn_nvn_draws < 2 && (bs->ubo_mask[0] & 1u) && bs->ubo[0][0].address) {
                    const uint32_t *u = (const uint32_t *)(uintptr_t)bs->ubo[0][0].address;
                    fprintf(stderr, "[totk-nvn] Submit cb=%p ubo0 hex:", (void *)cb);
                    for (int k = 0; k < 16; k++) fprintf(stderr, " %08x", u[k]);
                    fprintf(stderr, "\n");
                    size_t nw = bs->ubo[0][0].size / 4;
                    if (nw > (1u << 18)) nw = 1u << 18;
                    size_t nz = nw;
                    for (size_t k = 0; k < nw; k++) if (u[k]) { nz = k; break; }
                    if (nz == nw) {
                        fprintf(stderr, "[totk-nvn] Submit cb=%p ubo0 is all zero (%zu bytes)\n",
                                (void *)cb, bs->ubo[0][0].size);
                    } else {
                        fprintf(stderr, "[totk-nvn] Submit cb=%p ubo0 first nonzero +%#zx:",
                                (void *)cb, nz * 4);
                        for (size_t k = nz; k < nz + 16 && k < nw; k++)
                            fprintf(stderr, " %08x", u[k]);
                        fprintf(stderr, "\n");
                    }
                }
                if (nvn_nvn_draws < 2 && (bs->vb_mask & 1u) && bs->vb[0].address) {
                    const uint32_t *v = (const uint32_t *)(uintptr_t)bs->vb[0].address;
                    fprintf(stderr, "[totk-nvn] Submit cb=%p vb0 hex:", (void *)cb);
                    for (int k = 0; k < 16; k++) fprintf(stderr, " %08x", v[k]);
                    fprintf(stderr, "\n");
                }
                if (nvn_nvn_draws < 2) {
                    for (uint32_t ci = 0; ci < cb->ncmds; ci++) {
                        if (cb->cmds[ci].kind != NVN_CMD_DRAW_INDEXED) continue;
                        uint64_t ip = cb->cmds[ci].u.draw_indexed.indices;
                        const uint8_t *ib = (const uint8_t *)(uintptr_t)ip;
                        fprintf(stderr, "[totk-nvn] Submit cb=%p draw_indexed type=%u count=%u"
                                        " index_ptr=%#llx bytes=",
                                (void *)cb, cb->cmds[ci].u.draw_indexed.index_type,
                                cb->cmds[ci].u.draw_indexed.count, (unsigned long long)ip);
                        for (int k = 0; k < 16; k++) fprintf(stderr, "%02x", ib[k]);
                        fprintf(stderr, "\n");
                    }
                }
            }
        }
        GfxOffscreenTarget *rt = NULL;
        const void *rt_owner = NULL;
        if (have_layout && !window_pass) {
            nvn_cb_target_t *rt_target = nvn_cb_target(cb, 0);
            rt_owner = rt_target ? rt_target->color0_tex : NULL;
            rt = nvn_host_rt_get_owner(rw, rh, &layout, rt_owner);
            if (!rt) {
                nvn_fail("nvnQueueSubmitCommands", "cannot create host render-target layout");
                continue;
            }
        }

        GfxVertexLayout input;
        uint64_t vertex_sig = 0;
        nvn_build_vertex_layout(bs, &input, &vertex_sig);
        if (nvn_trace_first() && nvn_nvn_draws < 4) {
            for (uint32_t ci = 0; ci < cb->ncmds; ci++) {
                const nvn_cmd_t *cc = &cb->cmds[ci];
                if (cc->kind == NVN_CMD_SET_VIEWPORT || cc->kind == NVN_CMD_SET_SCISSOR)
                    fprintf(stderr, "[totk-nvn] Submit cb=%p cmd kind=%u rect=%d,%d %dx%d\n",
                            (void *)cb, cc->kind, cc->u.rect.x, cc->u.rect.y,
                            cc->u.rect.width, cc->u.rect.height);
                else if (cc->kind == NVN_CMD_CLEAR_COLOR)
                    fprintf(stderr, "[totk-nvn] Submit cb=%p cmd clear idx=%d rgba=%.3f,%.3f,%.3f,%.3f\n",
                            (void *)cb, cc->u.clear.index, cc->u.clear.rgba[0],
                            cc->u.clear.rgba[1], cc->u.clear.rgba[2], cc->u.clear.rgba[3]);
            }
            fprintf(stderr, "[totk-nvn] Submit cb=%p state cull=%u front=%u poly=%u dt=%u dw=%u df=%u"
                            " blend=%#x stencil=%u chmask=%#x\n", (void *)cb,
                    bs->rs.poly.cull, bs->rs.poly.front, bs->rs.poly.mode,
                    bs->rs.ds.dt, bs->rs.ds.dw, bs->rs.ds.df,
                    bs->rs.color.blend_en, bs->rs.ds.st, bs->rs.chmask.mask);
            fprintf(stderr, "[totk-nvn] Submit cb=%p vertex layout attrs=%u bindings=%u sig=%#llx\n",
                    (void *)cb, input.nattributes, input.nbindings,
                    (unsigned long long)vertex_sig);
            for (uint32_t ai = 0; ai < input.nattributes; ai++)
                fprintf(stderr, "[totk-nvn] Submit cb=%p attr loc=%u binding=%u fmt=%#x off=%u\n",
                        (void *)cb, input.attributes[ai].location, input.attributes[ai].binding,
                        input.attributes[ai].format, input.attributes[ai].offset);
            for (uint32_t bi = 0; bi < input.nbindings; bi++)
                fprintf(stderr, "[totk-nvn] Submit cb=%p vbind b=%u stride=%u rate=%u\n",
                        (void *)cb, input.bindings[bi].binding, input.bindings[bi].stride,
                        input.bindings[bi].input_rate);
        }
