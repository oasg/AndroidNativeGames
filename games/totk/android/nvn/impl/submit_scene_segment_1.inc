/* nvn_submit_scene_snapshot: implementation phase 1. */
    int drew = 0;
    uint32_t tl_count = 0, tl_streams = 0, tl_stream0 = 0;
    pthread_mutex_lock(&g_retired_lock);
    nvn_retired_t *e = NULL;
    for (unsigned i = 0; i < NVN_RETIRED_MAX; i++)
        if (g_retired[i].used && g_retired[i].handle == handle) { e = &g_retired[i]; break; }
    if (!e || seg_index >= e->nseg) {
        pthread_mutex_unlock(&g_retired_lock);
        return 0;
    }
    nvn_seg_t *sg = &e->seg[seg_index];
    if ((!sg->draw_count && !sg->clear_count) || !sg->have_layout ||
        sg->window_texture || !sg->color0_tex || !sg->ncmds) {
        if (nvn_ui_seg_want())
            fprintf(stderr, "[totk-ui-seg] skip handle=%#llx seg=%u reason=%s"
                            " draws=%u clears=%u layout=%d win=%d color0=%p ncmds=%u\n",
                    (unsigned long long)handle, seg_index, nvn_seg_reason(sg, e),
                    sg->draw_count, sg->clear_count, sg->have_layout,
                    sg->window_texture, sg->color0_tex, sg->ncmds);
        pthread_mutex_unlock(&g_retired_lock);
        return 0;
    }
    /* A segment that only clears its target still changes the image later
     * draws sample.  Execute the clear in recording order and associate the
     * RT, but do not count it as a rendered scene pass. */
    if (sg->draw_count == 0) {
        GfxRenderTargetLayout clayout = sg->layout;
        GfxOffscreenTarget *crt = nvn_host_rt_get_owner(sg->width, sg->height,
                                                        &clayout, sg->color0_tex);
        if (!crt) {
            pthread_mutex_unlock(&g_retired_lock);
            return 0;
        }
        int cleared = 0;
        if (sg->clear_count) {
            if (nvn_trace_inputs())
                fprintf(stderr, "[totk-frame] clear-begin %ux%u rgba=%.3f,%.3f,%.3f,%.3f\n",
                        sg->width, sg->height, sg->clear_rgba[0], sg->clear_rgba[1],
                        sg->clear_rgba[2], sg->clear_rgba[3]);
            cleared = gfx_backend_offscreen_clear(crt, sg->clear_rgba[0],
                                                  sg->clear_rgba[1], sg->clear_rgba[2],
                                                  sg->clear_rgba[3]);
            if (nvn_trace_inputs())
                fprintf(stderr, "[totk-frame] clear-done %ux%u ok=%d\n",
                        sg->width, sg->height, cleared);
        }
        const tex_t *cctex = nvn_tex_registry_find(sg->color0_tex);
        if (cleared && cctex)
            nvn_tex_set_rt(cctex, crt, &clayout, sg->width, sg->height);
        int sampled = 0;
        if (cctex) {
            for (uint32_t j = seg_index + 1; j < e->nseg && !sampled; j++) {
                const nvn_seg_t *later = &e->seg[j];
                if (!later->have_last) continue;
                const nvn_bind_state_t *lb = &later->last.state;
                for (int st = 0; st < 2 && !sampled; st++)
                    for (int slot = 0; slot < 128; slot++) {
                        if (!(lb->tex_mask[st][slot / 32] & (1u << (slot % 32)))) continue;
                        const tex_t *lt = e->tex_pool
                            ? nvn_tex_pool_lookup(e->tex_pool,
                                                  (int)(lb->tex[st][slot] & 0xfffff))
                            : NULL;
                        if (!lt) {
                            uint64_t gh = 0;
                            const tex_t *gt = NULL;
                            if (nvn_global_tex_get(st, slot, &gh, &gt) &&
                                gh == lb->tex[st][slot])
                                lt = gt;
                        }
                        if (lt && (lt == cctex || nvn_tex_same_resource(lt, cctex))) {
                            sampled = 1;
                            break;
                        }
                    }
            }
        }
        /* The sampling draw often inherits its texture slot from the queue
         * table instead of carrying the bind in its own snapshot.  Check the
         * queue-visible slots as well. */
        if (cctex && !sampled) {
            for (int st = 0; st < 2 && !sampled; st++)
                for (int slot = 0; slot < 128; slot++) {
                    uint64_t gh = 0;
                    const tex_t *gt = NULL;
                    if (!nvn_global_tex_get(st, slot, &gh, &gt) || !gt) continue;
                    if (gt == cctex || nvn_tex_same_resource(gt, cctex)) {
                        sampled = 1;
                        break;
                    }
                }
        }
        e->age = ++g_retired_age;
        nvn_scene_submits++;
        uint32_t ncmds = sg->ncmds, ccount = sg->clear_count;
        float cr = sg->clear_rgba[0], cg = sg->clear_rgba[1];
        float cb = sg->clear_rgba[2], ca = sg->clear_rgba[3];
        uint32_t cmask = sg->clear_mask;
        uint32_t first = sg->first_cmd;
        pthread_mutex_unlock(&g_retired_lock);
        if (nvn_trace_composite())
            fprintf(stderr, "[totk-frame] composite clear handle=%#llx seg=%u target=%p"
                            " %ux%u clears=%u rgba=%.3f,%.3f,%.3f,%.3f cleared=%d\n",
                    (unsigned long long)handle, seg_index, sg->color0_tex, sg->width,
                    sg->height, sg->clear_count, cr, cg, cb, ca, cleared);
        if (nvn_trace_frame() || nvn_trace_inputs())
            fprintf(stderr, "[totk-frame] clear-seg handle=%#llx seg=%u cmd=[%u,%u)"
                            " target=%p %ux%u clears=%u rgba=%.3f,%.3f,%.3f,%.3f"
                            " mask=%#x cleared=%d sampled_later=%d\n",
                    (unsigned long long)handle, seg_index, first, first + ncmds,
                    sg->color0_tex, sg->width, sg->height, ccount, cr, cg, cb, ca,
                    cmask, cleared, sampled);
        return 0;
    }
    const nvn_bind_state_t *seed = (sg->have_last && sg->last.have_state)
                                 ? &sg->last.state : e->bind;
    if (!seed) {
        if (nvn_ui_seg_want())
            fprintf(stderr, "[totk-ui-seg] skip handle=%#llx seg=%u reason=no-bind-state"
                            " last=%d\n", (unsigned long long)handle, seg_index,
                    sg->have_last);
        pthread_mutex_unlock(&g_retired_lock);
        return 0;
    }
    /* Fill texture slots the draw buffer did not rebind from the queue-scoped
     * bind state.  TOTK binds the font/atlas on a state-only recording and
     * relies on it staying live for later draw buffers. */
    nvn_bind_state_t eff = *seed;
    /* NVN uniform-buffer binds are queue-scoped: the last bind of a cbuf index
     * is what the GPU reads for draws that did not rebind it.  A UI recording
     * interleaves several programs that all use cbuf index 0 for their own c3,
     * so a segment's draw-time snapshot can hold a different program's c3
     * (observed: the 360x69 UI chain read a 64 KiB 1920x1080 projection instead
     * of its own 560-byte transform, which collapsed every quad off-screen).
     * The recording's final bind state is the last bind and is what the
     * producer segment already uses, so prefer it for the small UI passes. */
    if (sg->width <= 512 && e->bind) {
        for (int i = 0; i < 16; i++) {
            if (!(e->bind->ubo_mask[0] & (1u << i))) continue;
            if (!e->bind->ubo[0][i].address || !e->bind->ubo[0][i].size) continue;
            if (eff.ubo[0][i].address == e->bind->ubo[0][i].address &&
                eff.ubo[0][i].size == e->bind->ubo[0][i].size)
                continue;
            if (nvn_ui_seg_want()) {
                static unsigned lim;
                if (lim++ < 16)
                    fprintf(stderr, "[totk-ui-seg] ubo override seg=%u idx=%d"
                                    " snap=(%#llx,%zu) -> final=(%#llx,%zu)\n",
                            seg_index, i,
                            (unsigned long long)eff.ubo[0][i].address, eff.ubo[0][i].size,
                            (unsigned long long)e->bind->ubo[0][i].address,
                            e->bind->ubo[0][i].size);
            }
            eff.ubo[0][i] = e->bind->ubo[0][i];
            eff.ubo_mask[0] |= 1u << i;
        }
    }
    prog_t *p = e->prog;
    uint32_t ntex_spec[2] = { 0, 0 };
    {
        prog_shader_state_t *ss = sg->pstate;
        if (!ss && p && p->magic == NVN_MAGIC('P', 'R', 'O', 'G')) {
            pthread_mutex_lock(&g_prog_lock);
            ss = prog_state_find(p);
            pthread_mutex_unlock(&g_prog_lock);
        }
        if (ss) {
            for (int st = 0; st < 2; st++)
                for (uint32_t k = 0; k < ss->nspecs[st]; k++)
                    if (ss->specs[st][k].kind == GFX_BIND_COMBINED_IMAGE_SAMPLER ||
                        ss->specs[st][k].kind == GFX_BIND_SAMPLED_IMAGE)
                        ntex_spec[st]++;
        }
    }
    for (int st = 0; st < 2; st++) {
        uint32_t limit = ntex_spec[st] < 8 ? ntex_spec[st] : 8;
        for (uint32_t slot = 0; slot < limit; slot++) {
            if (eff.tex_mask[st][slot / 32] & (1u << (slot % 32))) continue;
            uint64_t gh = 0;
            const tex_t *gt = NULL;
            if (!nvn_global_tex_get(st, (int)slot, &gh, &gt) || !gt || !gh) continue;
            eff.tex[st][slot] = gh;
            eff.tex_mask[st][slot / 32] |= 1u << (slot % 32);
        }
    }
    if (getenv("TOTK_ANDROID_SEG_COPY")) {
        for (int i = 0; i < 16; i++)
            if (sg->vb_copy[i]) {
                eff.vb[i].address = (uint64_t)(uintptr_t)sg->vb_copy[i];
                eff.vb[i].size = sg->vb_copy_size[i];
                eff.vb_mask |= 1u << i;
            }
        for (int st = 0; st < 2; st++)
            for (int slot = 0; slot < 16; slot++)
                if (sg->ubo_copy[st][slot]) {
                    eff.ubo[st][slot].address = (uint64_t)(uintptr_t)sg->ubo_copy[st][slot];
                    eff.ubo[st][slot].size = sg->ubo_copy_size[st][slot];
                    eff.ubo_mask[st] |= 1u << slot;
                }
    }
    const nvn_bind_state_t *bs = &eff;
    /* Read-only view of the segment for the translation helpers. */
    cb_t view;
    memset(&view, 0, sizeof view);
    view.magic = NVN_MAGIC('C', 'M', 'D', 'B');
    view.dev = e->dev;
    view.cmds = e->cmds + sg->first_cmd;
    view.ncmds = sg->ncmds;
    view.bind_state = (void *)bs;
    view.prog = e->prog;
    view.win = e->win;
    view.tex_pool = e->tex_pool;
    view.samp_pool = e->samp_pool;
    view.draw_count = (int)sg->draw_count;
    view.handle = e->handle;
    view.serial = e->handle;
    /* Replay every uniform update from the start of the recording up to this
     * segment.  Draw-less prefix segments (clear/state only) are skipped by the
     * queue, but the engine records the per-frame UBO table there; replaying
     * only the segment's own range leaves the later draws reading zeros. */
    cb_t upd = view;
    upd.cmds = e->cmds;
    upd.ncmds = sg->first_cmd + sg->ncmds;
    nvn_cb_apply_uniform_updates(&upd);

    GfxRenderTargetLayout layout = sg->layout;
    uint32_t lw = sg->width, lh = sg->height;

    GfxOffscreenTarget *rt = nvn_host_rt_get_owner(lw, lh, &layout, sg->color0_tex);
    if (!rt) {
        pthread_mutex_unlock(&g_retired_lock);
        nvn_fail("nvnQueueSubmitCommands", "scene segment: cannot create host render target");
        return 0;
    }
    if (getenv("TOTK_ANDROID_TRACE_SCENE_STEPS"))
        fprintf(stderr, "[totk-scene-step] rt %ux%u fmt=%#x owner=%p rt=%p warm=%d"
                        " pipeline=%p pstate=%p draws=%u\n",
                lw, lh, layout.color_formats[0], sg->color0_tex, (void *)rt,
                sg->warm, (void *)sg->pipeline, (void *)sg->pstate, sg->draw_count);
    const tex_t *ctex = nvn_tex_registry_find(sg->color0_tex);
    int has_program = sg->warm || (p && p->magic == NVN_MAGIC('P', 'R', 'O', 'G'));
    prog_host_t ph = (has_program && !sg->warm) ? prog_host_view(p) : (prog_host_t){0};
    GfxVertexLayout input;
    uint64_t sig = 0;
    nvn_build_vertex_layout(bs, &input, &sig);
    GfxReflectedPipeline *rp = sg->warm ? sg->pipeline
                                       : (has_program ? ph.gfx_reflected_pipeline : NULL);
    if (!rp && has_program && !sg->warm) {
        char variant[192];
        nvn_variant_suffix(p, &view, variant, sizeof variant);
        rp = nvn_program_cached_pipeline(p, (uint32_t)layout.color_formats[0], &layout,
                                         0, input.nattributes ? &input : NULL, sig,
                                         variant, 0);
    }
    if (getenv("TOTK_ANDROID_TRACE_SCENE_STEPS"))
        fprintf(stderr, "[totk-scene-step] pipeline rp=%p prog=%p draws=%u texmask=%#x/%#x\n",
                (void *)rp, (void *)p, sg->draw_count, bs->tex_mask[0][0], bs->tex_mask[1][0]);
    nvn_inherit_ubo_slots(&eff, rp);
    uint32_t draw_mask0 = seed->ubo_mask[0];
    if (nvn_trace_inputs() && lw <= 512) {
        static unsigned ubo_traces;
        if (ubo_traces++ < 4) {
            const nvn_pipe_ubo_t *ps = nvn_pipe_ubo_get(rp);
            fprintf(stderr, "[totk-frame] ubo-inherit %ux%u drawmask0=%#x drawmask1=%#x"
                            " shader_slots0=", lw, lh, draw_mask0, seed->ubo_mask[1]);
            if (ps) for (uint32_t k = 0; k < ps->nubo[0]; k++)
                fprintf(stderr, " %d", ps->ubo_slot[0][k]);
            fprintf(stderr, " nspec0=%u aftermask0=%#x\n", ps ? ps->nubo[0] : 0,
                    bs->ubo_mask[0]);
            for (int slot = 0; slot < 8; slot++) {
                uint64_t ga = 0;
                size_t gn = 0;
                int gv = nvn_global_ubo_get(0, slot, &ga, &gn);
                int dv = (draw_mask0 & (1u << slot)) != 0;
                int iv = !dv && (bs->ubo_mask[0] & (1u << slot)) != 0;
                int used = 0;
                if (ps) for (uint32_t k = 0; k < ps->nubo[0]; k++)
                    if (ps->ubo_slot[0][k] == slot) used = 1;
                uint64_t a = dv ? seed->ubo[0][slot].address : ga;
                size_t n = dv ? seed->ubo[0][slot].size : gn;
                uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
                if (a) {
                    memcpy(&w0, (const void *)(uintptr_t)a, 4);
                    memcpy(&w1, (const void *)(uintptr_t)a + 4, 4);
                    memcpy(&w2, (const void *)(uintptr_t)a + 8, 4);
                    memcpy(&w3, (const void *)(uintptr_t)a + 12, 4);
                }
                fprintf(stderr, "[totk-frame] ubo-inherit   slot%d src=%s addr=%#llx size=%zu"
                                " first=%08x %08x %08x %08x global=%d used=%d\n",
                        slot, dv ? "draw" : (iv ? "inherit" : (gv ? "default" : "none")),
                        (unsigned long long)a, n, w0, w1, w2, w3, gv, used);
            }
        }
    }
    if (nvn_trace_composite()) {
        char uibuf[512] = "";
        int nui = nvn_bs_samples_ui(bs, &view, uibuf, sizeof uibuf);
        int big = lw >= 1000 || lh >= 1000;
        fprintf(stderr, "[totk-frame] composite seg handle=%#llx seq=%llu seg=%u target=%p %ux%u"
                        " color0=%p draws=%u nui=%d%s%s\n",
                (unsigned long long)handle, (unsigned long long)e->record_seq, seg_index,
                (void *)rt, lw, lh,
                sg->color0_tex, sg->draw_count, nui, uibuf[0] ? " ui=" : "", uibuf);
        if (big || nui)
            nvn_trace_segment_inputs(big ? "composite-3f" : "composite-ui",
                                     &view, bs, p, rp);
    }
    int traced = 0;
    if (nvn_trace_frame() || nvn_trace_inputs()) {
        if (lw <= 512) {
            static unsigned ui_traces, ui180;
            int want = ui_traces++ < 20 || (lw == 180 && ui180++ < 4);
            if (want) {
                char tag[48];
                snprintf(tag, sizeof tag, "ui-inputs[%ux%u]", lw, lh);
                nvn_trace_segment_inputs(tag, &view, bs, p, rp);
                traced = 1;
            }
        } else if (lw >= 1000) {
            static unsigned scene_traces;
            if (scene_traces++ < 1) {
                nvn_trace_segment_inputs("scene-inputs", &view, bs, p, rp);
                traced = 1;
            }
        }
    }
    if (rp) {
        /* UI recordings usually set their 360x69 viewport/scissor once, before
         * the first SetRenderTargets.  The per-target split starts each later
         * segment at its SetRenderTargets, so without this replay those
         * segments rasterize with the 0x0 default viewport. */
        nvn_cmd_t prefix[2];
        uint32_t npre = 0;
        if (lw <= 512) {
            const nvn_cmd_t *vp = NULL, *sc = NULL;
            int seg_vp = 0, seg_sc = 0;
            for (uint32_t ci = 0; ci < sg->first_cmd; ci++) {
                if (e->cmds[ci].kind == NVN_CMD_SET_VIEWPORT) vp = &e->cmds[ci];
                else if (e->cmds[ci].kind == NVN_CMD_SET_SCISSOR) sc = &e->cmds[ci];
            }
            for (uint32_t ci = sg->first_cmd; ci < sg->first_cmd + sg->ncmds; ci++) {
                if (e->cmds[ci].kind == NVN_CMD_SET_VIEWPORT) seg_vp = 1;
                else if (e->cmds[ci].kind == NVN_CMD_SET_SCISSOR) seg_sc = 1;
            }
            if (vp && !seg_vp) prefix[npre++] = *vp;
            if (sc && !seg_sc) prefix[npre++] = *sc;
        }
        uint32_t cap = (sg->ncmds ? sg->ncmds : 1) + npre;
        GfxCmd *translated = calloc(cap, sizeof *translated);
        if (translated) {
            float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
            int has_clear = 0;
            uint32_t base = 0;
            if (npre) {
                cb_t pv = view;
                nvn_cmd_t pre[2];
                memcpy(pre, prefix, npre * sizeof *pre);
                pv.cmds = pre;
                pv.ncmds = npre;
                float pclear[4] = { 0, 0, 0, 0 };
                int phas_clear = 0;
                base = nvn_translate_commands(&pv, translated, npre, rt, pclear, &phas_clear);
            }
            uint32_t count = nvn_translate_commands(&view, translated + base,
                                                    cap - base, rt, clear, &has_clear) + base;
            nvn_remap_texture_bind_cmds(translated, count, rp);
            /* The segment's recorded commands carry a private vertex snapshot
             * per draw.  Merge those windows into one stream per binding and
             * offset each draw, so a glyph/quad batch renders every draw from
             * its own record-time bytes instead of the last draw's recycled
             * range.  Falls back when any bound stream lacks a snapshot. */
            uint8_t *vb_merge[16] = { 0 };
            size_t vb_merge_size[16] = { 0 };
            unsigned nmerge = nvn_merge_draw_vertex_snapshots(
                e->cmds + sg->first_cmd, sg->ncmds, &eff, translated, count,
                lw, lh, vb_merge, vb_merge_size);
            if (nmerge) {
                static unsigned merge_seen, merge_log;
                merge_seen++;
                if ((merge_seen <= 16 || (merge_seen % 4096) == 0) && merge_log < 24) {
                    merge_log++;
                    fprintf(stderr, "[totk-nvn] vb-merge #%u seg=%u %ux%u draws=%u streams=%u",
                            merge_seen, seg_index, lw, lh, sg->draw_count, nmerge);
                    for (int i = 0; i < 16; i++)
                        if (vb_merge[i]) {
                            uint32_t mw[8] = { 0 };
                            size_t mn = vb_merge_size[i] < sizeof mw
                                      ? vb_merge_size[i] : sizeof mw;
                            if (mn) memcpy(mw, vb_merge[i], mn);
                            fprintf(stderr, " vb%d=%zu first=%08x %08x %08x %08x",
                                    i, vb_merge_size[i], mw[0], mw[1], mw[2], mw[3]);
                        }
                    fputc('\n', stderr);
                }
            }
            /* Diagnostic only (off by default): force blending off for the
             * small UI passes.  Used to tell "the shader wrote transparent
             * pixels" apart from "the blend state dropped them"; not a fix. */
            if (lw <= 512 && getenv("TOTK_ANDROID_UI_NO_BLEND")) {
                for (uint32_t ci = 0; ci < count; ci++) {
                    if (translated[ci].kind == GFX_CMD_DRAW)
                        translated[ci].u.draw.state.blend_enable = 0;
                    else if (translated[ci].kind == GFX_CMD_DRAW_INDEXED)
                        translated[ci].u.draw_indexed.state.blend_enable = 0;
                }
                static unsigned nb_logged;
                if (nb_logged++ < 4)
                    fprintf(stderr, "[totk-ui-seg] diagnostic: blend disabled for"
                                    " %ux%u seg=%u cmd=%p\n", lw, lh, seg_index, (void *)p);
            }
            if (getenv("TOTK_ANDROID_TRACE_SCENE_STEPS"))
                fprintf(stderr, "[totk-scene-step] apply bind count=%u\n", count);
            nvn_apply_bind_state(p, rp, bs, &view);
            GfxVertexStream streams[24];
            void *owned[16];
            uint32_t nowned = 0;
            uint32_t need_v = nvn_fix_index_range(translated, count, bs, "scene");
            uint32_t draw_v = need_v > sg->draw_count ? need_v : sg->draw_count;
            uint32_t nstreams = nvn_build_vertex_streams(bs, streams, 24, owned, 16,
                                                         &nowned, draw_v);
            if (nvn_ui_seg_want() && lw <= 512) {
                tl_count = count;
                tl_streams = nstreams;
                tl_stream0 = nstreams ? (uint32_t)streams[0].size : 0;
            }
            if (getenv("TOTK_ANDROID_TRACE_SCENE_STEPS"))
                fprintf(stderr, "[totk-scene-step] execute begin streams=%u\n", nstreams);
            {
                const char *ds = getenv("TOTK_ANDROID_SCENE_DELAY_US");
                if (ds && *ds) usleep((useconds_t)strtoul(ds, NULL, 0));
            }
            drew = gfx_backend_reflected_execute_ex(rp, rt, translated, count,
                                                    has_clear ? clear : NULL,
                                                    streams, nstreams);
            if (getenv("TOTK_ANDROID_TRACE_SCENE_STEPS"))
                fprintf(stderr, "[totk-scene-step] execute end drew=%d\n", drew);
            for (uint32_t oi = 0; oi < nowned; oi++) free(owned[oi]);
            for (int mi = 0; mi < 16; mi++) free(vb_merge[mi]);
            free(translated);
            if (nvn_trace_composite() && lw >= 1000) {
                size_t rn = 0;
                const uint8_t *rpix = gfx_backend_offscreen_pixels(rt, &rn);
                uint32_t rnz = nvn_first_nonzero(rpix, rn, 1u << 20);
                uint8_t cpx[4] = { 0 };
                gfx_backend_offscreen_read_pixel_index(rt, 0, lw / 2, lh / 2, cpx);
                fprintf(stderr, "[totk-frame] composite-target %ux%u drew=%d bytes=%zu"
                                " first_nz=%s%u center=%02x%02x%02x%02x\n",
                        lw, lh, drew, rn, rnz == UINT32_MAX ? "none " : "",
                        rnz == UINT32_MAX ? 0 : rnz, cpx[0], cpx[1], cpx[2], cpx[3]);
            }
            if (traced) {
                size_t rn = 0;
                const uint8_t *rpix = gfx_backend_offscreen_pixels(rt, &rn);
                uint32_t rnz = nvn_first_nonzero(rpix, rn, 1u << 20);
                fprintf(stderr, "[totk-frame] %s   target %ux%u bytes=%zu drew=%d"
                                " first_nz=%s%u\n", lw <= 512 ? "ui-inputs"
                                : "scene-inputs", lw, lh, rn, drew,
                        rnz == UINT32_MAX ? "none " : "", rnz == UINT32_MAX ? 0 : rnz);
            }
        }
    }
