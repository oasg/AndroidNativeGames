/* nvn_process_submit_commands: implementation phase 2. */
        prog_host_t ph = has_program ? prog_host_view(p) : (prog_host_t){0};
        GfxReflectedPipeline *reflected = has_program ? ph.gfx_reflected_pipeline : NULL;
        if (!reflected && has_program) {
            char variant[192];
            nvn_variant_suffix(p, cb, variant, sizeof variant);
            /* A window pass draws into the host surface, which has the window's
             * format and no depth attachment.  The recorded layout may carry a
             * depth target (the offscreen composite it samples), and passing it
             * here creates a pipeline the window target cannot match. */
            GfxRenderTargetLayout wlayout;
            const GfxRenderTargetLayout *pl = have_layout ? &layout : NULL;
            uint32_t pf = have_layout ? layout.color_formats[0] : 0;
            if (window_pass) {
                uint32_t ww = 0, wh = 0;
                nvn_window_layout(w, &wlayout, &ww, &wh);
                wlayout.depth_format = 0;
                pl = &wlayout;
                pf = nvn_window_format(w);
                if (!pf) pf = wlayout.color_formats[0];
            }
            reflected = nvn_program_cached_pipeline(p, pf, pl, window_pass,
                                                    input.nattributes ? &input : NULL,
                                                    vertex_sig, variant, 1);
        }

        if (nvn_trace_composite()) {
            char uibuf[512] = "";
            int nui = nvn_bs_samples_ui(bs, cb, uibuf, sizeof uibuf);
            fprintf(stderr, "[totk-frame] composite window target=%s %ux%u"
                            " program=%p draws=%d nui=%d%s%s\n",
                    window_pass ? "window" : "offscreen", rw, rh, (void *)p,
                    cb->draw_count, nui, uibuf[0] ? " ui=" : "", uibuf);
            if (window_pass || nui)
                nvn_trace_segment_inputs("composite-window", cb, bs, p, reflected);
        }
        int drew = 0;
        if (reflected) {
            uint32_t cap = (cb->ncmds ? cb->ncmds : 1) + 2;
            GfxCmd *translated = calloc(cap, sizeof *translated);
            if (!translated) {
                nvn_fail("nvnQueueSubmitCommands", "out of memory for translated commands");
                continue;
            }
            float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
            int has_clear = 0;
            /* A window pass draws into the host surface; if the recording never
             * set a usable viewport/scissor (the engine's final composite
             * inherits them from an earlier buffer) the draw lands in a
             * one-pixel band.  Seed the full window rectangle, like the segment
             * prefix does for the UI passes. */
            uint32_t pre = 0;
            if (window_pass && cap >= 2) {
                int have_vp = 0, have_sc = 0;
                for (uint32_t ci = 0; ci < cb->ncmds; ci++) {
                    if (cb->cmds[ci].kind == NVN_CMD_SET_VIEWPORT &&
                        cb->cmds[ci].u.rect.width > 0 && cb->cmds[ci].u.rect.height > 0)
                        have_vp = 1;
                    if (cb->cmds[ci].kind == NVN_CMD_SET_SCISSOR &&
                        cb->cmds[ci].u.rect.width > 0 && cb->cmds[ci].u.rect.height > 0)
                        have_sc = 1;
                }
                if (!have_vp) {
                    translated[pre].kind = GFX_CMD_SET_VIEWPORT;
                    translated[pre].u.rect.x = 0;
                    translated[pre].u.rect.y = 0;
                    translated[pre].u.rect.width = (int32_t)rw;
                    translated[pre].u.rect.height = (int32_t)rh;
                    pre++;
                }
                if (!have_sc) {
                    translated[pre].kind = GFX_CMD_SET_SCISSOR;
                    translated[pre].u.rect.x = 0;
                    translated[pre].u.rect.y = 0;
                    translated[pre].u.rect.width = (int32_t)rw;
                    translated[pre].u.rect.height = (int32_t)rh;
                    pre++;
                }
                if (nvn_trace_composite()) {
                    static unsigned vlog;
                    if (vlog++ < 6)
                        fprintf(stderr, "[totk-frame] window-pass seeded rects=%u"
                                        " %ux%u (vp=%d sc=%d)\n", pre, rw, rh, have_vp, have_sc);
                }
            }
            uint32_t count = nvn_translate_commands(cb, translated + pre, cap - pre,
                                                    window_pass ? NULL : rt,
                                                    clear, &has_clear) + pre;
            nvn_remap_texture_bind_cmds(translated, count, reflected);
            if (window_pass && nvn_trace_composite()) {
                static unsigned post_src, post_nz;
                const tex_t *st = nvn_window_sampled_tex(bs, cb);
                tex_bridge_t *sb = st ? nvn_tex_bridge(st, 0) : NULL;
                if (sb && sb->rt_valid) {
                    size_t n = 0;
                    const uint8_t *px = gfx_backend_offscreen_pixels(sb->rt, &n);
                    uint32_t nz = nvn_first_nonzero(px, n, 1u << 20);
                    int want = post_src < 4 ||
                               (nz != UINT32_MAX && post_nz < 4);
                    if (want) {
                        if (nz == UINT32_MAX) post_src++;
                        else post_nz++;
                        fprintf(stderr, "[totk-frame] window sampled tex=%p"
                                        " -> gfx=%p src=rt-gen gen=%llu/%llu"
                                        " rt=%p bytes=%zu first_nz=%s%u\n",
                                (const void *)st, sb->gfx,
                                (unsigned long long)sb->rt_frame,
                                (unsigned long long)sb->rt_uploaded,
                                (void *)sb->rt, n,
                                nz == UINT32_MAX ? "none " : "", nz == UINT32_MAX ? 0 : nz);
                    }
                }
            }
            nvn_bind_state_t bend = bs ? *bs : (nvn_bind_state_t){0};
            /* NVN uniform-buffer binds are queue-scoped, and this cb's draws can
             * predate a rebind that the engine intended for the whole
             * recording (same class as the UI consumer fix).  For the window
             * pass, take the recording's final stage-0 UBO state. */
            if (window_pass && cb->bind_state) {
                const nvn_bind_state_t *fin = cb->bind_state;
                for (int i = 0; i < 16; i++) {
                    if (!(fin->ubo_mask[0] & (1u << i))) continue;
                    if (!fin->ubo[0][i].address || !fin->ubo[0][i].size) continue;
                    if (bend.ubo[0][i].address == fin->ubo[0][i].address &&
                        bend.ubo[0][i].size == fin->ubo[0][i].size) continue;
                    if (nvn_trace_composite()) {
                        static unsigned wl;
                        if (wl++ < 8)
                            fprintf(stderr, "[totk-frame] window ubo override idx=%d"
                                            " snap=(%#llx,%zu) -> final=(%#llx,%zu)\n", i,
                                    (unsigned long long)bend.ubo[0][i].address,
                                    bend.ubo[0][i].size,
                                    (unsigned long long)fin->ubo[0][i].address,
                                    fin->ubo[0][i].size);
                    }
                    bend.ubo[0][i] = fin->ubo[0][i];
                    bend.ubo_mask[0] |= 1u << i;
                }
            }
            nvn_inherit_ubo_slots(&bend, reflected);
            nvn_apply_bind_state(p, reflected, &bend, cb);
            GfxVertexStream streams[24];
            void *owned[16];
            uint32_t nowned = 0;
            uint32_t need_v2 = nvn_fix_index_range(translated, count, bs, "submit");
            uint32_t draw_v2 = need_v2 > (uint32_t)cb->draw_count ? need_v2
                                                                 : (uint32_t)cb->draw_count;
            uint32_t nstreams = nvn_build_vertex_streams(bs, streams, 24, owned, 16,
                                                         &nowned, draw_v2);
            /* Diagnostic only: the same blend-off test for the window pass. */
            if (window_pass && getenv("TOTK_ANDROID_UI_NO_BLEND")) {
                for (uint32_t ci = 0; ci < count; ci++) {
                    if (translated[ci].kind == GFX_CMD_DRAW)
                        translated[ci].u.draw.state.blend_enable = 0;
                    else if (translated[ci].kind == GFX_CMD_DRAW_INDEXED)
                        translated[ci].u.draw_indexed.state.blend_enable = 0;
                }
                static unsigned wnb;
                if (wnb++ < 4)
                    fprintf(stderr, "[totk-frame] window-pass blend disabled (diagnostic)\n");
            }
            if (window_pass)
                drew = runtime_native_window_execute_reflected_ex(w->native, reflected,
                                                                  translated, count,
                                                                  has_clear ? clear : NULL,
                                                                  streams, nstreams);
            GfxOffscreenTarget *window_present_rt = NULL;
            const tex_t *window_present_tex = NULL;
            if (window_pass)
                window_present_rt = nvn_window_present_rt(bs, cb, &window_present_tex);
            if (window_pass && has_window && window_present_rt) {
                /* The window composite's own draw covers only the fade overlay.
                 * The frame content is the offscreen target its 0x3f sampler
                 * reads, so present that RT (route the sampled texture's RT, not
                 * the overlay). */
                runtime_native_window_set_present_target(w->native, window_present_rt);
                g_present_rt = window_present_rt;
                if (nvn_trace_composite()) {
                    static unsigned sl;
                    if (sl++ < 8) {
                        uint32_t fnz = UINT32_MAX;
                        uint8_t cpx[4] = { 0 };
                        tex_bridge_t *wb = window_present_tex
                                         ? nvn_tex_bridge(window_present_tex, 0) : NULL;
                        nvn_target_first_nz(window_present_rt, &fnz, cpx);
                        fprintf(stderr, "[totk-frame] window presents scene rt=%p %ux%u"
                                        " sampled_tex=%p rt_valid=%d gen=%llu first_nz=%s%u"
                                        " center=%02x%02x%02x%02x (overlay dropped)\n",
                                (void *)window_present_rt,
                                gfx_backend_offscreen_width(window_present_rt),
                                gfx_backend_offscreen_height(window_present_rt),
                                (const void *)window_present_tex, wb ? wb->rt_valid : 0,
                                wb ? (unsigned long long)wb->rt_frame : 0ull,
                                fnz == UINT32_MAX ? "none " : "", fnz == UINT32_MAX ? 0 : fnz,
                                cpx[0], cpx[1], cpx[2], cpx[3]);
                    }
                }
                const char *pp = getenv("TOTK_ANDROID_PRESENT_PPM");
                if (pp && *pp) {
                    /* Dump only once the target holds frame content: the first
                     * window submit of a recording can precede the text pass. */
                    static int dumped;
                    uint32_t fnz = UINT32_MAX;
                    uint8_t cpx[4] = { 0 };
                    nvn_target_first_nz(window_present_rt, &fnz, cpx);
                    int content = fnz != UINT32_MAX ||
                                  cpx[0] | cpx[1] | cpx[2] | cpx[3];
                    if (!dumped && content) {
                        dumped = 1;
                        int ok = gfx_backend_offscreen_write_ppm_index(window_present_rt, 0, pp);
                        fprintf(stderr, "[totk-frame] present ppm -> %s (%s) first_nz=%u"
                                        " center=%02x%02x%02x%02x\n", pp,
                                ok ? "ok" : "failed", fnz,
                                cpx[0], cpx[1], cpx[2], cpx[3]);
                    }
                }
            }
            if (window_pass && nvn_trace_composite()) {
                static unsigned wp_logged;
                if (wp_logged++ < 6) {
                    nvn_dump_vertex_stream("window", &input, bs, 4);
                    for (uint32_t ci = 0; ci < count && ci < 16; ci++) {
                        const GfxCmd *dg = &translated[ci];
                        const GfxPipelineState *st = NULL;
                        if (dg->kind == GFX_CMD_DRAW) st = &dg->u.draw.state;
                        else if (dg->kind == GFX_CMD_DRAW_INDEXED) st = &dg->u.draw_indexed.state;
                        if (!st) continue;
                        fprintf(stderr, "[totk-frame] window-pass   blend en=%u src_rgb=%u"
                                        " dst_rgb=%u src_a=%u dst_a=%u eq_rgb=%u eq_a=%u"
                                        " chmask=%#x | nvn packed=%llu\n",
                                st->blend_enable, st->blend_src_rgb, st->blend_dst_rgb,
                                st->blend_src_a, st->blend_dst_a, st->blend_eq_rgb,
                                st->blend_eq_a, st->color_write_mask,
                                (unsigned long long)bs->rs.blend.src_rgb |
                                    ((unsigned long long)bs->rs.blend.dst_rgb << 7) |
                                    ((unsigned long long)bs->rs.blend.src_a << 14) |
                                    ((unsigned long long)bs->rs.blend.dst_a << 21));
                        break;
                    }
                    fprintf(stderr, "[totk-frame] window-pass cb=%p rp=%p count=%u draws=%d"
                                    " streams=%u clear=%d texmask=%#x/%#x va=%#x vb=%#x"
                                    " vb0=%#llx/%zu\n", (void *)cb, (void *)reflected, count,
                            cb->draw_count, nstreams, has_clear, bend.tex_mask[0][0],
                            bend.tex_mask[1][0], bend.va_mask, bend.vb_mask,
                            (unsigned long long)bend.vb[0].address, bend.vb[0].size);
                    for (uint32_t ci = 0; ci < count && ci < 16; ci++) {
                        const GfxCmd *g = &translated[ci];
                        if (g->kind == GFX_CMD_DRAW)
                            fprintf(stderr, "[totk-frame] window-pass   draw prim=%u"
                                            " first=%u count=%u blend=%u\n",
                                    g->u.draw.primitive, g->u.draw.first, g->u.draw.count,
                                    g->u.draw.state.blend_enable);
                        else if (g->kind == GFX_CMD_DRAW_INDEXED)
                            fprintf(stderr, "[totk-frame] window-pass   drawi prim=%u"
                                            " count=%u type=%u\n", g->u.draw_indexed.primitive,
                                    g->u.draw_indexed.count, g->u.draw_indexed.index_type);
                        else if (g->kind == GFX_CMD_CLEAR_COLOR)
                            fprintf(stderr, "[totk-frame] window-pass   clear rgba=%.3f,"
                                            "%.3f,%.3f,%.3f mask=%#x\n", g->u.clear.rgba[0],
                                    g->u.clear.rgba[1], g->u.clear.rgba[2], g->u.clear.rgba[3],
                                    g->u.clear.mask);
                        else if (g->kind == GFX_CMD_SET_VIEWPORT)
                            fprintf(stderr, "[totk-frame] window-pass   viewport %d,%d %dx%d\n",
                                    g->u.rect.x, g->u.rect.y, g->u.rect.width,
                                    g->u.rect.height);
                        else if (g->kind == GFX_CMD_SET_SCISSOR)
                            fprintf(stderr, "[totk-frame] window-pass   scissor %d,%d %dx%d\n",
                                    g->u.rect.x, g->u.rect.y, g->u.rect.width,
                                    g->u.rect.height);
                    }
                    if (p) {
                        fprintf(stderr, "[totk-frame] window-pass   prog-history:");
                        for (int hi = 0; hi < 16; hi++) {
                            uint64_t ha = 0;
                            size_t hn = 0;
                            if (nvn_prog_ubo_get(p, 0, hi, &ha, &hn))
                                fprintf(stderr, " i%d=(%#llx,%zu)", hi,
                                        (unsigned long long)ha, hn);
                        }
                        fputc('\n', stderr);
                    }
                    for (int i = 0; i < 16; i++) {
                        if (!g_ubo_dec[0][i].valid) continue;
                        const uint32_t *w = (const uint32_t *)(uintptr_t)g_ubo_dec[0][i].addr;
                        size_t nw = g_ubo_dec[0][i].size / 4;
                        fprintf(stderr, "[totk-frame] window-pass   c%d<=idx%d(%#llx,%zu)"
                                " src=%s", i, g_ubo_dec[0][i].nvn,
                                (unsigned long long)g_ubo_dec[0][i].addr,
                                g_ubo_dec[0][i].size,
                                g_ubo_dec[0][i].src ? g_ubo_dec[0][i].src : "?");
                        if (w && nw >= 16) {
                            static const int rows[7] = { 1, 2, 3, 4, 5, 6, 33 };
                            for (int r = 0; r < 7; r++) {
                                int base = rows[r] * 4;
                                if ((size_t)base + 4 > nw) continue;
                                fprintf(stderr, " %d=%08x,%08x,%08x,%08x", rows[r],
                                        w[base], w[base + 1], w[base + 2], w[base + 3]);
                            }
                        }
                        fputc('\n', stderr);
                    }
                }
            }
            else
                drew = gfx_backend_reflected_execute_ex(reflected, rt, translated, count,
                                                        has_clear ? clear : NULL,
                                                        streams, nstreams);
            for (uint32_t oi = 0; oi < nowned; oi++) free(owned[oi]);
            free(translated);
            if (drew && !window_pass) {
                if (has_window) nvn_window_mirror_target(w, rt);
                const tex_t *ctex = nvn_tex_registry_find(rt_owner);
                if (ctex) nvn_tex_set_rt(ctex, rt, &layout, rw, rh);
                /* The engine renders the scene into an offscreen target and its
                 * window composite samples only the fade overlay, so the scene
                 * never reaches the surface.  Present the pass that actually
                 * holds the frame content (no invented pixels). */
                if (has_window && rt && rw >= 1024 && rh >= 1024) {
                    runtime_native_window_set_present_target(w->native, rt);
                    g_present_rt = rt;
                    if (nvn_trace_composite()) {
                        static unsigned pl;
                        if (pl++ < 8)
                            fprintf(stderr, "[totk-frame] present-target rt=%p %ux%u"
                                            " fmt=%#x (offscreen pass)\n", (void *)rt,
                                    rw, rh, layout.color_formats[0]);
                    }
                    const char *pp = getenv("TOTK_ANDROID_PRESENT_PPM");
                    if (pp && *pp) {
                        static int dumped;
                        if (!dumped) {
                            dumped = 1;
                            int ok = gfx_backend_offscreen_write_ppm_index(rt, 0, pp);
                            fprintf(stderr, "[totk-frame] present ppm -> %s (%s)\n", pp,
                                    ok ? "ok" : "failed");
                        }
                    }
                }
            }
        } else if (has_window && has_program && ph.textured &&
                   ph.gfx_textured_pipeline && ph.gfx_texture) {
            drew = runtime_native_window_draw_textured_triangle(w->native, ph.gfx_textured_pipeline,
                                                                ph.gfx_texture);
        } else if (has_window && has_program && ph.gfx_pipeline) {
            drew = runtime_native_window_draw_triangle_pipeline(w->native, ph.gfx_pipeline);
        } else {
            /* Clear-only command buffer, or a program whose shaders are not in
             * the cache: honour the recorded clear and present a real frame
             * instead of inventing a shader. */
            float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
            int has_clear = 0;
            GfxCmd ignored[1];
            (void)nvn_translate_commands(cb, ignored, 1, NULL, clear, &has_clear);
            if (!window_pass && rt)
                drew = gfx_backend_offscreen_clear(rt, clear[0], clear[1], clear[2], clear[3]);
            else if (has_window)
                drew = runtime_native_window_clear_rgba(w->native, clear);
            if (drew && !window_pass) {
                if (has_window) nvn_window_mirror_target(w, rt);
                const tex_t *ctex = nvn_tex_registry_find(rt_owner);
                if (ctex) nvn_tex_set_rt(ctex, rt, &layout, rw, rh);
            }
            static unsigned fallback_logged;
            if (fallback_logged++ < 8)
                fprintf(stderr, "[totk-nvn] no cached pipeline for program=%p: clear-only frame\n",
                        (void *)p);
        }
        if (!drew) {
            nvn_fail("nvnQueueSubmitCommands", "Vulkan draw failed");
            continue;
        }
        static int l2_frame_dumped;
        const char *l2_frame = getenv("TOTK_ANDROID_L2_FRAME");
        const char *l2_at_s = getenv("TOTK_ANDROID_L2_FRAME_AT");
        unsigned l2_at = l2_at_s && *l2_at_s ? (unsigned)strtoul(l2_at_s, NULL, 0) : 0;
        /* When the frame dump is not pinned to a draw index, wait until a
         * retired scene pass has actually rendered (the worker retires it a
         * few frames after the first compositor submit); fall back to draw 60
         * if no scene pass ever renders so the probe still produces a frame. */
        int scene_ready = nvn_scene_rendered > 0 || nvn_empty_sampled > 0 ||
                          (nvn_scene_queued == 0 && nvn_nvn_draws >= 4000);
        int dump_now = l2_at
            ? (nvn_nvn_draws == l2_at)
            : (scene_ready && nvn_nvn_draws >= 1);
        const char *l2_every_s = getenv("TOTK_ANDROID_L2_FRAME_EVERY");
        unsigned l2_every = l2_every_s && *l2_every_s
                          ? (unsigned)strtoul(l2_every_s, NULL, 0) : 0;
        if (has_window && l2_frame && *l2_frame && l2_every &&
            nvn_nvn_draws && (nvn_nvn_draws % l2_every) == 0) {
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s-%u.ppm", l2_frame, nvn_nvn_draws);
            int shot = nvn_window_frame_ppm(w, path);
            fprintf(stderr, "[totk-nvn] periodic frame #%u -> %s (%s)\n",
                    nvn_nvn_draws, path, shot ? "ok" : "failed");
        }
        if (has_window && l2_frame && *l2_frame && !l2_frame_dumped && dump_now) {
            l2_frame_dumped = 1;
            int shot = nvn_window_frame_ppm(w, l2_frame);
            if (l2_at)
                fprintf(stderr, "[totk-nvn] reflected frame #%u -> %s (%s)\n",
                        nvn_nvn_draws, l2_frame, shot ? "ok" : "failed");
            else
                fprintf(stderr, "[totk-nvn] first reflected frame -> %s (%s)\n",
                        l2_frame, shot ? "ok" : "failed");
        }
        unsigned total = __atomic_add_fetch(&nvn_nvn_draws, 1, __ATOMIC_RELAXED);
        if (getenv("TOTK_ANDROID_TRACE_LOAD") && (total % 300) == 0) {
            extern unsigned long runtime_fs_ops(void);
            extern const char *runtime_fs_last_op(void);
            extern unsigned long runtime_hid_polls(void);
            extern uint32_t runtime_hid_buttons(void);
            extern unsigned runtime_nvn_tex_pool_regs(void);
            extern unsigned runtime_oe_try_calls(void);
            extern unsigned runtime_oe_pop_calls(void);
            extern unsigned runtime_fs_errors(void);
            extern unsigned runtime_romfs_missing(void);
            fprintf(stderr, "[totk-load] draw=%u fs=%lu last=\"%s\" texpool=%u"
                            " fs_err=%u romfs_miss=%u oe_try=%u oe_pop=%u hid=%lu buttons=%#x scene=%u/%u\n",
                    total, runtime_fs_ops(), runtime_fs_last_op(),
                    runtime_nvn_tex_pool_regs(), runtime_fs_errors(), runtime_romfs_missing(),
                    runtime_oe_try_calls(),
                    runtime_oe_pop_calls(), runtime_hid_polls(), runtime_hid_buttons(),
                    nvn_scene_rendered, nvn_scene_submits);
        }
        if (getenv("TOTK_ANDROID_TRACE_TITLE") && (total % 300) == 0) {
            extern unsigned long runtime_fs_ops(void);
            extern const char *runtime_fs_last_op(void);
            extern unsigned long runtime_hid_polls(void);
            extern uint32_t runtime_hid_buttons(void);
            fprintf(stderr, "[totk-title] draw=%u record=%u scene=%u/%u queued=%u empty=%u"
                            " fs=%lu last=\"%s\" hid=%lu buttons=%#x\n",
                    total, nvn_record_count, nvn_scene_rendered, nvn_scene_submits,
                    nvn_scene_queued, nvn_empty_sampled, runtime_fs_ops(),
                    runtime_fs_last_op(), runtime_hid_polls(), runtime_hid_buttons());
        }
        if (total <= 32 || (total % 300) == 0 || nvn_trace())
            fprintf(stderr, "[totk-nvn] NVN draw #%u program=%p window=%p vertices=%d%s%s\n",
                    total, (void *)p, (void *)w, cb->draw_count,
                    reflected ? " reflected" : " fallback",
                    window_pass ? "" : " mrt");
    }
